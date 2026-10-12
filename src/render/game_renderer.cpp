#include "render/game_renderer.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <miniz.h>

namespace sh {

namespace {

struct DynVert {
    float p[3], n[3], uv[2];
    uint8_t c[4];
};
static_assert(sizeof(DynVert) == 36, "DynVert layout");
struct SpriteVert {
    float p[3], uv[2];
    uint8_t c[4];
    float a;
};
static_assert(sizeof(SpriteVert) == 28, "SpriteVert layout");
struct ShadowVert {
    float p[3], uv[2], a;
};
static_assert(sizeof(ShadowVert) == 24, "ShadowVert layout");
struct BallVert {
    float p[3];
    uint8_t t[3], pad;
    float c[4];
};
static_assert(sizeof(BallVert) == 32, "BallVert layout");

uint8_t b8(float v) { return (uint8_t)(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); }

uint32_t packColor(const float c[4]) {
    return b8(c[0]) | (b8(c[1]) << 8) | (b8(c[2]) << 16) | ((uint32_t)b8(c[3]) << 24);
}

bool inflateAll(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
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

// .mesh stream: u32 vertexCount, 24-byte vertices, u32 triCount, triCount x 3 u32 indices. Shared by the room batches
// and the menu corridor. The counts are checked against the buffer and triangles with an out-of-range index are
// dropped, so a corrupt file gives an empty or partial mesh instead of reads past the buffer or bad GPU vertex fetches.
bool parseMesh24(const std::vector<uint8_t>& un, uint32_t& n, std::vector<uint32_t>& idx) {
    n = 0;
    idx.clear();
    if (un.size() < 8) return false;
    uint32_t nv;
    memcpy(&nv, un.data(), 4);
    uint64_t off = 4 + 24ull * nv;
    if (off + 4 > un.size()) return false;
    uint32_t t;
    memcpy(&t, &un[off], 4);
    if (off + 4 + 12ull * t > un.size()) return false;
    idx.reserve(3ull * t);
    for (uint32_t k = 0; k < t; k++) {
        uint32_t tri[3];
        memcpy(tri, &un[off + 4 + 12ull * k], 12);
        if (tri[0] < nv && tri[1] < nv && tri[2] < nv) idx.insert(idx.end(), tri, tri + 3);
    }
    n = nv;
    return true;
}

void append(std::vector<uint8_t>& v, const void* p, size_t n) {
    size_t o = v.size();
    v.resize(o + n);
    memcpy(&v[o], p, n);
}

const float kWhite[4] = {1, 1, 1, 1};

// unit vector across a ribbon (rope, laser beam) running along d
Vec3 ribbonSide(const Vec3& d) {
    Vec3 side = std::fabs(d.z) <= 0.7f ? Vec3(-d.y, d.x, 0) : Vec3(0, -d.z, d.y);
    float sl = length(side);
    return sl > 0 ? side / sl : Vec3(1, 0, 0);
}

// powerups bob up and down, each with its own phase
Vec3 powerupPos(const Room& r, const PowerupEnt& p, float time) {
    return p.pos + Vec3(0, 0.1f * std::sin(time * 3.0f + (float)(&p - &r.powerups[0])), 0);
}

} // namespace

GameRenderer::GameRenderer() = default;
GameRenderer::~GameRenderer() = default;

Shader* GameRenderer::shader(const std::string& name) {
    auto it = shaders_.find(name);
    if (it != shaders_.end()) return it->second.get();
    auto s = std::make_unique<Shader>();
    std::string err;
    bool ok = false;
    std::string src = vfs_->readText("shaders/" + name + ".glsl", &ok);
    if (!ok || !s->build(src, name, &err)) {
        fprintf(stderr, "shader %s: %s\n", name.c_str(), ok ? err.c_str() : "missing");
        s.reset();
    }
    Shader* raw = s.get();
    shaders_[name] = std::move(s);
    return raw;
}

Texture& GameRenderer::tex(const std::string& name) {
    auto it = textures_.find(name);
    if (it != textures_.end()) return it->second;
    bool repeat = name.find("normal") != std::string::npos || name == "gfx/credits.png";
    // localized variants in <dir>/localize_<lang>/<file> win (e.g. the best-distance sign). Not for credits.png: the
    // shipped localized credits atlases are an older 2x8 layout the current credits scripts no longer address.
    std::string path = name;
    if (locale_ != "en" && name != "gfx/credits.png" &&
        (name.find(".png") != std::string::npos || name.find(".jpg") != std::string::npos)) {
        size_t sl = name.rfind('/');
        std::string loc = (sl == std::string::npos ? "" : name.substr(0, sl + 1)) + "localize_" + locale_ + "/" +
                          (sl == std::string::npos ? name : name.substr(sl + 1));
        if (vfs_->exists(loc) || vfs_->exists(loc + ".mtx")) path = loc;
    }
    std::string err;
    Texture t = loadTexture(*vfs_, path, repeat, &err);
    if (!t.valid()) fprintf(stderr, "texture %s: %s\n", name.c_str(), err.c_str());
    return textures_[name] = t;
}

bool GameRenderer::init(const Vfs& vfs, std::string* err) {
    vfs_ = &vfs;
    for (const char* n : {"room", "clear", "bodytex", "glass", "metal", "sprite", "softshadow", "basic"})
        if (!shader(n)) {
            if (err) *err = std::string("failed to build shader ") + n;
            return false;
        }
    tex("gfx/tiles.png");
    tex("gfx/sprites.png");
    tex("gfx/glassnormal.jpg");
    tex("gfx/metalnormal.jpg");
    tex("gfx/doors.png");
    blob_ = makeBlobTexture();
    dynPlain_.init(VLayout::Dyn36);
    dynGlass_.init(VLayout::Dyn36);
    dynGlassTex_.init(VLayout::Dyn36);
    dynMetal_.init(VLayout::Dyn36);
    dynDebris_.init(VLayout::Dyn36);
    dynSprites_.init(VLayout::Sprite28);
    dynShadow_.init(VLayout::Shadow24);
    dynBall_.init(VLayout::Ball32);
    dynEdges_.init(VLayout::Dyn36);
    dynWater_.init(VLayout::Dyn36);
    reflMaskMesh_.init(VLayout::Pos3);
    reflGeomMesh_.init(VLayout::Pos3Col16);
    quad_.init(VLayout::Pos2Tex2);
    GLfloat lw[2] = {1, 1};
    gl::GetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lw);
    lineWidth2_ = std::min(2.0f, std::max(1.0f, lw[1]));
    static const float qv[] = {0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1};
    quad_.upload(qv, sizeof qv, nullptr, 0);
    return true;
}

void GameRenderer::shutdown() {
    shaders_.clear();
    for (auto& kv : textures_) destroyTexture(kv.second);
    textures_.clear();
    destroyTexture(blob_);
    batches_.clear();
    refr_[0].destroy();
    refr_[1].destroy();
    scene_.destroy();
    dofA_.destroy();
    dofB_.destroy();
    for (DynMesh* m :
         {&dynPlain_, &dynGlass_, &dynGlassTex_, &dynMetal_, &dynDebris_, &dynSprites_, &dynShadow_, &dynBall_,
          &dynEdges_, &dynWater_, &splitQuad_[0], &splitQuad_[1], &quad_, &reflMaskMesh_, &reflGeomMesh_})
        m->destroy();
}

void GameRenderer::releaseBatch(int id) {
    if (id >= 0 && id < (int)batches_.size() && batches_[id]) batches_[id]->mesh.destroy();
}

void GameRenderer::loadBatch(Level& lv, MeshBatch& b) {
    std::vector<uint8_t> raw, un;
    std::vector<uint32_t> idx;
    uint32_t n;
    b.gpuId = -2; // mark attempted
    if (!vfs_->read(b.path, raw) || !inflateAll(raw.data(), raw.size(), un) || !parseMesh24(un, n, idx)) return;
    // vertices: add z offset; expert tint on G/B
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* v = &un[4 + 24 * i];
        float z;
        memcpy(&z, v + 8, 4);
        z += b.zOffset;
        memcpy(v + 8, &z, 4);
        if (lv.player.mode == kModeExpert) {
            v[21] = (uint8_t)(v[21] * 0.7f);
            v[22] = (uint8_t)(v[22] * 0.6f);
        }
    }
    auto g = std::make_unique<GpuBatch>();
    g->mesh.init(VLayout::Mesh24);
    g->mesh.upload(&un[4], 24ull * n, idx.data(), idx.size()); // validated indices only
    batches_.push_back(std::move(g));
    b.gpuId = (int)batches_.size() - 1;
}

void GameRenderer::computeFog(Level& lv) {
    Room* cur = lv.cur.get();
    float a[4] = {cur->fogA.x, cur->fogA.y, cur->fogA.z, 1}, bq[4] = {cur->fogB.x, cur->fogB.y, cur->fogB.z, 1};
    memcpy(lowerFog_, a, 16);
    memcpy(upperFog_, bq, 16);
    if (Room* nx = lv.next.get()) {
        float z = lv.cam.z - nx->body->pos.z;
        if (z < 10.0f) {
            float t = clampf(z / 10.0f, 0, 1);
            Vec3 la = nx->fogA, ub = nx->fogB;
            float n1[3] = {la.x, la.y, la.z}, n2[3] = {ub.x, ub.y, ub.z};
            for (int i = 0; i < 3; i++) {
                lowerFog_[i] = a[i] * t + n1[i] * (1 - t);
                upperFog_[i] = bq[i] * t + n2[i] * (1 - t);
            }
        }
    }
}

