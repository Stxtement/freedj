#pragma once

#include "Audio/DeckEngine.h"
#include "Mixer/MixerEngine.h"

namespace pathboard
{

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    // Returns an empty string on success, otherwise the device error.
    juce::String initialise (const juce::XmlElement* savedDeviceState);
    std::unique_ptr<juce::XmlElement> createDeviceStateXml() const;

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }
    juce::AudioFormatManager& getFormatManager() { return formatManager; }
    DeckEngine& getDeck (int index) { return index == 0 ? deckA : deckB; }
    MixerEngine& getMixer() { return mixer; }

    static constexpr int numDecks = MixerEngine::numDecks;

private:
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    DeckEngine deckA { formatManager };
    DeckEngine deckB { formatManager };
    MixerEngine mixer { deckA, deckB };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

} // namespace pathboard
