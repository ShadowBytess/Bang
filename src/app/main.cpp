#include "bang/DownloadService.hpp"
#include "bang/LibraryCatalog.hpp"
#include "bang/LibraryStore.hpp"
#include "bang/Paths.hpp"
#include "bang/Player.hpp"
#include "bang/SearchService.hpp"
#include "bang/TrackImporter.hpp"
#include "bang/platform/Window.hpp"
#include "bang/render/Renderer.hpp"
#include "bang/text/TextEngine.hpp"
#include "bang/ui/Ui.hpp"

#include "jetbrains_mono_bold.hpp"
#include "jetbrains_mono_regular.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string formatDuration(std::int64_t ms)
{
    if (ms <= 0) {
        return "--:--";
    }
    const std::int64_t totalSeconds = ms / 1000;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld",
        static_cast<long long>(totalSeconds / 60),
        static_cast<long long>(totalSeconds % 60));
    return buffer;
}

enum class Tab { Library = 0, Playlists = 1, Favorites = 2, Downloads = 3 };

struct App {
    bang::LibraryStore& store;
    bang::LibraryCatalog& catalog;
    bang::DownloadService& downloads;
    bang::SearchService& searchService;
    bang::Player& player;

    Tab tab = Tab::Library;
    std::string filter;
    std::string lastFilter;
    std::vector<bang::TrackListing> listing;
    std::vector<bang::Playlist> playlists;

    std::optional<std::int64_t> openPlaylistId;
    std::string newPlaylistName;
    bool playlistDialogOpen = false;

    bool urlDialogOpen = false;
    std::string urlBuffer;
    bool urlBackendSpotdl = false;

    bool searchDialogOpen = false;
    std::string searchBuffer;
    std::vector<bang::SearchService::Hit> searchHits;
    std::string searchError;
    bool searchBusy = false;
    bool searchDone = false;
    std::string searchQuery;
    std::mutex searchMutex;
    std::thread searchThread;

    ~App()
    {
        if (searchThread.joinable()) {
            searchThread.join();
        }
    }

    void startSearch()
    {
        if (searchBusy || searchBuffer.empty()) {
            return;
        }
        if (searchThread.joinable()) {
            searchThread.join();
        }
        searchQuery = searchBuffer;
        searchError.clear();
        searchHits.clear();
        searchBusy = true;
        searchDone = false;
        searchThread = std::thread([this] {
            std::vector<bang::SearchService::Hit> hits;
            std::string error;
            try {
                hits = searchService.searchYouTube(searchQuery, 20);
            } catch (const std::exception& searchFailure) {
                error = searchFailure.what();
            }
            std::lock_guard lock(searchMutex);
            searchHits = std::move(hits);
            searchError = std::move(error);
            searchDone = true;
        });
    }

    void pollSearch()
    {
        if (!searchBusy) {
            return;
        }
        {
            std::lock_guard lock(searchMutex);
            if (!searchDone) {
                return;
            }
        }
        if (searchThread.joinable()) {
            searchThread.join();
        }
        searchBusy = false;
        searchDone = false;
    }

    float libraryScroll = 0.0f;
    float downloadsScroll = 0.0f;

    std::optional<bang::Track> currentTrack;
    double volume = 0.8;
    bool seekDragging = false;

    void refresh()
    {
        listing = filter.empty()
            ? (tab == Tab::Favorites ? catalog.favorites()
                                     : catalog.allTracks())
            : catalog.searchTracks(filter);
        if (tab == Tab::Playlists && openPlaylistId.has_value()) {
            listing = catalog.playlistTracks(*openPlaylistId);
        }
        playlists = catalog.playlists();
    }

    [[nodiscard]] std::vector<const bang::Track*> contextQueue() const
    {
        std::vector<const bang::Track*> queue;
        for (const auto& entry : listing) {
            queue.push_back(&entry.track);
        }
        return queue;
    }

    void playTrack(const bang::Track& track)
    {
        const auto path = store.trackFilePath(track);
        if (!std::filesystem::exists(path)) {
            return;
        }
        player.load(path);
        player.setVolume(volume);
        currentTrack = track;
    }

