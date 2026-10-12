// Rigid-body world: bodies made of convex polyhedra, joints, motors, speculative contacts and fracture.
// Fixed 1/60 s tick, 4 solver iterations, contact impulses capped by each material's force limit.
#pragma once
#include <cfloat>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/math.h"
#include "physics/poly.h"

namespace sh {

enum MaterialType { kMaterialPlain, kMaterialMarble, kMaterialGlass, kMaterialMetal, kMaterialRoom };

// collision category bits (Material::category); obstacle scripts use the low bits for their own purposes
constexpr uint8_t kCategoryBall = 0x40, kCategoryRoom = 0x80;
constexpr uint8_t kMaskNoRoom = 0x7f, kMaskNoBall = 0xbf;

// nitro balls are armed when thrown and explode at the end of the step in which they touch something
enum BallState { kBallNormal, kBallNitro, kBallExploding };

// Per-shape material: builder state plus the matching materials.xml entry.
struct Material {
    float density = 1.0f, friction = 1.0f, restitution = 0.0f;
    float forceLimit = FLT_MAX;
    float roundness = 0.0f;
    int type = kMaterialPlain; // MaterialType
    float color[4] = {0, 0, 0, 0};
    uint8_t category = 1, mask = 0xff, isStatic = 0;
    float texOffset[2] = {0, 0}, texScale[2] = {1, 1};
    bool texEnabled = false, hideEdges = false;
};

struct Body;
class Obstacle;

struct Shape {
    Body* body = nullptr;
    Shape* origin = nullptr; // root shape this one was split from (self for originals)
    Polyhedron poly;         // geometry in the body (entity) frame
    Material mat;
    bool broken = false; // produced by a split
    bool plate = false;  // thin pane: equivalent box has one side < 0.15 and two > 0.5 (picks the glass_pane sound)
    float chamfer = 0;
    float volume = 0;
    // unbreakable cylinder (roundness 1): body-frame axis, centre and half length, so the renderer can
    // give it flat caps and radial side normals
    bool cyl = false;
    Vec3 cylAxis, cylCentre;
    float cylHalfLen = 0;
    // --- cached world data, refreshed by World ---
    std::vector<Vec3> wv;       // world vertices
    std::vector<Vec3> wn;       // world face normals
    std::vector<float> wd;      // world face plane offsets
    std::vector<Vec3> edgeDirs; // unique edge directions (local)
    Vec3 aabbMin, aabbMax;      // padded + swept
    int stamp = 0;
    bool inGrid = false;
    int gridC0 = 0, gridC1 = -1;
};

struct Motion {
    Vec3 pos;
    Quat rot;
    float linForce = 0, linMaxSpeed = 0, angForce = 0, angMaxSpeed = 0;
};

struct Joint;

struct Body {
    Vec3 pos; // entity transform
    Quat rot;
    Vec3 vel, angVel; // velocity of the centre of mass
    float mass = 0, invMass = 0;
    Vec3 localCom; // centre of mass in entity frame
    Mat3 invInertiaLocal = Mat3::zero();
    bool dynamic = true;    // false: static (any shape static==1)
    bool rotLocked = false; // any shape static==2: finite mass, zero inverse inertia
    bool isBall = false;    // collides as a sphere of radius kBallRadius (raycasts use kRayBallRadius)
    int ballState = kBallNormal;
    bool hitBreakable = false; // ball broke something / pressed a button / took a powerup (sticky)
    bool registered = false;   // ball already counted in the run statistics
    bool brokenFlag = false;   // has been broken at least once (shards inherit it)
    bool active = true;
    bool splitProcessed = false;
    int ballGhost = 0; // fresh shard bodies ignore balls for a couple of ticks (limits cascade slowdown)
    int id = -1;       // ball index / entity id
    int lastTouched = -1;
    float age = 0;
    std::vector<std::unique_ptr<Shape>> shapes;
    std::unique_ptr<Motion> motion;
    std::vector<Joint*> joints;
    Obstacle* owner = nullptr;
    float prevSpeedSq = 0; // for impact sounds (speed drop per tick)
    float splashTime = 0;  // level time it last crossed a water surface
    Vec3 boxDims;          // full sides of the box with the same principal moments (0 for static bodies)
    bool touching = false; // had a loaded contact this step (drives the rolling/sliding loops)
    Vec3 prevPos;          // pose at the start of the last tick (render interpolation)
    Quat prevRot;
    // transient solver data (valid during step)
    Mat3 invIw = Mat3::zero();
    float invMEff = 0;

