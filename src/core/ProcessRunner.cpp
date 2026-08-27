#include "bang/ProcessRunner.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace bang {

namespace {

std::vector<char*> buildArgv(const RunOptions& options, std::vector<std::string>& storage)
{
    storage.push_back(options.program);
    storage.insert(storage.end(), options.arguments.begin(), options.arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (const std::string& argument : storage) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

void writeAll(int descriptor, const char* data, std::size_t size)
{
    while (size > 0) {
        const ssize_t written = ::write(descriptor, data, size);
        if (written <= 0) {
            if (written < 0 && errno == EINTR) {
                continue;
            }
            _exit(126);
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

} // namespace

std::optional<std::string> ProcessRunner::findExecutable(std::string_view name)
{
    if (name.find('/') != std::string_view::npos) {
        return std::string(name);
    }
    const char* pathVariable = std::getenv("PATH");
    if (pathVariable == nullptr || *pathVariable == '\0') {
        return std::nullopt;
    }
    std::string_view paths = pathVariable;
    while (!paths.empty()) {
        const std::size_t separator = paths.find(':');
        const std::string_view directory = paths.substr(0, separator);
        std::string candidate(directory.empty() ? std::string_view(".") : directory);
        if (!candidate.empty() && candidate.back() != '/') {
            candidate.push_back('/');
        }
        candidate.append(name);
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        paths.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

ProcessResult ProcessRunner::run(const RunOptions& options)
{
    return execute(options, nullptr);
}

ProcessResult ProcessRunner::runStreaming(
    const RunOptions& options, const LineSink& onStandardOutputLine)
{
    return execute(options, onStandardOutputLine);
}

ProcessResult ProcessRunner::execute(const RunOptions& options, const LineSink& sink)
{
    int standardOutputPipe[2];
    int errorOutputPipe[2];
    if (::pipe(standardOutputPipe) != 0 || ::pipe(errorOutputPipe) != 0) {
        throw std::runtime_error("pipe() failed for child process");
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(standardOutputPipe[0]);
        ::close(standardOutputPipe[1]);
        ::close(errorOutputPipe[0]);
        ::close(errorOutputPipe[1]);
        throw std::runtime_error("fork() failed");
    }

    if (pid == 0) {
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::close(devNull);
        }
        ::dup2(standardOutputPipe[1], STDOUT_FILENO);
        ::dup2(errorOutputPipe[1], STDERR_FILENO);
        ::close(standardOutputPipe[0]);
        ::close(standardOutputPipe[1]);
        ::close(errorOutputPipe[0]);
        ::close(errorOutputPipe[1]);

        if (options.workingDirectory
            && ::chdir(options.workingDirectory->c_str()) != 0) {
            _exit(125);
        }

        std::vector<std::string> storage;
        const std::vector<char*> argv = buildArgv(options, storage);
        ::execvp(argv[0], argv.data());
        writeAll(STDERR_FILENO, "bang: exec failed\n", 18);
        _exit(127);
    }

    ::close(standardOutputPipe[1]);
    ::close(errorOutputPipe[1]);

    int readFds[2] = {standardOutputPipe[0], errorOutputPipe[0]};
    ProcessResult result;
    std::string pendingStandardLine;

    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    bool childrenStreamsClosed = false;

    auto drainDescriptor = [&](int descriptor, std::string& target,
                               bool streamToSink) {
        std::array<char, 4096> chunk{};
        while (true) {
            const ssize_t received =
                ::read(descriptor, chunk.data(), chunk.size());
            if (received > 0) {
                target.append(chunk.data(), static_cast<std::size_t>(received));
                if (streamToSink && sink) {
                    pendingStandardLine.append(
                        chunk.data(), static_cast<std::size_t>(received));
                    std::size_t newline = pendingStandardLine.find('\n');
                    while (newline != std::string::npos) {
                        std::string_view line(pendingStandardLine.data(), newline);
                        if (!line.empty() && line.back() == '\r') {
                            line.remove_suffix(1);
                        }
                        sink(line);
                        pendingStandardLine.erase(0, newline + 1);
                        newline = pendingStandardLine.find('\n');
                    }
                }
                continue;
            }
            if (received < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    };

    while (!childrenStreamsClosed) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();

        struct pollfd polled[2] = {
            {.fd = readFds[0], .events = POLLIN, .revents = 0},
            {.fd = readFds[1], .events = POLLIN, .revents = 0},
        };
        const int readyCount =
            ::poll(polled, 2, static_cast<int>(std::min<long>(remainingMs, 250)));
        if (readyCount < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        bool anyOpen = false;
        for (int index = 0; index < 2; ++index) {
            if (polled[index].fd < 0) {
                continue;
            }
            if ((polled[index].revents & (POLLIN | POLLHUP)) != 0) {
                const std::size_t before = index == 0
                    ? result.standardOutput.size()
                    : result.errorOutput.size();
                if (index == 0) {
                    drainDescriptor(readFds[0], result.standardOutput, true);
                } else {
                    drainDescriptor(readFds[1], result.errorOutput, false);
                }
                const std::size_t after = index == 0
                    ? result.standardOutput.size()
                    : result.errorOutput.size();
                if (after == before) {
                    ::close(readFds[index]);
                    polled[index].fd = -1;
                    readFds[index] = -1;
                    continue;
                }
                anyOpen = true;
            } else if (
                (polled[index].revents & (POLLERR | POLLNVAL)) != 0) {
                ::close(readFds[index]);
                polled[index].fd = -1;
                readFds[index] = -1;
                continue;
            } else {
                anyOpen = true;
            }
        }
        if (!anyOpen) {
            childrenStreamsClosed = true;
        }
    }

    for (int descriptor : readFds) {
        if (descriptor >= 0) {
            drainDescriptor(descriptor,
                descriptor == standardOutputPipe[0] ? result.standardOutput
                                                    : result.errorOutput,
                false);
            ::close(descriptor);
        }
    }
    if (sink && !pendingStandardLine.empty()) {
        std::string_view line(pendingStandardLine);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        sink(line);
        pendingStandardLine.clear();
    }

    const auto waitForExit = [&](int milliseconds) {
        const auto limit = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(milliseconds);
        while (true) {
            int status = 0;
            const pid_t done = ::waitpid(pid, &status, WNOHANG);
            if (done == pid) {
                if (WIFEXITED(status)) {
                    result.exitCode = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exitCode = 128 + WTERMSIG(status);
                }
                return true;
            }
            if (done < 0 && errno != EINTR) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= limit) {
                return false;
            }
            struct timespec pause{0, 20 * 1000 * 1000};
            ::nanosleep(&pause, nullptr);
        }
    };

    const auto expired = std::chrono::steady_clock::now() >= deadline;
    if (expired) {
        result.timedOut = true;
        ::kill(pid, SIGTERM);
        if (!waitForExit(2000)) {
            ::kill(pid, SIGKILL);
            waitForExit(2000);
        }
    } else {
        waitForExit(static_cast<int>(options.timeout.count()));
    }

    return result;
}

} // namespace bang
