#include "StemManager.h"
#include "Analysis/AudioAnalysis.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace pathboard::stems
{

const char* jobStateName (JobState s)
{
    switch (s)
    {
        case JobState::queued:         return "waiting";
        case JobState::fingerprinting: return "checking file";
        case JobState::loadingModel:   return "loading AI model";
        case JobState::decoding:       return "reading audio";
        case JobState::separating:     return "separating";
        case JobState::saving:         return "saving stems";
        case JobState::loadingCache:   return "loading stems";
        case JobState::done:           return "ready";
        case JobState::failed:         return "failed";
        case JobState::cancelled:      return "cancelled";
    }
    return "";
}

//==============================================================================
juce::String ModelInfo::computeSha256 (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (in.failedToOpen())
        return {};
    return juce::SHA256 (in).toHexString();
}

bool ModelInfo::isCompleteAndValid (const juce::File& file, bool checkHash)
{
    if (! file.existsAsFile() || file.getSize() != sizeBytes)
        return false;
    return ! checkHash || computeSha256 (file).equalsIgnoreCase (sha256);
}

//==============================================================================
bool decodeStereo44k (const juce::File& file, juce::AudioFormatManager& formats, juce::AudioBuffer<float>& out,
                      juce::String& error, const std::atomic<bool>* cancel)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        error = "Cannot read audio: " + file.getFileName();
        return false;
    }
    if (reader->lengthInSamples > (juce::int64) 60 * 60 * 192000)
    {
        error = "Track is too long for stem separation";
        return false;
    }

    const auto length = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> source (2, length);
    constexpr int chunk = 1 << 16;
    int decoded = length;
    for (int pos = 0; pos < length; pos += chunk)
    {
        if (cancel != nullptr && cancel->load())
        {
            error = "cancelled";
            return false;
        }
        const int n = juce::jmin (chunk, length - pos);
        if (! reader->read (&source, pos, n, pos, true, true))
        {
            if (pos == 0)
            {
                error = "Decoding error in: " + file.getFileName();
                return false;
            }
            decoded = pos; // compressed formats may end earlier than announced
            break;
        }
    }

    // Drop the zero padding a decoder leaves when it ends early.
    while (decoded > 1 && source.getSample (0, decoded - 1) == 0.0f && source.getSample (1, decoded - 1) == 0.0f)
        --decoded;
    source.setSize (2, decoded, true);

    if (std::abs (reader->sampleRate - modelSampleRate) < 0.5)
    {
        out = std::move (source);
        return true;
    }

    // The Lagrange interpolator delays its output by 2 output samples; render two extra samples and drop the
    // first two so the stems stay time-aligned with the original file (the deck maps both by time).
    constexpr int interpolatorLatency = 2; // Interpolators::Lagrange::algorithmicLatency in JUCE
    const double ratio = reader->sampleRate / (double) modelSampleRate;
    const int outLength = (int) std::ceil ((double) decoded / ratio);

    source.setSize (2, decoded + 16, true);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::clear (source.getWritePointer (ch) + decoded, 16);

    juce::AudioBuffer<float> resampled (2, outLength + interpolatorLatency);
    for (int ch = 0; ch < 2; ++ch)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (ratio, source.getReadPointer (ch), resampled.getWritePointer (ch), outLength + interpolatorLatency);
    }

    out.setSize (2, outLength);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::copy (out.getWritePointer (ch), resampled.getReadPointer (ch) + interpolatorLatency, outLength);
    return true;
}

//==============================================================================
StemManager::StemManager (const juce::File& cacheFolder, juce::int64 maxCacheBytes, juce::AudioFormatManager& fm)
    : juce::Thread ("Stem separation"), formats (fm), cache (cacheFolder, maxCacheBytes, ModelInfo::cacheId)
{
}

StemManager::~StemManager()
{
    cancelAll();
    download.cancel();
    stopThread (60000);
}

void StemManager::setModelFile (const juce::File& file)
{
    const juce::ScopedLock sl (lock);
    modelFile = file;
    modelHashChecked = false;
}

juce::File StemManager::getModelFile() const
{
    const juce::ScopedLock sl (lock);
    return modelFile;
}

bool StemManager::isModelPresent() const
{
    return ModelInfo::isCompleteAndValid (getModelFile(), false);
}

