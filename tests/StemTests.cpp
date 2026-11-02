// Stem separation tests.
//  - Always run: cache, deck playback from stems (digital silence, no original leaking in, no clicks, alignment).
//  - Run if the 370 MB model is installed: real separation (reconstruction, vocal removal, quality modes,
//    manager + cache end to end).

#include "TestSupport.h"
#include "Analysis/AudioAnalysis.h"
#include "Audio/DeckEngine.h"
#include "Stems/StemManager.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

using namespace pathboard;
using namespace pathboard::stems;
using testsupport::check;

namespace
{
constexpr double twoPi = juce::MathConstants<double>::twoPi;
constexpr int blockSize = 512;

double now() { return juce::Time::getMillisecondCounterHiRes(); }
void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

bool waitFor (std::function<bool()> done, int timeoutMs)
{
    const double start = now();
    while (! done() && now() - start < timeoutMs)
        pump (20);
    pump (50);
    return done();
}

double energy (const juce::AudioBuffer<float>& b, int from, int count)
{
    double s = 0.0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = from; i < from + count; ++i)
            s += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return s;
}

// Power of one frequency in channel 0 (Goertzel), normalised to the amplitude of a sine.
double tonePower (const juce::AudioBuffer<float>& b, int from, int count, double freq, double rate)
{
    const double w = twoPi * freq / rate, coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const double s0 = b.getSample (0, from + i) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 4.0 * power / ((double) count * count);
}

double toDb (double ratio) { return 10.0 * std::log10 (std::max (1.0e-30, ratio)); }

void fillSine (juce::AudioBuffer<float>& b, double freq, double amp, double rate)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (ch, i, (float) (amp * std::sin (twoPi * freq * i / rate)));
}

void writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
    writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

juce::AudioBuffer<float> readStereo44k (const juce::File& file, double startSeconds, double seconds)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    juce::AudioBuffer<float> out;
    if (reader == nullptr || std::abs (reader->sampleRate - 44100.0) > 1.0)
        return out;
    const int n = (int) std::min ((juce::int64) (seconds * 44100.0), reader->lengthInSamples - (juce::int64) (startSeconds * 44100.0));
    if (n <= 0)
        return out;
    out.setSize (2, n);
    reader->read (&out, 0, n, (juce::int64) (startSeconds * 44100.0), true, true);
    return out;
}

// Renders `seconds` from the deck in blocks; returns the output.
juce::AudioBuffer<float> render (DeckEngine& deck, double seconds, double rate)
{
    const int total = (int) (seconds * rate);
    juce::AudioBuffer<float> out (2, total), block (2, blockSize);
    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = std::min (blockSize, total - pos);
        deck.getNextAudioBlock (juce::AudioSourceChannelInfo (&block, 0, n));
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, block, ch, 0, n);
    }
    return out;
}

bool loadAndWait (DeckEngine& deck, const juce::File& file)
{
    deck.loadFile (file);
    const double start = now();
    while (deck.isLoading() && now() - start < 10000)
        juce::Thread::sleep (10);
    return deck.hasTrackLoaded();
}

std::shared_ptr<StemSet> makeSineStems (int length, const std::array<double, numStems>& freqs, double rate = 44100.0)
{
    auto set = std::make_shared<StemSet>();
    set->sampleRate = rate;
    for (int s = 0; s < numStems; ++s)
    {
        set->audio[(size_t) s].setSize (2, length);
        fillSine (set->audio[(size_t) s], freqs[(size_t) s], 0.2, rate);
    }
    return set;
}
}

juce::File findStemModel()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("PATHBOARD_MODEL_DIR", {});
    if (env.isNotEmpty())
        return juce::File (env).getChildFile (ModelInfo::fileName);
    return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String ("models/") + ModelInfo::fileName);
}

