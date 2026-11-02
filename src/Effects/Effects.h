#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>

namespace pathboard::fx
{

// Effects per deck, like the FX slots in DJ software: every deck has two slots, every slot holds one effect with
// on/off, DEPTH (how much of the effect is heard) and one effect-specific PARAM. Time-based effects follow the
// deck's tempo and, while the deck plays, its beat grid, so echoes, sweeps and gates stay on the beat.
enum EffectType { none = 0, echo, reverb, flanger, phaser, filter, gate, crush, roll, numTypes };

const char* effectName (int type);   // "Echo", ...
const char* paramName (int type);    // what PARAM does for this effect: "Time", "Size", ...

struct BeatInfo
{
    double bpm = 120.0;           // tempo that is audible
    double beatAtStart = -1.0;    // beat position at the start of the block (grid-based); < 0 = unknown / deck not playing
};

class Effect
{
public:
    virtual ~Effect() = default;
    virtual void prepare (double sampleRate) = 0;   // allocates; not for the audio thread
    virtual void reset() = 0;                       // clears the state (called when the effect is switched on)
    // In place on one stereo block. depth, param: 0..1.
    virtual void process (float* left, float* right, int numSamples, float depth, float param, const BeatInfo& beat) = 0;
};

std::unique_ptr<Effect> createEffect (int type);

class EffectSlot
{
public:
    void prepare (double sampleRate, int maxBlock);      // before audio starts
    // Audio thread. The block is changed in place.
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const BeatInfo& beat);

    // Message thread / controller.
    void setType (int type)     { typeValue = juce::jlimit (0, (int) numTypes - 1, type); }
    void setOn (bool on)        { onValue = on; }
    void setDepth (float depth) { depthValue = juce::jlimit (0.0f, 1.0f, depth); }
    void setParam (float param) { paramValue = juce::jlimit (0.0f, 1.0f, param); }
    int getType() const         { return typeValue.load(); }
    bool isOn() const           { return onValue.load(); }
    float getDepth() const      { return depthValue.load(); }
    float getParam() const      { return paramValue.load(); }

private:
    std::array<std::unique_ptr<Effect>, numTypes> effects;
    juce::AudioBuffer<float> scratch;
    double sampleRate = 44100.0;
    int activeType = 0;
    float mix = 0.0f;   // 0 = bypassed, 1 = effect; ramps in 20 ms so switching on/off does not click

    std::atomic<int> typeValue { echo };
    std::atomic<bool> onValue { false };
    std::atomic<float> depthValue { 0.5f }, paramValue { 0.5f };
};

class EffectRack
{
public:
    static constexpr int numSlots = 2;

    void prepare (double sampleRate, int maxBlock);
    // Audio thread: runs the slots in series, then guards the output (no NaN / runaway levels).
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const BeatInfo& beat);

    EffectSlot& slot (int index) { return slots[(size_t) juce::jlimit (0, numSlots - 1, index)]; }

private:
    std::array<EffectSlot, numSlots> slots;
};

} // namespace pathboard::fx