    void skip(std::int64_t direction)
    {
        if (!currentTrack.has_value()) {
            return;
        }
        auto queue = contextQueue();
        for (std::size_t index = 0; index < queue.size(); ++index) {
            if (queue[index]->id == currentTrack->id) {
                const auto next = static_cast<std::int64_t>(index) + direction;
                if (next >= 0 && next < static_cast<std::int64_t>(queue.size())) {
                    playTrack(*queue[static_cast<std::size_t>(next)]);
                }
                return;
            }
        }
    }

    void enqueueUrl(const std::string& url, bool spotdl)
    {
        if (url.find("open.spotify.com") != std::string::npos
            || url.find("spotify:") != std::string::npos) {
            spotdl = true;
        }
        bang::DownloadService::Request request;
        request.url = url;
        request.backend =
            spotdl ? bang::DownloadService::Backend::SpotDl
                   : bang::DownloadService::Backend::YtDlp;
        downloads.enqueue(std::move(request));
    }
};

void drawTopBar(bang::ui::Ui& ui, App& app, float width)
{
    constexpr float barHeight = 52.0f;
    ui.fillRect(0, 0, width, barHeight, bang::ui::palette::surface);

    ui.text("BANG", 20.0f,
        (barHeight - ui.lineHeight(22.0f)) * 0.5f, 22.0f,
        bang::text::Weight::Bold, bang::ui::palette::accent);

    static constexpr const char* tabNames[] = { "Library", "Playlists",
        "Favorites", "Downloads" };
    float x = 110.0f;
    for (int index = 0; index < 4; ++index) {
        const float w = ui.textWidth(tabNames[index], 14.0f,
            bang::text::Weight::Regular) + 28.0f;
        const bool active = static_cast<Tab>(index) == app.tab;
        if (active) {
            ui.fillRect(x, 6.0f, w, barHeight - 12.0f,
                bang::ui::palette::surfaceRaised, 8.0f);
            ui.fillRect(x, barHeight - 4.0f, w, 2.5f,
                bang::ui::palette::accent, 1.25f);
        }
        if (ui.rowClicked(("tab" + std::to_string(index)).c_str(), x, 0.0f, w,
                barHeight)) {
            app.tab = static_cast<Tab>(index);
            app.filter.clear();
            app.openPlaylistId.reset();
            app.refresh();
        }
        ui.text(tabNames[index], x + 14.0f,
            (barHeight - ui.lineHeight(14.0f)) * 0.5f, 14.0f,
            bang::text::Weight::Regular,
            active ? bang::ui::palette::text : bang::ui::palette::textDim);
        x += w + 6.0f;
    }

    const float addButtonX = width - 130.0f;
    if (ui.button("addUrl", "+ Add URL", addButtonX, 10.0f, 114.0f,
            barHeight - 20.0f, true)) {
        app.urlDialogOpen = true;
    }
}

