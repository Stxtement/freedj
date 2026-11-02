#pragma once

#include "Analysis/AudioAnalysis.h"
#include "Genre/GenreClassifier.h"
#include "Library/LibraryManager.h"
#include <deque>
#include <unordered_set>

namespace pathboard
{

// Analyses tracks in a low-priority background thread and stores the results in the library:
//  - tempo, beat grid, key, energy (analysis::analysisVersion)
//  - genre (genre::detectionVersion), if the genre model is installed
// Progress is kept in the database, so an interrupted run resumes by itself: enqueuePending() queues exactly
// what is missing. Already analysed, unchanged files are never decoded again for the same result.
// OneDrive online-only files are skipped (reading them would download them).
// All public methods are for the message thread. Listeners get a change message when progress changes.
class AnalysisManager : public juce::ChangeBroadcaster, private juce::Thread
{
public:
    AnalysisManager (LibraryManager& library, juce::AudioFormatManager& formats);
    ~AnalysisManager() override;

    // Genre detection needs the Discogs-EffNet model file. Without it, tempo/key analysis works as before.
    void setGenreModelFile (const juce::File& file);
    juce::File getGenreModelFile() const;
    bool isGenreModelInstalled() const;   // right file size (the checksum is verified before it is first used)

    // Queues everything that still needs analysis or genre detection. Returns the number of tracks queued.
    int enqueuePending();
    // Queues the given tracks; with force they are processed again (manual values stay).
    int enqueue (const std::vector<juce::int64>& ids, bool force);
    // Genre detection only (tempo and key are left as they are). Needs the genre model.
    int enqueueGenre (const std::vector<juce::int64>& ids);
    void cancel();
    void retrySkipped() { skippedIds.clear(); }

    bool isBusy() const;
    int getQueued() const { return queuedTotal.load(); }
    int getDone() const { return done.load(); }
    int getFailed() const { return failed.load(); }
    int getSkippedCloud() const { return skippedCloud.load(); }
    int getPercent() const;

private:
    struct Job { juce::int64 id; juce::String path; bool doAnalysis; bool doGenre; };
    struct Result
    {
        bool analysisOk = false, genreOk = false, genreAttempted = false, cloud = false;
        LibraryDatabase::AnalysisUpdate analysis;
        LibraryDatabase::GenreUpdate genre;
    };

    void run() override;
    void startWorkerIfNeeded();
    void jobFinished (const Job& job, Result result);
    bool ensureGenreModelLoaded();

    LibraryManager& library;
    juce::AudioFormatManager& formats;

    juce::CriticalSection queueLock;
    std::deque<Job> queue;
    std::unordered_set<juce::int64> skippedIds;   // message thread only
    std::unordered_set<juce::int64> retriedIds;   // failed analyses already retried in this run (message thread only)
    bool workerActive = false;
    juce::File genreModelFile;

    genre::GenreClassifier genreClassifier;
    bool genreModelChecked = false, genreModelUsable = false;   // worker thread only

    std::atomic<int> queuedTotal { 0 }, done { 0 }, failed { 0 }, skippedCloud { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (AnalysisManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalysisManager)
};

} // namespace pathboard
