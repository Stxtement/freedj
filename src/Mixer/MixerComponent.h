#pragma once

#include "Audio/AudioEngine.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

class LevelMeter : public juce::Component
{
public:
    void setLevel (float linear);
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f, peak = 0.0f;
    int peakHoldFrames = 0;
};

class MixerComponent : public juce::Component, private juce::Timer
{
public:
    explicit MixerComponent (AudioEngine& engine);

    void paint (juce::Graphics&) override;
    void resized() override;

    struct Channel
    {
        juce::Slider gain, high, mid, low, filter;
        juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
        juce::TextButton cueButton { "CUE" };
        LevelMeter meter;
    };

    Channel& getChannel (int deck) { return channels[(size_t) deck]; }
    juce::Slider& getCrossfader() { return crossfader; }
    juce::Slider& getMasterVolume() { return masterVolume; }
    juce::Slider& getBoothVolume() { return boothVolume; }
    juce::Slider& getHeadphoneMix() { return headphoneMix; }
    juce::Slider& getHeadphoneVolume() { return headphoneVolume; }

private:
    void timerCallback() override;
    void setupKnob (juce::Slider& s, const juce::String& tooltip, double min, double max, double def, juce::Colour colour);
    void updateEq (int deck);

    AudioEngine& engine;
    std::array<Channel, AudioEngine::numDecks> channels;

    juce::Slider crossfader { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::Slider masterVolume, boothVolume, headphoneMix, headphoneVolume;
    LevelMeter masterMeterL, masterMeterR;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};

} // namespace pathboard
