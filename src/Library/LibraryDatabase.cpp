#include "LibraryDatabase.h"
#include <sqlite3.h>

namespace pathboard
{

namespace
{
    constexpr const char* trackColumns =
        "id,path,title,artist,remix,album,genre,comment,tags,key,format,year,rating,bitrate,play_count,"
        "bpm,length,size,modified,first_seen,last_played,missing,analysed,"
        "analysis_version,analysed_at,fingerprint,bpm_conf,key_conf,first_beat,downbeat,variable,energy,loudness,manual_flags,"
        "genre_main,genre_sub,genre_category,genre_conf,genre_alt,genre_version";

    const char* schemaSql = R"sql(
        CREATE TABLE tracks (
            id          INTEGER PRIMARY KEY,
            path        TEXT NOT NULL UNIQUE,
            title       TEXT NOT NULL DEFAULT '',
            artist      TEXT NOT NULL DEFAULT '',
            remix       TEXT NOT NULL DEFAULT '',
            album       TEXT NOT NULL DEFAULT '',
            genre       TEXT NOT NULL DEFAULT '',
            comment     TEXT NOT NULL DEFAULT '',
            tags        TEXT NOT NULL DEFAULT '',
            key         TEXT NOT NULL DEFAULT '',
            format      TEXT NOT NULL DEFAULT '',
            year        INTEGER NOT NULL DEFAULT 0,
            rating      INTEGER NOT NULL DEFAULT 0,
            bitrate     INTEGER NOT NULL DEFAULT 0,
            play_count  INTEGER NOT NULL DEFAULT 0,
            bpm         REAL NOT NULL DEFAULT 0,
            length      REAL NOT NULL DEFAULT 0,
            size        INTEGER NOT NULL DEFAULT 0,
            modified    INTEGER NOT NULL DEFAULT 0,
            first_seen  INTEGER NOT NULL DEFAULT 0,
            last_played INTEGER NOT NULL DEFAULT 0,
            missing     INTEGER NOT NULL DEFAULT 0,
            analysed    INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE folders (path TEXT PRIMARY KEY);
        CREATE TABLE playlists (
            id    INTEGER PRIMARY KEY,
            name  TEXT NOT NULL,
            kind  INTEGER NOT NULL DEFAULT 0,
            rules TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE playlist_items (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
            track_id    INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
            position    INTEGER NOT NULL
        );
        CREATE INDEX idx_playlist_items ON playlist_items(playlist_id, position);
        CREATE TABLE history (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            track_id  INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
            played_at INTEGER NOT NULL
        );
        CREATE INDEX idx_history_time ON history(played_at);
    )sql";

    const char* migrationV2Sql = R"sql(
        ALTER TABLE tracks ADD COLUMN analysis_version INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN analysed_at INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN fingerprint TEXT NOT NULL DEFAULT '';
        ALTER TABLE tracks ADD COLUMN bpm_conf REAL NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN key_conf REAL NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN first_beat REAL NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN downbeat INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN variable INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN energy REAL NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN loudness REAL NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN manual_flags INTEGER NOT NULL DEFAULT 0;
        CREATE INDEX idx_tracks_fingerprint ON tracks(fingerprint);
    )sql";

    // Automatic genre detection lives in its own columns: the user's `genre` is never touched by it.
    const char* migrationV3Sql = R"sql(
        ALTER TABLE tracks ADD COLUMN genre_main TEXT NOT NULL DEFAULT '';
        ALTER TABLE tracks ADD COLUMN genre_sub TEXT NOT NULL DEFAULT '';
        ALTER TABLE tracks ADD COLUMN genre_category TEXT NOT NULL DEFAULT '';
        ALTER TABLE tracks ADD COLUMN genre_conf REAL NOT NULL DEFAULT 0;
        ALTER TABLE tracks ADD COLUMN genre_alt TEXT NOT NULL DEFAULT '';
        ALTER TABLE tracks ADD COLUMN genre_version INTEGER NOT NULL DEFAULT 0;
    )sql";
}

