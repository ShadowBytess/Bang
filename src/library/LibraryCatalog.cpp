#include "bang/LibraryCatalog.hpp"

#include "Sqlite.hpp"
#include "bang/LibraryStore.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>

using bang::sql::Statement;
using bang::sql::columnText;

namespace bang {

namespace {

Track readTrack(sqlite3_stmt* statement)
{
    Track track;
    track.id = sqlite3_column_int64(statement, 0);
    track.contentHash = columnText(statement, 1);
    track.storedExtension = columnText(statement, 9);
    track.title = columnText(statement, 2);
    track.artist = columnText(statement, 3);
    track.album = columnText(statement, 4);
    track.durationMs = sqlite3_column_int64(statement, 5);
    track.source = columnText(statement, 6);
    track.sourceUrl = columnText(statement, 7);
    track.addedAtMs = sqlite3_column_int64(statement, 8);
    return track;
}

TrackListing readListing(sqlite3_stmt* statement)
{
    TrackListing listing;
    listing.track = readTrack(statement);
    listing.favorite = sqlite3_column_int(statement, 10) != 0;
    return listing;
}

const std::string listingColumns =
    "t.id, t.content_hash, t.title, t.artist, t.album, t.duration_ms, t.source,"
    " t.source_url, t.added_at_ms, t.stored_extension,"
    " CASE WHEN f.track_id IS NULL THEN 0 ELSE 1 END";

} // namespace

LibraryCatalog::LibraryCatalog(const LibraryStore& store)
    : store_(store)
{
}

std::vector<TrackListing> LibraryCatalog::allTracks() const
{
    Statement statement(store_.handle(),
        "SELECT " + listingColumns + " FROM tracks t"
        " LEFT JOIN favorites f ON f.track_id = t.id"
        " ORDER BY LOWER(t.artist), LOWER(t.album), t.id");
    std::vector<TrackListing> listings;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        listings.push_back(readListing(statement.get()));
    }
    return listings;
}

std::vector<TrackListing> LibraryCatalog::searchTracks(const std::string& query) const
{
    if (query.empty()) {
        return allTracks();
    }
    std::string pattern;
    pattern.reserve(query.size() + 2);
    for (const char character : query) {
        if (character == '%' || character == '_' || character == '\\') {
            pattern.push_back('\\');
        }
        pattern.push_back(character);
    }
    pattern = "%" + pattern + "%";

    Statement statement(store_.handle(),
        "SELECT " + listingColumns + " FROM tracks t"
        " LEFT JOIN favorites f ON f.track_id = t.id"
        " WHERE t.title LIKE ? ESCAPE '\\'"
        " OR t.artist LIKE ? ESCAPE '\\'"
        " OR t.album LIKE ? ESCAPE '\\'"
        " ORDER BY LOWER(t.artist), LOWER(t.album), t.id");
    for (int index = 1; index <= 3; ++index) {
        sqlite3_bind_text(statement.get(), index, pattern.c_str(),
            static_cast<int>(pattern.size()), SQLITE_TRANSIENT);
    }
    std::vector<TrackListing> listings;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        listings.push_back(readListing(statement.get()));
    }
    return listings;
}

std::vector<TrackListing> LibraryCatalog::favorites() const
{
    Statement statement(store_.handle(),
        "SELECT " + listingColumns + " FROM tracks t"
        " JOIN favorites f ON f.track_id = t.id"
        " ORDER BY f.added_at_ms DESC");
    std::vector<TrackListing> listings;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        listings.push_back(readListing(statement.get()));
    }
    return listings;
}

std::vector<Playlist> LibraryCatalog::playlists() const
{
    Statement statement(store_.handle(),
        "SELECT id, name, created_at_ms FROM playlists ORDER BY created_at_ms");
    std::vector<Playlist> playlists;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        Playlist playlist;
        playlist.id = sqlite3_column_int64(statement.get(), 0);
        playlist.name = columnText(statement.get(), 1);
        playlist.createdAtMs = sqlite3_column_int64(statement.get(), 2);
        playlists.push_back(playlist);
    }
    return playlists;
}

std::vector<TrackListing> LibraryCatalog::playlistTracks(
    std::int64_t playlistId) const
{
    Statement statement(store_.handle(),
        "SELECT " + listingColumns + ", pt.position FROM tracks t"
        " JOIN playlist_tracks pt ON pt.track_id = t.id AND pt.playlist_id = ?"
        " LEFT JOIN favorites f ON f.track_id = t.id"
        " ORDER BY pt.position");
    sqlite3_bind_int64(statement.get(), 1, playlistId);
    std::vector<TrackListing> listings;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        listings.push_back(readListing(statement.get()));
    }
    return listings;
}

std::vector<DownloadRow> LibraryCatalog::recentDownloads(std::size_t limit) const
{
    Statement statement(store_.handle(),
        "SELECT d.id, d.request_url, d.backend, d.status, d.message,"
        " d.started_at_ms, t.id, t.content_hash, t.title, t.artist, t.album,"
        " t.duration_ms, t.source, t.source_url, t.added_at_ms, t.stored_extension"
        " FROM downloads d LEFT JOIN tracks t ON t.id = d.track_id"
        " ORDER BY d.id DESC LIMIT ?");
    sqlite3_bind_int64(statement.get(), 1, static_cast<std::int64_t>(limit));
    std::vector<DownloadRow> rows;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        DownloadRow row;
        row.id = sqlite3_column_int64(statement.get(), 0);
        row.requestUrl = columnText(statement.get(), 1);
        row.backend = columnText(statement.get(), 2);
        row.status = columnText(statement.get(), 3);
        row.message = columnText(statement.get(), 4);
        row.startedAtMs = sqlite3_column_int64(statement.get(), 5);
        if (sqlite3_column_type(statement.get(), 6) != SQLITE_NULL) {
            row.hasTrack = true;
            row.track.id = sqlite3_column_int64(statement.get(), 6);
            row.track.contentHash = columnText(statement.get(), 7);
            row.track.title = columnText(statement.get(), 8);
            row.track.artist = columnText(statement.get(), 9);
            row.track.album = columnText(statement.get(), 10);
            row.track.durationMs = sqlite3_column_int64(statement.get(), 11);
            row.track.source = columnText(statement.get(), 12);
            row.track.sourceUrl = columnText(statement.get(), 13);
            row.track.addedAtMs = sqlite3_column_int64(statement.get(), 14);
            row.track.storedExtension = columnText(statement.get(), 15);
        }
        rows.push_back(row);
    }
    return rows;
}

} // namespace bang
