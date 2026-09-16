#include "bang/DownloadService.hpp"
#include "bang/LibraryCatalog.hpp"
#include "bang/ProcessRunner.hpp"

#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

bool check(bool condition, const std::string& name)
{
    std::cout << (condition ? "PASS " : "FAIL ") << name << std::endl;
    return condition;
}

int runDownloader(const std::string& mode)
{
    if (mode == "slow") {
        std::cout << "BANGPCT|1" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
        return 0;
    }
    if (mode.starts_with("spotify-")) {
        std::cout << "AudioProviderError: no matching audio found\n";
        return mode == "spotify-zero" ? 0 : 1;
    }
    for (int index = 1; index <= 3; ++index) {
        const auto file = fs::current_path() / ("track" + std::to_string(index) + ".mp3");
        fs::copy_file(fs::path(std::getenv("BANG_TEST_FIXTURES")) / file.filename(), file);
        if (mode == "missing-second" && index == 2) {
            fs::remove(file);
        }
        std::cout << "BANGDONE|" << file.string() << "|Track " << index
                  << "|Test|1" << std::endl;
    }
    if (mode == "exit-error") {
        std::cerr << "ERROR: another playlist entry is unavailable\n";
        return 1;
    }
    return 0;
}

bool runCase(const fs::path& root, const std::string& mode, std::size_t expectedCount)
{
    bang::LibraryStore store(root / mode / "data");
    bang::TrackImporter importer(store);
    bang::LibraryCatalog catalog(store);
    std::mutex mutex;
    std::condition_variable signal;
    bool finished = false;
    bang::DownloadService::Job result;
    bang::DownloadService downloads(store, importer, root / mode / "tmp");
    downloads.setListener([&] {
        const auto jobs = downloads.snapshot();
        if (!jobs.empty() && (jobs[0].state == bang::DownloadService::State::Failed
                                || jobs[0].state == bang::DownloadService::State::Completed)) {
            std::lock_guard lock(mutex);
            result = jobs[0];
            finished = true;
            signal.notify_one();
        }
    });
    downloads.enqueue({ mode, mode.starts_with("spotify-")
        ? bang::DownloadService::Backend::SpotDl : bang::DownloadService::Backend::YtDlp });
    {
        std::unique_lock lock(mutex);
        if (!signal.wait_for(lock, std::chrono::seconds(5), [&] { return finished; })) {
            std::cerr << "FAIL " << mode << " did not finish\n";
            std::_Exit(1);
        }
    }
    downloads.setListener({});
    bool ok = check(catalog.allTracks().size() == expectedCount, mode + ": imported tracks");
    const bool failed = mode != "all-tracks";
    ok &= check(result.state == (failed ? bang::DownloadService::State::Failed
                                      : bang::DownloadService::State::Completed),
        mode + ": status");
    if (failed) {
        const std::string reason = mode.starts_with("spotify-") ? "no matching audio found"
            : mode == "missing-second" ? "missing file" : "unavailable";
        ok &= check(result.message.find(reason)
                != std::string::npos,
            mode + ": retains failure reason");
        if (expectedCount > 0) {
            ok &= check(result.message.find(std::to_string(expectedCount) + " tracks imported")
                    != std::string::npos,
                mode + ": reports partial success");
        }
    }
    const auto history = catalog.recentDownloads();
    ok &= check(history.size() == 1 && history[0].message == result.message,
        mode + ": persists result");
    return ok;
}

bool checkRestart(const fs::path& root, const std::string& mode, std::size_t expectedCount)
{
    bool ok = true;
    for (int restart = 0; restart < 2; ++restart) {
        bang::LibraryStore store(root / mode / "data");
        bang::TrackImporter importer(store);
        bang::LibraryCatalog catalog(store);
        bang::DownloadService downloads(store, importer, root / mode / "tmp");
        const auto history = catalog.recentDownloads();
        ok &= check(mode == "all-tracks"
                ? history.size() == 1 && history[0].status == "completed" && history[0].hasTrack
                : history.empty(),
            mode + ": restart retains only completed history");
        ok &= check(downloads.snapshot().empty(), mode + ": restart has an empty queue");
        const auto tracks = catalog.allTracks();
        ok &= check(tracks.size() == expectedCount, mode + ": restart preserves imported tracks");
        for (const auto& listing : tracks) {
            ok &= check(fs::exists(store.trackFilePath(listing.track)),
                mode + ": restart preserves audio files");
        }
    }
    return ok;
}