//==============================================================================
class LibraryDatabase::Statement
{
public:
    Statement (sqlite3* d, const char* sql)
    {
        if (sqlite3_prepare_v2 (d, sql, -1, &stmt, nullptr) != SQLITE_OK)
            error = sqlite3_errmsg (d);
    }
    ~Statement() { sqlite3_finalize (stmt); }

    bool ok() const { return stmt != nullptr; }
    juce::String getError() const { return error; }

    void bind (int i, const juce::String& v) { sqlite3_bind_text (stmt, i, v.toRawUTF8(), -1, SQLITE_TRANSIENT); }
    void bind (int i, juce::int64 v) { sqlite3_bind_int64 (stmt, i, v); }
    void bind (int i, int v) { sqlite3_bind_int (stmt, i, v); }
    void bind (int i, double v) { sqlite3_bind_double (stmt, i, v); }

    int step() { return sqlite3_step (stmt); }
    void reset() { sqlite3_reset (stmt); sqlite3_clear_bindings (stmt); }

    juce::String text (int c) const
    {
        auto* t = sqlite3_column_text (stmt, c);
        return t != nullptr ? juce::String::fromUTF8 (reinterpret_cast<const char*> (t)) : juce::String();
    }
    juce::int64 int64 (int c) const { return sqlite3_column_int64 (stmt, c); }
    int integer (int c) const { return sqlite3_column_int (stmt, c); }
    double real (int c) const { return sqlite3_column_double (stmt, c); }

private:
    sqlite3_stmt* stmt = nullptr;
    juce::String error;
};

//==============================================================================
LibraryDatabase::LibraryDatabase() = default;
LibraryDatabase::~LibraryDatabase() { close(); }

bool LibraryDatabase::isOpen() const
{
    const juce::ScopedLock sl (lock);
    return db != nullptr;
}

juce::String LibraryDatabase::getLastError() const
{
    const juce::ScopedLock sl (lock);
    return lastError;
}

void LibraryDatabase::setError (const juce::String& message)
{
    lastError = message;
    DBG ("LibraryDatabase error: " << message);
}

bool LibraryDatabase::exec (const char* sql)
{
    char* msg = nullptr;
    if (sqlite3_exec (db, sql, nullptr, nullptr, &msg) != SQLITE_OK)
    {
        setError (juce::String (msg != nullptr ? msg : "unknown") + " [" + juce::String (sql).substring (0, 60) + "]");
        sqlite3_free (msg);
        return false;
    }
    return true;
}

bool LibraryDatabase::beginTransaction() { return exec ("BEGIN IMMEDIATE"); }
bool LibraryDatabase::commit()            { return exec ("COMMIT"); }
void LibraryDatabase::rollback()          { sqlite3_exec (db, "ROLLBACK", nullptr, nullptr, nullptr); }

