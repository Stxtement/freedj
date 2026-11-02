#include "DeckEngine.h"

namespace pathboard
{

namespace
{
    constexpr float eqLowFreq = 250.0f;
    constexpr float eqMidFreq = 1000.0f;
    constexpr float eqHighFreq = 4000.0f;
    constexpr float shelfQ = 0.707f;
    constexpr float midQ = 0.7f;
    constexpr int decodeChunk = 1 << 16;
    constexpr juce::int64 maxTrackSamples = std::numeric_limits<int>::max() - 4;

    float knobToEqGain (float v)
    {
        return juce::Decibels::decibelsToGain (v < 0.0f ? v * 24.0f : v * 6.0f);
    }

    // 4-point Catmull-Rom interpolation, clamped at the buffer edges.
    inline float readInterpolated (const float* data, int length, double position)
    {
        const auto i = (int) std::floor (position);
        const auto f = (float) (position - (double) i);
        auto s = [data, length] (int idx) { return data[juce::jlimit (0, length - 1, idx)]; };

        const float y0 = s (i - 1), y1 = s (i), y2 = s (i + 1), y3 = s (i + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;
    }
}

DeckEngine::DeckEngine (juce::AudioFormatManager& formatManagerToUse)
    : formatManager (formatManagerToUse)
{
}

DeckEngine::~DeckEngine()
{
    ++loadGeneration;
    loaderPool.removeAllJobs (true, 10000);
}

//==============================================================================
void DeckEngine::prepareToPlay (int samplesPerBlockExpected, double sampleRate)
{
    const juce::ScopedLock sl (trackLock);

    deviceSampleRate = sampleRate;
    // ~5 ms time constant for rate changes: click-free start/stop and smooth scratching.
    rateSmoothing = 1.0 - std::exp (-1.0 / (0.005 * sampleRate));

    lowShelf.state  = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sampleRate, eqLowFreq, shelfQ, 1.0f);
    midPeak.state   = juce::dsp::IIR::Coefficients<float>::makePeakFilter (sampleRate, eqMidFreq, midQ, 1.0f);
    highShelf.state = juce::dsp::IIR::Coefficients<float>::makeHighShelf (sampleRate, eqHighFreq, shelfQ, 1.0f);

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlockExpected), 2 };
    lowShelf.prepare (spec);
    midPeak.prepare (spec);
    highShelf.prepare (spec);

    filterLp.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    filterHp.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    filterLp.prepare (spec);
    filterHp.prepare (spec);

    // First coefficient update grows the coefficient storage; do it here so the
    // audio thread never allocates.
    appliedLow = appliedMid = appliedHigh = appliedFilter = 99.0f;
    updateDspCoefficients();
    dspPrepared = true;
}

void DeckEngine::releaseResources()
{
    dspPrepared = false;
}

void DeckEngine::updateDspCoefficients()
{
    const auto low = eqLow.load(), mid = eqMid.load(), high = eqHigh.load(), filt = filterAmount.load();
    using AC = juce::dsp::IIR::ArrayCoefficients<float>;

    if (low != appliedLow)
    {
        *lowShelf.state = AC::makeLowShelf (deviceSampleRate, eqLowFreq, shelfQ, knobToEqGain (low));
        appliedLow = low;
    }
    if (mid != appliedMid)
    {
        *midPeak.state = AC::makePeakFilter (deviceSampleRate, eqMidFreq, midQ, knobToEqGain (mid));
        appliedMid = mid;
    }
    if (high != appliedHigh)
    {
        *highShelf.state = AC::makeHighShelf (deviceSampleRate, eqHighFreq, shelfQ, knobToEqGain (high));
        appliedHigh = high;
    }
    if (filt != appliedFilter)
    {
        const bool wasBypassed = std::abs (appliedFilter) < 0.01f || appliedFilter > 1.5f;
        if (wasBypassed)
        {
            filterLp.reset();
            filterHp.reset();
        }

        // Exponential sweeps: LP 20 kHz -> 80 Hz, HP 20 Hz -> 10 kHz.
        if (filt < 0.0f)
            filterLp.setCutoffFrequency (20000.0f * std::pow (80.0f / 20000.0f, -filt));
        else if (filt > 0.0f)
            filterHp.setCutoffFrequency (20.0f * std::pow (10000.0f / 20.0f, filt));

        appliedFilter = filt;
    }
}

