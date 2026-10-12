#include "physics/world.h"
#include <algorithm>
#include "physics/world_impl.h"

namespace sh {

World::World() : d(new Impl(this)) {
    ground_ = createBody(Vec3(), Quat());
    ground_->dynamic = false;
}
World::~World() = default;

// ---------------------------------------------------------------- bodies & shapes

Body* World::createBody(const Vec3& pos, const Quat& rot) {
    bodies_.emplace_back(new Body);
    Body* b = bodies_.back().get();
    b->pos = pos;
    b->rot = rot;
    b->prevPos = pos;
    b->prevRot = rot;
    return b;
}

Body* World::cloneWithoutShapes(const Body* src) {
    Body* b = createBody(src->pos, src->rot);
    b->vel = src->vel;
    b->angVel = src->angVel;
    b->dynamic = true;
    b->brokenFlag = true;
    b->age = src->age;
    b->owner = src->owner;
    b->splitProcessed = src->splitProcessed;
    b->active = true;
    b->ballGhost = 2;
    b->splashTime = src->splashTime; // shards inherit the splash cooldown
    return b;
}

void World::clearAll() {
    joints_.clear();
    bodies_.clear();
    d->grid.clear();
    d->anchors.clear();
    ground_ = createBody(Vec3(), Quat());
    ground_->dynamic = false;
    brokenBodyCount = 0;
}

void World::destroyBody(Body* b) {
    if (!b) return;
    for (auto& s : b->shapes) {
        d->gridRemove(s.get());
        // a reused address must not inherit an anchor
        d->dropAnchors(s.get());
    }
    for (Joint* j : std::vector<Joint*>(b->joints)) destroyJoint(j);
    auto it =
        std::find_if(bodies_.begin(), bodies_.end(), [&](const std::unique_ptr<Body>& p) { return p.get() == b; });
    if (it != bodies_.end()) {
        *it = std::move(bodies_.back());
        bodies_.pop_back();
    }
}

void World::Impl::gridInsert(Shape* s) {
    if (s->inGrid) return;
    int c0 = (int)std::floor(s->aabbMin.z / kCellSize), c1 = (int)std::floor(s->aabbMax.z / kCellSize);
    c1 = std::min(c1, c0 + 64);
    for (int c = c0; c <= c1; c++) grid[c].push_back(s);
    s->gridC0 = c0;
    s->gridC1 = c1;
    s->inGrid = true;
}

void World::Impl::gridRemove(Shape* s) {
    if (!s->inGrid) return;
    for (int c = s->gridC0; c <= s->gridC1; c++) {
        auto it = grid.find(c);
        if (it == grid.end()) continue;
        auto& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), s), v.end());
        if (v.empty()) grid.erase(it);
    }
    s->inGrid = false;
}

void World::Impl::updateShapeCache(Shape* s, bool sweep) {
    Body* b = s->body;
    const Polyhedron& p = s->poly;
    Mat3 R = Mat3::fromQuat(b->rot);
    s->wv.resize(p.v.size());
    Vec3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    if (b->isBall) {
        Vec3 c = b->worldCom();
        mn = c - Vec3(kBallRadius, kBallRadius, kBallRadius);
        mx = c + Vec3(kBallRadius, kBallRadius, kBallRadius);
    } else {
        for (size_t i = 0; i < p.v.size(); i++) {
            Vec3 q = b->pos + R * p.v[i];
            s->wv[i] = q;
            for (int k = 0; k < 3; k++) {
                mn[k] = std::min(mn[k], q[k]);
                mx[k] = std::max(mx[k], q[k]);
            }
        }
        s->wn.resize(p.f.size());
        s->wd.resize(p.f.size());
        for (size_t i = 0; i < p.f.size(); i++) {
            s->wn[i] = R * p.f[i].n;
            s->wd[i] = dot(s->wn[i], s->wv[p.f[i].idx[0]]);
        }
    }
    if (sweep) {
        Vec3 dv = b->vel * dt;
        for (int k = 0; k < 3; k++) {
            if (dv[k] < 0) mn[k] += dv[k];
            else mx[k] += dv[k];
        }
    }
    s->aabbMin = mn - Vec3(0.02f, 0.02f, 0.02f);
    s->aabbMax = mx + Vec3(0.02f, 0.02f, 0.02f);
}

