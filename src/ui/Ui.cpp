#include "bang/ui/Ui.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

namespace bang::ui {

namespace {

constexpr std::uint64_t glyphKey(
    text::Weight weight, int pixelSize, std::uint32_t index)
{
    return (static_cast<std::uint64_t>(weight) << 48)
        | (static_cast<std::uint64_t>(pixelSize) << 32)
        | static_cast<std::uint64_t>(index);
}

void appendQuad(std::vector<render::Instance>& instances, float x, float y,
    float w, float h, const Color& color, float radius, float kind,
    const std::array<float, 4>& uv = { 0.0f, 0.0f, 1.0f, 1.0f })
{
    render::Instance instance;
    instance.bounds[0] = x;
    instance.bounds[1] = y;
    instance.bounds[2] = w;
    instance.bounds[3] = h;
    instance.color[0] = color.r;
    instance.color[1] = color.g;
    instance.color[2] = color.b;
    instance.color[3] = color.a;
    instance.params[0] = kind;
    instance.params[1] = radius;
    for (int index = 0; index < 4; ++index) {
        instance.uv[index] = uv[index];
    }
    instances.push_back(instance);
}

} // namespace

Ui::Ui(render::Renderer& renderer, text::TextEngine& textEngine)
    : renderer_(&renderer)
    , textEngine_(&textEngine)
{
}

void Ui::beginFrame(float width, float height, const PointerState& pointer,
    const KeyboardState& keyboard)
{
    width_ = width;
    height_ = height;
    pointer_ = pointer;
    keyboard_ = keyboard;
    instances_.clear();
    hoveredId_.clear();

    if (pointer_.pressed && !pointerInRect(-1, -1, width + 2, height + 2)) {
        focused_.clear();
    }
}

void Ui::endFrame()
{
    renderer_->render(std::move(instances_));
    instances_.clear();

    if (pointer_.released) {
        pressedId_.clear();
    }

    pointer_.pressed = false;
    pointer_.released = false;
    pointer_.wheelDelta = 0.0f;
    keyboard_ = KeyboardState {};
    focusJustSet_ = false;
}

float Ui::textWidth(const std::string& value, float size, text::Weight weight)
{
    return textEngine_->shape(value, size, weight).width;
}

float Ui::lineHeight(float size) const
{
    const auto metrics = const_cast<text::TextEngine*>(textEngine_)
                             ->metrics(size, text::Weight::Regular);
    return metrics.ascent + metrics.descent;
}

void Ui::fillRect(float x, float y, float w, float h, Color color, float radius)
{
    appendQuad(instances_, x, y, w, h, color, radius, 0.0f);
}

const GlyphSprite& Ui::ensureGlyph(
    text::Weight weight, int pixelSize, std::uint32_t glyphIndex)
{
    const auto key = glyphKey(weight, pixelSize, glyphIndex);
    const auto found = glyphCache_.find(key);
    if (found != glyphCache_.end()) {
        return found->second;
    }

    text::TextEngine::Bitmap bitmap;
    if (!textEngine_->rasterize(weight, static_cast<float>(pixelSize),
            glyphIndex, bitmap)) {
        static const GlyphSprite empty;
        return empty;
    }

    GlyphSprite sprite;
    sprite.width = bitmap.width;
    sprite.height = bitmap.height;
    sprite.bearingX = bitmap.bearingX;
    sprite.bearingY = bitmap.bearingY;
    sprite.advanceX = bitmap.advanceX;
    sprite.region = renderer_->allocateAtlas(bitmap.width, bitmap.height);
    renderer_->uploadAtlas(sprite.region, bitmap.width, bitmap.height,
        bitmap.alpha.data());
    return glyphCache_.emplace(key, sprite).first->second;
}

void Ui::text(const std::string& value, float x, float y, float size,
    text::Weight weight, Color color)
{
    const auto run = textEngine_->shape(value, size, weight);
    const auto metrics =
        textEngine_->metrics(size, weight);
    const int pixelSize = std::max(1, static_cast<int>(std::lround(size)));
    const float baseline = y + metrics.ascent;

    for (const auto& shaped : run.glyphs) {
        const auto& sprite = ensureGlyph(weight, pixelSize, shaped.index);
        if (sprite.width > 0 && sprite.height > 0) {
            const float glyphX = x + shaped.x + static_cast<float>(sprite.bearingX);
            const float glyphY =
                baseline - static_cast<float>(sprite.bearingY);
            appendQuad(instances_, glyphX, glyphY,
                static_cast<float>(sprite.width),
                static_cast<float>(sprite.height), color, 0.0f, 1.0f,
                { sprite.region.u0, sprite.region.v0, sprite.region.u1,
                    sprite.region.v1 });
        }
    }
}

void Ui::textTruncated(const std::string& value, float x, float y, float w,
    float size, text::Weight weight, Color color)
{
    if (textWidth(value, size, weight) <= w || w <= 0.0f) {
        text(value, x, y, size, weight, color);
        return;
    }
    std::size_t length = value.size();
    while (length > 1) {
        --length;
        while (length > 0 && (value[length] & 0xC0) == 0x80) {
            --length;
        }
        if (textWidth(value.substr(0, length) + "…", size, weight) <= w) {
            break;
        }
    }
    text(value.substr(0, length) + "…", x, y, size, weight, color);
}

bool Ui::pointerInRect(float x, float y, float w, float h) const
{
    return pointer_.x >= x && pointer_.x <= x + w && pointer_.y >= y
        && pointer_.y <= y + h;
}

bool Ui::consumeClick(const char* id, float x, float y, float w, float h)
{
    if (!pointerInRect(x, y, w, h)) {
        return false;
    }
    hoveredId_ = id;
    if (pointer_.pressed) {
        pressedId_ = id;
    }
    if (pointer_.released && pressedId_ == id) {
        pressedId_.clear();
        return true;
    }
    return false;
}

bool Ui::button(const char* id, const std::string& label, float x, float y,
    float w, float h, bool primary)
{
    const bool hover = pointerInRect(x, y, w, h);
    const bool clicked = consumeClick(id, x, y, w, h);

    Color fill = primary ? palette::accent : palette::surfaceRaised;
    if (hover) {
        fill.a *= 1.25f;
    }
    fillRect(x, y, w, h, fill, 6.0f);

    const float labelWidth = textWidth(label, 13.0f, text::Weight::Bold);
    text(label, x + (w - labelWidth) * 0.5f, y + (h - lineHeight(13.0f)) * 0.5f,
        13.0f, text::Weight::Bold, palette::text);
    return clicked;
}

bool Ui::rowClicked(const char* id, float x, float y, float w, float h)
{
    if (pointerInRect(x, y, w, h)) {
        hoveredId_ = id;
    }
    if (hoveredId_ == id) {
        fillRect(x, y, w, h, Color { 1.0f, 1.0f, 1.0f, 0.04f }, 6.0f);
    }
    return consumeClick(id, x, y, w, h);
}

bool Ui::textField(const char* id, std::string& buffer,
    const std::string& placeholder, float x, float y, float w, float h,
    bool& focusedOut)
{
    focusedOut = false;
    const bool hover = pointerInRect(x, y, w, h);
    bool submitted = false;

    if (hover && pointer_.pressed) {
        focused_ = id;
        focusJustSet_ = true;
    }

    const bool isFocused = !focused_.empty() && focused_ == id;
    fillRect(x, y, w, h,
        isFocused ? palette::surfaceRaised : palette::surface, 6.0f);
    if (isFocused) {
        fillRect(x, y, w, 2.0f, palette::accent, 1.0f);
    }

    const float padding = 10.0f;
    const float innerW = w - padding * 2.0f;
    const float textY = y + (h - lineHeight(13.0f)) * 0.5f;

    if (isFocused) {
        for (const char character : keyboard_.text) {
            buffer.push_back(character);
        }
        if (keyboard_.backspace && !buffer.empty()) {
            while (!buffer.empty() && (buffer.back() & 0xC0) == 0x80) {
                buffer.pop_back();
            }
            if (!buffer.empty()) {
                buffer.pop_back();
            }
        }
        if (keyboard_.enter) {
            submitted = true;
        }
        if (keyboard_.escape) {
            focused_.clear();
            focusedOut = true;
        }
    }

    if (buffer.empty() && !isFocused) {
        textTruncated(placeholder, x + padding, textY, innerW, 13.0f,
            text::Weight::Regular, palette::textFaint);
    } else {
        textTruncated(buffer, x + padding, textY, innerW, 13.0f,
            text::Weight::Regular, palette::text);
    }

    if (isFocused) {
        const float cursorOffset = textWidth(buffer, 13.0f, text::Weight::Regular);
        if ((std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                .count()
                / 500)
                % 2
            == 0) {
            fillRect(x + padding + cursorOffset, textY - 1.0f, 1.5f,
                lineHeight(13.0f) + 2.0f, palette::accentDim, 0.0f);
        }
    }

    static_cast<void>(consumeClick(id, x, y, w, h));
    return submitted;
}

bool Ui::slider(const char* id, float& value01, float x, float y, float w,
    float h)
{
    const bool hover = pointerInRect(x, y, w, h);
    static_cast<void>(consumeClick(id, x, y, w, h));

    fillRect(x, y + h * 0.35f, w, h * 0.30f, palette::surfaceRaised,
        h * 0.15f);
    const float clamped = std::clamp(value01, 0.0f, 1.0f);
    fillRect(x, y + h * 0.35f, w * clamped, h * 0.30f, palette::accent,
        h * 0.15f);

    bool changed = false;
    if (pointer_.down && hover) {
        const float newValue = std::clamp((pointer_.x - x) / w, 0.0f, 1.0f);
        if (newValue != value01) {
            value01 = newValue;
            changed = true;
        }
    }
    return changed;
}

void Ui::progressBar(float ratio, float x, float y, float w, float h)
{
    fillRect(x, y, w, h, palette::surfaceRaised, h * 0.5f);
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    if (ratio > 0.001f) {
        fillRect(x, y, w * ratio, h, palette::accentDim, h * 0.5f);
    }
}

bool Ui::scrolled(float x, float y, float w, float h, float contentHeight,
    float& scrollOffset)
{
    if (!pointerInRect(x, y, w, h) || contentHeight <= h) {
        return false;
    }
    const float maxScroll = contentHeight - h;
    const float before = scrollOffset;
    scrollOffset = std::clamp(scrollOffset - pointer_.wheelDelta * 40.0f, 0.0f,
        maxScroll);
    return scrollOffset != before;
}

} // namespace bang::ui
