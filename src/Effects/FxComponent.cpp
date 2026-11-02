#include "FxComponent.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard
{

FxComponent::FxComponent (MixerEngine& m) : mixer (m)
{
    for (int d = 0; d < MixerEngine::numDecks; ++d)
        for (int s = 0; s < fx::EffectRack::numSlots; ++s)
        {
            auto& u = ui[(size_t) d][(size_t) s];
            u.slot = &mixer.getEffects (d).slot (s);

            u.on.setClickingTogglesState (true);
            u.on.setTooltip ("Effect on / off");
            u.on.setColour (juce::TextButton::buttonOnColourId, Colours::cue);
            u.on.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            u.on.onClick = [&u]
            {
                if (u.on.getToggleState() && u.slot->getType() == fx::none)
                    u.slot->setType (fx::echo);
                u.slot->setOn (u.on.getToggleState());
            };

            for (int t = 0; t < fx::numTypes; ++t)
                u.type.addItem (fx::effectName (t), t + 1);
            u.type.setSelectedId (u.slot->getType() + 1, juce::dontSendNotification);
            u.type.onChange = [this, &u]
            {
                u.slot->setType (u.type.getSelectedId() - 1);
                updateParamLabel (u);
            };

            for (auto* slider : { &u.depth, &u.param })
            {
                slider->setSliderStyle (juce::Slider::LinearHorizontal);
                slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                slider->setRange (0.0, 1.0, 0.001);
                slider->setColour (juce::Slider::trackColourId, Colours::forDeck (d));
            }
            u.depth.setValue (u.slot->getDepth(), juce::dontSendNotification);
            u.param.setValue (u.slot->getParam(), juce::dontSendNotification);
            u.depth.setTooltip ("How much of the effect is heard");
            u.depth.onValueChange = [&u] { u.slot->setDepth ((float) u.depth.getValue()); };
            u.param.onValueChange = [&u] { u.slot->setParam ((float) u.param.getValue()); };

            for (auto* label : { &u.depthLabel, &u.paramLabel })
            {
                label->setFont (juce::FontOptions (10.0f));
                label->setColour (juce::Label::textColourId, Colours::textDim);
                label->setInterceptsMouseClicks (false, false);
            }
            updateParamLabel (u);

            for (auto* c : std::initializer_list<juce::Component*> { &u.on, &u.type, &u.depth, &u.param, &u.depthLabel, &u.paramLabel })
                addAndMakeVisible (c);
        }

    startTimerHz (15);
}

void FxComponent::updateParamLabel (SlotUi& u)
{
    const auto name = juce::String (fx::paramName (u.slot->getType())).toUpperCase();
    u.paramLabel.setText (name.isEmpty() ? "PARAM" : name, juce::dontSendNotification);
    u.param.setTooltip (name.isEmpty() ? "Effect parameter" : "Effect parameter: " + name.toLowerCase());
}

void FxComponent::paint (juce::Graphics& g)
{
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f);

    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    const auto half = getLocalBounds().reduced (6, 4);
    const int w = half.getWidth() / 2;
    for (int d = 0; d < MixerEngine::numDecks; ++d)
    {
        g.setColour (d == 0 ? Colours::deckA : Colours::deckB);
        g.fillRoundedRectangle ((float) (half.getX() + d * w + (d == 0 ? 0 : 4)), (float) half.getY() + 4.0f, 3.0f, (float) half.getHeight() - 8.0f, 1.5f);
        g.setColour (Colours::textDim);
        g.drawText ("FX " + juce::String (d + 1), half.getX() + d * w + (d == 0 ? 0 : 4) + 8, half.getY(), 38, half.getHeight(), juce::Justification::centredLeft);
    }
}

void FxComponent::resized()
{
    auto area = getLocalBounds().reduced (6, 4);
    const int deckW = area.getWidth() / MixerEngine::numDecks;

    for (int d = 0; d < MixerEngine::numDecks; ++d)
    {
        auto deckArea = area.removeFromLeft (deckW).withTrimmedLeft (d == 0 ? 0 : 4);
        deckArea.removeFromLeft (46);   // "FX 1" / "FX 2" heading, drawn in paint()
        const int slotW = deckArea.getWidth() / fx::EffectRack::numSlots;

        // One row per slot: [on] [effect] [depth] [param], the two small captions sit above the sliders.
        for (int s = 0; s < fx::EffectRack::numSlots; ++s)
        {
            auto& u = ui[(size_t) d][(size_t) s];
            auto slotArea = deckArea.removeFromLeft (slotW).reduced (4, 0);

            u.on.setBounds (slotArea.removeFromLeft (30).withSizeKeepingCentre (30, 26));
            slotArea.removeFromLeft (4);
            const int comboW = juce::jlimit (80, 130, slotArea.getWidth() / 3);
            u.type.setBounds (slotArea.removeFromLeft (comboW).withSizeKeepingCentre (comboW, 26));
            slotArea.removeFromLeft (6);

            auto depthArea = slotArea.removeFromLeft (slotArea.getWidth() / 2).reduced (3, 0);
            u.depthLabel.setBounds (depthArea.removeFromTop (13));
            u.depth.setBounds (depthArea);

            auto paramArea = slotArea.reduced (3, 0);
            u.paramLabel.setBounds (paramArea.removeFromTop (13));
            u.param.setBounds (paramArea);
        }
    }
}

void FxComponent::timerCallback()
{
    for (auto& deck : ui)
        for (auto& u : deck)
        {
            // Show what the controller (or anything else) did; never fight a slider that is being dragged.
            if (u.on.getToggleState() != u.slot->isOn())
                u.on.setToggleState (u.slot->isOn(), juce::dontSendNotification);
            if (u.type.getSelectedId() != u.slot->getType() + 1)
            {
                u.type.setSelectedId (u.slot->getType() + 1, juce::dontSendNotification);
                updateParamLabel (u);
            }
            if (! u.depth.isMouseButtonDown() && std::abs (u.depth.getValue() - u.slot->getDepth()) > 0.002)
                u.depth.setValue (u.slot->getDepth(), juce::dontSendNotification);
            if (! u.param.isMouseButtonDown() && std::abs (u.param.getValue() - u.slot->getParam()) > 0.002)
                u.param.setValue (u.slot->getParam(), juce::dontSendNotification);
        }
}

} // namespace pathboard