void GameRenderer::useFog(Shader& s) {
    if (s.uniform("uLowerFog") >= 0)
        gl::Uniform4f(s.uniform("uLowerFog"), lowerFog_[0], lowerFog_[1], lowerFog_[2], lowerFog_[3]);
    if (s.uniform("uUpperFog") >= 0)
        gl::Uniform4f(s.uniform("uUpperFog"), upperFog_[0], upperFog_[1], upperFog_[2], upperFog_[3]);
}

void GameRenderer::drawBackground(Shader& clear) {
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_CULL_FACE);
    gl::Disable(GL_BLEND);
    clear.use();
    useFog(clear);
    quad_.draw();
}

void GameRenderer::drawRoom(Level& lv, Room& room, Shader& s) {
    float camLocalZ = lv.cam.z - room.body->pos.z;
    Mat4 M = Mat4::translation(room.body->pos);
    Mat4 mvp = proj_ * view_ * M;
    gl::UniformMatrix4fv(s.uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    for (MeshBatch& b : room.batches) {
        if (b.gpuId < 0) continue; // not loaded yet, or the load failed
        if (!(b.zMin < camLocalZ && camLocalZ < b.zMax + 25.0f)) continue;
        batches_[b.gpuId]->mesh.draw();
    }
}

void GameRenderer::addSprite(std::vector<SpriteQuad>& out, const Vec3& pos, const Quat& rot, float hw, float hh,
                             const float rgba[4], int tile, SpriteMode mode, float blend) {
    float f = std::fabs(cameraZ - pos.z - 0.5f) * 0.8f;
    if (f == 0) return;
    float col[4] = {rgba[0], rgba[1], rgba[2], rgba[3] * std::min(f, 1.0f)};
    if (tile < 0 && playerMode == kModeExpert) { // expert tint, doors only
        col[1] *= 0.7f;
        col[2] *= 0.6f;
    }
    SpriteQuad q;
    Vec3 local[4] = {{-hw, -hh, 0}, {hw, -hh, 0}, {hw, hh, 0}, {-hw, hh, 0}};
    for (int i = 0; i < 4; i++) q.c[i] = pos + rot.rotate(local[i]);
    bool doors = tile < 0;
    int t = doors ? ~tile : tile;
    int cols = doors ? 2 : 8;
    float u0 = (float)(t % cols) / cols, v0 = (float)(t / cols) / cols, u1 = u0 + 1.0f / cols, v1 = v0 + 1.0f / cols;
    float in = doors ? 0.0f : 1.0f / 512.0f;
    q.uv[0][0] = u0 + in;
    q.uv[0][1] = v1 - in;
    q.uv[1][0] = u1 - in;
    q.uv[1][1] = v1 - in;
    q.uv[2][0] = u1 - in;
    q.uv[2][1] = v0 + in;
    q.uv[3][0] = u0 + in;
    q.uv[3][1] = v0 + in;
    q.color = packColor(col);
    q.blend = blend;
    q.mode = mode;
    q.doors = doors;
    q.sortZ = pos.z;
    out.push_back(q);
}

void GameRenderer::addUvSprite(std::vector<SpriteQuad>& out, const Vec3& pos, const Quat& rot, float hw, float hh,
                               const float rgba[4], const float uv[4], float blend) {
    float f = std::fabs(cameraZ - pos.z - 0.5f) * 0.8f;
    if (f == 0) return;
    float col[4] = {rgba[0], rgba[1], rgba[2], rgba[3] * std::min(f, 1.0f)};
    // particle tints: expert rgb*(1.1, 0.8, 0.6), training rgb*(1.1, 1.1, 0.8), clamped by b8
    if (playerMode == kModeExpert) {
        col[0] *= 1.1f;
        col[1] *= 0.8f;
        col[2] *= 0.6f;
    } else if (playerMode == kModeTraining) {
        col[0] *= 1.1f;
        col[1] *= 1.1f;
        col[2] *= 0.8f;
    }
    SpriteQuad q;
    Vec3 local[4] = {{-hw, -hh, 0}, {hw, -hh, 0}, {hw, hh, 0}, {-hw, hh, 0}};
    for (int i = 0; i < 4; i++) q.c[i] = pos + rot.rotate(local[i]);
    q.uv[0][0] = uv[0];
    q.uv[0][1] = uv[3];
    q.uv[1][0] = uv[2];
    q.uv[1][1] = uv[3];
    q.uv[2][0] = uv[2];
    q.uv[2][1] = uv[1];
    q.uv[3][0] = uv[0];
    q.uv[3][1] = uv[1];
    q.color = packColor(col);
    q.blend = blend;
    q.mode = kSpriteDirect;
    q.doors = false;
    q.sortZ = pos.z;
    out.push_back(q);
}

static Quat nlerp(const Quat& a, const Quat& b, float t) {
    Quat bb = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < 0 ? Quat(-b.x, -b.y, -b.z, -b.w) : b;
    return Quat(a.x + (bb.x - a.x) * t, a.y + (bb.y - a.y) * t, a.z + (bb.z - a.z) * t, a.w + (bb.w - a.w) * t)
        .normalized();
}

// a body's pose as drawn: dynamic bodies are interpolated between the last two ticks
void GameRenderer::drawnPose(const Body& b, Vec3& pos, Quat& rot) const {
    pos = b.dynamic ? lerp(b.prevPos, b.pos, interp) : b.pos;
    rot = b.dynamic ? nlerp(b.prevRot, b.rot, interp) : b.rot;
}

void GameRenderer::buildShapes(Level& lv, std::vector<uint8_t>& plain, std::vector<uint8_t>& glass,
                               std::vector<uint8_t>& glassTex, std::vector<uint8_t>& metal,
                               std::vector<uint8_t>* edges) {
    Vec3 cam = camPos;
    std::vector<std::pair<uint64_t, int>> halfEdges; // (min,max vertex) key -> face
    for (auto& bp : lv.world.bodies()) {
        Body* b = bp.get();
        if (!b->active || b->isBall) continue;
        Vec3 bpos;
        Quat bq;
        drawnPose(*b, bpos, bq);
        Mat3 R = Mat3::fromQuat(bq);
        for (auto& sp : b->shapes) {
            Shape* s = sp.get();
            const Material& m = s->mat;
            if (m.type == kMaterialRoom || m.color[3] <= 0) continue;
            // cull shapes far behind/ahead of the camera
            if (s->aabbMin.z > cam.z + 1.0f || s->aabbMax.z < cam.z - 110.0f) continue;
            float col[4] = {m.color[0], m.color[1], m.color[2], 1.0f};
            if (m.type == kMaterialGlass) {
                float a = 0;
                Obstacle* ow = b->owner;
                if (ow && ow->clearedAt != 0 && ow->scoreValue > 0) {
                    float t = clampf((lv.time - ow->clearedAt) * 3.0f, 0, 1);
                    for (int i = 0; i < 3; i++) col[i] = t + (1 - t) * col[i];
                    a = 1 - t;
                }
                col[3] = a;
            }
            uint8_t cb[4] = {b8(col[0]), b8(col[1]), b8(col[2]), b8(col[3])};
            std::vector<uint8_t>& out = m.type == kMaterialGlass   ? (m.texEnabled ? glassTex : glass)
                                        : m.type == kMaterialMetal ? metal
                                                                   : plain;
            const Polyhedron& P = s->poly;
            // rounded shapes: corner normal = mix(face normal, vertex normal, roundness). Unbreakable cylinders keep
            // flat caps and radial sides, and the cap rings of a chamfer band take the axis.
            const float rr = clampf(m.roundness, 0, 1);
            const bool smooth = rr > 0 && P.vn.size() == P.v.size();
            auto cornerN = [&](const PolyFace& f, int vi) -> Vec3 {
                Vec3 vnorm = P.vn[(size_t)vi];
                if (s->cyl) {
                    Vec3 d = P.v[(size_t)vi] - s->cylCentre;
                    float ax = dot(d, s->cylAxis);
                    if (std::fabs(dot(f.n, s->cylAxis)) > 0.999f) vnorm = f.n;
                    else if (s->chamfer > 0 && std::fabs(ax) > s->cylHalfLen - s->chamfer * 0.5f)
                        vnorm = s->cylAxis * (ax > 0 ? 1.0f : -1.0f);
                    else {
                        Vec3 rd = d - s->cylAxis * ax;
                        float l = length(rd);
                        vnorm = l > 1e-6f ? rd / l : f.n;
                    }
                }
                return normalize(R * (vnorm * rr + f.n * (1 - rr)));
            };
            for (const PolyFace& f : P.f) {
                Vec3 nw = R * f.n;
                Vec3 w0 = bpos + R * P.v[f.idx[0]];
                if (dot(nw, w0 - cam) > 0) continue;
                // box-mapped UVs from the dominant local axis
                float ax = std::fabs(f.n.x), ay = std::fabs(f.n.y), az = std::fabs(f.n.z);
                int axis = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
                auto vert = [&](int vi) {
                    DynVert dv;
                    const Vec3& lp = P.v[vi];
                    Vec3 wp = bpos + R * lp;
                    dv.p[0] = wp.x;
                    dv.p[1] = wp.y;
                    dv.p[2] = wp.z;
                    const Vec3 cn = smooth ? cornerN(f, vi) : nw;
                    dv.n[0] = cn.x;
                    dv.n[1] = cn.y;
                    dv.n[2] = cn.z;
                    float u = axis == 0 ? lp.y : lp.x, v = axis == 2 ? lp.y : lp.z;
                    if (m.texEnabled) {
                        u = u * m.texScale[0] + m.texOffset[0];
                        v = v * m.texScale[1] + m.texOffset[1];
                    }
                    dv.uv[0] = u;
                    dv.uv[1] = v;
                    memcpy(dv.c, cb, 4);
                    return dv;
                };
                DynVert v0 = vert(f.idx[0]);
                for (size_t k = 1; k + 1 < f.idx.size(); k++) {
                    DynVert v1 = vert(f.idx[k]), v2 = vert(f.idx[k + 1]);
                    append(out, &v0, 36);
                    append(out, &v1, 36);
                    append(out, &v2, 36);
                }
            }
            // glass edge lines: creases (dot(nA,nB) <= 0.98) between two faces, pushed 0.01 toward the camera
            if (edges && m.type == kMaterialGlass && !m.texEnabled && !m.hideEdges) {
                halfEdges.clear();
                for (int fi = 0; fi < (int)P.f.size(); fi++) {
                    const auto& ix = P.f[fi].idx;
                    for (size_t k = 0; k < ix.size(); k++) {
                        uint32_t a = (uint32_t)ix[k], c = (uint32_t)ix[(k + 1) % ix.size()];
                        halfEdges.push_back({((uint64_t)std::min(a, c) << 32) | std::max(a, c), fi});
                    }
                }
                std::sort(halfEdges.begin(), halfEdges.end());
                for (size_t k = 0; k + 1 < halfEdges.size(); k++) {
                    if (halfEdges[k].first != halfEdges[k + 1].first) continue;
                    const PolyFace& fa = P.f[halfEdges[k].second];
                    const PolyFace& fb = P.f[halfEdges[k + 1].second];
                    k++;
                    if (dot(fa.n, fb.n) > 0.98f) continue;
                    Vec3 nw = normalize(R * (fa.n + fb.n));
                    for (int e = 0; e < 2; e++) {
                        int vi = e ? (int)(halfEdges[k].first & 0xffffffffu) : (int)(halfEdges[k].first >> 32);
                        const Vec3& lp = P.v[vi];
                        Vec3 wp = bpos + R * lp;
                        DynVert dv;
                        dv.p[0] = wp.x;
                        dv.p[1] = wp.y;
                        dv.p[2] = wp.z + 0.01f;
                        dv.n[0] = nw.x;
                        dv.n[1] = nw.y;
                        dv.n[2] = nw.z;
                        dv.uv[0] = lp.x;
                        dv.uv[1] = lp.y;
                        memcpy(dv.c, cb, 4);
                        append(*edges, &dv, 36);
                    }
                }
            }
        }
    }
    buildRopes(lv, metal);
}

// rope joints: flat ribbon (half-width 0.01) between the two anchors, grey at A, white at B, in the metal buffer
void GameRenderer::buildRopes(Level& lv, std::vector<uint8_t>& metal) {
    const Vec3 cam = camPos;
    auto anchor = [&](Body* body, const Vec3& local) {
        if (!body) return local;
        Vec3 p;
        Quat q;
        drawnPose(*body, p, q);
        return p + q.rotate(local);
    };
    for (auto& jp : lv.world.joints()) {
        const Joint& j = *jp;
        if (j.type != JointType::Rope) continue;
        Vec3 A = anchor(j.a, j.anchorA), B = anchor(j.b, j.anchorB);
        if (std::max(A.z, B.z) < cam.z - 110.0f || std::min(A.z, B.z) > cam.z + 1.0f) continue;
        Vec3 d = B - A;
        float len = length(d);
        if (len <= 0) continue;
        d = d / len;
        Vec3 side = ribbonSide(d);
        if (side.y < 0) side = -side;
        Vec3 off = side * 0.01f;
        Vec3 n = normalize(Vec3(off.x * 10.0f, off.y * 10.0f, 1.0f));
        const uint8_t grey[4] = {128, 128, 128, 255}, white[4] = {255, 255, 255, 255};
        DynVert q[4];
        const Vec3 p[4] = {A - off, B - off, B + off, A + off};
        const float u[4] = {0, 1, 1, 0};
        for (int i = 0; i < 4; i++) {
            q[i].p[0] = p[i].x;
            q[i].p[1] = p[i].y;
            q[i].p[2] = p[i].z;
            q[i].n[0] = n.x;
            q[i].n[1] = n.y;
            q[i].n[2] = n.z;
            q[i].uv[0] = u[i];
            q[i].uv[1] = 0.5f;
            memcpy(q[i].c, i < 2 ? grey : white, 4);
        }
        // wind toward the camera (metal is drawn with back-face culling)
        bool flip = dot(cross(p[1] - p[0], p[2] - p[0]), cam - p[0]) < 0;
        static const int kFwd[6] = {0, 1, 2, 0, 2, 3}, kFlip[6] = {0, 2, 1, 0, 3, 2};
        const int* idx = flip ? kFlip : kFwd;
        for (int k = 0; k < 6; k++) append(metal, &q[idx[k]], 36);
    }
}

void GameRenderer::buildSprites(Level& lv) {
    sprites_.clear();
    ballVerts_.clear();
    ballIdx_.clear();
    cameraZ = lv.cam.z;
    playerMode = lv.player.mode;
    addBallSprites(lv);
    addRoomSprites(lv);
    addButtonSprites(lv);
    addHintSprites(lv);
    addBodyDecalSprites(lv);
    addScoreSprites(lv);
    addLaserSprites(lv);
    for (auto& s : extraSprites) sprites_.push_back(s); // cleared by drawLevel once every view has used them
}

// balls: the tint only colours the chrome disc, both ball sprites stay white
void GameRenderer::addBallSprites(Level& lv) {
    const bool low = quality_ == "low";
    for (auto& bp : lv.world.bodies()) {
        Body* b = bp.get();
        if (!b->isBall || !b->active) continue;
        float c[4] = {1, 1, 1, 1};
        if (b->ballState != kBallNormal) {
            c[0] = 2;
            c[1] = 1;
            c[2] = 1;
        } else if (lv.powerupTimer[kPowerupFrenzy] > 0) {
            c[0] = 1;
            c[1] = 1.4f;
            c[2] = 1;
        } else if ((lv.player.mode == kModeVersus || lv.player.mode == kModeCoop) && b->id == 1) {
            c[0] = 1.4f;
            c[1] = 1.2f;
            c[2] = 0.8f;
        }
        Vec3 com = b->worldCom();
        com = com + (lerp(b->prevPos, b->pos, interp) - b->pos);
        if (low) addSprite(sprites_, com, Quat(), 0.11f, 0.11f, kWhite, 0, kSpriteDirectAndMirror, 1.0f);
        else { // disc + mirrored-only sprite
            addReflection(com, 0.11f, c);
            addSprite(sprites_, com, Quat(), 0.11f, 0.11f, kWhite, 0, kSpriteMirrorOnly, 1.0f);
        }
    }
}

// powerups and decals of the current and the next room
void GameRenderer::addRoomSprites(Level& lv) {
    static const int tiles[] = {0, 5, 7, 4, 2, 1, 3};
    for (Room* r : {lv.cur.get(), lv.next.get()}) {
        if (!r) continue;
        for (auto& p : r->powerups) {
            if (p.taken) continue;
            Vec3 pos = powerupPos(*r, p, lv.time);
            int tile = p.type >= 0 && p.type < 7 ? tiles[p.type] : 0;
            addSprite(sprites_, pos, Quat(), 0.3f, 0.3f, kWhite, tile, kSpriteDirectAndMirror, 1.0f);
            // every quality (low samples the never-drawn refraction FBO)
            addReflection(pos + Vec3(0, 0, -0.05f), 0.28f, kWhite);
        }
        for (auto& d : r->decals) {
            Vec3 wp = r->body->pointToWorld(d.pos);
            if (wp.z > lv.cam.z + 2 || wp.z < lv.cam.z - 60) continue;
            addSprite(sprites_, wp, (r->body->rot * d.rot).normalized(), d.w, d.h, d.color, d.tile, kSpriteDirect,
                      d.blend);
        }
    }
}

// door/laser buttons, unrotated since mgCreateButton only sets a position. The hint arrows are keyed to the
// current room's name, not the button's room.
void GameRenderer::addButtonSprites(Level& lv) {
    const float lt = lv.time;
    auto drawButton = [&](const Button& b) {
        Vec3 p = b.pos + Vec3(0, 0, 0.05f);
        if (b.down) {
            addSprite(sprites_, p, Quat(), 0.3f, 0.3f, kWhite, 9, kSpriteDirectAndMirror, 1.0f);
            return;
        }
        addSprite(sprites_, p, Quat(), 0.3f, 0.3f, kWhite, 8, kSpriteDirectAndMirror, 1.0f);
        p.z += 0.01f;
        // phase from the button's address, like the original
        float phase = (float)((uint32_t)(uintptr_t)&b % 100u);
        float glow[4] = {1, 1, 1, 0.8f + 0.2f * std::sin((phase + lt) * 9.0f)};
        addSprite(sprites_, p, Quat(), 0.3f, 0.3f, glow, 14, kSpriteDirectAndMirror, 0.0f);
        if ((lv.cur->name == "basic/basic" || lv.cur->name == "basic/dark") && std::fabs(p.z - lv.cam.z) < 16.0f) {
            float s = 0.8f + 0.2f * std::sin(lt * 7.0f);
            addSprite(sprites_, p - Vec3(s, 0, 0), Quat::axisAngle(Vec3(0, 0, 1), kPi / 2), 0.5f, 0.5f, kWhite, 13,
                      kSpriteDirectAndMirror, 1.0f);
            addSprite(sprites_, p + Vec3(s, 0, 0), Quat::axisAngle(Vec3(0, 0, 1), -kPi / 2), 0.5f, 0.5f, kWhite, 13,
                      kSpriteDirectAndMirror, 1.0f);
        }
    };
    for (Room* r : {lv.cur.get(), lv.next.get()}) {
        if (!r) continue;
        for (auto& bt : r->roomObstacle->buttons) drawButton(*bt);
        for (auto& op : r->obstacles)
            for (auto& bt : op->buttons) drawButton(*bt);
    }
}

// cave/narrow hint: an arrow at the first body (the glass emitter) of every unbroken 'laser' obstacle in the
// current room, while the distance is below 3850
void GameRenderer::addHintSprites(Level& lv) {
    if (lv.cur->name == "cave/narrow" && lv.distance < 3850.0f)
        for (auto& d : lv.cur->defs) {
            if (d.type != "laser" || !d.obj || d.obj->bodies.empty()) continue;
            const Body* b = d.obj->bodies[0];
            if (!b || b->isBall || b->brokenFlag) continue;
            Vec3 bp;
            Quat bq;
            drawnPose(*b, bp, bq);
            addSprite(sprites_, bp + Vec3(0.8f + 0.2f * std::sin(lv.time * 5.0f), 0, 0.5f),
                      Quat::axisAngle(Vec3(0, 0, 1), -kPi / 2), 0.5f, 0.5f, kWhite, 13, kSpriteDirectAndMirror, 1.0f);
        }
}

// the current room's obstacle scripts run draw() every frame and re-emit their mgBodyDecal sprites (boss glow
// frames and lights), drawn white and additive
void GameRenderer::addBodyDecalSprites(Level& lv) {
    lv.bodyDecals.clear();
    for (auto& d : lv.cur->defs)
        if (d.obj && d.obj->script) d.obj->script->draw();
    for (auto& bd : lv.bodyDecals) {
        Vec3 bp;
        Quat bq;
        drawnPose(*bd.body, bp, bq);
        addSprite(sprites_, bp + bq.rotate(bd.off), (bq * bd.rot).normalized(), bd.w, bd.h, kWhite, bd.sprite,
                  kSpriteDirect, 0.0f);
    }
    lv.bodyDecals.clear(); // the body pointers are only valid until the next tick
}

// obstacle score pointers and "+N" popups
void GameRenderer::addScoreSprites(Level& lv) {
    if (lv.player.mode == kModeZen) return;
    for (Room* r : {lv.cur.get(), lv.next.get()}) {
        if (!r) continue;
        for (auto& op : r->obstacles) {
            Obstacle& o = *op;
            if (o.scoreValue == 0 || o.done) continue;
            Vec3 target = o.pos + o.rot.rotate(o.scorePos);
            if (o.clearedAt == 0) {
                // pointer at the mgGlow point (not mgScore's), pulled 0.5 toward the camera. The spin uses the
                // distance before the pull, and a glow point exactly at the camera moves 0.5 in -x.
                Vec3 g = o.pos + o.rot.rotate(o.glowPos);
                float dist = length(g - lv.cam);
                g = dist > 0 ? g - (g - lv.cam) * (0.5f / dist) : g - Vec3(0.5f, 0, 0);
                if (g.z > lv.cam.z + 1 || g.z < lv.cam.z - 60) continue;
                float c[4] = {1, 1, 1, 0.6f};
                addSprite(sprites_, g, Quat::axisAngle(Vec3(0, 0, 1), dist * 0.1f), 0.8f, 0.8f, c, 21, kSpriteDirect,
                          0.2f);
            } else {
                float t = lv.time - o.clearedAt;
                float sc = std::pow(clampf(5.0f * t, 0, 1), 0.2f), alpha = clampf(2.5f - 2.0f * t, 0, 1);
                if (alpha <= 0 || sc <= 0) continue;
                Vec3 pos = target + Vec3(0, 0.1f * t, 0);
                static const int digitTile[10] = {44, 45, 46, 47, 52, 53, 54, 55, 60, 61};
                std::string txt = std::to_string(o.scoreValue);
                float w = 0;
                for (char ch : txt) w += (ch == '1' ? 0.5f : 1.0f) * 0.16f * sc;
                float bgc[4] = {0.3f, 0.7f, 1.0f, alpha};
                addSprite(sprites_, pos, Quat(), std::max(0.05f * sc + w / 2, 0.25f * sc), 0.25f * sc, bgc, 62,
                          kSpriteDirectAndMirror, 1.0f);
                float x = pos.x - w / 2;
                float wc[4] = {1, 1, 1, alpha};
                for (char ch : txt) {
                    float adv = (ch == '1' ? 0.5f : 1.0f) * 0.16f * sc;
                    addSprite(sprites_, Vec3(x + adv / 2, pos.y, pos.z + 0.01f), Quat(), 0.16f * sc, 0.16f * sc, wc,
                              digitTile[ch - '0'], kSpriteDirectAndMirror, 1.0f);
                    x += adv;
                }
            }
        }
    }
}

// lasers: beam sprite (tile 16) plus end caps (tiles 24/25), alpha flickering between 0.8 and 1
void GameRenderer::addLaserSprites(Level& lv) {
    auto flicker = [&]() {
        laserRng_ = laserRng_ * 1664525u + 1013904223u;
        return 0.8f + 0.2f * (float)(laserRng_ >> 8) / 16777216.0f;
    };
    for (auto& l : lv.lasers) {
        Vec3 beam = l.end - l.origin;
        float len = length(beam) + FLT_EPSILON;
        Vec3 d = beam / len;
        Vec3 side = ribbonSide(d);
        Vec3 up = normalize(cross(d, side));
        Quat q = Quat::fromBasis(d, side, up);
        Vec3 p0 = l.origin, p1 = l.end;
        float c[4] = {1, 1, 1, flicker()};
        addSprite(sprites_, (p0 + p1) * 0.5f, q, len * 0.5f - 0.3f, 0.15f, c, 16, kSpriteDirectAndMirror, 0.25f);
        c[3] = flicker();
        addSprite(sprites_, p0 + d * 0.1f, q, 0.2f, 0.15f, c, 24, kSpriteDirectAndMirror, 0.25f);
        c[3] = flicker();
        addSprite(sprites_, p1 - d * 0.1f, q, 0.2f, 0.15f, c, 25, kSpriteDirectAndMirror, 0.25f);
    }
}

// chrome disc: 10-gon fan, centre texcoord (0x80, 0x80, 0xff), rim (d*127+128, 0), alpha = distance fade
void GameRenderer::addReflection(const Vec3& pos, float radius, const float rgb[3]) {
    float fade = clampf((cameraZ - pos.z - 0.5f) * 0.8f, 0, 1);
    if (fade <= 0) return;
    uint32_t base = (uint32_t)(ballVerts_.size() / sizeof(BallVert));
    BallVert v{};
    v.p[0] = pos.x;
    v.p[1] = pos.y;
    v.p[2] = pos.z;
    v.t[0] = 0x80;
    v.t[1] = 0x80;
    v.t[2] = 0xff;
    v.c[0] = rgb[0];
    v.c[1] = rgb[1];
    v.c[2] = rgb[2];
    v.c[3] = fade;
    append(ballVerts_, &v, sizeof v);
    for (int i = 0; i < 10; i++) {
        float a = (float)i * 2.0f * kPi / 10.0f;
        float dx = clampf(std::cos(a) * 1.05f, -1, 1), dy = clampf(std::sin(a) * 1.05f, -1, 1);
        v.p[0] = pos.x + dx * radius;
        v.p[1] = pos.y + dy * radius;
        v.t[0] = (uint8_t)clampf(dx * 127 + 128, 0, 255);
        v.t[1] = (uint8_t)clampf(dy * 127 + 128, 0, 255);
        v.t[2] = 0;
        append(ballVerts_, &v, sizeof v);
    }
    for (uint32_t i = 0; i < 10; i++) {
        ballIdx_.push_back(base);
        ballIdx_.push_back(base + 1 + i);
        ballIdx_.push_back(base + 1 + (i + 1) % 10);
    }
}

void GameRenderer::sortSprites() {
    std::stable_sort(sprites_.begin(), sprites_.end(),
                     [](const SpriteQuad& a, const SpriteQuad& b) { return a.sortZ < b.sortZ; });
}

void GameRenderer::buildShadows(Level& lv, std::vector<uint8_t>& out) {
    auto addShadow = [&](const Vec3& pos, float intensity) {
        RayHit h = lv.world.raycast(pos, pos + Vec3(0, -1, 0), kCategoryRoom, nullptr);
        if (!h.hit) return;
        Vec3 hp = h.point;
        float height = pos.y - hp.y;
        float alpha = clampf((0.4f - 2.0f * (height - 0.5f)) * intensity, 0, 1);
        if (alpha <= 0) return;
        float cs[4][2] = {{-0.6f, -0.6f}, {0.6f, -0.6f}, {0.6f, 0.6f}, {-0.6f, 0.6f}};
        float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int i : {0, 1, 2, 0, 2, 3}) {
            ShadowVert v{{hp.x + cs[i][0], hp.y + 0.01f, hp.z + cs[i][1]}, {uv[i][0], uv[i][1]}, alpha};
            append(out, &v, sizeof v);
        }
    };
    // body shadows at every quality: any body whose lowest point is below y = 5 casts a ray from its centre of mass
    // down to the floor. The quad sits just above the hit, turned to the body axis with the largest horizontal
    // extent, half size = that extent + 0.25. Static bodies have no boxDims and get a 0.25 square.
    for (auto& bp : lv.world.bodies()) {
        Body* b = bp.get();
        if (!b->active || b->shapes.empty()) continue;
        float minY = FLT_MAX;
        for (auto& s : b->shapes) minY = std::min(minY, s->aabbMin.y);
        if (!(minY < 5.0f)) continue;
        Vec3 dp;
        Quat q;
        drawnPose(*b, dp, q);
        Vec3 c = b->worldCom();
        if (b->dynamic) c = c + (dp - b->pos);
        RayHit h = lv.world.raycast(c, Vec3(c.x, minY - 0.5f, c.z), kCategoryRoom, nullptr);
        if (!h.hit || h.t <= 0) continue; // t = 0: the centre is inside a floor shape (the original skips those too)
        Vec3 ax[3] = {q.rotate(Vec3(b->boxDims.x, 0, 0)), q.rotate(Vec3(0, b->boxDims.y, 0)),
                      q.rotate(Vec3(0, 0, b->boxDims.z))};
        int best = 0;
        for (int i = 1; i < 3; i++)
            if (ax[i].x * ax[i].x + ax[i].z * ax[i].z > ax[best].x * ax[best].x + ax[best].z * ax[best].z) best = i;
        Vec3 dir(ax[best].x, 0, ax[best].z);
        float dl = length(dir);
        dir = dl > 0 ? dir / dl : Vec3(1, 0, 0);
        Vec3 side(-dir.z, 0, dir.x); // cross(dir, Y)
        float e1 = 0, e2 = 0;
        for (const Vec3& a : ax) {
            e1 = std::max(e1, std::fabs(dot(a, dir)));
            e2 = std::max(e2, std::fabs(dot(a, side)));
        }
        e1 += 0.25f;
        e2 += 0.25f;
        float alpha =
            (float)std::min(255, std::max(0, (int)(clampf(1.0f - 2.0f * (minY - h.point.y), 0, 1) * 0.6f * 255.0f))) /
            255.0f;
        if (alpha <= 0) continue;
        Vec3 ctr(c.x, h.point.y + 0.01f, c.z);
        Vec3 cs[4] = {ctr - dir * e1 + side * e2, ctr + dir * e1 + side * e2, ctr + dir * e1 - side * e2,
                      ctr - dir * e1 - side * e2};
        float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int i : {0, 1, 2, 0, 2, 3}) {
            ShadowVert v{{cs[i].x, cs[i].y, cs[i].z}, {uv[i][0], uv[i][1]}, alpha};
            append(out, &v, sizeof v);
        }
    }
    for (Room* r : {lv.cur.get(), lv.next.get()}) {
        if (!r) continue;
        for (auto& p : r->powerups)
            if (!p.taken) addShadow(powerupPos(*r, p, lv.time), 1.0f);
    }
    // the rising "+N" popup casts a shadow as strong as its alpha (the original's ray is 10 long, but alpha is 0
    // above a height of 0.7 anyway)
    if (lv.player.mode != kModeZen)
        for (Room* r : {lv.cur.get(), lv.next.get()})
            if (r)
                for (auto& op : r->obstacles) {
                    const Obstacle& o = *op;
                    if (o.scoreValue < 1 || o.clearedAt == 0 || o.done) continue;
                    float t = lv.time - o.clearedAt, a = clampf(2.5f - 2.0f * t, 0, 1);
                    if (a > 0) addShadow(o.pos + o.rot.rotate(o.scorePos) + Vec3(0, 0.1f * t, 0), a);
                }
}

