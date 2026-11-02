// Accuracy tests for BPM, beatgrid, downbeat and key detection on synthetic audio with known ground truth.

#include "TestSupport.h"
#include "Analysis/AudioAnalysis.h"
#include "Analysis/AnalysisManager.h"
#include "Library/LibraryManager.h"
#include <sqlite3.h>
#include <juce_events/juce_events.h>
#include <juce_audio_formats/juce_audio_formats.h>

using namespace pathboard::analysis;
using namespace pathboard;
using testsupport::check;

namespace
{
constexpr double sr = 22050.0;

// Drum-like pattern: kick on every beat (accented on the bar start), hi-hat on the off-beats, optional noise bed.
// tempoAt(t) gives the tempo in BPM at time t, so ramps are possible.
MonoAudio makeDrumTrack (std::function<double (double)> tempoAt, double seconds, double firstBeat, bool accentBars,
                         double hatLevel = 0.15, int seed = 1)
{
    MonoAudio a;
    a.sampleRate = sr;
    a.durationSeconds = seconds;
    a.samples.assign ((size_t) (seconds * sr), 0.0f);
    juce::Random rng (seed);

    auto addKick = [&] (double t, float amp)
    {
        const int start = (int) (t * sr);
        for (int i = 0; i < (int) (0.12 * sr) && start + i < (int) a.samples.size(); ++i)
        {
            const double x = (double) i / sr;
            const double freq = 50.0 + 90.0 * std::exp (-x * 40.0);
            a.samples[(size_t) (start + i)] += amp * (float) (std::sin (juce::MathConstants<double>::twoPi * freq * x) * std::exp (-x * 28.0));
        }
    };
    auto addHat = [&] (double t, float amp)
    {
        const int start = (int) (t * sr);
        for (int i = 0; i < (int) (0.03 * sr) && start + i < (int) a.samples.size(); ++i)
            a.samples[(size_t) (start + i)] += amp * (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-(double) i / sr * 120.0);
    };

    double t = firstBeat;
    int beat = 0;
    while (t < seconds - 0.2)
    {
        const double period = 60.0 / tempoAt (t);
        const bool bar = accentBars && beat % 4 == 0;
        addKick (t, bar ? 0.9f : 0.55f);
        addHat (t + period * 0.5, (float) hatLevel);
        t += period;
        ++beat;
    }
    return a;
}

MonoAudio constantTempo (double bpm, double seconds, double firstBeat, bool accent = true, int seed = 1)
{
    return makeDrumTrack ([bpm] (double) { return bpm; }, seconds, firstBeat, accent, 0.15, seed);
}

// Adds a chord with 4 decaying harmonics.
void addNote (MonoAudio& a, int midi, double start, double length, float amp)
{
    const double f0 = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    const int s0 = (int) (start * sr), count = (int) (length * sr);
    for (int i = 0; i < count && s0 + i < (int) a.samples.size(); ++i)
    {
        const double x = (double) i / sr;
        const double env = std::min (1.0, x * 50.0) * std::exp (-x * 1.2);
        double v = 0.0;
        for (int h = 1; h <= 4; ++h)
            if (f0 * h < sr * 0.45)
                v += std::sin (juce::MathConstants<double>::twoPi * f0 * h * x) / h;
        a.samples[(size_t) (s0 + i)] += amp * (float) (v * env);
    }
}

MonoAudio makeProgression (int tonic, bool minor, double seconds)
{
    MonoAudio a;
    a.sampleRate = sr;
    a.durationSeconds = seconds;
    a.samples.assign ((size_t) (seconds * sr), 0.0f);

    // I - IV - V - I (i - iv - V - i in minor), root in octave 3, triad above.
    const int degrees[4] = { 0, 5, 7, 0 };
    const bool chordMinor[4] = { minor, minor, false, minor };
    for (double t = 0.0, chordLength = 1.5; t < seconds - chordLength; t += chordLength)
    {
        const int idx = (int) (t / chordLength) % 4;
        const int root = 48 + tonic + degrees[idx];
        addNote (a, root, t, chordLength, 0.2f);
        addNote (a, root + (chordMinor[idx] ? 3 : 4), t, chordLength, 0.15f);
        addNote (a, root + 7, t, chordLength, 0.15f);
        addNote (a, root - 12, t, chordLength, 0.12f);
    }
    return a;
}
}

