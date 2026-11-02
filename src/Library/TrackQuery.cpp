#include "TrackQuery.h"
#include <algorithm>
#include <map>

namespace pathboard
{

juce::String columnDisplayName (TrackColumn c)
{
    switch (c)
    {
        case TrackColumn::title:      return "Title";
        case TrackColumn::artist:     return "Artist";
        case TrackColumn::remix:      return "Remix";
        case TrackColumn::album:      return "Album";
        case TrackColumn::bpm:        return "BPM";
        case TrackColumn::key:        return "Key";
        case TrackColumn::genre:      return "Genre";
        case TrackColumn::length:     return "Length";
        case TrackColumn::rating:     return "Rating";
        case TrackColumn::path:       return "File path";
        case TrackColumn::bitrate:    return "Bitrate";
        case TrackColumn::format:     return "Format";
        case TrackColumn::year:       return "Year";
        case TrackColumn::playCount:  return "Plays";
        case TrackColumn::lastPlayed: return "Last played";
        case TrackColumn::firstSeen:  return "First seen";
        case TrackColumn::energy:     return "Energy";
        case TrackColumn::analysisQuality: return "Analysis";
    }
    return {};
}

//==============================================================================
namespace
{
    // Camelot wheel number+letter, e.g. "8A". Returns -1 if the text is not a key we understand.
    int camelotSortValue (const juce::String& raw)
    {
        const auto k = raw.trim();
        if (k.isEmpty())
            return -1;

        if (k.length() >= 2 && k.length() <= 3 && juce::CharacterFunctions::isDigit (k[0])
            && (k.getLastCharacter() == 'A' || k.getLastCharacter() == 'B' || k.getLastCharacter() == 'a' || k.getLastCharacter() == 'b'))
        {
            const int n = k.dropLastCharacters (1).getIntValue();
            if (n >= 1 && n <= 12)
                return n * 2 + (juce::CharacterFunctions::toUpperCase (k.getLastCharacter()) == 'B' ? 1 : 0);
        }
        return -1;
    }

    int compareText (const juce::String& a, const juce::String& b)
    {
        return a.compareNatural (b, false);
    }

    template <typename T>
    int compareNumbers (T a, T b) { return a < b ? -1 : (a > b ? 1 : 0); }

