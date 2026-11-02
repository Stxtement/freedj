#include "Effects.h"
#include <cmath>

namespace pathboard::fx
{

namespace
{
    constexpr double pi = 3.14159265358979323846;

    // Beat-synchronised effects need a running beat position. While the deck plays it comes from its grid;
    // otherwise the effect counts on by itself at the current tempo.
    class TimedEffect : public Effect
    {
    protected:
        void begin (const BeatInfo& b, int numSamples)
        {
            bpm = b.bpm > 20.0 ? b.bpm : 120.0;
            step = bpm / 60.0 / sampleRate;
            beat0 = b.beatAtStart >= 0.0 ? b.beatAtStart : freeBeat;
            freeBeat = std::fmod (beat0 + numSamples * step, 4096.0);
        }
        double beatAt (int i) const { return beat0 + i * step; }

        double sampleRate = 44100.0, bpm = 120.0, step = 0.0, beat0 = 0.0, freeBeat = 0.0;
    };

    inline float lerpRead (const std::vector<float>& buf, int size, double readPos)
    {
        while (readPos < 0.0)
            readPos += size;
        const int i0 = (int) readPos % size;
        const int i1 = (i0 + 1) % size;
        const float f = (float) (readPos - std::floor (readPos));
        return buf[(size_t) i0] + (buf[(size_t) i1] - buf[(size_t) i0]) * f;
    }

    //==========================================================================
    // Tempo-synced echo: 1/4, 1/2, 3/4, 1 or 2 beats, darkening repeats.
    class Echo : public TimedEffect
    {
    public:
        void prepare (double sr) override
        {
            sampleRate = sr;
            size = (int) (sr * 4.0) + 8;
            left.assign ((size_t) size, 0.0f);
            right.assign ((size_t) size, 0.0f);
            reset();
        }
        void reset() override
        {
            std::fill (left.begin(), left.end(), 0.0f);
            std::fill (right.begin(), right.end(), 0.0f);
            write = 0;
            lpL = lpR = 0.0f;
            delay = -1.0;
        }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo& b) override
        {
            begin (b, n);
            static constexpr double divisions[] = { 0.25, 0.5, 0.75, 1.0, 2.0 };
            const double beats = divisions[juce::jmin (4, (int) (param * 5.0f))];
            const double target = juce::jlimit (32.0, (double) size - 8.0, beats * 60.0 / bpm * sampleRate);
            if (delay < 0.0)
                delay = target;

            const float damping = 0.32f;
            for (int i = 0; i < n; ++i)
            {
                delay += (target - delay) * 0.0008;   // a tempo change glides instead of clicking
                const double readPos = write - delay;
                const float dl = lerpRead (left, size, readPos);
                const float dr = lerpRead (right, size, readPos);
                lpL += damping * (dl - lpL);
                lpR += damping * (dr - lpR);

                left[(size_t) write] = l[i] + 0.5f * lpL;
                right[(size_t) write] = r[i] + 0.5f * lpR;
                write = (write + 1) % size;

                l[i] += depth * dl;
                r[i] += depth * dr;
            }
        }
    private:
        std::vector<float> left, right;
        int size = 0, write = 0;
        double delay = -1.0;
        float lpL = 0.0f, lpR = 0.0f;
    };

