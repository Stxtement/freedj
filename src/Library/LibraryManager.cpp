#include "LibraryManager.h"
#include "TrackMetadata.h"

namespace pathboard
{

LibraryManager::LibraryManager() = default;

LibraryManager::~LibraryManager()
{
    scanner.cancel();
}

juce::Result LibraryManager::open (const juce::File& dbFile)
{
    auto result = database.open (dbFile);
    if (result.wasOk())
        reload();
    return result;
}

//==============================================================================
void LibraryManager::reload()
{
    applyCache (database.loadAllTracks());
}

void LibraryManager::applyCache (std::vector<TrackRecord>&& loaded)
{
    tracks = std::move (loaded);
    rebuildIndexes();
    sendChangeMessage();
}

void LibraryManager::rebuildIndexes()
{
    searchIndex.clear();
    indexById.clear();
    indexByPath.clear();
    searchIndex.reserve (tracks.size());
    indexById.reserve (tracks.size());
    indexByPath.reserve (tracks.size());

    for (int i = 0; i < (int) tracks.size(); ++i)
    {
        searchIndex.push_back (buildSearchText (tracks[(size_t) i]));
        indexById[tracks[(size_t) i].id] = i;
        indexByPath[tracks[(size_t) i].path] = i;
    }
}

const TrackRecord* LibraryManager::findTrack (juce::int64 id) const
{
    const auto it = indexById.find (id);
    return it != indexById.end() ? &tracks[(size_t) it->second] : nullptr;
}

const TrackRecord* LibraryManager::findTrackByPath (const juce::String& path) const
{
    const auto it = indexByPath.find (path);
    return it != indexByPath.end() ? &tracks[(size_t) it->second] : nullptr;
}

//==============================================================================
std::vector<int> LibraryManager::query (const QuerySpec& spec) const
{
    return runQuery (tracks, searchIndex, spec);
}

std::vector<int> LibraryManager::queryPlaylist (const PlaylistInfo& playlist, const QuerySpec& spec) const
{
    if (playlist.kind == PlaylistInfo::Kind::smart)
    {
        const auto rules = SmartRules::fromJson (playlist.rules);
        const auto now = juce::Time::currentTimeMillis();
        std::unordered_set<juce::int64> ids;
        for (const auto& t : tracks)
            if (rules.matches (t, now))
                ids.insert (t.id);

        auto smartSpec = spec;
        smartSpec.onlyIds = &ids;
        return runQuery (tracks, searchIndex, smartSpec);
    }

    const auto orderedIds = database.getPlaylistTrackIds (playlist.id);
    const std::unordered_set<juce::int64> idSet (orderedIds.begin(), orderedIds.end());

    auto filterSpec = spec;
    filterSpec.onlyIds = &idSet;
    const auto filtered = runQuery (tracks, searchIndex, filterSpec);
    const std::unordered_set<int> allowed (filtered.begin(), filtered.end());

    std::vector<int> result;
    result.reserve (orderedIds.size());
    for (auto id : orderedIds)
    {
        const auto it = indexById.find (id);
        if (it != indexById.end() && allowed.count (it->second) != 0)
            result.push_back (it->second);
    }
    return result;
}

std::vector<int> LibraryManager::queryHistory (int limit) const
{
    std::vector<int> result;
    for (const auto& entry : database.getHistory (limit))
    {
        const auto it = indexById.find (entry.trackId);
        if (it != indexById.end())
            result.push_back (it->second);
    }
    return result;
}

//==============================================================================
std::vector<juce::File> LibraryManager::getMusicFolders() const
{
    std::vector<juce::File> result;
    for (const auto& path : database.getMusicFolders())
        result.emplace_back (path);
    return result;
}

bool LibraryManager::addFolderAndScan (const juce::File& folder)
{
    if (! folder.isDirectory() || scanner.isScanning())
        return false;
    database.addMusicFolder (folder.getFullPathName());
    startScan ({ folder });
    return true;
}

bool LibraryManager::rescanAll()
{
    if (scanner.isScanning())
        return false;
    const auto folders = getMusicFolders();
    if (folders.empty())
        return false;
    startScan (folders);
    return true;
}

void LibraryManager::startScan (const std::vector<juce::File>& folders)
{
    scanner.start (folders, [weak = juce::WeakReference<LibraryManager> (this)] (const LibraryScanner::Summary& summary)
    {
        if (auto* self = weak.get())
        {
            self->lastScan = summary;
            self->reload();
        }
    });
    sendChangeMessage();
}

int LibraryManager::getScanProgressPercent() const
{
    const int total = scanner.getFilesToProcess();
    return total <= 0 ? 0 : juce::jlimit (0, 100, scanner.getFilesProcessed() * 100 / total);
}

//==============================================================================
bool LibraryManager::updateTrack (const TrackRecord& edited, bool writeToFile, juce::String* error)
{
    const auto it = indexById.find (edited.id);
    if (it == indexById.end())
        return false;

    if (! database.updateTrackInfo (edited))
    {
        if (error != nullptr)
            *error = database.getLastError();
        return false;
    }

    auto& cached = tracks[(size_t) it->second];
    cached.title = edited.title;     cached.artist = edited.artist;   cached.remix = edited.remix;
    cached.album = edited.album;     cached.genre = edited.genre;     cached.comment = edited.comment;
    cached.tags = edited.tags;       cached.key = edited.key;         cached.year = edited.year;
    cached.rating = juce::jlimit (0, 5, edited.rating);               cached.bpm = edited.bpm;
    refreshTrack (edited.id); // picks up the manual flags the database derived from the change

    bool fileOk = true;
    if (writeToFile)
    {
        fileOk = TrackMetadata::writeTags (cached.getFile(), cached);
        if (! fileOk && error != nullptr)
            *error = "Saved in the library, but the tags in the file could not be written (file read-only or in use).";
    }

    sendChangeMessage();
    return fileOk;
}

bool LibraryManager::setRating (juce::int64 id, int rating)
{
    const auto it = indexById.find (id);
    if (it == indexById.end() || ! database.setTrackRating (id, rating))
        return false;
    tracks[(size_t) it->second].rating = juce::jlimit (0, 5, rating);
    sendChangeMessage();
    return true;
}

bool LibraryManager::refreshTrack (juce::int64 id)
{
    const auto it = indexById.find (id);
    TrackRecord fresh;
    if (it == indexById.end() || ! database.loadTrack (id, fresh))
        return false;
    tracks[(size_t) it->second] = std::move (fresh);
    searchIndex[(size_t) it->second] = buildSearchText (tracks[(size_t) it->second]);
    return true;
}

bool LibraryManager::applyAnalysis (juce::int64 id, const LibraryDatabase::AnalysisUpdate& update)
{
    if (! database.saveAnalysis (id, update) || ! refreshTrack (id))
        return false;
    sendChangeMessage();
    return true;
}

bool LibraryManager::markAnalysisFailed (juce::int64 id)
{
    if (! database.markAnalysisFailed (id, juce::Time::currentTimeMillis()) || ! refreshTrack (id))
        return false;
    sendChangeMessage();
    return true;
}

bool LibraryManager::requestReanalysis (const std::vector<juce::int64>& ids)
{
    if (! database.requestReanalysis (ids))
        return false;
    for (auto id : ids)
        refreshTrack (id);
    sendChangeMessage();
    return true;
}

bool LibraryManager::setManualGrid (juce::int64 id, double bpm, double firstBeat, int downbeat)
{
    if (! database.setManualGrid (id, bpm, firstBeat, downbeat) || ! refreshTrack (id))
        return false;
    sendChangeMessage();
    return true;
}

bool LibraryManager::resetManualCorrections (juce::int64 id)
{
    if (! database.clearManualFlags (id, 0xff) || ! database.requestReanalysis ({ id }) || ! refreshTrack (id))
        return false;
    sendChangeMessage();
    return true;
}

bool LibraryManager::recordPlay (juce::int64 id)
{
    const auto it = indexById.find (id);
    const auto now = juce::Time::currentTimeMillis();
    if (it == indexById.end() || ! database.recordPlay (id, now))
        return false;
    auto& t = tracks[(size_t) it->second];
    ++t.playCount;
    t.lastPlayed = now;
    sendChangeMessage();
    return true;
}

bool LibraryManager::removeTracks (const std::vector<juce::int64>& ids)
{
    if (! database.deleteTracks (ids))
        return false;
    reload();
    return true;
}

//==============================================================================
juce::int64 LibraryManager::createPlaylist (const juce::String& name)
{
    const auto id = database.createPlaylist (name);
    if (id != 0)
        sendChangeMessage();
    return id;
}

juce::int64 LibraryManager::createSmartPlaylist (const juce::String& name, const SmartRules& rules)
{
    const auto id = database.createPlaylist (name, PlaylistInfo::Kind::smart, rules.toJson());
    if (id != 0)
        sendChangeMessage();
    return id;
}

bool LibraryManager::renamePlaylist (juce::int64 id, const juce::String& name)
{
    const bool ok = database.renamePlaylist (id, name);
    if (ok) sendChangeMessage();
    return ok;
}

bool LibraryManager::deletePlaylist (juce::int64 id)
{
    const bool ok = database.deletePlaylist (id);
    if (ok) sendChangeMessage();
    return ok;
}

bool LibraryManager::addToPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds)
{
    const bool ok = database.addToPlaylist (playlistId, trackIds);
    if (ok) sendChangeMessage();
    return ok;
}

bool LibraryManager::removeFromPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds)
{
    const bool ok = database.removeFromPlaylist (playlistId, trackIds);
    if (ok) sendChangeMessage();
    return ok;
}

bool LibraryManager::movePlaylistItem (juce::int64 playlistId, int from, int to)
{
    const bool ok = database.movePlaylistItem (playlistId, from, to);
    if (ok) sendChangeMessage();
    return ok;
}

} // namespace pathboard
