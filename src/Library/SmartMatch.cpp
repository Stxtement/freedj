#include "SmartMatch.h"
#include "Analysis/AudioAnalysis.h"

namespace pathboard
{

std::optional<CamelotKey> parseKey (const juce::String& raw)
{
    auto t = raw.trim();
    if (t.isEmpty())
        return std::nullopt;

    // Camelot: 1A..12B
    {
        const auto last = t.getLastCharacter();
        const auto letter = juce::CharacterFunctions::toUpperCase (last);
        if ((letter == 'A' || letter == 'B') && t.length() >= 2 && t.length() <= 3 && t.dropLastCharacters (1).containsOnly ("0123456789"))
        {
            const int n = t.dropLastCharacters (1).getIntValue();
            if (n >= 1 && n <= 12)
                return CamelotKey { n, letter == 'A' };
        }
    }

    // Note names: "Am", "A min", "C#m", "Dbm", "F# minor", "C", "Bb major"
    static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto lower = t.toLowerCase();
    const auto letterChar = juce::CharacterFunctions::toUpperCase (t[0]);
    if (letterChar < 'A' || letterChar > 'G')
        return std::nullopt;

    int index = 0;
    {
        juce::String note (juce::String::charToString (letterChar));
        int pos = 1;
        if (t.length() > 1 && (t[1] == '#' || t[1] == 'b' || t[1] == 0x266F || t[1] == 0x266D))
        {
            note += (t[1] == '#' || t[1] == 0x266F) ? "#" : "b";
            pos = 2;
        }
        // flats -> sharps
        static const std::pair<const char*, const char*> flats[] = { { "Db", "C#" }, { "Eb", "D#" }, { "Gb", "F#" }, { "Ab", "G#" }, { "Bb", "A#" },
                                                                    { "Cb", "B" }, { "Fb", "E" } };
        for (auto& f : flats)
            if (note == f.first)
                note = f.second;
        if (note == "E#") note = "F";
        if (note == "B#") note = "C";

        bool found = false;
        for (int i = 0; i < 12; ++i)
            if (note == noteNames[i]) { index = i; found = true; }
        if (! found)
            return std::nullopt;

        const auto rest = lower.substring (pos).trim();
        bool minor;
        if (rest.isEmpty() || rest == "maj" || rest == "major")
            minor = false;
        else if (rest == "m" || rest == "min" || rest == "minor")
            minor = true;
        else
            return std::nullopt;

        const auto camelot = analysis::toCamelot (index, minor);
        return CamelotKey { camelot.dropLastCharacters (1).getIntValue(), minor };
    }
}

double camelotCompatibility (const CamelotKey& a, const CamelotKey& b)
{
    int step = std::abs (a.number - b.number);
    step = std::min (step, 12 - step);
    if (a.minor == b.minor)
    {
        if (step == 0) return 1.0;
        if (step == 1) return 0.9;
        if (step == 2) return 0.5;
        return 0.0;
    }
    return step == 0 ? 0.9 : 0.0;   // relative major / minor
}

//==============================================================================
MatchSubject makeSubject (const TrackRecord& t, double bpmOverride)
{
    MatchSubject s;
    // Automatic results count only when they are reliable. Manual values and the file's own tags always count
    // (analysisVersion <= 0 means the value did not come from the analysis).
    const bool fromAnalysis = t.analysisVersion > 0;

    if (t.bpm > 0.0 && (! fromAnalysis || t.isManual (TrackRecord::manualBpm) || t.bpmConfidence >= minReliableBpmConfidence))
        s.bpm = bpmOverride > 0.0 ? bpmOverride : t.bpm;

    if (const auto k = parseKey (t.key); k.has_value() && (! fromAnalysis || t.isManual (TrackRecord::manualKey) || t.keyConfidence >= minReliableKeyConfidence))
        s.key = k;

    if (t.genre.trim().isNotEmpty())
        s.genres = genre::parseGenreText (t.genre);
    else if (t.genreAutoMain.isNotEmpty() && t.genreConfidence >= minReliableGenreConfidence)
        s.genres.push_back ({ t.genreAutoMain, t.genreAutoSub, t.genreAutoCategory });

    s.description = t.artist + (t.artist.isNotEmpty() ? " - " : "") + t.title;
    return s;
}

double bpmCompatibility (double ref, double cand, double* differencePercent, double* factorOut)
{
    if (ref <= 0.0 || cand <= 0.0)
        return -1.0;

    double bestDiff = 1.0e9, bestFactor = 1.0;
    for (double factor : { 1.0, 2.0, 0.5 })
    {
        const double diff = (cand * factor / ref - 1.0) * 100.0;
        if (std::abs (diff) < std::abs (bestDiff) - 1.0e-9)
        {
            bestDiff = diff;
            bestFactor = factor;
        }
    }
    if (differencePercent != nullptr) *differencePercent = bestDiff;
    if (factorOut != nullptr) *factorOut = bestFactor;

    const double d = std::abs (bestDiff);
    double score = d <= 3.0 ? 1.0 : (d <= 6.0 ? 0.7 : (d <= 10.0 ? 0.4 : 0.0));
    if (bestFactor != 1.0)
        score *= 0.95;   // half / double time works, but straight time is a little better
    return score;
}

MatchDetail evaluateMatch (const MatchSubject& ref, const MatchSubject& cand)
{
    MatchDetail d;
    d.bpmScore = bpmCompatibility (ref.bpm, cand.bpm, &d.bpmDifferencePercent, &d.bpmFactor);
    d.keyScore = ref.key && cand.key ? camelotCompatibility (*ref.key, *cand.key) : -1.0;
    d.genreScore = genre::genreSimilarity (ref.genres, cand.genres);

    const double B = d.bpmScore, K = d.keyScore, G = d.genreScore;

    // Tempo is the basis; one more known property is needed, and a clashing key rules a track out.
    if (B <= 0.0 || (K < 0.0 && G < 0.0) || K == 0.0)
        return d;

    double weighted = 0.4 * B, weights = 0.4;
    if (K >= 0.0) { weighted += 0.4 * K; weights += 0.4; }
    if (G >= 0.0) { weighted += 0.2 * G; weights += 0.2; }
    d.overall = weighted / weights;

    const bool genreOk = G < 0.0 || G >= 0.7;
    if (d.overall >= 0.85 && B >= 0.7 && K >= 0.9 && genreOk)
        d.level = MatchLevel::veryGood;
    else if (d.overall >= 0.6 && B >= 0.4 && (K < 0.0 || K >= 0.5) && (G < 0.0 || G > 0.0 || (K >= 0.9 && B >= 0.7)))
        d.level = MatchLevel::good;

    if (d.level != MatchLevel::none)
    {
        d.explanation = "BPM " + juce::String (cand.bpm, 1) + " vs " + juce::String (ref.bpm, 1)
                        + " (" + (d.bpmDifferencePercent >= 0 ? "+" : "") + juce::String (d.bpmDifferencePercent, 1) + " %"
                        + (d.bpmFactor == 2.0 ? ", double time" : (d.bpmFactor == 0.5 ? ", half time" : "")) + ")";
        if (ref.key && cand.key)
            d.explanation << ", key " << cand.key->text() << " / " << ref.key->text();
        if (G >= 0.0)
            d.explanation << ", genre " << cand.genres.front().toString() << " / " << ref.genres.front().toString();
    }
    return d;
}

} // namespace pathboard
