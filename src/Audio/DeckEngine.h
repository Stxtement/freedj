#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "Stems/StemSet.h"
#include <array>

namespace pathboard
{

// One DJ deck. The whole track is decoded into RAM on a background thread and
// played by a variable-rate reader (rate may be negative), which is what makes
// scratching, pitch bend, sample-accurate loops and reverse possible.
//
// Threading: getNextAudioBlock() runs on the audio thread and only try-locks
// trackLock (held briefly when a newly decoded track is swapped in). All other
// methods are for the message thread and communicate through atomics.
// Listeners get a change message when a track finishes loading or fails.
class DeckEngine : public juce::ChangeBroadcaster
{
public:
    explicit DeckEngine (juce::AudioFormatManager& formatManagerToUse);
    ~DeckEngine() override;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate);
    void releaseResources();
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& info);

    // Starts decoding asynchronously; returns false if the file can't be opened.
    bool loadFile (const juce::File& file);
    juce::File getLoadedFile() const;
    bool hasTrackLoaded() const { return lengthSamples.load() > 0.0; }
    bool isLoading() const { return loading.load(); }
    juce::String getLastError() const;

    void play();
    void pause();
    void togglePlay();
    bool isPlaying() const { return playing.load(); }

    // CDJ-style cue: when playing, returns to cue and stops; when stopped on the
    // cue point, previews while held; when stopped elsewhere, sets a new cue.
    void cueDown();
    void cueUp();

    void seekRelativeSeconds (double deltaSeconds);
    void setPositionSeconds (double seconds);
    double getCurrentPositionSeconds() const;
    double getLengthSeconds() const;

    // Scratch: while active, the playback rate follows setScratchRate()
    // (1.0 = normal forward speed, negative = backwards). Play state is kept.
    void beginScratch();
    void setScratchRate (double rate);
    void endScratch();
    bool isScratching() const { return scratching.load(); }

    // Pitch: -0.5 .. 0.5 == -50% .. +50% speed (vinyl-style, also shifts key).
    void setPitch (double normalisedPercent);
    double getPitch() const { return pitch.load(); }
    // Temporary nudge added on top of pitch (jog wheel outer ring).
    void setPitchBend (double amount);

    void setGainTrim (float linearGain);

    // -1..1 per band, 0 = flat, -1 = -24 dB (near kill), +1 = +6 dB.
    void setEq (float low, float mid, float high);
    // -1 (full low-pass) .. 0 (bypass) .. 1 (full high-pass)
    void setFilter (float amount);

    static constexpr int numHotCues = 4;
    void setHotCue (int slot);
    void jumpToHotCue (int slot);
    void clearHotCue (int slot);
    bool isHotCueSet (int slot) const;
    double getHotCueSeconds (int slot) const;
    double getCuePointSeconds() const { return cuePositionSeconds; }

    void toggleLoop (double lengthSeconds);
    void exitLoop();
    bool isLooping() const { return loopActive.load(); }
    double getLoopStartSeconds() const;
    double getLoopEndSeconds() const;

    // Manual BPM (tap tempo) until automatic analysis lands.
    void setNativeBpm (double bpm);
    double getNativeBpm() const { return nativeBpm.load(); }
    double getEffectiveBpm() const;

    // The two tempi, kept apart on purpose:
    //  original = the track's own tempo (library / analysis / tap), independent of the pitch fader
    //  playback = what is audible: original * (1 + pitch)
    double getOriginalBpm() const { return nativeBpm.load(); }
    double getPlaybackBpm() const { return getEffectiveBpm(); }

    // Beat sync: this deck follows `master`'s playback tempo (times `factor`: 1, 0.5 or 2) and a phase loop in the
    // audio thread keeps its beats on the master's beats, so the beat lines of both decks stay together instead
    // of drifting apart. Nudging moves the phase to keep; the sync ends when the pitch is moved by hand, on scratch,
    // on loading a track or clearSync().
    void setSyncMaster (DeckEngine* master, double factor);
    void clearSync();
    void alignToMaster();   // message thread
    bool isSynced() const { return syncMaster.load() != nullptr; }
    // Tempo change against the track's own tempo that is audible now (follows the master while synced).
    double getEffectivePitch() const;
    // Phase distance to the master's beat grid in beats, -0.5..0.5 (0 when not synced / not playing).
    double getSyncPhaseError() const { return syncPhaseError.load(); }

    // Beat position (grid beats, wrapped at 4096) at the start of the latest audio block, for beat-synchronised
    // effects. -1 when the deck does not play or has no tempo.
    double getFxBeatPosition() const;

    // Beat grid in track time: beats at firstBeat + k * 60/nativeBpm; bars start where (k - downbeat) % 4 == 0.
    void setBeatGrid (double firstBeatSeconds, int downbeatIndex) { gridFirstBeat = firstBeatSeconds; gridDownbeat = downbeatIndex; }
    double getGridFirstBeat() const { return gridFirstBeat.load(); }
    int getGridDownbeat() const { return gridDownbeat.load(); }

    // Stems (see Stems/). The set must belong to the loaded track. nullptr removes them.
    void setStems (std::shared_ptr<const stems::StemSet> set);
    bool hasStems() const { return stemsAvailable.load(); }
    // Gain per stem (drums, bass, other, vocals), 0..1. If any gain is below 1 the track plays from its
    // stems (a stem at 0 is digital silence); all at 1 plays the original file.
    void setStemGains (const std::array<float, stems::numStems>& gains);

    juce::AudioThumbnail& getThumbnail() { return thumbnail; }

