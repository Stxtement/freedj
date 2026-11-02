// Tests for: sync tempo math, genre taxonomy, key handling, Smart Matching rules, genre storage, model download checks.

#include "TestSupport.h"
#include "Decks/SyncMath.h"
#include "Genre/GenreTaxonomy.h"
#include "Library/LibraryDatabase.h"
#include "Library/SmartMatch.h"
#include "Stems/ModelDownload.h"
#include <juce_events/juce_events.h>

using namespace pathboard;
using testsupport::check;

namespace
{
TrackRecord track (double bpm, const juce::String& key, const juce::String& genreText = {})
{
    TrackRecord t;
    t.title = "t";
    t.bpm = bpm;
    t.key = key;
    t.genre = genreText;
    return t;
}

MatchLevel level (const TrackRecord& ref, const TrackRecord& cand)
{
    return evaluateMatch (makeSubject (ref), makeSubject (cand)).level;
}

void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }
}

int runMatchTests()
{
    //==========================================================================
    std::cout << "Sync tempo (original vs playback BPM)" << std::endl;
    {
        // The case from the bug report: a 95.5 BPM track synced to a deck playing 122 BPM.
        const auto far = planSync (95.5, 122.0, 16.0);
        check (far.possible && std::abs (far.percent - 27.75) < 0.05 && ! far.withinLimit, "95.5 -> 122 BPM needs +27.7 % and is refused (limit 16 %)",
               juce::String (far.percent, 2) + " %");

        const auto half = planSync (95.5, 191.0, 16.0);
        check (half.withinLimit && std::abs (half.percent) < 1.0e-9 && half.targetFactor == 0.5, "95.5 -> 191 BPM matches in half time with 0 % pitch");
        const auto dbl = planSync (190.0, 95.0, 16.0);
        check (dbl.withinLimit && std::abs (dbl.percent) < 1.0e-9 && dbl.targetFactor == 2.0, "190 -> 95 BPM matches in double time with 0 % pitch");
        const auto normal = planSync (120.0, 128.0, 16.0);
        check (normal.withinLimit && std::abs (normal.percent - 6.6667) < 1.0e-3 && normal.targetFactor == 1.0, "120 -> 128 BPM is +6.67 % at normal time");
        check (! planSync (0.0, 128.0, 16.0).possible && ! planSync (120.0, 0.0, 16.0).possible, "unknown tempo makes sync impossible");
    }

    //==========================================================================
    std::cout << "Genre taxonomy" << std::endl;
    {
        using namespace genre;
        auto l = fromDiscogs ("Electronic---Tech House");
        check (l.main == "House" && l.sub == "Tech House" && l.category == "Electronic", "Tech House belongs to House");
        l = fromDiscogs ("Electronic---Techno");
        check (l.main == "Techno" && l.sub.isEmpty(), "Techno is its own main genre");
        l = fromDiscogs ("Rock---Alternative Rock");
        check (l.main == "Rock" && l.sub == "Alternative Rock", "Alternative Rock belongs to Rock");
        l = fromDiscogs ("Hip Hop---Gangsta");
        check (l.main == "Hip Hop" && l.sub == "Gangsta", "Gangsta belongs to Hip Hop");
        l = fromDiscogs ("Reggae---Ragga");
        check (l.main == "Reggae" && l.sub == "Ragga", "Ragga belongs to Reggae");
        l = fromDiscogs ("Electronic---Drum n Bass");
        check (l.main == "Drum & Bass", "Drum n Bass is written as Drum & Bass");

        auto parse = [] (const juce::String& s) { return parseGenreText (s); };
        check (parse ("Tech House").size() == 1 && parse ("Tech House")[0].main == "House" && parse ("Tech House")[0].sub == "Tech House", "'Tech House' is understood");
        check (parse ("house")[0].main == "House" && parse ("house")[0].sub.isEmpty(), "'house' is the main genre only");
        check (parse ("Hip-Hop")[0].main == "Hip Hop" && parse ("hiphop")[0].main == "Hip Hop", "Hip-Hop spellings");
        check (parse ("DnB")[0].main == "Drum & Bass" && parse ("drum and bass")[0].main == "Drum & Bass", "Drum & Bass spellings");
        check (parse ("Afro House")[0].main == "House" && parse ("Afro House")[0].sub == "Afro House", "'Afro House' (not in the Discogs vocabulary) is understood from tags");
        check (parse ("Melodic Techno")[0].main == "Techno" && parse ("Melodic Techno")[0].sub == "Melodic Techno", "'Melodic Techno' is understood from tags");
        check (parse ("House / Deep House").size() == 1 && parse ("House / Deep House")[0].sub == "Deep House", "'House / Deep House' collapses into one genre");
        check (parse ("Pop; Rock").size() == 2, "several genres are kept");
        check (parse ("").empty() && parse ("   ").empty(), "empty text has no genre");
        const auto unknown = parse ("Zydeco Fusion");
        check (unknown.size() == 1 && unknown[0].main == "Zydeco Fusion", "an unknown word is kept as written, not replaced");

        auto sim = [&] (const char* a, const char* b) { return genreSimilarity (parse (a), parse (b)); };
        check (sim ("Tech House", "Tech House") == 1.0, "same genre = 1.0");
        check (sim ("House", "Tech House") == 0.9, "main vs sub-genre of it = 0.9");
        check (sim ("Tech House", "Deep House") == 0.75, "two sub-genres of House = 0.75");
        check (sim ("House", "Techno") == 0.5 && sim ("Techno", "House") == 0.5, "House and Techno are related, symmetrically");
        check (sim ("House", "Rock") == 0.0, "House and Rock are unrelated");
        check (sim ("Pop", "Rock") == 0.4, "Pop and Rock are related");
        check (sim ("", "House") == -1.0 && sim ("House", "") == -1.0, "a missing genre is 'unknown', not 'unrelated'");
    }

    //==========================================================================
    std::cout << "Keys and tempo compatibility" << std::endl;
    {
        auto key = [] (const char* s) { return parseKey (s); };
        check (key ("8A")->text() == "8A" && key ("08a")->text() == "8A" && key ("12B")->text() == "12B", "Camelot notation");
        check (key ("Am")->text() == "8A" && key ("A minor")->text() == "8A" && key ("C")->text() == "8B" && key ("C major")->text() == "8B", "note names");
        check (key ("F#m")->text() == "11A" && key ("Gbm")->text() == "11A" && key ("Bb")->text() == "6B" && key ("Db")->text() == "3B", "sharps and flats");
        check (! key ("").has_value() && ! key ("xyz").has_value() && ! key ("13A").has_value() && ! key ("0A").has_value() && ! key ("H").has_value(),
               "garbage is not a key");

        const CamelotKey k8a { 8, true };
        check (camelotCompatibility (k8a, { 8, true }) == 1.0 && camelotCompatibility (k8a, { 8, false }) == 0.9
                   && camelotCompatibility (k8a, { 9, true }) == 0.9 && camelotCompatibility (k8a, { 7, true }) == 0.9
                   && camelotCompatibility (k8a, { 10, true }) == 0.5 && camelotCompatibility (k8a, { 3, true }) == 0.0
                   && camelotCompatibility (k8a, { 9, false }) == 0.0,
               "Camelot wheel: same, relative, neighbours, two steps, clashes");
        check (camelotCompatibility ({ 12, true }, { 1, true }) == 0.9 && camelotCompatibility ({ 1, true }, { 12, true }) == 0.9, "the wheel wraps around 12 -> 1");

        double diff = 0, factor = 0;
        check (bpmCompatibility (128, 126, &diff, &factor) == 1.0 && factor == 1.0, "128 vs 126 BPM is a perfect tempo match");
        check (bpmCompatibility (128, 64, &diff, &factor) == 0.95 && factor == 2.0, "128 vs 64 BPM matches in half/double time");
        check (bpmCompatibility (64, 128, &diff, &factor) == 0.95 && factor == 0.5, "64 vs 128 BPM matches too");
        check (bpmCompatibility (128, 135) == 0.7 && bpmCompatibility (128, 140) == 0.4 && bpmCompatibility (128, 160) == 0.0, "tempo steps: 5.5 % good, 9.4 % acceptable, 25 % none");
        check (bpmCompatibility (0, 128) == -1.0 && bpmCompatibility (128, 0) == -1.0, "unknown tempo stays unknown");
    }

    //==========================================================================
    std::cout << "Smart Matching rules" << std::endl;
    {
        const auto ref = track (128.0, "8A", "Tech House");
        check (level (ref, track (126.0, "8A", "Tech House")) == MatchLevel::veryGood, "same genre, key and tempo: very good");
        check (level (ref, track (127.0, "9A", "Deep House")) == MatchLevel::veryGood, "neighbouring key, sub-genre of the same family: very good");
        check (level (ref, track (130.0, "8B", "Techno")) == MatchLevel::good, "relative key, related genre: good");
        check (level (ref, track (128.0, "8A", "Rock")) == MatchLevel::good, "perfect tempo and key but another genre: good, not very good");
        check (level (ref, track (128.0, "3A", "Tech House")) == MatchLevel::none, "a clashing key rules a track out");
        check (level (ref, track (150.0, "8A", "Tech House")) == MatchLevel::none, "tempo too far apart: none");
        check (level (ref, track (64.0, "8A", "Tech House")) == MatchLevel::veryGood, "half-time tempo counts");
        check (level (ref, track (126.0, "", "Tech House")) == MatchLevel::good, "unknown key: at best good");
        check (level (ref, track (126.0, "8A", "")) == MatchLevel::veryGood, "unknown genre does not count against a track");
        check (level (ref, track (0.0, "8A", "Tech House")) == MatchLevel::none, "unknown tempo: no match");
        check (level (ref, track (126.0, "", "")) == MatchLevel::none, "tempo alone is not enough evidence");
        check (level (ref, track (140.0, "8A", "Tech House")) == MatchLevel::good, "9 % tempo difference with key and genre: good");
        check (level (ref, track (140.0, "10A", "Rock")) == MatchLevel::none, "weak tempo, weak key, wrong genre: none");

        // Reliability: automatic results with low confidence are treated as unknown.
        TrackRecord shaky = track (128.0, "8A", "Tech House");
        shaky.analysisVersion = 1; shaky.bpmConfidence = 0.2; shaky.keyConfidence = 0.9;
        check (makeSubject (shaky).bpm == 0.0 && level (ref, shaky) == MatchLevel::none, "an unreliable analysed BPM is not used");
        shaky.bpmConfidence = 0.9; shaky.keyConfidence = 0.1;
        check (! makeSubject (shaky).key.has_value() && level (ref, shaky) == MatchLevel::good, "an unreliable analysed key is not used (genre keeps it good)");
        shaky.bpmConfidence = 0.1; shaky.manualFlags = TrackRecord::manualBpm;
        check (makeSubject (shaky).bpm == 128.0, "a BPM corrected by hand is always used");
        TrackRecord tagged = track (128.0, "8A", "Tech House");   // analysisVersion 0: values from the file's own tags
        check (makeSubject (tagged).bpm == 128.0 && makeSubject (tagged).key.has_value(), "values from the file's tags are used");

        // Genre sources: the user's tag wins; a confident automatic result is used when there is no tag.
        TrackRecord autoGenre = track (128.0, "8A");
        autoGenre.genreAutoMain = "House"; autoGenre.genreAutoSub = "Tech House"; autoGenre.genreAutoCategory = "Electronic"; autoGenre.genreConfidence = 0.6;
        check (makeSubject (autoGenre).genres.size() == 1 && makeSubject (autoGenre).genres[0].sub == "Tech House", "a confident automatic genre is used when there is no tag");
        autoGenre.genreConfidence = 0.1;
        check (makeSubject (autoGenre).genres.empty(), "an uncertain automatic genre is not used");
        autoGenre.genreConfidence = 0.9;
        autoGenre.genre = "Techno";
        check (makeSubject (autoGenre).genres[0].main == "Techno", "the user's genre beats the automatic one");

        // The audible tempo of a pitched deck replaces the original tempo.
        check (makeSubject (ref, 133.0).bpm == 133.0 && makeSubject (ref).bpm == 128.0, "the playback tempo can replace the original tempo");

        const auto detail = evaluateMatch (makeSubject (ref), makeSubject (track (64.0, "9A", "Deep House")));
        check (detail.explanation.contains ("half") || detail.explanation.contains ("double"), "the explanation names half/double time", detail.explanation);
        check (detail.explanation.contains ("9A") && detail.explanation.contains ("8A"), "and both keys", detail.explanation);
    }

    //==========================================================================
    std::cout << "Genre storage (schema 3)" << std::endl;
    {
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJGenreDb");
        root.deleteRecursively();
        LibraryDatabase db;
        check (db.open (root.getChildFile ("library.db")).wasOk(), "database opens (schema " + juce::String (LibraryDatabase::schemaVersion) + ")");

        std::vector<TrackRecord> batch (1);
        batch[0].path = "C:\\music\\x.mp3"; batch[0].title = "x"; batch[0].genre = "My Own Genre"; batch[0].lengthSeconds = 200; batch[0].fileSize = 10; batch[0].modified = 100;
        db.upsertTracks (batch);
        const auto id = batch[0].id;

        LibraryDatabase::GenreUpdate g;
        g.main = "House"; g.sub = "Tech House"; g.category = "Electronic"; g.confidence = 0.63; g.alternativesJson = "[{\"style\":\"Electronic---Techno\",\"p\":0.3}]"; g.version = 1;
        check (db.saveGenre (id, g), "a detected genre is saved");
        TrackRecord r;
        db.loadTrack (id, r);
        check (r.genreAutoMain == "House" && r.genreAutoSub == "Tech House" && r.genreAutoCategory == "Electronic" && r.genreConfidence == 0.63
                   && r.genreVersion == 1 && r.genreAlternatives.contains ("Techno"),
               "all detection fields are read back");
        check (r.genre == "My Own Genre" && r.displayGenre() == "My Own Genre" && ! r.isGenreAutomatic(), "the user's own genre is untouched and is shown");

        std::vector<TrackRecord> again (1);
        again[0] = batch[0]; again[0].genre = "Tag From File";
        db.upsertTracks (again);
        db.loadTrack (id, r);
        check (r.genreVersion == 1 && r.genreAutoMain == "House", "re-reading an unchanged file keeps the detected genre");

        again[0].modified = 200;
        db.upsertTracks (again);
        db.loadTrack (id, r);
        check (r.genreVersion == 0, "a changed file is queued for genre detection again");

        db.saveGenre (id, g);
        db.requestReanalysis ({ id });
        db.loadTrack (id, r);
        check (r.genreVersion == 0 && r.analysisVersion == 0, "'analyse again' also repeats the genre detection");
        db.markGenreFailed (id);
        db.loadTrack (id, r);
        check (r.genreVersion == -1, "a failed detection is recorded");

        TrackRecord plain;
        plain.genreAutoMain = "Rock"; plain.genreAutoSub = "Alternative Rock"; plain.genreConfidence = 0.4;
        check (plain.displayGenre() == "Rock / Alternative Rock" && plain.isGenreAutomatic(), "without a tag the confident automatic genre is displayed");
        plain.genreConfidence = 0.1;
        check (plain.displayGenre().isEmpty(), "an uncertain automatic genre is not displayed");
        root.deleteRecursively();
    }

    //==========================================================================
    std::cout << "Model download checks" << std::endl;
    {
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJDownloadTest");
        root.deleteRecursively();
        root.createDirectory();
        const auto source = root.getChildFile ("source.bin");
        juce::MemoryBlock data;
        juce::Random rng (5);
        for (int i = 0; i < 3000000; ++i)
        {
            const juce::uint8 b = (juce::uint8) rng.nextInt (256);
            data.append (&b, 1);
        }
        source.replaceWithData (data.getData(), data.getSize());
        const auto sha = ModelDownload::sha256Of (source);
        const auto url = juce::URL (source).toString (true);

        auto runDownload = [&] (const juce::File& target, juce::int64 size, const juce::String& hash)
        {
            ModelDownload download;
            juce::String result = "pending";
            float lastProgress = 0.0f;
            download.start (url, target, size, hash, [&] (float p) { lastProgress = p; }, [&] (const juce::String& e) { result = e; });
            for (int i = 0; i < 500 && result == "pending"; ++i)
                pump (20);
            return std::make_pair (result, lastProgress);
        };

        const auto good = root.getChildFile ("good/model.onnx");
        auto [ok, progress] = runDownload (good, source.getSize(), sha);
        check (ok.isEmpty() && good.existsAsFile() && good.getSize() == source.getSize() && ModelDownload::sha256Of (good) == sha, "a correct file is downloaded and put in place", ok);
        check (progress > 0.99f, "progress reaches 100 %", juce::String (progress, 2));
        check (! good.getSiblingFile ("model.onnx.part").exists(), "no .part file is left behind");

        const auto badHash = root.getChildFile ("badhash/model.onnx");
        auto [e1, p1] = runDownload (badHash, source.getSize(), "00" + sha.substring (2));
        check (e1.contains ("checksum") && ! badHash.exists() && ! badHash.getSiblingFile ("model.onnx.part").exists(), "a wrong checksum is rejected and nothing is kept", e1);

        const auto badSize = root.getChildFile ("badsize/model.onnx");
        auto [e2, p2] = runDownload (badSize, source.getSize() + 5, sha);
        check (e2.contains ("wrong size") && ! badSize.exists(), "a wrong size is rejected", e2);

        const auto missing = root.getChildFile ("missing/model.onnx");
        ModelDownload d3;
        juce::String e3 = "pending";
        d3.start (juce::URL (root.getChildFile ("does-not-exist.bin")).toString (true), missing, 100, sha, {}, [&] (const juce::String& e) { e3 = e; });
        for (int i = 0; i < 200 && e3 == "pending"; ++i) pump (20);
        check (e3.isNotEmpty() && e3 != "pending" && ! missing.exists(), "an unreachable source is reported", e3);

        // An existing good file is only replaced by a successful download.
        const auto existing = root.getChildFile ("good/model.onnx");
        auto [e4, p4] = runDownload (existing, source.getSize(), "ff" + sha.substring (2));
        check (e4.isNotEmpty() && existing.existsAsFile() && ModelDownload::sha256Of (existing) == sha, "a failed download does not destroy the installed model");
        root.deleteRecursively();
    }

    return 0;
}
