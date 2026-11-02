#include "MixerComponent.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard
{

//==============================================================================
void LevelMeter::setLevel (float linear)
{
    // Fast attack, slow release, with a peak-hold marker.
    level = linear > level ? linear : level * 0.88f;
    if (linear >= peak)
    {
        peak = linear;
        peakHoldFrames = 30;
    }
    else if (--peakHoldFrames <= 0)
    {
        peak *= 0.95f;
    }
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (bounds, 2.0f);

    auto toProportion = [] (float lin)
    {
        const float db = juce::Decibels::gainToDecibels (lin, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 63.0f); // -60 .. +3 dB
    };

    const int segments = 24;
    const float segH = bounds.getHeight() / (float) segments;
    const float lit = toProportion (level) * (float) segments;

    for (int i = 0; i < segments; ++i)
    {
        const float t = (float) i / (float) segments;
        const auto colour = t > 0.92f ? Colours::danger : (t > 0.78f ? Colours::cue : Colours::play);
        const auto seg = juce::Rectangle<float> (bounds.getX() + 1.0f, bounds.getBottom() - (float) (i + 1) * segH + 1.0f,
                                                 bounds.getWidth() - 2.0f, segH - 1.5f);
        g.setColour ((float) i < lit ? colour : colour.withAlpha (0.12f));
        g.fillRect (seg);
    }

    const float peakY = bounds.getBottom() - toProportion (peak) * bounds.getHeight();
    g.setColour (juce::Colours::white);
    g.fillRect (bounds.getX(), peakY, bounds.getWidth(), 2.0f);
}

//==============================================================================
MixerComponent::MixerComponent (AudioEngine& e) : engine (e)
{
    auto& mixer = engine.getMixer();

    for (int d = 0; d < AudioEngine::numDecks; ++d)
    {
        auto& ch = channels[(size_t) d];
        const auto colour = Colours::forDeck (d);
        auto& deck = engine.getDeck (d);

        setupKnob (ch.gain, "Gain (trim)", -12.0, 12.0, 0.0, colour);
        ch.gain.setTextValueSuffix (" dB");
        ch.gain.onValueChange = [&deck, &ch] { deck.setGainTrim (juce::Decibels::decibelsToGain ((float) ch.gain.getValue())); };

        setupKnob (ch.high, "High EQ", -1.0, 1.0, 0.0, colour);
        setupKnob (ch.mid, "Mid EQ", -1.0, 1.0, 0.0, colour);
        setupKnob (ch.low, "Low EQ", -1.0, 1.0, 0.0, colour);
        for (auto* s : { &ch.high, &ch.mid, &ch.low })
            s->onValueChange = [this, d] { updateEq (d); };

        setupKnob (ch.filter, "Filter: left = low-pass, right = high-pass", -1.0, 1.0, 0.0, colour);
        ch.filter.onValueChange = [&deck, &ch] { deck.setFilter ((float) ch.filter.getValue()); };

        ch.fader.setRange (0.0, 1.0);
        ch.fader.setValue (1.0, juce::dontSendNotification);
        ch.fader.setDoubleClickReturnValue (true, 1.0);
        ch.fader.setColour (juce::Slider::rotarySliderFillColourId, colour);
        ch.fader.onValueChange = [&mixer, &ch, d] { mixer.setChannelFader (d, (float) ch.fader.getValue()); };
        addAndMakeVisible (ch.fader);

        ch.cueButton.setClickingTogglesState (true);
        ch.cueButton.setTooltip ("Pre-listen this deck on the headphone output");
        ch.cueButton.setColour (juce::TextButton::buttonOnColourId, Colours::cue);
        ch.cueButton.onClick = [&mixer, &ch, d] { mixer.setHeadphoneCue (d, ch.cueButton.getToggleState()); };
        addAndMakeVisible (ch.cueButton);

        addAndMakeVisible (ch.meter);
    }

    const auto neutral = Colours::text;
    setupKnob (masterVolume, "Master volume", 0.0, 1.0, 0.8, neutral);
    masterVolume.onValueChange = [&mixer, this] { mixer.setMasterVolume ((float) masterVolume.getValue()); };
    setupKnob (boothVolume, "Booth volume (outputs 5/6, if your audio interface has them)", 0.0, 1.0, 0.0, neutral);
    boothVolume.onValueChange = [&mixer, this] { mixer.setBoothVolume ((float) boothVolume.getValue()); };
    setupKnob (headphoneMix, "Headphone mix: left = cue, right = master", 0.0, 1.0, 0.0, Colours::cue);
    headphoneMix.onValueChange = [&mixer, this] { mixer.setHeadphoneMix ((float) headphoneMix.getValue()); };
    setupKnob (headphoneVolume, "Headphone volume (outputs 3/4, if your audio interface has them)", 0.0, 1.0, 0.8, Colours::cue);
    headphoneVolume.onValueChange = [&mixer, this] { mixer.setHeadphoneVolume ((float) headphoneVolume.getValue()); };

    crossfader.setRange (-1.0, 1.0);
    crossfader.setValue (0.0, juce::dontSendNotification);
    crossfader.setDoubleClickReturnValue (true, 0.0);
    crossfader.setColour (juce::Slider::rotarySliderFillColourId, neutral);
    crossfader.onValueChange = [&mixer, this] { mixer.setCrossfader ((float) crossfader.getValue()); };
    addAndMakeVisible (crossfader);

    addAndMakeVisible (masterMeterL);
    addAndMakeVisible (masterMeterR);

    startTimerHz (30);
}

void MixerComponent::setupKnob (juce::Slider& s, const juce::String& tooltip, double min, double max, double def, juce::Colour colour)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    s.setRange (min, max);
    s.setNumDecimalPlacesToDisplay (2);
    s.setValue (def, juce::dontSendNotification);
    s.setDoubleClickReturnValue (true, def);
    s.setTooltip (tooltip + " (double-click to reset)");
    s.setColour (juce::Slider::rotarySliderFillColourId, colour);
    s.setPopupDisplayEnabled (true, true, this);
    addAndMakeVisible (s);
}