//==============================================================================
// `FreeDJTests --stem-probe <44.1kHz audio file> <startSeconds> <seconds>`
int runStemProbe (const juce::File& audioFile, double start, double seconds)
{
    const auto modelFile = findStemModel();
    auto mix = readStereo44k (audioFile, start, seconds);
    if (mix.getNumSamples() == 0)
    {
        std::cout << "cannot read a 44.1 kHz stereo excerpt from " << audioFile.getFullPathName() << std::endl;
        return 1;
    }

    for (auto quality : { Quality::fast, Quality::balanced, Quality::high })
    {
        StemSeparator separator;
        if (separator.load (modelFile, true).failed())
            return 1;
        std::array<juce::AudioBuffer<float>, numStems> out;
        const double t = now();
        separator.separate (mix, quality, out);
        std::cout << qualityName (quality) << " on " << separator.getDeviceName() << ": " << (int) (now() - t) << " ms for " << seconds << " s" << std::endl;
    }
    return 0;
}

//==============================================================================
static void testCache (const juce::File& root)
{
    std::cout << "Stem cache" << std::endl;
    const auto folder = root.getChildFile ("cache");

    auto stems = makeSineStems (44100 * 3, { 110.0, 220.0, 440.0, 880.0 });
    stems->audio[3].applyGain (4.0f); // 0.8 peak, and with a second channel offset below
    for (int i = 0; i < stems->audio[2].getNumSamples(); ++i)
        stems->audio[2].setSample (1, i, stems->audio[2].getSample (1, i) * 0.5f + 1.2f * (i % 100 == 0 ? 1.0f : 0.0f)); // peak above 1.0

    StemCache cache (folder, 0, "model-A");
    check (! cache.contains ("abc", Quality::fast), "empty cache has no entries");
    check (cache.store ("abc", Quality::balanced, stems->audio, "song.mp3"), "stems are stored");
    check (cache.contains ("abc", Quality::balanced) && cache.contains ("abc", Quality::fast), "a BALANCED entry also serves a FAST request");
    check (! cache.contains ("abc", Quality::high), "but not a HIGH request");
    check (! cache.contains ("other-file", Quality::fast), "another file has no entry");

    auto loaded = cache.load ("abc", Quality::fast);
    check (loaded != nullptr && loaded->quality == Quality::balanced && loaded->getNumSamples() == stems->getNumSamples(), "stems load back with the stored quality");
    double worst = 0.0;
    if (loaded != nullptr)
        for (int s = 0; s < numStems; ++s)
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < loaded->getNumSamples(); ++i)
                    worst = std::max (worst, (double) std::abs (loaded->audio[(size_t) s].getSample (ch, i) - stems->audio[(size_t) s].getSample (ch, i)));
    check (worst < 2.0e-6, "24-bit FLAC round trip is transparent, even for peaks above full scale", "max error " + juce::String (worst, 8));

    StemCache otherModel (folder, 0, "model-B");
    check (! otherModel.contains ("abc", Quality::fast), "entries of a different model are not used");

    // Truncated stem file -> invalid and recomputed, not a crash.
    const auto entry = folder.getChildFile ("abc-1");
    entry.getChildFile ("vocals.flac").replaceWithText ("broken");
    check (! cache.contains ("abc", Quality::fast), "a damaged entry is detected");
    juce::String error;
    check (cache.load ("abc", Quality::fast, &error) == nullptr, "a damaged entry is not returned");

    // Limit: oldest entries go first.
    StemCache limited (root.getChildFile ("limited"), 0, "m");
    auto small = makeSineStems (44100, { 100.0, 200.0, 300.0, 400.0 });
    for (const char* name : { "first", "second", "third" })
    {
        limited.store (name, Quality::fast, small->audio, name);
        juce::Thread::sleep (30);
    }
    limited.load ("first", Quality::fast); // refreshes "first": now "second" is the least recently used
    const auto bytes = limited.getUsedBytes();
    StemCache capped (root.getChildFile ("limited"), bytes * 2 / 3, "m");
    const int removed = capped.enforceLimit();
    check (removed >= 1 && capped.contains ("first", Quality::fast) && ! capped.contains ("second", Quality::fast),
           "least recently used entries are removed first", juce::String (removed) + " removed, " + juce::String (capped.getNumEntries()) + " left");

    // Leftovers of an interrupted write.
    root.getChildFile ("limited/zzz-1.tmp").createDirectory();
    capped.enforceLimit();
    check (! root.getChildFile ("limited/zzz-1.tmp").exists(), "leftovers of interrupted writes are cleaned up");
    capped.clear();
    check (capped.getNumEntries() == 0, "cache can be cleared");
}

