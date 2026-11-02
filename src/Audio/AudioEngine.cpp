#include "AudioEngine.h"

namespace pathboard
{

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (&mixer);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::initialise (const juce::XmlElement* savedDeviceState)
{
    // Up to 6 outputs: master (1/2), headphones (3/4), booth (5/6).
    auto error = deviceManager.initialise (0, 6, savedDeviceState, true);
    deviceManager.addAudioCallback (&mixer);
    return error;
}

std::unique_ptr<juce::XmlElement> AudioEngine::createDeviceStateXml() const
{
    return deviceManager.createStateXml();
}

} // namespace pathboard