bool drawLibraryPanel(bang::ui::Ui& ui, App& app, float x, float y, float w,
    float h)
{
    bool needsRefresh = false;

    if (app.tab == Tab::Library) {
        bool filterFocusOut = false;
        static_cast<void>(ui.textField("filter", app.filter,
            "Filter library…", x + 16.0f, y + 12.0f,
            std::min(w - 32.0f, 320.0f), 32.0f, filterFocusOut));
        if (app.filter != app.lastFilter) {
            app.lastFilter = app.filter;
            needsRefresh = true;
        }
    } else if (app.tab == Tab::Playlists && !app.openPlaylistId.has_value()) {
        if (ui.button("newPlaylist", "New playlist", x + 16.0f, y + 12.0f,
                140.0f, 32.0f)) {
            app.playlistDialogOpen = true;
        }
    }

    const float listTop = y + 56.0f;
    const float rowHeight = 44.0f;
    const float visibleHeight = y + h - listTop;
    const float contentHeight = static_cast<float>(app.listing.size())
        * rowHeight;

    static_cast<void>(ui.scrolled(x, listTop, w, visibleHeight, contentHeight,
        app.libraryScroll));

    const float startY = listTop - app.libraryScroll;
    for (std::size_t index = 0; index < app.listing.size(); ++index) {
        const auto& entry = app.listing[index];
        const float rowY = startY + static_cast<float>(index) * rowHeight;
        if (rowY + rowHeight < listTop || rowY > listTop + visibleHeight) {
            continue;
        }

        const std::string rowId = "track" + std::to_string(entry.track.id);
        const std::string favId = "fav" + std::to_string(entry.track.id);

        if (ui.rowClicked(rowId.c_str(), x + 8.0f, rowY, w - 16.0f,
                rowHeight - 4.0f)) {
            app.playTrack(entry.track);
        }

        const float titleWidth = w * 0.42f;
        ui.textTruncated(entry.track.title, x + 24.0f, rowY + 13.0f,
            titleWidth, 14.0f, bang::text::Weight::Bold,
            bang::ui::palette::text);
        ui.textTruncated(
            entry.track.artist.empty() ? "Unknown artist" : entry.track.artist,
            x + 24.0f + titleWidth, rowY + 13.0f, w * 0.26f, 13.0f,
            bang::text::Weight::Regular, bang::ui::palette::textDim);
        ui.text(formatDuration(entry.track.durationMs),
            x + w - 150.0f, rowY + 14.0f, 13.0f, bang::text::Weight::Regular,
            bang::ui::palette::textFaint);

        const bool isCurrent = app.currentTrack.has_value()
            && app.currentTrack->id == entry.track.id;
        if (isCurrent) {
            ui.fillRect(x + 8.0f, rowY + 2.0f, 4.0f, rowHeight - 8.0f,
                bang::ui::palette::accent, 2.0f);
        }

        const float starX = x + w - 96.0f;
        const float starY = rowY + 15.0f;
        const bool hoverStar =
            ui.rowClicked(favId.c_str(), starX, starY, 18.0f, 18.0f);
        if (entry.favorite) {
            ui.fillRect(starX + 3.0f, starY + 3.0f, 12.0f, 12.0f,
                bang::ui::palette::accent, 3.0f);
        } else {
            ui.fillRect(starX + 3.0f, starY + 3.0f, 12.0f, 12.0f,
                bang::ui::Color { 1, 1, 1, 0.12f }, 3.0f);
        }
        if (hoverStar) {
            app.store.setFavorite(entry.track.id, !entry.favorite);
            needsRefresh = true;
        }
    }
    return needsRefresh;
}

void drawPlaylistsPanel(bang::ui::Ui& ui, App& app, float x, float y, float w,
    float h)
{
    if (app.openPlaylistId.has_value()) {
        if (ui.button("backToPlaylists", "< Back", x + 16.0f, y + 12.0f, 90.0f,
                32.0f)) {
            app.openPlaylistId.reset();
            app.refresh();
            return;
        }
        drawLibraryPanel(ui, app, x, y, w, h);
        return;
    }

    const float cardWidth = 220.0f;
    const float cardHeight = 120.0f;
    float cx = x + 16.0f;
    float cy = y + 64.0f;
    for (const auto& playlist : app.playlists) {
        if (cx + cardWidth > x + w) {
            cx = x + 16.0f;
            cy += cardHeight + 12.0f;
        }
        if (cy > y + h) {
            break;
        }
        if (ui.rowClicked("playlistCard", cx, cy, cardWidth, cardHeight)) {
            app.openPlaylistId = playlist.id;
            app.libraryScroll = 0.0f;
            app.refresh();
            return;
        }
        ui.fillRect(cx, cy, cardWidth, cardHeight,
            bang::ui::palette::surfaceRaised, 10.0f);
        ui.textTruncated(playlist.name, cx + 16.0f, cy + 18.0f,
            cardWidth - 32.0f, 15.0f, bang::text::Weight::Bold,
            bang::ui::palette::text);
        const std::size_t count = app.catalog.playlistTracks(playlist.id).size();
        ui.text(std::to_string(count)
                + (count == 1 ? " track" : " tracks"),
            cx + 16.0f, cy + 46.0f, 12.0f, bang::text::Weight::Regular,
            bang::ui::palette::textDim);
        cx += cardWidth + 12.0f;
    }
}

