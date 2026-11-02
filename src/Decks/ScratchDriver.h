#pragma once

#include "Audio/DeckEngine.h"
#include <juce_events/juce_events.h>

namespace pathboard
{

// Converts discrete movement (mouse drags, jog-wheel MIDI ticks) into a continuous
// scratch rate for the deck. Movement is accumulated and turned into
// "track seconds per real second" at ~100 Hz; the engine smooths the steps.
class ScratchDriver : private juce::Timer
{
public:
    explicit ScratchDriver (DeckEngine& d) : deck (d) {}
    ~ScratchDriver() override { stopTimer(); }

    void begin()
    {
        accumulatedSeconds = 0.0;
        lastTickMs = juce::Time::getMillisecondCounterHiRes();
        deck.beginScratch();
        active = true;
        startTimerHz (100);
    }

    void move (double trackSecondsDelta)
    {
        if (active)
            accumulatedSeconds += trackSecondsDelta;
    }

    void end()
    {
        if (! active)
            return;
        stopTimer();
        active = false;
        deck.endScratch();
    }

    bool isActive() const { return active; }

private:
    void timerCallback() override
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double dt = juce::jmax (0.001, (now - lastTickMs) * 0.001);
        lastTickMs = now;

        deck.setScratchRate (accumulatedSeconds / dt);
        accumulatedSeconds = 0.0;
    }

    DeckEngine& deck;
    double accumulatedSeconds = 0.0;
    double lastTickMs = 0.0;
    bool active = false;
};

} // namespace pathboard
