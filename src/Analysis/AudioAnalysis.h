#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

namespace pathboard::analysis
{

// Bump when an algorithm change makes old results obsolete; tracks with an older version are re-analysed.
constexpr int analysisVersion = 1;

struct TempoResult
{
    double bpm = 0.0;               // 0 = no reliable tempo found
    double confidence = 0.0;        // 0..1
    double firstBeatSeconds = 0.0;  // time of a beat on the grid, 0 <= x < one beat period
    int downbeatIndex = 0;          // which of 4 consecutive grid beats is the bar start (0..3), see downbeatConfidence
    double downbeatConfidence = 0.0;
    bool variableTempo = false;     // tempo drifts or changes within the track
    double alternativeBpm = 0.0;    // the second most likely reading (e.g. half/double time), 0 = none
};

struct KeyResult
{
    int tonic = -1;                 // 0 = C ... 11 = B, -1 = unknown
    bool minor = false;
    juce::String camelot;           // e.g. "8A" (A = minor, B = major)
    juce::String name;              // e.g. "Am", "C", "F#m"
    double confidence = 0.0;        // 0..1
};

struct TrackAnalysis
{
    TempoResult tempo;
    KeyResult key;
    double energy = 0.0;            // 1..10 heuristic (loudness, onset density, brightness)
    double loudnessDb = -100.0;     // RMS in dBFS
    double durationSeconds = 0.0;
};

// Mono audio at a reduced sample rate, as used by all analysers.
struct MonoAudio
{
    std::vector<float> samples;
    double sampleRate = 22050.0;
    double durationSeconds = 0.0;
};

// Decodes any supported file to mono, reducing the sample rate to about 22 kHz.
// Returns false (with a message) if the file cannot be read or `cancel` becomes true.
bool decodeToMono (const juce::File& file, juce::AudioFormatManager& formats, MonoAudio& out,
                   juce::String& error, const std::atomic<bool>* cancel = nullptr);

TempoResult analyseTempo (const MonoAudio& audio);
KeyResult analyseKey (const MonoAudio& audio);
// Heuristic 1..10 energy from the loudness of the louder half of the track. Not validated against
// any reference; treat it as a rough sort aid, not as a measurement.
double estimateEnergy (const MonoAudio& audio, double& loudnessDbOut);

TrackAnalysis analyseAudio (const MonoAudio& audio);

// Camelot notation for a key, e.g. (0, major) -> "8B", (9, minor) -> "8A".
juce::String toCamelot (int tonic, bool minor);
juce::String toKeyName (int tonic, bool minor);

// Content fingerprint (size + three 64 KB samples, 128-bit FNV hash) used to recognise a file across moves and renames.
// Reading it downloads a OneDrive online-only file, so check isOnlineOnly() first.
juce::String computeFingerprint (const juce::File& file);

// True for cloud placeholder files (OneDrive "files on demand") whose content is not on this PC yet.
bool isOnlineOnly (const juce::File& file);

} // namespace pathboard::analysis
