#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bang::text {

enum class Weight : std::uint8_t { Regular, Bold };

struct ShapedGlyph {
    std::uint32_t index = 0;
    float x = 0.0f;
    float y = 0.0f;
};

struct ShapedRun {
    std::vector<ShapedGlyph> glyphs;
    float width = 0.0f;
};

class TextEngine {
public:
    TextEngine(const std::uint8_t* regularData, std::size_t regularSize,
        const std::uint8_t* boldData, std::size_t boldSize);
    ~TextEngine();
    TextEngine(const TextEngine&) = delete;
    TextEngine& operator=(const TextEngine&) = delete;

    [[nodiscard]] ShapedRun shape(
        std::string_view utf8, float pixelSize, Weight weight) const;

    struct Metrics {
        float ascent = 0.0f;
        float descent = 0.0f;
    };
    [[nodiscard]] Metrics metrics(float pixelSize, Weight weight);

    struct Bitmap {
        int width = 0;
        int height = 0;
        int bearingX = 0;
        int bearingY = 0;
        float advanceX = 0.0f;
        std::vector<std::uint8_t> alpha;
    };

    bool rasterize(
        Weight weight, float pixelSize, std::uint32_t glyphIndex, Bitmap& out);

private:
    struct FontSlot;
    void* freetypeLibrary_ = nullptr;
    FontSlot* regular_ = nullptr;
    FontSlot* bold_ = nullptr;
};

} // namespace bang::text
