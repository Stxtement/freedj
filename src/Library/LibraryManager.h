#pragma once

#include "Library/LibraryDatabase.h"
#include "Library/LibraryScanner.h"
#include "Library/TrackQuery.h"
#include <juce_events/juce_events.h>
#include <unordered_map>

namespace pathboard
{

// Message-thread facade over the library: owns the database, an in-memory copy of all tracks
// (so the UI never queries SQLite while painting or scrolling) and the background scanner.
// Listeners (ChangeBroadcaster) are notified whenever tracks, playlists or scan state change.
class LibraryManager : public juce::ChangeBroadcaster
{
public:
    LibraryManager();
    ~LibraryManager() override;

    juce::Result open (const juce::File& dbFile);
    LibraryDatabase& getDatabase() { return database; }

    // Cache access (message thread only)
    const std::vector<TrackRecord>& getTracks() const { return tracks; }
    const TrackRecord* findTrack (juce::int64 id) const;
    const TrackRecord* findTrackByPath (const juce::String& path) const;
    void reload();   // synchronous; used on start-up and after small edits

    std::vector<int> query (const QuerySpec& spec) const;
    // Playlist contents in playlist order (smart playlists in the spec's sort order). Filters in `spec` apply.
    std::vector<int> queryPlaylist (const PlaylistInfo& playlist, const QuerySpec& spec) const;
    std::vector<int> queryHistory (int limit) const;

    // Scanning
    bool addFolderAndScan (const juce::File& folder);
    bool rescanAll();
    bool isScanning() const { return scanner.isScanning(); }
    void cancelScan() { scanner.cancel(); }
    int getScanProgressPercent() const;
    int getFilesFound() const { return scanner.getFilesFound(); }
    const LibraryScanner::Summary& getLastScanSummary() const { return lastScan; }
    std::vector<juce::File> getMusicFolders() const;

    // Editing
    bool updateTrack (const TrackRecord& edited, bool writeToFile, juce::String* error = nullptr);
    bool setRating (juce::int64 id, int rating);
    bool applyAnalysis (juce::int64 id, const LibraryDatabase::AnalysisUpdate& update);
    bool markAnalysisFailed (juce::int64 id);
    bool applyGenre (juce::int64 id, const LibraryDatabase::GenreUpdate& update);
    bool markGenreFailed (juce::int64 id);
    bool requestReanalysis (const std::vector<juce::int64>& ids);
    bool setManualGrid (juce::int64 id, double bpm, double firstBeat, int downbeat);
    bool resetManualCorrections (juce::int64 id);   // back to the automatic values (re-analysis follows)
    bool recordPlay (juce::int64 id);
    bool removeTracks (const std::vector<juce::int64>& ids);
    // Cleans pool / site clutter out of all titles and artists (see TrackTidy.h). Returns how many tracks changed.
    int tidyAll();

    // Playlists
    std::vector<PlaylistInfo> getPlaylists() const { return database.getPlaylists(); }
    juce::int64 createPlaylist (const juce::String& name);
    juce::int64 createSmartPlaylist (const juce::String& name, const SmartRules& rules);
    bool renamePlaylist (juce::int64 id, const juce::String& name);
    bool deletePlaylist (juce::int64 id);
    bool addToPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds);
    bool removeFromPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds);
    bool movePlaylistItem (juce::int64 playlistId, int from, int to);

    // Maintenance
    std::vector<std::vector<juce::int64>> findDuplicates() const { return findDuplicateGroups (tracks); }
    bool backup (const juce::File& destination) { return database.backupTo (destination); }

private:
    void applyCache (std::vector<TrackRecord>&& loaded);
    bool refreshTrack (juce::int64 id);   // re-reads one row into the cache
    void rebuildIndexes();
    void startScan (const std::vector<juce::File>& folders);

    LibraryDatabase database;
    LibraryScanner scanner { database };
    std::vector<TrackRecord> tracks;
    std::vector<juce::String> searchIndex;
    std::unordered_map<juce::int64, int> indexById;
    std::unordered_map<juce::String, int> indexByPath;
    LibraryScanner::Summary lastScan;

    JUCE_DECLARE_WEAK_REFERENCEABLE (LibraryManager)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryManager)
};

} // namespace pathboard