void DeckEngine::renderPlayback (juce::AudioBuffer<float>& out, int startSample, int numSamples)
{
    const auto& audio = track->audio;
    const int length = audio.getNumSamples();
    const float* srcL = audio.getReadPointer (0);
    const float* srcR = audio.getReadPointer (audio.getNumChannels() > 1 ? 1 : 0);
    float* dstL = out.getWritePointer (0, startSample);
    float* dstR = out.getNumChannels() > 1 ? out.getWritePointer (1, startSample) : nullptr;

    const double srcPerDeviceSample = track->sampleRate / deviceSampleRate;
    const bool isScratching = scratching.load();
    const bool isPlayingNow = playing.load();
    const double targetRate = isScratching ? scratchRate.load()
                            : (isPlayingNow ? (syncRateNow > 0.0 ? syncRateNow : 1.0 + pitch.load() + pitchBend.load()) : 0.0);

    const bool looping = loopActive.load();
    const double loopStart = loopStartSamples.load();
    const double loopEnd = loopEndSamples.load();
    const double loopLen = loopEnd - loopStart;

    bool reachedEnd = false;

    // Stems: when any stem is switched off the track is played from the four stems instead of the original.
    // The original is cross-faded out (5 ms) and then not read at all, so no original signal can leak
    // into a muted stem. All stem gains equal to 1 means "play the original".
    const stems::StemSet* stemData = stemSet.get();
    const double stemRatio = stemData != nullptr ? stemData->sampleRate / track->sampleRate : 1.0;
    const int stemLength = stemData != nullptr ? stemData->getNumSamples() : 0;
    const float rampStep = (float) (1.0 / (0.005 * deviceSampleRate));

    for (int i = 0; i < numSamples; ++i)
    {
        currentRate += (targetRate - currentRate) * rateSmoothing;

        // Fade out as the platter slows to a halt so a held sample doesn't click/DC.
        const auto fade = (float) juce::jmin (1.0, std::abs (currentRate) * 8.0);

        // Linear ramps; a value that reaches its target is set to it exactly (a muted stem is exactly 0).
        auto ramp = [rampStep] (float& value, float target)
        {
            if (value < target)      value = std::min (target, value + rampStep);
            else if (value > target) value = std::max (target, value - rampStep);
        };
        // Targets are read per sample, so a change reacts within the 5 ms ramp whatever the buffer size is.
        bool engaged = false;
        for (int s = 0; s < stems::numStems; ++s)
        {
            const float target = stemGainTarget[(size_t) s].load (std::memory_order_relaxed);
            engaged = engaged || target < 0.9999f;
            ramp (stemGainCurrent[(size_t) s], target);
        }
        ramp (stemMix, (stemData != nullptr && engaged) ? 1.0f : 0.0f);

        float outL = 0.0f, outR = 0.0f;

        if (fade > 0.0001f)
        {
            if (stemMix < 1.0f)
            {
                const float w = 1.0f - stemMix;
                outL = readInterpolated (srcL, length, playhead) * w;
                outR = readInterpolated (srcR, length, playhead) * w;
            }

            if (stemMix > 0.0f && stemData != nullptr)
            {
                const double position = playhead * stemRatio;
                float sumL = 0.0f, sumR = 0.0f;
                for (int s = 0; s < stems::numStems; ++s)
                {
                    const float g = stemGainCurrent[(size_t) s];
                    if (g <= 0.0f)
                        continue; // muted stems are not read at all
                    const auto& buffer = stemData->audio[(size_t) s];
                    sumL += g * readInterpolated (buffer.getReadPointer (0), stemLength, position);
                    sumR += g * readInterpolated (buffer.getReadPointer (1), stemLength, position);
                }
                outL += sumL * stemMix;
                outR += sumR * stemMix;
            }
        }

        dstL[i] = outL * fade;
        if (dstR != nullptr)
            dstR[i] = outR * fade;

        playhead += currentRate * srcPerDeviceSample;

        if (looping && loopLen > 1.0 && currentRate > 0.0 && playhead >= loopEnd)
            playhead -= loopLen;

        if (playhead >= (double) (length - 1))
        {
            playhead = (double) (length - 1);
            reachedEnd = true;
        }
        else if (playhead < 0.0)
        {
            playhead = 0.0;
        }
    }

    if (reachedEnd && isPlayingNow && ! isScratching)
        playing = false;

    positionSamples = playhead;
}

