#ifndef NOMINMAX
 #define NOMINMAX
#endif

#include "GenreClassifier.h"
#include <juce_dsp/juce_dsp.h>

#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace pathboard::genre
{

namespace
{
    constexpr int frameSize = 512, hopSize = 256, patchHop = 62;
    constexpr double maxSeconds = 12.0 * 60.0;

    double hzToMel (double f)   // Slaney mel scale: linear below 1 kHz, logarithmic above
    {
        const double fsp = 200.0 / 3.0, minLogHz = 1000.0, minLogMel = minLogHz / fsp, logStep = std::log (6.4) / 27.0;
        return f < minLogHz ? f / fsp : minLogMel + std::log (f / minLogHz) / logStep;
    }

    double melToHz (double m)
    {
        const double fsp = 200.0 / 3.0, minLogHz = 1000.0, minLogMel = minLogHz / fsp, logStep = std::log (6.4) / 27.0;
        return m < minLogMel ? m * fsp : minLogHz * std::exp ((m - minLogMel) * logStep);
    }

    std::vector<float> makeFilterbank (bool areaNormalised)
    {
        const int bins = frameSize / 2 + 1;
        std::vector<float> bank ((size_t) melBands * bins, 0.0f);
        const double lowMel = hzToMel (0.0), highMel = hzToMel (8000.0);
        std::vector<double> edges ((size_t) melBands + 2);
        for (int i = 0; i < melBands + 2; ++i)
            edges[(size_t) i] = melToHz (lowMel + (highMel - lowMel) * i / (melBands + 1));

        for (int b = 0; b < melBands; ++b)
        {
            const double lo = edges[(size_t) b], mid = edges[(size_t) b + 1], hi = edges[(size_t) b + 2];
            for (int k = 0; k < bins; ++k)
            {
                const double f = (double) k * sampleRate / frameSize;
                double w = 0.0;
                if (f > lo && f <= mid)       w = (f - lo) / (mid - lo);
                else if (f > mid && f < hi)   w = (hi - f) / (hi - mid);
                if (areaNormalised)
                    w *= 2.0 / (hi - lo);
                bank[(size_t) b * bins + (size_t) k] = (float) w;
            }
        }
        return bank;
    }
}

//==============================================================================
std::vector<float> resampleSinc (const std::vector<float>& in, double inRate, double outRate)
{
    if (std::abs (inRate - outRate) < 0.5)
        return in;

    const double ratio = inRate / outRate;                     // input samples per output sample
    const double cutoff = std::min (1.0, outRate / inRate) * 0.97;   // relative to the input Nyquist
    constexpr int halfTaps = 24;
    const int outLength = (int) ((double) in.size() / ratio);
    std::vector<float> out ((size_t) outLength);
    const int n = (int) in.size();
    const double pi = juce::MathConstants<double>::pi;

    for (int i = 0; i < outLength; ++i)
    {
        const double centre = (double) i * ratio;
        const int first = (int) std::floor (centre) - halfTaps + 1;
        double sum = 0.0;
        for (int j = first; j < first + 2 * halfTaps; ++j)
        {
            if (j < 0 || j >= n)
                continue;
            const double x = (double) j - centre;
            const double arg = pi * x * cutoff;
            const double sinc = std::abs (arg) < 1.0e-9 ? 1.0 : std::sin (arg) / arg;
            const double window = 0.5 + 0.5 * std::cos (pi * x / halfTaps);   // Hann over the kernel
            sum += (double) in[(size_t) j] * sinc * window * cutoff;
        }
        out[(size_t) i] = (float) sum;
    }
    return out;
}

bool decodeMono16k (const juce::File& file, juce::AudioFormatManager& formats, std::vector<float>& out,
                    juce::String& error, const std::atomic<bool>* cancel)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        error = "Cannot read audio: " + file.getFileName();
        return false;
    }

    const juce::int64 total = std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (maxSeconds * reader->sampleRate));
    std::vector<float> mono;
    mono.reserve ((size_t) total);
    juce::AudioBuffer<float> buffer (2, 1 << 16);

    for (juce::int64 pos = 0; pos < total; pos += buffer.getNumSamples())
    {
        if (cancel != nullptr && cancel->load())
        {
            error = "cancelled";
            return false;
        }
        const int n = (int) std::min<juce::int64> (buffer.getNumSamples(), total - pos);
        if (! reader->read (&buffer, 0, n, pos, true, true))
        {
            if (pos == 0)
            {
                error = "Decoding error in: " + file.getFileName();
                return false;
            }
            break;
        }
        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (1);
        for (int i = 0; i < n; ++i)
            mono.push_back (0.5f * (l[i] + r[i]));
    }

    if ((double) mono.size() < reader->sampleRate * 5.0)
    {
        error = "Track is too short for genre detection";
        return false;
    }
    out = resampleSinc (mono, reader->sampleRate, sampleRate);
    return true;
}

