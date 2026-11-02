#include "DJLookAndFeel.h"

namespace pathboard
{

DJLookAndFeel::DJLookAndFeel()
{
    setColourScheme ({ Colours::background, Colours::panel, Colours::panelRaised,
                       Colours::outline, Colours::text, Colours::deckA,
                       juce::Colours::white, Colours::panelRaised, Colours::text });

    setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
    setColour (juce::TextButton::buttonColourId, Colours::panelRaised);
    setColour (juce::TextButton::buttonOnColourId, Colours::deckA);
    setColour (juce::TextButton::textColourOffId, Colours::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::Slider::rotarySliderFillColourId, Colours::deckA);
    setColour (juce::Slider::rotarySliderOutlineColourId, Colours::outline);
    setColour (juce::Slider::thumbColourId, Colours::text);
    setColour (juce::Slider::trackColourId, Colours::outline);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, Colours::text);
    setColour (juce::TreeView::backgroundColourId, Colours::panel);
    setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, Colours::deckA.withAlpha (0.35f));
    setColour (juce::DirectoryContentsDisplayComponent::textColourId, Colours::text);
    setColour (juce::ListBox::backgroundColourId, Colours::panel);
}

void DJLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float pos, float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
    const float arcRadius = radius - 2.0f;

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (Colours::outline);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Bipolar knobs (EQ, filter, pitch) fill from the centre; unipolar ones from the start.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, fromAngle, angle, true);
    g.setColour (accent);
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float knobRadius = radius * 0.68f;
    g.setGradientFill (juce::ColourGradient (Colours::panelRaised.brighter (0.25f), centre.x, centre.y - knobRadius,
                                             Colours::panelRaised.darker (0.4f), centre.x, centre.y + knobRadius, false));
    g.fillEllipse (centre.x - knobRadius, centre.y - knobRadius, knobRadius * 2.0f, knobRadius * 2.0f);

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -knobRadius, 3.0f, knobRadius * 0.55f, 1.5f);
    g.setColour (Colours::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void DJLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPos, float minPos, float maxPos,
                                      juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minPos, maxPos, style, slider);
        return;
    }

    const bool vertical = style == juce::Slider::LinearVertical;
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);

    const auto groove = vertical ? juce::Rectangle<float> (bounds.getCentreX() - 2.0f, bounds.getY(), 4.0f, bounds.getHeight())
                                 : juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - 2.0f, bounds.getWidth(), 4.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.fillRoundedRectangle (groove, 2.0f);

    // Tick marks
    g.setColour (Colours::outline);
    for (int i = 0; i <= 10; ++i)
    {
        const float t = (float) i / 10.0f;
        if (vertical)
        {
            const float ty = bounds.getY() + t * bounds.getHeight();
            g.drawHorizontalLine ((int) ty, bounds.getX() + 2.0f, bounds.getCentreX() - 6.0f);
            g.drawHorizontalLine ((int) ty, bounds.getCentreX() + 6.0f, bounds.getRight() - 2.0f);
        }
        else
        {
            const float tx = bounds.getX() + t * bounds.getWidth();
            g.drawVerticalLine ((int) tx, bounds.getY() + 2.0f, bounds.getCentreY() - 6.0f);
        }
    }

    const auto cap = vertical ? juce::Rectangle<float> (0, 0, juce::jmin (bounds.getWidth() - 4.0f, 34.0f), 16.0f)
                                    .withCentre ({ bounds.getCentreX(), sliderPos })
                              : juce::Rectangle<float> (0, 0, 18.0f, juce::jmin (bounds.getHeight() - 4.0f, 34.0f))
                                    .withCentre ({ sliderPos, bounds.getCentreY() });

    g.setGradientFill (juce::ColourGradient (Colours::panelRaised.brighter (0.4f), cap.getX(), cap.getY(),
                                             Colours::panelRaised.darker (0.3f), cap.getX(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 3.0f);
    g.setColour (accent);
    if (vertical)
        g.fillRect (cap.withSizeKeepingCentre (cap.getWidth() - 6.0f, 2.0f));
    else
        g.fillRect (cap.withSizeKeepingCentre (2.0f, cap.getHeight() - 6.0f));
}

void DJLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                          bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    auto base = backgroundColour;

    if (down)              base = base.brighter (0.25f);
    else if (highlighted)  base = base.brighter (0.1f);
    if (! button.isEnabled()) base = base.withMultipliedAlpha (0.4f);

    g.setColour (base);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (button.getToggleState() ? base.brighter (0.5f) : Colours::outline);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
}

juce::Font DJLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions (juce::jmin (15.0f, (float) buttonHeight * 0.5f), juce::Font::bold));
}

} // namespace pathboard
