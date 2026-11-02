#include "MixerEngine.h"

namespace pathboard
{

namespace
{
    // Dip-free curve: both decks at full level in the centre, smooth cosine fade at the edges.
    float crossfadeGain (float t)
    {
        return t <= 0.5f ? 1.0f : std::cos ((t - 0.5f) * juce::MathConstants<float>::pi);
    }

    void writeOrClear (float* dest, const juce::AudioBuffer<float>& src, int srcChannel, int numSamples, float gain)
    {
        if (dest == nullptr)
            return;

        if (gain <= 0.0f)
        {
            juce::FloatVectorOperations::clear (dest, numSamples);
            return;
        }

        juce::FloatVectorOperations::copyWithMultiply (dest, src.getReadPointer (srcChannel), gain, numSamples);
        juce::FloatVectorOperations::clip (dest, dest, -1.0f, 1.0f, numSamples);
    }
}

MixerEngine::MixerEngine (DeckEngine& deckA, DeckEngine& deckB)
    : decks { &deckA, &deckB }
{
    for (int d = 0; d < numDecks; ++d)
    {
        channelFaders[(size_t) d] = 1.0f;
        headphoneCue[(size_t) d] = false;
        deckLevels[(size_t) d] = 0.0f;
    }
    masterLevels[0] = masterLevels[1] = 0.0f;
}

void MixerEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const int blockSize = device->getCurrentBufferSizeSamples();
    const double sampleRate = device->getCurrentSampleRate();

    // Preallocate generously: some drivers deliver blocks larger than they announce,
    // and the callback must never allocate.
    const int capacity = juce::jmax (blockSize * 2, 8192);
    for (auto* buffer : { &deckBuffers[0], &deckBuffers[1], &masterBuffer, &cueBuffer })
    {
        buffer->setSize (2, capacity);
        buffer->setSize (2, blockSize, false, false, true);
    }

    numActiveOutputs = device->getActiveOutputChannels().countNumberOfSetBits();

    for (auto* deck : decks)
        deck->prepareToPlay (blockSize, sampleRate);
}

void MixerEngine::audioDeviceStopped()
{
    for (auto* deck : decks)
        deck->releaseResources();
}

void MixerEngine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    juce::ScopedNoDenormals noDenormals;

    masterBuffer.setSize (2, numSamples, false, false, true);
    cueBuffer.setSize (2, numSamples, false, false, true);
    masterBuffer.clear();
    cueBuffer.clear();

    const float t = (crossfader.load() + 1.0f) * 0.5f;
    const std::array<float, numDecks> xfGains { crossfadeGain (t), crossfadeGain (1.0f - t) };

    for (size_t d = 0; d < (size_t) numDecks; ++d)
    {
        auto& buffer = deckBuffers[d];
        buffer.setSize (2, numSamples, false, false, true);
        decks[d]->getNextAudioBlock (juce::AudioSourceChannelInfo (&buffer, 0, numSamples));

        const float fader = channelFaders[d].load();
        const float gain = fader * fader * xfGains[d]; // squared fader ~ perceptually linear

        deckLevels[d] = buffer.getMagnitude (0, numSamples) * fader * fader;

        if (gain > 0.0f)
            for (int ch = 0; ch < 2; ++ch)
                masterBuffer.addFrom (ch, 0, buffer, ch, 0, numSamples, gain);

        if (headphoneCue[d].load())
            for (int ch = 0; ch < 2; ++ch)
                cueBuffer.addFrom (ch, 0, buffer, ch, 0, numSamples);
    }

    const float master = masterVolume.load();
    for (int ch = 0; ch < 2; ++ch)
        masterLevels[(size_t) ch] = masterBuffer.getMagnitude (ch, 0, numSamples) * master;

    // Headphones: blend of cue bus and master bus.
    const float hpMix = headphoneMix.load();
    const float hpVol = headphoneVolume.load();
    for (int ch = 0; ch < 2; ++ch)
    {
        cueBuffer.applyGain (ch, 0, numSamples, 1.0f - hpMix);
        cueBuffer.addFrom (ch, 0, masterBuffer, ch, 0, numSamples, hpMix);
    }

    for (int out = 0; out < numOutputChannels; ++out)
    {
        auto* dest = outputChannelData[out];
        const int srcCh = out % 2;

        if (out < 2)       writeOrClear (dest, masterBuffer, srcCh, numSamples, master);
        else if (out < 4)  writeOrClear (dest, cueBuffer, srcCh, numSamples, hpVol);
        else if (out < 6)  writeOrClear (dest, masterBuffer, srcCh, numSamples, boothVolume.load());
        else if (dest != nullptr) juce::FloatVectorOperations::clear (dest, numSamples);
    }
}

void MixerEngine::setCrossfader (float position)        { crossfader = juce::jlimit (-1.0f, 1.0f, position); }
void MixerEngine::setMasterVolume (float value)         { masterVolume = juce::jlimit (0.0f, 1.0f, value); }
void MixerEngine::setBoothVolume (float value)          { boothVolume = juce::jlimit (0.0f, 1.0f, value); }
void MixerEngine::setHeadphoneMix (float cueToMaster)   { headphoneMix = juce::jlimit (0.0f, 1.0f, cueToMaster); }
void MixerEngine::setHeadphoneVolume (float value)      { headphoneVolume = juce::jlimit (0.0f, 1.0f, value); }

void MixerEngine::setChannelFader (int deck, float value)
{
    if (juce::isPositiveAndBelow (deck, numDecks))
        channelFaders[(size_t) deck] = juce::jlimit (0.0f, 1.0f, value);
}

void MixerEngine::setHeadphoneCue (int deck, bool enabled)
{
    if (juce::isPositiveAndBelow (deck, numDecks))
        headphoneCue[(size_t) deck] = enabled;
}

bool MixerEngine::isHeadphoneCueEnabled (int deck) const
{
    return juce::isPositiveAndBelow (deck, numDecks) && headphoneCue[(size_t) deck].load();
}

float MixerEngine::getDeckLevel (int deck) const
{
    return juce::isPositiveAndBelow (deck, numDecks) ? deckLevels[(size_t) deck].load() : 0.0f;
}

float MixerEngine::getMasterLevel (int channel) const
{
    return juce::isPositiveAndBelow (channel, 2) ? masterLevels[(size_t) channel].load() : 0.0f;
}

} // namespace pathboard