void drawDownloadsPanel(bang::ui::Ui& ui, App& app, float x, float y, float w,
    float h)
{
    const auto jobs = app.downloads.snapshot();
    const float rowHeight = 40.0f;
    float cy = y + 12.0f;

    for (std::size_t index = 0; index < jobs.size(); ++index) {
        const auto& job = jobs[index];
        ui.fillRect(x + 12.0f, cy, w - 24.0f, rowHeight - 6.0f,
            bang::ui::palette::surfaceRaised, 6.0f);
        const std::string label = job.label.size() > 70
            ? job.label.substr(0, 70) + "…"
            : job.label;
        ui.textTruncated(label, x + 24.0f, cy + 12.0f, w - 340.0f, 13.0f,
            bang::text::Weight::Regular, bang::ui::palette::text);
        switch (job.state) {
        case bang::DownloadService::State::Running:
            ui.progressBar(job.progressPercent / 100.0f, x + w - 300.0f,
                cy + 15.0f, 180.0f, 8.0f);
            break;
        case bang::DownloadService::State::Completed:
            ui.text("done", x + w - 290.0f, cy + 12.0f, 13.0f,
                bang::text::Weight::Bold, bang::ui::palette::accent);
            break;
        case bang::DownloadService::State::Failed:
            ui.text("failed", x + w - 300.0f, cy + 12.0f, 13.0f,
                bang::text::Weight::Bold,
                bang::ui::Color { 0.95f, 0.35f, 0.35f, 1.0f });
            break;
        default:
            ui.text("queued", x + w - 290.0f, cy + 12.0f, 13.0f,
                bang::text::Weight::Regular, bang::ui::palette::textDim);
            break;
        }
        cy += rowHeight;
    }

        cy += 8.0f;

    const float historyTop = cy + 12.0f;
    if (historyTop > y + h) {
        return;
    }
    ui.text("History", x + 16.0f, historyTop, 15.0f, bang::text::Weight::Bold,
        bang::ui::palette::textDim);
    float hy = historyTop + 30.0f;
    for (const auto& record : app.catalog.recentDownloads(50)) {
        if (hy > y + h - 24.0f) {
            break;
        }
        ui.textTruncated(record.requestUrl, x + 16.0f, hy, w - 200.0f, 12.0f,
            bang::text::Weight::Regular, bang::ui::palette::textDim);
        ui.text(record.status, x + w - 160.0f, hy, 12.0f,
            bang::text::Weight::Bold,
            record.status == "completed" ? bang::ui::palette::accent
                                         : bang::ui::palette::textFaint);
        hy += 22.0f;
    }
}

