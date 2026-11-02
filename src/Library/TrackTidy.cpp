#include "TrackTidy.h"
#include "Library/TrackMetadata.h"
#include <regex>

namespace pathboard
{

namespace
{
    bool looksLikeDomain (const juce::String& word)
    {
        // name.tld without spaces, e.g. "myfreemp3.vip", "djmebbe.com", "www.site.net"
        static const std::regex domain (R"(^(www\.)?[a-z0-9][a-z0-9\-]*(\.[a-z0-9\-]+)*\.(com|net|org|vip|info|ru|biz|to|me|fm|cc|tv|us|de|pl|ws|co)$)",
                                        std::regex::icase);
        return std::regex_match (word.toStdString(), domain);
    }

    // Removes "[site.com]" / "(www.site.net)" groups.
    juce::String removeSiteGroups (juce::String text)
    {
        for (const auto& pair : { std::pair<juce::juce_wchar, juce::juce_wchar> { '[', ']' }, { '(', ')' } })
        {
            int start = 0;
            while ((start = text.indexOfChar (start, pair.first)) >= 0)
            {
                const int end = text.indexOfChar (start, pair.second);
                if (end < 0)
                    break;
                if (looksLikeDomain (text.substring (start + 1, end).trim()))
                    text = text.substring (0, start) + " " + text.substring (end + 1);
                else
                    start = end + 1;
            }
        }
        return text;
    }

    juce::String collapse (const juce::String& s)
    {
        juce::StringArray words;
        words.addTokens (s, " \t", "");
        words.removeEmptyStrings();
        return words.joinIntoString (" ").trim();
    }

    // "10A - 128 - Rest": key, bpm and the rest. The rest may be empty.
    struct Prefix { bool found = false; juce::String key; double bpm = 0.0; juce::String rest; };

    Prefix parsePrefix (const juce::String& text)
    {
        static const std::regex pattern (R"(^\s*(\d{1,2}[ABab])\s*-\s*(\d{2,3}(?:[.,]\d+)?)\s*(?:-\s*(.*))?$)");
        std::smatch m;
        const auto s = text.toStdString();
        Prefix p;
        if (! std::regex_match (s, m, pattern))
            return p;
        p.found = true;
        p.key = juce::String (m[1].str()).toUpperCase();
        p.bpm = juce::String (m[2].str()).replaceCharacter (',', '.').getDoubleValue();
        p.rest = m[3].matched ? juce::String::fromUTF8 (m[3].str().c_str()).trim() : juce::String();
        return p;
    }

    bool isKeyOnly (const juce::String& s)
    {
        static const std::regex key (R"(^\d{1,2}[ABab]$)");
        return std::regex_match (s.trim().toStdString(), key);
    }

    juce::String underscoresToSpaces (const juce::String& s)
    {
        return s.containsChar (' ') || ! s.containsChar ('_') ? s : s.replaceCharacter ('_', ' ');
    }

    // Track numbers glued to the front: "10.The Chainsmokers", "27 - Artist". Needs two or three digits and a
    // letter after the separator, so "10cc", "50 Cent", "10.000 Maniacs" and "1.FC Koeln" stay as they are.
    juce::String dropTrackNumber (const juce::String& s)
    {
        static const std::regex number (R"(^\d{2,3}\s*[.\-_)]\s*([^\d\s\-_.].*)$)");
        std::smatch m;
        const auto utf8 = s.toStdString();
        if (std::regex_match (utf8, m, number))
            return juce::String::fromUTF8 (m[1].str().c_str());
        return s;
    }

    juce::String dropLeadingDashes (juce::String s)
    {
        while (s.startsWithChar ('-') || s.startsWithChar (0x2013) || s.startsWithChar (' '))
            s = s.substring (1);
        return s;
    }
}

juce::String tidyTitleText (const juce::String& input)
{
    auto s = input.trim();

    if (s.endsWith ("_PN"))
        s = s.dropLastCharacters (3);                              // DJ pool marker
    for (auto ext : { ".mp3", ".wav", ".flac", ".m4a", ".aif", ".aiff", ".ogg" })
        if (s.endsWithIgnoreCase (ext))
        {
            s = s.dropLastCharacters ((int) std::strlen (ext));
            break;
        }

    s = underscoresToSpaces (s);
    s = removeSiteGroups (s);

    // Loose web addresses ("myfreemp3.vip") that were glued to the title.
    juce::StringArray words;
    words.addTokens (s, " ", "");
    words.removeEmptyStrings();
    for (int i = words.size(); --i >= 0;)
        if (looksLikeDomain (words[i]))
            words.remove (i);
    s = words.joinIntoString (" ");

    s = dropLeadingDashes (collapse (s));
    while (s.endsWith (" -") || s.endsWith ("-"))
        s = s.dropLastCharacters (1).trim();
    return s;
}

bool tidyTrackText (TrackRecord& t)
{
    const auto before = t;

    t.title = tidyTitleText (t.title);

    // A title that starts with the pool's "10A - 128 - " prefix.
    if (const auto p = parsePrefix (t.title); p.found && p.rest.isNotEmpty())
    {
        t.title = p.rest;
        if (t.key.isEmpty() && ! t.isManual (TrackRecord::manualKey)) t.key = p.key;
        if (t.bpm <= 0.0 && ! t.isManual (TrackRecord::manualBpm))    t.bpm = p.bpm;
    }

    // Artist: pool prefix, site clutter, underscores.
    if (const auto p = parsePrefix (t.artist); p.found)
    {
        if (t.key.isEmpty() && ! t.isManual (TrackRecord::manualKey)) t.key = p.key;
        if (t.bpm <= 0.0 && ! t.isManual (TrackRecord::manualBpm))    t.bpm = p.bpm;
        t.artist = isKeyOnly (p.rest) ? juce::String() : p.rest;
    }
    t.artist = dropTrackNumber (collapse (removeSiteGroups (underscoresToSpaces (t.artist))));

    // "Artist - Title" stuck in the title while the artist is empty.
    if (t.artist.isEmpty())
    {
        const int dash = t.title.indexOf (" - ");
        if (dash > 0 && dash <= 60)
        {
            const auto left = underscoresToSpaces (t.title.substring (0, dash).trim());
            const auto right = t.title.substring (dash + 3).trim();
            if (left.isNotEmpty() && right.isNotEmpty() && ! isKeyOnly (left))
            {
                t.artist = left;
                t.title = right;
            }
        }
    }

    if (t.remix.isEmpty())
        TrackMetadata::splitRemix (t.title, t.remix);

    return t.title != before.title || t.artist != before.artist || t.remix != before.remix
           || t.key != before.key || t.bpm != before.bpm;
}

} // namespace pathboard