void World::Impl::syncGridMembership(Body* b) {
    for (auto& sp : b->shapes) {
        Shape* s = sp.get();
        if (!b->dynamic) {
            if (!s->inGrid) {
                updateShapeCache(s, false);
                gridInsert(s);
            }
        } else if (s->inGrid) gridRemove(s);
    }
}

static void computeEdgeDirs(Shape* s) {
    s->edgeDirs.clear();
    for (auto& e : s->poly.edges) {
        Vec3 dir = normalize(s->poly.v[e.second] - s->poly.v[e.first]);
        bool dup = false;
        for (auto& q : s->edgeDirs)
            if (std::fabs(dot(q, dir)) > 0.9995f) {
                dup = true;
                break;
            }
        if (!dup) s->edgeDirs.push_back(dir);
    }
}

// Diagonalizes a symmetric 3x3 matrix in place by cyclic Jacobi rotations, at most 16 sweeps. Stops once
// offDiag(a) < offEps and skips pairs with |a[p][q]| < skipEps; each caller keeps its own precision and tests.
template <class T, class OffDiag> static void jacobiSweeps(T a[3][3], OffDiag offDiag, T offEps, T skipEps) {
    for (int sweep = 0; sweep < 16; sweep++) {
        if (offDiag(a) < offEps) break;
        for (int p = 0; p < 2; p++)
            for (int q = p + 1; q < 3; q++) {
                T apq = a[p][q];
                if (std::fabs(apq) < skipEps) continue;
                T th = (a[q][q] - a[p][p]) / (2 * apq);
                T t = (th >= 0 ? T(1) : T(-1)) / (std::fabs(th) + std::sqrt(th * th + 1));
                T c = 1 / std::sqrt(t * t + 1), sn = t * c;
                for (int k = 0; k < 3; k++) { // A = J^T A J (rotation in the p,q plane)
                    T akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq;
                    a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 3; k++) {
                    T apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk;
                    a[q][k] = sn * apk + c * aqk;
                }
            }
    }
}

// Thin-pane test. The principal moments give the sides of the box with the same inertia,
// side_i^2 = 6*(d_j + d_k - d_i)/m. A pane has exactly one side below 0.15 and the other two above 0.5,
// so window panes pass and small shards don't.
static bool plateFromInertia(float m, const Mat3& I) {
    if (!(m > 0)) return false;
    double a[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) a[i][j] = I.m[i * 3 + j];
    auto sumSq = [](const double(*x)[3]) { return x[0][1] * x[0][1] + x[0][2] * x[0][2] + x[1][2] * x[1][2]; };
    jacobiSweeps(a, sumSq, 1e-24, 1e-30);
    float d[3] = {(float)a[0][0], (float)a[1][1], (float)a[2][2]}, side[3];
    for (int i = 0; i < 3; i++)
        side[i] = std::sqrt(std::max(0.0f, 6.0f * (d[(i + 1) % 3] + d[(i + 2) % 3] - d[i]) / m));
    int thin = 0, wide = 0;
    for (float sd : side) {
        if (sd < 0.15f) thin++;
        else if (sd > 0.5f) wide++;
    }
    return thin == 1 && wide == 2;
}

Shape* World::addPoly(Body* b, Polyhedron p, const Material& m, Shape* origin) {
    if (!p.valid()) return nullptr;
    auto s = std::make_unique<Shape>();
    s->body = b;
    s->poly = std::move(p);
    s->mat = m;
    s->origin = origin;
    Shape* raw = s.get();
    if (!raw->origin) raw->origin = raw;
    raw->volume = raw->poly.volume();
    computeEdgeDirs(raw);
    b->shapes.push_back(std::move(s));
    return raw;
}