void drawPlayerBar(bang::ui::Ui& ui, App& app, float width, float height)
{
    constexpr float barHeight = 68.0f;
    const float y = height - barHeight;
    ui.fillRect(0, y, width, barHeight, bang::ui::palette::surface);

    const float titleWidth = width * 0.30f;
    if (app.currentTrack.has_value()) {
        ui.textTruncated(app.currentTrack->title, 20.0f, y + 16.0f,
            titleWidth - 40.0f, 14.0f, bang::text::Weight::Bold,
            bang::ui::palette::text);
        ui.textTruncated(app.currentTrack->artist, 20.0f, y + 38.0f,
            titleWidth - 40.0f, 12.0f, bang::text::Weight::Regular,
            bang::ui::palette::textDim);
    } else {
        ui.text("Nothing playing", 20.0f, y + 26.0f, 13.0f,
            bang::text::Weight::Regular, bang::ui::palette::textFaint);
    }

    const float controlsX = titleWidth + 20.0f;
    if (ui.button("prev", "<", controlsX, y + 19.0f, 44.0f, 30.0f)) {
        app.skip(-1);
    }
    const bool playing = app.player.state() == bang::Player::State::Playing;
    if (ui.button("playPause", playing ? "| |" : "|>", controlsX + 50.0f,
            y + 19.0f, 56.0f, 30.0f, true)) {
        if (playing) {
            app.player.pause();
        } else if (app.currentTrack.has_value()) {
            app.player.toggle();
        } else if (!app.listing.empty()) {
            app.playTrack(app.listing.front().track);
        }
    }
    if (ui.button("next", ">", controlsX + 112.0f, y + 19.0f, 44.0f, 30.0f)) {
        app.skip(1);
    }

    const float seekX = controlsX + 180.0f;
    const float seekW = width - seekX - 260.0f;
    if (seekW > 100.0f && app.currentTrack.has_value()) {
        const double position = static_cast<double>(app.player.positionMs());
        const double duration =
            static_cast<double>(app.player.durationMs());
        float ratio =
            duration > 0.0 ? static_cast<float>(position / duration) : 0.0f;
        if (ui.slider("seek", ratio, seekX, y + 24.0f, seekW, 20.0f)) {
            app.player.seek(static_cast<std::int64_t>(
                static_cast<double>(ratio) * duration));
        }
        const std::string timeLabel = formatDuration(app.player.positionMs())
            + " / " + formatDuration(app.player.durationMs());
        ui.text(timeLabel, seekX + seekW + 12.0f, y + 27.0f, 12.0f,
            bang::text::Weight::Regular, bang::ui::palette::textDim);
    }

    const float volumeX = width - 190.0f;
    float volumeRatio = static_cast<float>(app.volume);
    if (ui.slider("volume", volumeRatio, volumeX, y + 24.0f, 140.0f, 20.0f)) {
        app.volume = volumeRatio;
        app.player.setVolume(app.volume);
        app.store.setSetting("volume",
            std::to_string(static_cast<int>(app.volume * 100.0)));
    }
}

