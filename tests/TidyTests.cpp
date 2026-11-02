// Tests for: cleaning pool / download clutter out of titles and artists (examples from a real library).

#include "TestSupport.h"
#include "Library/LibraryDatabase.h"
#include "Library/TrackTidy.h"

using namespace pathboard;
using testsupport::check;

namespace
{
TrackRecord make (const juce::String& title, const juce::String& artist, const juce::String& key = {}, double bpm = 0.0)
{
    TrackRecord t;
    t.title = title;
    t.artist = artist;
    t.key = key;
    t.bpm = bpm;
    return t;
}
}

int runTidyTests()
{
    std::cout << "Titles and artists" << std::endl;

    {
        auto t = make ("Get lucky (Jenia smile & Ser twister remix)_PN", "10A - 123 - Daft punk", "10A", 123.0);
        check (tidyTrackText (t) && t.title == "Get lucky" && t.artist == "Daft punk" && t.remix == "Jenia smile & Ser twister remix",
               "pool prefix, marker and remix are separated", t.title + " | " + t.artist + " | " + t.remix);
    }
    {
        auto t = make ("One More Time (Capital People Remix).mp3_PN", "10A - 123 - Daft Punk", "10B", 123.0);
        tidyTrackText (t);
        check (t.title == "One More Time" && t.remix == "Capital People Remix" && t.key == "10B", "file extension is removed, the existing key stays",
               t.title + " | " + t.remix + " | " + t.key);
    }
    {
        auto t = make ("Fedde_Le_Grand_vs._Kid_Cudi_-_Day_Night_Amplifier_[Roger-M_Mix]_PN", "10A - 126", "1B", 126.01);
        tidyTrackText (t);
        check (t.artist == "Fedde Le Grand vs. Kid Cudi" && t.title == "Day Night Amplifier" && t.remix == "Roger-M Mix",
               "a file-name style title is split into artist, title and remix", t.artist + " | " + t.title + " | " + t.remix);
        check (t.bpm == 126.01 && t.key == "1B", "BPM and key that exist are not replaced");
    }
    {
        auto t = make ("Hi_Tack - It's Gonna Be Alright_PN", "10A - 128 - 10A");
        tidyTrackText (t);
        check (t.artist == "Hi Tack" && t.title == "It's Gonna Be Alright", "a key in place of the artist is dropped, artist comes from the title",
               t.artist + " | " + t.title);
    }
    {
        auto t = make ("Lockdown myfreemp3.vip", "10.Koffee");
        tidyTrackText (t);
        check (t.title == "Lockdown" && t.artist == "10.Koffee", "website names are removed, other artists stay", t.title + " | " + t.artist);
    }
    {
        auto t = make ("Lovers On The Sun (Stadiumx Remix) [djmebbe.com]_PN", "10A - 128 - David Guetta ft. Sam Martin");
        tidyTrackText (t);
        check (t.title == "Lovers On The Sun" && t.remix == "Stadiumx Remix" && t.artist == "David Guetta ft. Sam Martin"
                   && t.key == "10A" && t.bpm == 128.0,
               "site group removed; key and BPM come from the prefix where the track has none",
               t.title + " | " + t.remix + " | " + t.artist + " | " + t.key + " | " + juce::String (t.bpm));
    }
    {
        auto t = make ("Poker Face", "Lady Gaga", "2A", 119.0);
        check (! tidyTrackText (t) && t.title == "Poker Face" && t.artist == "Lady Gaga", "a clean track is left alone");
    }
    {
        auto t = make ("Song (Radio Edit)", "Artist - Name", "", 0.0);
        check (! tidyTrackText (t) || t.artist == "Artist - Name", "an artist that merely contains a dash is not split");
    }
    {
        auto t = make ("Track", "9A - 128 - Someone", "", 0.0);
        t.manualFlags = TrackRecord::manualBpm | TrackRecord::manualKey;
        tidyTrackText (t);
        check (t.bpm == 0.0 && t.key.isEmpty() && t.artist == "Someone", "manual BPM and key are not filled from the prefix");
    }

    {
        auto a = make ("Closer", "10.The Chainsmokers");
        auto b = make ("Dream On", "10cc");
        auto c = make ("In Da Club", "50 Cent");
        auto d = make ("Like The Way I Do", "10.000 Maniacs");
        auto e = make ("- Geceleri Yakip 2014", "13.Dj Karaca ft Serdar Ortac");
        for (auto* t : { &a, &b, &c, &d, &e })
            tidyTrackText (*t);
        check (a.artist == "The Chainsmokers", "a track number in front of the artist is dropped", a.artist);
        check (b.artist == "10cc" && c.artist == "50 Cent" && d.artist == "10.000 Maniacs", "artists that start with a number stay",
               b.artist + " | " + c.artist + " | " + d.artist);
        check (e.title == "Geceleri Yakip 2014" && e.artist == "Dj Karaca ft Serdar Ortac", "leading dashes are removed from the title",
               e.title + " | " + e.artist);
    }

    // Stored in the database and kept.
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("pathboard_tidy_" + juce::String (juce::Random::getSystemRandom().nextInt()));
        dir.createDirectory();
        LibraryDatabase db;
        db.open (dir.getChildFile ("library.db"));
        TrackRecord t = make ("Get lucky (Remix)_PN", "10A - 123 - Daft punk");
        t.path = "C:\\music\\a.mp3";
        t.lengthSeconds = 200;
        t.fileSize = 1;
        t.firstSeen = 1;
        std::vector<TrackRecord> batch { t };
        db.upsertTracks (batch);

        auto loaded = db.loadAllTracks();
        auto copy = loaded.front();
        tidyTrackText (copy);
        check (db.updateTidiedTexts ({ copy }), "tidied texts are written");
        db.close();

        LibraryDatabase again;
        again.open (dir.getChildFile ("library.db"));
        const auto back = again.loadAllTracks().front();
        check (back.title == "Get lucky" && back.artist == "Daft punk" && back.remix == "Remix" && back.key == "10A" && back.bpm == 123.0,
               "the cleaned values are still there after reopening");
        again.close();
        dir.deleteRecursively();
    }

    return 0;
}
