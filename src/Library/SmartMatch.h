#pragma once

#include "Genre/GenreTaxonomy.h"
#include "Library/TrackRecord.h"
#include <optional>
#include <vector>

namespace pathboard
{

enum class MatchLevel { none = 0, good = 1, veryGood = 2 };

struct CamelotKey
{
    int number = 0;       // 1..12
    bool minor = false;   // A = minor, B = major
    juce::String text() const { return juce::String (number) + (minor ? "A" : "B"); }
};

// "8A", "08a", "Am", "C#m", "F# minor", "Db major" -> Camelot key. Unknown text -> nullopt (never guessed).
std::optional<CamelotKey> parseKey (const juce::String& text);

// 1.0 same key, 0.9 relative major/minor or one step on the wheel, 0.5 two steps (energy lift), 0 otherwise.
double camelotCompatibility (const CamelotKey& a, const CamelotKey& b);

// What is known about a track for matching. Anything unknown stays unknown.
struct MatchSubject
{
    double bpm = 0.0;                          // 0 = unknown / not reliable
    std::optional<CamelotKey> key;
    std::vector<genre::GenreLabel> genres;     // empty = unknown
    juce::String description;                  // for tooltips

    bool hasAnything() const { return bpm > 0.0 || key.has_value() || ! genres.empty(); }
};

// Below these confidences an automatic analysis result is treated as unknown. Values the user set by hand
// or that come from the file's own tags are always used.
constexpr double minReliableBpmConfidence = 0.35;
constexpr double minReliableKeyConfidence = 0.25;
constexpr double minReliableGenreConfidence = 0.25;

// bpmOverride: use this tempo instead of the track's own (the audible tempo of a deck with pitch applied).
MatchSubject makeSubject (const TrackRecord& track, double bpmOverride = 0.0);

struct MatchDetail
{
    MatchLevel level = MatchLevel::none;
    double bpmScore = -1.0, keyScore = -1.0, genreScore = -1.0;   // -1 = unknown on at least one side
    double overall = 0.0;
    double bpmDifferencePercent = 0.0;
    double bpmFactor = 1.0;     // 1, 2 or 0.5: the candidate matches at normal, double or half time
    juce::String explanation;
};

double bpmCompatibility (double referenceBpm, double candidateBpm, double* differencePercent = nullptr, double* factor = nullptr);

MatchDetail evaluateMatch (const MatchSubject& reference, const MatchSubject& candidate);

} // namespace pathboard
