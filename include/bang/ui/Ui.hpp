#pragma once

#include "bang/render/Renderer.hpp"
#include "bang/text/TextEngine.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bang::ui {

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

namespace palette {
inline constexpr Color background { 0.055f, 0.063f, 0.086f, 1.0f };
inline constexpr Color surface { 0.086f, 0.098f, 0.129f, 1.0f };
inline constexpr Color surfaceRaised { 0.114f, 0.129f, 0.169f, 1.0f };
inline constexpr Color accent { 0.988f, 0.400f, 0.298f, 1.0f };
inline constexpr Color accentDim { 0.616f, 0.251f, 0.188f, 1.0f };
inline constexpr Color text { 0.925f, 0.937f, 0.957f, 1.0f };
inline constexpr Color textDim { 0.565f, 0.596f, 0.653f, 1.0f };
inline constexpr Color textFaint { 0.373f, 0.400f, 0.447f, 1.0f };
} // namespace palette

struct PointerState {
    float x = -1000.0f;
    float y = -1000.0f;
    bool down = false;
    bool pressed = false;
    bool released = false;
    float wheelDelta = 0.0f;
};

struct KeyboardState {
    std::string text;
    bool backspace = false;
    bool enter = false;
    bool escape = false;
};

struct GlyphSprite {
    render::Renderer::AtlasRegion region;
    int width = 0;
    int height = 0;
    int bearingX = 0;
    int bearingY = 0;
    float advanceX = 0.0f;
};

class Ui {
public:
    Ui(render::Renderer& renderer, text::TextEngine& textEngine);

    void beginFrame(float width, float height, const PointerState& pointer,
        const KeyboardState& keyboard);
    void endFrame();

    [[nodiscard]] float textWidth(
        const std::string& value, float size, text::Weight weight);
    [[nodiscard]] float lineHeight(float size) const;

    void fillRect(float x, float y, float w, float h, Color color,
        float radius = 0.0f);

    void text(const std::string& value, float x, float y, float size,
        text::Weight weight, Color color);
    void textTruncated(const std::string& value, float x, float y, float w,
        float size, text::Weight weight, Color color);

    [[nodiscard]] bool button(const char* id, const std::string& label,
        float x, float y, float w, float h, bool primary = false);
    [[nodiscard]] bool rowClicked(const char* id, float x, float y, float w,
        float h);
    [[nodiscard]] bool textField(const char* id, std::string& buffer,
        const std::string& placeholder, float x, float y, float w, float h,
        bool& focusedOut);
    [[nodiscard]] bool slider(const char* id, float& value01, float x, float y,
        float w, float h);
    void progressBar(float ratio, float x, float y, float w, float h);
    [[nodiscard]] bool scrolled(float x, float y, float w, float h,
        float contentHeight, float& scrollOffset);

    [[nodiscard]] bool wantsKeyboard() const { return !focused_.empty(); }

private:
    struct GlyphKeyHash {
        std::size_t operator()(std::uint64_t key) const noexcept
        {
            return static_cast<std::size_t>(key * 1099511628211ULL >> 17);
        }
    };
    std::unordered_map<std::uint64_t, GlyphSprite, GlyphKeyHash> glyphCache_;

    render::Renderer* renderer_;
    text::TextEngine* textEngine_;
    std::vector<render::Instance> instances_;

    PointerState pointer_;
    KeyboardState keyboard_;
    float width_ = 0.0f;
    float height_ = 0.0f;

    std::string hoveredId_;
    std::string pressedId_;
    std::string focused_;
    bool focusJustSet_ = false;

    [[nodiscard]] bool pointerInRect(float x, float y, float w, float h) const;
    [[nodiscard]] bool consumeClick(const char* id, float x, float y, float w,
        float h);

    [[nodiscard]] const GlyphSprite& ensureGlyph(
        text::Weight weight, int pixelSize, std::uint32_t glyphIndex);
};

} // namespace bang::ui
