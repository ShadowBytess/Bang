#include "bang/LibraryStore.hpp"

#include "Sqlite.hpp"
#include "bang/Paths.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

using bang::sql::Statement;
using bang::sql::bindText;
using bang::sql::columnText;
using bang::sql::expectDone;
using bang::sql::expectRow;

namespace bang {

namespace {

constexpr int schemaVersion = 2;

std::int64_t unixTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class Transaction {
public:
    explicit Transaction(sqlite3* database)
        : database_(database)
    {
        if (sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error(
                std::string("SQLite transaction failed: ") + sqlite3_errmsg(database_));
        }
    }

    ~Transaction()
    {
        if (!committed_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit()
    {
        if (sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error(
                std::string("SQLite commit failed: ") + sqlite3_errmsg(database_));
        }
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_ = false;
};

Track readTrack(sqlite3_stmt* statement, int offset = 0)
{
    Track track;
    track.id = sqlite3_column_int64(statement, offset);
    track.contentHash = columnText(statement, offset + 1);
    track.storedExtension = columnText(statement, offset + 9);
    track.title = columnText(statement, offset + 2);
    track.artist = columnText(statement, offset + 3);
    track.album = columnText(statement, offset + 4);
    track.durationMs = sqlite3_column_int64(statement, offset + 5);
    track.source = columnText(statement, offset + 6);
    track.sourceUrl = columnText(statement, offset + 7);
    track.addedAtMs = sqlite3_column_int64(statement, offset + 8);
    return track;
}

const std::string trackColumns =
    "id, content_hash, title, artist, album, duration_ms, source, source_url, "
    "added_at_ms, stored_extension";

} // namespace

LibraryStore::LibraryStore(std::filesystem::path dataDirectory)
    : dataDirectory_(std::move(dataDirectory))
{
    std::error_code error;
    std::filesystem::create_directories(dataDirectory_, error);
    std::filesystem::create_directories(trackFilePath("").parent_path(), error);
    std::filesystem::create_directories(artworkPath("").parent_path(), error);
    std::filesystem::create_directories(temporaryDirectory(), error);

    const auto databasePath = dataDirectory_ / "library.sqlite3";
    if (sqlite3_open(databasePath.c_str(), &database_) != SQLITE_OK) {
        const std::string message = sqlite3_errmsg(database_);
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("cannot open library database: " + message);
    }
    sqlite3_busy_timeout(database_, 5000);
    sqlite3_exec(database_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(database_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    applyMigrations();
}

LibraryStore::~LibraryStore()
{
    if (database_ != nullptr) {
        sqlite3_close(database_);
    }
}

void LibraryStore::applyMigrations()
{
    int version = 0;
    {
        Statement statement(database_, "PRAGMA user_version");
        expectRow(statement.get());
        version = sqlite3_column_int(statement.get(), 0);
    }
    if (version >= schemaVersion) {
        return;
    }

    Transaction transaction(database_);
    sqlite3_exec(database_,
        "CREATE TABLE IF NOT EXISTS tracks("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " content_hash TEXT NOT NULL UNIQUE,"
        " title TEXT NOT NULL,"
        " artist TEXT NOT NULL DEFAULT '',"
        " album TEXT NOT NULL DEFAULT '',"
        " duration_ms INTEGER NOT NULL DEFAULT 0,"
        " source TEXT NOT NULL DEFAULT '',"
        " source_url TEXT NOT NULL DEFAULT '',"
        " added_at_ms INTEGER NOT NULL)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(database_,
        "CREATE TABLE IF NOT EXISTS playlists("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL UNIQUE,"
        " created_at_ms INTEGER NOT NULL)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(database_,
        "CREATE TABLE IF NOT EXISTS playlist_tracks("
        " playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
        " track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
        " position INTEGER NOT NULL,"
        " UNIQUE(playlist_id, track_id))",
        nullptr, nullptr, nullptr);
    sqlite3_exec(database_,
        "CREATE TABLE IF NOT EXISTS favorites("
        " track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,"
        " added_at_ms INTEGER NOT NULL)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(database_,
        "CREATE TABLE IF NOT EXISTS downloads("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " request_url TEXT NOT NULL,"
        " backend TEXT NOT NULL,"
        " status TEXT NOT NULL,"
        " message TEXT NOT NULL DEFAULT '',"
        " track_id INTEGER REFERENCES tracks(id) ON DELETE SET NULL,"
        " started_at_ms INTEGER NOT NULL,"
        " finished_at_ms INTEGER)",
        nullptr, nullptr, nullptr);
    sqlite3_exec(database_,
        "CREATE TABLE IF NOT EXISTS settings("
        " name TEXT PRIMARY KEY,"
        " value TEXT NOT NULL)",
        nullptr, nullptr, nullptr);
    if (version < 2) {
        sqlite3_exec(database_,
            "ALTER TABLE tracks ADD COLUMN stored_extension TEXT NOT NULL DEFAULT '.mp3'",
            nullptr, nullptr, nullptr);
    }

    sqlite3_exec(database_,
        "CREATE INDEX IF NOT EXISTS idx_playlist_tracks_position"
        " ON playlist_tracks(playlist_id, position)",
        nullptr, nullptr, nullptr);

    if (sqlite3_exec(database_,
            ("PRAGMA user_version = " + std::to_string(schemaVersion)).c_str(),
            nullptr, nullptr, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error(
            std::string("SQLite migration failed: ") + sqlite3_errmsg(database_));
    }
    transaction.commit();
}

std::filesystem::path LibraryStore::trackFilePath(std::string_view contentHash) const
{
    return dataDirectory_ / "tracks" / (std::string(contentHash) + ".mp3");
}

std::filesystem::path LibraryStore::trackFilePath(const Track& track) const
{
    const std::string extension = track.storedExtension.empty() ? ".mp3" : track.storedExtension;
    return dataDirectory_ / "tracks" / (track.contentHash + extension);
}

std::filesystem::path LibraryStore::artworkPath(std::string_view contentHash) const
{
    return dataDirectory_ / "artwork" / (std::string(contentHash) + ".jpg");
}

Track LibraryStore::addTrack(const Track& fields)
{
    {
        Statement statement(database_,
            "INSERT OR IGNORE INTO tracks"
            "(content_hash, title, artist, album, duration_ms, source, source_url,"
            " added_at_ms, stored_extension) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
        bindText(statement.get(), 1, fields.contentHash);
        bindText(statement.get(), 2, fields.title);
        bindText(statement.get(), 3, fields.artist);
        bindText(statement.get(), 4, fields.album);
        sqlite3_bind_int64(statement.get(), 5, fields.durationMs);
        bindText(statement.get(), 6, fields.source);
        bindText(statement.get(), 7, fields.sourceUrl);
        sqlite3_bind_int64(statement.get(), 8,
            fields.addedAtMs != 0 ? fields.addedAtMs : unixTimeMs());
        bindText(statement.get(), 9, fields.storedExtension.empty() ? ".mp3" : fields.storedExtension);
        expectDone(database_, statement.get());
    }

    Statement select(database_,
        "SELECT " + trackColumns + " FROM tracks WHERE content_hash = ?");
    bindText(select.get(), 1, fields.contentHash);
    expectRow(select.get());
    return readTrack(select.get());
}

std::optional<Track> LibraryStore::trackByHash(std::string_view hash) const
{
    Statement select(database_,
        "SELECT " + trackColumns + " FROM tracks WHERE content_hash = ?");
    bindText(select.get(), 1, hash);
    if (sqlite3_step(select.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return readTrack(select.get());
}

std::optional<Track> LibraryStore::trackById(std::int64_t trackId) const
{
    Statement select(database_,
        "SELECT " + trackColumns + " FROM tracks WHERE id = ?");
    sqlite3_bind_int64(select.get(), 1, trackId);
    if (sqlite3_step(select.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return readTrack(select.get());
}

std::optional<std::string> LibraryStore::removeTrack(std::int64_t trackId)
{
    std::optional<std::string> hash;
    {
        Statement select(database_,
            "SELECT content_hash FROM tracks WHERE id = ?");
        sqlite3_bind_int64(select.get(), 1, trackId);
        if (sqlite3_step(select.get()) == SQLITE_ROW) {
            hash = columnText(select.get(), 0);
        } else {
            return std::nullopt;
        }
    }
    Statement remove(database_, "DELETE FROM tracks WHERE id = ?");
    sqlite3_bind_int64(remove.get(), 1, trackId);
    expectDone(database_, remove.get());
    return hash;
}

std::int64_t LibraryStore::createPlaylist(const std::string& name)
{
    Statement insert(database_,
        "INSERT INTO playlists(name, created_at_ms) VALUES (?, ?)");
    bindText(insert.get(), 1, name);
    sqlite3_bind_int64(insert.get(), 2, unixTimeMs());
    expectDone(database_, insert.get());
    return sqlite3_last_insert_rowid(database_);
}

void LibraryStore::renamePlaylist(std::int64_t playlistId, const std::string& name)
{
    Statement update(database_, "UPDATE playlists SET name = ? WHERE id = ?");
    bindText(update.get(), 1, name);
    sqlite3_bind_int64(update.get(), 2, playlistId);
    expectDone(database_, update.get());
}

void LibraryStore::deletePlaylist(std::int64_t playlistId)
{
    Statement remove(database_, "DELETE FROM playlists WHERE id = ?");
    sqlite3_bind_int64(remove.get(), 1, playlistId);
    expectDone(database_, remove.get());
}

void LibraryStore::addToPlaylist(std::int64_t playlistId, std::int64_t trackId)
{
    Statement insert(database_,
        "INSERT INTO playlist_tracks(playlist_id, track_id, position)"
        " VALUES (?, ?, COALESCE((SELECT MAX(position) + 1 FROM playlist_tracks"
        " WHERE playlist_id = ?), 0))"
        " ON CONFLICT(playlist_id, track_id) DO NOTHING");
    sqlite3_bind_int64(insert.get(), 1, playlistId);
    sqlite3_bind_int64(insert.get(), 2, trackId);
    sqlite3_bind_int64(insert.get(), 3, playlistId);
    expectDone(database_, insert.get());
}

void LibraryStore::removeFromPlaylist(std::int64_t playlistId, std::int64_t trackId)
{
    std::optional<std::int64_t> removedPosition;
    {
        Statement select(database_,
            "SELECT position FROM playlist_tracks"
            " WHERE playlist_id = ? AND track_id = ?");
        sqlite3_bind_int64(select.get(), 1, playlistId);
        sqlite3_bind_int64(select.get(), 2, trackId);
        if (sqlite3_step(select.get()) == SQLITE_ROW) {
            removedPosition = sqlite3_column_int64(select.get(), 0);
        } else {
            return;
        }
    }
    Transaction transaction(database_);
    {
        Statement remove(database_,
            "DELETE FROM playlist_tracks WHERE playlist_id = ? AND track_id = ?");
        sqlite3_bind_int64(remove.get(), 1, playlistId);
        sqlite3_bind_int64(remove.get(), 2, trackId);
        expectDone(database_, remove.get());
    }
    if (removedPosition.has_value()) {
        Statement compact(database_,
            "UPDATE playlist_tracks SET position = position - 1"
            " WHERE playlist_id = ? AND position > ?");
        sqlite3_bind_int64(compact.get(), 1, playlistId);
        sqlite3_bind_int64(compact.get(), 2, *removedPosition);
        expectDone(database_, compact.get());
    }
    transaction.commit();
}

void LibraryStore::moveWithinPlaylist(
    std::int64_t playlistId, std::size_t oldIndex, std::size_t newIndex)
{
    if (oldIndex == newIndex) {
        return;
    }
    Transaction transaction(database_);
    std::vector<std::int64_t> orderedIds;
    {
        Statement select(database_,
            "SELECT track_id FROM playlist_tracks WHERE playlist_id = ?"
            " ORDER BY position");
        sqlite3_bind_int64(select.get(), 1, playlistId);
        while (sqlite3_step(select.get()) == SQLITE_ROW) {
            orderedIds.push_back(sqlite3_column_int64(select.get(), 0));
        }
    }
    if (oldIndex >= orderedIds.size() || newIndex >= orderedIds.size()) {
        throw std::runtime_error("playlist move out of range");
    }
    const std::int64_t moved = orderedIds[oldIndex];
    orderedIds.erase(orderedIds.begin() + static_cast<std::ptrdiff_t>(oldIndex));
    orderedIds.insert(orderedIds.begin() + static_cast<std::ptrdiff_t>(newIndex),
        moved);

    for (std::size_t index = 0; index < orderedIds.size(); ++index) {
        Statement update(database_,
            "UPDATE playlist_tracks SET position = ? WHERE playlist_id = ?"
            " AND track_id = ?");
        sqlite3_bind_int64(update.get(), 1, static_cast<std::int64_t>(index));
        sqlite3_bind_int64(update.get(), 2, playlistId);
        sqlite3_bind_int64(update.get(), 3, orderedIds[index]);
        expectDone(database_, update.get());
    }
    transaction.commit();
}

void LibraryStore::setFavorite(std::int64_t trackId, bool favorite)
{
    if (favorite) {
        Statement insert(database_,
            "INSERT OR IGNORE INTO favorites(track_id, added_at_ms) VALUES (?, ?)");
        sqlite3_bind_int64(insert.get(), 1, trackId);
        sqlite3_bind_int64(insert.get(), 2, unixTimeMs());
        expectDone(database_, insert.get());
    } else {
        Statement remove(database_, "DELETE FROM favorites WHERE track_id = ?");
        sqlite3_bind_int64(remove.get(), 1, trackId);
        expectDone(database_, remove.get());
    }
}

bool LibraryStore::isFavorite(std::int64_t trackId) const
{
    Statement select(database_, "SELECT 1 FROM favorites WHERE track_id = ?");
    sqlite3_bind_int64(select.get(), 1, trackId);
    return sqlite3_step(select.get()) == SQLITE_ROW;
}

void LibraryStore::setSetting(std::string_view name, std::string_view value)
{
    Statement upsert(database_,
        "INSERT INTO settings(name, value) VALUES (?, ?)"
        " ON CONFLICT(name) DO UPDATE SET value = excluded.value");
    bindText(upsert.get(), 1, name);
    bindText(upsert.get(), 2, value);
    expectDone(database_, upsert.get());
}

std::optional<std::string> LibraryStore::setting(std::string_view name) const
{
    Statement select(database_, "SELECT value FROM settings WHERE name = ?");
    bindText(select.get(), 1, name);
    if (sqlite3_step(select.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    return columnText(select.get(), 0);
}

std::int64_t LibraryStore::beginDownload(
    std::string_view url, std::string_view backend)
{
    Statement insert(database_,
        "INSERT INTO downloads(request_url, backend, status, started_at_ms)"
        " VALUES (?, ?, 'running', ?)");
    bindText(insert.get(), 1, url);
    bindText(insert.get(), 2, backend);
    sqlite3_bind_int64(insert.get(), 3, unixTimeMs());
    expectDone(database_, insert.get());
    return sqlite3_last_insert_rowid(database_);
}

void LibraryStore::completeDownload(std::int64_t downloadId,
    DownloadStatus status, std::string_view message,
    std::optional<std::int64_t> trackId)
{
    const char* statusText = status == DownloadStatus::Completed ? "completed"
        : status == DownloadStatus::Failed                       ? "failed"
                                                                 : "running";
    Statement update(database_,
        "UPDATE downloads SET status = ?, message = ?, track_id = ?,"
        " finished_at_ms = ? WHERE id = ?");
    bindText(update.get(), 1, statusText);
    bindText(update.get(), 2, message);
    if (trackId.has_value()) {
        sqlite3_bind_int64(update.get(), 3, *trackId);
    } else {
        sqlite3_bind_null(update.get(), 3);
    }
    sqlite3_bind_int64(update.get(), 4, unixTimeMs());
    sqlite3_bind_int64(update.get(), 5, downloadId);
    expectDone(database_, update.get());
}

} // namespace bang
