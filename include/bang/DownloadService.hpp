#pragma once

#include "bang/LibraryStore.hpp"
#include "bang/TrackImporter.hpp"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bang {

class DownloadService {
public:
    enum class Backend { YtDlp, SpotDl };

    struct Request {
        std::string url;
        Backend backend = Backend::YtDlp;
    };

    enum class State { Queued, Running, Completed, Failed };

    struct Job {
        std::int64_t recordId = 0;
        Request request;
        State state = State::Queued;
        double progressPercent = 0.0;
        std::string label;
        std::string message;
    };

    using ChangeListener = std::function<void()>;

    DownloadService(LibraryStore& store, TrackImporter& importer,
        std::filesystem::path workRoot);
    ~DownloadService();
    DownloadService(const DownloadService&) = delete;
    DownloadService& operator=(const DownloadService&) = delete;

    void setListener(ChangeListener listener);
    void enqueue(Request request);

    [[nodiscard]] std::vector<Job> snapshot() const;

    [[nodiscard]] static const char* backendName(Backend backend);

private:
    void workerLoop();
    void runJob(Job& job);
    void publish();
    [[nodiscard]] std::vector<std::filesystem::path> collectAudioFiles(
        const std::filesystem::path& directory) const;

    LibraryStore* store_;
    TrackImporter* importer_;
    std::filesystem::path workRoot_;

    mutable std::mutex mutex_;
    std::condition_variable signal_;
    std::deque<Job> jobs_;
    ChangeListener listener_;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace bang
