// GJK/EPA queries (physics/convex.h): box, sphere, cylinder and hull overlaps, distances and raycasts.
#include <cstdio>
#include "physics/convex.h"
#include "check.h"
using namespace sh;
static bool near(float a, float b, float e = 1e-2f) { return std::fabs(a - b) < e; }

int main() {
    Contact c;
    Convex a;
    a.type = ShapeType::Box;
    a.half = Vec3(1, 1, 1);
    Convex b = a;
    b.pos = Vec3(1.5f, 0.1f, 0);
    CHECK(collide(a, b, c));
    CHECK(near(c.depth, 0.5f));
    CHECK(c.normal.x > 0.9f);
    b.pos = Vec3(2.5f, 0, 0);
    CHECK(!collide(a, b, c));
    CHECK(near(distance(a, b), 0.5f));
    // rotated box 45 deg about Y: corner reaches sqrt(2)
    b.pos = Vec3(2.2f, 0, 0);
    b.rot = Quat::axisAngle(Vec3(0, 1, 0), kPi / 4);
    CHECK(collide(a, b, c));
    CHECK(near(c.depth, 1 + 1.41421f - 2.2f, 0.05f));
    // sphere vs box
    Convex s;
    s.type = ShapeType::Sphere;
    s.half = Vec3(0.5f, 0, 0);
    s.pos = Vec3(0, 1.3f, 0);
    CHECK(collide(a, s, c));
    CHECK(near(c.depth, 0.2f));
    CHECK(c.normal.y > 0.9f);
    s.pos = Vec3(0, 1.6f, 0);
    CHECK(!collide(a, s, c));
    CHECK(near(distance(a, s), 0.1f));
    // cylinder vs sphere
    Convex cy;
    cy.type = ShapeType::Cylinder;
    cy.half = Vec3(1, 2, 0);
    s.pos = Vec3(1.3f, 0, 0);
    CHECK(collide(cy, s, c));
    CHECK(near(c.depth, 0.2f));
    // deep overlap (concentric) still returns something sane
    b = a;
    CHECK(collide(a, b, c));
    CHECK(c.depth > 1.9f);
    // rays
    float t;
    Vec3 n;
    CHECK(raycast(a, Vec3(-5, 0.2f, 0), Vec3(1, 0, 0), 100, t, n));
    CHECK(near(t, 4));
    CHECK(n.x < -0.9f);
    CHECK(!raycast(a, Vec3(-5, 2.0f, 0), Vec3(1, 0, 0), 100, t, n));
    CHECK(raycast(s, Vec3(-5, 0, 0), Vec3(1, 0, 0), 100, t, n));
    CHECK(near(t, 5 + 1.3f - 0.5f));
    CHECK(raycast(cy, Vec3(-5, 0, 0), Vec3(1, 0, 0), 100, t, n));
    CHECK(near(t, 4));
    Convex hull;
    hull.type = ShapeType::Hull;
    hull.verts = {{-1, -1, -1}, {1, -1, -1}, {0, 1, -1}, {0, 0, 1}};
    CHECK(raycast(hull, Vec3(0, 0, 5), Vec3(0, 0, -1), 100, t, n));
    CHECK(near(t, 4, 0.05f));
    CHECK(collide(hull, a, c));
    printf(fails ? "physics_test: %d failure(s)\n" : "physics_test: ok\n", fails);
    return fails ? 1 : 0;
}