//==============================================================================
juce::Result LibraryDatabase::open (const juce::File& dbFile)
{
    const juce::ScopedLock sl (lock);
    close();
    recoveryNote.clear();

    if (! dbFile.getParentDirectory().createDirectory())
        return juce::Result::fail ("Cannot create folder " + dbFile.getParentDirectory().getFullPathName());

    auto tryOpen = [this, &dbFile]() -> juce::Result
    {
        if (sqlite3_open_v2 (dbFile.getFullPathName().toRawUTF8(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
        {
            auto msg = juce::String (db != nullptr ? sqlite3_errmsg (db) : "cannot open");
            sqlite3_close (db);
            db = nullptr;
            return juce::Result::fail (msg);
        }

        sqlite3_busy_timeout (db, 5000);

        if (! exec ("PRAGMA journal_mode=WAL") || ! exec ("PRAGMA synchronous=NORMAL") || ! exec ("PRAGMA foreign_keys=ON"))
            return juce::Result::fail (lastError);

        {
            Statement check (db, "PRAGMA quick_check");
            if (! check.ok() || check.step() != SQLITE_ROW || check.text (0) != "ok")
                return juce::Result::fail ("integrity check failed: " + (check.ok() ? check.text (0) : check.getError()));
        }

        return migrate();
    };

    auto result = tryOpen();
    if (result.wasOk())
        return result;

    const auto firstError = result.getErrorMessage();
    close();

    if (! dbFile.existsAsFile())
        return result;

    // Automatic recovery: keep the damaged file for inspection, start with an empty database.
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
    const auto corrupt = dbFile.getSiblingFile (dbFile.getFileName() + ".corrupt-" + stamp);
    if (! dbFile.moveFileTo (corrupt))
        return juce::Result::fail ("Database is damaged (" + firstError + ") and could not be moved aside");

    dbFile.getSiblingFile (dbFile.getFileName() + "-wal").deleteFile();
    dbFile.getSiblingFile (dbFile.getFileName() + "-shm").deleteFile();

    result = tryOpen();
    if (result.wasOk())
        recoveryNote = "Library database was damaged (" + firstError + "). It was moved to " + corrupt.getFileName()
                       + " and a new database was created. Rescan your music folders.";
    return result;
}

void LibraryDatabase::close()
{
    const juce::ScopedLock sl (lock);
    if (db != nullptr)
    {
        sqlite3_close (db);
        db = nullptr;
    }
}

juce::Result LibraryDatabase::migrate()
{
    int version = 0;
    {
        Statement s (db, "PRAGMA user_version");
        if (s.ok() && s.step() == SQLITE_ROW)
            version = s.integer (0);
    }

    if (version > schemaVersion)
        return juce::Result::fail ("Database was created by a newer version (schema " + juce::String (version) + ")");

    if (version < schemaVersion)
    {
        // One transaction: a crash half-way leaves the old schema untouched.
        if (! beginTransaction())
            return juce::Result::fail (lastError);

        bool ok = true;
        if (version < 1)
            ok = exec (schemaSql);
        if (ok && version < 2)
            ok = exec (migrationV2Sql);
        if (ok && version < 3)
            ok = exec (migrationV3Sql);
        if (ok)
            ok = exec (("PRAGMA user_version=" + juce::String (schemaVersion)).toRawUTF8());

        if (! ok)
        {
            rollback();
            return juce::Result::fail (lastError);
        }
        if (! commit())
            return juce::Result::fail (lastError);
    }

    return juce::Result::ok();
}

//==============================================================================
TrackRecord LibraryDatabase::readTrack (const Statement& s)
{
    TrackRecord t;
    int c = 0;
    t.id = s.int64 (c++);
    t.path = s.text (c++);
    t.title = s.text (c++);
    t.artist = s.text (c++);
    t.remix = s.text (c++);
    t.album = s.text (c++);
    t.genre = s.text (c++);
    t.comment = s.text (c++);
    t.tags = s.text (c++);
    t.key = s.text (c++);
    t.format = s.text (c++);
    t.year = s.integer (c++);
    t.rating = s.integer (c++);
    t.bitrate = s.integer (c++);
    t.playCount = s.integer (c++);
    t.bpm = s.real (c++);
    t.lengthSeconds = s.real (c++);
    t.fileSize = s.int64 (c++);
    t.modified = s.int64 (c++);
    t.firstSeen = s.int64 (c++);
    t.lastPlayed = s.int64 (c++);
    t.missing = s.integer (c++) != 0;
    t.analysed = s.integer (c++) != 0;
    t.analysisVersion = s.integer (c++);
    t.analysedAt = s.int64 (c++);
    t.fingerprint = s.text (c++);
    t.bpmConfidence = s.real (c++);
    t.keyConfidence = s.real (c++);
    t.firstBeat = s.real (c++);
    t.downbeat = s.integer (c++);
    t.variableTempo = s.integer (c++) != 0;
    t.energy = s.real (c++);
    t.loudnessDb = s.real (c++);
    t.manualFlags = s.integer (c++);
    t.genreAutoMain = s.text (c++);
    t.genreAutoSub = s.text (c++);
    t.genreAutoCategory = s.text (c++);
    t.genreConfidence = s.real (c++);
    t.genreAlternatives = s.text (c++);
    t.genreVersion = s.integer (c++);
    return t;
}

bool LibraryDatabase::loadTrack (juce::int64 id, TrackRecord& out)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, (juce::String ("SELECT ") + trackColumns + " FROM tracks WHERE id=?1").toRawUTF8());
    if (! s.ok())
        return false;
    s.bind (1, id);
    if (s.step() != SQLITE_ROW)
        return false;
    out = readTrack (s);
    return true;
}

std::vector<TrackRecord> LibraryDatabase::loadAllTracks()
{
    const juce::ScopedLock sl (lock);
    std::vector<TrackRecord> result;
    if (db == nullptr)
        return result;

    Statement count (db, "SELECT COUNT(*) FROM tracks");
    if (count.ok() && count.step() == SQLITE_ROW)
        result.reserve ((size_t) count.int64 (0));

    Statement s (db, (juce::String ("SELECT ") + trackColumns + " FROM tracks").toRawUTF8());
    if (! s.ok())
    {
        setError (s.getError());
        return result;
    }

    while (s.step() == SQLITE_ROW)
        result.push_back (readTrack (s));
    return result;
}

std::unordered_map<juce::String, LibraryDatabase::KnownFile> LibraryDatabase::loadKnownFiles()
{
    const juce::ScopedLock sl (lock);
    std::unordered_map<juce::String, KnownFile> result;
    if (db == nullptr)
        return result;

    Statement s (db, "SELECT path,id,size,modified,missing,fingerprint FROM tracks");
    while (s.ok() && s.step() == SQLITE_ROW)
        result[s.text (0)] = { s.int64 (1), s.int64 (2), s.int64 (3), s.integer (4) != 0, s.text (5) };
    return result;
}

bool LibraryDatabase::upsertTracks (std::vector<TrackRecord>& tracks)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;

    // Re-reading a changed file refreshes tag data but keeps what the user built up in the library
    // (rating, play count, tags, first seen) and keeps an analysed BPM/key if the file has none.
    Statement s (db,
        "INSERT INTO tracks (path,title,artist,remix,album,genre,comment,key,format,year,bitrate,bpm,length,size,modified,first_seen,missing) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,0) "
        "ON CONFLICT(path) DO UPDATE SET title=?2,artist=?3,remix=?4,album=?5,comment=?7,format=?9,year=?10,"
        "bitrate=?11,length=?13,missing=0,"
        // A changed file invalidates its analysis; the new content has to be analysed again. Only the
        // timestamp changing (cloud sync, copying) does not: the content fingerprint (?17, given by the
        // scanner) still equals the stored one, so the results stay.
        "analysis_version=CASE WHEN (modified<>?15 OR size<>?14) AND NOT (fingerprint<>'' AND fingerprint=?17) THEN 0 ELSE analysis_version END,"
        "genre_version=CASE WHEN (modified<>?15 OR size<>?14) AND NOT (fingerprint<>'' AND fingerprint=?17) THEN 0 ELSE genre_version END,"
        "size=?14,modified=?15,"
        // Hand-corrected values are never replaced by what the file's tags say.
        "genre=CASE WHEN (manual_flags&8)<>0 THEN genre ELSE ?6 END,"
        "key=CASE WHEN (manual_flags&2)<>0 OR ?8='' THEN key ELSE ?8 END,"
        "bpm=CASE WHEN (manual_flags&1)<>0 OR ?12<=0 THEN bpm ELSE ?12 END "
        "RETURNING id");

    if (! s.ok())
    {
        setError (s.getError());
        return false;
    }

    if (! beginTransaction())
        return false;

    for (auto& t : tracks)
    {
        s.bind (1, t.path);      s.bind (2, t.title);   s.bind (3, t.artist);  s.bind (4, t.remix);
        s.bind (5, t.album);     s.bind (6, t.genre);   s.bind (7, t.comment); s.bind (8, t.key);
        s.bind (9, t.format);    s.bind (10, t.year);   s.bind (11, t.bitrate); s.bind (12, t.bpm);
        s.bind (13, t.lengthSeconds); s.bind (14, t.fileSize); s.bind (15, t.modified);
        s.bind (16, t.firstSeen);  s.bind (17, t.fingerprint);

        if (s.step() != SQLITE_ROW)
        {
            setError (sqlite3_errmsg (db));
            s.reset();
            rollback();
            return false;
        }
        t.id = s.int64 (0);
        while (s.step() == SQLITE_ROW) {}
        s.reset();
    }

    return commit();
}

