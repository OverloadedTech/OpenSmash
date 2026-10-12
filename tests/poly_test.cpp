// Polyhedron (physics/poly.h): construction, volume and mass properties, plane splits, hulls and point distance.
#include <map>
#include <cstdio>
#include "physics/poly.h"
#include "check.h"
using namespace sh;
static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e * std::max(1.0f, std::fabs(b)); }

static bool closed(const Polyhedron& p) { // every edge shared by exactly two faces, orientation consistent
    std::map<std::pair<int, int>, int> cnt;
    for (auto& f : p.f)
        for (size_t i = 0; i < f.idx.size(); i++) {
            int a = f.idx[i], b = f.idx[(i + 1) % f.idx.size()];
            cnt[{a, b}]++;
        }
    for (auto& kv : cnt)
        if (kv.second != 1 || !cnt.count({kv.first.second, kv.first.first})) return false;
    return true;
}

int main() {
    Polyhedron b = Polyhedron::box(Vec3(1, 2, 3));
    CHECK(b.valid());
    CHECK(closed(b));
    CHECK(near(b.volume(), 2 * 4 * 6));
    float m;
    Vec3 com;
    Mat3 I;
    b.massProps(2.0f, m, com, I);
    CHECK(near(m, 96));
    CHECK(length(com) < 1e-4f);
    // box inertia: Ixx = m/12 (h^2+d^2) with full sizes (2,4,6)
    CHECK(near(I.m[0], m / 12 * (16 + 36), 1e-3f));
    CHECK(near(I.m[4], m / 12 * (4 + 36), 1e-3f));
    CHECK(near(I.m[8], m / 12 * (4 + 16), 1e-3f));
    // face normals point outward
    for (auto& f : b.f) CHECK(f.d > 0);
    // chamfered box
    Polyhedron cb = Polyhedron::box(Vec3(1, 1, 1), 0.2f);
    CHECK(cb.valid());
    CHECK(closed(cb));
    CHECK(cb.volume() < 8 && cb.volume() > 7);
    for (auto& f : cb.f) CHECK(f.d > 0);
    // cylinder along X
    Polyhedron cy = Polyhedron::cylinderX(1.0f, 2.0f, 24);
    CHECK(closed(cy));
    CHECK(near(cy.volume(), 0.5f * 24 * std::sin(2 * kPi / 24) * 4, 1e-3f));
    for (auto& f : cy.f) CHECK(f.d > 0);
    Vec3 mn, mx;
    cy.bounds(mn, mx);
    CHECK(near(mx.x, 2) && near(mx.y, 1, 0.02f));
    // split box
    Polyhedron p, n;
    CHECK(b.split(Vec3(1, 0, 0), 0.5f, p, n));
    CHECK(closed(p) && closed(n));
    CHECK(near(p.volume(), 0.5f * 4 * 6));
    CHECK(near(n.volume(), 1.5f * 24));
    CHECK(near(p.volume() + n.volume(), b.volume()));
    // diagonal split keeps volume and closedness
    Vec3 dn = normalize(Vec3(1, 1, 0.3f));
    CHECK(b.split(dn, 0.2f, p, n));
    CHECK(closed(p) && closed(n));
    CHECK(near(p.volume() + n.volume(), b.volume(), 2e-3f));
    // plane outside: no split
    CHECK(!b.split(Vec3(1, 0, 0), 5, p, n));
    // repeated splits
    Polyhedron q = n, a, c;
    CHECK(q.split(normalize(Vec3(0, 1, 1)), 0.1f, a, c));
    CHECK(near(a.volume() + c.volume(), n.volume(), 2e-3f));
    // hull of tetra + extra interior point
    Polyhedron h = Polyhedron::hull({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.1f, 0.1f, 0.1f}});
    CHECK(h.valid());
    CHECK(near(h.volume(), 1.0f / 6));
    CHECK(h.f.size() == 4);
    CHECK(closed(h));
    // distance
    CHECK(near(b.distanceTo(Vec3(3, 0, 0)), 2));
    CHECK(b.distanceTo(Vec3(0, 0, 0)) == 0);
    CHECK(near(b.distanceTo(Vec3(2, 3, 0)), std::sqrt(1.0f + 1.0f)));
    // translated box COM
    Polyhedron t = Polyhedron::box(Vec3(1, 1, 1));
    t.transform(Vec3(1, 1, 1), Quat(), Vec3(5, 0, 0));
    t.massProps(1, m, com, I);
    CHECK(near(com.x, 5));
    CHECK(near(m, 8));
    CHECK(near(I.m[0], 8.0f / 12 * 8, 1e-3f));
    printf(fails ? "poly_test: %d failure(s)\n" : "poly_test: ok\n", fails);
    return fails ? 1 : 0;
}