    //==========================================================================
    class ReverbFx : public Effect
    {
    public:
        void prepare (double sr) override { reverb.setSampleRate (sr); }
        void reset() override { reverb.reset(); }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo&) override
        {
            juce::Reverb::Parameters p;
            p.roomSize = 0.3f + 0.65f * param;
            p.damping = 0.45f;
            p.wetLevel = depth * 0.25f;   // juce::Reverb scales wet by 3 and dry by 2
            p.dryLevel = 0.5f;
            p.width = 1.0f;
            p.freezeMode = 0.0f;
            reverb.setParameters (p);
            reverb.processStereo (l, r, n);
        }
    private:
        juce::Reverb reverb;
    };

    //==========================================================================
    // Flanger: short modulated delay with feedback; one sweep every 4..32 beats.
    class Flanger : public TimedEffect
    {
    public:
        void prepare (double sr) override
        {
            sampleRate = sr;
            size = (int) (sr * 0.02) + 8;
            left.assign ((size_t) size, 0.0f);
            right.assign ((size_t) size, 0.0f);
            reset();
        }
        void reset() override
        {
            std::fill (left.begin(), left.end(), 0.0f);
            std::fill (right.begin(), right.end(), 0.0f);
            write = 0;
        }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo& b) override
        {
            begin (b, n);
            const double cycleBeats = 32.0 * std::pow (0.5, param * 3.0);
            for (int i = 0; i < n; ++i)
            {
                const double lfo = 0.5 + 0.5 * std::sin (2.0 * pi * beatAt (i) / cycleBeats);
                const double delaySamples = (0.0004 + lfo * 0.0036) * sampleRate;
                const double readPos = write - delaySamples;
                const float dl = lerpRead (left, size, readPos);
                const float dr = lerpRead (right, size, readPos);

                left[(size_t) write] = l[i] + 0.55f * dl;
                right[(size_t) write] = r[i] + 0.55f * dr;
                write = (write + 1) % size;

                l[i] += depth * 0.9f * dl;
                r[i] += depth * 0.9f * dr;
            }
        }
    private:
        std::vector<float> left, right;
        int size = 0, write = 0;
    };

    //==========================================================================
    // Phaser: six all-pass stages swept between 300 Hz and 2.4 kHz, one sweep every 2..16 beats.
    class Phaser : public TimedEffect
    {
    public:
        void prepare (double sr) override { sampleRate = sr; reset(); }
        void reset() override { for (auto& ch : z) ch.fill (0.0f); last = { 0.0f, 0.0f }; }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo& b) override
        {
            begin (b, n);
            const double cycleBeats = 16.0 * std::pow (0.5, param * 3.0);
            float* data[2] = { l, r };
            for (int i = 0; i < n; ++i)
            {
                const double lfo = 0.5 + 0.5 * std::sin (2.0 * pi * beatAt (i) / cycleBeats);
                const double freq = 300.0 * std::pow (8.0, lfo);
                const double t = std::tan (pi * freq / sampleRate);
                const float a = (float) ((t - 1.0) / (t + 1.0));

                for (int c = 0; c < 2; ++c)
                {
                    const float in = data[c][i];
                    float x = in + 0.5f * last[(size_t) c];
                    for (auto& state : z[(size_t) c])
                    {
                        const float y = a * x + state;
                        state = x - a * y;
                        x = y;
                    }
                    last[(size_t) c] = x;
                    data[c][i] = in * (1.0f - 0.5f * depth) + x * 0.5f * depth;
                }
            }
        }
    private:
        std::array<std::array<float, 6>, 2> z {};
        std::array<float, 2> last { 0.0f, 0.0f };
    };

    //==========================================================================
    // Filter sweep: PARAM below the middle closes a low-pass, above it opens a high-pass (middle = open).
    class FilterFx : public Effect
    {
    public:
        void prepare (double sr) override { sampleRate = sr; reset(); }
        void reset() override { ic1 = {}; ic2 = {}; cutoff = -1.0; }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo&) override
        {
            const bool lowPass = param < 0.5f;
            double target;
            if (lowPass)
                target = 20000.0 * std::pow (80.0 / 20000.0, juce::jlimit (0.0, 1.0, (0.45 - param) / 0.45));
            else
                target = 20.0 * std::pow (400.0, juce::jlimit (0.0, 1.0, (param - 0.55) / 0.45));
            target = juce::jlimit (20.0, 0.45 * sampleRate, target);
            if (cutoff < 0.0)
                cutoff = target;

            constexpr double k = 0.7;   // damping: a little resonance
            float* data[2] = { l, r };
            for (int i = 0; i < n; ++i)
            {
                cutoff += (target - cutoff) * 0.002;
                const double g = std::tan (pi * cutoff / sampleRate);
                const double a1 = 1.0 / (1.0 + g * (g + k));
                const double a2 = g * a1;
                const double a3 = g * a2;

                for (int c = 0; c < 2; ++c)
                {
                    const double x = data[c][i];
                    const double v3 = x - ic2[(size_t) c];
                    const double v1 = a1 * ic1[(size_t) c] + a2 * v3;
                    const double v2 = ic2[(size_t) c] + a2 * ic1[(size_t) c] + a3 * v3;
                    ic1[(size_t) c] = 2.0 * v1 - ic1[(size_t) c];
                    ic2[(size_t) c] = 2.0 * v2 - ic2[(size_t) c];
                    const double filtered = lowPass ? v2 : x - k * v1 - v2;
                    data[c][i] = (float) (x * (1.0 - depth) + filtered * depth);
                }
            }
        }
    private:
        double sampleRate = 44100.0, cutoff = -1.0;
        std::array<double, 2> ic1 {}, ic2 {};
    };

    //==========================================================================
    // Trance gate: the sound is chopped on the beat (1/8, 1/16 or 1/32 notes), DEPTH sets how far it closes.
    class Gate : public TimedEffect
    {
    public:
        void prepare (double sr) override { sampleRate = sr; coef = 1.0f - (float) std::exp (-1.0 / (0.002 * sr)); reset(); }
        void reset() override { gain = 1.0f; }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo& b) override
        {
            begin (b, n);
            static constexpr double periods[] = { 0.5, 0.25, 0.125 };
            const double period = periods[juce::jmin (2, (int) (param * 3.0f))];
            for (int i = 0; i < n; ++i)
            {
                const double phase = beatAt (i) / period;
                const bool open = (phase - std::floor (phase)) < 0.5;
                const float target = open ? 1.0f : 1.0f - depth;
                gain += (target - gain) * coef;
                l[i] *= gain;
                r[i] *= gain;
            }
        }
    private:
        double sampleRate = 44100.0;
        float gain = 1.0f, coef = 0.01f;
    };

    //==========================================================================
    // Bit crusher: fewer bits and a lower sample rate as PARAM goes up.
    class Crush : public Effect
    {
    public:
        void prepare (double) override { reset(); }
        void reset() override { held = { 0.0f, 0.0f }; counter = 0; }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo&) override
        {
            const int bits = juce::jlimit (3, 16, 16 - (int) std::round (param * 12.0f));
            const float levels = (float) (1 << (bits - 1));
            const int hold = 1 + (int) (param * 15.0f);
            float* data[2] = { l, r };
            for (int i = 0; i < n; ++i)
            {
                if (counter == 0)
                    for (int c = 0; c < 2; ++c)
                        held[(size_t) c] = std::round (juce::jlimit (-1.0f, 1.0f, data[c][i]) * levels) / levels;
                counter = (counter + 1) % hold;
                for (int c = 0; c < 2; ++c)
                    data[c][i] = data[c][i] * (1.0f - depth) + held[(size_t) c] * depth;
            }
        }
    private:
        std::array<float, 2> held { 0.0f, 0.0f };
        int counter = 0;
    };

    //==========================================================================
    // Beat roll: takes the next 1/8, 1/4, 1/2 or 1 beat when switched on and repeats it as long as it stays on.
    class Roll : public TimedEffect
    {
    public:
        void prepare (double sr) override
        {
            sampleRate = sr;
            size = (int) (sr * 2.0) + 8;
            left.assign ((size_t) size, 0.0f);
            right.assign ((size_t) size, 0.0f);
            reset();
        }
        void reset() override { started = false; recorded = 0; position = 0; length = 1; }
        void process (float* l, float* r, int n, float depth, float param, const BeatInfo& b) override
        {
            begin (b, n);
            if (! started)
            {
                static constexpr double beats[] = { 0.125, 0.25, 0.5, 1.0 };
                length = juce::jlimit (64, size - 8, (int) std::round (beats[juce::jmin (3, (int) (param * 4.0f))] * 60.0 / bpm * sampleRate));
                recorded = 0;
                position = 0;
                started = true;
            }
            for (int i = 0; i < n; ++i)
            {
                if (recorded < length)
                {
                    left[(size_t) recorded] = l[i];   // still recording: the sound passes unchanged
                    right[(size_t) recorded] = r[i];
                    ++recorded;
                }
                else
                {
                    l[i] = l[i] * (1.0f - depth) + left[(size_t) position] * depth;
                    r[i] = r[i] * (1.0f - depth) + right[(size_t) position] * depth;
                    position = (position + 1) % length;
                }
            }
        }
    private:
        std::vector<float> left, right;
        int size = 0, length = 1, recorded = 0, position = 0;
        bool started = false;
    };
}

