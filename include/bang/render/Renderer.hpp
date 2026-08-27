#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct wl_display;
struct wl_surface;

namespace bang::render {

struct Instance {
    float bounds[4] = { 0, 0, 0, 0 };
    float color[4] = { 1, 1, 1, 1 };
    float params[4] = { 0, 0, 0, 0 };
    float uv[4] = { 0, 0, 1, 1 };
    float clip[4] = { 0, 0, 1000000, 1000000 };

    static constexpr std::size_t floatCount() { return 20; }
};

class Renderer {
public:
    struct AtlasRegion {
        float u0 = 0;
        float v0 = 0;
        float u1 = 0;
        float v1 = 0;
        int x = 0;
        int y = 0;
    };

    Renderer(wl_display* display, wl_surface* surface, std::uint32_t width,
        std::uint32_t height);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void resize(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] AtlasRegion allocateAtlas(int width, int height);
    void uploadAtlas(const AtlasRegion& region, int width, int height,
        const std::uint8_t* alphaPixels);

    bool render(std::vector<Instance> instances);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace bang::render