private:
    struct LoadedTrack
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        juce::File file;
    };

    void decodeTrack (const juce::File& file, int generation);
    void updateDspCoefficients();
    void renderPlayback (juce::AudioBuffer<float>& out, int startSample, int numSamples);

    juce::AudioFormatManager& formatManager;

    juce::CriticalSection trackLock;
    std::unique_ptr<LoadedTrack> track;
    juce::ThreadPool loaderPool { 1 };
    std::atomic<int> loadGeneration { 0 };
    std::atomic<bool> loading { false };
    juce::String lastError;
    juce::File loadedFile;
    mutable juce::CriticalSection infoLock;

    juce::AudioThumbnailCache thumbnailCache { 4 };
    juce::AudioThumbnail thumbnail { 512, formatManager, thumbnailCache };

    // Published by the loader / audio thread.
    std::atomic<double> lengthSamples { 0.0 };
    std::atomic<double> trackSampleRate { 44100.0 };
    std::atomic<double> positionSamples { 0.0 };
    std::atomic<double> pendingSeekSamples { -1.0 };

    std::atomic<bool> playing { false };
    std::atomic<bool> scratching { false };
    std::atomic<double> scratchRate { 0.0 };
    std::atomic<double> pitch { 0.0 };
    std::atomic<double> pitchBend { 0.0 };

    std::shared_ptr<const stems::StemSet> stemSet;   // guarded by trackLock
    std::atomic<bool> stemsAvailable { false };
    std::array<std::atomic<float>, stems::numStems> stemGainTarget { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, stems::numStems> stemGainCurrent { 1.0f, 1.0f, 1.0f, 1.0f };  // audio thread
    float stemMix = 0.0f;                                                           // audio thread: 0 original, 1 stems

    // Audio-thread state.
    double deviceSampleRate = 44100.0;
    double playhead = 0.0;
    double currentRate = 0.0;
    double rateSmoothing = 0.01;

    std::atomic<float> gainTrim { 1.0f };
    std::atomic<float> eqLow { 0.0f }, eqMid { 0.0f }, eqHigh { 0.0f };
    std::atomic<float> filterAmount { 0.0f };

    using StereoIIR = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    StereoIIR lowShelf, midPeak, highShelf;
    juce::dsp::StateVariableTPTFilter<float> filterLp, filterHp;
    std::atomic<bool> dspPrepared { false };
    float appliedLow = 99.0f, appliedMid = 99.0f, appliedHigh = 99.0f, appliedFilter = 99.0f;

    // Message-thread state.
    double cuePositionSeconds = 0.0;
    bool cuePreviewing = false;
    std::array<double, numHotCues> hotCues { -1.0, -1.0, -1.0, -1.0 };

    std::atomic<bool> loopActive { false };
    std::atomic<double> loopStartSamples { 0.0 };
    std::atomic<double> loopEndSamples { 0.0 };

    // Beat sync. Written by the message thread (master, factor) and the audio thread (the rest).
    std::atomic<DeckEngine*> syncMaster { nullptr };
    std::atomic<double> syncFactor { 1.0 };
    std::atomic<double> syncBaseRate { 0.0 };      // playback rate that matches the master, without the phase correction
    std::atomic<double> syncPhaseError { 0.0 };
    std::atomic<bool> alignRequested { false };
    std::atomic<double> syncOffset { 0.0 };        // phase (in beats) to keep against the master; set by aligning / nudging
    std::atomic<double> callbackStartPos { 0.0 };  // playhead at the start of the latest audio callback
    std::atomic<unsigned> callbackSerial { 0 };    // counts audio callbacks; both decks run once per callback
    double syncTrim = 0.0, syncRateNow = 0.0;      // audio thread
    void updateSync (unsigned serial);

    std::atomic<double> nativeBpm { 0.0 }; // 0 == unknown
    std::atomic<double> gridFirstBeat { 0.0 };
    std::atomic<int> gridDownbeat { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckEngine)
};

} // namespace pathboard