//==============================================================================
std::vector<float> computeMelPatches (const std::vector<float>& audio, const MelOptions& options, int maxPatches, int& numPatches)
{
    numPatches = 0;
    const int n = (int) audio.size();
    if (n < frameSize)
        return {};

    const int bins = frameSize / 2 + 1;
    const int frames = (n - frameSize) / hopSize + 1;
    if (frames < patchFrames)
        return {};

    // Window
    std::vector<float> window ((size_t) frameSize);
    double sum = 0.0;
    for (int i = 0; i < frameSize; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (frameSize - 1));
        sum += window[(size_t) i];
    }
    if (options.normalisedWindow)
        for (auto& w : window)
            w = (float) (w * 2.0 / sum);

    const auto bank = makeFilterbank (options.areaNormalisedFilters);

    // Choose patches evenly over the track, leaving out the first and last 10 % (intros / outros).
    const int totalPatches = (frames - patchFrames) / patchHop + 1;
    const int from = totalPatches > 8 ? totalPatches / 10 : 0;
    const int to = totalPatches > 8 ? totalPatches - totalPatches / 10 : totalPatches;
    const int available = std::max (1, to - from);
    numPatches = std::min (maxPatches, available);

    std::vector<float> patches ((size_t) numPatches * patchFrames * melBands);
    juce::dsp::FFT fft (9);
    std::vector<float> data ((size_t) frameSize * 2);

    for (int p = 0; p < numPatches; ++p)
    {
        const int patchIndex = from + (numPatches == 1 ? available / 2 : (int) ((juce::int64) p * (available - 1) / (numPatches - 1)));
        const int firstFrame = patchIndex * patchHop;
        float* out = patches.data() + (size_t) p * patchFrames * melBands;

        for (int f = 0; f < patchFrames; ++f)
        {
            const float* src = audio.data() + (size_t) (firstFrame + f) * hopSize;
            for (int i = 0; i < frameSize; ++i)
                data[(size_t) i] = src[i] * window[(size_t) i];
            std::fill (data.begin() + frameSize, data.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (data.data(), true);

            for (int b = 0; b < melBands; ++b)
            {
                const float* filter = bank.data() + (size_t) b * bins;
                double acc = 0.0;
                for (int k = 0; k < bins; ++k)
                {
                    const double magnitude = data[(size_t) k];
                    acc += (double) filter[k] * (options.powerSpectrum ? magnitude * magnitude : magnitude);
                }
                out[(size_t) f * melBands + (size_t) b] = (float) std::log10 (1.0 + options.logScale * acc);
            }
        }
    }
    return patches;
}

//==============================================================================
struct GenreClassifier::Impl
{
    Ort::Env env { ORT_LOGGING_LEVEL_ERROR, "PathboardGenre" };
    std::unique_ptr<Ort::Session> session;
    std::string inputName, predictionName;
    std::mutex mutex;
};

GenreClassifier::GenreClassifier() : impl (std::make_unique<Impl>()) {}
GenreClassifier::~GenreClassifier() = default;

bool GenreClassifier::isLoaded() const { return impl->session != nullptr; }

juce::Result GenreClassifier::load (const juce::File& modelFile, const juce::StringArray&)
{
    std::lock_guard<std::mutex> lock (impl->mutex);
    impl->session.reset();
    if (! modelFile.existsAsFile())
        return juce::Result::fail ("Genre model not found: " + modelFile.getFullPathName());

    try
    {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.SetIntraOpNumThreads (std::max (1, juce::SystemStats::getNumCpus() / 4));
        impl->session = std::make_unique<Ort::Session> (impl->env, modelFile.getFullPathName().toWideCharPointer(), options);

        Ort::AllocatorWithDefaultOptions allocator;
        impl->inputName = impl->session->GetInputNameAllocated (0, allocator).get();
        // The model has two outputs: style predictions [n, 400] and embeddings [n, 1280].
        impl->predictionName.clear();
        for (size_t i = 0; i < impl->session->GetOutputCount(); ++i)
        {
            const auto shape = impl->session->GetOutputTypeInfo (i).GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() == 2 && shape[1] == numStyles)
                impl->predictionName = impl->session->GetOutputNameAllocated (i, allocator).get();
        }
        if (impl->predictionName.empty())
        {
            impl->session.reset();
            return juce::Result::fail ("This is not the 400-style Discogs-EffNet model");
        }
    }
    catch (const std::exception& e)
    {
        impl->session.reset();
        return juce::Result::fail (e.what());
    }
    return juce::Result::ok();
}

juce::Result GenreClassifier::classify (const std::vector<float>& mono16k, std::vector<float>& probabilities, int maxPatches,
                                        const MelOptions& options)
{
    std::lock_guard<std::mutex> lock (impl->mutex);
    if (impl->session == nullptr)
        return juce::Result::fail ("Genre model is not loaded");

    int patches = 0;
    auto input = computeMelPatches (mono16k, options, maxPatches, patches);
    if (patches == 0)
        return juce::Result::fail ("Track is too short for genre detection");

    try
    {
        auto memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[] = { patches, patchFrames, melBands };
        auto tensor = Ort::Value::CreateTensor<float> (memory, input.data(), input.size(), shape, 3);
        const char* inputNames[] = { impl->inputName.c_str() };
        const char* outputNames[] = { impl->predictionName.c_str() };
        auto outputs = impl->session->Run (Ort::RunOptions { nullptr }, inputNames, &tensor, 1, outputNames, 1);
        const float* result = outputs[0].GetTensorData<float>();

        probabilities.assign (numStyles, 0.0f);
        for (int p = 0; p < patches; ++p)
            for (int s = 0; s < numStyles; ++s)
                probabilities[(size_t) s] += result[(size_t) p * numStyles + (size_t) s] / (float) patches;
    }
    catch (const std::exception& e)
    {
        return juce::Result::fail (juce::String ("Genre detection failed: ") + e.what());
    }
    return juce::Result::ok();
}

} // namespace pathboard::genre
