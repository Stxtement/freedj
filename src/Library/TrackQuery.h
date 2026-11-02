#pragma once

#include "Library/TrackRecord.h"
#include <unordered_set>
#include <vector>

namespace pathboard
{

enum class TrackColumn
{
    title, artist, remix, album, bpm, key, genre, length, rating, path, bitrate, format,
    year, playCount, lastPlayed, firstSeen, energy, analysisQuality
};

juce::String columnDisplayName (TrackColumn);

// One condition of a smart playlist, e.g. { bpm, ">=", "120" }.
struct SmartCondition
{
    juce::String field;  // title artist remix album genre key tags comment format path bpm rating year length playCount bitrate daysSincePlayed
    juce::String op;     // contains, is, >, >=, <, <=
    juce::String value;
};

struct SmartRules
{
    bool matchAll = true;
    std::vector<SmartCondition> conditions;

    static SmartRules fromJson (const juce::String& json);
    juce::String toJson() const;
    bool matches (const TrackRecord&, juce::int64 nowMs) const;
};

// Search index entry kept next to each cached track (lower-case, built once).
juce::String buildSearchText (const TrackRecord&);

struct QuerySpec
{
    juce::String searchText;                         // all words must occur somewhere
    const std::unordered_set<juce::int64>* onlyIds = nullptr; // restrict to these track ids
    juce::String folderPrefix;                       // only tracks below this folder
    bool onlyMissing = false;
    bool hideMissing = false;
    bool onlyPlayed = false;
    int minRating = 0;
    double minBpm = 0.0, maxBpm = 0.0;               // 0 = unbounded
    juce::String keyFilter;
    juce::String genreFilter;
    TrackColumn sortColumn = TrackColumn::title;
    bool ascending = true;
};

// Returns indices into `tracks`, filtered and sorted. `searchIndex` must parallel `tracks`.
std::vector<int> runQuery (const std::vector<TrackRecord>& tracks, const std::vector<juce::String>& searchIndex,
                           const QuerySpec& spec);

// Sorts a list of indices into `tracks` in place (stable, with a secondary artist/title order).
void sortTrackIndices (const std::vector<TrackRecord>& tracks, std::vector<int>& indices, TrackColumn column, bool ascending);

// Groups of track ids that look like the same recording (same artist, title, remix and length +-1 s).
std::vector<std::vector<juce::int64>> findDuplicateGroups (const std::vector<TrackRecord>& tracks);

// Compares keys like "8A", "Am", "C#m" for sorting: Camelot order when parseable, then text.
int compareKeys (const juce::String& a, const juce::String& b);

} // namespace pathboard