// Audio thread. The master's position is taken as it was at the start of this callback, whichever deck runs
// first (a deck that already ran has advanced by one block, which would show up as a constant phase offset).
void DeckEngine::updateSync (unsigned serial)
{
    syncRateNow = 0.0;
    auto* master = syncMaster.load();
    const double ownBpm = nativeBpm.load();
    const double masterBpm = master != nullptr ? master->getEffectiveBpm() : 0.0;

    if (master == nullptr || ownBpm <= 0.0 || masterBpm <= 0.0 || scratching.load())
    {
        syncBaseRate = 0.0;
        syncTrim = 0.0;
        syncPhaseError = 0.0;
        return;
    }

    const double factor = syncFactor.load();
    const double base = factor * masterBpm / ownBpm;
    syncBaseRate = base;
    pitch = juce::jlimit (-0.5, 0.5, base - 1.0);   // if the sync ends, the deck keeps this tempo (no jump)

    const double bend = pitchBend.load();
    double targetTrim = 0.0;
    double error = 0.0;
    if (playing.load() && master->playing.load() && master->nativeBpm.load() > 0.0 && ! master->scratching.load())
    {
        const double masterPos = master->callbackSerial.load() > serial ? master->callbackStartPos.load() : master->positionSamples.load();
        const double masterBeats = (masterPos / master->trackSampleRate.load() - master->gridFirstBeat.load()) * master->nativeBpm.load() / 60.0;
        auto ownBeatsNow = [&] { return (playhead / track->sampleRate - gridFirstBeat.load()) * ownBpm / 60.0; };

        // Requested by SYNC / PLAY: jump to the nearest master beat, to the sample, right here where both
        // positions are known exactly (the phase loop below then only has to hold it).
        if (alignRequested.exchange (false))
        {
            double jump = ownBeatsNow() - factor * masterBeats;
            jump -= std::round (jump);
            playhead = juce::jlimit (0.0, (double) track->audio.getNumSamples() - 1.0, playhead - jump * 60.0 / ownBpm * track->sampleRate);
            callbackStartPos = playhead;
        }

        const double raw = ownBeatsNow() - factor * masterBeats;
        if (std::abs (bend) > 1.0e-9)
        {
            // The player nudges to line the beats up by ear: that position becomes the new target, the sync stays.
            syncOffset = raw - std::round (raw);
        }
        else
        {
            error = raw - syncOffset.load();
            error -= std::round (error);                            // nearest beat, -0.5 .. 0.5
            if (std::abs (error) > 0.004)                           // ~2 ms: close enough, no constant hunting
                targetTrim = juce::jlimit (-0.01, 0.01, -0.12 * error);   // gentle: at most 1 % speed change
        }
    }
    else if (playing.load())
    {
        alignRequested = false;   // the master is not playing: nothing to align to, and no late jump when it starts
    }
    syncPhaseError = error;
    syncTrim += (targetTrim - syncTrim) * 0.2;             // no jumps in speed
    syncRateNow = base * (1.0 + syncTrim) * (1.0 + bend);
}

// Message thread: asks the audio thread to move this deck to the nearest beat of the master (at most half a
// beat) as soon as both decks play, and to keep that phase. Used when SYNC is pressed and when a synced deck starts.
void DeckEngine::alignToMaster()
{
    if (syncMaster.load() == nullptr || ! hasTrackLoaded())
        return;
    syncOffset = 0.0;
    alignRequested = true;
}

