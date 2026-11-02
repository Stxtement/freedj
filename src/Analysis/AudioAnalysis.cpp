#include "AudioAnalysis.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace pathboard::analysis
{

namespace
{
    constexpr double analysisSampleRate = 22050.0;
    constexpr double maxAnalysedSeconds = 12.0 * 60.0;
    constexpr int decodeChunk = 1 << 16;

    constexpr int onsetFftOrder = 10;
    constexpr int onsetFft = 1 << onsetFftOrder;
    constexpr int onsetHop = 256;

    // Time between an onset in the audio and the flux peak it produces, measured on synthetic click tracks.
    constexpr double envelopeLatencyFrames = 0.52; // about 6 ms

    constexpr double minBpm = 60.0, maxBpm = 200.0, priorBpm = 125.0, priorSigmaOctaves = 0.75;

    std::vector<float> hannWindow (int n)
    {
        std::vector<float> w ((size_t) n);
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1));
        return w;
    }

    //==========================================================================
    struct Envelopes
    {
        std::vector<float> full, low;
        std::vector<float> mix; // full + 3 x low, each normalised: kicks outweigh off-beat hi-hats
        double rate = 0.0;      // frames per second
    };

    // Subtracts a moving average and keeps the positive part, then smooths slightly.
    void conditionEnvelope (std::vector<float>& x, int halfWindow)
    {
        const int n = (int) x.size();
        std::vector<double> prefix ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i)
            prefix[(size_t) i + 1] = prefix[(size_t) i] + x[(size_t) i];

        std::vector<float> y ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const int lo = std::max (0, i - halfWindow), hi = std::min (n - 1, i + halfWindow);
            const double mean = (prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (double) (hi - lo + 1);
            y[(size_t) i] = std::max (0.0f, x[(size_t) i] - (float) mean);
        }

        for (int i = 0; i < n; ++i)
        {
            const float a = y[(size_t) std::max (0, i - 1)], b = y[(size_t) i], c = y[(size_t) std::min (n - 1, i + 1)];
            x[(size_t) i] = 0.25f * a + 0.5f * b + 0.25f * c;
        }
    }

    Envelopes computeEnvelopes (const MonoAudio& audio)
    {
        Envelopes e;
        e.rate = audio.sampleRate / onsetHop;

        const int n = (int) audio.samples.size();
        if (n < onsetFft * 8)
            return e;

        const int frames = (n - onsetFft) / onsetHop + 1;
        juce::dsp::FFT fft (onsetFftOrder);
        const auto window = hannWindow (onsetFft);
        std::vector<float> data ((size_t) onsetFft * 2);
        std::vector<float> previous ((size_t) onsetFft / 2 + 1, 0.0f);

        const double binHz = audio.sampleRate / onsetFft;
        const int fullBins = std::min (onsetFft / 2 - 1, (int) (6500.0 / binHz));
        const int lowBins = std::max (2, (int) (200.0 / binHz));

        e.full.assign ((size_t) frames, 0.0f);
        e.low.assign ((size_t) frames, 0.0f);

        for (int f = 0; f < frames; ++f)
        {
            const float* src = audio.samples.data() + (size_t) f * onsetHop;
            for (int i = 0; i < onsetFft; ++i)
                data[(size_t) i] = src[i] * window[(size_t) i];
            std::fill (data.begin() + onsetFft, data.end(), 0.0f);

            fft.performFrequencyOnlyForwardTransform (data.data(), true);

            float fluxAll = 0.0f, fluxLow = 0.0f;
            for (int k = 1; k <= fullBins; ++k)
            {
                const float v = std::log1p (data[(size_t) k] * (100.0f / (float) (onsetFft / 2)));
                const float d = v - previous[(size_t) k];
                previous[(size_t) k] = v;
                if (d > 0.0f)
                {
                    fluxAll += d;
                    if (k <= lowBins)
                        fluxLow += d;
                }
            }
            e.full[(size_t) f] = fluxAll;
            e.low[(size_t) f] = fluxLow;
        }

        const int half = std::max (4, (int) std::round (0.3 * e.rate));
        conditionEnvelope (e.full, half);
        conditionEnvelope (e.low, half);

        double meanFull = 0.0, meanLow = 0.0;
        for (int i = 0; i < frames; ++i)
        {
            meanFull += e.full[(size_t) i];
            meanLow += e.low[(size_t) i];
        }
        meanFull /= frames;
        meanLow /= frames;

        e.mix.assign ((size_t) frames, 0.0f);
        for (int i = 0; i < frames; ++i)
            e.mix[(size_t) i] = (meanFull > 1.0e-9 ? e.full[(size_t) i] / (float) meanFull : 0.0f)
                              + (meanLow > 1.0e-9 ? 3.0f * e.low[(size_t) i] / (float) meanLow : 0.0f);
        return e;
    }

    inline double sampleAt (const std::vector<float>& e, double t)
    {
        const int i = (int) t;
        if (i < 0 || i + 1 >= (int) e.size())
            return 0.0;
        const double f = t - (double) i;
        return (double) e[(size_t) i] * (1.0 - f) + (double) e[(size_t) i + 1] * f;
    }

    struct Fit { double score = 0.0, phase = 0.0; };

    // Best phase (in frames, absolute) of a beat grid with the given period inside [from, to).
    Fit bestPhase (const std::vector<float>& e, double period, double step, int from, int to)
    {
        Fit best;
        best.score = -1.0;
        for (double ph = (double) from; ph < (double) from + period; ph += step)
        {
            double sum = 0.0;
            int count = 0;
            for (double t = ph; t < (double) to - 1.0; t += period, ++count)
                sum += sampleAt (e, t);
            if (count > 0 && sum / count > best.score)
            {
                best.score = sum / count;
                best.phase = ph;
            }
        }
        return best;
    }

    double meanOf (const std::vector<float>& e, int from, int to)
    {
        double s = 0.0;
        for (int i = from; i < to; ++i)
            s += e[(size_t) i];
        return to > from ? s / (to - from) : 0.0;
    }

    struct Acf
    {
        std::vector<double> v;
        double at (double lag) const
        {
            const int i = (int) lag;
            if (i < 1 || i + 1 >= (int) v.size())
                return 0.0;
            const double f = lag - i;
            return v[(size_t) i] * (1.0 - f) + v[(size_t) i + 1] * f;
        }
    };

    Acf autocorrelate (const std::vector<float>& e, int maxLag)
    {
        Acf a;
        const int n = (int) e.size();
        maxLag = std::min (maxLag, n / 2);
        a.v.assign ((size_t) maxLag + 2, 0.0);

        double zero = 0.0;
        for (float x : e)
            zero += (double) x * x;
        if (zero <= 0.0)
            return a;

        for (int lag = 1; lag <= maxLag; ++lag)
        {
            double s = 0.0;
            for (int i = 0; i + lag < n; ++i)
                s += (double) e[(size_t) i] * e[(size_t) (i + lag)];
            a.v[(size_t) lag] = s / (double) (n - lag) / (zero / (double) n);
        }
        return a;
    }

    double harmonicScore (const Acf& acf, double rate, double bpm)
    {
        static constexpr double weights[] = { 1.0, 0.7, 0.5, 0.35 };
        const double lag = rate * 60.0 / bpm;
        double s = 0.0;
        for (int k = 1; k <= 4; ++k)
            s += weights[k - 1] * acf.at (lag * k);
        return s;
    }

    double priorWeight (double bpm)
    {
        const double octaves = std::log2 (bpm / priorBpm) / priorSigmaOctaves;
        return std::exp (-0.5 * octaves * octaves);
    }

}