void GameRenderer::drawRefractionBuffer(Level& lv, bool split) {
    // split-screen reads and writes the same buffer, so each eye samples its own current image. A single view
    // ping-pongs and samples the previous frame.
    if (split) refrRead_ = 0;
    else refrRead_ = 1 - refrRead_;
    Fbo& dst = refr_[split ? 0 : 1 - refrRead_];
    dst.bind();
    gl::ClearColor(0, 0, 0, 0);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    drawBackground(*shader("clear"));
    gl::Enable(GL_DEPTH_TEST);
    gl::Enable(GL_CULL_FACE);
    Shader* basic = shader("basic");
    basic->use();
    useFog(*basic);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/tiles.png").id);
    gl::Uniform1i(basic->uniform("uTexture0"), 0);
    gl::Enable(GL_POLYGON_OFFSET_FILL);
    gl::PolygonOffset(1.0f, 10.0f);
    drawRoom(lv, *lv.cur, *basic);
    if (lv.next) drawRoom(lv, *lv.next, *basic);
    gl::Disable(GL_POLYGON_OFFSET_FILL);
}

// Full-screen quad (quad_ spans 0..1, or `mesh` in its place) with an identity-ortho blit shader; tex0 optionally
// sampled NEAREST.
void GameRenderer::drawFullscreen(const char* shaderName, GLuint tex0, bool nearest0, GLuint tex1,
                                  const DynMesh* mesh) {
    Shader* s = shader(shaderName);
    if (!s) return;
    s->use();
    Mat4 ortho = Mat4::ortho(0, 1, 0, 1, -1, 1);
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, ortho.m);
    if (tex1) {
        gl::ActiveTexture(GL_TEXTURE1);
        gl::BindTexture(GL_TEXTURE_2D, tex1);
        gl::Uniform1i(s->uniform("uTexture1"), 1);
    }
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex0);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    GLint f = nearest0 ? GL_NEAREST : GL_LINEAR;
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    (mesh ? *mesh : quad_).draw();
    if (nearest0) {
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
}