juce::String StemManager::getDeviceName() const
{
    const juce::ScopedLock sl (lock);
    return deviceName;
}

//==============================================================================
int StemManager::request (const juce::File& file, Quality quality, bool interactive, Callback onDone)
{
    Job job;
    job.file = file;
    job.quality = quality;
    job.interactive = interactive;
    job.callback = std::move (onDone);

    int id;
    bool startWorker = false;
    {
        const juce::ScopedLock sl (lock);
        id = job.id = nextId++;
        JobProgress p;
        p.quality = quality;
        progress[id] = p;
        cancelFlags[id] = job.cancelled;

        // Interactive jobs go in front of background jobs (but behind other interactive ones).
        if (interactive)
        {
            auto it = std::find_if (queue.begin(), queue.end(), [] (const Job& j) { return ! j.interactive; });
            queue.insert (it, std::move (job));
        }
        else
        {
            queue.push_back (std::move (job));
        }

        if (! workerActive)
        {
            workerActive = true;
            startWorker = true;
        }
    }

    if (startWorker)
    {
        stopThread (60000); // the previous worker has already decided to exit; this returns quickly
        startThread (juce::Thread::Priority::low);
    }
    return id;
}

void StemManager::cancel (int requestId)
{
    const juce::ScopedLock sl (lock);
    if (auto it = cancelFlags.find (requestId); it != cancelFlags.end())
        it->second->store (true);

    // A queued job disappears right away; a running one stops at the next segment.
    queue.erase (std::remove_if (queue.begin(), queue.end(), [requestId] (const Job& j) { return j.id == requestId; }), queue.end());
}

void StemManager::cancelAll()
{
    const juce::ScopedLock sl (lock);
    for (auto& [id, flag] : cancelFlags)
        flag->store (true);
    queue.clear();
}

JobProgress StemManager::getProgress (int requestId) const
{
    const juce::ScopedLock sl (lock);
    const auto it = progress.find (requestId);
    return it != progress.end() ? it->second : JobProgress();
}

bool StemManager::isBusy() const
{
    const juce::ScopedLock sl (lock);
    return workerActive || ! queue.empty();
}

int StemManager::getQueueLength() const
{
    const juce::ScopedLock sl (lock);
    return (int) queue.size() + (workerActive ? 1 : 0);
}

void StemManager::setProgress (int id, const std::function<void (JobProgress&)>& change)
{
    const juce::ScopedLock sl (lock);
    if (auto it = progress.find (id); it != progress.end())
        change (it->second);
}

void StemManager::finish (const Job& job, std::shared_ptr<const StemSet> stems, const juce::String& error, bool wasCancelled)
{
    setProgress (job.id, [&] (JobProgress& p)
    {
        p.state = wasCancelled ? JobState::cancelled : (error.isNotEmpty() ? JobState::failed : JobState::done);
        p.fraction = 1.0f;
        p.error = error;
    });

    juce::MessageManager::callAsync ([weak = juce::WeakReference<StemManager> (this), cb = job.callback, stems = std::move (stems), error, wasCancelled]
                                     {
                                         if (weak != nullptr && cb && ! wasCancelled)
                                             cb (stems, error);
                                     });
}

//==============================================================================
void StemManager::run()
{
    for (;;)
    {
        Job job;
        {
            const juce::ScopedLock sl (lock);
            if (queue.empty() || threadShouldExit())
            {
                workerActive = false;
                currentJobId = 0;
                break;
            }
            job = std::move (queue.front());
            queue.pop_front();
            currentJobId = job.id;
        }
        process (job);
    }
}

