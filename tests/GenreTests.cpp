// Genre detection tests.

#include "TestSupport.h"
#include "Analysis/AnalysisManager.h"
#include "Analysis/AudioAnalysis.h"
#include "Genre/DiscogsStyles.h"
#include "Genre/GenreClassifier.h"
#include "Genre/GenreTaxonomy.h"
#include "Library/LibraryManager.h"
#include "Stems/ModelDownload.h"
#include <juce_events/juce_events.h>
#include <map>

using namespace pathboard;
using testsupport::check;

juce::File findGenreModel()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("PATHBOARD_MODEL_DIR", {});
    const auto folder = env.isNotEmpty() ? juce::File (env) : juce::File::getCurrentWorkingDirectory().getChildFile ("models");
    return folder.getChildFile ("discogs-effnet-bsdynamic-1.onnx");
}

static juce::StringArray loadLabels (const juce::File& jsonFile)
{
    juce::StringArray labels;
    const auto parsed = juce::JSON::parse (jsonFile);
    if (auto* arr = parsed.getProperty ("classes", {}).getArray())
        for (const auto& v : *arr)
            labels.add (v.toString());
    return labels;
}

// `FreeDJTests --genre-probe <audio files...>`: top styles per preprocessing variant.
int runGenreProbe (const juce::StringArray& files)
{
    const auto modelFile = findGenreModel();
    const auto labels = loadLabels (modelFile.withFileExtension (".json"));
    genre::GenreClassifier classifier;
    const auto loaded = classifier.load (modelFile, labels);
    if (loaded.failed())
    {
        std::cout << "cannot load model: " << loaded.getErrorMessage() << std::endl;
        return 1;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    struct Variant { juce::String name; genre::MelOptions options; };
    std::vector<Variant> variants;
    variants.push_back ({ "default (power, area, raw)", genre::MelOptions{} });

    for (const auto& path : files)
    {
        const juce::File file (path);
        std::vector<float> audio;
        juce::String error;
        if (! genre::decodeMono16k (file, formats, audio, error))
        {
            std::cout << file.getFileName() << ": " << error << std::endl;
            continue;
        }
        std::cout << "\n" << file.getFileName() << "  (" << audio.size() / 16000 << " s)" << std::endl;
        for (const auto& v : variants)
        {
            std::vector<float> p;
            const double t = juce::Time::getMillisecondCounterHiRes();
            if (classifier.classify (audio, p, 24, v.options).failed())
                continue;
            const double ms = juce::Time::getMillisecondCounterHiRes() - t;
            std::vector<int> order (p.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = (int) i;
            std::partial_sort (order.begin(), order.begin() + 3, order.end(), [&] (int a, int b) { return p[(size_t) a] > p[(size_t) b]; });

            // strongest top-level category (the part before "---")
            std::map<juce::String, float> top;
            for (size_t i = 0; i < p.size(); ++i)
            {
                auto& slot = top[labels[(int) i].upToFirstOccurrenceOf ("---", false, false)];
                slot = std::max (slot, p[i]);
            }
            std::vector<std::pair<float, juce::String>> cats;
            for (auto& [k, val] : top) cats.push_back ({ val, k });
            std::sort (cats.rbegin(), cats.rend());

            std::cout << "  " << v.name.paddedRight (' ', 26) << " top: ";
            for (int i = 0; i < 3; ++i)
                std::cout << labels[order[(size_t) i]].fromLastOccurrenceOf ("---", false, false) << " " << juce::String (p[(size_t) order[(size_t) i]], 2) << ", ";
            std::cout << "  categories: " << cats[0].second << " " << juce::String (cats[0].first, 2) << ", " << cats[1].second << " " << juce::String (cats[1].first, 2)
                      << ", " << cats[2].second << " " << juce::String (cats[2].first, 2) << std::endl;
            juce::ignoreUnused (ms);
        }
    }
    return 0;
}

namespace
{
double toneAmplitude (const std::vector<float>& x, double freq, double rate)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / rate, coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    const int from = (int) x.size() / 10, to = (int) x.size() * 9 / 10;
    for (int i = from; i < to; ++i)
    {
        const double s0 = x[(size_t) i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt (std::max (0.0, power)) / (double) (to - from);
}

void writeDrumWav (const juce::File& file, double bpm, double seconds)
{
    constexpr double sr = 44100.0;
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    const int n = (int) (seconds * sr);
    juce::AudioBuffer<float> buffer (2, n);
    buffer.clear();
    juce::Random rng (3);
    const double period = 60.0 / bpm;
    for (double t = 0.1; t < seconds - 0.3; t += period)
    {
        const int s = (int) (t * sr);
        for (int i = 0; i < (int) (0.15 * sr) && s + i < n; ++i)
        {
            const double x = (double) i / sr;
            const float v = 0.8f * (float) (std::sin (2.0 * juce::MathConstants<double>::pi * (50.0 + 90.0 * std::exp (-x * 40.0)) * x) * std::exp (-x * 25.0));
            buffer.addSample (0, s + i, v);
            buffer.addSample (1, s + i, v);
        }
        const int h = (int) ((t + period * 0.5) * sr);
        for (int i = 0; i < (int) (0.03 * sr) && h + i < n; ++i)
        {
            const float v = 0.15f * (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-(double) i / sr * 120.0);
            buffer.addSample (0, h + i, v);
            buffer.addSample (1, h + i, v);
        }
    }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
    writer->writeFromAudioSampleBuffer (buffer, 0, n);
}

void pumpMessages (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }
}

int runGenreTests()
{
    std::cout << "Genre detection" << std::endl;

    //==========================================================================
    // Pure signal processing: always runs.
    {
        // Resampler: 1 kHz survives, a 10 kHz tone (above the new Nyquist of 8 kHz) must not alias into the band.
        constexpr double inRate = 44100.0;
        std::vector<float> tone ((size_t) (inRate * 2)), high ((size_t) (inRate * 2));
        for (size_t i = 0; i < tone.size(); ++i)
        {
            tone[i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * (double) i / inRate);
            high[i] = 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 10000.0 * (double) i / inRate);
        }
        const auto lowOut = genre::resampleSinc (tone, inRate, 16000.0);
        check (std::abs ((double) lowOut.size() - 2.0 * 16000.0) <= 2.0, "44.1 -> 16 kHz gives the right length", juce::String ((int) lowOut.size()));
        check (std::abs (toneAmplitude (lowOut, 1000.0, 16000.0) - 0.5) < 0.01, "a 1 kHz tone keeps its level", juce::String (toneAmplitude (lowOut, 1000.0, 16000.0), 4));

        // The 10 kHz tone would alias to 6 kHz at 16 kHz: measure there.
        const auto highOut = genre::resampleSinc (high, inRate, 16000.0);
        const double aliasDb = 20.0 * std::log10 (std::max (1.0e-9, toneAmplitude (highOut, 6000.0, 16000.0) / 0.5));
        check (aliasDb < -40.0, "a 10 kHz tone does not alias into the band (> 40 dB down)", juce::String (aliasDb, 1) + " dB");
        check (genre::resampleSinc (tone, 16000.0, 16000.0).size() == tone.size(), "same rate: unchanged");

        // Mel patches
        std::vector<float> noise ((size_t) (16000 * 20));
        juce::Random rng (11);
        for (auto& s : noise) s = (rng.nextFloat() * 2.0f - 1.0f) * 0.3f;
        int patches = 0;
        const auto mel = genre::computeMelPatches (noise, {}, 10, patches);
        check (patches == 10 && mel.size() == (size_t) 10 * genre::patchFrames * genre::melBands, "mel patches have the shape [patches][128][96]", juce::String (patches));
        bool finite = true;
        float maxValue = 0.0f;
        for (float v : mel) { finite = finite && std::isfinite (v); maxValue = std::max (maxValue, v); }
        check (finite && maxValue > 0.0f && maxValue < 12.0f, "mel values are finite and in a sane range", "max " + juce::String (maxValue, 2));
        int p2 = 0;
        check (genre::computeMelPatches (std::vector<float> (1000, 0.0f), {}, 10, p2).empty() && p2 == 0, "too short audio gives no patches");
        int p3 = 0;
        const auto again = genre::computeMelPatches (noise, {}, 10, p3);
        check (again == mel, "mel computation is deterministic");

        // interpret()
        std::vector<float> probabilities (genre::numStyles, 0.01f);
        int techHouse = -1, techno = -1;
        for (int i = 0; i < genre::numStyles; ++i)
        {
            const auto label = genre::discogsStyle (i);
            if (label == "Electronic---Tech House") techHouse = i;
            if (label == "Electronic---Techno") techno = i;
        }
        probabilities[(size_t) techHouse] = 0.7f;
        probabilities[(size_t) techno] = 0.3f;
        const auto d = genre::interpret (probabilities);
        check (d.main == "House" && d.sub == "Tech House" && d.category == "Electronic" && std::abs (d.confidence - 0.7) < 1.0e-6,
               "interpret() maps the strongest style to main genre and sub-genre");
        const auto alt = juce::JSON::parse (d.alternativesJson);
        check (alt.getArray() != nullptr && alt.getArray()->size() >= 1 && alt[0].getProperty ("style", {}).toString() == "Electronic---Techno",
               "and lists the next most likely styles as alternatives", d.alternativesJson.replace ("\n", " "));
        check (genre::interpret (std::vector<float> (10, 0.0f)).main.isEmpty(), "a wrong-sized vector gives nothing");

        // Style names with accents must survive (they are UTF-8 in the vocabulary).
        bool accentOk = false;
        for (int i = 0; i < genre::numStyles; ++i)
            if (genre::discogsStyle (i).startsWith ("Electronic---Musique Conc"))
                accentOk = genre::discogsStyle (i).containsChar ((juce::juce_wchar) 0xE8) && genre::fromDiscogs (genre::discogsStyle (i)).sub.containsChar ((juce::juce_wchar) 0xE8);
        check (accentOk, "accented style names (Musique Concr\xC3\xA8te) keep their accent");
    }

    //==========================================================================
    // The real download from the official server (needs the internet): only with PATHBOARD_NET_TESTS=1.
    if (juce::SystemStats::getEnvironmentVariable ("PATHBOARD_NET_TESTS", {}) == "1")
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJNetTest");
        folder.deleteRecursively();
        const auto target = folder.getChildFile (genre::ModelInfo::fileName);
        ModelDownload download;
        juce::String result = "pending";
        float lastProgress = 0.0f;
        const double started = juce::Time::getMillisecondCounterHiRes();
        download.start (genre::ModelInfo::downloadUrl, target, genre::ModelInfo::sizeBytes, genre::ModelInfo::sha256,
                        [&] (float p) { lastProgress = p; }, [&] (const juce::String& e) { result = e; });
        for (int i = 0; i < 6000 && result == "pending"; ++i)
            pumpMessages (50);
        check (result.isEmpty() && target.existsAsFile() && target.getSize() == genre::ModelInfo::sizeBytes,
               "the real model downloads from the official server and passes the size and SHA-256 check",
               result.isEmpty() ? juce::String ((int) (juce::Time::getMillisecondCounterHiRes() - started)) + " ms" : result);
        check (lastProgress > 0.99f, "progress reached 100 %");
        folder.deleteRecursively();
    }

    const auto modelFile = findGenreModel();
    if (! modelFile.existsAsFile())
    {
        std::cout << "  [SKIP] genre model tests: model not found at " << modelFile.getFullPathName() << " (set PATHBOARD_MODEL_DIR)" << std::endl;
        return 0;
    }

    check (modelFile.getSize() == genre::ModelInfo::sizeBytes && ModelDownload::sha256Of (modelFile).equalsIgnoreCase (genre::ModelInfo::sha256),
           "the installed model matches the pinned size and SHA-256");

    genre::GenreClassifier classifier;
    check (classifier.load (modelFile, {}).wasOk() && classifier.isLoaded(), "the model loads");
    check (genre::GenreClassifier().load (modelFile.getSiblingFile ("nope.onnx"), {}).failed(), "a missing model is reported");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // Real songs from the user's library (skipped when not available locally).
    struct Song { const char* name; const char* file; const char* expectedCategory; const char* expectedMain; };
    const juce::File music ("C:\\Users\\deaga\\OneDrive\\Music");
    const Song songs[] = {
        { "Mr. Brightside (rock)", "DJ Musik\\02 Mr. Brightside.wav", "Rock", "Rock" },
        { "In Da Club (hip-hop)", "DJ Musik\\14 In Da Club.wav", "Hip Hop", "Hip Hop" },
        { "Get Busy (dancehall)", "DJ Musik\\05 Get Busy.wav", "Reggae", "Reggae" },
        { "Teenage Dirtbag (alt rock)", "DJ Musik\\10 Teenage Dirtbag.wav", "Rock", "Rock" },
    };
    std::cout << "  song                         detected                          confidence   time" << std::endl;
    int tested = 0, correct = 0;
    std::vector<float> firstRun, secondRun;
    for (const auto& s : songs)
    {
        const auto file = music.getChildFile (s.file);
        if (! file.existsAsFile() || analysis::isOnlineOnly (file))
            continue;
        std::vector<float> audio, p;
        juce::String error;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        if (! genre::decodeMono16k (file, formats, audio, error) || classifier.classify (audio, p).failed())
            continue;
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const auto d = genre::interpret (p);
        ++tested;
        const bool ok = d.category == s.expectedCategory && d.main == s.expectedMain;
        correct += ok ? 1 : 0;
        std::cout << "  " << juce::String (s.name).paddedRight (' ', 28) << " " << (d.main + " / " + d.sub).paddedRight (' ', 33) << juce::String (d.confidence, 2).paddedLeft (' ', 6)
                  << "      " << (int) ms << " ms" << (ok ? "" : "   <-- not as expected") << std::endl;
        if (tested == 1)
            firstRun = p;
    }
    if (tested == 0)
        std::cout << "  [SKIP] none of the reference songs is available locally" << std::endl;
    else
        check (correct == tested, "known songs are assigned to the right main genre", juce::String (correct) + " of " + juce::String (tested));

    // Determinism and robustness
    {
        std::vector<float> noise ((size_t) (16000 * 30));
        juce::Random rng (21);
        for (auto& s : noise) s = (rng.nextFloat() * 2.0f - 1.0f) * 0.3f;
        std::vector<float> a, b;
        check (classifier.classify (noise, a).wasOk() && classifier.classify (noise, b).wasOk() && a == b, "the same audio always gives the same result");
        bool finite = true;
        for (float v : a) finite = finite && std::isfinite (v) && v >= 0.0f && v <= 1.0f;
        check (finite, "all 400 probabilities are valid numbers between 0 and 1");
        std::vector<float> silence ((size_t) (16000 * 30), 0.0f), c;
        check (classifier.classify (silence, c).wasOk(), "silence does not crash");
        check (classifier.classify (std::vector<float> (500, 0.1f), c).failed(), "audio that is too short is reported");
    }

    //==========================================================================
    std::cout << "Genre in the background analysis" << std::endl;
    {
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("FreeDJGenrePipeline");
        root.deleteRecursively();
        const auto musicFolder = root.getChildFile ("music");
        writeDrumWav (musicFolder.getChildFile ("track one.wav"), 128.0, 40.0);
        writeDrumWav (musicFolder.getChildFile ("track two.wav"), 124.0, 40.0);

        LibraryManager lib;
        lib.open (root.getChildFile ("library.db"));
        lib.addFolderAndScan (musicFolder);
        for (int i = 0; i < 200 && lib.isScanning(); ++i) pumpMessages (50);
        pumpMessages (100);
        check (lib.getTracks().size() == 2, "two test tracks imported");

        // The user tags one of them with his own genre.
        {
            auto edited = lib.getTracks()[0];
            edited.genre = "My Own Genre";
            lib.updateTrack (edited, false);
        }
        const auto ownGenreId = lib.getTracks()[0].id;

        // 1. Analysis without the genre model: tempo and key only.
        AnalysisManager manager (lib, formats);
        manager.setGenreModelFile (root.getChildFile ("no-model-here.onnx"));
        check (! manager.isGenreModelInstalled(), "the genre model is reported as not installed");
        check (manager.enqueuePending() == 2, "both tracks are queued for analysis");
        for (int i = 0; i < 600 && manager.isBusy(); ++i) pumpMessages (50);
        pumpMessages (100);
        bool analysedOnly = true;
        for (const auto& t : lib.getTracks())
            analysedOnly = analysedOnly && t.analysisVersion == analysis::analysisVersion && t.genreVersion == 0 && t.genreAutoMain.isEmpty();
        check (analysedOnly, "without the model tempo/key are analysed and genre stays undetected");
        check (manager.enqueuePending() == 0, "and nothing is queued again");

        std::vector<juce::int64> analysedAt;
        for (const auto& t : lib.getTracks()) analysedAt.push_back (t.analysedAt);

        // 2. The model is installed later: only the genre is added, tempo/key are not computed again.
        manager.setGenreModelFile (modelFile);
        check (manager.isGenreModelInstalled(), "the model is now installed");
        check (manager.enqueuePending() == 2, "both already analysed tracks are queued for genre detection only");
        for (int i = 0; i < 600 && manager.isBusy(); ++i) pumpMessages (50);
        pumpMessages (100);

        bool allDetected = true, unchanged = true;
        for (size_t i = 0; i < lib.getTracks().size(); ++i)
        {
            const auto& t = lib.getTracks()[i];
            allDetected = allDetected && t.genreVersion == genre::detectionVersion && t.genreAutoMain.isNotEmpty() && t.genreConfidence > 0.0
                          && juce::JSON::parse (t.genreAlternatives).isArray();
            unchanged = unchanged && t.analysedAt == analysedAt[i];
        }
        check (allDetected, "genre, sub-genre, confidence and alternatives are stored for every track");
        check (unchanged, "tempo and key were not analysed a second time");
        const auto* mine = lib.findTrack (ownGenreId);
        check (mine != nullptr && mine->genre == "My Own Genre" && mine->displayGenre() == "My Own Genre" && mine->isManual (TrackRecord::manualGenre),
               "the user's own genre is untouched and still shown", mine != nullptr ? mine->genre + " / " + mine->genreAutoMain : juce::String());
        check (manager.enqueuePending() == 0, "nothing is queued once everything is done");
        check (manager.getFailed() == 0, "no failures");

        // 3. A forced re-analysis keeps the user's genre as well.
        manager.enqueue ({ ownGenreId }, true);
        for (int i = 0; i < 600 && manager.isBusy(); ++i) pumpMessages (50);
        pumpMessages (100);
        const auto* afterForce = lib.findTrack (ownGenreId);
        check (afterForce != nullptr && afterForce->genre == "My Own Genre" && afterForce->genreVersion == genre::detectionVersion, "'analyse again' keeps the user's genre");
        root.deleteRecursively();
    }
    return 0;
}
