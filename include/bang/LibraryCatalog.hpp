#pragma once

#include "bang/Track.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "bang/Playlist.hpp"

namespace bang {

class LibraryStore;

struct DownloadRow {
    std::int64_t id = 0;
    std::string requestUrl;
    std::string backend;
    std::string status;
    std::string message;
    Track track;
    bool hasTrack = false;
    std::int64_t startedAtMs = 0;
};

class LibraryCatalog {
public:
    explicit LibraryCatalog(const LibraryStore& store);

    [[nodiscard]] std::vector<TrackListing> allTracks() const;
    [[nodiscard]] std::vector<TrackListing> searchTracks(
        const std::string& query) const;
    [[nodiscard]] std::vector<TrackListing> favorites() const;

    [[nodiscard]] std::vector<Playlist> playlists() const;
    [[nodiscard]] std::vector<TrackListing> playlistTracks(
        std::int64_t playlistId) const;

    [[nodiscard]] std::vector<DownloadRow> recentDownloads(
        std::size_t limit = 100) const;

private:
    const LibraryStore& store_;
};

} // namespace bang