//==============================================================================
static void testDeckStems (const juce::File& root)
{
    std::cout << "Deck playback from stems" << std::endl;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    constexpr double rate = 44100.0;
    const int length = (int) (6 * rate);
    // drums, bass, other, vocals at distinct frequencies; the original is their sum plus a 5 kHz marker
    // that exists in no stem, so any original signal leaking into the stem path is visible.
    const std::array<double, numStems> freqs { 3000.0, 100.0, 2000.0, 1000.0 };
    auto stems = makeSineStems (length, freqs);
    juce::AudioBuffer<float> original (2, length);
    original.clear();
    for (int s = 0; s < numStems; ++s)
        for (int ch = 0; ch < 2; ++ch)
            original.addFrom (ch, 0, stems->audio[(size_t) s], ch, 0, length);
    juce::AudioBuffer<float> marker (2, length);
    fillSine (marker, 5000.0, 0.1, rate);
    for (int ch = 0; ch < 2; ++ch)
        original.addFrom (ch, 0, marker, ch, 0, length);

    const auto wav = root.getChildFile ("stem-original.wav");
    writeWav (wav, original, rate);

    DeckEngine deck (formats);
    deck.prepareToPlay (blockSize, rate);
    check (loadAndWait (deck, wav), "test track loads");
    deck.setStems (stems);
    check (deck.hasStems(), "stems are attached to the deck");
    deck.setPositionSeconds (1.0);
    deck.play();
    render (deck, 0.2, rate); // let the rate smoothing settle

    auto measure = [&] (double seconds = 0.5)
    {
        const auto out = render (deck, seconds, rate);
        const int n = out.getNumSamples();
        struct R { double vocal, drums, bass, other, marker; double total; };
        return R { tonePower (out, 0, n, freqs[3], rate), tonePower (out, 0, n, freqs[0], rate), tonePower (out, 0, n, freqs[1], rate),
                   tonePower (out, 0, n, freqs[2], rate), tonePower (out, 0, n, 5000.0, rate), energy (out, 0, n) };
    };

    const auto full = measure();
    check (full.marker > 0.005 && full.vocal > 0.03, "with all stems on the original plays (5 kHz marker is present)",
           "marker power " + juce::String (full.marker, 4));

    // Vocals off.
    deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 0.0f });
    render (deck, 0.05, rate); // 5 ms ramp plus margin
    const auto noVocal = measure();
    check (toDb (noVocal.vocal / full.vocal) < -120.0, "vocals off: the vocal tone is gone (> 120 dB down)", juce::String (toDb (noVocal.vocal / full.vocal), 1) + " dB");
    check (noVocal.marker < 1.0e-12, "vocals off: the original track is not playing in the background (marker tone absent)",
           "marker power " + juce::String (noVocal.marker, 14));
    check (std::abs (toDb (noVocal.drums / full.drums)) < 0.1 && std::abs (toDb (noVocal.bass / full.bass)) < 0.1
               && std::abs (toDb (noVocal.other / full.other)) < 0.1,
           "vocals off: instrumental stems play unchanged");

    // Everything off: digital zero.
    deck.setStemGains ({ 0.0f, 0.0f, 0.0f, 0.0f });
    render (deck, 0.05, rate);
    const auto silence = render (deck, 0.3, rate);
    float peak = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        peak = std::max (peak, silence.getMagnitude (ch, 0, silence.getNumSamples()));
    check (peak == 0.0f, "all stems off: output is exactly digital silence", "peak " + juce::String (peak));

    // Isolate vocals.
    deck.setStemGains ({ 0.0f, 0.0f, 0.0f, 1.0f });
    render (deck, 0.05, rate);
    const auto solo = measure();
    check (solo.vocal / full.vocal > 0.99 && toDb (solo.drums / full.drums) < -120.0 && toDb (solo.bass / full.bass) < -120.0
               && toDb (solo.other / full.other) < -120.0 && solo.marker < 1.0e-12,
           "vocal solo: only the vocal stem is heard");

    // Back to the original.
    deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 1.0f });
    render (deck, 0.05, rate);
    const auto back = measure();
    check (back.marker > 0.005, "all stems on again: the original plays again");

    // No click when muting: compare the largest sample step with and without the switch.
    {
        deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 1.0f });
        render (deck, 0.1, rate);
        auto steady = render (deck, 0.2, rate);
        float steadyStep = 0.0f;
        for (int i = 1; i < steady.getNumSamples(); ++i)
            steadyStep = std::max (steadyStep, std::abs (steady.getSample (0, i) - steady.getSample (0, i - 1)));

        deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 0.0f });
        auto switching = render (deck, 0.2, rate);
        float switchStep = 0.0f;
        for (int i = 1; i < switching.getNumSamples(); ++i)
            switchStep = std::max (switchStep, std::abs (switching.getSample (0, i) - switching.getSample (0, i - 1)));
        check (switchStep <= steadyStep * 1.05f, "switching a stem off causes no click (no larger sample step than normal playback)",
               juce::String (switchStep, 4) + " vs " + juce::String (steadyStep, 4));
    }

    // Mute reaction time: the 5 ms ramp must be over 6 ms after the command, independent of the buffer size.
    {
        deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 1.0f });
        render (deck, 0.1, rate);
        deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 0.0f });
        const auto after = render (deck, 0.04, rate);
        const int from = (int) (0.006 * rate);
        const double remaining = tonePower (after, from, 441, freqs[3], rate);
        check (toDb (remaining / full.vocal) < -100.0, "mute is complete 6 ms after the command", juce::String (toDb (remaining / full.vocal), 1) + " dB");
    }
    // A new track starts on its original audio, without the previous track's stems.
    const auto second = root.getChildFile ("second.wav");
    writeWav (second, original, rate);
    deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 0.0f });
    loadAndWait (deck, second);
    check (! deck.hasStems(), "loading another track removes the old stems");
    deck.setPositionSeconds (1.0);
    deck.play();
    render (deck, 0.1, rate);
    const auto fresh = measure();
    check (fresh.vocal > 0.03 && fresh.marker > 0.005, "and the new track plays unchanged, gains are reset");

    //==========================================================================
    std::cout << "Stem alignment (no phase shift between stems and original)" << std::endl;
    for (double sourceRate : { 44100.0, 48000.0 })
    {
        const int len = (int) (4 * sourceRate);
        juce::AudioBuffer<float> sine (2, len);
        fillSine (sine, 200.0, 0.5, sourceRate);
        const auto file = root.getChildFile ("align" + juce::String ((int) sourceRate) + ".wav");
        writeWav (file, sine, sourceRate);

        // Stems analytically generated at 44.1 kHz: three carry 1/4 of the sine each, "other" is silent.
        auto set = std::make_shared<StemSet>();
        set->sampleRate = 44100.0;
        const int stemLen = (int) (4 * 44100.0);
        for (int s = 0; s < numStems; ++s)
        {
            set->audio[(size_t) s].setSize (2, stemLen);
            if (s != 2)
                fillSine (set->audio[(size_t) s], 200.0, 0.5 / 3.0, 44100.0);
            else
                set->audio[(size_t) s].clear();
        }

        DeckEngine d (formats);
        d.prepareToPlay (blockSize, sourceRate);
        loadAndWait (d, file);
        d.setStems (set);
        d.setStemGains ({ 1.0f, 1.0f, 0.0f, 1.0f }); // engage stems by muting the silent one
        d.setPositionSeconds (1.0);
        d.play();
        render (d, 0.2, sourceRate);
        const double startPos = d.getCurrentPositionSeconds();
        const auto out = render (d, 1.0, sourceRate);

        double worstError = 0.0;
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            const double t = startPos + (double) i / sourceRate;
            worstError = std::max (worstError, std::abs ((double) out.getSample (0, i) - 0.5 * std::sin (twoPi * 200.0 * t)));
        }
        check (worstError < 2.0e-3, "stem playback is time-aligned with the original at " + juce::String ((int) sourceRate) + " Hz source",
               "max error " + juce::String (worstError, 5));
    }

    //==========================================================================
    std::cout << "Resampling to the model rate" << std::endl;
    {
        const int len = (int) (3 * 48000.0);
        juce::AudioBuffer<float> sine (2, len);
        fillSine (sine, 300.0, 0.5, 48000.0);
        const auto file = root.getChildFile ("decode48.wav");
        writeWav (file, sine, 48000.0);
        juce::AudioBuffer<float> decoded;
        juce::String error;
        check (decodeStereo44k (file, formats, decoded, error), "48 kHz file decodes to 44.1 kHz", error);
        check (std::abs (decoded.getNumSamples() - (int) (3 * 44100.0)) <= 1, "length matches the duration", juce::String (decoded.getNumSamples()));

        // Best lag between the decoded signal and the ideal 44.1 kHz sine (sub-sample, by phase estimate).
        double bestError = 1.0;
        int bestShiftTenths = 0;
        for (int shiftTenths = -30; shiftTenths <= 30; ++shiftTenths)
        {
            double e = 0.0;
            for (int i = 2000; i < 40000; ++i)
                e = std::max (e, std::abs ((double) decoded.getSample (0, i) - 0.5 * std::sin (twoPi * 300.0 * (i + shiftTenths / 10.0) / 44100.0)));
            if (e < bestError) { bestError = e; bestShiftTenths = shiftTenths; }
        }
        check (std::abs (bestShiftTenths) <= 3 && bestError < 5.0e-3, "the resampler adds no audible delay or distortion",
               "best shift " + juce::String (bestShiftTenths / 10.0, 1) + " samples, error " + juce::String (bestError, 5));
    }

    //==========================================================================
    std::cout << "Real-time cost of stem playback" << std::endl;
    {
        DeckEngine d (formats);
        d.prepareToPlay (blockSize, rate);
        loadAndWait (d, wav);
        d.setStems (stems);
        d.setStemGains ({ 1.0f, 1.0f, 1.0f, 0.0f });
        d.play();
        render (d, 0.2, rate);
        const double t0 = now();
        render (d, 2.0, rate);
        const double elapsed = now() - t0;
        check (elapsed < 2000.0 * 0.05, "rendering 2 s from stems uses under 5% of one core", juce::String (elapsed / 20.0, 2) + " % of real time");
    }
}