bool LibraryDatabase::updateTrackInfo (const TrackRecord& t)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;

    // Fields the user changes by hand are flagged so that analysis and re-scans leave them alone.
    // (All right-hand sides see the old row, so the comparisons below compare new against stored values.)
    Statement s (db, "UPDATE tracks SET manual_flags=manual_flags"
                     " | (CASE WHEN abs(?12-bpm)>0.005 THEN 1 ELSE 0 END)"
                     " | (CASE WHEN ?9<>key THEN 2 ELSE 0 END)"
                     " | (CASE WHEN ?6<>genre THEN 8 ELSE 0 END),"
                     "title=?2,artist=?3,remix=?4,album=?5,genre=?6,comment=?7,tags=?8,key=?9,"
                     "year=?10,rating=?11,bpm=?12 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }

    s.bind (1, t.id);   s.bind (2, t.title);  s.bind (3, t.artist);  s.bind (4, t.remix);
    s.bind (5, t.album); s.bind (6, t.genre); s.bind (7, t.comment); s.bind (8, t.tags);
    s.bind (9, t.key);  s.bind (10, t.year);  s.bind (11, juce::jlimit (0, 5, t.rating)); s.bind (12, t.bpm);

    if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); return false; }
    return true;
}

bool LibraryDatabase::setTrackRating (juce::int64 id, int rating)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET rating=?2 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    s.bind (2, juce::jlimit (0, 5, rating));
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::saveAnalysis (juce::int64 id, const AnalysisUpdate& u)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;

    // bpm/key/grid: never over a manual value; an empty result keeps what is there; a weak tempo result
    // does not replace a BPM that was already known (from the file's tag or an earlier analysis).
    Statement s (db,
        "UPDATE tracks SET analysis_version=?2,analysed_at=?3,fingerprint=?4,bpm_conf=?5,key_conf=?6,"
        "variable=?8,energy=?9,loudness=?10,analysed=1,"
        "bpm=CASE WHEN (manual_flags&1)<>0 OR ?11<=0 OR (bpm>0 AND ?5<0.5) THEN bpm ELSE ?11 END,"
        "key=CASE WHEN (manual_flags&2)<>0 OR ?12='' THEN key ELSE ?12 END,"
        "first_beat=CASE WHEN (manual_flags&4)<>0 THEN first_beat ELSE ?13 END,"
        "downbeat=CASE WHEN (manual_flags&4)<>0 THEN downbeat ELSE ?7 END "
        "WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }

    s.bind (1, id);          s.bind (2, u.version);   s.bind (3, u.analysedAt);  s.bind (4, u.fingerprint);
    s.bind (5, u.bpmConfidence); s.bind (6, u.keyConfidence); s.bind (7, u.downbeat);
    s.bind (8, u.variableTempo ? 1 : 0);  s.bind (9, u.energy);  s.bind (10, u.loudnessDb);
    s.bind (11, u.bpm);      s.bind (12, u.key);      s.bind (13, u.firstBeat);

    if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); return false; }
    return true;
}

