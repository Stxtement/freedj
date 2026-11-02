// Tests for: the effect engine (every effect is finite and bounded, bypass is exact, depth 0 is transparent,
// echo / gate / roll behave as described, NaN protection).

#include "TestSupport.h"
#include "Effects/Effects.h"
#include <cmath>
#include <iostream>

using namespace pathboard;
using testsupport::check;

namespace
{
constexpr double sampleRate = 48000.0;

juce::AudioBuffer<float> makeSignal (int numSamples)
{
    juce::AudioBuffer<float> b (2, numSamples);
    juce::Random random (42);
    for (int i = 0; i < numSamples; ++i)
    {
        const float tone = 0.3f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) sampleRate);
        b.setSample (0, i, tone + 0.2f * (random.nextFloat() * 2.0f - 1.0f));
        b.setSample (1, i, tone - 0.2f * (random.nextFloat() * 2.0f - 1.0f));
    }
    return b;
}

// Runs a slot over the whole signal in blocks of `block` samples.
juce::AudioBuffer<float> run (fx::EffectSlot& slot, juce::AudioBuffer<float> signal, int block, double bpm, double beatAtStart)
{
    const int total = signal.getNumSamples();
    for (int start = 0; start < total; start += block)
    {
        const int n = juce::jmin (block, total - start);
        juce::AudioBuffer<float> view (signal.getArrayOfWritePointers(), 2, start, n);
        fx::BeatInfo info;
        info.bpm = bpm;
        info.beatAtStart = beatAtStart < 0.0 ? -1.0 : beatAtStart + start * bpm / 60.0 / sampleRate;
        slot.process (view, n, info);
    }
    return signal;
}

float maxAbsDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from, int to)
{
    float worst = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = from; i < to; ++i)
            worst = juce::jmax (worst, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    return worst;
}
}