    int compareBy (TrackColumn column, const TrackRecord& a, const TrackRecord& b)
    {
        switch (column)
        {
            case TrackColumn::title:      return compareText (a.title, b.title);
            case TrackColumn::artist:     return compareText (a.artist, b.artist);
            case TrackColumn::remix:      return compareText (a.remix, b.remix);
            case TrackColumn::album:      return compareText (a.album, b.album);
            case TrackColumn::bpm:        return compareNumbers (a.bpm, b.bpm);
            case TrackColumn::key:        return compareKeys (a.key, b.key);
            case TrackColumn::genre:      return compareText (a.genre, b.genre);
            case TrackColumn::length:     return compareNumbers (a.lengthSeconds, b.lengthSeconds);
            case TrackColumn::rating:     return compareNumbers (a.rating, b.rating);
            case TrackColumn::path:       return compareText (a.path, b.path);
            case TrackColumn::bitrate:    return compareNumbers (a.bitrate, b.bitrate);
            case TrackColumn::format:     return compareText (a.format, b.format);
            case TrackColumn::year:       return compareNumbers (a.year, b.year);
            case TrackColumn::playCount:  return compareNumbers (a.playCount, b.playCount);
            case TrackColumn::lastPlayed: return compareNumbers (a.lastPlayed, b.lastPlayed);
            case TrackColumn::firstSeen:  return compareNumbers (a.firstSeen, b.firstSeen);
            case TrackColumn::energy:     return compareNumbers (a.energy, b.energy);
            case TrackColumn::analysisQuality: return compareNumbers (a.analysisQuality(), b.analysisQuality());
        }
        return 0;
    }
}

int compareKeys (const juce::String& a, const juce::String& b)
{
    const int ca = camelotSortValue (a), cb = camelotSortValue (b);
    if (ca >= 0 && cb >= 0)
        return compareNumbers (ca, cb);
    if (ca >= 0) return -1;   // known keys before unknown text
    if (cb >= 0) return 1;
    return compareText (a, b);
}

juce::String buildSearchText (const TrackRecord& t)
{
    return (t.title + " " + t.artist + " " + t.remix + " " + t.album + " " + t.genre + " " + t.comment + " "
            + t.tags + " " + t.key + " " + juce::File (t.path).getFileName()).toLowerCase();
}

//==============================================================================
std::vector<int> runQuery (const std::vector<TrackRecord>& tracks, const std::vector<juce::String>& searchIndex,
                           const QuerySpec& spec)
{
    juce::StringArray words;
    words.addTokens (spec.searchText.toLowerCase(), " ", "\"");
    words.removeEmptyStrings();

    const auto folder = spec.folderPrefix.isEmpty() ? juce::String()
                                                    : spec.folderPrefix.trimCharactersAtEnd ("\\/") + juce::File::getSeparatorString();

    std::vector<int> result;
    result.reserve (tracks.size());

    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        const auto& t = tracks[(size_t) i];

        if (spec.onlyIds != nullptr && spec.onlyIds->count (t.id) == 0) continue;
        if (spec.onlyMissing && ! t.missing) continue;
        if (spec.hideMissing && t.missing) continue;
        if (spec.onlyPlayed && t.lastPlayed == 0) continue;
        if (t.rating < spec.minRating) continue;
        if (spec.minBpm > 0.0 && t.bpm < spec.minBpm) continue;
        if (spec.maxBpm > 0.0 && t.bpm > spec.maxBpm) continue;
        if (spec.keyFilter.isNotEmpty() && ! t.key.equalsIgnoreCase (spec.keyFilter)) continue;
        if (spec.genreFilter.isNotEmpty() && ! t.genre.equalsIgnoreCase (spec.genreFilter)) continue;
        if (folder.isNotEmpty() && ! t.path.startsWithIgnoreCase (folder)) continue;

        bool allWords = true;
        for (const auto& w : words)
            if (! searchIndex[(size_t) i].contains (w)) { allWords = false; break; }
        if (! allWords) continue;

        result.push_back (i);
    }

    sortTrackIndices (tracks, result, spec.sortColumn, spec.ascending);
    return result;
}

void sortTrackIndices (const std::vector<TrackRecord>& tracks, std::vector<int>& indices, TrackColumn column, bool asc)
{
    // Key strings are parsed once per track, not once per comparison.
    std::vector<int> keyRank;
    if (column == TrackColumn::key)
    {
        keyRank.assign (tracks.size(), -1);
        for (int i : indices)
            keyRank[(size_t) i] = camelotSortValue (tracks[(size_t) i].key);
    }

    std::stable_sort (indices.begin(), indices.end(), [&] (int a, int b)
    {
        const auto& ta = tracks[(size_t) a];
        const auto& tb = tracks[(size_t) b];

        int c;
        if (column == TrackColumn::key)
        {
            const int ra = keyRank[(size_t) a], rb = keyRank[(size_t) b];
            c = (ra >= 0 && rb >= 0) ? compareNumbers (ra, rb) : (ra >= 0 ? -1 : (rb >= 0 ? 1 : compareText (ta.key, tb.key)));
        }
        else
        {
            c = compareBy (column, ta, tb);
        }

        if (c != 0)
            return asc ? c < 0 : c > 0;
        // Stable secondary order so equal values don't jump around.
        return compareText (ta.artist + ta.title, tb.artist + tb.title) < 0;
    });
}