void StemManager::process (const Job& job)
{
    auto cancelled = [&] { return job.cancelled->load(); };
    auto fail = [&] (const juce::String& message) { finish (job, nullptr, message, false); };

    if (cancelled())
        return finish (job, nullptr, {}, true);

    // 1. Identify the file by content.
    setProgress (job.id, [] (JobProgress& p) { p.state = JobState::fingerprinting; });
    if (! job.file.existsAsFile())
        return fail ("File not found: " + job.file.getFileName());
    if (analysis::isOnlineOnly (job.file))
        return fail ("The file is online-only in OneDrive; make it available offline first.");
    const auto fingerprint = analysis::computeFingerprint (job.file);
    if (fingerprint.isEmpty())
        return fail ("Cannot read " + job.file.getFileName());

    // 2. Cached stems (of at least the requested quality) are used as they are.
    setProgress (job.id, [] (JobProgress& p) { p.state = JobState::loadingCache; });
    juce::String cacheError;
    if (auto cached = cache.load (fingerprint, job.quality, &cacheError))
    {
        setProgress (job.id, [&] (JobProgress& p) { p.fromCache = true; p.quality = cached->quality; });
        return finish (job, std::move (cached), {}, false);
    }
    if (cancelled())
        return finish (job, nullptr, {}, true);

    // 3. Model.
    setProgress (job.id, [] (JobProgress& p) { p.state = JobState::loadingModel; });
    juce::File model;
    bool needHashCheck;
    {
        const juce::ScopedLock sl (lock);
        model = modelFile;
        needHashCheck = ! modelHashChecked;
    }
    if (! ModelInfo::isCompleteAndValid (model, false))
        return fail ("The AI stem model is not installed (" + model.getFullPathName() + ").");
    if (needHashCheck)
    {
        // Hashing 370 MB takes seconds; a marker next to the model remembers a successful check for this exact
        // file (size and modification time), so it is done once per download.
        const auto marker = model.withFileExtension (".verified");
        const auto stamp = juce::String (model.getSize()) + "|" + juce::String (model.getLastModificationTime().toMilliseconds()) + "|" + ModelInfo::sha256;
        if (marker.loadFileAsString().trim() != stamp)
        {
            if (! ModelInfo::isCompleteAndValid (model, true))
                return fail ("The AI stem model file is damaged or not the expected model. Please download it again.");
            marker.replaceWithText (stamp);
        }
        const juce::ScopedLock sl (lock);
        modelHashChecked = true;
    }
    if (! separator.isLoaded())
    {
        const auto loaded = separator.load (model, preferGpu.load());
        if (loaded.failed())
            return fail ("Cannot load the AI model: " + loaded.getErrorMessage());
    }
    {
        const juce::ScopedLock sl (lock);
        deviceName = separator.getDeviceName();
    }
    setProgress (job.id, [&] (JobProgress& p) { p.device = separator.getDeviceName(); });

    // 4. Audio.
    setProgress (job.id, [] (JobProgress& p) { p.state = JobState::decoding; });
    juce::AudioBuffer<float> mix;
    juce::String error;
    if (! decodeStereo44k (job.file, formats, mix, error, job.cancelled.get()))
        return error == "cancelled" ? finish (job, nullptr, {}, true) : fail (error);

    // 5. Separation.
    setProgress (job.id, [] (JobProgress& p) { p.state = JobState::separating; p.fraction = 0.0f; });
    auto set = std::make_shared<StemSet>();
    set->quality = job.quality;
    set->fingerprint = fingerprint;
    const auto result = separator.separate (mix, job.quality, set->audio, [&] (float fraction)
    {
        setProgress (job.id, [fraction] (JobProgress& p) { p.fraction = fraction; });
        return ! cancelled();
    });
    mix = {};

    {
        const juce::ScopedLock sl (lock);
        deviceName = separator.getDeviceName();
    }

    if (result.failed())
        return result.getErrorMessage() == "cancelled" ? finish (job, nullptr, {}, true) : fail (result.getErrorMessage());

    // 6. Keep for next time.
    setProgress (job.id, [] (JobProgress& p) { p.state = JobState::saving; });
    // A failed save (disk full, folder not writable) does not fail the job: the stems are still usable now,
    // they are just recomputed next time.
    const bool stored = cache.store (fingerprint, job.quality, set->audio, job.file.getFileName());
    juce::ignoreUnused (stored);
    jassert (stored);

    finish (job, std::move (set), {}, false);
}

void StemManager::downloadModel (std::function<void (float)> onProgress, std::function<void (const juce::String&)> onFinished)
{
    download.start (ModelInfo::downloadUrl, getModelFile(), ModelInfo::sizeBytes, ModelInfo::sha256, std::move (onProgress), std::move (onFinished));
}

bool StemManager::isDownloading() const
{
    return download.isRunning();
}
} // namespace pathboard::stems
