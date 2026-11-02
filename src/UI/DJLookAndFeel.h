#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

namespace Colours
{
    const juce::Colour background   { 0xff0e1014 };
    const juce::Colour panel        { 0xff171a20 };
    const juce::Colour panelRaised  { 0xff20242c };
    const juce::Colour outline      { 0xff2c313b };
    const juce::Colour text         { 0xffe4e7ec };
    const juce::Colour textDim      { 0xff8a919e };
    const juce::Colour deckA        { 0xff1fb5ff };
    const juce::Colour deckB        { 0xffff7a1a };
    const juce::Colour cue          { 0xffffc21a };
    const juce::Colour play         { 0xff32d06c };
    const juce::Colour danger       { 0xffff3b4a };

    const juce::Colour matchVeryGood { 0xff2ecc71 };   // Smart Matching: very good fit
    const juce::Colour matchGood     { 0xff3b82f6 };   // Smart Matching: good fit

    inline juce::Colour forDeck (int deck) { return deck == 0 ? deckA : deckB; }
}

class DJLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DJLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};

} // namespace pathboard
