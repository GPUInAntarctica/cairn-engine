// Cairn Engine - internal 5x7 bitmap font (public-domain GLCD glyph data).
// Baked into a texture atlas at startup; used by TextRenderer and editor aids.
#pragma once

#include "cairn/texture.h"

namespace Cairn {

struct GlyphRect {
    float u, v, w, h;  // uv rect in the atlas (v top-down)
    int advance;       // pixel advance in glyph units (5 + 1 spacing)
};

class BitmapFont {
public:
    // Font covers printable ASCII 32..126. Glyph cells are 5x7 pixels.
    static const BitmapFont& Get();

    const Texture* atlas() const { return atlas_.get(); }
    // Returns false for unsupported characters (mapped to '?').
    bool Glyph(char32_t ch, GlyphRect* out) const;
    // Width in glyph units: advance per char, 7 = line height.
    float Measure(const std::string& text) const;

    static constexpr int kGlyphW = 5;
    static constexpr int kGlyphH = 7;
    static constexpr int kCellW = 6;   // 1px horizontal gap
    static constexpr int kCellH = 9;   // padding rows
    static constexpr int kFirstChar = 32;
    static constexpr int kLastChar = 126;

private:
    BitmapFont();
    std::shared_ptr<Texture> atlas_;
    GlyphRect glyphs_[kLastChar - kFirstChar + 1];
};

}  // namespace Cairn
