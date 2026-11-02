#pragma once

#include "Library/LibraryDatabase.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <functional>

namespace pathboard
{

// Background folder scanner. Only files that are new or whose size/modification time changed are
// parsed. Files that vanished are flagged as missing; if a missing track's file name and size show up
// elsewhere in the scanned folders, the track is relinked (keeping rating, play count and playlists).
class LibraryScanner : private juce::Thread
{
public:
    struct Summary
    {
        int filesFound = 0;
        int added = 0;
        int updated = 0;
        int unchanged = 0;
        int relinked = 0;
        int markedMissing = 0;
        int restored = 0;
        int failed = 0;
        bool cancelled = false;
        juce::StringArray failedFiles;
    };

    explicit LibraryScanner (LibraryDatabase& database);
    ~LibraryScanner() override;

    // onFinished is called on the message thread. Ignored if a scan is already running.
    bool start (const std::vector<juce::File>& folders, std::function<void (const Summary&)> onFinished);
    void cancel();
    bool isScanning() const { return isThreadRunning(); }

    int getFilesToProcess() const { return toProcess.load(); }
    int getFilesProcessed() const { return processed.load(); }
    int getFilesFound() const { return found.load(); }

private:
    void run() override;

    LibraryDatabase& db;
    std::vector<juce::File> folders;
    std::function<void (const Summary&)> finishedCallback;

    std::atomic<int> found { 0 }, toProcess { 0 }, processed { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (LibraryScanner)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryScanner)
};

} // namespace pathboard
