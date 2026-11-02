// Genre detection tests.

#include "TestSupport.h"
#include "Analysis/AudioAnalysis.h"
#include "Genre/GenreClassifier.h"
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

int runGenreTests()
{
    std::cout << "Genre detection" << std::endl;
    return 0;
}