void DeckEngine::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    const auto serial = callbackSerial.load();
    const juce::ScopedTryLock sl (trackLock);

    if (! sl.isLocked() || track == nullptr || ! dspPrepared.load())
    {
        syncRateNow = 0.0;
        callbackSerial = serial + 1;
        info.clearActiveBufferRegion();
        return;
    }

    const double seek = pendingSeekSamples.exchange (-1.0);
    if (seek >= 0.0)
        playhead = seek;

    callbackStartPos = playhead;
    updateSync (serial);
    callbackSerial = serial + 1;

    renderPlayback (*info.buffer, info.startSample, info.numSamples);

    info.buffer->applyGain (info.startSample, info.numSamples, gainTrim.load());

    updateDspCoefficients();

    const auto numChannels = (size_t) juce::jmin (2, info.buffer->getNumChannels());
    juce::dsp::AudioBlock<float> block (*info.buffer);
    auto sub = block.getSubBlock ((size_t) info.startSample, (size_t) info.numSamples)
                    .getSubsetChannelBlock (0, numChannels);
    juce::dsp::ProcessContextReplacing<float> ctx (sub);

    lowShelf.process (ctx);
    midPeak.process (ctx);
    highShelf.process (ctx);

    if (appliedFilter < -0.01f)
        filterLp.process (ctx);
    else if (appliedFilter > 0.01f)
        filterHp.process (ctx);
}

//==============================================================================
bool DeckEngine::loadFile (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        const juce::ScopedLock sl (infoLock);
        lastError = "File not found: " + file.getFullPathName();
        return false;
    }

    const int generation = ++loadGeneration;

    clearSync();
    playing = false;
    scratching = false;
    loopActive = false;
    cuePreviewing = false;
    cuePositionSeconds = 0.0;
    hotCues.fill (-1.0);
    nativeBpm = 0.0;
    gridFirstBeat = 0.0;
    gridDownbeat = 0;
    for (auto& g : stemGainTarget)
        g = 1.0f;
    loading = true;
    thumbnail.clear();

    {
        const juce::ScopedLock sl (infoLock);
        lastError.clear();
        loadedFile = file;
    }

    loaderPool.addJob ([this, file, generation] { decodeTrack (file, generation); });
    sendChangeMessage();
    return true;
}

void DeckEngine::decodeTrack (const juce::File& file, int generation)
{
    auto fail = [this, generation] (const juce::String& message)
    {
        if (generation != loadGeneration.load())
            return;
        {
            const juce::ScopedLock sl (infoLock);
            lastError = message;
        }
        loading = false;
        sendChangeMessage();
    };

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
        return fail ("Unsupported or unreadable audio file: " + file.getFileName());

    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > maxTrackSamples || reader->sampleRate <= 0.0)
        return fail ("Invalid audio length: " + file.getFileName());

    auto newTrack = std::make_unique<LoadedTrack>();
    const auto totalSamples = (int) reader->lengthInSamples;

    try
    {
        newTrack->audio.setSize (2, totalSamples);
    }
    catch (const std::bad_alloc&)
    {
        return fail ("Not enough memory to load: " + file.getFileName());
    }

    newTrack->sampleRate = reader->sampleRate;
    newTrack->file = file;

    int decodedSamples = totalSamples;

    for (int pos = 0; pos < totalSamples; pos += decodeChunk)
    {
        if (generation != loadGeneration.load())
            return; // superseded by a newer load

        const int num = juce::jmin (decodeChunk, totalSamples - pos);
        if (! reader->read (&newTrack->audio, pos, num, pos, true, true))
        {
            // Compressed formats only estimate their length; running out of frames
            // early is a normal end of stream. Failing on the first chunk is not.
            if (pos == 0)
                return fail ("Decoding error in: " + file.getFileName());

            decodedSamples = pos + num;
            break;
        }
    }

    if (generation != loadGeneration.load())
        return;

    if (decodedSamples < totalSamples)
    {
        // Trim the zero padding the decoder wrote past the real end of the stream.
        const float* l = newTrack->audio.getReadPointer (0);
        const float* r = newTrack->audio.getReadPointer (1);
        while (decodedSamples > 1 && l[decodedSamples - 1] == 0.0f && r[decodedSamples - 1] == 0.0f)
            --decodedSamples;
        newTrack->audio.setSize (2, decodedSamples, true);
    }

    const int finalLength = newTrack->audio.getNumSamples();
    thumbnail.reset (2, newTrack->sampleRate, finalLength);
    thumbnail.addBlock (0, newTrack->audio, 0, finalLength);

    std::unique_ptr<LoadedTrack> oldTrack;
    std::shared_ptr<const stems::StemSet> oldStems;
    {
        const juce::ScopedLock sl (trackLock);
        oldTrack = std::move (track);
        track = std::move (newTrack);

        // Stems belong to the previous track; the new track starts on its original audio.
        oldStems = std::move (stemSet);
        stemsAvailable = false;
        stemMix = 0.0f;
        stemGainCurrent.fill (1.0f);
        playhead = 0.0;
        currentRate = 0.0;
        pendingSeekSamples = -1.0;
        positionSamples = 0.0;
        trackSampleRate = track->sampleRate;
        lengthSamples = (double) finalLength;
    }

    loading = false;
    sendChangeMessage();
    // oldTrack is freed here, on the loader thread, never on the audio thread.
}

