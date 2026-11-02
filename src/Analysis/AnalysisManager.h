#pragma once

#include "Analysis/AudioAnalysis.h"
#include "Library/LibraryManager.h"
#include <deque>
#include <unordered_set>

namespace pathboard
{

// Analyses tracks in a low-priority background thread and stores the results in the library.
//  - Progress is kept in the database (analysisVersion), so an interrupted run resumes by itself:
//    enqueuePending() queues everything that is not analysed with the current algorithm version.
//  - Unchanged, already analysed files are never decoded again.
//  - OneDrive online-only files are skipped (reading them would download them).
// All public methods are for the message thread. Listeners get a change message when progress changes.
class AnalysisManager : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    AnalysisManager (LibraryManager& library, juce::AudioFormatManager& formats);
    ~AnalysisManager() override;

    // Queues every track that still needs analysis. Returns the number queued.
    int enqueuePending();
    // Queues the given tracks; with force they are analysed again even if up to date (manual values stay).
    int enqueue (const std::vector<juce::int64>& ids, bool force);
    void cancel();
    // Forget which cloud-only tracks were skipped, e.g. after the user made them available offline.
    void retrySkipped() { skippedIds.clear(); }

    bool isBusy() const;
    int getQueued() const { return queuedTotal.load(); }   // jobs accepted since the queue was last empty
    int getDone() const { return done.load(); }
    int getFailed() const { return failed.load(); }
    int getSkippedCloud() const { return skippedCloud.load(); }
    int getPercent() const;

private:
    struct Job { juce::int64 id; juce::String path; };

    void run() override;
    void startWorkerIfNeeded();
    void jobFinished (juce::int64 id, bool ok, bool cloudSkipped, LibraryDatabase::AnalysisUpdate update);

    LibraryManager& library;
    juce::AudioFormatManager& formats;

    juce::CriticalSection queueLock;
    std::deque<Job> queue;
    std::unordered_set<juce::int64> skippedIds;   // message thread only
    bool workerActive = false;

    std::atomic<int> queuedTotal { 0 }, done { 0 }, failed { 0 }, skippedCloud { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (AnalysisManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisManager)
};

} // namespace pathboard
