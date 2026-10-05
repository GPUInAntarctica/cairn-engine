// Cairn Engine - CPU textures. RGBA8, top-left origin, v = 0 at the top.
#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Cairn {

class Texture {
public:
    Texture() = default;
    Texture(int w, int h, const uint8_t* rgba = nullptr);
    ~Texture();
    Texture(Texture&&) noexcept;
    Texture& operator=(Texture&&) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    bool IsValid() const { return width_ > 0 && height_ > 0 && !pixels_.empty(); }
    int width() const { return width_; }
    int height() const { return height_; }
    const uint8_t* pixels() const { return pixels_.data(); }
    uint8_t* pixels() { return pixels_.data(); }

    // Mip chain -------------------------------------------------------------
    // Level 0 is the full-resolution image; higher levels are halved until a
    // dimension reaches 1. Minified surfaces sample a coarser level, which is
    // what stops textures from shimmering at distance.
    // Always at least one level: level 0 is the image itself.
    int mipLevels() const { return levels_.empty() ? 1 : (int)levels_.size(); }
    bool hasMipmaps() const { return levels_.size() > 1; }
    // Builds the chain with a 2x2 box filter. Cheap, and called once per load.
    void GenerateMipmaps();
    // Drops the chain (call after writing pixels()).
    void InvalidateMipmaps() { levels_.clear(); }
    int levelWidth(int level) const { return std::max(1, width_ >> level); }
    int levelHeight(int level) const { return std::max(1, height_ >> level); }

    // Color space. sRGB textures (albedo, sprites) are decoded to linear on
    // sampling and averaged in linear space when the mip chain is built; data
    // textures (normal maps, masks) stay linear.
    bool isSRGB() const { return srgb_; }
    // Changing the flag invalidates the mip chain (mips are averaged in the
    // texture's decode space); no-op when the value already matches.
    void setSRGB(bool on) { if (srgb_ == on) return; srgb_ = on; levels_.clear(); }

    // Bytes held by the full mip chain: sum of levelWidth*levelHeight*4.
    // The texture cache budgets against this number.
    size_t ByteCount() const;

    // Loads png/jpg/bmp/tga via stb_image; returns nullptr on failure.
    static std::shared_ptr<Texture> LoadFromFile(const std::string& absolutePath);
    // Decodes an in-memory image file (png/jpg/bmp).
    static std::shared_ptr<Texture> LoadFromMemory(const uint8_t* data, size_t size);

    // Procedural textures used by templates and as fallbacks.
    static std::shared_ptr<Texture> Solid(int size, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    static std::shared_ptr<Texture> Checker(int size, int cells, uint32_t a, uint32_t b);
    static std::shared_ptr<Texture> UvGrid(int size, int cells);
    static std::shared_ptr<Texture> Circle(int size, uint32_t fill, uint32_t bg);

    enum class Wrap : int { Repeat, Clamp, Mirror };
    enum class Filter : int { Nearest, Bilinear, Trilinear };

    // Samples with uv in [0,1], v = 0 at top. Returns straight alpha.
    void Sample(float u, float v, Wrap wrap, Filter filter, float out[4]) const;
    // Explicit level of detail (0 = full resolution). Trilinear blends the two
    // neighbouring levels; Nearest/Bilinear sample `level` directly.
    void SampleLod(float u, float v, float lod, Wrap wrap, Filter filter, float out[4]) const;

private:
    void SampleLevel(int level, float u, float v, Wrap wrap, Filter filter, float out[4]) const;

    int width_ = 0, height_ = 0;
    bool srgb_ = false;
    std::vector<uint8_t> pixels_;
    std::vector<std::vector<uint8_t>> levels_;  // levels_[0] mirrors pixels_
};

// Loads and caches textures by absolute path (shared between renderer users),
// with a byte-budgeted LRU: entries above the budget are evicted
// least-recently-used first. Per-slot color space is part of the cache key —
// the same file can exist once as sRGB (albedo) and once as linear (normal
// map). Asset .meta import settings will own color-space selection later; the
// renderer's explicit Get(path, srgb) overload is the temporary mechanism.
class TextureLibrary {
public:
    // srgb=false loads a data texture (normal/emission masks) that samples as
    // linear. Cached separately from the sRGB variant of the same file.
    std::shared_ptr<Texture> Get(const std::string& absolutePath, bool srgb);
    std::shared_ptr<Texture> Get(const std::string& absolutePath) { return Get(absolutePath, true); }
    std::shared_ptr<Texture> GetBuiltin(const std::string& name);    // "white", "checker", "uvgrid", "font"
    void Clear();
    // Evicts entries whose file's mtime changed (or vanished) so the next Get
    // reloads them; cheap no-op for unchanged files.
    void ReloadChanged();
    // Drops one path's entries if the file changed on disk (editor hot reload).
    void ReloadPath(const std::string& absolutePath);

    // LRU budget over the file cache (builtins are never evicted).
    void SetBudgetBytes(size_t bytes) { budgetBytes_ = bytes; Evict(); }
    size_t budgetBytes() const { return budgetBytes_; }
    size_t cachedBytes() const { return cachedBytes_; }

private:
    struct Entry {
        std::shared_ptr<Texture> texture;
        std::filesystem::file_time_type mtime{};
        bool hasFile = false;
        uint64_t lastUse = 0;
    };
    void Touch(Entry& e);
    void Evict();
    Entry* Find(const std::string& key);

    std::map<std::string, Entry> cache_;
    std::map<std::string, std::shared_ptr<Texture>> builtin_;
    size_t budgetBytes_ = 256ull * 1024 * 1024;  // scene setting textureMemoryMB
    size_t cachedBytes_ = 0;
    uint64_t useClock_ = 0;
};

}  // namespace Cairn