// Depth of field: RGB = the previous frame's scene downsampled (in split-screen this runs after the eye is drawn, so
// it is that eye's current scene), A = near-field blur amount from the current geometry. Then a separable 7-tap blur
// whose radius is that alpha. Result in dofA_.
void GameRenderer::drawDofBuffer(Level& lv) {
    dofA_.bind();
    gl::ClearColor(0, 0, 0, 0);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl::Disable(GL_BLEND);
    gl::Disable(GL_DEPTH_TEST);
    gl::Disable(GL_CULL_FACE);
    gl::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    drawFullscreen("blitfbo", scene_.tex, false);
    gl::ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    if (Shader* da = shader("dofalpha")) {
        gl::Enable(GL_DEPTH_TEST);
        gl::DepthMask(GL_TRUE);
        gl::DepthFunc(GL_LEQUAL);
        da->use();
        drawRoom(lv, *lv.cur, *da);
        Mat4 mvp = proj_ * view_;
        gl::UniformMatrix4fv(da->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
        dynGlass_.draw();
        dynGlassTex_.draw();
        dynMetal_.draw();
        gl::Disable(GL_DEPTH_TEST);
    }
    gl::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    dofB_.bind();
    gl::Clear(GL_COLOR_BUFFER_BIT);
    drawFullscreen("blurh", dofA_.tex, true);
    dofA_.bind();
    gl::Clear(GL_COLOR_BUFFER_BIT);
    drawFullscreen("blurv", dofB_.tex, true);
}

// Best-distance sign at (0, 0.5, z) for the best distance, shown while within 100 units ahead. When the player runs
// out of balls after passing the best, it drops in 3 units in front of the camera instead.
void GameRenderer::drawBestDistance(Level& lv, const Mat4& mvp) {
    if (!hudOn || !lv.realLevel || lv.player.mode == kModeZen || !lv.cur) return;
    if (lv.cur->name.find("boss") != std::string::npos) return;
    Room& r = *lv.cur;
    const RoomDef& def = lv.roomDefs[r.defIdx];
    float markerZ =
        def.length > 0 ? -(((float)bestDistance - lv.distanceBase) / def.length * r.length + r.start) : 0.0f;
    float camZ = lv.cam.z, show;
    if (lv.outOfBalls <= 0 || lv.distance <= (float)bestDistance) {
        // a best of 0 has no sign ahead; the out-of-balls drop below shows even then (first run in a mode)
        if (bestDistance <= 0 || def.length <= 0 || !(markerZ < camZ && camZ - 100.0f < markerZ)) return;
        show = 1;
    } else {
        show = clampf(lv.outOfBalls * 4.0f - 3.0f, 0, 1);
        if (show <= 0) return;
        markerZ = camZ - 3.0f;
    }
    float alpha = show * clampf(1.0f - 0.03f * std::fabs(camZ - markerZ), 0, 1);
    Shader* s = shader("2dtex");
    if (!s || alpha <= 0) return;
    Mat4 M = Mat4::translation(Vec3(0, 0.5f, markerZ)) * Mat4::scale(Vec3(2.16f, -0.8f, 0)) *
             Mat4::translation(Vec3(-0.5f, -0.5f, 0));
    Mat4 m = mvp * M;
    s->use();
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, m.m);
    gl::Uniform4f(s->uniform("uColor"), 1, 1, 1, alpha);
    gl::Uniform2f(s->uniform("uTexScale0"), 1, 1);
    gl::Uniform2f(s->uniform("uTexOffset0"), 0, 0);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/bestdistance.png").id);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::DepthMask(GL_FALSE);
    gl::Disable(GL_CULL_FACE);
    quad_.draw();
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_BLEND);
}

