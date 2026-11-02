#include "SpotifyMatch.h"

namespace pathboard::spotify
{

namespace
{
    // Removes bracketed credits like "(feat. X)", "[with Y]" - but keeps "(Extended Mix)" and "(X Remix)".
    juce::String stripCredits (juce::String text)
    {
        for (const auto& pair : { std::pair<juce::juce_wchar, juce::juce_wchar> { '(', ')' }, { '[', ']' } })
        {
            int start = 0;
            while ((start = text.indexOfChar (start, pair.first)) >= 0)
            {
                const int end = text.indexOfChar (start, pair.second);
                if (end < 0)
                    break;
                const auto inner = text.substring (start + 1, end).trim().toLowerCase();
                const bool credit = inner.startsWith ("feat") || inner.startsWith ("ft.") || inner.startsWith ("ft ")
                                    || inner.startsWith ("with ") || inner.startsWith ("prod");
                if (credit)
                    text = text.substring (0, start) + " " + text.substring (end + 1);
                else
                    start = end + 1;
            }
        }
        return text;
    }

    bool shareArtist (const juce::String& localArtist, const juce::String& spotifyArtists)
    {
        const auto local = normaliseForMatch (localArtist);
        if (local.isEmpty())
            return false;
        juce::StringArray parts;
        parts.addTokens (spotifyArtists, ",", "");
        for (const auto& p : parts)
        {
            const auto one = normaliseForMatch (p);
            if (one.isNotEmpty() && (local.contains (one) || one.contains (local)))
                return true;
        }
        const auto all = normaliseForMatch (spotifyArtists);
        return all.isNotEmpty() && (local.contains (all) || all.contains (local));
    }
}

juce::String normaliseForMatch (const juce::String& text)
{
    auto s = stripCredits (text).toLowerCase().replace ("&", " and ");
    juce::String out;
    for (auto c : s)
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            out << c;
    return out;
}

LocalIndex::LocalIndex (const std::vector<TrackRecord>& tracks)
{
    for (const auto& t : tracks)
    {
        if (t.missing || t.title.isEmpty())
            continue;
        byTitle[normaliseForMatch (t.title)].push_back ({ t.id, t.artist, t.lengthSeconds });
    }
}

juce::int64 LocalIndex::find (const SpotifyTrack& track) const
{
    const auto it = byTitle.find (normaliseForMatch (track.title));
    if (it == byTitle.end())
        return 0;

    for (const auto& e : it->second)
    {
        if (! shareArtist (e.artist, track.artist))
            continue;
        if (e.lengthSeconds > 0.0 && track.durationMs > 0 && std::abs (e.lengthSeconds - track.durationMs / 1000.0) > 15.0)
            continue;
        return e.id;
    }
    return 0;
}

} // namespace pathboard::spotify
