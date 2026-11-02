#include "WaveformDisplay.h"
#include "UI/DJLookAndFeel.h"

namespace pathboard
{

WaveformDisplay::WaveformDisplay (DeckEngine& d, int index, Mode m)
    : deck (d), deckIndex (index), mode (m)
{
    deck.getThumbnail().addChangeListener (this);
    deck.addChangeListener (this);
    setOpaque (true);
    startTimerHz (60);
}

WaveformDisplay::~WaveformDisplay()
{
    deck.getThumbnail().removeChangeListener (this);
    deck.removeChangeListener (this);
}

void WaveformDisplay::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

void WaveformDisplay::timerCallback()
{
    if (deck.hasTrackLoaded())
        repaint();
}

float WaveformDisplay::timeToX (double seconds, double viewStart, double viewSpan) const
{
    return (float) ((seconds - viewStart) / viewSpan * (double) getWidth());
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    const auto accent = Colours::forDeck (deckIndex);
    g.fillAll (Colours::panel.darker (0.35f));

    auto& thumb = deck.getThumbnail();
    const double length = thumb.getTotalLength();

    if (deck.isLoading() || length <= 0.0)
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (14.0f));
        const auto error = deck.getLastError();
        g.drawFittedText (deck.isLoading() ? "Loading..."
                          : error.isNotEmpty() ? error
                          : (mode == Mode::scrolling ? "Drop a track here or load one from the browser" : ""),
                          bounds, juce::Justification::centred, 2);
        return;
    }

    const double position = deck.getCurrentPositionSeconds();
    const double viewSpan = mode == Mode::overview ? length : zoomSpanSeconds;
    const double viewStart = mode == Mode::overview ? 0.0 : position - viewSpan * 0.5;

    const double drawStart = juce::jmax (0.0, viewStart);
    const double drawEnd = juce::jmin (length, viewStart + viewSpan);

    if (drawEnd > drawStart)
    {
        const float x0 = timeToX (drawStart, viewStart, viewSpan);
        const float x1 = timeToX (drawEnd, viewStart, viewSpan);
        const auto area = juce::Rectangle<float> (x0, 0.0f, x1 - x0, (float) getHeight()).toNearestInt();

        g.setColour (accent.withAlpha (0.9f));
        thumb.drawChannels (g, area, drawStart, drawEnd, 1.0f);
    }

    drawMarkers (g, viewStart, viewSpan);

    if (mode == Mode::overview)
    {
        const float px = timeToX (position, viewStart, viewSpan);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (0.0f, 0.0f, px, (float) getHeight());
        g.setColour (juce::Colours::white);
        g.fillRect (px - 1.0f, 0.0f, 2.0f, (float) getHeight());
    }
    else
    {
        const float cx = (float) getWidth() * 0.5f;
        g.setColour (juce::Colours::white);
        g.fillRect (cx - 1.0f, 0.0f, 2.0f, (float) getHeight());

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (zoomSpanSeconds, 1) + " s", bounds.reduced (4), juce::Justification::topRight);
    }

    g.setColour (Colours::outline);
    g.drawRect (bounds);
}

void WaveformDisplay::drawMarkers (juce::Graphics& g, double viewStart, double viewSpan)
{
    const float h = (float) getHeight();

    // Beat grid (scrolling view only; in the whole-track overview the lines would merge into a wall).
    const double bpm = deck.getNativeBpm();
    if (mode == Mode::scrolling && bpm > 0.0)
    {
        const double period = 60.0 / bpm;
        const double first = deck.getGridFirstBeat();
        const int downbeat = deck.getGridDownbeat();
        const int k0 = (int) std::floor ((viewStart - first) / period);
        const int k1 = (int) std::ceil ((viewStart + viewSpan - first) / period);

        if (k1 - k0 < 400)
            for (int k = k0; k <= k1; ++k)
            {
                const float x = timeToX (first + k * period, viewStart, viewSpan);
                const bool barStart = (((k - downbeat) % 4) + 4) % 4 == 0;
                g.setColour (barStart ? juce::Colours::white.withAlpha (0.85f) : juce::Colours::white.withAlpha (0.28f));
                g.fillRect (x - (barStart ? 1.0f : 0.5f), barStart ? 0.0f : h * 0.7f, barStart ? 2.0f : 1.0f, barStart ? h : h * 0.3f);
            }
    }

    if (deck.isLooping())
    {
        const float lx0 = timeToX (deck.getLoopStartSeconds(), viewStart, viewSpan);
        const float lx1 = timeToX (deck.getLoopEndSeconds(), viewStart, viewSpan);
        g.setColour (Colours::play.withAlpha (0.18f));
        g.fillRect (lx0, 0.0f, juce::jmax (1.0f, lx1 - lx0), h);
        g.setColour (Colours::play);
        g.drawVerticalLine ((int) lx0, 0.0f, h);
        g.drawVerticalLine ((int) lx1, 0.0f, h);
    }

    const float cueX = timeToX (deck.getCuePointSeconds(), viewStart, viewSpan);
    g.setColour (Colours::cue);
    g.fillRect (cueX - 1.0f, 0.0f, 2.0f, h);
    juce::Path tri;
    tri.addTriangle (cueX - 5.0f, 0.0f, cueX + 5.0f, 0.0f, cueX, 7.0f);
    g.fillPath (tri);

    for (int i = 0; i < DeckEngine::numHotCues; ++i)
    {
        if (! deck.isHotCueSet (i))
            continue;

        const float x = timeToX (deck.getHotCueSeconds (i), viewStart, viewSpan);
        const auto colour = juce::Colour::fromHSV ((float) i / (float) DeckEngine::numHotCues, 0.75f, 1.0f, 1.0f);
        g.setColour (colour);
        g.drawVerticalLine ((int) x, 0.0f, h);
        auto label = juce::Rectangle<float> (x, h - 14.0f, 14.0f, 14.0f);
        g.fillRect (label);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (juce::String (i + 1), label, juce::Justification::centred);
    }
}

void WaveformDisplay::seekFromX (float x)
{
    const double length = deck.getLengthSeconds();
    if (length > 0.0 && getWidth() > 0)
        deck.setPositionSeconds (juce::jlimit (0.0, 1.0, (double) x / (double) getWidth()) * length);
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (! deck.hasTrackLoaded())
        return;

    if (mode == Mode::overview)
    {
        seekFromX (e.position.x);
        return;
    }

    lastDragX = e.position.x;
    scratch.begin();
}

void WaveformDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (mode == Mode::overview)
    {
        seekFromX (e.position.x);
        return;
    }

    if (! scratch.isActive() || getWidth() <= 0)
        return;

    // Dragging the waveform left moves forward through the track, like pushing a record.
    const float dx = e.position.x - lastDragX;
    lastDragX = e.position.x;
    scratch.move (-(double) dx / (double) getWidth() * zoomSpanSeconds);
}

void WaveformDisplay::mouseUp (const juce::MouseEvent&)
{
    scratch.end();
}

void WaveformDisplay::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (mode != Mode::scrolling)
        return;

    zoomSpanSeconds = juce::jlimit (1.0, 60.0, zoomSpanSeconds * (wheel.deltaY > 0 ? 0.8 : 1.25));
    repaint();
}

} // namespace pathboard