//==============================================================================
struct SeparationStats
{
    double reconstructionDb = 0.0;
    double vocalShare = 0.0;          // vocal stem energy / mix energy
    double instrumentalLeakDb = 0.0;  // vocal energy found by re-separating the instrumental, relative to it
    double acapellaBleedDb = 0.0;     // non-vocal energy found by re-separating the vocal stem, relative to it
};

static SeparationStats evaluate (StemSeparator& separator, const juce::AudioBuffer<float>& mix,
                                 std::array<juce::AudioBuffer<float>, numStems>& stems, Quality quality)
{
    SeparationStats r;
    const int n = mix.getNumSamples();
    separator.separate (mix, quality, stems);

    juce::AudioBuffer<float> sum (2, n), instrumental (2, n);
    sum.clear();
    instrumental.clear();
    for (int s = 0; s < numStems; ++s)
        for (int ch = 0; ch < 2; ++ch)
        {
            sum.addFrom (ch, 0, stems[(size_t) s], ch, 0, n);
            if (s != (int) Stem::vocals)
                instrumental.addFrom (ch, 0, stems[(size_t) s], ch, 0, n);
        }

    double error = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const double d = (double) mix.getSample (ch, i) - sum.getSample (ch, i);
            error += d * d;
        }
    const double mixEnergy = energy (mix, 0, n);
    r.reconstructionDb = toDb (mixEnergy / std::max (1.0e-20, error));
    r.vocalShare = energy (stems[(size_t) Stem::vocals], 0, n) / mixEnergy;

    // Leakage: separate the instrumental again. A clean instrumental contains no vocals, so the vocal stem
    // of that second run should be almost silent.
    std::array<juce::AudioBuffer<float>, numStems> second;
    separator.separate (instrumental, quality, second);
    r.instrumentalLeakDb = toDb (energy (second[(size_t) Stem::vocals], 0, n) / std::max (1.0e-20, energy (instrumental, 0, n)));

    // Bleed: separate the vocal stem again; instruments left inside it show up in the non-vocal stems.
    std::array<juce::AudioBuffer<float>, numStems> third;
    separator.separate (stems[(size_t) Stem::vocals], quality, third);
    double nonVocal = 0.0;
    for (int s = 0; s < numStems; ++s)
        if (s != (int) Stem::vocals)
            nonVocal += energy (third[(size_t) s], 0, n);
    // Meaningless when the track has no vocals at all (the vocal stem is then near silence).
    r.acapellaBleedDb = r.vocalShare < 0.001 ? -100.0 : toDb (nonVocal / std::max (1.0e-20, energy (stems[(size_t) Stem::vocals], 0, n)));
    return r;
}

