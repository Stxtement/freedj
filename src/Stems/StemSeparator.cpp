#ifndef NOMINMAX
 #define NOMINMAX // windows.h (pulled in by the ONNX headers) must not define min/max macros
#endif

#include "StemSeparator.h"

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace pathboard::stems
{

const char* stemName (int s)
{
    static const char* names[] = { "drums", "bass", "other", "vocals" };
    return juce::isPositiveAndBelow (s, numStems) ? names[s] : "";
}

const char* qualityName (Quality q)
{
    switch (q)
    {
        case Quality::fast:     return "FAST";
        case Quality::balanced: return "BALANCED";
        case Quality::high:     return "HIGH";
    }
    return "";
}

StemSeparator::Tuning StemSeparator::getTuning (Quality q)
{
    switch (q)
    {
        case Quality::fast:     return { 0.10, 1 };
        case Quality::balanced: return { 0.25, 1 };
        case Quality::high:     return { 0.50, 2 };
    }
    return { 0.25, 1 };
}

//==============================================================================
struct StemSeparator::Impl
{
    Ort::Env env { ORT_LOGGING_LEVEL_ERROR, "FreeDJStems" };
    std::unique_ptr<Ort::Session> session;
    juce::String device;
    bool onGpu = false;
    std::mutex runMutex;
    std::string inputName, outputName;
};

StemSeparator::StemSeparator() : impl (std::make_unique<Impl>()) {}
StemSeparator::~StemSeparator() = default;

bool StemSeparator::isLoaded() const { return impl->session != nullptr; }
juce::String StemSeparator::getDeviceName() const { return impl->device; }

juce::Result StemSeparator::load (const juce::File& modelFile, bool preferGpu)
{
    std::lock_guard<std::mutex> lock (impl->runMutex);
    return loadLocked (modelFile, preferGpu);
}

juce::Result StemSeparator::loadLocked (const juce::File& modelFile, bool preferGpu)
{
    impl->session.reset();
    impl->device.clear();

    if (! modelFile.existsAsFile())
        return juce::Result::fail ("Stem model not found: " + modelFile.getFullPathName());

    loadedModel = modelFile;

    auto tryCreate = [&] (bool gpu) -> juce::Result
    {
        try
        {
            Ort::SessionOptions options;
            const int level = optimisationLevel >= 0 ? optimisationLevel : (gpu ? 0 : 99);
            switch (level)
            {
                case 0:  options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_DISABLE_ALL); break;
                case 1:  options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_BASIC); break;
                case 2:  options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_EXTENDED); break;
                default: options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL); break;
            }

            if (gpu)
            {
                // DirectML requirements: no memory pattern optimisation, sequential execution.
                options.DisableMemPattern();
                options.SetExecutionMode (ExecutionMode::ORT_SEQUENTIAL);
                Ort::ThrowOnError (OrtSessionOptionsAppendExecutionProvider_DML (options, 0));
            }
            else
            {
                options.SetIntraOpNumThreads (juce::jmax (1, juce::SystemStats::getNumCpus() - 2));
            }

            impl->session = std::make_unique<Ort::Session> (impl->env, modelFile.getFullPathName().toWideCharPointer(), options);

            Ort::AllocatorWithDefaultOptions allocator;
            impl->inputName = impl->session->GetInputNameAllocated (0, allocator).get();
            impl->outputName = impl->session->GetOutputNameAllocated (0, allocator).get();
            impl->onGpu = gpu;
            impl->device = gpu ? "GPU (DirectML)" : "CPU";
            return juce::Result::ok();
        }
        catch (const std::exception& e)
        {
            impl->session.reset();
            return juce::Result::fail (juce::String (gpu ? "GPU: " : "CPU: ") + e.what());
        }
    };

    if (preferGpu)
    {
        const auto gpu = tryCreate (true);
        if (gpu.wasOk())
            return gpu;
        DBG ("Stem model: " << gpu.getErrorMessage() << " - using the CPU");
    }
    return tryCreate (false);
}

//==============================================================================
juce::Result StemSeparator::separate (const juce::AudioBuffer<float>& mix, Quality quality,
                                      std::array<juce::AudioBuffer<float>, numStems>& out,
                                      const std::function<bool (float)>& progress)
{
    std::lock_guard<std::mutex> lock (impl->runMutex);
    if (impl->session == nullptr)
        return juce::Result::fail ("Stem model is not loaded");
    if (mix.getNumChannels() != 2 || mix.getNumSamples() < 1)
        return juce::Result::fail ("Stem separation needs stereo audio");

    bool invalid = false;
    auto result = runSeparation (mix, quality, out, progress, invalid);

    if (invalid && impl->onGpu)
    {
        // The GPU produced output that is not a plausible separation of the input (driver or DirectML
        // problem). Never hand such audio to the DJ: redo the whole job on the CPU.
        DBG ("Stem separation: implausible GPU result, switching to the CPU");
        const auto reload = loadLocked (loadedModel, false);
        if (reload.failed())
            return reload;
        switchedToCpu = true;
        result = runSeparation (mix, quality, out, progress, invalid);
    }

    if (result.wasOk() && invalid)
        return juce::Result::fail ("Stem separation produced implausible output");
    return result;
}

