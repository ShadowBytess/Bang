#pragma once

#include "bang/Playlist.hpp"
#include "bang/Track.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace bang {

enum class DownloadStatus { Running, Completed, Failed };

struct DownloadRecord {
    std::int64_t id = 0;
    std::string requestUrl;
    std::string backend;
    DownloadStatus status = DownloadStatus::Running;
    std::string message;
    std::optional<std::int64_t> trackId;
    std::int64_t startedAtMs = 0;
    std::optional<std::int64_t> finishedAtMs;
};

class LibraryStore {
public:
    explicit LibraryStore(std::filesystem::path dataDirectory);
    ~LibraryStore();
    LibraryStore(const LibraryStore&) = delete;
    LibraryStore& operator=(const LibraryStore&) = delete;

    [[nodiscard]] std::filesystem::path trackFilePath(
        std::string_view contentHash) const;
    [[nodiscard]] std::filesystem::path trackFilePath(const Track& track) const;
    [[nodiscard]] std::filesystem::path artworkPath(
        std::string_view contentHash) const;

    [[nodiscard]] Track addTrack(const Track& fields);
    [[nodiscard]] std::optional<Track> trackByHash(std::string_view hash) const;
    [[nodiscard]] std::optional<Track> trackById(std::int64_t trackId) const;
    [[nodiscard]] std::optional<std::string> removeTrack(std::int64_t trackId);

    [[nodiscard]] std::int64_t createPlaylist(const std::string& name);
    void renamePlaylist(std::int64_t playlistId, const std::string& name);
    void deletePlaylist(std::int64_t playlistId);
    void addToPlaylist(std::int64_t playlistId, std::int64_t trackId);
    void removeFromPlaylist(std::int64_t playlistId, std::int64_t trackId);
    void moveWithinPlaylist(
        std::int64_t playlistId, std::size_t oldIndex, std::size_t newIndex);

    void setFavorite(std::int64_t trackId, bool favorite);
    [[nodiscard]] bool isFavorite(std::int64_t trackId) const;

    void setSetting(std::string_view name, std::string_view value);
    [[nodiscard]] std::optional<std::string> setting(std::string_view name) const;

    [[nodiscard]] std::int64_t beginDownload(
        std::string_view url, std::string_view backend);
    void completeDownload(std::int64_t downloadId,
        DownloadStatus status, std::string_view message,
        std::optional<std::int64_t> trackId);

    [[nodiscard]] sqlite3* handle() const { return database_; }

private:
    void applyMigrations();

    std::filesystem::path dataDirectory_;
    sqlite3* database_ = nullptr;
};

} // namespace bang
