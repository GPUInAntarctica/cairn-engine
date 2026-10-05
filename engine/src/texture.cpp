#include "cairn/texture.h"

#include "cairn/log.h"
#include "cairn/math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_HDR
#include "stb/stb_image.h"

namespace Cairn {

Texture::Texture(int w, int h, const uint8_t* rgba) : width_(w), height_(h) {
    pixels_.assign((size_t)w * h * 4, 0);
    if (rgba) std::memcpy(pixels_.data(), rgba, pixels_.size());
}

Texture::~Texture() = default;
Texture::Texture(Texture&&) noexcept = default;
Texture& Texture::operator=(Texture&&) noexcept = default;

std::shared_ptr<Texture> Texture::LoadFromMemory(const uint8_t* data, size_t size) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* decoded = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
    if (!decoded) return nullptr;
    auto tex = std::make_shared<Texture>(w, h, decoded);
    stbi_image_free(decoded);
    tex->setSRGB(true);   // image files hold gamma-encoded color
    tex->GenerateMipmaps();
    return tex;
}

std::shared_ptr<Texture> Texture::LoadFromFile(const std::string& absolutePath) {
    FILE* f = std::fopen(absolutePath.c_str(), "rb");
    if (!f) return nullptr;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return nullptr; }
    std::vector<uint8_t> bytes((size_t)size);
    size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (read != bytes.size()) return nullptr;
    return LoadFromMemory(bytes.data(), bytes.size());
}

std::shared_ptr<Texture> Texture::Solid(int size, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    auto tex = std::make_shared<Texture>(size, size);
    tex->setSRGB(true);
    for (size_t i = 0; i < tex->pixels_.size(); i += 4) {
        tex->pixels_[i] = r;
        tex->pixels_[i + 1] = g;
        tex->pixels_[i + 2] = b;
        tex->pixels_[i + 3] = a;
    }
    return tex;
}

std::shared_ptr<Texture> Texture::Checker(int size, int cells, uint32_t a, uint32_t b) {
    auto tex = std::make_shared<Texture>(size, size);
    tex->setSRGB(true);
    int cell = std::max(1, size / std::max(1, cells));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            bool odd = ((x / cell) + (y / cell)) % 2 == 1;
            uint32_t c = odd ? b : a;
            size_t i = ((size_t)y * size + x) * 4;
            tex->pixels_[i] = (uint8_t)((c >> 16) & 0xFF);
            tex->pixels_[i + 1] = (uint8_t)((c >> 8) & 0xFF);
            tex->pixels_[i + 2] = (uint8_t)(c & 0xFF);
            tex->pixels_[i + 3] = (uint8_t)((c >> 24) & 0xFF);
        }
    }
    tex->GenerateMipmaps();
    return tex;
}

std::shared_ptr<Texture> Texture::UvGrid(int size, int cells) {
    auto tex = std::make_shared<Texture>(size, size);
    tex->setSRGB(true);
    int cell = std::max(1, size / std::max(1, cells));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            size_t i = ((size_t)y * size + x) * 4;
            bool border = (x % cell) == 0 || (y % cell) == 0;
            if (border) {
                tex->pixels_[i] = 30; tex->pixels_[i + 1] = 30; tex->pixels_[i + 2] = 34; tex->pixels_[i + 3] = 255;
            } else {
                float u = (float)(x % cell) / cell;
                float v = (float)(y % cell) / cell;
                tex->pixels_[i] = (uint8_t)(90 + u * 120);
                tex->pixels_[i + 1] = (uint8_t)(96 + v * 120);
                tex->pixels_[i + 2] = 112;
                tex->pixels_[i + 3] = 255;
            }
        }
    }
    tex->GenerateMipmaps();
    return tex;
}