void drawDialogs(bang::ui::Ui& ui, App& app, float width, float height)
{
    const auto dimOverlay = [&] {
        ui.fillRect(0, 0, width, height, bang::ui::Color { 0, 0, 0, 0.55f });
    };
    const auto dialogFrame = [&](float dw, float dh) {
        ui.fillRect((width - dw) * 0.5f, (height - dh) * 0.5f, dw, dh,
            bang::ui::palette::background, 12.0f);
        ui.fillRect((width - dw) * 0.5f, (height - dh) * 0.5f, dw, 2.0f,
            bang::ui::palette::accent, 1.0f);
    };

    if (app.urlDialogOpen) {
        dimOverlay();
        const float dw = 520.0f;
        const float dh = 210.0f;
        const float dx = (width - dw) * 0.5f;
        const float dy = (height - dh) * 0.5f;
        dialogFrame(dw, dh);

        ui.text("Download music", dx + 24.0f, dy + 22.0f, 17.0f,
            bang::text::Weight::Bold, bang::ui::palette::text);
        bool focusOut = false;
        if (ui.textField("urlField", app.urlBuffer,
                "Paste a YouTube or Spotify link…", dx + 24.0f, dy + 58.0f,
                dw - 48.0f, 36.0f, focusOut)) {
            if (!app.urlBuffer.empty()) {
                app.enqueueUrl(app.urlBuffer, app.urlBackendSpotdl);
                app.urlBuffer.clear();
                app.urlDialogOpen = false;
                return;
            }
        }

        const float chipY = dy + 110.0f;
        ui.text("Backend:", dx + 24.0f, chipY + 7.0f, 13.0f,
            bang::text::Weight::Regular, bang::ui::palette::textDim);
        const bool ytActive = !app.urlBackendSpotdl;
        ui.fillRect(dx + 92.0f, chipY, 96.0f, 30.0f,
            ytActive ? bang::ui::palette::accent : bang::ui::palette::surface,
            6.0f);
        ui.text("yt-dlp", dx + 116.0f, chipY + 8.0f, 13.0f,
            bang::text::Weight::Bold,
            ytActive ? bang::ui::Color { 0, 0, 0, 1 } : bang::ui::palette::text);
        if (ui.rowClicked("backendYt", dx + 92.0f, chipY, 96.0f, 30.0f)) {
            app.urlBackendSpotdl = false;
        }
        ui.fillRect(dx + 196.0f, chipY, 96.0f, 30.0f,
            app.urlBackendSpotdl ? bang::ui::palette::accent
                                 : bang::ui::palette::surface,
            6.0f);
        ui.text("spotdl", dx + 218.0f, chipY + 8.0f, 13.0f,
            bang::text::Weight::Bold,
            app.urlBackendSpotdl
                ? bang::ui::Color { 0, 0, 0, 1 }
                : bang::ui::palette::text);
        if (ui.rowClicked("backendSpot", dx + 196.0f, chipY, 96.0f, 30.0f)) {
            app.urlBackendSpotdl = true;
        }

        if (ui.button("urlCancel", "Cancel", dx + 24.0f, dy + dh - 48.0f, 90.0f,
                32.0f)) {
            app.urlDialogOpen = false;
        }
        if (ui.button("urlGo", "Download", dx + dw - 128.0f,
                dy + dh - 48.0f, 104.0f, 32.0f, true)) {
            if (!app.urlBuffer.empty()) {
                app.enqueueUrl(app.urlBuffer, app.urlBackendSpotdl);
                app.urlBuffer.clear();
                app.urlDialogOpen = false;
            }
        }
    }

    if (app.searchDialogOpen) {
        dimOverlay();
        const float dw = std::min(width - 80.0f, 680.0f);
        const float dh = std::min(height - 80.0f, 480.0f);
        const float dx = (width - dw) * 0.5f;
        const float dy = (height - dh) * 0.5f;
        dialogFrame(dw, dh);

        ui.text("Find music on YouTube", dx + 24.0f, dy + 22.0f, 17.0f,
            bang::text::Weight::Bold, bang::ui::palette::text);

        bool focusOut = false;
        if (ui.textField("searchField", app.searchBuffer,
                "Search YouTube…", dx + 24.0f, dy + 56.0f, dw - 148.0f, 34.0f,
                focusOut)) {
            app.startSearch();
        }
        if (ui.button("searchGo", "Search", dx + dw - 116.0f, dy + 56.0f, 92.0f,
                34.0f, true)) {
            app.startSearch();
        }
        app.pollSearch();

        if (app.searchBusy) {
            ui.text("Searching YouTube…", dx + 24.0f, dy + 104.0f, 13.0f,
                bang::text::Weight::Regular, bang::ui::palette::textDim);
        }

        float ry = dy + (app.searchBusy ? 130.0f : 104.0f);
        if (!app.searchError.empty()) {
            ui.textTruncated(app.searchError, dx + 24.0f, ry, dw - 48.0f, 12.0f,
                bang::text::Weight::Regular,
                bang::ui::Color { 0.95f, 0.45f, 0.45f, 1.0f });
        }
        for (const auto& hit : app.searchHits) {
            if (ry > dy + dh - 56.0f) {
                break;
            }
            const std::string hitId = "hit" + hit.videoId;
            if (ui.rowClicked(hitId.c_str(), dx + 16.0f, ry, dw - 32.0f,
                    40.0f)) {
                app.enqueueUrl("https://www.youtube.com/watch?v=" + hit.videoId,
                    false);
                ry += 40.0f;
                continue;
            }
            ui.textTruncated(hit.title, dx + 28.0f, ry + 12.0f, dw - 240.0f,
                13.0f, bang::text::Weight::Bold, bang::ui::palette::text);
            ui.textTruncated(hit.uploader, dx + 28.0f, ry + 26.0f, dw - 240.0f,
                11.0f, bang::text::Weight::Regular,
                bang::ui::palette::textFaint);
            ui.text(formatDuration(hit.durationSec * 1000), dx + dw - 170.0f,
                ry + 13.0f, 12.0f, bang::text::Weight::Regular,
                bang::ui::palette::textDim);
            ui.text("get", dx + dw - 84.0f, ry + 13.0f, 12.0f,
                bang::text::Weight::Bold, bang::ui::palette::accent);
            ry += 44.0f;
        }

        if (ui.button("searchClose", "Close", dx + dw - 104.0f,
                dy + dh - 44.0f, 88.0f, 30.0f)) {
            app.searchDialogOpen = false;
            app.searchHits.clear();
        }
    }

    if (app.playlistDialogOpen) {
        dimOverlay();
        const float dw = 420.0f;
        const float dh = 180.0f;
        const float dx = (width - dw) * 0.5f;
        const float dy = (height - dh) * 0.5f;
        dialogFrame(dw, dh);
        ui.text("New playlist", dx + 24.0f, dy + 22.0f, 17.0f,
            bang::text::Weight::Bold, bang::ui::palette::text);

        bool focusOut = false;
        const bool submitted = ui.textField("playlistName", app.newPlaylistName,
            "Playlist name…", dx + 24.0f, dy + 60.0f, dw - 48.0f, 34.0f,
            focusOut);
        if (submitted || ui.button("createPlaylist", "Create", dx + dw - 122.0f,
                                   dy + dh - 48.0f, 98.0f, 32.0f, true)) {
            if (!app.newPlaylistName.empty()) {
                const auto createdId = app.store.createPlaylist(app.newPlaylistName);
                app.openPlaylistId = createdId;
                app.newPlaylistName.clear();
                app.playlistDialogOpen = false;
                app.refresh();
            }
        }
        if (ui.button("cancelPlaylist", "Cancel", dx + 24.0f, dy + dh - 48.0f,
                90.0f, 32.0f)) {
            app.playlistDialogOpen = false;
        }
    }
}

} // namespace

