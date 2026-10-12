// Convex polyhedron: vertices + planar faces (CCW seen from outside). Built from boxes/cylinders/point
// clouds, split by planes (fracture) and integrated for mass properties.
#pragma once
#include <utility>
#include <vector>
#include "core/math.h"

namespace sh {

struct PolyFace {
    std::vector<int> idx; // CCW from outside
    Vec3 n;               // outward unit normal
    float d = 0;          // plane: dot(n, x) = d
};

struct Polyhedron {
    std::vector<Vec3> v;
    std::vector<PolyFace> f;
    std::vector<std::pair<int, int>> edges; // unique edges (i<j)
    // vertex normals: normalised sum of the unit normals of the faces using the vertex (rendering of round shapes)
    std::vector<Vec3> vn;

    static Polyhedron box(const Vec3& half, float chamfer = 0);
    // Prism with axis along local X (matches mgCreateCylinder), `sides`-gon cross-section.
    static Polyhedron cylinderX(float radius, float halfLen, int sides = 24, float chamfer = 0);
    static Polyhedron hull(const std::vector<Vec3>& pts);

    bool valid() const { return v.size() >= 4 && f.size() >= 4; }
    void rebuildDerived();                                           // normals, planes, edge list from faces
    void transform(const Vec3& scale, const Quat& q, const Vec3& t); // scale, rotate, translate
    // Clip by plane dot(n,x)=d. Returns false if the plane does not cut through the interior.
    // `pos` is the half with dot(n,x) >= d, `neg` the other half; both closed convex solids.
    bool split(const Vec3& n, float d, Polyhedron& pos, Polyhedron& neg, float eps = 1e-3f) const;
    void massProps(float density, float& mass, Vec3& com, Mat3& inertiaAboutCom) const;
    float volume() const;
    void bounds(Vec3& mn, Vec3& mx) const;
    // Closest distance from point to polyhedron (0 if inside), with closest point.
    float distanceTo(const Vec3& p, Vec3* closest = nullptr) const;
};

// Closest point to p on triangle abc (Ericson, Real-Time Collision Detection).
Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);

} // namespace sh
