#include "DownloadParsing.hpp"

#include <cstdlib>
#include <sstream>

namespace bang::download {

namespace {

std::vector<std::string> split(const std::string& line, char separator)
{
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream(line);
    while (std::getline(stream, current, separator)) {
        parts.push_back(current);
    }
    return parts;
}

double parseLeadingNumber(const std::string& text)
{
    const char* begin = text.c_str();
    while (*begin != '\0' && (*begin == ' ' || *begin == '\t')) {
        ++begin;
    }
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin) {
        return 0.0;
    }
    return value;
}

} // namespace

LineKind classifyLine(
    const std::string& line, double* progressPercent, DoneItem* doneItem)
{
    if (line.rfind(progressPrefix, 0) == 0) {
        if (progressPercent != nullptr) {
            *progressPercent = parseLeadingNumber(
                line.substr(std::char_traits<char>::length(progressPrefix)));
        }
        return LineKind::Progress;
    }
    if (line.rfind(donePrefix, 0) != 0) {
        return LineKind::Other;
    }
    if (doneItem == nullptr) {
        return LineKind::Done;
    }

    const auto parts = split(line.substr(std::char_traits<char>::length(donePrefix)),
        '|');
    if (parts.size() < 4) {
        return LineKind::Other;
    }
    doneItem->filePath = parts[0];
    if (parts.size() >= 4) {
        doneItem->uploader = parts[parts.size() - 2];
        std::string title = parts[1];
        for (std::size_t index = 2; index + 2 < parts.size(); ++index) {
            title += "|" + parts[index];
        }
        doneItem->title = title;
        doneItem->durationSec =
            static_cast<std::int64_t>(parseLeadingNumber(parts.back()));
    }
    return LineKind::Done;
}

std::vector<std::string> ytDlpArguments(
    const std::string& url, const std::filesystem::path& workDirectory)
{
    return {
        url,
        "-x",
        "--audio-format",
        "mp3",
        "--audio-quality",
        "0",
        "--embed-metadata",
        "--embed-thumbnail",
        "--yes-playlist",
        "--newline",
        "--no-simulate",
        "--progress",
        "--progress-template",
        std::string("download:") + progressPrefix + "%(progress._percent_str)s",
        "--print",
        std::string("after_move:") + donePrefix + "%(filepath)s|%(title)s"
            + "|%(uploader,channel)s|%(duration)s",
        "--paths",
        workDirectory.string(),
    };
}

std::vector<std::string> ytDlpSearchArguments(
    const std::string& query, std::size_t limit)
{
    return {
        "--flat-playlist",
        "--print",
        "%(id)s\t%(title)s\t%(uploader,channel)s\t%(duration)s",
        "ytsearch" + std::to_string(limit) + ":" + query,
    };
}

std::vector<SearchHit> parseSearchOutput(const std::string& output)
{
    std::vector<SearchHit> hits;
    for (const auto& rawLine : split(output, '\n')) {
        std::string line = rawLine;
        while (!line.empty() && (line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty() || line.starts_with("[")) {
            continue;
        }
        std::size_t position = 0;
        std::size_t separator = line.find('\t');
        auto nextField = [&]() -> std::string {
            if (separator == std::string::npos) {
                const std::string remainder = line.substr(position);
                position = line.size();
                return remainder;
            }
            const std::string field = line.substr(position, separator - position);
            position = separator + 1;
            separator = line.find('\t', position);
            return field;
        };

        SearchHit hit;
        hit.videoId = nextField();
        hit.title = nextField();
        hit.uploader = nextField();
        const std::string durationText = nextField();
        if (hit.videoId.empty()) {
            continue;
        }
        hit.durationSec =
            static_cast<std::int64_t>(parseLeadingNumber(durationText));
        hits.push_back(std::move(hit));
    }
    return hits;
}

} // namespace bang::download
