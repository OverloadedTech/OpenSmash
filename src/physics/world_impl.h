// Internal data shared by the physics translation units (world.cpp, solver.cpp, fracture.cpp).
#pragma once
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>
#include "physics/convex.h"
#include "physics/world.h"

namespace sh {

constexpr float kBallRadius = 0.1f;
constexpr float kRayBallRadius = 0.2f;
constexpr float kCellSize = 4.0f; // static broadphase grid along z

struct ContactPt {
    Vec3 pos;      // world position (midpoint)
    float sep = 0; // signed separation along normal (negative = penetrating)
    float accN = 0;
};

// armed: store the centroid and normal at the next init; active: drift target on the tangential rows
enum AnchorState { kAnchorNone, kAnchorArmed, kAnchorActive };

// Friction anchor of a contact pair, keyed by the pair's shapes in pointer order. p/n hold the contact
// centroid and normal in the body frames of the lower and higher shape.
struct Anchor {
    AnchorState state = kAnchorNone;
    int seen = -10; // solver pass that last had a contact for the pair
    Vec3 p[2], n[2];
};
using ShapePair = std::pair<const Shape*, const Shape*>;

struct Manifold {
    Shape *a = nullptr, *b = nullptr;
    Vec3 n; // A -> B
    int count = 0;
    ContactPt pts[4];
    float cap = FLT_MAX, mu = 0, e = 0;
    bool edgeContact = false;
    Anchor* anchor = nullptr; // valid until the next pass rebuilds the manifolds
    bool slipped = false;     // a tangential row ended the solve at its Coulomb bound
};

enum RowKind { kRowGeneric, kRowContactNormal, kRowFriction };

struct Row {
    Body *a = nullptr, *b = nullptr; // nullptr = static world
    Vec3 lA, aA, lB, aB;
    float target = 0, lo = -FLT_MAX, hi = FLT_MAX, acc = 0, invEff = 0;
    int mf = -1; // manifold index for friction rows (limit = mu * sum of normal impulses)
    RowKind kind = kRowGeneric;
    int mpt = 0;
    bool tangent = false; // one of a manifold's two tangential friction rows
};

struct World::Impl {
    World* w;
    std::unordered_map<int, std::vector<Shape*>> grid;
    std::vector<Shape*> dynShapes;
    std::vector<Manifold> manifolds;
    std::vector<Row> rows;
    int stampCounter = 0;
    float dt = 1.0f / 60.0f;
    std::map<ShapePair, Anchor> anchors;
    int solvePass = 0; // counts solver passes (up to 3 per step) for the anchors' "in contact last pass" test

    explicit Impl(World* world) : w(world) {}
    // world.cpp
    void gridInsert(Shape* s);
    void gridRemove(Shape* s);
    void updateShapeCache(Shape* s, bool sweep);
    void syncGridMembership(Body* b);
    // solver.cpp
    void buildPairs(std::vector<std::pair<Shape*, Shape*>>& out);
    bool acceptPair(const Shape* a, const Shape* b) const;
    int generate(Shape* a, Shape* b, float margin, Manifold& m);
    void addContactRows();
    void postSolveAnchors();
    void dropAnchors(const Shape* s);
    void addJointRows();
    void addMotionRows();
    void solve(int iterations);
    bool processBreakage();
    void integratePositions();
    void deactivate();
};

inline bool aabbOverlap(const Shape* a, const Shape* b) {
    return a->aabbMin.x <= b->aabbMax.x && a->aabbMax.x >= b->aabbMin.x && a->aabbMin.y <= b->aabbMax.y &&
           a->aabbMax.y >= b->aabbMin.y && a->aabbMin.z <= b->aabbMax.z && a->aabbMax.z >= b->aabbMin.z;
}

} // namespace sh
