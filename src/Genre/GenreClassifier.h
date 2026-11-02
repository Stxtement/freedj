#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <vector>

namespace pathboard::genre
{

constexpr int numStyles = 400;
constexpr int sampleRate = 16000;
constexpr int patchFrames = 128;
constexpr int melBands = 96;

// Preprocessing choices of the mel spectrogram. The defaults were chosen by comparing the model's output on
// songs of known genre (see docs); they are kept as options so the choice stays visible and testable.
struct MelOptions
{
    // Chosen by measurement: of 24 combinations, only power spectra gave sensible styles on songs of known
    // genre (alternative rock for Mr. Brightside, gangsta rap for In Da Club); magnitude spectra read rock and
    // hip-hop as "experimental / noise". This is the MusiCNN / librosa convention.
    bool normalisedWindow = false;      // Hann window scaled to unit area (x2)
    bool areaNormalisedFilters = true;  // true: unit-area (Slaney) triangles, false: peak 1
    double logScale = 10000.0;          // log10 (1 + logScale * mel)
    bool powerSpectrum = true;          // true: power (magnitude squared), false: magnitude
};

// Mel spectrogram patches [patch][128][96] from mono audio at 16 kHz.
std::vector<float> computeMelPatches (const std::vector<float>& mono16k, const MelOptions& options, int maxPatches, int& numPatches);

// Resamples with a windowed-sinc filter (alias-free for downsampling).
std::vector<float> resampleSinc (const std::vector<float>& input, double inputRate, double outputRate);

// Decodes any supported file to mono 16 kHz (the model's input). Cancel-aware; at most 12 minutes are read.
bool decodeMono16k (const juce::File& file, juce::AudioFormatManager& formats, std::vector<float>& out,
                    juce::String& error, const std::atomic<bool>* cancel = nullptr);

struct StyleProbability
{
    int index = 0;
    juce::String label;      // "Electronic---Tech House"
    float probability = 0.0f;
};

// Wraps the Discogs-EffNet ONNX model (400 music styles). Thread-safe: classify() locks internally.
class GenreClassifier
{
public:
    GenreClassifier();
    ~GenreClassifier();

    juce::Result load (const juce::File& modelFile, const juce::StringArray& styleLabels);
    bool isLoaded() const;

    // Mean sigmoid probability of every style over evenly spread patches of the track.
    juce::Result classify (const std::vector<float>& mono16k, std::vector<float>& probabilities, int maxPatches = 24,
                           const MelOptions& options = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace pathboard::genre