// Split-screen (versus/co-op) renders each player's portrait eye (H x W/2, camera x -/+1) into the scene FBO and
// composes it rotated -90 deg for player 0 on the left and +90 deg for player 1 on the right, the tabletop layout.
void GameRenderer::drawLevel(Level& lv, int w, int h) {
    if (!lv.cur) {
        extraSprites.clear();
        return;
    }
    const bool split = lv.player.mode == kModeVersus || lv.player.mode == kModeCoop;
    if (!split) {
        drawView(lv, w, h, -1);
        extraSprites.clear();
        return;
    }
    GLint outFbo = 0, vp[4];
    gl::GetIntegerv(GL_FRAMEBUFFER_BINDING, &outFbo);
    gl::GetIntegerv(GL_VIEWPORT, vp);
    if (splitQuad_[0].empty()) {
        // window-half position (0..1) -> eye texture uv: eye 0 samples (1-v, u), eye 1 samples (v, 1-u)
        const float q0[] = {0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0};
        const float q1[] = {0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1};
        splitQuad_[0].init(VLayout::Pos2Tex2);
        splitQuad_[0].upload(q0, sizeof q0, nullptr, 0);
        splitQuad_[1].init(VLayout::Pos2Tex2);
        splitQuad_[1].upload(q1, sizeof q1, nullptr, 0);
    }
    const bool high = quality_ == "high";
    for (int eye = 0; eye < 2; eye++) {
        drawView(lv, vp[3], vp[2] / 2, eye);
        // at high quality the near-field blur is built from this eye's scene and geometry and composed with dof,
        // medium/low just blit the eye
        if (high) drawDofBuffer(lv);
        gl::BindFramebuffer(GL_FRAMEBUFFER, (GLuint)outFbo);
        gl::Viewport(vp[0] + (eye ? vp[2] / 2 : 0), vp[1], vp[2] / 2, vp[3]);
        gl::Disable(GL_DEPTH_TEST);
        gl::Disable(GL_CULL_FACE);
        gl::Disable(GL_BLEND);
        // the scene FBO is sampled NEAREST, the halves map 1:1 onto its pixels
        drawFullscreen(high ? "dof" : "blitfbo", scene_.tex, true, high ? dofA_.tex : 0, &splitQuad_[eye]);
    }
    gl::Viewport(vp[0], vp[1], vp[2], vp[3]);
    gl::Enable(GL_DEPTH_TEST);
    gl::Enable(GL_CULL_FACE);
    extraSprites.clear(); // particles were added to both eyes
}