int main()
{
    using namespace bang;

    try {
        LibraryStore store(dataDirectory());
        LibraryCatalog catalog(store);
        TrackImporter importer(store);
        DownloadService downloads(store, importer, temporaryDirectory());
        SearchService searchService;
        Player player;

        platform::Window window("Bang", 1180, 720);
        render::Renderer renderer(window.display(), window.surface(),
            window.width(), window.height());

        text::TextEngine textEngine(jetbrains_mono_regular_bytes(),
            jetbrains_mono_regular_size(), jetbrains_mono_bold_bytes(),
            jetbrains_mono_bold_size());
        ui::Ui ui(renderer, textEngine);

        std::atomic<bool> libraryDirty { true };
        downloads.setListener([&libraryDirty] { libraryDirty.store(true); });

        window.setEvents(platform::WindowEvents {
            .onResize =
                [&renderer](std::uint32_t w, std::uint32_t h) {
                    renderer.resize(w, h);
                },
            .onConfigure = [] { },
        });

        App app { store, catalog, downloads, searchService, player };
        if (const auto storedVolume = store.setting("volume");
            storedVolume.has_value()) {
            app.volume = std::clamp(
                std::stod(*storedVolume) / 100.0, 0.0, 1.0);
            player.setVolume(app.volume);
        }
        app.refresh();

        while (window.poll()) {
            const std::uint32_t width = window.width();
            const std::uint32_t height = window.height();

            player.poll();

            auto pointer = window.takePointer();
            auto keyboard = window.takeKeyboard();

            if (keyboard.escape && app.urlDialogOpen) {
                app.urlDialogOpen = false;
                keyboard.escape = false;
            }

            if (libraryDirty.exchange(false)) {
                app.refresh();
            }

            ui.beginFrame(static_cast<float>(width),
                static_cast<float>(height), pointer, keyboard);

            drawTopBar(ui, app, static_cast<float>(width));

            const float contentTop = 52.0f;
            const float contentHeight =
                static_cast<float>(height) - contentTop - 68.0f;

            switch (app.tab) {
            case Tab::Playlists:
                drawPlaylistsPanel(ui, app, 0.0f, contentTop,
                    static_cast<float>(width), contentHeight);
                break;
            case Tab::Downloads:
                drawDownloadsPanel(ui, app, 0.0f, contentTop,
                    static_cast<float>(width), contentHeight);
                break;
            default:
                if (drawLibraryPanel(ui, app, 0.0f, contentTop,
                        static_cast<float>(width), contentHeight)) {
                    app.refresh();
                }
                break;
            }

            drawPlayerBar(ui, app, static_cast<float>(width),
                static_cast<float>(height));
            drawDialogs(ui, app, static_cast<float>(width),
                static_cast<float>(height));

            ui.endFrame();
        }

        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "bang: %s\n", error.what());
        return 1;
    }
}