//==============================================================================
const char* effectName (int type)
{
    static const char* names[] = { "No effect", "Echo", "Reverb", "Flanger", "Phaser", "Filter", "Trance gate", "Bit crush", "Beat roll" };
    return juce::isPositiveAndBelow (type, (int) numTypes) ? names[type] : "";
}

const char* paramName (int type)
{
    static const char* names[] = { "", "Time", "Size", "Speed", "Speed", "Cutoff", "Rate", "Amount", "Length" };
    return juce::isPositiveAndBelow (type, (int) numTypes) ? names[type] : "";
}

std::unique_ptr<Effect> createEffect (int type)
{
    switch (type)
    {
        case echo:     return std::make_unique<Echo>();
        case reverb:   return std::make_unique<ReverbFx>();
        case flanger:  return std::make_unique<Flanger>();
        case phaser:   return std::make_unique<Phaser>();
        case filter:   return std::make_unique<FilterFx>();
        case gate:     return std::make_unique<Gate>();
        case crush:    return std::make_unique<Crush>();
        case roll:     return std::make_unique<Roll>();
        default:       return nullptr;
    }
}

//==============================================================================
void EffectSlot::prepare (double sr, int maxBlock)
{
    sampleRate = sr;
    for (int t = 0; t < numTypes; ++t)
    {
        effects[(size_t) t] = createEffect (t);
        if (effects[(size_t) t] != nullptr)
            effects[(size_t) t]->prepare (sr);
    }
    scratch.setSize (2, juce::jmax (maxBlock, 512));
    activeType = 0;
    mix = 0.0f;
}