bool LibraryDatabase::saveGenre (juce::int64 id, const GenreUpdate& u)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET genre_main=?2,genre_sub=?3,genre_category=?4,genre_conf=?5,genre_alt=?6,genre_version=?7 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);       s.bind (2, u.main);       s.bind (3, u.sub);      s.bind (4, u.category);
    s.bind (5, u.confidence);  s.bind (6, u.alternativesJson);  s.bind (7, u.version);
    if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); return false; }
    return true;
}

bool LibraryDatabase::markGenreFailed (juce::int64 id)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET genre_version=-1 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::markAnalysisFailed (juce::int64 id, juce::int64 timeMs)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET analysis_version=-1,analysed_at=?2 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    s.bind (2, timeMs);
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::requestReanalysis (const std::vector<juce::int64>& ids)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET analysis_version=0,genre_version=0 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    if (! beginTransaction())
        return false;
    for (auto id : ids)
    {
        s.bind (1, id);
        if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); s.reset(); rollback(); return false; }
        s.reset();
    }
    return commit();
}

bool LibraryDatabase::setManualGrid (juce::int64 id, double bpm, double firstBeat, int downbeat)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET bpm=?2,first_beat=?3,downbeat=?4,manual_flags=manual_flags|5 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    s.bind (2, bpm);
    s.bind (3, firstBeat);
    s.bind (4, ((downbeat % 4) + 4) % 4);
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::clearManualFlags (juce::int64 id, int mask)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET manual_flags=manual_flags&~?2 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    s.bind (2, mask);
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::setMissing (const std::vector<juce::int64>& ids, bool missing)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET missing=?2 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    if (! beginTransaction())
        return false;
    for (auto id : ids)
    {
        s.bind (1, id);
        s.bind (2, missing ? 1 : 0);
        if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); s.reset(); rollback(); return false; }
        s.reset();
    }
    return commit();
}

