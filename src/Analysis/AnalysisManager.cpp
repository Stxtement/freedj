#include "AnalysisManager.h"

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
    std::vector<juce::int64> ids;
    for (const auto& t : library.getTracks())
        if (! t.missing && t.analysisVersion >= 0 && t.analysisVersion < analysis::analysisVersion && skippedIds.count (t.id) == 0)
            ids.push_back (t.id);
    return enqueue (ids, false);
}

int AnalysisManager::enqueue (const std::vector<juce::int64>& ids, bool force)
{
    std::vector<Job> jobs;
    for (auto id : ids)
    {
        const auto* t = library.findTrack (id);
        if (t == nullptr || t->missing)
            continue;
        if (! force && t->analysisVersion >= analysis::analysisVersion)
            continue;
        jobs.push_back ({ id, t->path });
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
        LibraryDatabase::AnalysisUpdate update;
        bool ok = false, cloud = false;

        if (analysis::isOnlineOnly (file))
        {
            cloud = true;
        }
        else
        {
            analysis::MonoAudio audio;
            juce::String error;
            if (analysis::decodeToMono (file, formats, audio, error, nullptr))
            {
                const auto result = analysis::analyseAudio (audio);
                update.version = analysis::analysisVersion;
                update.analysedAt = juce::Time::currentTimeMillis();
                update.fingerprint = analysis::computeFingerprint (file);
                update.bpm = result.tempo.bpm;
                update.bpmConfidence = result.tempo.confidence;
                update.firstBeat = result.tempo.firstBeatSeconds;
                update.downbeat = result.tempo.downbeatIndex;
                update.variableTempo = result.tempo.variableTempo;
                update.key = result.key.camelot;
                update.keyConfidence = result.key.confidence;
                update.energy = result.energy;
                update.loudnessDb = result.loudnessDb;
                ok = true;
            }
        }

        juce::MessageManager::callAsync ([weak = juce::WeakReference<AnalysisManager> (this), id = job.id, ok, cloud, update]
                                         {
                                             if (auto* self = weak.get())
                                                 self->jobFinished (id, ok, cloud, update);
                                         });
    }

    juce::MessageManager::callAsync ([weak = juce::WeakReference<AnalysisManager> (this)]
                                     {
                                         if (auto* self = weak.get())
                                             self->sendChangeMessage();
                                     });
}

void AnalysisManager::jobFinished (juce::int64 id, bool ok, bool cloudSkipped, LibraryDatabase::AnalysisUpdate update)
{
    if (cloudSkipped)
    {
        ++skippedCloud;
        skippedIds.insert (id);
    }
    else if (ok && library.applyAnalysis (id, update))
        ++done;
    else
    {
        ++failed;
        library.markAnalysisFailed (id);
    }
    sendChangeMessage();
}

} // namespace pathboard