static Polyhedron placed(Polyhedron p, const Vec3& offset, const Quat& rot) {
    p.transform(Vec3(1, 1, 1), rot, offset);
    return p;
}

Shape* World::addBox(Body* b, const Vec3& half, const Vec3& offset, const Quat& rot, const Material& m, float chamfer) {
    Polyhedron p =
        placed(Polyhedron::box(Vec3(std::fabs(half.x), std::fabs(half.y), std::fabs(half.z)), chamfer), offset, rot);
    bool fast = !b->dynamic && !b->shapes.empty() && m.isStatic == 1;
    Shape* s = addPoly(b, std::move(p), m);
    if (s) {
        s->chamfer = chamfer;
        if (fast) addStaticFast(b, s);
        else computeMass(b);
    }
    return s;
}

void World::addStaticFast(Body* b, Shape* s) {
    d->updateShapeCache(s, false);
    s->volume = s->poly.volume();
    if (s->mat.forceLimit < FLT_MAX) {
        float m;
        Vec3 c;
        Mat3 I;
        s->poly.massProps(s->mat.density, m, c, I);
        s->plate = plateFromInertia(m, I);
    }
    d->gridInsert(s);
}

void World::removeShape(Shape* s) {
    if (!s) return;
    Body* b = s->body;
    d->gridRemove(s);
    d->dropAnchors(s);
    b->shapes.erase(std::remove_if(b->shapes.begin(), b->shapes.end(),
                                   [&](const std::unique_ptr<Shape>& p) { return p.get() == s; }),
                    b->shapes.end());
    computeMass(b);
}

void World::clearShapes(Body* b) {
    for (auto& s : b->shapes) {
        d->gridRemove(s.get());
        d->dropAnchors(s.get());
    }
    b->shapes.clear();
}

void World::refreshShapes(Body* b) {
    for (auto& sp : b->shapes) {
        Shape* s = sp.get();
        computeEdgeDirs(s);
        s->volume = s->poly.volume();
    }
    computeMass(b);
}

void Body::computeMassProperties() {
    // Aggregates per-shape polyhedral mass properties (bodies are in the entity frame).
    float M = 0;
    Vec3 com;
    struct Part {
        float m;
        Vec3 c;
        Mat3 I;
    };
    std::vector<Part> parts;
    bool anyStatic1 = false, anyStatic2 = false;
    for (auto& sp : shapes) {
        Part pt;
        sp->poly.massProps(sp->mat.density, pt.m, pt.c, pt.I);
        sp->plate = plateFromInertia(pt.m, pt.I);
        if (pt.m <= 0) continue;
        parts.push_back(pt);
        M += pt.m;
        com += pt.c * pt.m;
        if (sp->mat.isStatic == 1) anyStatic1 = true;
        if (sp->mat.isStatic == 2) anyStatic2 = true;
    }
    if (M > 0) com = com / M;
    Mat3 I = Mat3::zero();
    for (auto& pt : parts) {
        Vec3 r = pt.c - com;
        float rr = dot(r, r);
        Mat3 par = Mat3::zero();
        par.m[0] = rr - r.x * r.x;
        par.m[1] = -r.x * r.y;
        par.m[2] = -r.x * r.z;
        par.m[3] = -r.y * r.x;
        par.m[4] = rr - r.y * r.y;
        par.m[5] = -r.y * r.z;
        par.m[6] = -r.z * r.x;
        par.m[7] = -r.z * r.y;
        par.m[8] = rr - r.z * r.z;
        I = I + pt.I + par * pt.m;
    }
    mass = M;
    dynamic = !anyStatic1 && M > 0;
    rotLocked = anyStatic2 && !anyStatic1;
    // Equivalent box for dynamic bodies: diagonalize I with Jacobi sweeps, s_k = 12*l_k/M, then the box sides are
    // a^2 = (s2-s0+s1)/2, b^2 = s0-s1+a^2, c^2 = s1-a^2, clamped at 0. The sides stay on the diagonal slots, so they
    // line up with the entity axes, which is how the soft-shadow footprint uses them.
    boxDims = Vec3();
    if (dynamic) {
        float A[3][3];
        memcpy(A, I.m, sizeof A);
        auto sumAbs = [](const float(*x)[3]) { return std::fabs(x[0][1]) + std::fabs(x[0][2]) + std::fabs(x[1][2]); };
        jacobiSweeps(A, sumAbs, 1e-4f, 1e-12f);
        float s0 = 12 * A[0][0] / M, s1 = 12 * A[1][1] / M, s2 = 12 * A[2][2] / M;
        float a2 = (s2 - s0 + s1) * 0.5f;
        boxDims = Vec3(std::sqrt(std::max(0.0f, a2)), std::sqrt(std::max(0.0f, s0 - s1 + a2)),
                       std::sqrt(std::max(0.0f, s1 - a2)));
    }
    if (!dynamic) {
        invMass = 0;
        invInertiaLocal = Mat3::zero();
        localCom = Vec3();
        vel = Vec3();
        angVel = Vec3();
    } else {
        invMass = 1.0f / M;
        localCom = com;
        invInertiaLocal = rotLocked ? Mat3::zero() : I.inverse();
    }
}

