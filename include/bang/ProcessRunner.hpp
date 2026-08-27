#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bang {

struct ProcessResult {
    int exitCode = -1;
    bool timedOut = false;
    std::string standardOutput;
    std::string errorOutput;

    [[nodiscard]] bool succeeded() const { return exitCode == 0 && !timedOut; }
};

struct RunOptions {
    std::string program;
    std::vector<std::string> arguments;
    std::optional<std::filesystem::path> workingDirectory;
    std::chrono::milliseconds timeout{600000};
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
