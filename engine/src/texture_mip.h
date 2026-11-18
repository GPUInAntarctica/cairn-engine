// Cairn Engine - the 2x2 box filter shared by both mip paths.
//
// Texture::GenerateMipmaps builds the CPU chain, and the Vulkan upload path
// rebuilds the same chain for the GPU. Averaging a coarse sRGB level as
// encoded bytes gives a darker colour than averaging it in linear light, so a
// minified black/white checker used to draw 128 on the GPU and 188 on the
// reference. Both paths call this function, which is what keeps the chains
// identical.
#pragma once

#include "cairn/math.h"

#include <algorithm>
#include <cstdint>

namespace Cairn {

// Averages a 2x2 block of RGBA8 texels into `out`. sRGB textures decode the
// colour channels before averaging and re-encode the result; alpha stays
// linear either way. This is the single definition of the filter, so the CPU
// chain and the GPU upload cannot drift apart.
inline void DownsampleTexel2x2(const uint8_t* a, const uint8_t* b, const uint8_t* c,
                               const uint8_t* d, bool srgb, uint8_t* out) {
    for (int k = 0; k < 4; ++k) {
        if (srgb && k < 3) {
            const float lin =
                (SrgbToLinearFast(a[k] / 255.0f) + SrgbToLinearFast(b[k] / 255.0f) +
                 SrgbToLinearFast(c[k] / 255.0f) + SrgbToLinearFast(d[k] / 255.0f)) *
                0.25f;
            out[k] = (uint8_t)std::clamp(LinearToSrgbFast(lin) * 255.0f + 0.5f, 0.0f, 255.0f);
        } else {
            out[k] = (uint8_t)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
        }
    }
}

}  // namespace Cairn