//==============================================================================
std::vector<std::vector<juce::int64>> findDuplicateGroups (const std::vector<TrackRecord>& tracks)
{
    std::map<juce::String, std::vector<const TrackRecord*>> groups;

    for (const auto& t : tracks)
    {
        if (t.title.isEmpty())
            continue;
        const auto key = (t.artist + "|" + t.title + "|" + t.remix).toLowerCase().trim();
        groups[key].push_back (&t);
    }

    std::vector<std::vector<juce::int64>> result;
    for (auto& [key, members] : groups)
    {
        if (members.size() < 2)
            continue;

        // Within a name group, only tracks of (almost) equal length are duplicates.
        std::sort (members.begin(), members.end(),
                   [] (auto* a, auto* b) { return a->lengthSeconds < b->lengthSeconds; });

        std::vector<juce::int64> current { members[0]->id };
        for (size_t i = 1; i < members.size(); ++i)
        {
            if (members[i]->lengthSeconds - members[i - 1]->lengthSeconds <= 1.0)
            {
                current.push_back (members[i]->id);
            }
            else
            {
                if (current.size() > 1) result.push_back (current);
                current = { members[i]->id };
            }
        }
        if (current.size() > 1)
            result.push_back (current);
    }
    return result;
}

//==============================================================================
SmartRules SmartRules::fromJson (const juce::String& json)
{
    SmartRules rules;
    const auto parsed = juce::JSON::parse (json);
    if (! parsed.isObject())
        return rules;

    rules.matchAll = (bool) parsed.getProperty ("matchAll", true);
    if (auto* list = parsed.getProperty ("conditions", {}).getArray())
        for (const auto& c : *list)
            rules.conditions.push_back ({ c.getProperty ("field", {}).toString(),
                                          c.getProperty ("op", {}).toString(),
                                          c.getProperty ("value", {}).toString() });
    return rules;
}

juce::String SmartRules::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("matchAll", matchAll);
    juce::Array<juce::var> list;
    for (const auto& c : conditions)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("field", c.field);
        o->setProperty ("op", c.op);
        o->setProperty ("value", c.value);
        list.add (juce::var (o));
    }
    root->setProperty ("conditions", list);
    return juce::JSON::toString (juce::var (root), true);
}

namespace
{
    bool conditionMatches (const SmartCondition& c, const TrackRecord& t, juce::int64 nowMs)
    {
        const auto f = c.field;

        // Numeric fields
        double number = 0.0;
        bool numeric = true;
        if (f == "bpm")                 number = t.bpm;
        else if (f == "rating")         number = t.rating;
        else if (f == "year")           number = t.year;
        else if (f == "length")         number = t.lengthSeconds;
        else if (f == "playCount")      number = t.playCount;
        else if (f == "bitrate")        number = t.bitrate;
        else if (f == "daysSincePlayed")
            number = t.lastPlayed == 0 ? 100000.0 : (double) (nowMs - t.lastPlayed) / 86400000.0;
        else numeric = false;

        if (numeric)
        {
            const double v = c.value.getDoubleValue();
            if (c.op == ">")  return number > v;
            if (c.op == ">=") return number >= v;
            if (c.op == "<")  return number < v;
            if (c.op == "<=") return number <= v;
            if (c.op == "is") return std::abs (number - v) < 0.5;
            return false;
        }

        juce::String text;
        if (f == "title") text = t.title;
        else if (f == "artist") text = t.artist;
        else if (f == "remix") text = t.remix;
        else if (f == "album") text = t.album;
        else if (f == "genre") text = t.genre;
        else if (f == "key") text = t.key;
        else if (f == "tags") text = t.tags;
        else if (f == "comment") text = t.comment;
        else if (f == "format") text = t.format;
        else if (f == "path") text = t.path;
        else return false;

        if (c.op == "is")       return text.equalsIgnoreCase (c.value);
        if (c.op == "contains") return text.containsIgnoreCase (c.value);
        return false;
    }
}

bool SmartRules::matches (const TrackRecord& t, juce::int64 nowMs) const
{
    if (conditions.empty())
        return false;

    for (const auto& c : conditions)
    {
        const bool m = conditionMatches (c, t, nowMs);
        if (matchAll && ! m) return false;
        if (! matchAll && m) return true;
    }
    return matchAll;
}

} // namespace pathboard