juce::File DeckEngine::getLoadedFile() const
{
    const juce::ScopedLock sl (infoLock);
    return loadedFile;
}

juce::String DeckEngine::getLastError() const
{
    const juce::ScopedLock sl (infoLock);
    return lastError;
}

//==============================================================================
void DeckEngine::play()
{
    if (hasTrackLoaded() && ! loading.load())
    {
        if (! playing.load() && isSynced())
            alignToMaster();   // start on the other deck's beat
        playing = true;
    }
}

void DeckEngine::pause()
{
    playing = false;
}

void DeckEngine::togglePlay()
{
    cuePreviewing = false;
    if (isPlaying()) pause(); else play();
}

void DeckEngine::cueDown()
{
    if (! hasTrackLoaded())
        return;

    if (isPlaying())
    {
        pause();
        setPositionSeconds (cuePositionSeconds);
        return;
    }

    if (std::abs (getCurrentPositionSeconds() - cuePositionSeconds) < 0.02)
    {
        cuePreviewing = true;
        play();
    }
    else
    {
        cuePositionSeconds = getCurrentPositionSeconds();
    }
}

void DeckEngine::cueUp()
{
    if (! cuePreviewing)
        return;

    cuePreviewing = false;
    pause();
    setPositionSeconds (cuePositionSeconds);
}

void DeckEngine::seekRelativeSeconds (double deltaSeconds)
{
    setPositionSeconds (getCurrentPositionSeconds() + deltaSeconds);
}

void DeckEngine::setPositionSeconds (double seconds)
{
    if (! hasTrackLoaded())
        return;

    const double samples = juce::jlimit (0.0, lengthSamples.load() - 1.0, seconds * trackSampleRate.load());
    pendingSeekSamples = samples;
    positionSamples = samples; // so the UI reflects the jump immediately
}

double DeckEngine::getCurrentPositionSeconds() const
{
    const double pending = pendingSeekSamples.load();
    return (pending >= 0.0 ? pending : positionSamples.load()) / trackSampleRate.load();
}

double DeckEngine::getLengthSeconds() const
{
    return lengthSamples.load() / trackSampleRate.load();
}

//==============================================================================
void DeckEngine::beginScratch()
{
    clearSync();
    cuePreviewing = false;
    scratchRate = 0.0;
    scratching = true;
}

void DeckEngine::setScratchRate (double rate)
{
    scratchRate = juce::jlimit (-8.0, 8.0, rate);
}

void DeckEngine::endScratch()
{
    scratching = false;
    scratchRate = 0.0;
}

void DeckEngine::setPitch (double normalisedPercent)
{
    pitch = juce::jlimit (-0.5, 0.5, normalisedPercent);
}

void DeckEngine::setPitchBend (double amount)
{
    pitchBend = juce::jlimit (-0.5, 0.5, amount);
}

void DeckEngine::setSyncMaster (DeckEngine* master, double factor)
{
    if (master == nullptr || master == this)
    {
        clearSync();
        return;
    }
    if (master->syncMaster.load() == this)
        master->clearSync();   // two decks following each other would chase one another
    syncFactor = factor;
    syncOffset = 0.0;
    syncMaster = master;
}