    Vec3 worldCom() const { return pos + rot.rotate(localCom); }
    Vec3 pointToWorld(const Vec3& p) const { return pos + rot.rotate(p); }
    Vec3 pointToLocal(const Vec3& w) const { return rot.conj().rotate(w - pos); }
    void computeMassProperties();
};

enum class JointType { Ball = 1, Hinge = 2, Slider = 3, Rope = 4 };

struct Joint {
    JointType type = JointType::Ball;
    Body *a = nullptr, *b = nullptr;
    Vec3 anchorA, anchorB; // entity-local anchors
    Vec3 axisA, axisB;     // entity-local axes (hinge/slider)
    float ropeLength = 0;
    bool noCollide = true; // jointed bodies do not collide (false for ropes)
    float motorVel = 0, motorForce = 0;
};

struct RayHit {
    bool hit = false;
    Vec3 point, normal;
    float t = 0;
    Shape* shape = nullptr;
};

struct BreakEvent {
    Body* body; // body that was broken (before split)
    Vec3 point, normal;
    Vec3 bodyVel;
    float color[4];
    bool firstBreak = false;
    bool firstInPass = false; // first queued body of this processBreakage pass
    bool byBall = false;
};

class World {
public:
    World();
    ~World();

    Vec3 gravity{0, -10, 0};
    float cullZ = 1e30f; // bodies fully behind (minZ > cullZ) with no motion/joints are deactivated

    Body* createBody(const Vec3& pos, const Quat& rot);
    void destroyBody(Body* b);
    void clearAll(); // drop every body/joint, keep an empty static ground
    Body* cloneWithoutShapes(const Body* b);
    Shape* addBox(Body* b, const Vec3& half, const Vec3& offset, const Quat& rot, const Material& m, float chamfer);
    Shape* addPoly(Body* b, Polyhedron p, const Material& m, Shape* origin = nullptr);
    void removeShape(Shape* s); // frees the shape and recomputes mass
    void clearShapes(Body* b);
    void setTransform(Body* b, const Vec3& pos, const Quat& rot);
    void refreshShapes(Body* b);           // after shape geometry edits
    void computeMass(Body* b);             // recompute + refresh grid
    void addStaticFast(Body* b, Shape* s); // adding a static-1 shape to an already static body

    Joint* createJoint(JointType t, Body* a, Body* b);
    void destroyJoint(Joint* j);
    bool connected(Body* a, Body* b) const;

    // One game tick of `dt` seconds (the original always uses 1/60).
    void step(float dt);

    RayHit raycast(const Vec3& from, const Vec3& to, int mask, const Obstacle* onlyOwner = nullptr) const;
    bool overlapBox(const Vec3& center, const Vec3& half) const;
    bool pointInBody(const Body* b, const Vec3& worldPoint) const;
    bool closestPoint(const Vec3& p, float maxDist, int mask, Vec3* outPt, Vec3* outN, Shape** outShape) const;
    void collect(const Vec3& mn, const Vec3& mx, int mask, std::vector<Body*>& out) const;

    // Breaks `b` around a hit point: cell fracture with half-size `cell` (0.25 hits, 0.8 explosions).
    void breakBody(Body* b, const Vec3& worldPoint, const Vec3& worldNormal, float limit, float cell);

    // Game hooks
    std::function<void(const BreakEvent&)> onBreak;
    // called once per breakage pass, before anything splits, so only one shatter sound plays. Gets the first
    // queued body's position and whether any overloaded shape is a thin pane.
    std::function<void(const Vec3& pos, bool plate)> onBreakSound;

    const std::vector<std::unique_ptr<Body>>& bodies() const { return bodies_; }
    Body* ground() const { return ground_; }
    const std::vector<std::unique_ptr<Joint>>& joints() const { return joints_; }
    int brokenBodyCount = 0; // bodies broken for the first time this level

private:
    Body* splitByRandomPlane(Body* src, const std::vector<Shape*>& shapes, const Vec3& Pl);

    struct Impl;
    std::unique_ptr<Impl> d;
    std::vector<std::unique_ptr<Body>> bodies_;
    std::vector<std::unique_ptr<Joint>> joints_;
    Body* ground_ = nullptr;
    std::mt19937 rng_{12345}; // fixed seed: fracture planes are deterministic per World
    friend struct Impl;
};

} // namespace sh