namespace
{
void writeStereoWav (const juce::File& file, const MonoAudio& track)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
    juce::AudioBuffer<float> buffer (2, (int) track.samples.size());
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        buffer.setSample (0, i, track.samples[(size_t) i]);
        buffer.setSample (1, i, track.samples[(size_t) i]);
    }
    writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

void pumpMessages (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

bool waitFor (std::function<bool()> done, int timeoutMs = 60000)
{
    const double start = juce::Time::getMillisecondCounterHiRes();
    while (! done() && juce::Time::getMillisecondCounterHiRes() - start < timeoutMs)
        pumpMessages (20);
    pumpMessages (100);
    return done();
}
}

int runAnalysisStorageTests()
{
    const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJAnalysisStorage");
    root.deleteRecursively();
    root.createDirectory();

    //==========================================================================
    std::cout << "Database upgrade from schema 1" << std::endl;
    {
        const auto dbFile = root.getChildFile ("old/library.db");
        dbFile.getParentDirectory().createDirectory();
        sqlite3* raw = nullptr;
        sqlite3_open (dbFile.getFullPathName().toRawUTF8(), &raw);
        const char* v1 = "CREATE TABLE tracks (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, title TEXT NOT NULL DEFAULT '', artist TEXT NOT NULL DEFAULT '',"
                         "remix TEXT NOT NULL DEFAULT '', album TEXT NOT NULL DEFAULT '', genre TEXT NOT NULL DEFAULT '', comment TEXT NOT NULL DEFAULT '',"
                         "tags TEXT NOT NULL DEFAULT '', key TEXT NOT NULL DEFAULT '', format TEXT NOT NULL DEFAULT '', year INTEGER NOT NULL DEFAULT 0,"
                         "rating INTEGER NOT NULL DEFAULT 0, bitrate INTEGER NOT NULL DEFAULT 0, play_count INTEGER NOT NULL DEFAULT 0, bpm REAL NOT NULL DEFAULT 0,"
                         "length REAL NOT NULL DEFAULT 0, size INTEGER NOT NULL DEFAULT 0, modified INTEGER NOT NULL DEFAULT 0, first_seen INTEGER NOT NULL DEFAULT 0,"
                         "last_played INTEGER NOT NULL DEFAULT 0, missing INTEGER NOT NULL DEFAULT 0, analysed INTEGER NOT NULL DEFAULT 0);"
                         "CREATE TABLE folders (path TEXT PRIMARY KEY);"
                         "CREATE TABLE playlists (id INTEGER PRIMARY KEY, name TEXT NOT NULL, kind INTEGER NOT NULL DEFAULT 0, rules TEXT NOT NULL DEFAULT '');"
                         "CREATE TABLE playlist_items (id INTEGER PRIMARY KEY AUTOINCREMENT, playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
                         "track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE, position INTEGER NOT NULL);"
                         "CREATE TABLE history (id INTEGER PRIMARY KEY AUTOINCREMENT, track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE, played_at INTEGER NOT NULL);"
                         "INSERT INTO tracks (path,title,artist,bpm,rating,play_count) VALUES ('C:\\\\old.mp3','Old Song','Old Artist',99,4,7);"
                         "PRAGMA user_version=1;";
        sqlite3_exec (raw, v1, nullptr, nullptr, nullptr);
        sqlite3_close (raw);

        LibraryDatabase db;
        check (db.open (dbFile).wasOk(), "a schema-1 database opens and is upgraded");
        const auto tracks = db.loadAllTracks();
        check (tracks.size() == 1 && tracks[0].title == "Old Song" && tracks[0].bpm == 99.0 && tracks[0].rating == 4 && tracks[0].playCount == 7,
               "existing tracks keep all their data");
        check (tracks[0].analysisVersion == 0 && tracks[0].manualFlags == 0 && tracks[0].fingerprint.isEmpty(), "new columns start empty");
        db.close();
        LibraryDatabase again;
        check (again.open (dbFile).wasOk() && again.loadAllTracks().size() == 1, "upgraded database reopens");
    }

    //==========================================================================
    std::cout << "Analysis results and manual corrections" << std::endl;
    {
        LibraryDatabase db;
        db.open (root.getChildFile ("manual/library.db"));

        auto make = [] (const juce::String& name, double tagBpm)
        {
            TrackRecord t;
            t.path = "C:\\music\\" + name + ".mp3"; t.title = name; t.bpm = tagBpm; t.key = ""; t.genre = "Tag Genre";
            t.lengthSeconds = 200; t.fileSize = 1000; t.modified = 5000; t.firstSeen = 1;
            return t;
        };
        std::vector<TrackRecord> batch { make ("a", 0.0), make ("b", 100.0), make ("c", 0.0) };
        db.upsertTracks (batch);
        const auto a = batch[0].id, b = batch[1].id, c = batch[2].id;

        LibraryDatabase::AnalysisUpdate u;
        u.version = 1; u.analysedAt = 123; u.fingerprint = "abc"; u.bpm = 128.37; u.bpmConfidence = 0.9; u.firstBeat = 0.25;
        u.downbeat = 2; u.key = "8A"; u.keyConfidence = 0.7; u.energy = 8.0; u.loudnessDb = -10.0;
        check (db.saveAnalysis (a, u), "analysis result is stored");
        TrackRecord r;
        db.loadTrack (a, r);
        check (r.bpm == 128.37 && r.key == "8A" && r.firstBeat == 0.25 && r.downbeat == 2 && r.analysisVersion == 1
                   && r.fingerprint == "abc" && r.bpmConfidence == 0.9 && r.energy == 8.0 && r.analysed,
               "all analysis fields are saved and read back");

        // Manual BPM edit through the info panel path.
        r.bpm = 130.0;
        db.updateTrackInfo (r);
        db.loadTrack (a, r);
        check (r.isManual (TrackRecord::manualBpm) && ! r.isManual (TrackRecord::manualKey), "editing the BPM marks it as manual");
        u.bpm = 99.0; u.key = "3B"; u.firstBeat = 0.5;
        db.saveAnalysis (a, u);
        db.loadTrack (a, r);
        check (r.bpm == 130.0 && r.key == "3B", "a new analysis keeps the manual BPM but updates the automatic key", juce::String (r.bpm) + " " + r.key);

        r.title = "renamed";
        db.updateTrackInfo (r);
        db.loadTrack (a, r);
        check (! r.isManual (TrackRecord::manualKey) && r.isManual (TrackRecord::manualBpm), "editing only the title sets no extra manual flag");

        r.genre = "Techno";
        db.updateTrackInfo (r);
        std::vector<TrackRecord> rescanned { make ("a", 111.0) };
        rescanned[0].path = batch[0].path;
        rescanned[0].modified = 9000;
        db.upsertTracks (rescanned);
        db.loadTrack (a, r);
        check (r.genre == "Techno" && r.bpm == 130.0, "re-reading the file does not overwrite a manual genre or BPM");
        check (r.analysisVersion == 0, "a changed file is queued for re-analysis");

        // Only the timestamp changed (same size, same content fingerprint): the results stay.
        db.saveAnalysis (a, u);   // fingerprint "abc"
        std::vector<TrackRecord> touched { make ("a", 111.0) };
        touched[0].path = batch[0].path;
        touched[0].modified = 12000;
        touched[0].fingerprint = "abc";
        db.upsertTracks (touched);
        db.loadTrack (a, r);
        check (r.analysisVersion == 1 && r.modified == 12000, "a changed timestamp with the same fingerprint keeps the analysis");
        touched[0].modified = 13000;
        touched[0].fingerprint = "different";
        db.upsertTracks (touched);
        db.loadTrack (a, r);
        check (r.analysisVersion == 0, "a different fingerprint invalidates the analysis");

        // Results survive closing and reopening the database.
        db.saveAnalysis (a, u);
        db.close();
        LibraryDatabase reopened;
        reopened.open (root.getChildFile ("manual/library.db"));
        TrackRecord back;
        reopened.loadTrack (a, back);
        check (back.analysisVersion == 1 && back.fingerprint == "abc" && back.key == "3B" && back.isManual (TrackRecord::manualBpm),
               "analysis and manual flags are still there after reopening");
        reopened.close();
        db.open (root.getChildFile ("manual/library.db"));

        db.setManualGrid (c, 124.0, 0.33, 5);
        db.loadTrack (c, r);
        check (r.bpm == 124.0 && r.firstBeat == 0.33 && r.downbeat == 1 && r.isManual (TrackRecord::manualGrid) && r.isManual (TrackRecord::manualBpm),
               "manual beat grid is stored and flagged");
        u.bpm = 90.0; u.firstBeat = 0.9; u.downbeat = 3;
        db.saveAnalysis (c, u);
        db.loadTrack (c, r);
        check (r.bpm == 124.0 && r.firstBeat == 0.33 && r.downbeat == 1, "analysis does not touch a manual grid");

        // Weak result must not replace a BPM that is already known; strong one must.
        LibraryDatabase::AnalysisUpdate weak;
        weak.version = 1; weak.bpm = 77.0; weak.bpmConfidence = 0.2;
        db.saveAnalysis (b, weak);
        db.loadTrack (b, r);
        check (r.bpm == 100.0, "an unreliable result does not replace the tagged BPM", juce::String (r.bpm));
        weak.bpmConfidence = 0.9;
        db.saveAnalysis (b, weak);
        db.loadTrack (b, r);
        check (r.bpm == 77.0, "a confident result replaces the tagged BPM");
        LibraryDatabase::AnalysisUpdate nothing;
        nothing.version = 1;
        db.saveAnalysis (b, nothing);
        db.loadTrack (b, r);
        check (r.bpm == 77.0, "a result without tempo keeps the existing BPM");

        db.markAnalysisFailed (b, 5);
        db.loadTrack (b, r);
        check (r.analysisVersion == -1 && r.bpm == 77.0, "failed analysis is recorded without losing data");
        db.clearManualFlags (a, 0xff);
        db.loadTrack (a, r);
        check (r.manualFlags == 0, "manual flags can be reset");
    }

    //==========================================================================
    std::cout << "Background analysis end to end" << std::endl;
    {
        const auto music = root.getChildFile ("music");
        struct Spec { const char* name; double bpm; double offset; };
        const Spec specs[] = { { "one 120", 120.0, 0.10 }, { "two 128.5", 128.5, 0.20 }, { "three 140", 140.0, 0.05 } };
        for (const auto& s : specs)
            writeStereoWav (music.getChildFile (juce::String (s.name) + ".wav"), constantTempo (s.bpm, 40.0, s.offset));
        music.getChildFile ("broken.wav").replaceWithText ("not audio");

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const auto dbFile = root.getChildFile ("e2e/library.db");
        {
            LibraryManager lib;
            lib.open (dbFile);
            lib.addFolderAndScan (music);
            waitFor ([&] { return ! lib.isScanning(); });
            check (lib.getTracks().size() == 3, "3 playable files imported", juce::String ((int) lib.getTracks().size()));

            AnalysisManager manager (lib, formats);
            check (manager.enqueuePending() == 3, "all three tracks are queued");
            check (waitFor ([&] { return ! manager.isBusy(); }), "analysis finishes");

            bool allGood = true;
            juce::String detail;
            for (const auto& t : lib.getTracks())
            {
                for (const auto& s : specs)
                    if (t.title.startsWithIgnoreCase (juce::String (s.name).upToFirstOccurrenceOf (" ", false, false)))
                    {
                        const bool ok = std::abs (t.bpm - s.bpm) <= 0.05 && t.analysisVersion == analysisVersion && t.fingerprint.isNotEmpty()
                                        && t.bpmConfidence > 0.8 && t.analysedAt > 0 && t.energy >= 1.0;
                        allGood = allGood && ok;
                        detail << t.title << ": " << juce::String (t.bpm, 2) << " conf " << juce::String (t.bpmConfidence, 2) << "; ";
                    }
            }
            check (allGood && manager.getDone() == 3, "BPM, confidence, fingerprint and date are stored for each track", detail);
            check (manager.getPercent() == 100, "progress reaches 100%");
            check (manager.enqueuePending() == 0, "already analysed tracks are not queued again");

            // Manual correction survives a forced re-analysis.
            const auto victim = lib.getTracks()[0].id;
            lib.setManualGrid (victim, 100.0, 0.4, 1);
            manager.enqueue ({ victim }, true);
            waitFor ([&] { return ! manager.isBusy(); });
            const auto* v = lib.findTrack (victim);
            check (v != nullptr && v->bpm == 100.0 && v->firstBeat == 0.4 && v->analysisVersion == analysisVersion,
                   "forced re-analysis keeps the manual BPM and grid");
        }

        // Interrupted run: new session picks up exactly what is left.
        {
            LibraryManager lib;
            lib.open (dbFile);
            check (lib.getTracks().size() == 3 && lib.getTracks()[0].analysisVersion == analysisVersion, "results persist across sessions");
            lib.requestReanalysis ({ lib.getTracks()[1].id });
            AnalysisManager manager (lib, formats);
            check (manager.enqueuePending() == 1, "after an interruption only the unfinished track is queued");
            waitFor ([&] { return ! manager.isBusy(); });
            check (manager.getDone() == 1, "the unfinished track is completed");
        }

        // Unreadable audio is marked failed and not retried every time.
        {
            writeStereoWav (music.getChildFile ("short.wav"), constantTempo (120.0, 0.5, 0.0));
            LibraryManager lib;
            lib.open (dbFile);
            lib.rescanAll();
            waitFor ([&] { return ! lib.isScanning(); });
            AnalysisManager manager (lib, formats);
            check (manager.enqueuePending() == 1, "the new (too short) file is queued");
            waitFor ([&] { return ! manager.isBusy(); });
            check (manager.getFailed() == 1, "a file that cannot be analysed counts as failed");
            check (manager.enqueuePending() == 0, "and is not retried automatically");
        }
    }

    root.deleteRecursively();
    return 0;
}

