// Parses every data file the game uses from a user-supplied APK or extracted folder.
// Usage: sh_asset_test [apk-or-dir]   (or env SH_ASSETS). Exit 77 (skip) if no assets given.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "core/vfs.h"
#include "core/xml.h"
#include "script/lua_host.h"
#include "audio/ogg.h"
#include "assets/image.h"
#include <miniz.h>

using namespace sh;

static int failures = 0;
static void fail(const std::string& f, const std::string& m) {
    printf("FAIL %s: %s\n", f.c_str(), m.c_str());
    failures++;
}
static bool endsWith(const std::string& s, const char* x) {
    size_t n = strlen(x);
    return s.size() >= n && !s.compare(s.size() - n, n, x);
}
static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool inflateAll(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
    mz_ulong sz = (mz_ulong)(n * 16 + 1024);
    for (int t = 0; t < 8; t++) {
        out.resize(sz);
        int rc = mz_uncompress(out.data(), &sz, d, (mz_ulong)n);
        if (rc == MZ_OK) {
            out.resize(sz);
            return true;
        }
        if (rc != MZ_BUF_ERROR) return false;
        sz *= 4;
    }
    return false;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : getenv("SH_ASSETS");
    if (!path) {
        printf("SKIP: pass an APK/asset dir or set SH_ASSETS\n");
        return 77;
    }
    Vfs vfs;
    std::string err;
    if (!vfs.mount(path, &err)) {
        printf("mount failed: %s\n", err.c_str());
        return 1;
    }

    LuaHost lua;
    int nXml = 0, nLua = 0, nMesh = 0, nShader = 0, nMtx = 0, nSeg = 0, nOgg = 0, nPng = 0;
    std::vector<uint8_t> buf, un;
    for (const std::string& f : vfs.list("")) {
        bool ok = true;
        if (endsWith(f, ".xml")) {
            std::string e;
            if (!parseXml(vfs.readText(f), &e)) fail(f, e);
            else nXml++;
        } else if (endsWith(f, ".lua")) {
            std::string e;
            if (!lua.check(vfs.readText(f), "@" + f, &e)) fail(f, e);
            else nLua++;
        } else if (endsWith(f, ".mesh")) {
            vfs.read(f, buf);
            if (!inflateAll(buf.data(), buf.size(), un) || un.size() < 8) {
                fail(f, "inflate");
                continue;
            }
            uint32_t n = rd32(un.data());
            size_t off = 4 + 24ull * n;
            if (off + 4 > un.size()) {
                fail(f, "truncated vertices");
                continue;
            }
            uint32_t t = rd32(&un[off]);
            if (off + 4 + 12ull * t != un.size()) {
                fail(f, "size mismatch");
                continue;
            }
            for (uint32_t i = 0; i < 3 * t && ok; i++)
                if (rd32(&un[off + 4 + 4ull * i]) >= n) {
                    fail(f, "index oob");
                    ok = false;
                }
            if (ok) nMesh++;
        } else if (endsWith(f, ".glsl")) {
            std::string s = vfs.readText(f);
            if (s.find("#ifdef VERTEX") == std::string::npos || s.find("#ifdef FRAGMENT") == std::string::npos)
                fail(f, "missing stages");
            else nShader++;
        } else if (endsWith(f, ".ogg")) {
            vfs.read(f, buf);
            auto o = OggStream::open(buf);
            if (!o) {
                fail(f, "ogg open");
                continue;
            }
            std::vector<float> pcm(4096 * 2);
            uint32_t total = 0;
            int n;
            while ((n = o->read(pcm.data(), 4096)) > 0) total += n;
            if (total == 0) fail(f, "ogg decoded no samples");
            else nOgg++;
        } else if (endsWith(f, ".mtx")) {
            vfs.read(f, buf);
            Image im;
            std::string ie;
            if (!decodeImageMemory(buf.data(), buf.size(), true, im, &ie)) {
                fail(f, "mtx: " + ie);
                continue;
            }
            nMtx++;
        } else if (endsWith(f, ".png")) {
            vfs.read(f, buf);
            Image im;
            std::string ie;
            if (!decodeImageMemory(buf.data(), buf.size(), false, im, &ie)) {
                fail(f, "png: " + ie);
                continue;
            }
            nPng++;
        }
    }
    for (auto& f : vfs.list("segments/"))
        if (endsWith(f, ".xml")) nSeg++;
    // every level listed in game.xml has a levels/<name>.xml
    auto lv = parseXml(vfs.readText("game.xml"));
    if (!lv) fail("game.xml", "parse");
    else if (auto* g = lv->child("game"))
        if (auto* l = g->child("levels"))
            for (auto& c : l->children)
                if (!vfs.exists("levels/" + c->attrOr("name", "?") + ".xml"))
                    fail("game.xml", "missing level " + c->attrOr("name", "?"));

    printf("xml=%d lua=%d mesh=%d glsl=%d mtx=%d segments=%d png=%d ogg=%d failures=%d\n", nXml, nLua, nMesh, nShader,
           nMtx, nSeg, nPng, nOgg, failures);
    return failures ? 1 : 0;
}
