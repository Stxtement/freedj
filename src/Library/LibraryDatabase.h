#pragma once

#include "Library/TrackRecord.h"
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace pathboard
{

// Thin, thread-safe wrapper around one SQLite connection (WAL mode).
// All public methods lock an internal mutex, so they may be called from any thread,
// but none of them may be called from the audio thread.
class LibraryDatabase
{
public:
    LibraryDatabase();
    ~LibraryDatabase();

    // Opens (and migrates) the database. A corrupt file is moved aside to *.corrupt-<time>
    // and replaced by a fresh database; the returned message then describes what happened.
    juce::Result open (const juce::File& dbFile);
    void close();
    bool isOpen() const;
    juce::String getRecoveryNote() const { return recoveryNote; }

    // Tracks
    std::vector<TrackRecord> loadAllTracks();
    struct KnownFile { juce::int64 id; juce::int64 size; juce::int64 modified; bool missing; };
    std::unordered_map<juce::String, KnownFile> loadKnownFiles();
    // Inserts new tracks and updates existing ones (matched by path). Sets record.id on insert.
    bool upsertTracks (std::vector<TrackRecord>& tracks);
    bool updateTrackInfo (const TrackRecord& track);   // user-editable fields only
    bool setTrackRating (juce::int64 id, int rating);
    bool loadTrack (juce::int64 id, TrackRecord& out);

    // Result of an automatic analysis. Manual corrections (TrackRecord::manualFlags) are never overwritten.
    struct AnalysisUpdate
    {
        int version = 0;
        juce::int64 analysedAt = 0;
        juce::String fingerprint;
        double bpm = 0.0, bpmConfidence = 0.0, firstBeat = 0.0;
        int downbeat = 0;
        bool variableTempo = false;
        juce::String key;
        double keyConfidence = 0.0, energy = 0.0, loudnessDb = 0.0;
    };
    bool saveAnalysis (juce::int64 id, const AnalysisUpdate& update);
    bool markAnalysisFailed (juce::int64 id, juce::int64 timeMs);
    bool requestReanalysis (const std::vector<juce::int64>& ids);
    // Hand-corrected BPM and beat grid (sets the manual flags for BPM and grid).
    bool setManualGrid (juce::int64 id, double bpm, double firstBeat, int downbeat);
    bool clearManualFlags (juce::int64 id, int mask);
    bool setMissing (const std::vector<juce::int64>& ids, bool missing);
    bool relinkTrack (juce::int64 id, const juce::String& newPath, juce::int64 size, juce::int64 modified);
    bool deleteTracks (const std::vector<juce::int64>& ids);
    bool recordPlay (juce::int64 id, juce::int64 timeMs);

    // Folders that are rescanned on demand
    std::vector<juce::String> getMusicFolders() const;
    bool addMusicFolder (const juce::String& path);
    bool removeMusicFolder (const juce::String& path);

    // Playlists
    juce::int64 createPlaylist (const juce::String& name, PlaylistInfo::Kind kind = PlaylistInfo::Kind::playlist,
                                const juce::String& rules = {});
    bool renamePlaylist (juce::int64 id, const juce::String& name);
    bool setPlaylistRules (juce::int64 id, const juce::String& rules);
    bool deletePlaylist (juce::int64 id);
    std::vector<PlaylistInfo> getPlaylists() const;
    std::vector<juce::int64> getPlaylistTrackIds (juce::int64 playlistId) const;
    bool addToPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds);
    bool removeFromPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds);
    bool movePlaylistItem (juce::int64 playlistId, int fromIndex, int toIndex);

    // History
    struct HistoryEntry { juce::int64 trackId; juce::int64 playedAt; };
    std::vector<HistoryEntry> getHistory (int limit) const;

    // Maintenance
    juce::Result checkIntegrity();
    bool backupTo (const juce::File& destination);
    juce::String getLastError() const;

    static constexpr int schemaVersion = 2;

private:
    class Statement;
    static TrackRecord readTrack (const Statement&);
    bool exec (const char* sql);
    juce::Result migrate();
    void setError (const juce::String& message);
    bool beginTransaction();
    bool commit();
    void rollback();

    mutable juce::CriticalSection lock;
    sqlite3* db = nullptr;
    juce::String lastError;
    juce::String recoveryNote;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibraryDatabase)
};

} // namespace pathboard