int runAnalysisTests()
{
    //==========================================================================
    std::cout << "Tempo accuracy (synthetic drum tracks)" << std::endl;
    {
        struct Case { double bpm; double offset; const char* style; };
        const Case cases[] = { { 128.00, 0.10, "house" }, { 123.45, 0.31, "odd tempo" }, { 140.00, 0.05, "techno/EDM" },
                               { 100.00, 0.22, "pop" }, { 87.50, 0.17, "hip-hop" }, { 174.00, 0.12, "drum & bass" },
                               { 126.00, 0.00, "tech house" } };
        double worstBpmError = 0.0, worstBeatError = 0.0;
        for (const auto& c : cases)
        {
            const auto track = constantTempo (c.bpm, 90.0, c.offset);
            const auto t = analyseTempo (track);
            const double bpmError = std::abs (t.bpm - c.bpm);
            const double period = 60.0 / c.bpm;
            double beatError = std::fmod (std::abs (t.firstBeatSeconds - std::fmod (c.offset, period)), period);
            beatError = std::min (beatError, period - beatError);
            worstBpmError = std::max (worstBpmError, bpmError);
            worstBeatError = std::max (worstBeatError, beatError);

            check (bpmError <= 0.05, juce::String ("BPM ") + c.style + " " + juce::String (c.bpm, 2),
                   "detected " + juce::String (t.bpm, 2) + ", conf " + juce::String (t.confidence, 2)
                       + ", beat error " + juce::String (beatError * 1000.0, 1) + " ms"
                       + (t.alternativeBpm > 0 ? ", alt " + juce::String (t.alternativeBpm, 2) : juce::String()));
        }
        check (worstBeatError < 0.025, "beatgrid position within 25 ms on all tracks", juce::String (worstBeatError * 1000.0, 1) + " ms worst");
    }

    std::cout << "Confidence, silence and noise" << std::endl;
    {
        const auto clean = analyseTempo (constantTempo (128.0, 90.0, 0.1));
        check (clean.confidence > 0.8, "clear rhythm gets high confidence", juce::String (clean.confidence, 2));

        MonoAudio noise;
        noise.sampleRate = sr;
        noise.durationSeconds = 60.0;
        juce::Random rng (7);
        for (int i = 0; i < (int) (60.0 * sr); ++i)
            noise.samples.push_back ((rng.nextFloat() * 2.0f - 1.0f) * 0.3f);
        const auto n = analyseTempo (noise);
        check (n.confidence < 0.3, "white noise is marked as unreliable", "conf " + juce::String (n.confidence, 2) + ", bpm " + juce::String (n.bpm, 2));

        MonoAudio silence;
        silence.sampleRate = sr;
        silence.durationSeconds = 30.0;
        silence.samples.assign ((size_t) (30.0 * sr), 0.0f);
        check (analyseTempo (silence).bpm == 0.0, "silence yields no tempo");

        MonoAudio tiny;
        tiny.sampleRate = sr;
        tiny.durationSeconds = 1.0;
        tiny.samples.assign ((size_t) sr, 0.1f);
        check (analyseTempo (tiny).bpm == 0.0 && analyseKey (tiny).tonic == -1, "very short audio is rejected without crashing");
    }

    std::cout << "Downbeat" << std::endl;
    {
        int correct = 0, total = 0;
        juce::StringArray wrongCases;
        for (double offset : { 0.0, 0.12, 0.33 })
            for (int shift = 0; shift < 4; ++shift)
            {
                // Bar starts on beat number `shift` after the first grid beat.
                const double bpm = 126.0, period = 60.0 / bpm;
                MonoAudio a = makeDrumTrack ([bpm] (double) { return bpm; }, 80.0, offset, false);
                // Accent every 4th beat by adding a second kick.
                for (double t = offset + shift * period; t < 78.0; t += 4 * period)
                {
                    const int s = (int) (t * sr);
                    for (int i = 0; i < (int) (0.15 * sr); ++i)
                    {
                        const double x = (double) i / sr;
                        a.samples[(size_t) (s + i)] += 0.5f * (float) (std::sin (juce::MathConstants<double>::twoPi * 55.0 * x) * std::exp (-x * 20.0));
                    }
                }
                const auto t = analyseTempo (a);
                ++total;
                // The reported index counts from the reported first grid beat, which may be a later beat than
                // the one at `offset` when that beat lies before the first analysis window.
                const int skipped = (int) std::lround ((t.firstBeatSeconds - offset) / period);
                const int expected = ((shift - skipped) % 4 + 4) % 4;
                if (t.downbeatIndex == expected)
                    ++correct;
                else
                    wrongCases.add ("offset " + juce::String (offset, 2) + " shift " + juce::String (shift) + " -> " + juce::String (t.downbeatIndex)
                                    + " (grid " + juce::String (t.firstBeatSeconds, 3) + ")");
            }
        check (correct == total, "bar start is found for every beat position",
               juce::String (correct) + " of " + juce::String (total) + "; " + wrongCases.joinIntoString ("; "));
    }

    std::cout << "Tempo changes" << std::endl;
    {
        const auto constant = analyseTempo (constantTempo (125.0, 120.0, 0.1));
        check (! constant.variableTempo, "constant tempo is not flagged");
        const auto ramp = analyseTempo (makeDrumTrack ([] (double t) { return 120.0 + 6.0 * t / 120.0; }, 120.0, 0.1, true));
        check (ramp.variableTempo, "a 120 -> 126 BPM ramp is flagged as variable tempo");
    }

    //==========================================================================
    std::cout << "Key detection (all 24 keys, I-IV-V-I progressions)" << std::endl;
    {
        int exact = 0, relative = 0, wrong = 0;
        juce::StringArray problems;
        double minConfidence = 1.0;
        for (int minor = 0; minor < 2; ++minor)
            for (int tonic = 0; tonic < 12; ++tonic)
            {
                const auto a = makeProgression (tonic, minor != 0, 30.0);
                const auto k = analyseKey (a);
                minConfidence = std::min (minConfidence, k.confidence);
                if (k.tonic == tonic && k.minor == (minor != 0))
                    ++exact;
                else if (k.camelot.getIntValue() == toCamelot (tonic, minor != 0).getIntValue())
                {
                    ++relative;
                    problems.add (toKeyName (tonic, minor != 0) + "->" + k.name);
                }
                else
                {
                    ++wrong;
                    problems.add (toKeyName (tonic, minor != 0) + "->" + k.name);
                }
            }
        check (exact == 24, "all 24 keys identified exactly", juce::String (exact) + " exact, " + juce::String (relative) + " relative-key mix-ups, "
                                                                 + juce::String (wrong) + " wrong " + problems.joinIntoString (" "));
        check (minConfidence > 0.3, "key confidence is reasonable on clear harmony", "lowest " + juce::String (minConfidence, 2));

        check (toCamelot (0, false) == "8B" && toCamelot (9, true) == "8A" && toCamelot (7, false) == "9B"
                   && toCamelot (6, true) == "11A" && toCamelot (11, false) == "1B" && toCamelot (5, true) == "4A",
               "Camelot notation");

        // A song slightly off concert pitch (+35 cents) must still be recognised.
        auto detuned = makeProgression (2, false, 30.0);
        MonoAudio shifted;
        shifted.sampleRate = sr;
        shifted.durationSeconds = 30.0;
        const double ratio = std::pow (2.0, 0.35 / 12.0);
        for (int i = 0; i < (int) detuned.samples.size() - 2; ++i)
        {
            const double pos = i * ratio;
            const int j = (int) pos;
            if (j + 1 >= (int) detuned.samples.size())
                break;
            shifted.samples.push_back (detuned.samples[(size_t) j] * (1.0f - (float) (pos - j)) + detuned.samples[(size_t) j + 1] * (float) (pos - j));
        }
        const auto kd = analyseKey (shifted);
        check (kd.tonic == 2 && ! kd.minor, "a +35 cent detuned track is still D major (tuning compensation)", kd.name);

        MonoAudio noise;
        noise.sampleRate = sr;
        juce::Random rng (3);
        for (int i = 0; i < (int) (30 * sr); ++i)
            noise.samples.push_back ((rng.nextFloat() * 2.0f - 1.0f) * 0.3f);
        const auto kn = analyseKey (noise);
        check (kn.confidence < 0.5, "noise gives low key confidence", juce::String (kn.confidence, 2));
    }

    //==========================================================================
    std::cout << "Decoding, fingerprint, energy, performance" << std::endl;
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJAnalysisTests");
        dir.deleteRecursively();
        dir.createDirectory();

        // A 44.1 kHz stereo WAV of a 128 BPM track must give the same tempo after decoding.
        const auto track = constantTempo (128.0, 60.0, 0.2);
        const auto wav = dir.getChildFile ("track.wav");
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream());
            auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
            juce::AudioBuffer<float> buffer (2, (int) (60.0 * 44100.0));
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const double pos = i / 2.0;
                const int j = (int) pos;
                const float v = j + 1 < (int) track.samples.size()
                                    ? track.samples[(size_t) j] * (1.0f - (float) (pos - j)) + track.samples[(size_t) j + 1] * (float) (pos - j) : 0.0f;
                buffer.setSample (0, i, v);
                buffer.setSample (1, i, v);
            }
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        MonoAudio decoded;
        juce::String error;
        check (decodeToMono (wav, formats, decoded, error), "decodes a 44.1 kHz stereo WAV", error);
        check (std::abs (decoded.durationSeconds - 60.0) < 0.05 && std::abs (decoded.sampleRate - 22050.0) < 1.0,
               "mono at 22.05 kHz with the right duration", juce::String (decoded.durationSeconds, 2) + " s, " + juce::String (decoded.sampleRate, 0) + " Hz");

        const double started = juce::Time::getMillisecondCounterHiRes();
        const auto full = analyseAudio (decoded);
        const double elapsed = juce::Time::getMillisecondCounterHiRes() - started;
        check (std::abs (full.tempo.bpm - 128.0) <= 0.05, "decoded file analyses to 128 BPM", juce::String (full.tempo.bpm, 2));
        check (elapsed < 5000.0, "analysis of 60 s of audio is fast", juce::String (elapsed, 0) + " ms");

        std::atomic<bool> cancel { true };
        MonoAudio cancelled;
        check (! decodeToMono (wav, formats, cancelled, error, &cancel) && error == "cancelled", "decoding can be cancelled");

        const auto junk = dir.getChildFile ("junk.wav");
        junk.replaceWithText ("nothing");
        check (! decodeToMono (junk, formats, cancelled, error) && error.isNotEmpty(), "an unreadable file reports an error");

        const auto copy = dir.getChildFile ("renamed copy.wav");
        wav.copyFileTo (copy);
        const auto other = dir.getChildFile ("other.wav");
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream (other.createOutputStream());
            auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
            juce::AudioBuffer<float> buffer (2, 44100 * 60);
            buffer.clear();
            buffer.setSample (0, 100000, 0.5f);
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        }
        check (computeFingerprint (wav) == computeFingerprint (copy) && computeFingerprint (wav).isNotEmpty(), "a renamed copy has the same fingerprint");
        check (computeFingerprint (wav) != computeFingerprint (other), "different audio has a different fingerprint");
        check (computeFingerprint (dir.getChildFile ("missing.wav")).isEmpty(), "missing file has no fingerprint");
        check (! isOnlineOnly (wav), "an ordinary local file is not online-only");

        double loud = 0.0, quiet = 0.0, loudDb = 0.0, quietDb = 0.0;
        auto loudTrack = constantTempo (128.0, 30.0, 0.0);
        auto quietTrack = loudTrack;
        for (auto& s : quietTrack.samples) s *= 0.05f;
        loud = estimateEnergy (loudTrack, loudDb);
        quiet = estimateEnergy (quietTrack, quietDb);
        check (loud > quiet && loud >= 1.0 && loud <= 10.0 && quiet >= 1.0, "energy rises with loudness and stays within 1..10",
               juce::String (quiet, 1) + " < " + juce::String (loud, 1));

        dir.deleteRecursively();
    }

    return 0;
}