juce::Result StemSeparator::runSeparation (const juce::AudioBuffer<float>& mix, Quality quality,
                                           std::array<juce::AudioBuffer<float>, numStems>& out,
                                           const std::function<bool (float)>& progress, bool& gpuResultInvalid)
{
    gpuResultInvalid = false;
    const int length = mix.getNumSamples();
    constexpr int S = modelSegmentSamples;
    const auto tuning = getTuning (quality);

    // The exported graph normalises its input and restores the scale itself (measured: an external
    // mean/std normalisation changes nothing), so the audio is passed as it is.
    for (auto& buffer : out)
    {
        buffer.setSize (2, length);
        buffer.clear();
    }

    // Triangular weights so overlapping segments cross-fade instead of producing seams.
    std::vector<float> weight ((size_t) S);
    for (int i = 0; i < S; ++i)
        weight[(size_t) i] = (float) std::min (i + 1, S - i);
    const float maxWeight = *std::max_element (weight.begin(), weight.end());
    for (auto& w : weight)
        w /= maxWeight;

    const int stride = std::max (1, (int) std::lround (S * (1.0 - tuning.overlap)));
    const int shifts = std::max (1, tuning.shifts);
    const int maxShift = modelSampleRate / 2;

    std::vector<float> input ((size_t) 2 * S), weightSum;
    const int64_t inputShape[] = { 1, 2, S };
    const char* inputNames[] = { impl->inputName.c_str() };
    const char* outputNames[] = { impl->outputName.c_str() };
    auto memoryInfo = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);

    int segmentsPerShift = 0;
    for (int start = 0; start < length + (shifts > 1 ? maxShift : 0); start += stride)
        ++segmentsPerShift;
    const int totalSegments = segmentsPerShift * shifts;
    int doneSegments = 0;

    for (int shiftIndex = 0; shiftIndex < shifts; ++shiftIndex)
    {
        // Audio position p is processed at p + shift; averaging shifted runs removes segment-grid artefacts.
        const int shift = shifts == 1 ? 0 : (shiftIndex * maxShift) / shifts;
        const int paddedLength = length + shift;

        std::array<std::vector<float>, numStems * 2> acc;
        for (auto& a : acc)
            a.assign ((size_t) paddedLength, 0.0f);
        weightSum.assign ((size_t) paddedLength, 0.0f);

        for (int start = 0; start < paddedLength; start += stride)
        {
            double inputEnergy = 0.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* src = mix.getReadPointer (ch);
                float* dst = input.data() + (size_t) ch * S;
                for (int i = 0; i < S; ++i)
                {
                    const int p = start + i - shift;
                    const float v = (p >= 0 && p < length) ? src[p] : 0.0f;
                    dst[i] = v;
                    inputEnergy += (double) v * v;
                }
            }

            try
            {
                auto tensor = Ort::Value::CreateTensor<float> (memoryInfo, input.data(), input.size(), inputShape, 3);
                auto outputs = impl->session->Run (Ort::RunOptions { nullptr }, inputNames, &tensor, 1, outputNames, 1);
                const float* result = outputs[0].GetTensorData<float>();

                // Plausibility: the four stems must add up to roughly the input. A working separation
                // reconstructs the mix to ~25 dB; garbage from a broken GPU path is off by 40+ dB.
                double errorEnergy = 0.0;
                bool finite = true;
                for (int ch = 0; ch < 2 && finite; ++ch)
                    for (int i = 0; i < S; ++i)
                    {
                        float sum = 0.0f;
                        for (int stem = 0; stem < numStems; ++stem)
                            sum += result[((size_t) stem * 2 + (size_t) ch) * S + (size_t) i];
                        if (! std::isfinite (sum)) { finite = false; break; }
                        const double d = (double) sum - input[(size_t) ch * S + (size_t) i];
                        errorEnergy += d * d;
                    }
                if (! finite || errorEnergy > 0.5 * inputEnergy + 1.0e-6)
                {
                    gpuResultInvalid = true;
                    return juce::Result::fail ("implausible output");
                }

                const int span = std::min (S, paddedLength - start);
                for (int stem = 0; stem < numStems; ++stem)
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const float* r = result + ((size_t) stem * 2 + (size_t) ch) * S;
                        float* a = acc[(size_t) stem * 2 + (size_t) ch].data() + start;
                        for (int i = 0; i < span; ++i)
                            a[i] += r[i] * weight[(size_t) i];
                    }
                for (int i = 0; i < span; ++i)
                    weightSum[(size_t) (start + i)] += weight[(size_t) i];
            }
            catch (const std::exception& e)
            {
                return juce::Result::fail (juce::String ("Stem separation failed: ") + e.what());
            }

            ++doneSegments;
            if (progress && ! progress ((float) doneSegments / (float) totalSegments))
                return juce::Result::fail ("cancelled");
        }

        const float shareOfRun = 1.0f / (float) shifts;
        for (int stem = 0; stem < numStems; ++stem)
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* a = acc[(size_t) stem * 2 + (size_t) ch].data();
                float* dst = out[(size_t) stem].getWritePointer (ch);
                for (int p = 0; p < length; ++p)
                {
                    const float w = weightSum[(size_t) (p + shift)];
                    const float v = w > 0.0f ? a[p + shift] / w : 0.0f;
                    dst[p] += v * shareOfRun;
                }
            }
    }

    return juce::Result::ok();
}

} // namespace pathboard::stems
