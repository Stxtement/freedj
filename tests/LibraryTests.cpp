// Offline tests for the music library: database, tag reading, scanner, queries, playlists, performance.

#include "TestSupport.h"
#include "Library/LibraryManager.h"
#include "Library/TrackMetadata.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

using namespace pathboard;
using testsupport::check;

namespace
{
double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

void writeAudio (const juce::File& file, juce::AudioFormat& format, double seconds, int bits, double freq)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (22050.0)
                                                                                .withNumChannels (1)
                                                                                .withBitsPerSample (bits));
    jassert (writer != nullptr);

    const int total = (int) (seconds * 22050.0);
    juce::AudioBuffer<float> buffer (1, total);
    for (int i = 0; i < total; ++i)
        buffer.setSample (0, i, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / 22050.0));
    writer->writeFromAudioSampleBuffer (buffer, 0, total);
}

void writeWav (const juce::File& f, double seconds = 0.4, double freq = 440.0)
{
    juce::WavAudioFormat wav;
    writeAudio (f, wav, seconds, 16, freq);
}

void pump (int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
}

bool waitForScan (LibraryManager& lib, int timeoutMs = 60000)
{
    const auto start = nowMs();
    while (lib.isScanning() && nowMs() - start < timeoutMs)
        pump (20);
    pump (50); // deliver the completion callback
    return ! lib.isScanning();
}

TrackRecord synthetic (int i)
{
    static const char* artists[] = { "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel" };
    static const char* genres[] = { "House", "Techno", "Hip Hop", "Pop", "Drum & Bass" };
    TrackRecord t;
    t.path = "D:\\Music\\Synthetic\\" + juce::String (i % 500) + "\\track " + juce::String (i) + ".mp3";
    t.title = "Song " + juce::String (i);
    t.artist = artists[i % 8];
    t.album = "Album " + juce::String (i % 1000);
    t.genre = genres[i % 5];
    t.bpm = 70.0 + (i % 100);
    t.key = juce::String (1 + i % 12) + ((i % 2) ? "A" : "B");
    t.lengthSeconds = 150.0 + (i % 200);
    t.bitrate = 320;
    t.format = "MP3";
    t.fileSize = 5000000 + i;
    t.modified = 1700000000000LL + i;
    t.firstSeen = 1700000000000LL;
    return t;
}
}

// Development helper: `FreeDJTests --seed-library <dbFile> <folder> <count>` writes <count> tagged
// WAV files into <folder> (if it is empty) and scans them into a fresh library database.
int seedLibrary (const juce::File& dbFile, const juce::File& folder, int count)
{
    static const char* artists[] = { "Nova Drift", "Kiko Bass", "The Lowlines", "Mara Voss", "Echo Chamber", "DJ Prism" };
    static const char* genres[] = { "Tech House", "House", "Techno", "Hip-Hop", "Pop", "Drum & Bass", "Afro House" };
    static const char* keys[] = { "1A", "4A", "5A", "8A", "8B", "9A", "11B", "12A" };
    static const char* words[] = { "Midnight", "Pulse", "Gravity", "Sunrise", "Neon", "Echoes", "Voltage", "Mirage", "Skyline", "Rewind" };

    if (folder.findChildFiles (juce::File::findFiles, true, "*.wav").isEmpty())
    {
        juce::Random rng (42);
        for (int i = 0; i < count; ++i)
        {
            TrackRecord t;
            t.artist = artists[i % 6];
            t.title = juce::String (words[rng.nextInt (10)]) + " " + words[rng.nextInt (10)];
            if (i % 5 == 0) t.remix = "Club Remix";
            t.album = "Album " + juce::String (i % 7 + 1);
            t.genre = genres[i % 7];
            t.bpm = 90.0 + rng.nextInt (60);
            t.key = keys[i % 8];
            t.year = 2015 + i % 10;
            const auto file = folder.getChildFile (juce::String (i % 4) + "/" + t.artist + " - " + t.title + " " + juce::String (i) + ".wav");
            writeWav (file, 1.0 + 0.05 * (i % 40), 200.0 + 5.0 * i);
            TrackMetadata::writeTags (file, t);
        }
    }

    LibraryManager lib;
    if (! lib.open (dbFile).wasOk())
        return 1;
    lib.addFolderAndScan (folder);
    waitForScan (lib);
    for (int i = 0; i < (int) lib.getTracks().size(); i += 3)
        lib.setRating (lib.getTracks()[(size_t) i].id, 1 + i % 5);
    std::cout << "seeded " << lib.getTracks().size() << " tracks" << std::endl;
    return 0;
}

