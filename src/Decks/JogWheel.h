#pragma once

#include "Audio/DeckEngine.h"
#include "Decks/ScratchDriver.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace pathboard
{

// Virtual platter. Inner area: scratch (touch = hold the record).
// Outer ring: pitch bend / nudge while playing.
// One full turn == one rotation of a 33 1/3 RPM record (1.8 s of audio).
class JogWheel : public juce::Component, private juce::Timer
{
public:
    static constexpr double secondsPerRevolution = 60.0 / (100.0 / 3.0);

    JogWheel (DeckEngine& deck, int deckIndex);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // External jog input (e.g. MIDI controller) - delta in revolutions.
    void touch (bool touched);
    void spin (double revolutions);
    void bend (double revolutions);

private:
    void timerCallback() override;
    float angleAt (juce::Point<float> p) const;
    bool isOnOuterRing (juce::Point<float> p) const;

    DeckEngine& deck;
    const int deckIndex;
    ScratchDriver scratch { deck };

    enum class DragMode { none, scratch, bend };
    DragMode dragMode = DragMode::none;
    float lastAngle = 0.0f;
    double bendAmount = 0.0;
    float displayAngle = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JogWheel)
};

} // namespace pathboard