bool LibraryDatabase::relinkTrack (juce::int64 id, const juce::String& newPath, juce::int64 size, juce::int64 modified)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE tracks SET path=?2,size=?3,modified=?4,missing=0 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    s.bind (2, newPath);
    s.bind (3, size);
    s.bind (4, modified);
    if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); return false; }
    return true;
}

bool LibraryDatabase::deleteTracks (const std::vector<juce::int64>& ids)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "DELETE FROM tracks WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    if (! beginTransaction())
        return false;
    for (auto id : ids)
    {
        s.bind (1, id);
        if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); s.reset(); rollback(); return false; }
        s.reset();
    }
    return commit();
}

bool LibraryDatabase::recordPlay (juce::int64 id, juce::int64 timeMs)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    if (! beginTransaction())
        return false;

    bool ok = false;
    {
        Statement update (db, "UPDATE tracks SET play_count=play_count+1,last_played=?2 WHERE id=?1");
        Statement insert (db, "INSERT INTO history (track_id,played_at) VALUES (?1,?2)");
        if (update.ok() && insert.ok())
        {
            update.bind (1, id); update.bind (2, timeMs);
            insert.bind (1, id); insert.bind (2, timeMs);
            ok = update.step() == SQLITE_DONE && insert.step() == SQLITE_DONE;
        }
    }
    if (! ok)
    {
        setError (sqlite3_errmsg (db));
        rollback();
        return false;
    }
    return commit();
}

//==============================================================================
std::vector<juce::String> LibraryDatabase::getMusicFolders() const
{
    const juce::ScopedLock sl (lock);
    std::vector<juce::String> result;
    if (db == nullptr)
        return result;
    Statement s (db, "SELECT path FROM folders ORDER BY path");
    while (s.ok() && s.step() == SQLITE_ROW)
        result.push_back (s.text (0));
    return result;
}

bool LibraryDatabase::addMusicFolder (const juce::String& path)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "INSERT OR IGNORE INTO folders (path) VALUES (?1)");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, path);
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::removeMusicFolder (const juce::String& path)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "DELETE FROM folders WHERE path=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, path);
    return s.step() == SQLITE_DONE;
}

