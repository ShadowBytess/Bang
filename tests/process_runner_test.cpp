#include "bang/ProcessRunner.hpp"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool check(bool condition, const char* name)
{
    std::cout << (condition ? "PASS " : "FAIL ") << name << std::endl;
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2) {
        const std::string mode = argv[1];
        if (mode == "both-streams") {
            std::cout << "ready" << std::endl;
            std::cerr << std::string(1024 * 1024, 'x') << std::flush;
            std::cout << "done";
        } else if (mode == "descendant") {
            const auto child = ::fork();
            if (child == 0) {
                ::signal(SIGTERM, SIG_IGN);
                std::cout << ::getpid() << std::endl;
                while (true) {
                    ::pause();
                }
            }
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
            ::waitpid(child, nullptr, 0);
        } else {
            if (mode == "closed-streams") {
                ::close(STDOUT_FILENO);
                ::close(STDERR_FILENO);
            } else {
                std::cout << "ready" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
        return 0;
    }

    bang::RunOptions options;
    options.program = std::filesystem::canonical(argv[0]).string();
    options.arguments = { "both-streams" };
    options.timeout = std::chrono::seconds(2);
    std::vector<std::string> lines;
    const auto result = bang::ProcessRunner::runStreaming(options,
        [&](std::string_view line) { lines.emplace_back(line); });
    bool ok = check(result.succeeded(), "child can fill stderr while stdout stays open");
    ok &= check(result.errorOutput == std::string(1024 * 1024, 'x'),
        "capture all stderr");
    ok &= check(lines == std::vector<std::string> { "ready", "done" },
        "stream complete lines and final unterminated line");

    for (const char* mode : { "open-streams", "closed-streams" }) {
        options.arguments = { mode };
        options.timeout = std::chrono::milliseconds(100);
        const auto start = std::chrono::steady_clock::now();
        const auto timed = bang::ProcessRunner::run(options);
        ok &= check(timed.timedOut && !timed.succeeded(), mode);
        ok &= check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
            "timeout stops and reaps the child promptly");
    }

    for (const char* mode : { "open-streams", "closed-streams" }) {
        std::stop_source stop;
        options.stopToken = stop.get_token();
        options.arguments = { mode };
        options.timeout = std::chrono::seconds(5);
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
        const auto start = std::chrono::steady_clock::now();
        const auto cancelled = bang::ProcessRunner::run(options);
        ok &= check(cancelled.cancelled && !cancelled.timedOut && !cancelled.succeeded(),
            "cancellation is distinct from timeout with open or closed streams");
        ok &= check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "cancellation stops and reaps the child promptly");
    }

    // Adopt grandchildren so their exit status proves process-group cleanup.
    ok &= check(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "adopt downloader descendants");
    for (bool cancel : { false, true }) {
        std::stop_source stop;
        options.stopToken = stop.get_token();
        options.arguments = { "descendant" };
        options.timeout = std::chrono::milliseconds(300);
        pid_t descendant = -1;
        const auto stopped = bang::ProcessRunner::runStreaming(options,
            [&](std::string_view line) {
                descendant = static_cast<pid_t>(std::stoi(std::string(line)));
                if (cancel) {
                    stop.request_stop();
                }
            });
        ok &= check(stopped.cancelled == cancel && stopped.timedOut != cancel,
            "stop a downloader with a surviving subprocess");
        int status = 0;
        pid_t reaped = -1;
        if (descendant > 0) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                reaped = ::waitpid(descendant, &status, WNOHANG);
                if (reaped != 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        ok &= check(reaped == descendant && descendant > 0 && WIFSIGNALED(status)
                && WTERMSIG(status) == SIGKILL,
            "kill descendants that ignore SIGTERM even after the parent exits");
        if (descendant > 0 && reaped == 0) {
            ::kill(descendant, SIGKILL);
            ::waitpid(descendant, nullptr, 0);
        }
    }

    std::stop_source stopped;
    stopped.request_stop();
    options.stopToken = stopped.get_token();
    options.program = "/does-not-exist";
    const auto skipped = bang::ProcessRunner::run(options);
    ok &= check(skipped.cancelled && skipped.exitCode == -1 && skipped.errorOutput.empty(),
        "do not launch a process when already cancelled");
    return ok ? 0 : 1;
}
