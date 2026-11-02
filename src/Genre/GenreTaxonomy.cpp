#include "GenreTaxonomy.h"
#include "DiscogsStyles.h"
#include "GenreClassifier.h"
#include <map>

namespace pathboard::genre
{

namespace
{
    // Electronic styles that belong to a larger main genre; everything else keeps its own name.
    const std::map<juce::String, juce::String>& electronicMainGenres()
    {
        static const std::map<juce::String, juce::String> m = [] {
            std::map<juce::String, juce::String> r;
            auto add = [&r] (const char* main, std::initializer_list<const char*> styles) { for (auto* s : styles) r[s] = main; };
            add ("House", { "House", "Acid House", "Deep House", "Electro House", "Euro House", "Garage House", "Ghetto House", "Hard House",
                            "Hip-House", "Italo House", "Progressive House", "Tech House", "Tribal House", "Tropical House" });
            add ("Techno", { "Techno", "Deep Techno", "Dub Techno", "Hard Techno", "Minimal Techno", "Minimal", "Schranz", "Acid" });
            add ("Trance", { "Trance", "Goa Trance", "Hard Trance", "Progressive Trance", "Psy-Trance", "Tech Trance" });
            add ("Drum & Bass", { "Drum n Bass", "Jungle" });
            add ("Garage / Bass", { "UK Garage", "Speed Garage", "Grime", "Bassline", "Dubstep", "Halftime", "Ghetto" });
            add ("Breaks", { "Breakbeat", "Breaks", "Big Beat", "Progressive Breaks", "Broken Beat", "Breakcore" });
            add ("Hard Dance", { "Hardcore", "Happy Hardcore", "Gabber", "Speedcore", "Hardstyle", "Jumpstyle", "Makina", "Hands Up", "Donk", "Beatdown" });
            add ("Electro", { "Electro", "Electroclash" });
            add ("Disco", { "Disco", "Nu-Disco", "Italo-Disco", "Euro-Disco", "Hi NRG", "Disco Polo" });
            add ("Eurodance", { "Eurodance", "Eurobeat", "Italodance" });
            add ("Synth-pop", { "Synth-pop", "Synthwave", "New Wave", "Darkwave", "Vaporwave", "Chillwave" });
            add ("Pop", { "Dance-pop" });
            add ("Hip Hop", { "Hip Hop" });
            add ("Latin", { "Latin" });
            return r;
        }();
        return m;
    }

    // Words people write in tags that are not Discogs style names.
    const std::map<juce::String, GenreLabel>& aliases()
    {
        static const std::map<juce::String, GenreLabel> m = [] {
            std::map<juce::String, GenreLabel> r;
            auto add = [&r] (const char* key, const char* main, const char* sub, const char* category) { r[key] = { main, sub, category }; };
            add ("afro house", "House", "Afro House", "Electronic");
            add ("melodic house", "House", "Melodic House", "Electronic");
            add ("melodic techno", "Techno", "Melodic Techno", "Electronic");
            add ("peak time techno", "Techno", "Peak Time Techno", "Electronic");
            add ("organic house", "House", "Organic House", "Electronic");
            add ("bass house", "House", "Bass House", "Electronic");
            add ("funky house", "House", "Funky House", "Electronic");
            add ("future house", "House", "Future House", "Electronic");
            add ("progressive", "House", "Progressive House", "Electronic");
            add ("edm", "EDM", "", "Electronic");
            add ("big room", "EDM", "Big Room", "Electronic");
            add ("dance", "Dance", "", "Electronic");
            add ("club", "Dance", "", "Electronic");
            add ("electronic", "Electronic", "", "Electronic");
            add ("electronica", "Electronic", "", "Electronic");
            add ("dnb", "Drum & Bass", "", "Electronic");
            add ("drum and bass", "Drum & Bass", "", "Electronic");
            add ("drum & bass", "Drum & Bass", "", "Electronic");
            add ("drum&bass", "Drum & Bass", "", "Electronic");
            add ("hip-hop", "Hip Hop", "", "Hip Hop");
            add ("hiphop", "Hip Hop", "", "Hip Hop");
            add ("hip hop", "Hip Hop", "", "Hip Hop");
            add ("rap", "Hip Hop", "Rap", "Hip Hop");
            add ("r&b", "R&B", "", "Funk / Soul");
            add ("rnb", "R&B", "", "Funk / Soul");
            add ("pop", "Pop", "", "Pop");
            add ("rock", "Rock", "", "Rock");
            add ("reggaeton", "Latin", "Reggaeton", "Latin");
            add ("dancehall", "Reggae", "Dancehall", "Reggae");
            add ("deutschrap", "Hip Hop", "German Hip Hop", "Hip Hop");
            add ("schlager", "Schlager", "", "Pop");
            add ("funk / soul", "Funk / Soul", "", "Funk / Soul");
            add ("funk", "Funk / Soul", "Funk", "Funk / Soul");
            add ("soul", "Funk / Soul", "Soul", "Funk / Soul");
            add ("folk, world, & country", "Folk / World", "", "Folk, World, & Country");
            return r;
        }();
        return m;
    }

