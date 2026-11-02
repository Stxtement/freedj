#pragma once

#include "Audio/DeckEngine.h"
#include "Decks/ScratchDriver.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

// Draws a deck's waveform from its AudioThumbnail.
//  - Overview: whole track, click/drag to seek.
//  - Scrolling: window centred on the playhead, drag to scratch, wheel to zoom.
// Positions are read from the engine every frame so the view follows actual playback.
class WaveformDisplay : public juce::Component,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    enum class Mode { overview, scrolling };

    WaveformDisplay (DeckEngine& deck, int deckIndex, Mode mode);
    ~WaveformDisplay() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    float timeToX (double seconds, double viewStart, double viewSpan) const;
    void seekFromX (float x);
    void drawMarkers (juce::Graphics&, double viewStart, double viewSpan);

    DeckEngine& deck;
    const int deckIndex;
    const Mode mode;
    double zoomSpanSeconds = 8.0;

    ScratchDriver scratch { deck };
    float lastDragX = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};

} // namespace pathboard
