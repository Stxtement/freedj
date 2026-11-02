#pragma once

#include "Mixer/MixerEngine.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

// Effect strip: two slots per deck (on/off, effect, DEPTH, PARAM). It also follows changes made from the
// controller, so the screen always shows what is heard.
class FxComponent : public juce::Component, private juce::Timer
{
public:
    explicit FxComponent (MixerEngine& mixer);

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct SlotUi
    {
        fx::EffectSlot* slot = nullptr;
        juce::TextButton on { "FX" };
        juce::ComboBox type;
        juce::Slider depth, param;
        juce::Label depthLabel { {}, "DEPTH" }, paramLabel;
    };

    void timerCallback() override;
    void updateParamLabel (SlotUi&);

    MixerEngine& mixer;
    std::array<std::array<SlotUi, fx::EffectRack::numSlots>, MixerEngine::numDecks> ui;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxComponent)
};

} // namespace pathboard