//==============================================================================
juce::int64 LibraryDatabase::createPlaylist (const juce::String& name, PlaylistInfo::Kind kind, const juce::String& rules)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return 0;
    Statement s (db, "INSERT INTO playlists (name,kind,rules) VALUES (?1,?2,?3)");
    if (! s.ok()) { setError (s.getError()); return 0; }
    s.bind (1, name);
    s.bind (2, (int) kind);
    s.bind (3, rules);
    if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); return 0; }
    return sqlite3_last_insert_rowid (db);
}

bool LibraryDatabase::renamePlaylist (juce::int64 id, const juce::String& name)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE playlists SET name=?2 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    s.bind (2, name);
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::setPlaylistRules (juce::int64 id, const juce::String& rules)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "UPDATE playlists SET rules=?2 WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    s.bind (2, rules);
    return s.step() == SQLITE_DONE;
}

bool LibraryDatabase::deletePlaylist (juce::int64 id)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    Statement s (db, "DELETE FROM playlists WHERE id=?1");
    if (! s.ok()) { setError (s.getError()); return false; }
    s.bind (1, id);
    return s.step() == SQLITE_DONE;
}

std::vector<PlaylistInfo> LibraryDatabase::getPlaylists() const
{
    const juce::ScopedLock sl (lock);
    std::vector<PlaylistInfo> result;
    if (db == nullptr)
        return result;
    Statement s (db, "SELECT id,name,kind,rules FROM playlists ORDER BY name COLLATE NOCASE");
    while (s.ok() && s.step() == SQLITE_ROW)
        result.push_back ({ s.int64 (0), s.text (1), (PlaylistInfo::Kind) s.integer (2), s.text (3) });
    return result;
}

std::vector<juce::int64> LibraryDatabase::getPlaylistTrackIds (juce::int64 playlistId) const
{
    const juce::ScopedLock sl (lock);
    std::vector<juce::int64> result;
    if (db == nullptr)
        return result;
    Statement s (db, "SELECT track_id FROM playlist_items WHERE playlist_id=?1 ORDER BY position,id");
    if (! s.ok())
        return result;
    s.bind (1, playlistId);
    while (s.step() == SQLITE_ROW)
        result.push_back (s.int64 (0));
    return result;
}

bool LibraryDatabase::addToPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    if (! beginTransaction())
        return false;

    int position = 0;
    {
        Statement m (db, "SELECT COALESCE(MAX(position),-1)+1 FROM playlist_items WHERE playlist_id=?1");
        if (! m.ok()) { rollback(); return false; }
        m.bind (1, playlistId);
        if (m.step() == SQLITE_ROW)
            position = m.integer (0);
    }

    Statement s (db, "INSERT INTO playlist_items (playlist_id,track_id,position) VALUES (?1,?2,?3)");
    if (! s.ok()) { setError (s.getError()); rollback(); return false; }

    for (auto trackId : trackIds)
    {
        s.bind (1, playlistId);
        s.bind (2, trackId);
        s.bind (3, position++);
        if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); s.reset(); rollback(); return false; }
        s.reset();
    }
    return commit();
}

bool LibraryDatabase::removeFromPlaylist (juce::int64 playlistId, const std::vector<juce::int64>& trackIds)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    if (! beginTransaction())
        return false;

    {
        Statement s (db, "DELETE FROM playlist_items WHERE playlist_id=?1 AND track_id=?2");
        if (! s.ok()) { setError (s.getError()); rollback(); return false; }
        for (auto trackId : trackIds)
        {
            s.bind (1, playlistId);
            s.bind (2, trackId);
            if (s.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); s.reset(); rollback(); return false; }
            s.reset();
        }
    }

    // Close the gaps so positions stay 0..n-1.
    std::vector<juce::int64> itemIds;
    {
        Statement q (db, "SELECT id FROM playlist_items WHERE playlist_id=?1 ORDER BY position,id");
        if (! q.ok()) { rollback(); return false; }
        q.bind (1, playlistId);
        while (q.step() == SQLITE_ROW)
            itemIds.push_back (q.int64 (0));
    }
    Statement u (db, "UPDATE playlist_items SET position=?2 WHERE id=?1");
    if (! u.ok()) { setError (u.getError()); rollback(); return false; }
    int pos = 0;
    for (auto itemId : itemIds)
    {
        u.bind (1, itemId);
        u.bind (2, pos++);
        if (u.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); u.reset(); rollback(); return false; }
        u.reset();
    }
    return commit();
}