    juce::String normalise (juce::String s)
    {
        s = s.trim().toLowerCase().replace ("_", " ").replace ("  ", " ");
        return s;
    }

    // Style name (lower case) -> label, built once from the Discogs vocabulary.
    const std::map<juce::String, GenreLabel>& styleIndex()
    {
        static const std::map<juce::String, GenreLabel> m = [] {
            std::map<juce::String, GenreLabel> r;
            for (int i = 0; i < numStyles; ++i)
            {
                const auto raw = discogsStyle (i);
                const auto label = fromDiscogs (raw);
                const auto key = normalise (raw.fromLastOccurrenceOf ("---", false, false));
                if (r.find (key) == r.end())
                    r[key] = label;
            }
            // Top-level category names and main-genre names are genres too ("Rock", "Reggae", "Jazz").
            for (int i = 0; i < numStyles; ++i)
            {
                const juce::String category = discogsStyle (i).upToFirstOccurrenceOf ("---", false, false);
                const auto key = normalise (category);
                if (r.find (key) == r.end())
                    r[key] = fromDiscogs (category + "---" + category);
            }
            return r;
        }();
        return m;
    }

    std::vector<juce::String> splitTokens (const juce::String& text)
    {
        juce::String s = text;
        for (auto sep : { "/", ",", ";", "|", " - ", " & " })
            s = s.replace (sep, "\n");
        // "r&b" and "drum & bass" must survive: only split " & " when both sides are known words (see parse).
        juce::StringArray parts;
        parts.addLines (s);
        std::vector<juce::String> out;
        for (auto& p : parts)
            if (p.trim().isNotEmpty())
                out.push_back (p.trim());
        return out;
    }