void World::computeMass(Body* b) {
    b->computeMassProperties();
    for (auto& s : b->shapes) {
        d->updateShapeCache(s.get(), false);
        s->volume = s->poly.volume();
    }
    // static: refresh grid entries with up-to-date AABBs
    if (!b->dynamic)
        for (auto& s : b->shapes) d->gridRemove(s.get());
    d->syncGridMembership(b);
}

void World::setTransform(Body* b, const Vec3& pos, const Quat& rot) {
    b->pos = pos;
    b->rot = rot.normalized();
    b->prevPos = b->pos;
    b->prevRot = b->rot; // teleport: do not interpolate across it
    for (auto& s : b->shapes) {
        if (!b->dynamic) d->gridRemove(s.get());
        d->updateShapeCache(s.get(), false);
    }
    if (!b->dynamic)
        for (auto& s : b->shapes) d->gridInsert(s.get());
}

// ---------------------------------------------------------------- joints

Joint* World::createJoint(JointType t, Body* a, Body* b) {
    joints_.emplace_back(new Joint);
    Joint* j = joints_.back().get();
    j->type = t;
    j->a = a;
    j->b = b;
    j->noCollide = (t != JointType::Rope);
    a->joints.push_back(j);
    if (b != a) b->joints.push_back(j);
    return j;
}

void World::destroyJoint(Joint* j) {
    for (Body* b : {j->a, j->b})
        if (b) b->joints.erase(std::remove(b->joints.begin(), b->joints.end(), j), b->joints.end());
    auto it =
        std::find_if(joints_.begin(), joints_.end(), [&](const std::unique_ptr<Joint>& p) { return p.get() == j; });
    if (it != joints_.end()) {
        *it = std::move(joints_.back());
        joints_.pop_back();
    }
}

