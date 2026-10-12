#include "assets/image.h"
#include "core/vfs.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#include <miniz.h>
#include <cstring>

namespace sh {

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool decodeStb(const uint8_t* d, size_t n, Image& out, std::string* err) {
    int w, h, c;
    uint8_t* px = stbi_load_from_memory(d, (int)n, &w, &h, &c, 4);
    if (!px) {
        if (err) *err = stbi_failure_reason();
        return false;
    }
    out.w = w;
    out.h = h;
    out.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

bool decodeImageMemory(const uint8_t* d, size_t n, bool isMtx, Image& out, std::string* err) {
    if (!isMtx) return decodeStb(d, n, out, err);
    if (n < 16 || rd32(d + 8) != 0 || 12 + (size_t)rd32(d + 4) != n) {
        if (err) *err = "bad mtx header";
        return false;
    }
    uint32_t flag = rd32(d);
    if (flag == 0) return decodeStb(d + 12, n - 12, out, err);
    if (flag != 1 || n < 32) {
        if (err) *err = "unknown mtx variant";
        return false;
    }
    uint32_t w = rd32(d + 16), h = rd32(d + 20), jl = rd32(d + 24);
    if (28 + (size_t)jl + 4 > n) {
        if (err) *err = "bad mtx jpeg length";
        return false;
    }
    if (!decodeStb(d + 28, jl, out, err)) return false;
    if ((uint32_t)out.w != w || (uint32_t)out.h != h) {
        if (err) *err = "mtx size mismatch";
        return false;
    }
    uint32_t al = rd32(d + 28 + jl);
    std::vector<uint8_t> a((size_t)w * h);
    mz_ulong sz = (mz_ulong)a.size();
    if (28 + (size_t)jl + 4 + al > n || mz_uncompress(a.data(), &sz, d + 32 + jl, al) != MZ_OK || sz != a.size()) {
        if (err) *err = "bad mtx alpha";
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) out.rgba[i * 4 + 3] = a[i];
    return true;
}

bool decodeImage(const Vfs& vfs, const std::string& name, Image& out, std::string* err) {
    // The engine asks for "x.png"/"x.jpg" and falls back to the packed "x.png.mtx" container.
    std::vector<uint8_t> b;
    bool mtx = false;
    if (!vfs.read(name, b)) {
        if (!vfs.read(name + ".mtx", b)) {
            if (err) *err = "not found: " + name;
            return false;
        }
        mtx = true;
    } else {
        mtx = name.size() > 4 && name.compare(name.size() - 4, 4, ".mtx") == 0;
    }
    return decodeImageMemory(b.data(), b.size(), mtx, out, err);
}

} // namespace sh