    juce::String titleCase (const juce::String& s)
    {
        juce::StringArray words;
        words.addTokens (s, " ", "");
        for (auto& w : words)
            if (w.isNotEmpty())
                w = w.substring (0, 1).toUpperCase() + w.substring (1);
        return words.joinIntoString (" ");
    }
}

//==============================================================================
GenreLabel fromDiscogs (const juce::String& discogs)
{
    const auto category = discogs.upToFirstOccurrenceOf ("---", false, false).trim();
    const auto style = discogs.fromLastOccurrenceOf ("---", false, false).trim();

    GenreLabel label;
    label.category = category;
    label.sub = style;

    if (category == "Electronic")
    {
        const auto& m = electronicMainGenres();
        const auto it = m.find (style);
        label.main = it != m.end() ? it->second : "Electronic";
        if (label.main == "Electronic")
            label.sub = style;
    }
    else if (category == "Folk, World, & Country")
    {
        label.main = "Folk / World";
    }
    else if (category == "Non-Music")
    {
        label.main = "Non-Music";
    }
    else
    {
        label.main = category;
    }

    if (label.sub == label.main)
        label.sub.clear();
    return label;
}

std::vector<GenreLabel> parseGenreText (const juce::String& text)
{
    std::vector<GenreLabel> result;
    const auto whole = normalise (text);
    if (whole.isEmpty())
        return result;

    // The whole text first ("Drum & Bass", "R&B", "Tech House").
    auto lookup = [] (const juce::String& key, GenreLabel& out)
    {
        if (const auto a = aliases().find (key); a != aliases().end()) { out = a->second; return true; }
        if (const auto s = styleIndex().find (key); s != styleIndex().end()) { out = s->second; return true; }
        return false;
    };

    GenreLabel found;
    if (lookup (whole, found))
    {
        result.push_back (found);
        return result;
    }

    for (const auto& token : splitTokens (text))
    {
        const auto key = normalise (token);
        if (lookup (key, found))
            result.push_back (found);
        else if (key.isNotEmpty())
            result.push_back ({ titleCase (key), {}, {} });   // unknown word: kept as it is, compared by name only
    }

    // "House / Tech House": main + sub written separately collapse into one label.
    if (result.size() == 2 && result[0].main == result[1].main)
        result.erase (result.begin() + (result[0].sub.isEmpty() ? 0 : 1));
    return result;
}

double genreSimilarity (const std::vector<GenreLabel>& a, const std::vector<GenreLabel>& b)
{
    if (a.empty() || b.empty())
        return -1.0;

    static const std::map<std::pair<juce::String, juce::String>, double> adjacency = [] {
        std::map<std::pair<juce::String, juce::String>, double> m;
        auto add = [&m] (const char* x, const char* y, double v) { m[{ x, y }] = v; m[{ y, x }] = v; };
        add ("House", "Techno", 0.5);       add ("House", "Trance", 0.4);        add ("Techno", "Trance", 0.5);
        add ("House", "Electro", 0.55);     add ("House", "Disco", 0.55);        add ("House", "Eurodance", 0.5);
        add ("House", "EDM", 0.6);          add ("Techno", "EDM", 0.45);         add ("Trance", "EDM", 0.6);
        add ("Electro", "EDM", 0.55);       add ("House", "Dance", 0.65);        add ("Dance", "EDM", 0.7);
        add ("Dance", "Eurodance", 0.7);    add ("Dance", "Electro", 0.55);      add ("Dance", "Pop", 0.5);
        add ("Dance", "Techno", 0.45);      add ("Dance", "Trance", 0.5);        add ("Dance", "Disco", 0.5);
        add ("Drum & Bass", "Breaks", 0.5); add ("Drum & Bass", "Garage / Bass", 0.45); add ("Breaks", "Garage / Bass", 0.4);
        add ("Hard Dance", "Techno", 0.4);  add ("Hard Dance", "Trance", 0.45);
        add ("Hip Hop", "Funk / Soul", 0.5); add ("Hip Hop", "R&B", 0.65);       add ("Funk / Soul", "R&B", 0.7);
        add ("Hip Hop", "Reggae", 0.35);    add ("Hip Hop", "Pop", 0.3);         add ("Reggae", "Latin", 0.4);
        add ("Pop", "Rock", 0.4);           add ("Pop", "Eurodance", 0.5);       add ("Pop", "Electro", 0.4);
        add ("Pop", "Disco", 0.4);          add ("Pop", "Synth-pop", 0.65);      add ("Pop", "House", 0.3);
        add ("Pop", "R&B", 0.45);           add ("Pop", "Latin", 0.35);          add ("Disco", "Funk / Soul", 0.5);
        add ("Synth-pop", "Electro", 0.5);  add ("Rock", "Synth-pop", 0.3);
        return m;
    }();

    double best = 0.0;
    for (const auto& x : a)
        for (const auto& y : b)
        {
            double s = 0.0;
            if (x.main == y.main)
            {
                if (x.sub.isNotEmpty() && y.sub.isNotEmpty())
                    s = x.sub == y.sub ? 1.0 : 0.75;
                else
                    s = 0.9;   // one side only knows the main genre
            }
            else if (const auto it = adjacency.find ({ x.main, y.main }); it != adjacency.end())
            {
                s = it->second;
            }
            best = std::max (best, s);
        }
    return best;
}

} // namespace pathboard::genre
