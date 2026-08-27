#include "bang/DownloadService.hpp"

#include "DownloadParsing.hpp"
#include "bang/MetadataService.hpp"
#include "bang/ProcessRunner.hpp"

#include <algorithm>
#include <chrono>

namespace bang {

namespace {

constexpr std::chrono::milliseconds downloadTimeout { 3600000 };
constexpr std::size_t maxErrorMessageLength = 400;

std::string trimToLimit(std::string text)
{
    while (!text.empty()
        && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.size() > maxErrorMessageLength) {
        text = text.substr(text.size() - maxErrorMessageLength);
        const auto newline = text.find('\n');
        if (newline != std::string::npos) {
            text = text.substr(newline + 1);
        }
    }
    return text;
}

} // namespace

const char* DownloadService::backendName(Backend backend)
{
    return backend == Backend::YtDlp ? "yt-dlp" : "spotdl";
}

DownloadService::DownloadService(LibraryStore& store, TrackImporter& importer,
    std::filesystem::path workRoot)
    : store_(&store)
    , importer_(&importer)
    , workRoot_(std::move(workRoot))
{
    std::filesystem::create_directories(workRoot_);
    worker_ = std::thread([this] { workerLoop(); });
}

DownloadService::~DownloadService()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    signal_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DownloadService::setListener(ChangeListener listener)
{
    std::lock_guard lock(mutex_);
    listener_ = std::move(listener);
}

void DownloadService::enqueue(Request request)
{
    Job job;
    job.request = std::move(request);
    job.label = job.request.url;
    {
        std::lock_guard lock(mutex_);
        job.recordId = store_->beginDownload(
            job.request.url, backendName(job.request.backend));
        jobs_.push_back(std::move(job));
    }
    signal_.notify_one();
}

std::vector<DownloadService::Job> DownloadService::snapshot() const
{
    std::lock_guard lock(mutex_);
    return { jobs_.begin(), jobs_.end() };
}

void DownloadService::publish()
{
    ChangeListener listener;
    {
        std::lock_guard lock(mutex_);
        listener = listener_;
    }
    if (listener) {
        listener();
    }
}

std::vector<std::filesystem::path> DownloadService::collectAudioFiles(
    const std::filesystem::path& directory) const
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             directory, error)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mp3") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

void DownloadService::workerLoop()
{
    while (true) {
        Job* next = nullptr;
        {
        std::unique_lock lock(mutex_);
        signal_.wait(lock, [this] {
            if (stopping_) {
                return true;
            }
            return std::any_of(jobs_.begin(), jobs_.end(),
                [](const Job& job) { return job.state == State::Queued; });
        });
            if (stopping_) {
                return;
            }
            for (Job& job : jobs_) {
                if (job.state == State::Queued) {
                    next = &job;
                    break;
                }
            }
            if (next == nullptr) {
                continue;
            }
            next->state = State::Running;
            next->progressPercent = 0.0;
        }
        publish();
        runJob(*next);
    }
}

void DownloadService::runJob(Job& job)
{
    const std::string url = job.request.url;
    const Backend backend = job.request.backend;

    std::filesystem::path workDirectory =
        workRoot_ / ("job-" + std::to_string(job.recordId));
    std::filesystem::create_directories(workDirectory);

    const auto program = ProcessRunner::findExecutable(backendName(backend));
    if (!program.has_value()) {
        job.state = State::Failed;
        job.message = std::string(backendName(backend))
            + " is not installed or not on PATH";
        store_->completeDownload(job.recordId,
            DownloadStatus::Failed, job.message, std::nullopt);
        publish();
        return;
    }

    RunOptions options;
    options.program = *program;
    options.timeout = downloadTimeout;
    options.workingDirectory = workDirectory;

    std::vector<download::DoneItem> doneItems;
    double lastProgress = 0.0;

    if (backend == Backend::YtDlp) {
        options.arguments = download::ytDlpArguments(url, workDirectory);
    } else {
        const std::filesystem::path outputTemplate =
            workDirectory / "{artists} - {title}.{output-ext}";
        options.arguments = {
            "download", url, "--format", "mp3",
            "--output", outputTemplate.string(),
        };
    }

    const ProcessResult result = ProcessRunner::runStreaming(options,
        [&](std::string_view lineText) {
            std::string line(lineText);
            double progress = 0.0;
            download::DoneItem item;
            switch (download::classifyLine(line, &progress, &item)) {
            case download::LineKind::Progress:
                if (progress > lastProgress) {
                    lastProgress = progress;
                    std::lock_guard lock(mutex_);
                    job.progressPercent = progress;
                }
                publish();
                break;
            case download::LineKind::Done:
                doneItems.push_back(std::move(item));
                {
                    std::lock_guard lock(mutex_);
                    job.label = doneItems.back().title.empty()
                        ? url
                        : doneItems.back().title;
                }
                publish();
                break;
            case download::LineKind::Other: {
                bool spotdlUpdate = false;
                if (backend == Backend::SpotDl) {
                    const auto downloaded = line.find("Downloaded \"");
                    if (downloaded != std::string::npos) {
                        const auto titleStart = downloaded + 12;
                        const auto titleEnd = line.find('"', titleStart);
                        std::lock_guard lock(mutex_);
                        if (titleEnd != std::string::npos) {
                            job.label = line.substr(
                                titleStart, titleEnd - titleStart);
                        }
                        job.progressPercent = std::min(
                            job.progressPercent + 25.0, 95.0);
                        spotdlUpdate = true;
                    }
                }
                if (spotdlUpdate) {
                    publish();
                }
                break;
            }
            }
        });

    std::vector<Track> importedTracks;
    std::string failureMessage;

    if (!result.succeeded()) {
        failureMessage = result.timedOut
            ? "download timed out"
            : trimToLimit(result.errorOutput);
    } else {
        std::vector<std::filesystem::path> sources;
        if (backend == Backend::YtDlp && !doneItems.empty()) {
            for (const auto& item : doneItems) {
                sources.push_back(item.filePath);
            }
        } else {
            sources = collectAudioFiles(workDirectory);
        }

        const std::string sourceTag = backend == Backend::YtDlp ? "youtube" : "spotify";
        for (std::size_t index = 0; index < sources.size(); ++index) {
            AudioMetadata overrides;
            if (backend == Backend::YtDlp && index < doneItems.size()) {
                overrides.title = doneItems[index].title;
                overrides.artist = doneItems[index].uploader;
            }
            try {
                const TrackImporter::Result imported =
                    importer_->importFile(sources[index], overrides,
                        sourceTag, url);
                importedTracks.push_back(imported.track);
            } catch (const std::exception& error) {
                failureMessage = error.what();
                break;
            }
        }
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(workDirectory, cleanupError);

    {
        std::lock_guard lock(mutex_);
        if (importedTracks.empty()) {
            job.state = State::Failed;
            job.message = failureMessage.empty() ? "no audio produced"
                                                 : failureMessage;
            store_->completeDownload(job.recordId,
                DownloadStatus::Failed, job.message, std::nullopt);
        } else {
            job.state = State::Completed;
            job.progressPercent = 100.0;
            job.message = importedTracks.size() == 1
                ? importedTracks.front().title
                : std::to_string(importedTracks.size()) + " tracks";
            store_->completeDownload(job.recordId,
                DownloadStatus::Completed, job.message,
                importedTracks.front().id);
        }
    }
    publish();
}

} // namespace bang
