// Convex shapes with GJK/EPA overlap, distance and ray queries (box overlap tests, fracture connectivity). Solver
// contacts come from the SAT code in solver.cpp.
#pragma once
#include <vector>
#include "core/math.h"

namespace sh {

enum class ShapeType { Box, Sphere, Cylinder, Hull };

// Convex shape in local space, placed in the world with (pos, rot).
struct Convex {
    ShapeType type = ShapeType::Box;
    Vec3 half{0.5f, 0.5f, 0.5f}; // Box half extents; Sphere: x = radius; Cylinder: x = radius, y = half height (axis Y)
    std::vector<Vec3> verts;     // Hull
    Vec3 pos;
    Quat rot;

    Vec3 supportLocal(const Vec3& d) const;
    Vec3 support(const Vec3& worldDir) const; // world-space support point
};

struct Contact {
    Vec3 normal;         // from A towards B (direction A must move to separate is -normal)
    float depth = 0;     // penetration depth (>0)
    Vec3 pointA, pointB; // witness points on each surface
};

// Returns true if overlapping and fills `out`. Penetration direction is stable for deep overlaps.
bool collide(const Convex& a, const Convex& b, Contact& out);

// Closest points between two separated shapes (returns distance, 0 if overlapping).
float distance(const Convex& a, const Convex& b, Vec3* pa = nullptr, Vec3* pb = nullptr);

// Ray vs convex (in world space). Returns hit fraction t in [0, maxT] along dir (unit length not required).
bool raycast(const Convex& s, const Vec3& origin, const Vec3& dir, float maxT, float& t, Vec3& normal);

} // namespace sh
