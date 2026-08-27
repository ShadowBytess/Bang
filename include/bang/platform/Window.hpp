#pragma once

#include "bang/ui/Ui.hpp"

#include <functional>
#include <string>

struct wl_display;
struct wl_surface;

namespace bang::platform {

struct WindowEvents {
    std::function<void(std::uint32_t width, std::uint32_t height)> onResize;
    std::function<void()> onConfigure;
};

class Window {
public:
    Window(const std::string& title, std::uint32_t width, std::uint32_t height);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    void setEvents(WindowEvents events);
    [[nodiscard]] wl_display* display() const;
    [[nodiscard]] wl_surface* surface() const;
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }

    [[nodiscard]] ui::PointerState takePointer();
    [[nodiscard]] ui::KeyboardState takeKeyboard();

    bool poll();

    struct State;

private:
    State* state_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace bang::platform
