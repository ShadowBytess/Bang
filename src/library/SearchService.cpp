#include "bang/SearchService.hpp"

#include "DownloadParsing.hpp"
#include "bang/ProcessRunner.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace bang {

namespace {

std::string trimToLimit(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.size() > 200) {
        text.resize(200);
        text += "…";
    }
    return text;
}

} // namespace

std::vector<SearchService::Hit> SearchService::searchYouTube(
    const std::string& query, std::size_t limit) const
{
    if (query.empty()) {
        return {};
    }
    const auto program = ProcessRunner::findExecutable("yt-dlp");
    if (!program.has_value()) {
        throw std::runtime_error("yt-dlp is not installed or not on PATH");
    }

    RunOptions options;
    options.program = *program;
    options.arguments = download::ytDlpSearchArguments(query, limit);
    options.timeout = std::chrono::seconds { 60 };

    const ProcessResult result = ProcessRunner::run(options);
    if (!result.succeeded()) {
        throw std::runtime_error(result.errorOutput.empty()
                ? "YouTube search failed"
                : "YouTube search failed: " + trimToLimit(result.errorOutput));
    }
    std::vector<Hit> hits;
    for (auto& parsed : download::parseSearchOutput(result.standardOutput)) {
        Hit hit;
        hit.videoId = std::move(parsed.videoId);
        hit.title = std::move(parsed.title);
        hit.uploader = std::move(parsed.uploader);
        hit.durationSec = parsed.durationSec;
        hits.push_back(std::move(hit));
    }
    return hits;
}

} // namespace bang