bool World::connected(Body* a, Body* b) const {
    if (a == b) return true;
    std::vector<Body*> stack{a}, seen{a};
    while (!stack.empty()) {
        Body* x = stack.back();
        stack.pop_back();
        for (Joint* j : x->joints) {
            Body* o = j->a == x ? j->b : j->a;
            if (o == b) return true;
            if (std::find(seen.begin(), seen.end(), o) == seen.end()) {
                seen.push_back(o);
                stack.push_back(o);
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------- queries

// Like the original, a ray only hits when it really enters a shape. Touching or grazing it (starting on the surface
// and leaving, or running along a face or edge) is a miss, and the start only counts as inside when strictly inside.
static constexpr float kRayTouch = 1e-4f;

static bool rayPoly(const Shape* s, const Vec3& o, const Vec3& dir, float maxT, float& tHit, Vec3& nHit) {
    // entry/exit of the whole line; t1 is NOT cut at maxT so the touch test sees the full chord
    float t0 = -FLT_MAX, t1 = FLT_MAX;
    Vec3 n0 = dir;
    bool inside = true;
    for (size_t i = 0; i < s->wn.size(); i++) {
        float denom = dot(s->wn[i], dir), dist = dot(s->wn[i], o) - s->wd[i];
        if (dist > -kRayTouch) inside = false; // on the surface is not inside
        if (std::fabs(denom) < 1e-12f) {
            if (dist > 0) return false;
            continue;
        }
        float t = -dist / denom;
        if (denom < 0) {
            if (t > t0) {
                t0 = t;
                n0 = s->wn[i];
            }
        } else t1 = std::min(t1, t);
    }
    // starts inside: hit at the start, normal along the ray
    if (inside) {
        tHit = 0;
        nHit = dir;
        return true;
    }
    const float a = std::max(t0, 0.0f);
    if (t1 == FLT_MAX || a > t1 || a > maxT) return false;
    // forward chord through the whole shape, so touching or grazing is a miss
    const Vec3 mid = o + dir * (0.5f * (a + t1));
    for (size_t i = 0; i < s->wn.size(); i++)
        if (dot(s->wn[i], mid) - s->wd[i] > -kRayTouch) return false;
    tHit = a;
    nHit = t0 >= 0 ? n0 : dir; // starts within kRayTouch heading in: t = 0, normal along the ray
    return true;
}

RayHit World::raycast(const Vec3& from, const Vec3& to, int mask, const Obstacle* onlyOwner) const {
    RayHit best;
    Vec3 dir = to - from;
    float len = length(dir);
    if (len < 1e-9f) return best;
    Vec3 u = dir / len;
    best.t = len;
    auto test = [&](Shape* s) {
        if (!(s->mat.category & mask)) return;
        if (onlyOwner && s->body->owner != onlyOwner) return;
        Vec3 mn = s->aabbMin, mx = s->aabbMax;
        // quick slab on AABB
        float a0 = 0, a1 = best.t;
        for (int k = 0; k < 3; k++) {
            if (std::fabs(u[k]) < 1e-12f) {
                if (from[k] < mn[k] || from[k] > mx[k]) return;
                continue;
            }
            float ta = (mn[k] - from[k]) / u[k], tb = (mx[k] - from[k]) / u[k];
            if (ta > tb) std::swap(ta, tb);
            a0 = std::max(a0, ta);
            a1 = std::min(a1, tb);
            if (a0 > a1) return;
        }
        float t;
        Vec3 n;
        if (s->body->isBall) {
            Vec3 c = s->body->worldCom(), oc = from - c;
            float r = kRayBallRadius, b = dot(oc, u), oo = dot(oc, oc), disc = b * b - (oo - r * r);
            const float in2 = (r - kRayTouch) * (r - kRayTouch);
            if (oo < in2) { // starts inside the ball
                t = 0;
                n = u;
            } else { // same touch rule as rayPoly: the forward chord must reach more than kRayTouch inside
                if (disc <= 0) return;
                float sq = std::sqrt(disc), e0 = -b - sq, e1 = -b + sq;
                t = std::max(e0, 0.0f);
                if (e1 <= t || lengthSq(oc + u * (0.5f * (t + e1))) >= in2) return;
                n = e0 >= 0 ? normalize(from + u * t - c) : u;
            }
        } else if (!rayPoly(s, from, u, best.t, t, n)) return;
        if (t < best.t) {
            best.hit = true;
            best.t = t;
            best.normal = n;
            best.shape = s;
        }
    };
    float zmin = std::min(from.z, to.z), zmax = std::max(from.z, to.z);
    for (int c = (int)std::floor(zmin / kCellSize) - 1; c <= (int)std::floor(zmax / kCellSize) + 1; c++) {
        auto it = d->grid.find(c);
        if (it == d->grid.end()) continue;
        for (Shape* s : it->second) test(s);
    }
    for (auto& bp : bodies_) {
        Body* b = bp.get();
        if (!b->dynamic || !b->active) continue;
        for (auto& sp : b->shapes) test(sp.get());
    }
    if (best.hit) best.point = from + u * best.t;
    best.t = best.hit ? best.t / len : 1.0f;
    return best;
}

bool World::overlapBox(const Vec3& c, const Vec3& h) const {
    Convex box;
    box.type = ShapeType::Box;
    box.half = h;
    box.pos = c;
    Vec3 mn = c - h, mx = c + h;
    auto test = [&](const Shape* s) {
        if (s->aabbMax.x < mn.x || s->aabbMin.x > mx.x || s->aabbMax.y < mn.y || s->aabbMin.y > mx.y ||
            s->aabbMax.z < mn.z || s->aabbMin.z > mx.z)
            return false;
        Convex hull;
        hull.type = ShapeType::Hull;
        hull.verts = s->wv;
        Contact ct;
        return collide(hull, box, ct);
    };
    for (int cz = (int)std::floor(mn.z / kCellSize); cz <= (int)std::floor(mx.z / kCellSize); cz++) {
        auto it = d->grid.find(cz);
        if (it == d->grid.end()) continue;
        for (Shape* s : it->second)
            if (test(s)) return true;
    }
    for (auto& bp : bodies_)
        if (bp->dynamic && bp->active && !bp->isBall)
            for (auto& sp : bp->shapes)
                if (test(sp.get())) return true;
    return false;
}

bool World::pointInBody(const Body* b, const Vec3& w) const {
    Vec3 l = b->pointToLocal(w);
    for (auto& s : b->shapes) {
        bool in = true;
        for (auto& f : s->poly.f)
            if (dot(f.n, l) - f.d > 0) {
                in = false;
                break;
            }
        if (in) return true;
    }
    return false;
}

bool World::closestPoint(const Vec3& p, float maxDist, int mask, Vec3* outPt, Vec3* outN, Shape** outShape) const {
    float best = maxDist;
    bool found = false;
    auto test = [&](Shape* s) {
        if (!(s->mat.category & mask)) return;
        if (p.x < s->aabbMin.x - maxDist || p.x > s->aabbMax.x + maxDist || p.y < s->aabbMin.y - maxDist ||
            p.y > s->aabbMax.y + maxDist)
            return;
        Vec3 l = s->body->pointToLocal(p), q;
        float dd = s->poly.distanceTo(l, &q);
        if (dd < best) {
            best = dd;
            found = true;
            if (outPt) *outPt = s->body->pointToWorld(q);
            if (outN) *outN = dd > 1e-6f ? normalize(s->body->rot.rotate(l - q)) : Vec3(0, 1, 0);
            if (outShape) *outShape = s;
        }
    };
    for (int cz = (int)std::floor((p.z - maxDist) / kCellSize); cz <= (int)std::floor((p.z + maxDist) / kCellSize);
         cz++) {
        auto it = d->grid.find(cz);
        if (it == d->grid.end()) continue;
        for (Shape* s : it->second) test(s);
    }
    for (auto& bp : bodies_)
        if (bp->dynamic && bp->active)
            for (auto& sp : bp->shapes) test(sp.get());
    return found;
}

void World::collect(const Vec3& mn, const Vec3& mx, int mask, std::vector<Body*>& out) const {
    auto add = [&](Shape* s) {
        if (!(s->mat.category & mask)) return;
        if (s->aabbMax.x < mn.x || s->aabbMin.x > mx.x || s->aabbMax.y < mn.y || s->aabbMin.y > mx.y ||
            s->aabbMax.z < mn.z || s->aabbMin.z > mx.z)
            return;
        if (std::find(out.begin(), out.end(), s->body) == out.end()) out.push_back(s->body);
    };
    for (int cz = (int)std::floor(mn.z / kCellSize); cz <= (int)std::floor(mx.z / kCellSize); cz++) {
        auto it = d->grid.find(cz);
        if (it == d->grid.end()) continue;
        for (Shape* s : it->second) add(s);
    }
    for (auto& bp : bodies_)
        if (bp->dynamic && bp->active)
            for (auto& sp : bp->shapes) add(sp.get());
}

} // namespace sh