//==============================================================================
juce::String toCamelot (int tonic, bool minor)
{
    if (tonic < 0)
        return {};
    const int relativeMajor = minor ? (tonic + 3) % 12 : tonic;
    const int fifths = (relativeMajor * 7) % 12;
    return juce::String ((fifths + 7) % 12 + 1) + (minor ? "A" : "B");
}

juce::String toKeyName (int tonic, bool minor)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (tonic < 0)
        return {};
    return juce::String (names[tonic % 12]) + (minor ? "m" : "");
}

//==============================================================================
bool decodeToMono (const juce::File& file, juce::AudioFormatManager& formats, MonoAudio& out,
                   juce::String& error, const std::atomic<bool>* cancel)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        error = "Cannot read audio: " + file.getFileName();
        return false;
    }

    const int factor = std::max (1, (int) std::lround (reader->sampleRate / analysisSampleRate));
    out.sampleRate = reader->sampleRate / factor;
    out.durationSeconds = (double) reader->lengthInSamples / reader->sampleRate;

    const juce::int64 total = std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (maxAnalysedSeconds * reader->sampleRate));
    out.samples.clear();
    out.samples.reserve ((size_t) (total / factor) + 1);

    juce::AudioBuffer<float> buffer (2, decodeChunk);
    double accumulator = 0.0;
    int accumulated = 0;

    for (juce::int64 pos = 0; pos < total; pos += decodeChunk)
    {
        if (cancel != nullptr && cancel->load())
        {
            error = "cancelled";
            return false;
        }

        const int num = (int) std::min<juce::int64> (decodeChunk, total - pos);
        if (! reader->read (&buffer, 0, num, pos, true, true))
        {
            if (pos == 0)
            {
                error = "Decoding error in: " + file.getFileName();
                return false;
            }
            break; // compressed formats may end earlier than announced
        }

        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (1);
        for (int i = 0; i < num; ++i)
        {
            accumulator += 0.5 * ((double) l[i] + (double) r[i]);
            if (++accumulated == factor)
            {
                out.samples.push_back ((float) (accumulator / factor));
                accumulator = 0.0;
                accumulated = 0;
            }
        }
    }

    if (out.samples.size() < (size_t) out.sampleRate)
    {
        error = "Track is shorter than one second";
        return false;
    }
    return true;
}

