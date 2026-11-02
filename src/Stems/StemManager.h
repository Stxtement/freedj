#pragma once

#include "Stems/ModelDownload.h"
#include "Stems/StemCache.h"
#include "Stems/StemSeparator.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <deque>
#include <map>

namespace pathboard::stems
{

// Expected model file (HTDemucs exported for ONNX; MIT licence, Meta AI Research).
struct ModelInfo
{
    static constexpr const char* fileName = "htdemucs-dml.onnx";
    static constexpr const char* sha256 = "9e06fef2b23e0f772c6a0cedc4da3aeacb066fa2fa7b6bf70ba08bb1db6220ab";
    static constexpr juce::int64 sizeBytes = 371576554;
    static constexpr const char* downloadUrl = "https://huggingface.co/ferrite-stems/htdemucs-onnx-dml/resolve/main/htdemucs-dml.onnx";
    // Identifies the model inside cached stems: other model, other cache entries.
    static constexpr const char* cacheId = "htdemucs-dml-9e06fef2";

    static bool isCompleteAndValid (const juce::File& file, bool checkHash);
    static juce::String computeSha256 (const juce::File& file);
};

enum class JobState { queued, fingerprinting, loadingModel, decoding, separating, saving, loadingCache, done, failed, cancelled };
const char* jobStateName (JobState);

struct JobProgress
{
    JobState state = JobState::queued;
    float fraction = 0.0f;     // meaningful while separating
    juce::String error;
    bool fromCache = false;
    Quality quality = Quality::balanced;
    juce::String device;
};

// Computes stems in a background thread; never touches the audio thread. Results are delivered on the
// message thread. One job runs at a time; interactive requests (a track on a deck) go before precomputing.
class StemManager : private juce::Thread
{
public:
    using Callback = std::function<void (std::shared_ptr<const StemSet> stems, const juce::String& error)>;

    StemManager (const juce::File& cacheFolder, juce::int64 maxCacheBytes, juce::AudioFormatManager& formats);
    ~StemManager() override;

    void setModelFile (const juce::File& file);
    juce::File getModelFile() const;
    bool isModelPresent() const;
    void setPreferGpu (bool prefer) { preferGpu = prefer; }

    // Starts the model download (370 MB, verified by SHA-256). onProgress(0..1) and onFinished run on the
    // message thread; onFinished gets an empty string on success.
    void downloadModel (std::function<void (float)> onProgress, std::function<void (const juce::String&)> onFinished);
    bool isDownloading() const;

    // Returns a request id. `interactive` jobs are processed before background ones.
    int request (const juce::File& file, Quality quality, bool interactive, Callback onDone);
    void cancel (int requestId);
    void cancelAll();
    JobProgress getProgress (int requestId) const;
    bool isBusy() const;
    int getQueueLength() const;

    StemCache& getCache() { return cache; }
    juce::String getDeviceName() const;

private:
    struct Job
    {
        int id = 0;
        juce::File file;
        Quality quality = Quality::balanced;
        bool interactive = false;
        Callback callback;
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>> (false);
    };

    void run() override;
    void process (const Job& job);
    void setProgress (int id, const std::function<void (JobProgress&)>& change);
    void finish (const Job& job, std::shared_ptr<const StemSet> stems, const juce::String& error, bool cancelled);

    juce::AudioFormatManager& formats;
    StemCache cache;
    StemSeparator separator;
    juce::File modelFile;
    std::atomic<bool> preferGpu { true };
    bool modelHashChecked = false;
    ModelDownload download;

    mutable juce::CriticalSection lock;
    std::deque<Job> queue;
    std::map<int, JobProgress> progress;
    std::map<int, std::shared_ptr<std::atomic<bool>>> cancelFlags;
    int nextId = 1;
    bool workerActive = false;
    int currentJobId = 0;
    juce::String deviceName;

    JUCE_DECLARE_WEAK_REFERENCEABLE (StemManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemManager)
};

// Decodes any supported file to stereo float at 44.1 kHz (resampling if needed). Cancel-aware.
bool decodeStereo44k (const juce::File& file, juce::AudioFormatManager& formats, juce::AudioBuffer<float>& out,
                      juce::String& error, const std::atomic<bool>* cancel = nullptr);

} // namespace pathboard::stems
