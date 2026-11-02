#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <memory>
#include <array>

namespace pathboard::stems
{

// Order of the stems produced by the HTDemucs model.
enum class Stem { drums = 0, bass = 1, other = 2, vocals = 3 };
constexpr int numStems = 4;
const char* stemName (int stem);

// FAST/BALANCED/HIGH use the same network; they differ in how much the segments overlap and how many
// time-shifted passes are averaged. More overlap/shifts = fewer boundary and phase artefacts, but slower.
enum class Quality { fast = 0, balanced = 1, high = 2 };
const char* qualityName (Quality);

constexpr int modelSampleRate = 44100;
constexpr int modelSegmentSamples = 343980; // 7.8 s, fixed by the exported graph

// Wraps an ONNX Runtime session of the HTDemucs model (DirectML GPU if possible, otherwise CPU).
// separate() is blocking and not re-entrant; call it from a worker thread, never from the audio thread.
class StemSeparator
{
public:
    StemSeparator();
    ~StemSeparator();

    juce::Result load (const juce::File& modelFile, bool preferGpu);
    bool isLoaded() const;
    juce::String getDeviceName() const;   // "GPU (DirectML)" or "CPU"

    // `mix`: stereo, 44.1 kHz. `out[i]` receives stem i with the same length. `progress` gets 0..1 and
    // returns false to cancel (the result is then "cancelled").
    juce::Result separate (const juce::AudioBuffer<float>& mix, Quality quality,
                           std::array<juce::AudioBuffer<float>, numStems>& out,
                           const std::function<bool (float)>& progress = {});

    struct Tuning { double overlap; int shifts; };
    static Tuning getTuning (Quality q);

    // ONNX Runtime graph optimisation: 0 = none, 1 = basic, 2 = extended, 99 = all, -1 = automatic
    // (none on the GPU, all on the CPU). On DirectML every level above 0 produces wrong output with this
    // model (measured with ONNX Runtime 1.22 and 1.24), so automatic is the only sensible choice.
    void setOptimisationLevel (int level) { optimisationLevel = level; }

    // True if the GPU result failed the plausibility check and the separator switched to the CPU.
    bool fellBackToCpu() const { return switchedToCpu; }

private:
    juce::Result loadLocked (const juce::File& modelFile, bool preferGpu);
    juce::Result runSeparation (const juce::AudioBuffer<float>& mix, Quality quality,
                                std::array<juce::AudioBuffer<float>, numStems>& out,
                                const std::function<bool (float)>& progress, bool& gpuResultInvalid);

    struct Impl;
    std::unique_ptr<Impl> impl;
    int optimisationLevel = -1;
    bool switchedToCpu = false;
    juce::File loadedModel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemSeparator)
};

} // namespace pathboard::stems
