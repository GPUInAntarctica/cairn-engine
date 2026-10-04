#include "cairn/texture.h"

#include "cairn/log.h"

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
    return tex;
}

std::shared_ptr<Texture> Texture::UvGrid(int size, int cells) {
    auto tex = std::make_shared<Texture>(size, size);
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
    return tex;
}

std::shared_ptr<Texture> Texture::Circle(int size, uint32_t fill, uint32_t bg) {
    auto tex = std::make_shared<Texture>(size, size);
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

void Texture::Sample(float u, float v, Wrap wrap, Filter filter, float out[4]) const {
    if (width_ <= 0 || height_ <= 0) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    if (wrap == Wrap::Repeat) {
        u = u - std::floor(u);
        v = v - std::floor(v);
    } else {
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);
    }
    float fx = u * width_ - 0.5f;
    float fy = v * height_ - 0.5f;
    if (filter == Filter::Nearest) {
        int x = (int)std::floor(fx + 0.5f);
        int y = (int)std::floor(fy + 0.5f);
        x = std::clamp(x, 0, width_ - 1);
        y = std::clamp(y, 0, height_ - 1);
        const uint8_t* p = &pixels_[((size_t)y * width_ + x) * 4];
        out[0] = p[0] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[2] / 255.0f; out[3] = p[3] / 255.0f;
        return;
    }
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - x0, ty = fy - y0;
    auto fetch = [&](int x, int y, float outc[4]) {
        if (wrap == Wrap::Repeat) {
            x = ((x % width_) + width_) % width_;
            y = ((y % height_) + height_) % height_;
        } else {
            x = std::clamp(x, 0, width_ - 1);
            y = std::clamp(y, 0, height_ - 1);
        }
        const uint8_t* p = &pixels_[((size_t)y * width_ + x) * 4];
        outc[0] = p[0] / 255.0f; outc[1] = p[1] / 255.0f; outc[2] = p[2] / 255.0f; outc[3] = p[3] / 255.0f;
    };
    float c00[4], c10[4], c01[4], c11[4];
    fetch(x0, y0, c00);
    fetch(x0 + 1, y0, c10);
    fetch(x0, y0 + 1, c01);
    fetch(x0 + 1, y0 + 1, c11);
    for (int i = 0; i < 4; ++i) {
        float a = c00[i] + (c10[i] - c00[i]) * tx;
        float b = c01[i] + (c11[i] - c01[i]) * tx;
        out[i] = a + (b - a) * ty;
    }
}

std::shared_ptr<Texture> TextureLibrary::Get(const std::string& absolutePath) {
    auto it = cache_.find(absolutePath);
    if (it != cache_.end()) return it->second;
    auto tex = Texture::LoadFromFile(absolutePath);
    if (!tex) {
        tex = GetBuiltin("missing");
        Log::Warning("texture not found: %s", absolutePath.c_str());
    }
    cache_[absolutePath] = tex;
    return tex;
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

void TextureLibrary::Clear() { cache_.clear(); }

void TextureLibrary::ReloadPath(const std::string& absolutePath) { cache_.erase(absolutePath); }

}  // namespace Cairn
