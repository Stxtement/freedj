#pragma once

#include "Stems/StemSeparator.h"

namespace pathboard::stems
{

// The four separated stems of one track, in RAM, ready for playback. Immutable once published
// (shared between the loader thread and the deck's audio thread through a shared_ptr).
struct StemSet
{
    std::array<juce::AudioBuffer<float>, numStems> audio; // stereo each, same length
    double sampleRate = modelSampleRate;
    Quality quality = Quality::balanced;
    juce::String fingerprint;                            // of the source file the stems were made from

    int getNumSamples() const { return audio[0].getNumSamples(); }
    bool isValid() const { return audio[0].getNumSamples() > 0 && audio[0].getNumChannels() == 2; }
};

} // namespace pathboard::stems
