#include "bang/MetadataService.hpp"

#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tbytevector.h>
#include <taglib/tstring.h>

#include <stdexcept>

namespace bang {

namespace {

TagLib::String toTagString(const std::string& value)
{
    return TagLib::String(value, TagLib::String::UTF8);
}

std::string fromTagString(const TagLib::String& value)
{
    return value.to8Bit(true);
}

bool looksLikePng(const std::vector<std::byte>& image)
{
    return image.size() >= 8 && static_cast<unsigned char>(image[0]) == 0x89
        && static_cast<unsigned char>(image[1]) == 0x50;
}

TagLib::ID3v2::Tag* id3v2Tag(TagLib::MPEG::File& file)
{
    if (!file.ID3v2Tag()) {
        file.tag();
    }
    return file.ID3v2Tag(true);
}

} // namespace

AudioMetadata MetadataService::read(const std::filesystem::path& file)
{
    TagLib::MPEG::File mpegFile(file.c_str());
    if (!mpegFile.isValid()) {
        throw std::runtime_error("not a readable MPEG file: " + file.string());
    }

    AudioMetadata metadata;
    if (const auto* properties = mpegFile.audioProperties()) {
        metadata.durationMs =
            static_cast<std::int64_t>(properties->lengthInMilliseconds());
    }
    if (const auto* tag = mpegFile.ID3v2Tag(false)) {
        metadata.title = fromTagString(tag->title());
        metadata.artist = fromTagString(tag->artist());
        metadata.album = fromTagString(tag->album());
        for (const auto& frame : tag->frameListMap()["APIC"]) {
            const auto* picture =
                dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frame);
            if (picture != nullptr && !picture->picture().isEmpty()) {
                const auto& data = picture->picture();
                metadata.artwork.reserve(data.size());
                for (const char byte : data) {
                    metadata.artwork.push_back(
                        static_cast<std::byte>(static_cast<unsigned char>(byte)));
                }
                break;
            }
        }
    } else if (const auto* tag = mpegFile.tag()) {
        metadata.title = fromTagString(tag->title());
        metadata.artist = fromTagString(tag->artist());
        metadata.album = fromTagString(tag->album());
    }
    return metadata;
}

void MetadataService::write(const std::filesystem::path& file,
    const AudioMetadata& fields)
{
    TagLib::MPEG::File mpegFile(file.c_str());
    if (!mpegFile.isValid()) {
        throw std::runtime_error("cannot tag invalid MPEG file: " + file.string());
    }
    auto* tag = id3v2Tag(mpegFile);
    tag->setTitle(toTagString(fields.title));
    tag->setArtist(toTagString(fields.artist));
    tag->setAlbum(toTagString(fields.album));
    if (!mpegFile.save()) {
        throw std::runtime_error("failed to write tags: " + file.string());
    }
}

void MetadataService::embedArtwork(
    const std::filesystem::path& file, const std::vector<std::byte>& image)
{
    if (image.empty()) {
        return;
    }
    TagLib::MPEG::File mpegFile(file.c_str());
    if (!mpegFile.isValid()) {
        throw std::runtime_error(
            "cannot embed artwork into invalid MPEG file: " + file.string());
    }
    auto* tag = id3v2Tag(mpegFile);
    auto* picture = new TagLib::ID3v2::AttachedPictureFrame;
    picture->setMimeType(looksLikePng(image) ? "image/png" : "image/jpeg");
    picture->setType(TagLib::ID3v2::AttachedPictureFrame::FrontCover);
    picture->setPicture(TagLib::ByteVector(
        reinterpret_cast<const char*>(image.data()),
        static_cast<unsigned int>(image.size())));
    tag->removeFrames("APIC");
    tag->addFrame(picture);
    if (!mpegFile.save()) {
        throw std::runtime_error("failed to embed artwork: " + file.string());
    }
}

} // namespace bang
