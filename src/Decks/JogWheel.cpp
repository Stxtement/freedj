#include "JogWheel.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard
{

namespace
{
    float wrapAngleDelta (float delta)
    {
        const auto pi = juce::MathConstants<float>::pi;
        while (delta > pi)  delta -= juce::MathConstants<float>::twoPi;
        while (delta < -pi) delta += juce::MathConstants<float>::twoPi;
        return delta;
    }
}

JogWheel::JogWheel (DeckEngine& d, int index)
    : deck (d), deckIndex (index)
{
    startTimerHz (60);
}

void JogWheel::timerCallback()
{
    // Platter angle follows the real playback position.
    const auto newAngle = (float) std::fmod (deck.getCurrentPositionSeconds() / secondsPerRevolution, 1.0)
                          * juce::MathConstants<float>::twoPi;

    // Bend decays back to zero as soon as the ring stops moving.
    if (std::abs (bendAmount) > 0.0)
    {
        bendAmount *= 0.85;
        if (std::abs (bendAmount) < 0.001)
            bendAmount = 0.0;
        deck.setPitchBend (bendAmount);
    }

    if (std::abs (newAngle - displayAngle) > 0.001f)
    {
        displayAngle = newAngle;
        repaint();
    }
}

float JogWheel::angleAt (juce::Point<float> p) const
{
    const auto c = getLocalBounds().toFloat().getCentre();
    return std::atan2 (p.y - c.y, p.x - c.x);
}

bool JogWheel::isOnOuterRing (juce::Point<float> p) const
{
    const auto bounds = getLocalBounds().toFloat();
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    return p.getDistanceFrom (bounds.getCentre()) > radius * 0.78f;
}

void JogWheel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto area = juce::Rectangle<float> (size, size).withCentre (bounds.getCentre());
    const auto centre = area.getCentre();
    const float radius = size * 0.5f;
    const auto accent = Colours::forDeck (deckIndex);

    // Outer ring
    g.setColour (Colours::panelRaised.darker (0.3f));
    g.fillEllipse (area);
    g.setColour (dragMode == DragMode::bend ? accent : Colours::outline);
    g.drawEllipse (area.reduced (1.0f), 2.0f);

    // Platter
    const auto platter = area.reduced (radius * 0.22f);
    g.setGradientFill (juce::ColourGradient (Colours::panelRaised.brighter (0.2f), platter.getX(), platter.getY(),
                                             juce::Colours::black, platter.getRight(), platter.getBottom(), false));
    g.fillEllipse (platter);

    // Groove rings
    g.setColour (juce::Colours::white.withAlpha (0.04f));
    for (float r = 0.3f; r < 0.95f; r += 0.12f)
        g.drawEllipse (platter.reduced (platter.getWidth() * 0.5f * (1.0f - r)), 1.0f);

    // Position marker
    const float markerR = platter.getWidth() * 0.5f - 6.0f;
    const auto marker = centre.getPointOnCircumference (markerR, displayAngle + juce::MathConstants<float>::halfPi);
    g.setColour (deck.isScratching() ? juce::Colours::white : accent);
    g.drawLine ({ centre.getPointOnCircumference (markerR * 0.35f, displayAngle + juce::MathConstants<float>::halfPi), marker }, 4.0f);

    // Centre label
    const auto label = platter.withSizeKeepingCentre (platter.getWidth() * 0.32f, platter.getHeight() * 0.32f);
    g.setColour (accent.withAlpha (deck.isPlaying() ? 0.9f : 0.4f));
    g.fillEllipse (label);
}

void JogWheel::mouseDown (const juce::MouseEvent& e)
{
    if (! deck.hasTrackLoaded())
        return;

    lastAngle = angleAt (e.position);

    if (isOnOuterRing (e.position) && deck.isPlaying())
    {
        dragMode = DragMode::bend;
    }
    else
    {
        dragMode = DragMode::scratch;
        scratch.begin();
    }
    repaint();
}

void JogWheel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::none)
        return;

    const float angle = angleAt (e.position);
    const double revolutions = (double) wrapAngleDelta (angle - lastAngle) / juce::MathConstants<double>::twoPi;
    lastAngle = angle;

    if (dragMode == DragMode::scratch)
        spin (revolutions);
    else
        bend (revolutions);
}

void JogWheel::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::scratch)
        scratch.end();
    dragMode = DragMode::none;
    repaint();
}

void JogWheel::touch (bool touched)
{
    if (touched && ! scratch.isActive() && deck.hasTrackLoaded())
        scratch.begin();
    else if (! touched)
        scratch.end();
}

void JogWheel::spin (double revolutions)
{
    if (scratch.isActive())
        scratch.move (revolutions * secondsPerRevolution);
}

void JogWheel::bend (double revolutions)
{
    // A quick quarter turn gives roughly +/-10% speed for a moment.
    bendAmount = juce::jlimit (-0.3, 0.3, bendAmount + revolutions * 0.4);
    deck.setPitchBend (bendAmount);
}

} // namespace pathboard