int runLibraryTests()
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJLibraryTests");
    root.deleteRecursively();
    root.createDirectory();

    //==========================================================================
    std::cout << "Database" << std::endl;
    {
        const auto dbFile = root.getChildFile ("db/library.db");
        {
            LibraryDatabase db;
            check (db.open (dbFile).wasOk(), "creates a new database");

            std::vector<TrackRecord> batch { synthetic (1), synthetic (2) };
            const auto unicodeTitle = juce::String::fromUTF8 ("\xC3\x9Cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 T\xC3\xA4g \xE2\x80\x93 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
            batch[0].title = unicodeTitle;
            check (db.upsertTracks (batch) && batch[0].id > 0 && batch[1].id > batch[0].id, "inserts tracks and returns ids");
            check (db.loadAllTracks()[0].title == unicodeTitle, "umlauts, dashes and Japanese text survive the database");

            auto again = batch[0];
            again.title = "Changed In File";
            std::vector<TrackRecord> reinsert { again };
            check (db.upsertTracks (reinsert) && reinsert[0].id == batch[0].id, "upsert by path keeps the same id");
            check (db.loadAllTracks().size() == 2, "upsert does not duplicate rows");

            db.setTrackRating (batch[0].id, 5);
            db.upsertTracks (reinsert);
            check (db.loadAllTracks()[0].rating == 5, "re-reading a file keeps the user's rating");

            check (db.recordPlay (batch[0].id, 1000) && db.recordPlay (batch[0].id, 2000), "records plays");
            const auto history = db.getHistory (10);
            check (history.size() == 2 && history[0].playedAt == 2000, "history is newest first");
            check (db.checkIntegrity().wasOk(), "integrity check passes");
        }
        {
            LibraryDatabase db;
            db.open (dbFile);
            const auto tracks = db.loadAllTracks();
            check (tracks.size() == 2 && tracks[0].playCount == 2 && tracks[0].rating == 5,
                   "data survives closing and reopening");
        }
        {
            LibraryDatabase db;
            db.open (dbFile);
            const auto backup = root.getChildFile ("db/backup.db");
            check (db.backupTo (backup) && backup.existsAsFile(), "online backup creates a file");
            LibraryDatabase restored;
            check (restored.open (backup).wasOk() && restored.loadAllTracks().size() == 2, "backup opens and contains the data");
        }
    }

    std::cout << "Database recovery" << std::endl;
    {
        const auto dbFile = root.getChildFile("broken/library.db");
        dbFile.getParentDirectory().createDirectory();
        dbFile.replaceWithText (juce::String::repeatedString ("this is definitely not a sqlite file. ", 400));

        LibraryDatabase db;
        const auto result = db.open (dbFile);
        check (result.wasOk(), "a corrupt database file is replaced instead of failing", result.getErrorMessage());
        check (db.getRecoveryNote().isNotEmpty(), "recovery is reported to the user");
        check (dbFile.getParentDirectory().findChildFiles (juce::File::findFiles, false, "*.corrupt-*").size() == 1,
               "the damaged file is kept for inspection");
        std::vector<TrackRecord> t { synthetic (1) };
        check (db.upsertTracks (t), "recovered database is usable");
    }

    //==========================================================================
    std::cout << "Tags and metadata" << std::endl;
    {
        const auto flac = root.getChildFile ("tags/tagged.flac");
        juce::FlacAudioFormat flacFormat;
        writeAudio (flac, flacFormat, 1.0, 16, 330.0);

        TrackRecord source;
        source.title = "Midnight Run";
        source.remix = "Club Remix";
        source.artist = "Test Artist";
        source.album = "Test Album";
        source.genre = "House";
        source.comment = "hello";
        source.year = 2021;
        source.bpm = 128.0;
        source.key = "8A";
        check (TrackMetadata::writeTags (flac, source), "writes tags into a FLAC file");

        TrackRecord read;
        check (TrackMetadata::read (flac, read), "reads the file back");
        check (read.title == "Midnight Run" && read.remix == "Club Remix", "title and remix are split", read.title + " | " + read.remix);
        check (read.artist == "Test Artist" && read.album == "Test Album" && read.genre == "House", "artist, album, genre");
        check (read.year == 2021 && read.comment == "hello", "year and comment");
        check (read.bpm == 128.0 && read.key == "8A", "BPM and key tags", juce::String (read.bpm) + " " + read.key);
        check (std::abs (read.lengthSeconds - 1.0) < 0.05 && read.format == "FLAC", "length and format",
               juce::String (read.lengthSeconds, 3));

        const auto wav = root.getChildFile ("tags/Some Artist - Some Title.wav");
        writeWav (wav, 0.5);
        TrackRecord wavRecord;
        check (TrackMetadata::read (wav, wavRecord) && wavRecord.artist == "Some Artist" && wavRecord.title == "Some Title",
               "untagged file falls back to 'Artist - Title' from the file name");

        const auto junk = root.getChildFile ("tags/junk.mp3");
        junk.replaceWithText ("not audio at all");
        TrackRecord junkRecord;
        check (! TrackMetadata::read (junk, junkRecord), "an unreadable file is rejected without crashing");
        check (! TrackMetadata::readCoverArt (wav).isValid(), "no cover art in a file without one");

        juce::String t = "Track (Radio Edit)", r;
        TrackMetadata::splitRemix (t, r);
        check (t == "Track" && r == "Radio Edit", "splits '(Radio Edit)'");
        t = "Track (feat. Someone)"; r.clear();
        TrackMetadata::splitRemix (t, r);
        check (t == "Track (feat. Someone)" && r.isEmpty(), "keeps '(feat. ...)' in the title");
    }

    //==========================================================================
    std::cout << "Scanner" << std::endl;
    {
        const auto music = root.getChildFile ("music");
        const int perFolder = 12;
        for (int folder = 0; folder < 3; ++folder)
            for (int i = 0; i < perFolder; ++i)
                writeWav (music.getChildFile ("folder" + juce::String (folder))
                               .getChildFile ("Artist" + juce::String (i % 4) + " - Song " + juce::String (folder * 100 + i) + ".wav"),
                          0.3 + 0.01 * i, 200.0 + 10.0 * i);
        music.getChildFile ("folder0").getChildFile ("notes.txt").replaceWithText ("not music");
        music.getChildFile ("folder0").getChildFile ("broken.mp3").replaceWithText ("garbage");
        const int total = perFolder * 3;

        LibraryManager lib;
        check (lib.open (root.getChildFile ("scan/library.db")).wasOk(), "library opens");
        check (lib.addFolderAndScan (music), "scan starts");
        check (waitForScan (lib), "scan finishes");

        auto s = lib.getLastScanSummary();
        check (s.added == total && lib.getTracks().size() == (size_t) total, "all audio files are imported",
               juce::String (s.added) + " added, " + juce::String ((int) lib.getTracks().size()) + " in library");
        check (s.failed == 1 && s.failedFiles[0].endsWith ("broken.mp3"), "the broken file is reported, the scan goes on");

        // Rescan without changes.
        lib.rescanAll();
        waitForScan (lib);
        s = lib.getLastScanSummary();
        check (s.added == 0 && s.updated == 0 && s.unchanged == total, "rescan skips unchanged files",
               juce::String (s.unchanged) + " unchanged");

        // Rate a track, then move its file: the track must be relinked, not duplicated.
        auto* victim = lib.findTrackByPath (music.getChildFile ("folder1/Artist1 - Song 101.wav").getFullPathName());
        check (victim != nullptr, "track is found by path");
        const auto victimId = victim != nullptr ? victim->id : 0;
        lib.setRating (victimId, 4);
        lib.recordPlay (victimId);
        const auto pid = lib.createPlaylist ("My Set");
        lib.addToPlaylist (pid, { victimId });

        const auto moved = music.getChildFile ("folder2/moved/Artist1 - Song 101.wav");
        moved.getParentDirectory().createDirectory();
        check (music.getChildFile ("folder1/Artist1 - Song 101.wav").moveFileTo (moved), "file is moved on disk");
        lib.rescanAll();
        waitForScan (lib);
        s = lib.getLastScanSummary();
        const auto* relinked = lib.findTrack (victimId);
        check (s.relinked == 1 && s.added == 0 && lib.getTracks().size() == (size_t) total, "moved file is relinked, not duplicated");
        check (relinked != nullptr && relinked->path == moved.getFullPathName() && ! relinked->missing, "new path is stored");
        check (relinked != nullptr && relinked->rating == 4 && relinked->playCount == 1, "rating and play count survive the move");
        check (lib.getDatabase().getPlaylistTrackIds (pid).size() == 1, "playlist membership survives the move");

        // Deleted file -> missing, restored file -> not missing.
        const auto gone = music.getChildFile ("folder2/Artist0 - Song 200.wav");
        const auto backupCopy = root.getChildFile ("gone-backup.wav");
        gone.copyFileTo (backupCopy);
        gone.deleteFile();
        lib.rescanAll();
        waitForScan (lib);
        s = lib.getLastScanSummary();
        const auto* missing = lib.findTrackByPath (gone.getFullPathName());
        check (s.markedMissing == 1 && missing != nullptr && missing->missing, "deleted file is flagged as missing");
        QuerySpec onlyMissing;
        onlyMissing.onlyMissing = true;
        check (lib.query (onlyMissing).size() == 1, "missing-files view lists it");

        backupCopy.copyFileTo (gone);
        gone.setLastModificationTime (juce::Time (missing != nullptr ? missing->modified : 0));
        lib.rescanAll();
        waitForScan (lib);
        s = lib.getLastScanSummary();
        const auto* back = lib.findTrackByPath (gone.getFullPathName());
        check (s.restored + s.updated >= 1 && back != nullptr && ! back->missing, "file that comes back is no longer missing");

        // A changed file is re-read.
        const auto changed = music.getChildFile ("folder0/Artist0 - Song 0.wav");
        TrackRecord edit;
        TrackMetadata::read (changed, edit);
        edit.title = "Edited Outside";
        edit.artist = "Someone Else";
        TrackMetadata::writeTags (changed, edit);
        changed.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (30));
        lib.rescanAll();
        waitForScan (lib);
        const auto* reread = lib.findTrackByPath (changed.getFullPathName());
        check (reread != nullptr && reread->title == "Edited Outside" && reread->artist == "Someone Else",
               "externally changed tags are picked up");

        // Duplicates: copy a file under another name.
        const auto dupSource = music.getChildFile ("folder0/Artist1 - Song 1.wav");
        dupSource.copyFileTo (music.getChildFile ("folder0/copy of song 1.wav"));
        lib.rescanAll();
        waitForScan (lib);
        // The copy has no tags, so the name fallback gives it a different title: tag it identically.
        TrackRecord orig, copy;
        TrackMetadata::read (dupSource, orig);
        const auto copyFile = music.getChildFile ("folder0/copy of song 1.wav");
        TrackMetadata::read (copyFile, copy);
        copy.title = orig.title; copy.artist = orig.artist;
        TrackMetadata::writeTags (copyFile, copy);
        copyFile.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (60));
        lib.rescanAll();
        waitForScan (lib);
        const auto groups = lib.findDuplicates();
        check (groups.size() == 1 && groups[0].size() == 2, "duplicate recordings are grouped", juce::String ((int) groups.size()) + " groups");
    }

    //==========================================================================
    std::cout << "Queries, playlists, smart playlists" << std::endl;
    {
        LibraryManager lib;
        lib.open (root.getChildFile ("queries/library.db"));
        std::vector<TrackRecord> batch;
        for (int i = 0; i < 200; ++i)
            batch.push_back (synthetic (i));
        batch[7].title = "Deep Blue Sea";   batch[7].artist = "Alpha"; batch[7].bpm = 124; batch[7].key = "8A";
        batch[8].title = "Deep Purple Haze"; batch[8].artist = "Bravo"; batch[8].bpm = 98;  batch[8].key = "10B";
        batch[9].title = "Shallow";         batch[9].artist = "Alpha"; batch[9].bpm = 128; batch[9].key = "9A";
        lib.getDatabase().upsertTracks (batch);
        lib.reload();

        QuerySpec spec;
        spec.searchText = "deep";
        check (lib.query (spec).size() == 2, "search finds words in titles");
        spec.searchText = "deep alpha";
        check (lib.query (spec).size() == 1, "all search words must match");
        spec.searchText = "DEEP   ALPHA";
        check (lib.query (spec).size() == 1, "search ignores case and extra spaces");
        spec.searchText = "10b";
        check (lib.query (spec).size() >= 1, "search also matches the key");

        QuerySpec bpmSpec;
        bpmSpec.minBpm = 120; bpmSpec.maxBpm = 130;
        bpmSpec.sortColumn = TrackColumn::bpm;
        const auto bpmHits = lib.query (bpmSpec);
        bool sorted = true;
        for (size_t i = 1; i < bpmHits.size(); ++i)
            sorted = sorted && lib.getTracks()[(size_t) bpmHits[i - 1]].bpm <= lib.getTracks()[(size_t) bpmHits[i]].bpm;
        check (! bpmHits.empty() && sorted, "BPM range filter and ascending sort");
        bpmSpec.ascending = false;
        check (lib.getTracks()[(size_t) lib.query (bpmSpec).front()].bpm >= lib.getTracks()[(size_t) lib.query (bpmSpec).back()].bpm,
               "descending sort");

        check (compareKeys ("2A", "10A") < 0 && compareKeys ("8A", "8B") < 0 && compareKeys ("9B", "10A") < 0,
               "keys sort in Camelot order, not alphabetically");

        // Playlists
        const auto a = lib.getTracks()[7].id, b = lib.getTracks()[8].id, c = lib.getTracks()[9].id;
        const auto pid = lib.createPlaylist ("Warm-up");
        check (pid > 0 && lib.addToPlaylist (pid, { a, b, c }), "creates a playlist and adds tracks");
        PlaylistInfo info;
        for (const auto& p : lib.getPlaylists()) if (p.id == pid) info = p;
        auto order = [&]
        {
            std::vector<juce::int64> ids;
            for (int i : lib.queryPlaylist (info, {})) ids.push_back (lib.getTracks()[(size_t) i].id);
            return ids;
        };
        check (order() == std::vector<juce::int64> { a, b, c }, "playlist keeps insertion order");
        lib.movePlaylistItem (pid, 2, 0);
        check (order() == std::vector<juce::int64> { c, a, b }, "moving an entry reorders the playlist");
        lib.removeFromPlaylist (pid, { a });
        check (order() == std::vector<juce::int64> { c, b }, "removing an entry");
        lib.removeTracks ({ b });
        check (order() == std::vector<juce::int64> { c }, "deleting a track removes it from playlists");

        // Smart playlist
        SmartRules rules;
        rules.conditions.push_back ({ "bpm", ">=", "120" });
        rules.conditions.push_back ({ "artist", "is", "Alpha" });
        const auto smartId = lib.createSmartPlaylist ("Fast Alpha", rules);
        PlaylistInfo smart;
        for (const auto& p : lib.getPlaylists()) if (p.id == smartId) smart = p;
        const auto smartHits = lib.queryPlaylist (smart, {});
        bool allMatch = ! smartHits.empty();
        for (int i : smartHits)
            allMatch = allMatch && lib.getTracks()[(size_t) i].bpm >= 120 && lib.getTracks()[(size_t) i].artist == "Alpha";
        int expected = 0;
        for (const auto& t : lib.getTracks()) if (t.bpm >= 120 && t.artist == "Alpha") ++expected;
        check (allMatch && (int) smartHits.size() == expected, "smart playlist applies all conditions", juce::String (expected) + " expected");
        rules.matchAll = false;
        check (SmartRules::fromJson (rules.toJson()).conditions.size() == 2 && ! SmartRules::fromJson (rules.toJson()).matchAll,
               "smart rules survive JSON round trip");

        // Favorites / recently played / history
        lib.setRating (c, 5);
        QuerySpec fav; fav.minRating = 4;
        check (lib.query (fav).size() == 1, "favorites view (rating >= 4)");
        lib.recordPlay (c);
        QuerySpec played; played.onlyPlayed = true; played.sortColumn = TrackColumn::lastPlayed; played.ascending = false;
        check (lib.query (played).size() == 1 && lib.queryHistory (10).size() == 1, "recently played and history views");
    }

    //==========================================================================
    std::cout << "Performance with 50,000 tracks" << std::endl;
    {
        LibraryManager lib;
        lib.open (root.getChildFile ("perf/library.db"));

        const int n = 50000;
        auto t0 = nowMs();
        for (int start = 0; start < n; start += 1000)
        {
            std::vector<TrackRecord> batch;
            for (int i = start; i < start + 1000; ++i)
                batch.push_back (synthetic (i));
            lib.getDatabase().upsertTracks (batch);
        }
        const auto insertMs = nowMs() - t0;

        t0 = nowMs();
        lib.reload();
        const auto loadMs = nowMs() - t0;
        check (lib.getTracks().size() == (size_t) n, "50,000 tracks stored and loaded");
        check (loadMs < 2000.0, "loading into the cache is fast", juce::String (loadMs, 0) + " ms (insert took " + juce::String (insertMs, 0) + " ms)");

        QuerySpec spec;
        spec.searchText = "golf song 4";
        t0 = nowMs();
        const auto hits = lib.query (spec);
        const auto searchMs = nowMs() - t0;
        check (! hits.empty() && searchMs < 300.0, "live search over 50,000 tracks", juce::String (searchMs, 0) + " ms, " + juce::String ((int) hits.size()) + " hits");

        QuerySpec sort;
        for (auto column : { TrackColumn::title, TrackColumn::bpm, TrackColumn::key, TrackColumn::length })
        {
            sort.sortColumn = column;
            t0 = nowMs();
            const auto sorted = lib.query (sort);
            const auto sortMs = nowMs() - t0;
            check (sorted.size() == (size_t) n && sortMs < 1500.0, "sort by " + columnDisplayName (column), juce::String (sortMs, 0) + " ms");
        }

        t0 = nowMs();
        const auto dupes = lib.findDuplicates();
        check (nowMs() - t0 < 2000.0, "duplicate detection", juce::String (nowMs() - t0, 0) + " ms, " + juce::String ((int) dupes.size()) + " groups");
    }

    root.deleteRecursively();
    return 0;
}