void GameRenderer::drawView(Level& lv, int w, int h, int eye) {
    // the caller's target, captured before any FBO below is (re)created and bound (Fbo::bind also sets the viewport)
    GLint outFbo = 0, vp[4];
    gl::GetIntegerv(GL_FRAMEBUFFER_BINDING, &outFbo);
    gl::GetIntegerv(GL_VIEWPORT, vp);
    const bool high = quality_ == "high", low = quality_ == "low";
    const bool useScene = high || eye >= 0;
    prepareTargets(w, h, useScene);
    setupCamera(lv, w, h, eye);
    computeFog(lv);
    preloadBatches(lv);
    buildGeometry(lv, low);

    // depth-of-field buffer from the previous frame's scene (split-screen builds it per eye after drawing)
    if (high && eye < 0) drawDofBuffer(lv);
    // refraction buffer (previous frame is what glass samples; split-screen: this eye's current one)
    if (!low) drawRefractionBuffer(lv, eye >= 0);
    if (useScene) scene_.bind();
    else {
        gl::BindFramebuffer(GL_FRAMEBUFFER, (GLuint)outFbo);
        gl::Viewport(vp[0], vp[1], vp[2], vp[3]);
    }
    // scene
    gl::ClearColor(0, 0, 0, 0);
    gl::Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    drawBackground(*shader("clear"));
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthFunc(GL_LEQUAL);
    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_CULL_FACE);
    gl::CullFace(GL_BACK);
    gl::Disable(GL_BLEND);
    gl::ActiveTexture(GL_TEXTURE0);
    const Mat4 mvp = proj_ * view_;
    drawRooms(lv, low);
    drawPlainShapes(mvp);
    drawGlass(low, mvp);
    drawMetal(low, mvp);
    uploadSprites();
    if (!low) drawFloorReflections(lv, mvp);
    drawSoftShadows(mvp);
    if (!low && !lv.waters.empty()) drawWater(lv, mvp);
    drawBallDiscs(mvp);
    drawSprites(low, mvp);
    drawBestDistance(lv, mvp);
    gl::Enable(GL_CULL_FACE);
    // final composite (high): mix(scene, nearBlur, nearBlur.a); split-screen composes in drawLevel
    if (high && eye < 0) {
        gl::BindFramebuffer(GL_FRAMEBUFFER, (GLuint)outFbo);
        gl::Viewport(vp[0], vp[1], vp[2], vp[3]);
        gl::Disable(GL_DEPTH_TEST);
        gl::Disable(GL_CULL_FACE);
        gl::Disable(GL_BLEND);
        drawFullscreen("dof", scene_.tex, true, dofA_.tex);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::Enable(GL_DEPTH_TEST);
        gl::Enable(GL_CULL_FACE);
    }
}

// (Re)creates the refraction targets on a size change and the scene/DOF targets when they are needed.
void GameRenderer::prepareTargets(int w, int h, bool useScene) {
    if (w != lastW_ || h != lastH_) {
        lastW_ = w;
        lastH_ = h;
        refr_[0].create(w / 4, h / 4, true);
        refr_[1].create(w / 4, h / 4, true);
        for (auto& f : refr_) {
            f.bind();
            gl::ClearColor(0, 0, 0, 0);
            gl::Clear(GL_COLOR_BUFFER_BIT);
        }
        scene_.destroy();
        dofA_.destroy();
        dofB_.destroy();
    }
    if (useScene && !scene_.fbo) {
        scene_.create(w, h, true);
        dofA_.create(w / 4, h / 4, true);
        dofB_.create(w / 4, h / 4, false);
        for (Fbo* f : {&scene_, &dofA_, &dofB_}) {
            f->bind();
            gl::ClearColor(0, 0, 0, 0);
            gl::Clear(GL_COLOR_BUFFER_BIT);
        }
    }
}

void GameRenderer::setupCamera(Level& lv, int w, int h, int eye) {
    float aspect = (float)w / (float)h;
    float hfov = ViewParams{(float)w, (float)h}.fovDeg() * kPi / 180.0f;
    float fovY = 2.0f * std::atan(std::tan(hfov * 0.5f) / aspect);
    proj_ = Mat4::perspective(fovY, aspect, 0.2f, 100.0f);
    Vec3 cp = lerp(lv.prevCam, lv.cam, interp);
    if (eye >= 0) cp.x += eye == 0 ? -1.0f : 1.0f;
    // camera shake (the global one plus this eye's own in versus): offset 0.2*s*(rnd(+-0.1) + 0.3*sin(40t)) in x,
    // the same with 33t in y, and a roll of -dx radians about the view axis
    Quat shakeRoll;
    const float shake = lv.cameraShake + (eye >= 0 ? lv.playerShake[eye] : 0.0f);
    if (shake > 0) {
        if (lv.tickCount != shakeTick_) { // new jitter once per game tick, so it freezes while fully paused
            shakeTick_ = lv.tickCount;
            std::uniform_real_distribution<float> jit(-0.1f, 0.1f);
            for (auto& j : shakeJit_) {
                j[0] = jit(shakeRnd_);
                j[1] = jit(shakeRnd_);
            }
        }
        const float s = 0.2f * shake;
        const float* j = shakeJit_[eye + 1];
        const float dx = s * (j[0] + 0.3f * std::sin(lv.time * 40.0f)),
                    dy = s * (j[1] + 0.3f * std::sin(lv.time * 33.0f));
        cp.x += dx;
        cp.y += dy;
        shakeRoll = Quat::axisAngle(Vec3(0, 0, 1), -dx);
    }
    camPos = cp;
    view_ = Mat4::fromQuat((lv.roll * shakeRoll).conj()) * Mat4::translation(-cp);
}

// uploads a few mesh batches per room and frame, so entering a room does not stall
void GameRenderer::preloadBatches(Level& lv) {
    for (Room* r : {lv.cur.get(), lv.next.get()}) {
        if (!r) continue;
        int budget = 6;
        for (MeshBatch& b : r->batches)
            if (b.gpuId == -1 && budget-- > 0) loadBatch(lv, b);
    }
}

// Builds and uploads the per-frame geometry: shapes, sprites, debris and soft shadows.
void GameRenderer::buildGeometry(Level& lv, bool low) {
    std::vector<uint8_t> plain, glass, glassTex, metal, debris;
    std::vector<uint8_t> edges;
    buildShapes(lv, plain, glass, glassTex, metal, low ? nullptr : &edges);
    buildSprites(lv);
    sortSprites();
    buildDebris(lv, debris);
    dynPlain_.upload(plain.data(), plain.size(), nullptr, 0);
    dynGlass_.upload(glass.data(), glass.size(), nullptr, 0);
    dynGlassTex_.upload(glassTex.data(), glassTex.size(), nullptr, 0);
    dynMetal_.upload(metal.data(), metal.size(), nullptr, 0);
    dynEdges_.upload(edges.data(), edges.size(), nullptr, 0);
    dynDebris_.upload(debris.data(), debris.size(), nullptr, 0);
    std::vector<uint8_t> shadows;
    buildShadows(lv, shadows); // soft shadows are not gated by quality (only the ball sprite choice is)
    dynShadow_.upload(shadows.data(), shadows.size(), nullptr, 0);
}

