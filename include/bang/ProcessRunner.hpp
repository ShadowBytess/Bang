// Thin wrapper around fork/exec for shelling out to yt-dlp, spotdl, and
// ffmpeg. runStreaming feeds stdout back line by line, which is how
// DownloadService reads the BANGPCT|/BANGDONE| markers (see
// library/DownloadParsing.hpp) while a download is in progress.
// Default timeout is 10 minutes, DownloadService overrides it to an hour.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace bang {

struct ProcessResult {
    int exitCode = -1;
    bool timedOut = false;
    bool cancelled = false;
    std::string standardOutput;
    std::string errorOutput;

    [[nodiscard]] bool succeeded() const { return exitCode == 0 && !timedOut && !cancelled; }
};

struct RunOptions {
    std::string program;
    std::vector<std::string> arguments;
    std::optional<std::filesystem::path> workingDirectory;
    std::chrono::milliseconds timeout{600000};
    std::stop_token stopToken;
};

class ProcessRunner {
public:
    using LineSink = std::function<void(std::string_view line)>;

    static std::optional<std::string> findExecutable(std::string_view name);
    static ProcessResult run(const RunOptions& options);
    static ProcessResult runStreaming(
        const RunOptions& options, const LineSink& onStandardOutputLine);

private:
    static ProcessResult execute(const RunOptions& options, const LineSink& sink);
};

} // namespace bang
