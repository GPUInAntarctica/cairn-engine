// Cairn Engine - CPU textures. RGBA8, top-left origin, v = 0 at the top.
#pragma once

#include <cstdint>
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

    // Loads png/jpg/bmp/tga via stb_image; returns nullptr on failure.
    static std::shared_ptr<Texture> LoadFromFile(const std::string& absolutePath);
    // Decodes an in-memory image file (png/jpg/bmp).
    static std::shared_ptr<Texture> LoadFromMemory(const uint8_t* data, size_t size);

    // Procedural textures used by templates and as fallbacks.
    static std::shared_ptr<Texture> Solid(int size, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
    static std::shared_ptr<Texture> Checker(int size, int cells, uint32_t a, uint32_t b);
    static std::shared_ptr<Texture> UvGrid(int size, int cells);
    static std::shared_ptr<Texture> Circle(int size, uint32_t fill, uint32_t bg);

    enum class Wrap : int { Repeat, Clamp };
    enum class Filter : int { Nearest, Bilinear };

    // Samples with uv in [0,1], v = 0 at top. Returns premultiplied-by-alpha? No: straight alpha.
    void Sample(float u, float v, Wrap wrap, Filter filter, float out[4]) const;

private:
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> pixels_;
};

// Loads and caches textures by absolute path (shared between renderer users).
class TextureLibrary {
public:
    std::shared_ptr<Texture> Get(const std::string& absolutePath);   // loads once, caches
    std::shared_ptr<Texture> GetBuiltin(const std::string& name);    // "white", "checker", "uvgrid", "font"
    void Clear();
    // Drops entries whose file changed on disk (editor hot reload).
    void ReloadPath(const std::string& absolutePath);

private:
    std::map<std::string, std::shared_ptr<Texture>> cache_;
    std::map<std::string, std::shared_ptr<Texture>> builtin_;
};

}  // namespace Cairn
