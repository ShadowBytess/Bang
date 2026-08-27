#include "bang/PlaylistService.hpp"

#include "Sqlite.hpp"
#include "bang/Hash.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string_view>

using bang::sql::Statement;
using bang::sql::columnText;

namespace bang {

namespace {

std::string trim(std::string value)
{
    auto isWhitespace = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    const auto first = std::find_if_not(value.begin(), value.end(), isWhitespace);
    if (first == value.end()) {
        return {};
    }
    const auto last = std::find_if_not(value.rbegin(), value.rend(), isWhitespace).base();
    return std::string(first, last);
}

bool isUrl(std::string_view value)
{
    const auto separator = value.find("://");
    if (separator == std::string_view::npos || separator == 0) {
        return false;
    }
    return std::ranges::all_of(value.substr(0, separator), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '+' || character == '-' || character == '.';
    });
}

bool playlistExists(const LibraryStore& store, std::int64_t playlistId)
{
    Statement statement(store.handle(), "SELECT 1 FROM playlists WHERE id=?");
    sqlite3_bind_int64(statement.get(), 1, playlistId);
    return sqlite3_step(statement.get()) == SQLITE_ROW;
}

std::size_t playlistTrackCount(const LibraryStore& store, std::int64_t playlistId)
{
    Statement statement(store.handle(), "SELECT COUNT(*) FROM playlist_tracks WHERE playlist_id=?");
    sqlite3_bind_int64(statement.get(), 1, playlistId);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return 0;
    }
    return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

} // namespace

PlaylistService::PlaylistService(LibraryStore& store, TrackImporter& importer)
    : store_(&store), importer_(&importer)
{
}

PlaylistService::ImportResult PlaylistService::importM3u(
    const std::filesystem::path& playlistPath, std::int64_t playlistId) const
{
    if (!playlistExists(*store_, playlistId)) {
        throw std::runtime_error("cannot import playlist into missing playlist: "
            + std::to_string(playlistId));
    }

    std::ifstream file(playlistPath);
    if (!file) {
        throw std::runtime_error("cannot open playlist: " + playlistPath.string());
    }

    ImportResult result;
    std::string line;
    bool firstLine = true;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (firstLine && line.starts_with("\xEF\xBB\xBF")) {
            line.erase(0, 3);
        }
        firstLine = false;

        const std::string entryText = trim(std::move(line));
        if (entryText.empty() || entryText.front() == '#' || isUrl(entryText)) {
            continue;
        }

        std::filesystem::path entry{entryText};
        if (entry.is_relative()) {
            entry = playlistPath.parent_path() / entry;
        }
        entry = entry.lexically_normal();

        try {
            const std::string contentHash = sha256File(entry);
            if (const auto existing = store_->trackByHash(contentHash); existing.has_value()) {
                const auto before = playlistTrackCount(*store_, playlistId);
                store_->addToPlaylist(playlistId, existing->id);
                const auto after = playlistTrackCount(*store_, playlistId);
                if (after == before) {
                    ++result.duplicates;
                } else {
                    ++result.imported;
                }
                continue;
            }

            const auto imported = importer_->importFile(entry, {}, "playlist", {});
            store_->addToPlaylist(playlistId, imported.track.id);
            ++result.imported;
        } catch (const std::exception&) {
            ++result.failed;
        }
    }

    return result;
}

bool PlaylistService::exportM3u(
    std::int64_t playlistId, const std::filesystem::path& destination) const
{
    if (!playlistExists(*store_, playlistId)) {
        return false;
    }

    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    file << "#EXTM3U\n";

    Statement tracks(store_->handle(), R"SQL(
        SELECT t.content_hash, t.stored_extension
        FROM playlist_tracks p
        JOIN tracks t ON t.id=p.track_id
        WHERE p.playlist_id=?
        ORDER BY p.position
    )SQL");
    sqlite3_bind_int64(tracks.get(), 1, playlistId);

    while (sqlite3_step(tracks.get()) == SQLITE_ROW) {
        Track track;
        track.contentHash = columnText(tracks.get(), 0);
        track.storedExtension = columnText(tracks.get(), 1);
        file << store_->trackFilePath(track).string() << '\n';
    }

    return file.good();
}

} // namespace bang