// debris: each piece is a flat-shaded tetrahedron with outward-wound faces, emitted only where they face the
// camera. The vertex colour is stored as rgb*0.5 with alpha kept, so glass.glsl lights it as 0.5c*(light + a).
void GameRenderer::buildDebris(Level& lv, std::vector<uint8_t>& debris) {
    static const int kDebrisFaces[4][3] = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
    for (auto& d : lv.debris) {
        Mat3 R = Mat3::fromQuat(d.rot);
        Vec3 wc[4];
        for (int i = 0; i < 4; i++) wc[i] = d.pos + R * d.corner[i];
        uint8_t cb[4] = {(uint8_t)(d.color & 255), (uint8_t)((d.color >> 8) & 255), (uint8_t)((d.color >> 16) & 255),
                         (uint8_t)(d.color >> 24)};
        for (const auto& f : kDebrisFaces) {
            Vec3 c = cross(wc[f[1]] - wc[f[0]], wc[f[2]] - wc[f[0]]);
            float cl = length(c);
            Vec3 n = cl > 0 ? c / cl : Vec3(1, 0, 0);
            if (dot(n, wc[f[0]] - camPos) > 0) continue;
            for (int k = 0; k < 3; k++) {
                DynVert v{};
                v.p[0] = wc[f[k]].x;
                v.p[1] = wc[f[k]].y;
                v.p[2] = wc[f[k]].z;
                v.n[0] = n.x;
                v.n[1] = n.y;
                v.n[2] = n.z;
                memcpy(v.c, cb, 4);
                append(debris, &v, 36);
            }
        }
    }
}

Shader* GameRenderer::qualityShader(const std::string& name, bool low) {
    Shader* s = shader(low ? name + "low" : name);
    return s ? s : shader(name);
}

void GameRenderer::drawRooms(Level& lv, bool low) {
    Shader* room = qualityShader("room", low);
    room->use();
    useFog(*room);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/tiles.png").id);
    gl::Uniform1i(room->uniform("uTexture0"), 0);
    drawRoom(lv, *lv.cur, *room);
    if (lv.next) drawRoom(lv, *lv.next, *room);
}

void GameRenderer::drawPlainShapes(const Mat4& mvp) {
    if (dynPlain_.empty()) return;
    Shader* s = shader("bodytex");
    s->use();
    useFog(*s);
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/tiles.png").id);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    dynPlain_.draw();
}

// glass and textured glass, then the debris with the glass state left behind
void GameRenderer::drawGlass(bool low, const Mat4& mvp) {
    GLuint refrId = low ? tex("gfx/glassnormal.jpg").id : refr_[refrRead_].tex;
    // glass shader `name` (or its low variant) with texture `texName` and the refraction buffer, blending at low
    auto useGlass = [&](const char* name, const char* texName) {
        Shader* s = qualityShader(name, low);
        s->use();
        useFog(*s);
        gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
        gl::ActiveTexture(GL_TEXTURE0);
        gl::BindTexture(GL_TEXTURE_2D, tex(texName).id);
        gl::Uniform1i(s->uniform("uTexture0"), 0);
        gl::ActiveTexture(GL_TEXTURE1);
        gl::BindTexture(GL_TEXTURE_2D, refrId);
        gl::Uniform1i(s->uniform("uTexture1"), 1);
        if (low) {
            gl::Enable(GL_BLEND);
            gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
    };
    auto drawMesh = [&](DynMesh& mesh, const char* name, const char* texName) {
        if (mesh.empty()) return;
        useGlass(name, texName);
        gl::Enable(GL_POLYGON_OFFSET_FILL);
        gl::PolygonOffset(1.0f, 10.0f);
        mesh.draw();
        gl::Disable(GL_POLYGON_OFFSET_FILL);
        if (&mesh == &dynGlass_ && !dynEdges_.empty()) { // edge lines with the still-bound glass shader
            gl::LineWidth(lineWidth2_);
            dynEdges_.draw(GL_LINES);
            gl::LineWidth(1.0f);
        }
        gl::Disable(GL_BLEND);
        gl::ActiveTexture(GL_TEXTURE0);
    };
    drawMesh(dynGlass_, "glass", "gfx/glassnormal.jpg");
    drawMesh(dynGlassTex_, "glasstex", "gfx/credits.png");
    // debris reuses the glass state left behind: the last glass shader drawn (glasstex + credits.png if textured glass
    // was drawn), culling on, blending only at low quality (glasslow's alpha is 0.5+0.5*spec)
    if (!dynDebris_.empty()) {
        const bool tex2 = !dynGlassTex_.empty();
        useGlass(tex2 ? "glasstex" : "glass", tex2 ? "gfx/credits.png" : "gfx/glassnormal.jpg");
        dynDebris_.draw();
        gl::Disable(GL_BLEND);
        gl::ActiveTexture(GL_TEXTURE0);
    }
}

void GameRenderer::drawMetal(bool low, const Mat4& mvp) {
    if (dynMetal_.empty()) return;
    Shader* s = qualityShader("metal", low);
    s->use();
    useFog(*s);
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    if (s->uniform("uCameraPos") >= 0) gl::Uniform3f(s->uniform("uCameraPos"), camPos.x, camPos.y, camPos.z);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, low ? tex("gfx/metalnormal.jpg").id : refr_[refrRead_].tex);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    gl::ActiveTexture(GL_TEXTURE1);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/metalnormal.jpg").id);
    gl::Uniform1i(s->uniform("uTexture1"), 1);
    dynMetal_.draw();
    gl::ActiveTexture(GL_TEXTURE0);
}

// one vertex buffer for the sprite pass and the reflection pass: normal, door and mirrored index ranges
void GameRenderer::uploadSprites() {
    spriteMirCount_ = spriteIdxCount_ = spriteDoorCount_ = 0;
    if (sprites_.empty()) return;
    std::vector<uint8_t> sv;
    std::vector<uint32_t> idx;
    std::vector<uint32_t> doorIdx, mirIdx;
    uint32_t base = 0;
    for (auto& q : sprites_) {
        for (int i = 0; i < 4; i++) {
            SpriteVert v;
            v.p[0] = q.c[i].x;
            v.p[1] = q.c[i].y;
            v.p[2] = q.c[i].z;
            v.uv[0] = q.uv[i][0];
            v.uv[1] = q.uv[i][1];
            memcpy(v.c, &q.color, 4);
            v.a = q.blend;
            append(sv, &v, 28);
        }
        auto& dst = q.doors ? doorIdx : idx;
        if (q.mode != kSpriteMirrorOnly) {
            for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) dst.push_back(base + k);
        }
        if (!q.doors && q.mode != kSpriteDirect) {
            for (uint32_t k : {0u, 3u, 2u, 0u, 2u, 1u}) mirIdx.push_back(base + k);
        }
        base += 4;
    }
    std::vector<uint32_t> all = idx;
    size_t doorStart = all.size();
    all.insert(all.end(), doorIdx.begin(), doorIdx.end());
    size_t mirStart = all.size();
    all.insert(all.end(), mirIdx.begin(), mirIdx.end());
    spriteMirStart_ = mirStart;
    spriteMirCount_ = mirIdx.size();
    spriteIdxCount_ = idx.size();
    spriteDoorStart_ = doorStart;
    spriteDoorCount_ = doorIdx.size();
    dynSprites_.upload(sv.data(), sv.size(), all.data(), all.size());
}