void EffectSlot::process (juce::AudioBuffer<float>& buffer, int numSamples, const BeatInfo& beat)
{
    const int type = typeValue.load();
    const bool wantOn = onValue.load() && type != none && effects[(size_t) type] != nullptr;
    const float target = wantOn ? 1.0f : 0.0f;

    if (target == 0.0f && mix <= 0.0f)
        return;   // idle

    if (type != activeType)
    {
        activeType = type;
        if (effects[(size_t) type] != nullptr)
            effects[(size_t) type]->reset();
    }
    auto* effect = effects[(size_t) activeType].get();
    if (effect == nullptr)
    {
        mix = 0.0f;
        return;
    }
    if (mix <= 0.0f)
        effect->reset();   // a fresh start every time the effect is switched on

    const float depth = depthValue.load(), param = paramValue.load();
    const float step = (float) (1.0 / (0.02 * sampleRate));
    const int capacity = scratch.getNumSamples();
    if (capacity <= 0)
        return;   // not prepared yet
    float* dryL = buffer.getWritePointer (0);
    float* dryR = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : dryL;

    for (int offset = 0; offset < numSamples; offset += capacity)
    {
        const int n = juce::jmin (capacity, numSamples - offset);
        float* wetL = scratch.getWritePointer (0);
        float* wetR = scratch.getWritePointer (1);
        juce::FloatVectorOperations::copy (wetL, dryL + offset, n);
        juce::FloatVectorOperations::copy (wetR, dryR + offset, n);

        BeatInfo chunk = beat;
        if (chunk.beatAtStart >= 0.0)
            chunk.beatAtStart = std::fmod (beat.beatAtStart + offset * (beat.bpm / 60.0 / sampleRate), 4096.0);
        effect->process (wetL, wetR, n, depth, param, chunk);

        for (int i = 0; i < n; ++i)
        {
            mix = target > mix ? juce::jmin (target, mix + step) : juce::jmax (target, mix - step);
            dryL[offset + i] += (wetL[i] - dryL[offset + i]) * mix;
            if (dryR != dryL)
                dryR[offset + i] += (wetR[i] - dryR[offset + i]) * mix;
        }
    }
}

//==============================================================================
void EffectRack::prepare (double sampleRate, int maxBlock)
{
    for (auto& s : slots)
        s.prepare (sampleRate, maxBlock);
}

void EffectRack::process (juce::AudioBuffer<float>& buffer, int numSamples, const BeatInfo& beat)
{
    bool any = false;
    for (auto& s : slots)
        if (s.isOn() || s.getType() != none)
            any = true;
    if (! any)
        return;

    for (auto& s : slots)
        s.process (buffer, numSamples, beat);

    // Safety: an effect must never send NaN, infinity or a huge level to the speakers.
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        float* p = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            p[i] = std::isfinite (p[i]) ? juce::jlimit (-4.0f, 4.0f, p[i]) : 0.0f;
    }
}

} // namespace pathboard::fx
