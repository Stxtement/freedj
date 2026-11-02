#pragma once

#include "Library/TrackRecord.h"
#include <unordered_map>
#include <vector>

namespace pathboard::spotify
{

struct SpotifyTrack
{
    juce::String id, title, artist, album;   // artist: all artists, joined with ", "
    int durationMs = 0;
};

// Lower case, letters and digits only; "(feat. x)" style credits and "&" are normalised, so the same
// song is recognised across tag styles.
juce::String normaliseForMatch (const juce::String& text);

// Finds the local file for a Spotify track: same title (normalised), a shared artist, and - if both lengths
// are known - about the same duration (a 3 min radio edit is not the 7 min extended mix).
// Only exact matches count; nothing is guessed.
class LocalIndex
{
public:
    explicit LocalIndex (const std::vector<TrackRecord>& tracks);
    // Library id of the matching track, or 0.
    juce::int64 find (const SpotifyTrack& track) const;

private:
    struct Entry { juce::int64 id; juce::String artist; double lengthSeconds; };
    std::unordered_map<juce::String, std::vector<Entry>> byTitle;
};

} // namespace pathboard::spotify