int runEffectTests()
{
    std::cout << "Effects" << std::endl;

    const int total = (int) (sampleRate * 2.0);
    const auto input = makeSignal (total);

    // Every effect, at several settings: finite, bounded, and it does something.
    for (int type = fx::echo; type < fx::numTypes; ++type)
    {
        bool finite = true, bounded = true, changed = false;
        for (float param : { 0.0f, 0.3f, 0.7f, 1.0f })
        {
            fx::EffectSlot slot;
            slot.prepare (sampleRate, 480);
            slot.setType (type);
            slot.setDepth (1.0f);
            slot.setParam (param);
            slot.setOn (true);
            const auto out = run (slot, input, 480, 126.0, 0.0);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < total; ++i)
                {
                    const float v = out.getSample (ch, i);
                    finite = finite && std::isfinite (v);
                    bounded = bounded && std::abs (v) < 6.0f;
                }
            changed = changed || maxAbsDifference (out, input, 4000, total) > 0.01f;
        }
        const juce::String name = fx::effectName (type);
        check (finite && bounded, name + ": output is finite and bounded");
        check (changed, name + ": changes the sound");
    }

    // Off = untouched, bit for bit.
    {
        fx::EffectSlot slot;
        slot.prepare (sampleRate, 480);
        slot.setType (fx::echo);
        slot.setOn (false);
        check (maxAbsDifference (run (slot, input, 480, 120.0, -1.0), input, 0, total) == 0.0f, "an effect that is off leaves the signal untouched");
    }

    // Depth 0 is transparent for every effect (once the switch-on fade is over).
    for (int type = fx::echo; type < fx::numTypes; ++type)
    {
        fx::EffectSlot slot;
        slot.prepare (sampleRate, 480);
        slot.setType (type);
        slot.setDepth (0.0f);
        slot.setParam (0.6f);
        slot.setOn (true);
        const auto out = run (slot, input, 480, 120.0, 0.0);
        check (maxAbsDifference (out, input, 3000, total) < 1.0e-3f, juce::String (fx::effectName (type)) + ": depth 0 is transparent",
               juce::String (maxAbsDifference (out, input, 3000, total)));
    }

    // Switching off ends in an exact bypass again, without a hard cut in between.
    {
        fx::EffectSlot slot;
        slot.prepare (sampleRate, 480);
        slot.setType (fx::filter);
        slot.setDepth (1.0f);
        slot.setParam (0.1f);
        slot.setOn (true);
        auto first = run (slot, makeSignal (24000), 480, 120.0, -1.0);
        slot.setOn (false);
        auto second = run (slot, input, 480, 120.0, -1.0);
        check (maxAbsDifference (second, input, 2000, total) == 0.0f, "after switching off the signal is untouched again");
        float step = 0.0f;
        for (int i = 1; i < 960; ++i)
            step = juce::jmax (step, std::abs (second.getSample (0, i) - second.getSample (0, i - 1)));
        check (step < 1.0f, "switching off is smooth (no click)", juce::String (step));
    }

    // Echo: an impulse comes back after exactly half a beat (126 -> 120 bpm: 0.25 s = 12000 samples).
    {
        juce::AudioBuffer<float> impulse (2, 24000);
        impulse.clear();
        impulse.setSample (0, 0, 1.0f);
        impulse.setSample (1, 0, 1.0f);
        fx::EffectSlot slot;
        slot.prepare (sampleRate, 480);
        slot.setType (fx::echo);
        slot.setDepth (0.5f);
        slot.setParam (0.25f);   // second division: 1/2 beat
        slot.setOn (true);
        const auto out = run (slot, impulse, 480, 120.0, -1.0);
        check (std::abs (out.getSample (0, 12000) - 0.5f) < 0.02f, "echo: the repeat arrives after half a beat at the right level",
               juce::String (out.getSample (0, 12000)));
        check (std::abs (out.getSample (0, 11000)) < 0.01f && std::abs (out.getSample (0, 13000)) < 0.01f, "echo: nothing before or after the repeat");
    }

    // Trance gate: open in the first half of each 1/8 note, closed in the second; follows the beat position.
    {
        juce::AudioBuffer<float> dc (2, 24000);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 24000; ++i)
                dc.setSample (ch, i, 1.0f);

        fx::EffectSlot slot;
        slot.prepare (sampleRate, 480);
        slot.setType (fx::gate);
        slot.setDepth (1.0f);
        slot.setParam (0.0f);   // 1/8 note = half a beat = 12000 samples at 120 bpm
        slot.setOn (true);
        const auto aligned = run (slot, dc, 480, 120.0, 0.0);
        check (aligned.getSample (0, 3000) > 0.95f && aligned.getSample (0, 9000) < 0.05f, "gate: open then closed within the note");

        fx::EffectSlot shifted;
        shifted.prepare (sampleRate, 480);
        shifted.setType (fx::gate);
        shifted.setDepth (1.0f);
        shifted.setParam (0.0f);
        shifted.setOn (true);
        const auto late = run (shifted, dc, 480, 120.0, 0.25);   // a quarter beat later: starts in the closed half
        check (late.getSample (0, 2000) < 0.05f, "gate: follows the deck's beat position");
    }

    // Beat roll: after one beat has been recorded it repeats.
    {
        const auto source = makeSignal (72000);
        fx::EffectSlot slot;
        slot.prepare (sampleRate, 480);
        slot.setType (fx::roll);
        slot.setDepth (1.0f);
        slot.setParam (0.9f);   // one beat = 24000 samples at 120 bpm
        slot.setOn (true);
        const auto out = run (slot, source, 480, 120.0, -1.0);
        check (std::abs (out.getSample (0, 24000 + 5000) - source.getSample (0, 5000)) < 1.0e-5f
                   && std::abs (out.getSample (0, 48000 + 7000) - source.getSample (0, 7000)) < 1.0e-5f,
               "roll: the recorded beat repeats");
    }

    // Protection: a NaN or a huge value never reaches the output.
    {
        juce::AudioBuffer<float> bad (2, 480);
        bad.clear();
        bad.setSample (0, 5, std::nanf (""));
        bad.setSample (1, 9, 1.0e9f);
        fx::EffectRack rack;
        rack.prepare (sampleRate, 480);
        rack.process (bad, 480, {});
        bool ok = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 480; ++i)
                ok = ok && std::isfinite (bad.getSample (ch, i)) && std::abs (bad.getSample (ch, i)) <= 4.0f;
        check (ok, "rack: NaN and huge values are removed from the output");
    }

    // Different block sizes give the same result (the effect must not depend on how the driver slices the audio).
    {
        fx::EffectSlot a, b;
        for (auto* s : { &a, &b })
        {
            s->prepare (sampleRate, 1024);
            s->setType (fx::flanger);
            s->setDepth (0.8f);
            s->setParam (0.4f);
            s->setOn (true);
        }
        const auto small = run (a, input, 128, 124.0, 1.0);
        const auto large = run (b, input, 1024, 124.0, 1.0);
        check (maxAbsDifference (small, large, 0, total) < 2.0e-3f, "flanger: same result for different block sizes",
               juce::String (maxAbsDifference (small, large, 0, total)));
    }

    return 0;
}
