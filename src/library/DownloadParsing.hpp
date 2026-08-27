#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bang::download {

constexpr const char* progressPrefix = "BANGPCT|";
constexpr const char* donePrefix = "BANGDONE|";

struct DoneItem {
    std::string filePath;
    std::string title;
    std::string uploader;
    std::int64_t durationSec = 0;
};

enum class LineKind { Other, Progress, Done };

LineKind classifyLine(
    const std::string& line, double* progressPercent, DoneItem* doneItem);

std::vector<std::string> ytDlpArguments(
    const std::string& url, const std::filesystem::path& workDirectory);

struct SearchHit {
    std::string videoId;
    std::string title;
    std::string uploader;
    std::int64_t durationSec = 0;
};

std::vector<std::string> ytDlpSearchArguments(
    const std::string& query, std::size_t limit);

std::vector<SearchHit> parseSearchOutput(const std::string& output);

} // namespace bang::download
