#include "bang/TrackImporter.hpp"

#include "bang/Hash.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <system_error>

namespace bang {

namespace {

std::string normalizedExtension(const std::filesystem::path& file)
{
    std::string extension = file.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension;
}

bool isModuleFile(const std::filesystem::path& file)
{
    return normalizedExtension(file) == ".mod";
}

} // namespace

TrackImporter::TrackImporter(LibraryStore& store)
    : store_(&store)
{
}

TrackImporter::Result TrackImporter::importFile(const std::filesystem::path& sourceFile,
    const AudioMetadata& overrides, const std::string& source,
    const std::string& sourceUrl) const
{
    if (!std::filesystem::is_regular_file(sourceFile)) {
        throw std::runtime_error("cannot import missing file: "
            + sourceFile.string());
    }

    const std::string contentHash = sha256File(sourceFile);
    const bool moduleFile = isModuleFile(sourceFile);
    const std::string storedExtension = moduleFile ? ".mod" : ".mp3";
    AudioMetadata metadata;
    try {
        metadata = MetadataService::read(sourceFile);
    } catch (const std::exception&) {
        metadata.title = sourceFile.stem().string();
    }
    if (!overrides.title.empty()) {
        metadata.title = overrides.title;
    } else if (metadata.title.empty()) {
        metadata.title = sourceFile.stem().string();
    }
    if (!overrides.artist.empty()) {
        metadata.artist = overrides.artist;
    }
    if (!overrides.album.empty()) {
        metadata.album = overrides.album;
    }

    Track storedFields;
    storedFields.contentHash = contentHash;
    storedFields.storedExtension = storedExtension;
    const auto storedPath = store_->trackFilePath(storedFields);
    const bool isNew = !std::filesystem::exists(storedPath);
    if (isNew) {
        std::filesystem::copy_file(
            sourceFile, storedPath, std::filesystem::copy_options::none);
    }

    try {
        if (!moduleFile) {
            MetadataService::write(storedPath,
            [&] {
                AudioMetadata fields;
                fields.title = metadata.title;
                fields.artist = metadata.artist;
                fields.album = metadata.album;
                return fields;
            }());
        }
        if (!moduleFile && !metadata.artwork.empty()) {
            MetadataService::embedArtwork(storedPath, metadata.artwork);
            std::ofstream artworkFile(store_->artworkPath(contentHash),
                std::ios::binary);
            artworkFile.write(
                reinterpret_cast<const char*>(metadata.artwork.data()),
                static_cast<std::streamsize>(metadata.artwork.size()));
        }
    } catch (const std::exception&) {
        if (isNew) {
            std::error_code ignored;
            std::filesystem::remove(storedPath, ignored);
        }
        throw;
    }

    Track fields;
    fields.contentHash = contentHash;
    fields.storedExtension = storedExtension;
    fields.title = metadata.title;
    fields.artist = metadata.artist;
    fields.album = metadata.album;
    fields.durationMs = overrides.durationMs != 0 ? overrides.durationMs
                                                  : metadata.durationMs;
    fields.source = source;
    fields.sourceUrl = sourceUrl;
    Result result;
    result.track = store_->addTrack(fields);
    result.isNew = isNew;
    return result;
}

void TrackImporter::deleteStoredFiles(
    const LibraryStore& store, const std::string& contentHash)
{
    std::error_code ignored;
    std::filesystem::remove(store.trackFilePath(contentHash), ignored);
    std::filesystem::remove(store.trackFilePath(Track{.contentHash = contentHash, .storedExtension = ".mod"}), ignored);
    std::filesystem::remove(store.artworkPath(contentHash), ignored);
}

} // namespace bang