//==============================================================================
TempoResult analyseTempo (const MonoAudio& audio)
{
    TempoResult result;
    const auto env = computeEnvelopes (audio);
    const int n = (int) env.full.size();
    if (n < (int) (env.rate * 8.0))
        return result;

    const double rate = env.rate;
    const double meanEnv = meanOf (env.full, 0, n);
    if (meanEnv <= 1.0e-6)
        return result; // silence

    // 1. Coarse tempo from the autocorrelation of the onset envelope.
    const int maxLag = (int) std::ceil (rate * 60.0 / (minBpm * 0.9) * 4.0) + 2;
    const auto acf = autocorrelate (env.mix, maxLag);

    double coarseBpm = 0.0, coarseBest = -1.0;
    for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.25)
    {
        const double s = harmonicScore (acf, rate, bpm) * priorWeight (bpm);
        if (s > coarseBest)
        {
            coarseBest = s;
            coarseBpm = bpm;
        }
    }
    if (coarseBest <= 0.0)
        return result;

    // 2. Fine tempo and phase: maximise the onset energy sampled on a regular grid over the whole track.
    Fit best;
    best.score = -1.0;
    double bestBpm = coarseBpm;
    std::vector<double> fineScores;
    const double fineStart = std::max (minBpm, coarseBpm - 1.5);
    const double fineStep = 0.01;
    const int fineCount = (int) std::round (3.0 / fineStep) + 1;
    int bestIndex = 0;

    for (int i = 0; i < fineCount; ++i)
    {
        const double bpm = fineStart + i * fineStep;
        const double period = rate * 60.0 / bpm;
        const auto fit = bestPhase (env.mix, period, 0.25, 0, n);
        fineScores.push_back (fit.score);
        if (fit.score > best.score)
        {
            best = fit;
            bestBpm = bpm;
            bestIndex = i;
        }
    }

    // Parabolic interpolation between the three best points gives sub-step precision.
    if (bestIndex > 0 && bestIndex + 1 < (int) fineScores.size())
    {
        const double a = fineScores[(size_t) bestIndex - 1], b = fineScores[(size_t) bestIndex], c = fineScores[(size_t) bestIndex + 1];
        const double denom = a - 2.0 * b + c;
        if (denom < 0.0)
            bestBpm += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) * fineStep;
    }

    const double period = rate * 60.0 / bestBpm;
    best = bestPhase (env.mix, period, 0.1, 0, n);

    result.bpm = std::round (bestBpm * 100.0) / 100.0;

    // Confidence: how much stronger beat-aligned frames are than the average frame.
    const double strength = best.score / meanOf (env.mix, 0, n);
    result.confidence = juce::jlimit (0.0, 1.0, (strength - 1.5) / 3.0);

    // Half/double time: report the other plausible reading.
    {
        const double own = harmonicScore (acf, rate, result.bpm);
        double altBpm = 0.0, altScore = 0.0;
        for (double candidate : { result.bpm * 2.0, result.bpm * 0.5 })
            if (candidate >= 55.0 && candidate <= 210.0)
            {
                const double s = harmonicScore (acf, rate, candidate);
                if (s > altScore) { altScore = s; altBpm = candidate; }
            }
        if (own > 0.0 && altScore > 0.6 * own)
            result.alternativeBpm = std::round (altBpm * 100.0) / 100.0;
    }

    // 3. First beat (seconds), reduced to one beat period.
    const double periodSeconds = 60.0 / result.bpm;
    const double firstBeat = ((best.phase + envelopeLatencyFrames) * onsetHop + onsetFft * 0.5) / audio.sampleRate;
    // Number of whole beat periods removed by the reduction; the downbeat index below is shifted by it so
    // that it counts from the reported first beat.
    const int removedBeats = (int) std::floor (firstBeat / periodSeconds);
    result.firstBeatSeconds = firstBeat - removedBeats * periodSeconds;

    // 4. Downbeat: which of four consecutive beats carries the strongest low-frequency attack.
    {
        double sums[4] = {}, counts[4] = {};
        int k = 0;
        for (double t = best.phase; t < n - 2; t += period, ++k)
        {
            const double v = sampleAt (env.low, t) + 0.5 * sampleAt (env.full, t);
            sums[k % 4] += v;
            counts[k % 4] += 1.0;
        }

        double means[4] = {};
        int top = 0;
        for (int i = 0; i < 4; ++i)
        {
            means[i] = counts[i] > 0.0 ? sums[i] / counts[i] : 0.0;
            if (means[i] > means[top])
                top = i;
        }
        double second = 0.0;
        for (int i = 0; i < 4; ++i)
            if (i != top)
                second = std::max (second, means[i]);

        result.downbeatIndex = ((top + removedBeats) % 4 + 4) % 4;
        result.downbeatConfidence = means[top] > 0.0 ? juce::jlimit (0.0, 1.0, (means[top] - second) / means[top] * 2.0) : 0.0;
    }

    // 5. Tempo changes: fit the tempo separately in four segments. Segments without a clear rhythm
    //    (intro, breakdown) are ignored; at least three must be usable to make a statement.
    {
        double minSeg = 1.0e9, maxSeg = -1.0e9;
        int valid = 0;
        for (int s = 0; s < 4; ++s)
        {
            const int from = s * n / 4, to = (s + 1) * n / 4;
            const double segMean = meanOf (env.mix, from, to);
            if (segMean <= 0.0)
                continue;

            Fit segBest;
            segBest.score = -1.0;
            double segBpm = result.bpm;
            for (double bpm = std::max (minBpm, result.bpm - 4.0); bpm <= result.bpm + 4.0; bpm += 0.05)
            {
                const auto fit = bestPhase (env.mix, rate * 60.0 / bpm, 0.5, from, to);
                if (fit.score > segBest.score)
                {
                    segBest = fit;
                    segBpm = bpm;
                }
            }
            if (segBest.score / segMean < 2.0)
                continue;
            minSeg = std::min (minSeg, segBpm);
            maxSeg = std::max (maxSeg, segBpm);
            ++valid;
        }
        result.variableTempo = valid >= 3 && maxSeg - minSeg > 0.6;
    }

    return result;
}

