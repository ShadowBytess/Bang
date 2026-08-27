#include "bang/text/TextEngine.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb-ft.h>
#include <hb-ot.h>
#include <hb.h>

#include <cmath>
#include <stdexcept>

namespace bang::text {

struct TextEngine::FontSlot {
    FT_Face face = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* faceHandle = nullptr;
    hb_font_t* font = nullptr;
    int loadedPixelSize = -1;

    ~FontSlot()
    {
        if (font != nullptr) {
            hb_font_destroy(font);
        }
        if (faceHandle != nullptr) {
            hb_face_destroy(faceHandle);
        }
        if (blob != nullptr) {
            hb_blob_destroy(blob);
        }
        if (face != nullptr && freetypeLibrary != nullptr) {
            FT_Done_Face(face);
        }
    }
    void* freetypeLibrary = nullptr;
};

namespace {

FT_Face createFace(void* library, const std::uint8_t* data, std::size_t size)
{
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(static_cast<FT_Library>(library), data,
            static_cast<FT_Long>(size), 0, &face)
        != 0) {
        throw std::runtime_error("cannot load embedded font face");
    }
    return face;
}

void* initFreetype()
{
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) {
        throw std::runtime_error("cannot initialize FreeType");
    }
    return library;
}

} // namespace

TextEngine::TextEngine(const std::uint8_t* regularData, std::size_t regularSize,
    const std::uint8_t* boldData, std::size_t boldSize)
    : freetypeLibrary_(initFreetype())
{
    regular_ = new FontSlot;
    bold_ = new FontSlot;
    regular_->freetypeLibrary = freetypeLibrary_;
    regular_->face = createFace(freetypeLibrary_, regularData, regularSize);
    regular_->blob = hb_blob_create(reinterpret_cast<const char*>(regularData),
        static_cast<unsigned int>(regularSize), HB_MEMORY_MODE_READONLY, nullptr,
        nullptr);
    regular_->faceHandle = hb_face_create(regular_->blob, 0);
    regular_->font = hb_font_create(regular_->faceHandle);
    hb_ot_font_set_funcs(regular_->font);

    bold_->freetypeLibrary = freetypeLibrary_;
    bold_->face = createFace(freetypeLibrary_, boldData, boldSize);
    bold_->blob = hb_blob_create(reinterpret_cast<const char*>(boldData),
        static_cast<unsigned int>(boldSize), HB_MEMORY_MODE_READONLY, nullptr,
        nullptr);
    bold_->faceHandle = hb_face_create(bold_->blob, 0);
    bold_->font = hb_font_create(bold_->faceHandle);
    hb_ot_font_set_funcs(bold_->font);
}

TextEngine::~TextEngine()
{
    delete regular_;
    delete bold_;
    if (freetypeLibrary_ != nullptr) {
        FT_Done_FreeType(static_cast<FT_Library>(freetypeLibrary_));
    }
}

ShapedRun TextEngine::shape(
    std::string_view utf8, float pixelSize, Weight weight) const
{
    FontSlot& slot = weight == Weight::Bold ? *bold_ : *regular_;

    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, utf8.data(), static_cast<int>(utf8.size()), 0, -1);
    hb_buffer_guess_segment_properties(buffer);

    const unsigned int scale = static_cast<unsigned int>(pixelSize * 64.0f);
    hb_font_set_scale(slot.font, scale, scale);
    hb_shape(slot.font, buffer, nullptr, 0);

    unsigned int glyphCount = 0;
    const hb_glyph_info_t* infos =
        hb_buffer_get_glyph_infos(buffer, &glyphCount);
    const hb_glyph_position_t* positions =
        hb_buffer_get_glyph_positions(buffer, &glyphCount);

    ShapedRun run;
    run.glyphs.reserve(glyphCount);
    float penX = 0.0f;
    for (unsigned int index = 0; index < glyphCount; ++index) {
        ShapedGlyph glyph;
        glyph.index = infos[index].codepoint;
        glyph.x = penX + static_cast<float>(positions[index].x_offset) / 64.0f;
        glyph.y = static_cast<float>(positions[index].y_offset) / 64.0f;
        penX += static_cast<float>(positions[index].x_advance) / 64.0f;
        run.glyphs.push_back(glyph);
    }
    run.width = penX;
    hb_buffer_destroy(buffer);
    return run;
}

bool TextEngine::rasterize(Weight weight, float pixelSize,
    std::uint32_t glyphIndex, Bitmap& out)
{
    FontSlot& slot = weight == Weight::Bold ? *bold_ : *regular_;
    const int pixelHeight = std::max(1, static_cast<int>(std::lround(pixelSize)));
    if (slot.loadedPixelSize != pixelHeight) {
        FT_Set_Pixel_Sizes(slot.face, 0, pixelHeight);
        slot.loadedPixelSize = pixelHeight;
    }

    if (FT_Load_Glyph(slot.face, glyphIndex, FT_LOAD_DEFAULT) != 0) {
        return false;
    }
    out.advanceX = static_cast<float>(slot.face->glyph->advance.x) / 64.0f;

    if (FT_Render_Glyph(slot.face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return false;
    }
    const FT_Bitmap& bitmap = slot.face->glyph->bitmap;
    out.width = static_cast<int>(bitmap.width);
    out.height = static_cast<int>(bitmap.rows);
    out.bearingX = slot.face->glyph->bitmap_left;
    out.bearingY = slot.face->glyph->bitmap_top;

    out.alpha.assign(bitmap.width * bitmap.rows, 0);
    for (int row = 0; row < out.height; ++row) {
        const auto* source = bitmap.buffer + static_cast<std::ptrdiff_t>(row)
                * bitmap.pitch;
        std::uint8_t* target =
            out.alpha.data() + static_cast<std::size_t>(row) * bitmap.width;
        for (int column = 0; column < out.width; ++column) {
            target[column] = source[column];
        }
    }
    return true;
}

TextEngine::Metrics TextEngine::metrics(float pixelSize, Weight weight)
{
    FontSlot& slot = weight == Weight::Bold ? *bold_ : *regular_;
    const int pixelHeight = std::max(1, static_cast<int>(std::lround(pixelSize)));
    if (slot.loadedPixelSize != pixelHeight) {
        FT_Set_Pixel_Sizes(slot.face, 0, pixelHeight);
        slot.loadedPixelSize = pixelHeight;
    }
    const auto& sizeMetrics = slot.face->size->metrics;
    Metrics result;
    result.ascent = static_cast<float>(sizeMetrics.ascender) / 64.0f;
    result.descent = static_cast<float>(-sizeMetrics.descender) / 64.0f;
    return result;
}

} // namespace bang::text
