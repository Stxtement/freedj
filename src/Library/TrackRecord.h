#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

namespace pathboard
{

struct TrackRecord
{
    juce::int64 id = 0;
    juce::String path, title, artist, remix, album, genre, comment, tags, key, format;
    int year = 0;
    int rating = 0;          // 0..5
    int bitrate = 0;         // kbit/s
    int playCount = 0;
    double bpm = 0.0;
    double lengthSeconds = 0.0;
    juce::int64 fileSize = 0;
    juce::int64 modified = 0;    // file modification time, ms since epoch
    juce::int64 firstSeen = 0;   // ms since epoch
    juce::int64 lastPlayed = 0;  // ms since epoch, 0 = never
    bool missing = false;
    bool analysed = false;

    // Automatic analysis (see Analysis/AudioAnalysis.h). analysisVersion: 0 = not analysed, -1 = failed.
    int analysisVersion = 0;
    juce::int64 analysedAt = 0;
    juce::String fingerprint;
    double bpmConfidence = 0.0;
    double keyConfidence = 0.0;
    double firstBeat = 0.0;       // seconds; beat grid anchor
    int downbeat = 0;             // 0..3: beats from the anchor to the first bar start
    bool variableTempo = false;
    double energy = 0.0;          // 1..10 heuristic, 0 = unknown
    double loudnessDb = 0.0;
    int manualFlags = 0;          // ManualFlag bits: values the user set by hand are never overwritten by analysis

    enum ManualFlag { manualBpm = 1, manualKey = 2, manualGrid = 4, manualGenre = 8 };
    bool isManual (ManualFlag f) const { return (manualFlags & f) != 0; }

    // 0..100, how much to trust the automatic result; 100 for hand-corrected values.
    int analysisQuality() const
    {
        if (analysisVersion <= 0)
            return -1;
        const double bpmPart = isManual (manualBpm) ? 1.0 : bpmConfidence;
        const double keyPart = isManual (manualKey) ? 1.0 : keyConfidence;
        return (int) std::lround (100.0 * (0.7 * bpmPart + 0.3 * keyPart));
    }

    juce::File getFile() const { return juce::File (path); }
};

struct PlaylistInfo
{
    enum class Kind { playlist = 0, smart = 1 };

    juce::int64 id = 0;
    juce::String name;
    Kind kind = Kind::playlist;
    juce::String rules; // smart playlists: JSON, see SmartRules
};

} // namespace pathboard