static void testWithModel (const juce::File& modelFile, const juce::File& root)
{
    std::cout << "Stem separation with the AI model" << std::endl;

    StemSeparator separator;
    const double loadStart = now();
    const auto loaded = separator.load (modelFile, true);
    check (loaded.wasOk(), "model loads", loaded.getErrorMessage());
    if (loaded.failed())
        return;
    check (separator.getDeviceName() == "GPU (DirectML)" || separator.getDeviceName() == "CPU", "inference device: " + separator.getDeviceName(),
           juce::String ((int) (now() - loadStart)) + " ms to load");

    // Real music: different genres from the user's own library (skipped when a file is not available).
    struct Song { const char* genre; const char* file; };
    const juce::File music = juce::File ("C:\\Users\\deaga\\OneDrive\\Music");
    const Song songs[] = {
        { "pop / dance",   "DJ Musik\\01 I Gotta Feeling.wav" },
        { "rock",          "DJ Musik\\02 Mr. Brightside.wav" },
        { "pop / electro", "DJ Musik\\02 Sexyback.wav" },
        { "hip-hop",       "DJ Musik\\14 In Da Club.wav" },
        { "dance",         "DJ Musik\\03 Heavy Cross.wav" },
        { "EDM / house (own production)", "untitled.wav" },
    };

    std::cout << "  genre                          recon.   vocal    inst. vocal-leak   vocal-stem bleed   time" << std::endl;
    int measured = 0;
    SeparationStats worst { 100.0, 0.0, -100.0, -100.0 };
    for (const auto& song : songs)
    {
        const auto file = music.getChildFile (song.file);
        if (! file.existsAsFile() || analysis::isOnlineOnly (file))
            continue;
        const auto mix = readStereo44k (file, 40.0, 20.0);
        if (mix.getNumSamples() < 44100 * 10)
            continue;

        std::array<juce::AudioBuffer<float>, numStems> stems;
        const double t = now();
        const auto r = evaluate (separator, mix, stems, Quality::balanced);
        const double ms = (now() - t) / 3.0;
        ++measured;
        worst.reconstructionDb = std::min (worst.reconstructionDb, r.reconstructionDb);
        worst.instrumentalLeakDb = std::max (worst.instrumentalLeakDb, r.instrumentalLeakDb);
        worst.acapellaBleedDb = std::max (worst.acapellaBleedDb, r.acapellaBleedDb);
        std::cout << "  " << juce::String (song.genre).paddedRight (' ', 30) << juce::String (r.reconstructionDb, 1).paddedLeft (' ', 6) << " dB "
                  << juce::String (100.0 * r.vocalShare, 0).paddedLeft (' ', 4) << " %   " << juce::String (r.instrumentalLeakDb, 1).paddedLeft (' ', 8) << " dB      "
                  << juce::String (r.acapellaBleedDb, 1).paddedLeft (' ', 8) << " dB      " << juce::String ((int) ms) << " ms/pass" << std::endl;
    }

    if (measured == 0)
    {
        std::cout << "  [SKIP] none of the reference songs is available locally" << std::endl;
    }
    else
    {
        check (worst.reconstructionDb > 15.0, "the four stems add up to the original (worst " + juce::String (worst.reconstructionDb, 1) + " dB SNR)");
        check (worst.instrumentalLeakDb < -20.0, "re-separating the instrumental finds almost no vocals (worst " + juce::String (worst.instrumentalLeakDb, 1) + " dB)");
        check (worst.acapellaBleedDb < -10.0, "re-separating the vocal stem finds little instrument bleed (worst " + juce::String (worst.acapellaBleedDb, 1) + " dB)");
    }

    // Quality modes: same network, more overlap/shifts = slower; results must agree closely.
    {
        const auto file = music.getChildFile ("DJ Musik\\02 Mr. Brightside.wav");
        const auto mix = readStereo44k (file, 40.0, 20.0);
        if (mix.getNumSamples() > 0 && ! analysis::isOnlineOnly (file))
        {
            std::array<std::array<juce::AudioBuffer<float>, numStems>, 3> results;
            double ms[3] = {};
            for (int q = 0; q < 3; ++q)
            {
                const double t = now();
                separator.separate (mix, (Quality) q, results[(size_t) q]);
                ms[q] = now() - t;
            }
            check (ms[0] < ms[2] && ms[1] < ms[2], "HIGH is the slowest mode",
                   "FAST " + juce::String ((int) ms[0]) + " ms, BALANCED " + juce::String ((int) ms[1]) + " ms, HIGH " + juce::String ((int) ms[2]) + " ms");
            const double diff = toDb ([&] { double e = 0.0; const int n = mix.getNumSamples();
                                            for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < n; ++i) { const double d = (double) results[0][3].getSample (ch, i) - results[2][3].getSample (ch, i); e += d * d; }
                                            return e / std::max (1.0e-20, energy (results[2][3], 0, n)); }());
            check (diff < -10.0, "FAST and HIGH produce the same vocals closely", "difference " + juce::String (diff, 1) + " dB");
        }
    }

    //==========================================================================
    std::cout << "Vocal removal through the whole chain (separation -> cache -> deck)" << std::endl;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto song = music.getChildFile ("DJ Musik\\02 Mr. Brightside.wav");
    if (song.existsAsFile() && ! analysis::isOnlineOnly (song))
    {
        // A 25 s excerpt as its own file, so the manager works on a small file.
        const auto excerpt = root.getChildFile ("excerpt.wav");
        const auto mix = readStereo44k (song, 40.0, 25.0);
        writeWav (excerpt, mix, 44100.0);

        StemManager manager (root.getChildFile ("manager-cache"), 0, formats);
        manager.setModelFile (modelFile);
        std::shared_ptr<const StemSet> delivered;
        juce::String deliveredError;
        bool done = false;

        const double t1 = now();
        const int id = manager.request (excerpt, Quality::balanced, true, [&] (std::shared_ptr<const StemSet> s, const juce::String& e)
                                        { delivered = s; deliveredError = e; done = true; });
        bool sawProgress = false;
        float lastFraction = 0.0f;
        bool monotonic = true;
        while (! done && now() - t1 < 120000)
        {
            const auto p = manager.getProgress (id);
            if (p.state == JobState::separating)
            {
                sawProgress = sawProgress || p.fraction > 0.0f;
                monotonic = monotonic && p.fraction + 1.0e-6f >= lastFraction;
                lastFraction = p.fraction;
            }
            pump (10);
        }
        const double firstMs = now() - t1;
        check (done && delivered != nullptr && deliveredError.isEmpty(), "manager separates a file in the background", deliveredError);
        check (monotonic, "progress never goes backwards");
        if (delivered == nullptr)
            return;

        // Second request: served from the cache.
        done = false;
        const double t2 = now();
        const int id2 = manager.request (excerpt, Quality::fast, true, [&] (std::shared_ptr<const StemSet> s, const juce::String& e)
                                         { delivered = s; deliveredError = e; done = true; });
        waitFor ([&] { return done; }, 20000);
        const double cachedMs = now() - t2;
        check (manager.getProgress (id2).fromCache && cachedMs < firstMs / 3.0, "the second request comes from the cache and is much faster",
               "first " + juce::String ((int) firstMs) + " ms, cached " + juce::String ((int) cachedMs) + " ms");
        check (manager.getCache().getNumEntries() == 1, "exactly one cache entry exists");

        // Changed file -> new fingerprint -> separated again.
        auto changed = mix;
        changed.applyGain (0.5f);
        writeWav (excerpt, changed, 44100.0);
        done = false;
        const int id3 = manager.request (excerpt, Quality::fast, true, [&] (std::shared_ptr<const StemSet> s, const juce::String& e)
                                         { delivered = s; deliveredError = e; done = true; });
        waitFor ([&] { return done; }, 120000);
        check (done && ! manager.getProgress (id3).fromCache && manager.getCache().getNumEntries() == 2, "a changed file is not served stale stems");

        // Missing model gives a clear error.
        StemManager noModel (root.getChildFile ("nomodel-cache"), 0, formats);
        noModel.setModelFile (root.getChildFile ("does-not-exist.onnx"));
        juce::String noModelError;
        bool noModelDone = false;
        noModel.request (excerpt, Quality::fast, true, [&] (std::shared_ptr<const StemSet>, const juce::String& e) { noModelError = e; noModelDone = true; });
        waitFor ([&] { return noModelDone; }, 10000);
        check (noModelError.contains ("not installed"), "a missing model is reported clearly", noModelError);

        // Cancelling a running job.
        {
            std::atomic<bool> cancelledCallback { false };
            bool finished = false;
            const auto longer = root.getChildFile ("long.wav");
            juce::AudioBuffer<float> longMix (2, 44100 * 60);
            for (int ch = 0; ch < 2; ++ch)
                for (int rep = 0; rep < 3; ++rep)
                    longMix.copyFrom (ch, rep * mix.getNumSamples(), mix, ch, 0, std::min (mix.getNumSamples(), longMix.getNumSamples() - rep * mix.getNumSamples()));
            writeWav (longer, longMix, 44100.0);
            const double t = now();
            const int idc = manager.request (longer, Quality::high, true, [&] (std::shared_ptr<const StemSet>, const juce::String&) { cancelledCallback = true; });
            waitFor ([&] { return manager.getProgress (idc).state == JobState::separating; }, 30000);
            manager.cancel (idc);
            waitFor ([&] { return ! manager.isBusy(); }, 30000);
            finished = ! manager.isBusy();
            check (finished && ! cancelledCallback.load() && manager.getProgress (idc).state == JobState::cancelled,
                   "a running job can be cancelled quickly", juce::String ((int) (now() - t)) + " ms");
        }

        // Deck: vocals off must equal the instrumental stems, with nothing of the original left.
        DeckEngine deck (formats);
        deck.prepareToPlay (blockSize, 44100.0);
        loadAndWait (deck, excerpt);
        changed = {};
        std::shared_ptr<const StemSet> set = manager.getCache().load (manager.getProgress (id).state == JobState::done ? delivered->fingerprint : delivered->fingerprint, Quality::fast);
        check (set != nullptr, "stems can be loaded from the cache for the deck");
        if (set != nullptr)
        {
            deck.setStems (set);
            deck.setPositionSeconds (5.0);
            deck.play();
            render (deck, 0.2, 44100.0);
            const int startSample = (int) (deck.getCurrentPositionSeconds() * 44100.0);
            deck.setStemGains ({ 1.0f, 1.0f, 1.0f, 0.0f });
            render (deck, 0.05, 44100.0);
            const int blockStart = (int) std::lround (deck.getCurrentPositionSeconds() * 44100.0);
            const auto out = render (deck, 1.0, 44100.0);
            juce::ignoreUnused (startSample);

            double err = 0.0, ref = 0.0, vocalCorrelation = 0.0, vocalE = 0.0, outE = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    const int p = blockStart + i;
                    float inst = 0.0f;
                    for (int s = 0; s < numStems; ++s)
                        if (s != (int) Stem::vocals)
                            inst += set->audio[(size_t) s].getSample (ch, p);
                    const double d = (double) out.getSample (ch, i) - inst;
                    err += d * d;
                    ref += (double) inst * inst;
                    const double v = set->audio[(size_t) Stem::vocals].getSample (ch, p);
                    vocalCorrelation += v * out.getSample (ch, i);
                    vocalE += v * v;
                    outE += (double) out.getSample (ch, i) * out.getSample (ch, i);
                }
            check (toDb (err / std::max (1.0e-20, ref)) < -60.0, "vocals off: deck output equals drums + bass + other of the separation",
                   juce::String (toDb (err / std::max (1.0e-20, ref)), 1) + " dB");
            const double corr = vocalCorrelation / std::sqrt (std::max (1.0e-20, vocalE * outE));
            check (std::abs (corr) < 0.2, "vocals off: output is uncorrelated with the vocal stem", "correlation " + juce::String (corr, 3));
        }
    }
}

//==============================================================================
int runStemTests()
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJStemTests");
    root.deleteRecursively();
    root.createDirectory();

    testCache (root);
    testDeckStems (root);

    const auto modelFile = findStemModel();
    if (! modelFile.existsAsFile())
        std::cout << "  [SKIP] AI model tests: model not found at " << modelFile.getFullPathName() << " (set PATHBOARD_MODEL_DIR)" << std::endl;
    else
        testWithModel (modelFile, root);

    root.deleteRecursively();
    return 0;
}