// floor reflections, medium/high quality: mark the reflective floor in the stencil buffer, then draw mirrored
// sprites, bodies and the room reflection geometry where it is set
void GameRenderer::drawFloorReflections(Level& lv, const Mat4& mvp) {
    std::vector<float> maskV;
    std::vector<uint8_t> geomV;
    struct RoomRef {
        Room* r;
        size_t mOff, mCnt, gOff, gCnt;
    };
    std::vector<RoomRef> refs;
    for (Room* r : {lv.cur.get(), lv.next.get()}) {
        if (!r || r->reflMask.empty()) continue;
        refs.push_back({r, maskV.size() / 3, r->reflMask.size() / 3, geomV.size() / 16, r->reflGeom.size() / 16});
        maskV.insert(maskV.end(), r->reflMask.begin(), r->reflMask.end());
        geomV.insert(geomV.end(), r->reflGeom.begin(), r->reflGeom.end());
    }
    if (refs.empty()) return;
    reflMaskMesh_.upload(maskV.data(), maskV.size() * 4, nullptr, 0);
    reflGeomMesh_.upload(geomV.data(), geomV.size(), nullptr, 0);
    gl::Clear(GL_STENCIL_BUFFER_BIT);
    gl::Enable(GL_STENCIL_TEST);
    gl::StencilFunc(GL_ALWAYS, 1, 1);
    gl::StencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    Shader* bd = shader("bodydepth");
    gl::ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl::DepthMask(GL_FALSE);
    gl::Disable(GL_CULL_FACE);
    bd->use();
    for (auto& rr : refs) {
        Mat4 m = proj_ * view_ * Mat4::translation(rr.r->body->pos);
        gl::UniformMatrix4fv(bd->uniform("uMvpMatrix"), 1, GL_FALSE, m.m);
        reflMaskMesh_.drawRange(GL_TRIANGLES, rr.mOff, rr.mCnt);
    }
    gl::ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl::StencilFunc(GL_EQUAL, 1, 1);
    gl::StencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Disable(GL_DEPTH_TEST);
    gl::Enable(GL_CULL_FACE);
    // mirrored sprites (balls, powerups)
    if (spriteMirCount_ > 0) {
        Shader* sr = shader("spritereflection");
        if (sr) {
            sr->use();
            useFog(*sr);
            gl::UniformMatrix4fv(sr->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
            gl::ActiveTexture(GL_TEXTURE0);
            gl::BindTexture(GL_TEXTURE_2D, tex("gfx/sprites.png").id);
            gl::Uniform1i(sr->uniform("uTexture0"), 0);
            dynSprites_.draw(GL_TRIANGLES, (GLsizei)spriteMirCount_, spriteMirStart_);
        }
    }
    // mirrored bodies + room reflection geometry
    Shader* br = shader("bodyreflection");
    if (br) {
        br->use();
        useFog(*br);
        gl::UniformMatrix4fv(br->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
        gl::StencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
        gl::CullFace(GL_FRONT);
        dynPlain_.draw();
        dynGlass_.draw();
        dynGlassTex_.draw();
        dynMetal_.draw();
        for (auto& rr : refs) {
            Mat4 m = proj_ * view_ * Mat4::translation(rr.r->body->pos);
            gl::UniformMatrix4fv(br->uniform("uMvpMatrix"), 1, GL_FALSE, m.m);
            reflGeomMesh_.drawRange(GL_TRIANGLES, rr.gOff, rr.gCnt);
        }
        gl::CullFace(GL_BACK);
    }
    gl::Disable(GL_STENCIL_TEST);
    gl::Disable(GL_BLEND);
    gl::Enable(GL_DEPTH_TEST);
}

void GameRenderer::drawSoftShadows(const Mat4& mvp) {
    if (dynShadow_.empty()) return;
    Shader* s = shader("softshadow");
    if (!s) return;
    s->use();
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    gl::BindTexture(GL_TEXTURE_2D, blob_.id);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::DepthMask(GL_FALSE);
    gl::Disable(GL_CULL_FACE);
    dynShadow_.draw();
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_BLEND);
}

// water, medium/high quality: a height-field grid over the water's extent that samples the refraction buffer,
// blended with depth test, depth write and culling on. Normals stay unnormalised, (3*dH/dx, 1, 3*dH/dz) from clamped
// neighbours.
void GameRenderer::drawWater(Level& lv, const Mat4& mvp) {
    Shader* s = shader("water");
    if (!s) return;
    s->use();
    useFog(*s);
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    const Vec3 lc = lerp(lv.prevCam, lv.cam, interp); // the level camera, without eye offset or shake
    if (s->uniform("uCameraPos") >= 0) gl::Uniform3f(s->uniform("uCameraPos"), lc.x, lc.y, lc.z);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, refr_[refrRead_].tex);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthMask(GL_TRUE);
    gl::Enable(GL_CULL_FACE);
    for (const Water& w : lv.waters) {
        const int nx = w.nx, ny = w.ny;
        std::vector<DynVert> vv((size_t)(nx * ny));
        auto H = [&](int i, int j) { return w.H[(size_t)(j * nx + i)]; };
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                DynVert& v = vv[(size_t)(j * nx + i)];
                v = DynVert{};
                v.p[0] = w.pos.x - w.sx + (float)i * (2 * w.sx / (float)(nx - 1));
                v.p[1] = w.pos.y + H(i, j);
                v.p[2] = w.pos.z + w.sz - (float)j * (2 * w.sz / (float)(ny - 1));
                v.n[0] = (H(std::min(i + 1, nx - 1), j) - H(std::max(i, 1) - 1, j)) * 3.0f;
                v.n[1] = 1;
                v.n[2] = (H(i, std::min(j + 1, ny - 1)) - H(i, std::max(j, 1) - 1)) * 3.0f;
            }
        std::vector<uint32_t> idx;
        idx.reserve((size_t)((nx - 1) * (ny - 1) * 6));
        for (int j = 0; j + 1 < ny; j++)
            for (int i = 0; i + 1 < nx; i++) {
                uint32_t a = (uint32_t)(j * nx + i), b = a + 1, c = (uint32_t)((j + 1) * nx + i + 1), d = c - 1;
                for (uint32_t k : {a, b, c, c, d, a}) idx.push_back(k);
            }
        dynWater_.upload(vv.data(), vv.size() * sizeof(DynVert), idx.data(), idx.size());
        dynWater_.draw();
    }
    gl::Disable(GL_BLEND);
}

// ball and powerup chrome discs: background refraction through ball.png, at every quality. In low only powerups
// add discs and refr_ is the cleared, never redrawn FBO, same as the original game.
void GameRenderer::drawBallDiscs(const Mat4& mvp) {
    if (ballIdx_.empty()) return;
    Shader* s = shader("ballreflection");
    if (!s) return;
    dynBall_.upload(ballVerts_.data(), ballVerts_.size(), ballIdx_.data(), ballIdx_.size());
    s->use();
    useFog(*s);
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/ball.png").id);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    gl::ActiveTexture(GL_TEXTURE1);
    gl::BindTexture(GL_TEXTURE_2D, refr_[refrRead_].tex);
    gl::Uniform1i(s->uniform("uTexture1"), 1);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_CULL_FACE);
    dynBall_.draw();
    gl::Disable(GL_BLEND);
}

void GameRenderer::drawSprites(bool low, const Mat4& mvp) {
    if (sprites_.empty()) return;
    const size_t doorStart = spriteDoorStart_, idxCount = spriteIdxCount_, doorCount = spriteDoorCount_;
    Shader* s = qualityShader("sprite", low);
    s->use();
    useFog(*s);
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(low ? GL_SRC_ALPHA : GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl::DepthMask(GL_FALSE);
    gl::Disable(GL_CULL_FACE);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/sprites.png").id);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    if (idxCount) dynSprites_.draw(GL_TRIANGLES, (GLsizei)idxCount, 0);
    if (doorCount) {
        gl::BindTexture(GL_TEXTURE_2D, tex("gfx/doors.png").id);
        dynSprites_.draw(GL_TRIANGLES, (GLsizei)doorCount, doorStart);
    }
    gl::DepthMask(GL_TRUE);
    gl::Disable(GL_BLEND);
}

void GameRenderer::drawMenuStrips(const Mat4& clip, float scroll, int mode, float alpha, unsigned reached) {
    Shader* s = shader("roommenu");
    if (!s || alpha <= 0.01f) return;
    static const float tints[6][3] = {{0.75f, 0.65f, 0.45f}, {1, 1, 1},          {1, 0.5f, 0.6f},
                                      {0.6f, 0.9f, 0.55f},   {0.5f, 0.3f, 0.5f}, {0.2f, 0.5f, 0.6f}};
    const float* tint = tints[mode >= 0 && mode < 6 ? mode : 1];
    gl::Clear(GL_DEPTH_BUFFER_BIT);
    gl::Enable(GL_DEPTH_TEST);
    gl::DepthFunc(GL_LEQUAL);
    gl::Enable(GL_CULL_FACE);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    s->use();
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, tex("gfx/tiles.png").id);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
    gl::Uniform4f(s->uniform("uColor"), tint[0], tint[1], tint[2], alpha);
    gl::Uniform4f(s->uniform("uLowerFog"), 0.05f, 0.1f, 0.12f, 1);
    gl::Uniform4f(s->uniform("uUpperFog"), 0.1f, 0.25f, 0.3f, 1);
    // training/classic/expert draw 13 tiles: tile i uses mesh i+1 when i == 0 or checkpoint i is reached, else
    // hidden.mesh. zen/versus/co-op draw a single tile with zen, versus or coop.mesh.
    static const char* kSingle[3] = {"zen", "versus", "coop"};
    const bool single = mode >= kModeZen && mode <= kModeCoop;
    for (int i = 1; i <= (single ? 1 : 13); i++) {
        const int tile = i - 1;
        std::string name = single                                  ? kSingle[mode - kModeZen]
                           : (tile == 0 || (reached >> tile & 1u)) ? std::to_string(i)
                                                                   : "hidden";
        std::string key = "segments/menu/" + name + ".mesh";
        auto& slot = menuMeshes_[key];
        if (!slot) {
            slot.reset(new DynMesh());
            std::vector<uint8_t> raw, un;
            std::vector<uint32_t> idx;
            uint32_t n;
            if (vfs_->read(key, raw) && inflateAll(raw.data(), raw.size(), un) && parseMesh24(un, n, idx)) {
                // drop decoration lying between the menu camera and the GUI plane (x > 6) so it cannot cover the
                // buttons
                std::vector<uint32_t> keep;
                for (size_t k = 0; k + 3 <= idx.size(); k += 3) {
                    float mn = 1e30f;
                    for (int c = 0; c < 3; c++) {
                        float x;
                        memcpy(&x, &un[4 + 24ull * idx[k + c]], 4);
                        mn = std::min(mn, x);
                    }
                    if (mn <= 6.0f) keep.insert(keep.end(), idx.begin() + k, idx.begin() + k + 3);
                }
                slot->init(VLayout::Mesh24);
                slot->upload(&un[4], 24ull * n, keep.data(), keep.size());
            }
        }
        if (slot->empty()) continue;
        Mat4 M = Mat4::translation(Vec3(0, 0, -(float)(i - 1) * 32.0f - 12.0f - scroll));
        Mat4 mvp = clip * M;
        gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
        slot->draw();
    }
    gl::Disable(GL_BLEND);
}

} // namespace sh
