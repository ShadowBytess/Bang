#pragma once

#include <filesystem>
#include <functional>
#include <string>

struct _GstElement;
struct _GstBus;

namespace bang {

class Player {
public:
    enum class State { Stopped, Playing, Paused };

    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void load(const std::filesystem::path& file);
    void play();
    void pause();
    void toggle();
    void stop();

    void seek(std::int64_t positionMs);
    void setVolume(double volume);

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] std::int64_t positionMs();
    [[nodiscard]] std::int64_t durationMs() const;

    using FinishedCallback = std::function<void()>;
    void setOnFinished(FinishedCallback callback);

    bool poll();

private:
    _GstElement* playbin_ = nullptr;
    _GstBus* bus_ = nullptr;
    State state_ = State::Stopped;
    double volume_ = 0.8;
    FinishedCallback onFinished_;
};

} // namespace bang
