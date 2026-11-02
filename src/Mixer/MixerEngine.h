#pragma once

#include "Audio/DeckEngine.h"
#include "Effects/Effects.h"
#include <array>

namespace pathboard
{

// Device callback that pulls both decks and mixes them.
// Output routing (if the device has enough channels):
//   ch 1/2 = master, ch 3/4 = headphones (cue), ch 5/6 = booth.
class MixerEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr int numDecks = 2;

    MixerEngine (DeckEngine& deckA, DeckEngine& deckB);

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void setCrossfader (float position);          // -1 = deck A only, 1 = deck B only
    float getCrossfader() const { return crossfader.load(); }
    void setChannelFader (int deck, float value); // 0..1
    void setMasterVolume (float value);           // 0..1
    void setBoothVolume (float value);            // 0..1

    void setHeadphoneCue (int deck, bool enabled);
    bool isHeadphoneCueEnabled (int deck) const;
    void setHeadphoneMix (float cueToMaster);     // 0 = cue only, 1 = master only
    void setHeadphoneVolume (float value);

    // Effect rack of a deck (two slots), applied before the channel fader.
    fx::EffectRack& getEffects (int deck) { return racks[(size_t) juce::jlimit (0, numDecks - 1, deck)]; }

    float getDeckLevel (int deck) const;
    float getMasterLevel (int channel) const;
    int getNumOutputChannels() const { return numActiveOutputs.load(); }

private:
    std::array<DeckEngine*, numDecks> decks;
    std::array<juce::AudioBuffer<float>, numDecks> deckBuffers;
    std::array<fx::EffectRack, numDecks> racks;
    juce::AudioBuffer<float> masterBuffer, cueBuffer;

    std::atomic<float> crossfader { 0.0f };
    std::array<std::atomic<float>, numDecks> channelFaders;
    std::array<std::atomic<bool>, numDecks> headphoneCue;
    std::array<std::atomic<float>, numDecks> deckLevels;
    std::array<std::atomic<float>, 2> masterLevels;

    std::atomic<float> masterVolume { 0.8f };
    std::atomic<float> boothVolume { 0.0f };
    std::atomic<float> headphoneMix { 0.0f };
    std::atomic<float> headphoneVolume { 0.8f };
    std::atomic<int> numActiveOutputs { 0 };
};

} // namespace pathboard