//==============================================================================
KeyResult analyseKey (const MonoAudio& audio)
{
    KeyResult result;
    constexpr int order = 12, size = 1 << order, hop = 2048;
    const int n = (int) audio.samples.size();
    if (n < size * 8)
        return result;

    juce::dsp::FFT fft (order);
    const auto window = hannWindow (size);
    std::vector<float> data ((size_t) size * 2);
    const double binHz = audio.sampleRate / size;
    const int lo = (int) std::ceil (110.0 / binHz), hi = std::min (size / 2 - 2, (int) (3500.0 / binHz));

    struct Peak { float semitones; float weight; };
    std::vector<Peak> peaks;

    const int frames = (n - size) / hop + 1;
    for (int f = 0; f < frames; ++f)
    {
        const float* src = audio.samples.data() + (size_t) f * hop;
        for (int i = 0; i < size; ++i)
            data[(size_t) i] = src[i] * window[(size_t) i];
        std::fill (data.begin() + size, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        float frameMax = 0.0f;
        for (int k = lo; k <= hi; ++k)
            frameMax = std::max (frameMax, data[(size_t) k]);
        if (frameMax < 1.0e-3f)
            continue;

        const float threshold = 0.1f * frameMax;
        for (int k = lo + 1; k < hi; ++k)
        {
            const float a = data[(size_t) k - 1], b = data[(size_t) k], c = data[(size_t) k + 1];
            if (b <= threshold || b <= a || b < c)
                continue;
            const float denom = a - 2.0f * b + c;
            const float delta = denom != 0.0f ? 0.5f * (a - c) / denom : 0.0f;
            const double freq = ((double) k + (double) juce::jlimit (-0.5f, 0.5f, delta)) * binHz;
            peaks.push_back ({ (float) (12.0 * std::log2 (freq / 440.0)), std::sqrt (b) });
        }
    }

    if (peaks.empty())
        return result;

    // Tuning offset: weighted circular mean of the distance of each peak to the nearest semitone.
    double sx = 0.0, sy = 0.0;
    for (const auto& p : peaks)
    {
        const double angle = juce::MathConstants<double>::twoPi * (p.semitones - std::round (p.semitones));
        sx += p.weight * std::cos (angle);
        sy += p.weight * std::sin (angle);
    }
    const double tuning = std::atan2 (sy, sx) / juce::MathConstants<double>::twoPi;

    double chroma[12] = {};
    for (const auto& p : peaks)
    {
        const int pitchClass = (((int) std::lround (p.semitones - tuning) + 9) % 12 + 12) % 12;
        chroma[pitchClass] += p.weight;
    }

    // Krumhansl-Kessler key profiles.
    static constexpr double major[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    static constexpr double minorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    auto correlation = [&] (const double* profile, int tonic)
    {
        double meanC = 0.0, meanP = 0.0;
        for (int i = 0; i < 12; ++i) { meanC += chroma[i]; meanP += profile[i]; }
        meanC /= 12.0; meanP /= 12.0;
        double num = 0.0, dc = 0.0, dp = 0.0;
        for (int pc = 0; pc < 12; ++pc)
        {
            const double c = chroma[pc] - meanC, p = profile[(pc - tonic + 12) % 12] - meanP;
            num += c * p; dc += c * c; dp += p * p;
        }
        return dc > 0.0 && dp > 0.0 ? num / std::sqrt (dc * dp) : 0.0;
    };

    double best = -2.0, second = -2.0;
    int bestTonic = -1;
    bool bestMinor = false;
    for (int minor = 0; minor < 2; ++minor)
        for (int tonic = 0; tonic < 12; ++tonic)
        {
            const double r = correlation (minor != 0 ? minorProfile : major, tonic);
            if (r > best)
            {
                second = best;
                best = r;
                bestTonic = tonic;
                bestMinor = minor != 0;
            }
            else if (r > second)
            {
                second = r;
            }
        }

    result.tonic = bestTonic;
    result.minor = bestMinor;
    result.camelot = toCamelot (bestTonic, bestMinor);
    result.name = toKeyName (bestTonic, bestMinor);
    // Confidence combines how clearly one key wins with how "pitched" the material is: noise and
    // drum-only passages spread their energy evenly over all 12 pitch classes.
    double chromaSum = 0.0, chromaMax = 0.0;
    for (double c : chroma)
    {
        chromaSum += c;
        chromaMax = std::max (chromaMax, c);
    }
    const double peakiness = chromaSum > 0.0 ? chromaMax / (chromaSum / 12.0) : 0.0;
    const double clarity = 0.6 * (best - second) / 0.12 + 0.4 * (best - 0.4) / 0.4;
    result.confidence = juce::jlimit (0.0, 1.0, clarity * juce::jlimit (0.0, 1.0, (peakiness - 1.3) / 0.7));
    return result;
}

//==============================================================================
double estimateEnergy (const MonoAudio& audio, double& loudnessDbOut)
{
    constexpr int frame = 2048;
    const int n = (int) audio.samples.size();
    loudnessDbOut = -100.0;
    if (n < frame)
        return 1.0;

    std::vector<double> power;
    double total = 0.0;
    for (int pos = 0; pos + frame <= n; pos += frame)
    {
        double s = 0.0;
        for (int i = 0; i < frame; ++i)
            s += (double) audio.samples[(size_t) (pos + i)] * audio.samples[(size_t) (pos + i)];
        power.push_back (s / frame);
        total += s / frame;
    }

    loudnessDbOut = 10.0 * std::log10 (std::max (1.0e-10, total / (double) power.size()));

    std::sort (power.begin(), power.end(), std::greater<double>());
    double top = 0.0;
    const size_t half = std::max<size_t> (1, power.size() / 2);
    for (size_t i = 0; i < half; ++i)
        top += power[i];
    const double topDb = 10.0 * std::log10 (std::max (1.0e-10, top / (double) half));

    return 1.0 + 9.0 * juce::jlimit (0.0, 1.0, (topDb + 26.0) / 17.0);
}

TrackAnalysis analyseAudio (const MonoAudio& audio)
{
    TrackAnalysis a;
    a.durationSeconds = audio.durationSeconds;
    a.tempo = analyseTempo (audio);
    a.key = analyseKey (audio);
    a.energy = estimateEnergy (audio, a.loudnessDb);
    return a;
}

//==============================================================================
juce::String computeFingerprint (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (in.failedToOpen())
        return {};

    constexpr juce::int64 chunk = 64 * 1024;
    const auto size = in.getTotalLength();

    juce::MemoryBlock block;
    block.append (&size, sizeof (size));

    auto readAt = [&] (juce::int64 position)
    {
        in.setPosition (juce::jmax<juce::int64> (0, position));
        juce::MemoryBlock part;
        in.readIntoMemoryBlock (part, (juce::ssize_t) chunk);
        block.append (part.getData(), part.getSize());
    };

    readAt (0);
    if (size > chunk * 2)
        readAt (size / 2 - chunk / 2);
    if (size > chunk)
        readAt (size - chunk);

    // Two independent 64-bit FNV-1a runs -> 128 bits. Not cryptographic; only for recognising the same file.
    auto fnv = [&block] (juce::uint64 hash)
    {
        const auto* bytes = static_cast<const juce::uint8*> (block.getData());
        for (size_t i = 0; i < block.getSize(); ++i)
        {
            hash ^= bytes[i];
            hash *= 0x100000001b3ULL;
        }
        return hash;
    };
    return juce::String::toHexString (fnv (0xcbf29ce484222325ULL)).paddedLeft ('0', 16)
         + juce::String::toHexString (fnv (0x84222325cbf29ce4ULL)).paddedLeft ('0', 16);
}

} // namespace pathboard::analysis
