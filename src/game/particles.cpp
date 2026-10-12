#include "game/particles.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "core/random.h"
#include "core/xml.h"

namespace sh {

// a direction uniform over the sphere (rejection sampling), scaled to len
static Vec3 randSphereDir(std::mt19937& r, float len) {
    for (int i = 0; i < 16; i++) {
        Vec3 v = randCube(r);
        float l = length(v);
        if (l > 0.05f && l <= 1) return v / l * len;
    }
    return Vec3(0, len, 0);
}

bool ParticleSystem::load(const Vfs& vfs, const std::string& name) {
    bool ok = false;
    std::string txt = vfs.readText("effects/default.xml", &ok);
    if (!ok) return false;
    auto doc = parseXml(txt);
    if (!doc) return false;
    const XmlNode* root = doc->child("effects");
    if (!root) return false;
    for (auto& e : root->children) {
        if (e->attrOr("name", "") != name) continue;
        float c[4] = {0, 0, 2048, 2048};
        sscanf(e->attrOr("coords", "0 0 2048 2048").c_str(), "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
        for (int i = 0; i < 4; i++) coords_[i] = c[i] / 2048.0f; // .mtx sprite sheet has a 2048 virtual size
        cols_ = std::max(1, atoi(e->attrOr("cols", "1").c_str()));
        rows_ = std::max(1, atoi(e->attrOr("rows", "1").c_str()));
        max_ = (size_t)std::max(1, atoi(e->attrOr("max", "128").c_str()));
        gravity_ = e->floatAttr("gravity", 0);
        keep_ = 1.0f - clampf(e->floatAttr("damping", 0), 0, 1);
        auto pair = [&](const char* k, float* o, float d0, float d1) {
            o[0] = d0;
            o[1] = d1;
            sscanf(e->attrOr(k, "").c_str(), "%f %f", &o[0], &o[1]);
        };
        pair("life", life_, 1, 1);
        pair("rot", rot_, 0, 0);
        pair("size", size_, 1, 1);
        pair("alpha", alpha_, 1, 0);
        blending_ = e->attr("blending") ? e->floatAttr("blending", 1) : 1.0f;
        randomRot_ = e->attrOr("randomrot", "true") != "false";
        angVelSize_ = e->attrOr("angvelsize", "false") == "true";
        return true;
    }
    return false;
}

void ParticleSystem::spawn(const Vec3& pos, const Vec3& vel, const float rgba[4], int frame, std::mt19937& rng) {
    P q;
    q.pos = pos;
    q.vel = vel;
    for (int i = 0; i < 4; i++) q.color[i] = rgba[i];
    q.frame = frame >= 0 ? frame : (int)(rng() % (unsigned)(cols_ * rows_));
    q.angle = randomRot_ ? randRange(rng, 0, 2 * kPi) : 0;
    q.angVel = randCentered(rng, rot_[0], rot_[1]);
    q.life = std::max(0.01f, randCentered(rng, life_[0], life_[1]));
    q.age = 0;
    if (p_.size() >= max_) {
        size_t oldest = 0;
        for (size_t i = 1; i < p_.size(); i++)
            if (p_[i].age > p_[oldest].age) oldest = i;
        p_[oldest] = q;
    } else p_.push_back(q);
}

void ParticleSystem::update(float dt, std::mt19937& rng) {
    for (size_t i = 0; i < p_.size();) {
        P& q = p_[i];
        if (q.age >= q.life) {
            p_[i] = p_.back();
            p_.pop_back();
            continue;
        }
        // random 0.06 kick once per tick, not scaled by dt, so slow motion and the pause fade keep it
        if (q.vel.x != 0 || q.vel.y != 0) q.vel += randCubeDir(rng, 0.06f);
        q.age += dt;
        q.vel.y += gravity_ * dt;
        q.vel.x *= keep_;
        q.vel.y *= keep_;
        q.vel.z *= keep_;
        q.pos += q.vel * dt;
        q.angle += q.angVel * dt;
        i++;
    }
}

void ParticleSystem::shift(float dz) {
    for (auto& q : p_) q.pos.z += dz;
}

void ParticleSystem::collect(std::vector<ParticleQuad>& out) const {
    for (const P& q : p_) {
        float t = clampf(q.age / q.life, 0, 1);
        ParticleQuad pq;
        pq.pos = q.pos;
        pq.half = (size_[0] * (1 - t) + size_[1] * t + (angVelSize_ ? q.angVel : 0)) * 0.5f;
        pq.angle = q.angle;
        pq.streak = q.vel.x == 0 && q.vel.y == 0;
        for (int i = 0; i < 3; i++) pq.rgba[i] = q.color[i];
        pq.rgba[3] = q.color[3] * (alpha_[0] * (1 - t) + alpha_[1] * t);
        int cx = q.frame % cols_, cy = q.frame / cols_;
        float cw = (coords_[2] - coords_[0]) / cols_, ch = (coords_[3] - coords_[1]) / rows_;
        pq.uv[0] = coords_[0] + cx * cw;
        pq.uv[1] = coords_[1] + cy * ch;
        pq.uv[2] = pq.uv[0] + cw;
        pq.uv[3] = pq.uv[1] + ch;
        pq.blend = blending_;
        out.push_back(pq);
    }
}

ParticleSystem* Effects::get(const std::string& name) {
    auto it = systems_.find(name);
    if (it != systems_.end()) return it->second.get();
    auto s = std::make_unique<ParticleSystem>();
    if (!s->load(vfs_, name)) {
        fprintf(stderr, "Couldn't find effect %s\n", name.c_str());
        s.reset();
    }
    ParticleSystem* raw = s.get();
    systems_[name] = std::move(s);
    return raw;
}

void Effects::burst(const std::string& name, const Vec3& pos, const Vec3& vel, const float rgba[4], int count,
                    float posSpread, float velSpread) {
    ParticleSystem* s = get(name);
    if (!s) return;
    std::normal_distribution<float> g(0, 1);
    for (int i = 0; i < count; i++) {
        Vec3 p = pos + randSphereDir(rng, std::fabs(g(rng)) * posSpread);
        Vec3 v = vel + randSphereDir(rng, std::fabs(g(rng)) * velSpread);
        s->spawn(p, v, rgba, -1, rng);
    }
}

void Effects::burstCentered(const std::string& name, const Vec3& pos, float posSpread, const Vec3& vel, float velSpread,
                            const float rgba[4], int count) {
    ParticleSystem* s = get(name);
    if (!s) return;
    for (int i = 0; i < count; i++) {
        Vec3 p = pos + randCubeDir(rng, randCentered(rng, 0, posSpread));
        Vec3 v = vel + randCubeDir(rng, randCentered(rng, 0, velSpread));
        s->spawn(p, v, rgba, -1, rng);
    }
}

void Effects::update(float dt) {
    for (auto& kv : systems_)
        if (kv.second) kv.second->update(dt, rng);
}
void Effects::shift(float dz) {
    for (auto& kv : systems_)
        if (kv.second) kv.second->shift(dz);
}
void Effects::clear() {
    for (auto& kv : systems_)
        if (kv.second) kv.second->clear();
}
void Effects::collect(std::vector<ParticleQuad>& out) const {
    for (auto& kv : systems_)
        if (kv.second) kv.second->collect(out);
}

} // namespace sh
