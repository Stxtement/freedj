#include "AnalysisManager.h"
#include "Stems/ModelDownload.h"

namespace pathboard
{

AnalysisManager::AnalysisManager (LibraryManager& lib, juce::AudioFormatManager& fm)
    : juce::Thread ("Track analysis"), library (lib), formats (fm)
{
}

AnalysisManager::~AnalysisManager()
{
    cancel();
    stopThread (30000);
}

void AnalysisManager::setGenreModelFile (const juce::File& file)
{
    const juce::ScopedLock sl (queueLock);
    genreModelFile = file;
}

juce::File AnalysisManager::getGenreModelFile() const
{
    const juce::ScopedLock sl (queueLock);
    return genreModelFile;
}

bool AnalysisManager::isGenreModelInstalled() const
{
    const auto file = getGenreModelFile();
    return file.existsAsFile() && file.getSize() == genre::ModelInfo::sizeBytes;
}

bool AnalysisManager::isBusy() const
{
    const juce::ScopedLock sl (queueLock);
    return workerActive || ! queue.empty();
}

int AnalysisManager::getPercent() const
{
    const int total = queuedTotal.load();
    return total <= 0 ? 100 : juce::jlimit (0, 100, (done.load() + failed.load() + skippedCloud.load()) * 100 / total);
}

int AnalysisManager::enqueuePending()
{
    const bool genreAvailable = isGenreModelInstalled();
    std::vector<Job> jobs;
    for (const auto& t : library.getTracks())
    {
        if (t.missing || skippedIds.count (t.id) != 0)
            continue;
        // A failed analysis (-1) is tried once more per program run; after that it stays failed until asked again.
        const bool retryAnalysis = t.analysisVersion < 0 && retriedIds.insert (t.id).second;
        const bool needAnalysis = retryAnalysis || (t.analysisVersion >= 0 && t.analysisVersion < analysis::analysisVersion);
        const bool needGenre = genreAvailable && t.genreVersion >= 0 && t.genreVersion < genre::detectionVersion;
        if (needAnalysis || needGenre)
            jobs.push_back ({ t.id, t.path, needAnalysis, needGenre });
    }

    // Reuse enqueue()'s bookkeeping.
    std::vector<juce::int64> ids;
    {
        const juce::ScopedLock sl (queueLock);
        if (! workerActive && queue.empty())
            queuedTotal = done = failed = skippedCloud = 0;
        for (auto& j : jobs)
        {
            const bool already = std::any_of (queue.begin(), queue.end(), [&] (const Job& q) { return q.id == j.id; });
            if (! already)
            {
                queue.push_back (j);
                ++queuedTotal;
            }
        }
    }
    if (jobs.empty())
        return 0;
    startWorkerIfNeeded();
    sendChangeMessage();
    return (int) jobs.size();
}

int AnalysisManager::enqueue (const std::vector<juce::int64>& ids, bool force)
{
    const bool genreAvailable = isGenreModelInstalled();
    std::vector<Job> jobs;
    for (auto id : ids)
    {
        const auto* t = library.findTrack (id);
        if (t == nullptr || t->missing)
            continue;
        const bool needAnalysis = force || (t->analysisVersion >= 0 && t->analysisVersion < analysis::analysisVersion);
        const bool needGenre = genreAvailable && (force || (t->genreVersion >= 0 && t->genreVersion < genre::detectionVersion));
        if (needAnalysis || needGenre)
            jobs.push_back ({ id, t->path, needAnalysis, needGenre });
    }
    if (jobs.empty())
        return 0;

    {
        const juce::ScopedLock sl (queueLock);
        if (! workerActive && queue.empty())
            queuedTotal = done = failed = skippedCloud = 0;

        for (auto& j : jobs)
        {
            const bool already = std::any_of (queue.begin(), queue.end(), [&] (const Job& q) { return q.id == j.id; });
            if (! already)
            {
                queue.push_back (j);
                ++queuedTotal;
            }
        }
    }

    startWorkerIfNeeded();
    sendChangeMessage();
    return (int) jobs.size();
}

int AnalysisManager::enqueueGenre (const std::vector<juce::int64>& ids)
{
    if (! isGenreModelInstalled())
        return 0;

    int queued = 0;
    {
        const juce::ScopedLock sl (queueLock);
        if (! workerActive && queue.empty())
            queuedTotal = done = failed = skippedCloud = 0;
        for (auto id : ids)
        {
            const auto* t = library.findTrack (id);
            if (t == nullptr || t->missing)
                continue;
            if (std::any_of (queue.begin(), queue.end(), [id] (const Job& q) { return q.id == id; }))
                continue;
            queue.push_back ({ id, t->path, false, true });
            ++queuedTotal;
            ++queued;
        }
    }
    if (queued > 0)
    {
        startWorkerIfNeeded();
        sendChangeMessage();
    }
    return queued;
}

void AnalysisManager::cancel()
{
    // Drops everything that has not started yet; the job being analysed right now finishes (well under a second).
    const juce::ScopedLock sl (queueLock);
    queue.clear();
}

void AnalysisManager::startWorkerIfNeeded()
{
    {
        const juce::ScopedLock sl (queueLock);
        if (workerActive)
            return;
        workerActive = true;
    }

    // A previous worker has already decided to exit (workerActive was false), so this returns quickly.
    stopThread (30000);
    startThread (juce::Thread::Priority::low);
}

bool AnalysisManager::ensureGenreModelLoaded()
{
    if (genreModelChecked)
        return genreModelUsable;
    genreModelChecked = true;

    const auto file = getGenreModelFile();
    if (! file.existsAsFile() || file.getSize() != genre::ModelInfo::sizeBytes)
        return false;

    // The file must be exactly the model this code was built and tested for (checked once, ~0.1 s).
    if (! ModelDownload::sha256Of (file).equalsIgnoreCase (genre::ModelInfo::sha256))
        return false;

    genreModelUsable = genreClassifier.load (file, {}).wasOk();
    return genreModelUsable;
}

void AnalysisManager::run()
{
    for (;;)
    {
        Job job;
        {
            const juce::ScopedLock sl (queueLock);
            if (queue.empty() || threadShouldExit())
            {
                workerActive = false;
                break;
            }
            job = queue.front();
            queue.pop_front();
        }

        const juce::File file (job.path);
        Result result;

        if (analysis::isOnlineOnly (file))
        {
            result.cloud = true;
        }
        else
        {
            if (job.doAnalysis)
            {
                analysis::MonoAudio audio;
                juce::String error;
                if (analysis::decodeToMono (file, formats, audio, error, nullptr))
                {
                    const auto a = analysis::analyseAudio (audio);
                    auto& u = result.analysis;
                    u.version = analysis::analysisVersion;
                    u.analysedAt = juce::Time::currentTimeMillis();
                    u.fingerprint = analysis::computeFingerprint (file);
                    u.bpm = a.tempo.bpm;
                    u.bpmConfidence = a.tempo.confidence;
                    u.firstBeat = a.tempo.firstBeatSeconds;
                    u.downbeat = a.tempo.downbeatIndex;
                    u.variableTempo = a.tempo.variableTempo;
                    u.key = a.key.camelot;
                    u.keyConfidence = a.key.confidence;
                    u.energy = a.energy;
                    u.loudnessDb = a.loudnessDb;
                    result.analysisOk = true;
                }
            }

            if (job.doGenre && ensureGenreModelLoaded())
            {
                result.genreAttempted = true;
                std::vector<float> audio16k, probabilities;
                juce::String error;
                if (genre::decodeMono16k (file, formats, audio16k, error, nullptr)
                    && genreClassifier.classify (audio16k, probabilities).wasOk())
                {
                    const auto d = genre::interpret (probabilities);
                    result.genre.main = d.main;
                    result.genre.sub = d.sub;
                    result.genre.category = d.category;
                    result.genre.confidence = d.confidence;
                    result.genre.alternativesJson = d.alternativesJson;
                    result.genre.version = genre::detectionVersion;
                    result.genreOk = true;
                }
            }
        }

        juce::MessageManager::callAsync ([weak = juce::WeakReference<AnalysisManager> (this), job, result]
                                         {
                                             if (auto* self = weak.get())
                                                 self->jobFinished (job, result);
                                         });
    }

    juce::MessageManager::callAsync ([weak = juce::WeakReference<AnalysisManager> (this)]
                                     {
                                         if (auto* self = weak.get())
                                             self->sendChangeMessage();
                                     });
}

void AnalysisManager::jobFinished (const Job& job, Result r)
{
    if (r.cloud)
    {
        ++skippedCloud;
        skippedIds.insert (job.id);
        sendChangeMessage();
        return;
    }

    bool ok = true;
    if (job.doAnalysis)
    {
        if (r.analysisOk && library.applyAnalysis (job.id, r.analysis))
        {
            // fine
        }
        else
        {
            ok = false;
            retriedIds.insert (job.id);   // failed in this run: the next try is in the next run
            library.markAnalysisFailed (job.id);
        }
    }

    if (job.doGenre && r.genreAttempted)
    {
        if (r.genreOk)
            library.applyGenre (job.id, r.genre);
        else
            library.markGenreFailed (job.id);   // not counted as a failed track: tempo and key may be fine
    }

    if (ok)
        ++done;
    else
        ++failed;
    sendChangeMessage();
}

} // namespace pathboard
