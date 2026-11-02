#include "LibraryScanner.h"
#include "TrackMetadata.h"
#include <unordered_set>

namespace pathboard
{

namespace
{
    struct FoundFile
    {
        juce::String path;
        juce::int64 size = 0;
        juce::int64 modified = 0;
    };

    juce::String moveKey (const juce::String& fileName, juce::int64 size)
    {
        return fileName.toLowerCase() + "|" + juce::String (size);
    }

    constexpr int batchSize = 200;
}

LibraryScanner::LibraryScanner (LibraryDatabase& database)
    : juce::Thread ("Library scanner"), db (database)
{
}

LibraryScanner::~LibraryScanner()
{
    stopThread (30000);
}

bool LibraryScanner::start (const std::vector<juce::File>& foldersToScan, std::function<void (const Summary&)> onFinished)
{
    if (isThreadRunning())
        return false;

    folders = foldersToScan;
    finishedCallback = std::move (onFinished);
    found = toProcess = processed = 0;
    startThread (juce::Thread::Priority::low);
    return true;
}

void LibraryScanner::cancel()
{
    signalThreadShouldExit();
}

void LibraryScanner::run()
{
    Summary summary;

    // 1. Walk the folders.
    std::vector<FoundFile> files;
    for (const auto& folder : folders)
    {
        if (threadShouldExit())
            break;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        {
            if (threadShouldExit())
                break;

            const auto file = entry.getFile();
            if (! TrackMetadata::isSupportedAudioFile (file))
                continue;

            files.push_back ({ file.getFullPathName(), entry.getFileSize(), entry.getModificationTime().toMilliseconds() });
            found = (int) files.size();
        }
    }

    summary.filesFound = (int) files.size();

    if (threadShouldExit())
    {
        summary.cancelled = true;
    }
    else
    {
        // 2. Compare with the database.
        auto known = db.loadKnownFiles();
        std::unordered_set<juce::String> seen;
        seen.reserve (files.size());

        std::vector<FoundFile> toRead;
        std::vector<juce::int64> restoreIds;

        for (const auto& f : files)
        {
            seen.insert (f.path);
            const auto it = known.find (f.path);
            if (it == known.end() || it->second.size != f.size || it->second.modified != f.modified)
            {
                toRead.push_back (f);
            }
            else
            {
                ++summary.unchanged;
                if (it->second.missing)
                    restoreIds.push_back (it->second.id);
            }
        }

        if (! restoreIds.empty() && db.setMissing (restoreIds, false))
            summary.restored = (int) restoreIds.size();

        // 3. Tracks whose file vanished from the scanned folders.
        std::vector<juce::int64> vanishedIds;
        std::unordered_map<juce::String, std::pair<juce::int64, juce::String>> moveCandidates; // key -> (id, path)
        for (const auto& [path, info] : known)
        {
            if (seen.count (path) != 0)
                continue;

            bool underScannedFolder = false;
            for (const auto& folder : folders)
            {
                const auto prefix = folder.getFullPathName().trimCharactersAtEnd ("\\/") + juce::File::getSeparatorString();
                if (path.startsWithIgnoreCase (prefix))
                {
                    underScannedFolder = true;
                    break;
                }
            }
            if (! underScannedFolder)
                continue;

            if (! info.missing)
                vanishedIds.push_back (info.id);
            moveCandidates.emplace (moveKey (juce::File (path).getFileName(), info.size), std::make_pair (info.id, path));
        }

        // 4. Moved files: same name and size as a vanished track -> relink instead of adding a duplicate.
        std::vector<FoundFile> reallyNew;
        std::unordered_set<juce::int64> relinkedIds;
        for (const auto& f : toRead)
        {
            if (known.find (f.path) == known.end())
            {
                const auto it = moveCandidates.find (moveKey (juce::File (f.path).getFileName(), f.size));
                if (it != moveCandidates.end() && db.relinkTrack (it->second.first, f.path, f.size, f.modified))
                {
                    relinkedIds.insert (it->second.first);
                    moveCandidates.erase (it);
                    ++summary.relinked;
                    continue;
                }
            }
            reallyNew.push_back (f);
        }

        vanishedIds.erase (std::remove_if (vanishedIds.begin(), vanishedIds.end(),
                                           [&] (juce::int64 id) { return relinkedIds.count (id) != 0; }),
                           vanishedIds.end());
        if (! vanishedIds.empty() && db.setMissing (vanishedIds, true))
            summary.markedMissing = (int) vanishedIds.size();

        // 5. Read tags for new/changed files and write them in batches.
        toProcess = (int) reallyNew.size();
        std::vector<TrackRecord> batch;
        batch.reserve (batchSize);
        const auto now = juce::Time::currentTimeMillis();

        auto flush = [&]
        {
            if (batch.empty())
                return;
            if (! db.upsertTracks (batch))
            {
                summary.failed += (int) batch.size();
                for (const auto& t : batch)
                    summary.failedFiles.add (t.path);
            }
            batch.clear();
        };

        for (const auto& f : reallyNew)
        {
            if (threadShouldExit())
            {
                summary.cancelled = true;
                break;
            }

            TrackRecord record;
            if (TrackMetadata::read (juce::File (f.path), record))
            {
                record.firstSeen = now;
                if (known.find (f.path) != known.end()) ++summary.updated; else ++summary.added;
                batch.push_back (std::move (record));
                if ((int) batch.size() >= batchSize)
                    flush();
            }
            else
            {
                ++summary.failed;
                summary.failedFiles.add (f.path);
            }
            ++processed;
        }
        flush();
    }

    auto callback = std::move (finishedCallback);
    finishedCallback = nullptr;

    if (callback)
        juce::MessageManager::callAsync ([weak = juce::WeakReference<LibraryScanner> (this),
                                          callback = std::move (callback), summary]
                                         {
                                             if (weak != nullptr)
                                                 callback (summary);
                                         });
}

} // namespace pathboard
