// Random helpers on std::mt19937 with the distributions the original game uses for effects and debris.
#pragma once
#include <random>
#include "core/math.h"

namespace sh {

// uniform in [a, b), from the low 24 bits
inline float randRange(std::mt19937& r, float a, float b) { return a + (b - a) * ((r() & 0xffffff) / 16777216.0f); }

// centre-weighted in [a, b]: (a+b)/2 + (b-a)/2 * sgn(x)*x^2 with x uniform in [-1, 1)
inline float randCentered(std::mt19937& r, float a, float b) {
    float x = randRange(r, -1, 1);
    return (a + b) / 2 + (b - a) / 2 * (x < 0 ? -1.0f : 1.0f) * x * x;
}

// a uniform point in the [-1, 1) cube, drawn x, y, z in that order
inline Vec3 randCube(std::mt19937& r) {
    float x = randRange(r, -1, 1), y = randRange(r, -1, 1), z = randRange(r, -1, 1);
    return Vec3(x, y, z);
}

// a cube sample scaled to length len, so directions are not uniform over the sphere (as in the original)
inline Vec3 randCubeDir(std::mt19937& r, float len) {
    Vec3 v = randCube(r);
    float l = length(v);
    return l > 0 ? v * (len / l) : Vec3(len, 0, 0);
}

} // namespace sh