std::shared_ptr<Texture> Texture::Circle(int size, uint32_t fill, uint32_t bg) {
    auto tex = std::make_shared<Texture>(size, size);
    tex->setSRGB(true);
    float r = size * 0.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = x + 0.5f - r, dy = y + 0.5f - r;
            float dist = std::sqrt(dx * dx + dy * dy);
            float alpha = dist > r - 1.0f ? (r - dist) : 1.0f;  // 1px soft edge
            alpha = std::clamp(alpha, 0.0f, 1.0f);
            uint32_t c = fill;
            size_t i = ((size_t)y * size + x) * 4;
            tex->pixels_[i] = (uint8_t)(((c >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (1 - alpha));
            tex->pixels_[i + 1] = (uint8_t)(((c >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (1 - alpha));
            tex->pixels_[i + 2] = (uint8_t)((c & 0xFF) * alpha + (bg & 0xFF) * (1 - alpha));
            tex->pixels_[i + 3] = (uint8_t)(((c >> 24) & 0xFF) * alpha + ((bg >> 24) & 0xFF) * (1 - alpha));
        }
    }
    return tex;
}

void Texture::GenerateMipmaps() {
    levels_.clear();
    if (width_ <= 0 || height_ <= 0) return;
    levels_.push_back(pixels_);
    int w = width_, h = height_;
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const std::vector<uint8_t>& src = levels_.back();
        std::vector<uint8_t> dst((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                // 2x2 box filter, clamped at the edges of odd sizes.
                int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                const uint8_t* a = &src[((size_t)y0 * w + x0) * 4];
                const uint8_t* b = &src[((size_t)y0 * w + x1) * 4];
                const uint8_t* c = &src[((size_t)y1 * w + x0) * 4];
                const uint8_t* d = &src[((size_t)y1 * w + x1) * 4];
                uint8_t* o = &dst[((size_t)y * nw + x) * 4];
                for (int k = 0; k < 4; ++k) {
                    if (srgb_ && k < 3) {
                        // Averaging gamma-encoded values darkens the result, so
                        // decode, average, re-encode.
                        const float lin = (SrgbToLinearFast(a[k] / 255.0f) + SrgbToLinearFast(b[k] / 255.0f) +
                                           SrgbToLinearFast(c[k] / 255.0f) + SrgbToLinearFast(d[k] / 255.0f)) * 0.25f;
                        o[k] = (uint8_t)std::clamp(LinearToSrgbFast(lin) * 255.0f + 0.5f, 0.0f, 255.0f);
                    } else {
                        o[k] = (uint8_t)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
                    }
                }
            }
        }
        levels_.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

// Wraps or clamps a uv pair for the given addressing mode.
static void WrapUv(float u, float v, Texture::Wrap wrap, float* uo, float* vo) {
    if (wrap == Texture::Wrap::Repeat) {
        *uo = u - std::floor(u);
        *vo = v - std::floor(v);
    } else if (wrap == Texture::Wrap::Mirror) {
        float a = u - std::floor(u);
        float b = v - std::floor(v);
        *uo = (std::fmod(std::floor(u), 2.0f) == 0.0f) ? a : 1.0f - a;
        *vo = (std::fmod(std::floor(v), 2.0f) == 0.0f) ? b : 1.0f - b;
    } else {
        *uo = std::clamp(u, 0.0f, 1.0f);
        *vo = std::clamp(v, 0.0f, 1.0f);
    }
}

static int WrapIndex(int i, int size, Texture::Wrap wrap) {
    switch (wrap) {
        case Texture::Wrap::Repeat: return ((i % size) + size) % size;
        case Texture::Wrap::Mirror: {
            int period = size * 2;
            int m = ((i % period) + period) % period;
            return m < size ? m : period - 1 - m;
        }
        default: return std::clamp(i, 0, size - 1);
    }
}

void Texture::SampleLevel(int level, float u, float v, Wrap wrap, Filter filter, float out[4]) const {
    const std::vector<uint8_t>* src = &pixels_;
    int w = width_, h = height_;
    if (!levels_.empty()) {
        level = std::clamp(level, 0, (int)levels_.size() - 1);
        src = &levels_[level];
        w = levelWidth(level);
        h = levelHeight(level);
    } else {
        level = 0;
    }
    float uu, vv;
    WrapUv(u, v, wrap, &uu, &vv);
    float fx = uu * w - 0.5f;
    float fy = vv * h - 0.5f;

    if (filter == Filter::Nearest) {
        int x = std::clamp((int)std::floor(fx + 0.5f), 0, w - 1);
        int y = std::clamp((int)std::floor(fy + 0.5f), 0, h - 1);
        const uint8_t* p = &(*src)[((size_t)y * w + x) * 4];
        for (int i = 0; i < 4; ++i) {
            out[i] = p[i] / 255.0f;
            if (srgb_ && i < 3) out[i] = SrgbToLinearFast(out[i]);
        }
        return;
    }
    const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - x0, ty = fy - y0;
    float c[4][4];
    const int xs[2] = {x0, x0 + 1};
    const int ys[2] = {y0, y0 + 1};
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int xx = WrapIndex(xs[i], w, wrap);
            const int yy = WrapIndex(ys[j], h, wrap);
            const uint8_t* p = &(*src)[((size_t)yy * w + xx) * 4];
            for (int k = 0; k < 4; ++k) {
                float value = p[k] / 255.0f;
                if (srgb_ && k < 3) value = SrgbToLinearFast(value);
                c[j * 2 + i][k] = value;
            }
        }
    }
    // Taps: c[0]=(x0,y0) c[1]=(x1,y0) c[2]=(x0,y1) c[3]=(x1,y1); second index
    // is the channel.
    for (int k = 0; k < 4; ++k) {
        const float top = c[0][k] + (c[1][k] - c[0][k]) * tx;
        const float bottom = c[2][k] + (c[3][k] - c[2][k]) * tx;
        out[k] = top + (bottom - top) * ty;
    }
}

void Texture::Sample(float u, float v, Wrap wrap, Filter filter, float out[4]) const {
    SampleLod(u, v, 0.0f, wrap, filter, out);
}

void Texture::SampleLod(float u, float v, float lod, Wrap wrap, Filter filter, float out[4]) const {
    if (width_ <= 0 || height_ <= 0) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    const int levels = mipLevels();
    if (levels <= 1 || filter != Filter::Trilinear) {
        // No chain (or no trilinear request): a coarser lod still clamps to the
        // smallest available level so callers can pass a lod unconditionally.
        const int level = levels <= 1 ? 0 : std::clamp((int)std::lround(lod), 0, levels - 1);
        SampleLevel(level, u, v, wrap, filter == Filter::Trilinear ? Filter::Bilinear : filter, out);
        return;
    }
    lod = std::clamp(lod, 0.0f, (float)(levels - 1));
    const int l0 = (int)std::floor(lod);
    const int l1 = std::min(l0 + 1, levels - 1);
    const float t = lod - (float)l0;
    float a[4], b[4];
    SampleLevel(l0, u, v, wrap, Filter::Bilinear, a);
    SampleLevel(l1, u, v, wrap, Filter::Bilinear, b);
    for (int i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
}

size_t Texture::ByteCount() const {
    // Level 0 lives in pixels_, further levels in levels_ (levels_[0] mirrors
    // pixels_ when a chain exists, so skip it to avoid double counting).
    size_t bytes = (size_t)width_ * height_ * 4;
    for (size_t i = 1; i < levels_.size(); ++i) {
        bytes += (size_t)levelWidth((int)i) * levelHeight((int)i) * 4;
    }
    return bytes;
}

// TextureLibrary -----------------------------------------------------------
// Cache key: the color-space variant is part of the key so the same file can
// exist once as sRGB (albedo) and once as linear (normal maps).
static std::string CacheKey(const std::string& absolutePath, bool srgb) {
    return srgb ? absolutePath : absolutePath + "|linear";
}

std::shared_ptr<Texture> TextureLibrary::Get(const std::string& absolutePath, bool srgb) {
    const std::string key = CacheKey(absolutePath, srgb);
    if (Entry* e = Find(key)) {
        Touch(*e);
        Evict();
        return e->texture;
    }
    auto tex = Texture::LoadFromFile(absolutePath);
    if (!tex) {
        tex = GetBuiltin("missing");
        Log::Warning("texture not found: %s", absolutePath.c_str());
        // The missing placeholder is shared; do not account it against the
        // budget (it is also reachable through GetBuiltin).
        return tex;
    }
    tex->setSRGB(srgb);
    if (!tex->hasMipmaps()) tex->GenerateMipmaps();  // flag flip drops the chain
    Entry e;
    e.texture = tex;
    e.lastUse = ++useClock_;
    std::error_code ec;
    e.mtime = std::filesystem::last_write_time(absolutePath, ec);
    e.hasFile = !ec;
    cachedBytes_ += tex->ByteCount();
    cache_[key] = std::move(e);
    Evict();
    return cache_[key].texture;
}

TextureLibrary::Entry* TextureLibrary::Find(const std::string& key) {
    auto it = cache_.find(key);
    return it == cache_.end() ? nullptr : &it->second;
}

void TextureLibrary::Touch(Entry& e) { e.lastUse = ++useClock_; }

// LRU eviction: least-recently-used entries go first until the cache fits the
// budget. The most recently touched entry survives even if it alone exceeds
// the budget (a texture in use cannot just vanish).
void TextureLibrary::Evict() {
    if (cachedBytes_ <= budgetBytes_ || cache_.empty()) return;
    // Simple O(n log n) sweep; caches hold dozens of entries, not thousands.
    std::vector<std::pair<uint64_t, std::string>> order;
    order.reserve(cache_.size());
    for (const auto& [key, e] : cache_) order.push_back({e.lastUse, key});
    std::sort(order.begin(), order.end());
    for (const auto& [_, key] : order) {
        if (cachedBytes_ <= budgetBytes_) break;
        auto it = cache_.find(key);
        // Keep at least the newest entry even when over budget.
        if (it->second.lastUse == useClock_ && cache_.size() == 1) break;
        cachedBytes_ -= it->second.texture->ByteCount();
        cache_.erase(it);
    }
}

std::shared_ptr<Texture> TextureLibrary::GetBuiltin(const std::string& name) {
    auto it = builtin_.find(name);
    if (it != builtin_.end()) return it->second;
    std::shared_ptr<Texture> tex;
    if (name == "white") tex = Texture::Solid(4, 255, 255, 255, 255);
    else if (name == "missing") tex = Texture::Checker(64, 4, 0xFF3FA0A0u, 0xFF283238u);
    else if (name == "checker") tex = Texture::Checker(256, 8, 0xFFE0E0E0u, 0xFF605F60u);
    else if (name == "uvgrid") tex = Texture::UvGrid(256, 4);
    else if (name == "circle") tex = Texture::Circle(64, 0xFFFFFFFFu, 0x00000000u);
    else tex = Texture::Solid(4, 255, 255, 255, 255);
    builtin_[name] = tex;
    return tex;
}

void TextureLibrary::Clear() {
    cache_.clear();
    cachedBytes_ = 0;
}

void TextureLibrary::ReloadChanged() {
    std::error_code ec;
    for (auto it = cache_.begin(); it != cache_.end();) {
        // The key may carry the "|linear" suffix; recover the file path.
        std::string path = it->first;
        const size_t suffix = path.find("|linear");
        const bool linear = suffix != std::string::npos;
        if (linear) path = path.substr(0, suffix);
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec || mtime != it->second.mtime) {
            cachedBytes_ -= it->second.texture->ByteCount();
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void TextureLibrary::ReloadPath(const std::string& absolutePath) {
    // Conditional: only drop the entries if the file actually changed, so hot
    // reload does not force pointless reloads of untouched textures.
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(absolutePath, ec);
    for (const std::string key : {absolutePath, CacheKey(absolutePath, false)}) {
        auto it = cache_.find(key);
        if (it == cache_.end()) continue;
        if (ec || mtime != it->second.mtime) {
            cachedBytes_ -= it->second.texture->ByteCount();
            cache_.erase(it);
        }
    }
}

}  // namespace Cairn
