#pragma once

#include "bang/LibraryStore.hpp"
#include "bang/MetadataService.hpp"
#include "bang/Track.hpp"

#include <filesystem>
#include <string>

namespace bang {

class TrackImporter {
public:
    struct Result {
        Track track;
        bool isNew = false;
    };

    explicit TrackImporter(LibraryStore& store);

    [[nodiscard]] Result importFile(const std::filesystem::path& sourceFile,
        const AudioMetadata& overrides, const std::string& source,
        const std::string& sourceUrl) const;

    static void deleteStoredFiles(
        const LibraryStore& store, const std::string& contentHash);

private:
    LibraryStore* store_;
};

} // namespace bang