bool runCloseCase(const fs::path& root)
{
    bang::LibraryStore store(root / "close" / "data");
    bang::TrackImporter importer(store);
    std::mutex mutex;
    std::condition_variable signal;
    bool started = false;
    auto downloads = std::make_unique<bang::DownloadService>(store, importer,
        root / "close" / "tmp");
    downloads->setListener([&] {
        const auto jobs = downloads->snapshot();
        if (!jobs.empty() && jobs[0].progressPercent > 0) {
            std::lock_guard lock(mutex);
            started = true;
            signal.notify_one();
        }
    });
    downloads->enqueue({ "slow" });
    downloads->enqueue({ "all-tracks" });
    {
        std::unique_lock lock(mutex);
        if (!signal.wait_for(lock, std::chrono::seconds(5), [&] { return started; })) {
            std::cerr << "FAIL close: downloader did not start\n";
            std::_Exit(1);
        }
    }
    downloads->setListener({});
    const auto start = std::chrono::steady_clock::now();
    downloads.reset();
    bool ok = check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
        "close: cancel active download promptly");
    ok &= check(fs::is_empty(root / "close" / "tmp"),
        "close: remove interrupted job files");
    ok &= check(bang::LibraryCatalog(store).allTracks().empty(),
        "close: do not run queued downloads");
    return checkRestart(root, "close", 0) && ok;
}

bool runKilledCase(const fs::path& root, int terminationSignal)
{
    const std::string mode = "killed-" + std::to_string(terminationSignal);
    int ready[2];
    if (::pipe(ready) != 0) {
        return check(false, mode + ": create readiness pipe");
    }
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(ready[0]);
        bang::LibraryStore store(root / mode / "data");
        bang::TrackImporter importer(store);
        bang::DownloadService downloads(store, importer, root / mode / "tmp");
        downloads.setListener([&] {
            // Hold the worker before launching a downloader, with another job queued.
            downloads.enqueue({ "queued" });
            if (::write(ready[1], "R", 1) != 1) {
                std::_Exit(1);
            }
            while (true) {
                ::pause();
            }
        });
        downloads.enqueue({ "interrupted" });
        while (true) {
            ::pause();
        }
    }
    ::close(ready[1]);
    if (child < 0) {
        ::close(ready[0]);
        return check(false, mode + ": fork download process");
    }
    char marker = 0;
    bool ok = check(::read(ready[0], &marker, 1) == 1 && marker == 'R',
        mode + ": running and queued jobs are persisted");
    ::close(ready[0]);
    ok &= check(::kill(child, terminationSignal) == 0, mode + ": terminate process");
    int status = 0;
    ok &= check(::waitpid(child, &status, 0) == child && WIFSIGNALED(status)
            && WTERMSIG(status) == terminationSignal,
        mode + ": process died without shutdown cleanup");
    {
        bang::LibraryStore store(root / mode / "data");
        bang::LibraryCatalog catalog(store);
        const auto history = catalog.recentDownloads();
        ok &= check(history.size() == 2 && history[0].status == "running"
                && history[1].status == "running",
            mode + ": interrupted records survive process death");
    }
    return checkRestart(root, mode, 0) && ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (fs::path(argv[0]).filename() == "yt-dlp") {
        return runDownloader(argv[1]);
    }
    if (fs::path(argv[0]).filename() == "spotdl") {
        return runDownloader(argv[2]);
    }
    char directory[] = "/tmp/bang-download-test-XXXXXX";
    const char* temporary = ::mkdtemp(directory);
    if (temporary == nullptr) {
        return 1;
    }
    const fs::path root(temporary);
    fs::create_directories(root / "bin");
    fs::create_symlink(fs::canonical(argv[0]), root / "bin/yt-dlp");
    fs::create_symlink(fs::canonical(argv[0]), root / "bin/spotdl");
    ::setenv("BANG_TEST_FIXTURES", root.c_str(), 1);
    ::setenv("XDG_DATA_HOME", root.c_str(), 1);
    const auto ffmpeg = bang::ProcessRunner::findExecutable("ffmpeg");
    if (!ffmpeg) {
        std::cerr << "ffmpeg is required to generate the download test audio\n";
        fs::remove_all(root);
        return 1;
    }
    for (int index = 1; index <= 3; ++index) {
        bang::RunOptions options;
        options.program = *ffmpeg;
        options.arguments = { "-v", "error", "-f", "lavfi", "-i",
            "sine=frequency=" + std::to_string(index * 440) + ":duration=0.1",
            "-c:a", "libmp3lame", (root / ("track" + std::to_string(index) + ".mp3")).string() };
        if (!bang::ProcessRunner::run(options).succeeded()) {
            fs::remove_all(root);
            return 1;
        }
    }
    const std::string path = (root / "bin").string() + ":" + std::getenv("PATH");
    ::setenv("PATH", path.c_str(), 1);
    bool ok = runCase(root, "all-tracks", 3);
    ok &= runCase(root, "missing-second", 2);
    ok &= runCase(root, "exit-error", 3);
    ok &= runCase(root, "spotify-nonzero", 0);
    ok &= runCase(root, "spotify-zero", 0);
    ok &= checkRestart(root, "all-tracks", 3);
    ok &= checkRestart(root, "missing-second", 2);
    ok &= checkRestart(root, "exit-error", 3);
    ok &= checkRestart(root, "spotify-nonzero", 0);
    ok &= checkRestart(root, "spotify-zero", 0);
    ok &= runKilledCase(root, SIGTERM);
    ok &= runKilledCase(root, SIGKILL);
    ok &= runCloseCase(root);
    fs::remove_all(root);
    return ok ? 0 : 1;
}