void DeckEngine::clearSync()
{
    syncMaster = nullptr;
}

double DeckEngine::getEffectivePitch() const
{
    const double base = syncBaseRate.load();
    if (syncMaster.load() != nullptr && base > 0.0)
        return base - 1.0;
    return pitch.load();
}

void DeckEngine::setStems (std::shared_ptr<const stems::StemSet> set)
{
    std::shared_ptr<const stems::StemSet> previous;
    {
        const juce::ScopedLock sl (trackLock);
        previous = std::move (stemSet);
        stemSet = std::move (set);
        stemsAvailable = stemSet != nullptr;
        if (stemSet == nullptr)
            stemMix = 0.0f;
    }
    // `previous` (large buffers) is released here, outside the lock and never on the audio thread.
}

void DeckEngine::setStemGains (const std::array<float, stems::numStems>& gains)
{
    for (size_t s = 0; s < gains.size(); ++s)
        stemGainTarget[s] = juce::jlimit (0.0f, 1.0f, gains[s]);
}

void DeckEngine::setGainTrim (float linearGain)
{
    gainTrim = juce::jlimit (0.0f, 4.0f, linearGain);
}

void DeckEngine::setEq (float low, float mid, float high)
{
    eqLow  = juce::jlimit (-1.0f, 1.0f, low);
    eqMid  = juce::jlimit (-1.0f, 1.0f, mid);
    eqHigh = juce::jlimit (-1.0f, 1.0f, high);
}

void DeckEngine::setFilter (float amount)
{
    filterAmount = std::abs (amount) < 0.02f ? 0.0f : juce::jlimit (-1.0f, 1.0f, amount);
}

//==============================================================================
void DeckEngine::setHotCue (int slot)
{
    if (juce::isPositiveAndBelow (slot, numHotCues) && hasTrackLoaded())
        hotCues[(size_t) slot] = getCurrentPositionSeconds();
}

void DeckEngine::jumpToHotCue (int slot)
{
    if (! isHotCueSet (slot))
        return;

    cuePreviewing = false;
    setPositionSeconds (hotCues[(size_t) slot]);
    play();
}

void DeckEngine::clearHotCue (int slot)
{
    if (juce::isPositiveAndBelow (slot, numHotCues))
        hotCues[(size_t) slot] = -1.0;
}

bool DeckEngine::isHotCueSet (int slot) const
{
    return juce::isPositiveAndBelow (slot, numHotCues) && hotCues[(size_t) slot] >= 0.0;
}

double DeckEngine::getHotCueSeconds (int slot) const
{
    return isHotCueSet (slot) ? hotCues[(size_t) slot] : -1.0;
}

void DeckEngine::toggleLoop (double lengthSeconds)
{
    if (loopActive.load())
    {
        exitLoop();
        return;
    }

    if (! hasTrackLoaded() || lengthSeconds <= 0.0)
        return;

    const double sr = trackSampleRate.load();
    const double start = getCurrentPositionSeconds() * sr;
    loopStartSamples = start;
    loopEndSamples = juce::jmin (start + lengthSeconds * sr, lengthSamples.load() - 1.0);
    loopActive = true;
}

void DeckEngine::exitLoop()
{
    loopActive = false;
}

double DeckEngine::getLoopStartSeconds() const { return loopStartSamples.load() / trackSampleRate.load(); }
double DeckEngine::getLoopEndSeconds() const   { return loopEndSamples.load() / trackSampleRate.load(); }

void DeckEngine::setNativeBpm (double bpm)
{
    nativeBpm = juce::jlimit (0.0, 300.0, bpm);
}

double DeckEngine::getFxBeatPosition() const
{
    const double bpm = nativeBpm.load();
    if (! playing.load() || bpm <= 0.0 || ! hasTrackLoaded())
        return -1.0;
    double beats = std::fmod ((callbackStartPos.load() / trackSampleRate.load() - gridFirstBeat.load()) * bpm / 60.0, 4096.0);
    if (beats < 0.0)
        beats += 4096.0;
    return beats;
}

double DeckEngine::getEffectiveBpm() const
{
    return nativeBpm.load() * (1.0 + getEffectivePitch());
}

} // namespace pathboard
