// Sprite particle systems driven by effects/default.xml.
#pragma once
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "core/math.h"
#include "core/vfs.h"

namespace sh {

struct ParticleQuad {
    Vec3 pos;
    float half = 0, angle = 0;
    bool streak = false; // axial motion: stretched along the view direction
    float rgba[4] = {1, 1, 1, 1};
    float uv[4] = {0, 0, 1, 1}; // u0,v0,u1,v1 of the frame (virtual 2048x2048 coords already normalised)
    float blend = 1;
};

class ParticleSystem {
public:
    bool load(const Vfs& vfs, const std::string& name);
    void spawn(const Vec3& pos, const Vec3& vel, const float rgba[4], int frame, std::mt19937& rng);
    void update(float dt, std::mt19937& rng);
    void shift(float dz);
    void collect(std::vector<ParticleQuad>& out) const;
    void clear() { p_.clear(); }

private:
    struct P {
        Vec3 pos, vel;
        float color[4];
        float angle, angVel, life, age;
        int frame;
    };
    std::vector<P> p_;
    float coords_[4] = {0, 0, 1, 1};
    int cols_ = 1, rows_ = 1;
    size_t max_ = 128;
    float gravity_ = 0, keep_ = 1, life_[2] = {1, 1}, rot_[2] = {0, 0}, size_[2] = {1, 1}, alpha_[2] = {1, 0},
          blending_ = 1;
    bool randomRot_ = true, angVelSize_ = false;
};

class Effects {
public:
    explicit Effects(const Vfs& vfs) : vfs_(vfs) {}
    ParticleSystem* get(const std::string& name);
    // `count` particles around pos/vel, offsets of random direction and length |N(0,1)| * posSpread / velSpread
    void burst(const std::string& name, const Vec3& pos, const Vec3& vel, const float rgba[4], int count,
               float posSpread, float velSpread);
    // like the original: cube-sampled directions (not uniform) with lengths randCentered(0, posSpread) /
    // randCentered(0, velSpread)
    void burstCentered(const std::string& name, const Vec3& pos, float posSpread, const Vec3& vel, float velSpread,
                       const float rgba[4], int count);
    void update(float dt);
    void shift(float dz);
    void clear();
    void collect(std::vector<ParticleQuad>& out) const;
    std::mt19937 rng{1234};

private:
    const Vfs& vfs_;
    std::map<std::string, std::unique_ptr<ParticleSystem>> systems_;
};

} // namespace sh