bool LibraryDatabase::movePlaylistItem (juce::int64 playlistId, int fromIndex, int toIndex)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;
    if (! beginTransaction())
        return false;

    std::vector<juce::int64> itemIds;
    {
        Statement q (db, "SELECT id FROM playlist_items WHERE playlist_id=?1 ORDER BY position,id");
        if (! q.ok()) { rollback(); return false; }
        q.bind (1, playlistId);
        while (q.step() == SQLITE_ROW)
            itemIds.push_back (q.int64 (0));
    }

    const int n = (int) itemIds.size();
    if (! juce::isPositiveAndBelow (fromIndex, n) || ! juce::isPositiveAndBelow (toIndex, n))
    {
        rollback();
        return false;
    }

    const auto moved = itemIds[(size_t) fromIndex];
    itemIds.erase (itemIds.begin() + fromIndex);
    itemIds.insert (itemIds.begin() + toIndex, moved);

    Statement u (db, "UPDATE playlist_items SET position=?2 WHERE id=?1");
    if (! u.ok()) { setError (u.getError()); rollback(); return false; }
    int pos = 0;
    for (auto itemId : itemIds)
    {
        u.bind (1, itemId);
        u.bind (2, pos++);
        if (u.step() != SQLITE_DONE) { setError (sqlite3_errmsg (db)); u.reset(); rollback(); return false; }
        u.reset();
    }
    return commit();
}

std::vector<LibraryDatabase::HistoryEntry> LibraryDatabase::getHistory (int limit) const
{
    const juce::ScopedLock sl (lock);
    std::vector<HistoryEntry> result;
    if (db == nullptr)
        return result;
    Statement s (db, "SELECT track_id,played_at FROM history ORDER BY played_at DESC,id DESC LIMIT ?1");
    if (! s.ok())
        return result;
    s.bind (1, limit);
    while (s.step() == SQLITE_ROW)
        result.push_back ({ s.int64 (0), s.int64 (1) });
    return result;
}

//==============================================================================
juce::Result LibraryDatabase::checkIntegrity()
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return juce::Result::fail ("database is not open");
    Statement s (db, "PRAGMA integrity_check");
    if (! s.ok())
        return juce::Result::fail (s.getError());
    juce::StringArray problems;
    while (s.step() == SQLITE_ROW)
        problems.add (s.text (0));
    return problems.size() == 1 && problems[0] == "ok" ? juce::Result::ok()
                                                      : juce::Result::fail (problems.joinIntoString ("; "));
}

bool LibraryDatabase::backupTo (const juce::File& destination)
{
    const juce::ScopedLock sl (lock);
    if (db == nullptr)
        return false;

    destination.getParentDirectory().createDirectory();
    destination.deleteFile();

    sqlite3* target = nullptr;
    if (sqlite3_open_v2 (destination.getFullPathName().toRawUTF8(), &target,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        setError (target != nullptr ? sqlite3_errmsg (target) : "cannot create backup file");
        sqlite3_close (target);
        return false;
    }

    bool ok = false;
    if (auto* backup = sqlite3_backup_init (target, "main", db, "main"))
    {
        ok = sqlite3_backup_step (backup, -1) == SQLITE_DONE;
        sqlite3_backup_finish (backup);
    }
    if (! ok)
        setError (sqlite3_errmsg (target));

    sqlite3_close (target);
    return ok;
}

} // namespace pathboard