void MixerComponent::updateEq (int d)
{
    auto& ch = channels[(size_t) d];
    engine.getDeck (d).setEq ((float) ch.low.getValue(), (float) ch.mid.getValue(), (float) ch.high.getValue());
}

void MixerComponent::timerCallback()
{
    auto& mixer = engine.getMixer();
    for (int d = 0; d < AudioEngine::numDecks; ++d)
        channels[(size_t) d].meter.setLevel (mixer.getDeckLevel (d));
    masterMeterL.setLevel (mixer.getMasterLevel (0));
    masterMeterR.setLevel (mixer.getMasterLevel (1));

    const bool hasHeadphoneOut = mixer.getNumOutputChannels() >= 4;
    for (auto& ch : channels)
        ch.cueButton.setAlpha (hasHeadphoneOut ? 1.0f : 0.6f);
}

//==============================================================================
void MixerComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (Colours::textDim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));

    auto drawCaption = [&g] (const juce::Component& c, const juce::String& text)
    {
        g.drawText (text, c.getBounds().withY (c.getBottom() - 2).withHeight (12), juce::Justification::centred);
    };

    for (auto& ch : channels)
    {
        drawCaption (ch.gain, "GAIN");
        drawCaption (ch.high, "HIGH");
        drawCaption (ch.mid, "MID");
        drawCaption (ch.low, "LOW");
        drawCaption (ch.filter, "FILTER");
    }
    drawCaption (masterVolume, "MASTER");
    drawCaption (boothVolume, "BOOTH");
    drawCaption (headphoneMix, "CUE/MST");
    drawCaption (headphoneVolume, "PHONES");

    if (engine.getMixer().getNumOutputChannels() < 4)
    {
        g.setFont (juce::FontOptions (9.0f));
        g.drawFittedText ("Headphone cue needs a 4+ output device", crossfader.getBounds().translated (0, -16).withHeight (14),
                          juce::Justification::centred, 1);
    }
}

void MixerComponent::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto xfArea = area.removeFromBottom (36);
    crossfader.setBounds (xfArea.reduced (20, 0));
    area.removeFromBottom (18);

    const int colW = area.getWidth() / 3;
    auto left = area.removeFromLeft (colW);
    auto right = area.removeFromRight (colW);
    auto centre = area;

    // Each channel: the five knobs in a column on the outer side, the fader (with level meter and headphone cue)
    // next to it on the side of the centre - so everything stays visible however low the mixer is.
    auto layoutChannel = [] (Channel& ch, juce::Rectangle<int> col, bool knobsOnLeft)
    {
        const int knobColumnWidth = juce::jmax (40, (int) (col.getWidth() * 0.55f));
        auto knobs = knobsOnLeft ? col.removeFromLeft (knobColumnWidth) : col.removeFromRight (knobColumnWidth);

        const int slotH = knobs.getHeight() / 5;
        const int knob = juce::jmin (knobs.getWidth(), slotH - 12, 52);   // leaves room for the label under each knob
        for (auto* s : { &ch.gain, &ch.high, &ch.mid, &ch.low, &ch.filter })
            s->setBounds (knobs.removeFromTop (slotH).withSizeKeepingCentre (knob, knob));

        ch.cueButton.setBounds (col.removeFromTop (26).reduced (2, 0));
        col.removeFromTop (6);
        auto meter = knobsOnLeft ? col.removeFromRight (10) : col.removeFromLeft (10);
        ch.meter.setBounds (meter);
        ch.fader.setBounds (col.reduced (2, 0));
    };

    layoutChannel (channels[0], left, true);
    layoutChannel (channels[1], right, false);

    const int knob = juce::jmin (centre.getWidth(), 48);
    for (auto* s : { &masterVolume, &boothVolume, &headphoneMix, &headphoneVolume })
    {
        s->setBounds (centre.removeFromTop (knob).withSizeKeepingCentre (knob, knob));
        centre.removeFromTop (14);
    }

    auto meters = centre.reduced (0, 4).withSizeKeepingCentre (24, centre.getHeight() - 8);
    masterMeterL.setBounds (meters.removeFromLeft (11));
    masterMeterR.setBounds (meters.removeFromRight (11));
}

} // namespace pathboard
