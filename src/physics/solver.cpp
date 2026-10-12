// Broadphase, narrowphase (SAT + clipping, sphere for balls), constraint rows, PGS solver, World::step.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "physics/world.h"
#include "physics/world_impl.h"

namespace sh {

static constexpr float kSkin = 0.02f;
static constexpr float kErp = 0.8f;
static constexpr float kMaxPush = 1.0f;

// ------------------------------------------------------------------ broadphase

bool World::Impl::acceptPair(const Shape* a, const Shape* b) const {
    const Body *A = a->body, *B = b->body;
    if (A == B || !A->active || !B->active) return false;
    if (!A->dynamic && !B->dynamic) return false;
    if (!(a->mat.category & b->mat.mask) || !(b->mat.category & a->mat.mask)) return false;
    if (A->isBall && B->isBall && A->age < 0.5f && B->age < 0.5f) return false;
    if ((A->isBall && B->ballGhost > 0) || (B->isBall && A->ballGhost > 0)) return false;
    for (const Joint* j : A->joints)
        if (j->noCollide && ((j->a == A && j->b == B) || (j->a == B && j->b == A))) return false;
    return true;
}

void World::Impl::buildPairs(std::vector<std::pair<Shape*, Shape*>>& out) {
    dynShapes.clear();
    for (auto& bp : w->bodies_) {
        Body* b = bp.get();
        if (!b->dynamic || !b->active) continue;
        for (auto& sp : b->shapes) {
            updateShapeCache(sp.get(), true);
            dynShapes.push_back(sp.get());
        }
    }
    std::sort(dynShapes.begin(), dynShapes.end(),
              [](const Shape* a, const Shape* b) { return a->aabbMin.z < b->aabbMin.z; });
    // dynamic vs dynamic sweep along z
    for (size_t i = 0; i < dynShapes.size(); i++)
        for (size_t j = i + 1; j < dynShapes.size() && dynShapes[j]->aabbMin.z <= dynShapes[i]->aabbMax.z; j++)
            if (aabbOverlap(dynShapes[i], dynShapes[j]) && acceptPair(dynShapes[i], dynShapes[j]))
                out.emplace_back(dynShapes[i], dynShapes[j]);
    // dynamic vs static grid
    for (Shape* a : dynShapes) {
        int stamp = ++stampCounter;
        int c0 = (int)std::floor(a->aabbMin.z / kCellSize), c1 = (int)std::floor(a->aabbMax.z / kCellSize);
        for (int c = c0; c <= c1; c++) {
            auto it = grid.find(c);
            if (it == grid.end()) continue;
            for (Shape* s : it->second) {
                if (s->stamp == stamp) continue;
                s->stamp = stamp;
                if (aabbOverlap(a, s) && acceptPair(a, s)) out.emplace_back(a, s);
            }
        }
    }
}

// ------------------------------------------------------------------ narrowphase

namespace {

struct Pt {
    Vec3 p;
    float sep;
};

float supportMin(const Shape* s, const Vec3& n) {
    float m = 1e30f;
    for (const Vec3& v : s->wv) m = std::min(m, dot(n, v));
    return m;
}
float supportMax(const Shape* s, const Vec3& n) {
    float m = -1e30f;
    for (const Vec3& v : s->wv) m = std::max(m, dot(n, v));
    return m;
}

void clipPolygon(std::vector<Vec3>& poly, const Vec3& n, const Vec3& pt) { // keep dot(n, x - pt) >= 0
    std::vector<Vec3> out;
    size_t m = poly.size();
    for (size_t i = 0; i < m; i++) {
        const Vec3 &a = poly[i], &b = poly[(i + 1) % m];
        float da = dot(n, a - pt), db = dot(n, b - pt);
        if (da >= 0) out.push_back(a);
        if ((da > 0 && db < 0) || (da < 0 && db > 0)) out.push_back(a + (b - a) * (da / (da - db)));
    }
    poly.swap(out);
}

void closestSegSeg(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, Vec3& c1, Vec3& c2) {
    Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r), s, t;
    if (a <= 1e-12f && e <= 1e-12f) {
        c1 = p1;
        c2 = p2;
        return;
    }
    if (a <= 1e-12f) {
        s = 0;
        t = clampf(f / e, 0, 1);
    } else {
        float c = dot(d1, r);
        if (e <= 1e-12f) {
            t = 0;
            s = clampf(-c / a, 0, 1);
        } else {
            float b = dot(d1, d2), den = a * e - b * b;
            s = den > 1e-12f ? clampf((b * f - c * e) / den, 0, 1) : 0;
            t = (b * s + f) / e;
            if (t < 0) {
                t = 0;
                s = clampf(-c / a, 0, 1);
            } else if (t > 1) {
                t = 1;
                s = clampf((b - c) / a, 0, 1);
            }
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

int reduce(std::vector<Pt>& pts, Pt out[4]) {
    int n = (int)pts.size();
    if (n <= 4) {
        for (int i = 0; i < n; i++) out[i] = pts[i];
        return n;
    }
    int i0 = 0;
    for (int i = 1; i < n; i++)
        if (pts[i].sep < pts[i0].sep) i0 = i;
    int i1 = -1;
    float best = -1;
    for (int i = 0; i < n; i++) {
        float d = lengthSq(pts[i].p - pts[i0].p);
        if (d > best) {
            best = d;
            i1 = i;
        }
    }
    int i2 = -1;
    best = -1;
    for (int i = 0; i < n; i++) {
        float d = lengthSq(cross(pts[i].p - pts[i0].p, pts[i1].p - pts[i0].p));
        if (d > best) {
            best = d;
            i2 = i;
        }
    }
    int i3 = -1;
    best = -1;
    for (int i = 0; i < n; i++) {
        if (i == i0 || i == i1 || i == i2) continue;
        float d =
            std::min({lengthSq(pts[i].p - pts[i0].p), lengthSq(pts[i].p - pts[i1].p), lengthSq(pts[i].p - pts[i2].p)});
        if (d > best) {
            best = d;
            i3 = i;
        }
    }
    out[0] = pts[i0];
    out[1] = pts[i1];
    out[2] = pts[i2];
    int c = 3;
    if (i3 >= 0) out[c++] = pts[i3];
    return c;
}

// Ball centre c against a polyhedron, from the cached world planes and vertices: normal pointing at the ball,
// separation from the ball surface and the closest point on the shape.
void ballVsPoly(const Shape* other, const Vec3& c, float r, Vec3& nToBall, float& sep, Vec3& onOther) {
    float maxd = -1e30f;
    int mi = 0;
    for (size_t i = 0; i < other->wn.size(); i++) {
        float dd = dot(other->wn[i], c) - other->wd[i];
        if (dd > maxd) {
            maxd = dd;
            mi = (int)i;
        }
    }
    if (maxd <= 0) {
        nToBall = other->wn[mi];
        sep = maxd - r;
        onOther = c - nToBall * maxd;
        return;
    }
    // outside: closest point over faces (triangle fans in world space)
    float best = 1e30f;
    Vec3 bp;
    const Polyhedron& p = other->poly;
    for (size_t fi = 0; fi < p.f.size(); fi++) {
        // quick reject by plane distance
        float pd = dot(other->wn[fi], c) - other->wd[fi];
        if (pd * pd > best) continue;
        const auto& idx = p.f[fi].idx;
        for (size_t i = 1; i + 1 < idx.size(); i++) {
            Vec3 q = closestPointOnTriangle(c, other->wv[idx[0]], other->wv[idx[i]], other->wv[idx[i + 1]]);
            float dd = lengthSq(c - q);
            if (dd < best) {
                best = dd;
                bp = q;
            }
        }
    }
    float l = std::sqrt(best);
    nToBall = l > 1e-9f ? (c - bp) / l : other->wn[mi];
    sep = l - r;
    onOther = bp;
}

// Ball vs breakable: a speculative contact must only exist if the swept ball can actually reach the
// shape this tick; otherwise passing near a neighbouring shard would eat impulse (and trigger breakage).
bool sweptBallReaches(const Shape* ballS, const Shape* other, const Vec3& c, float r, float dt) {
    Vec3 vrel = ballS->body->vel - other->body->vel;
    float t0 = 0, t1 = 1.5f * dt; // sweep slightly longer than one tick
    for (size_t i = 0; i < other->wn.size(); i++) {
        float dist = dot(other->wn[i], c) - other->wd[i] - r;
        float den = dot(other->wn[i], vrel);
        if (std::fabs(den) < 1e-9f) {
            if (dist > 0) return false;
            continue;
        }
        float t = -dist / den;
        if (den < 0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    return true;
}

// Balls collide as spheres of radius kBallRadius: one contact point halfway between the surfaces.
int ballContact(const Shape* A, const Shape* B, float margin, float dt, Manifold& mf) {
    bool aBall = A->body->isBall;
    const Shape* ballS = aBall ? A : B;
    const Shape* other = aBall ? B : A;
    Vec3 c = ballS->body->worldCom();
    float r = kBallRadius;
    Vec3 nToBall; // points other -> ball
    float sep;
    Vec3 onOther;
    if (other->body->isBall) {
        Vec3 c2 = other->body->worldCom(), dv = c - c2;
        float l = length(dv);
        nToBall = l > 1e-9f ? dv / l : Vec3(0, 1, 0);
        sep = l - 2 * r;
        onOther = c2 + nToBall * r;
    } else ballVsPoly(other, c, r, nToBall, sep, onOther);
    if (sep >= margin) return 0;
    if (!other->body->isBall && (other->mat.forceLimit < FLT_MAX) && sep > kSkin &&
        !sweptBallReaches(ballS, other, c, r, dt))
        return 0;
    mf.n = aBall ? -nToBall : nToBall;
    mf.count = 1;
    mf.pts[0].sep = sep;
    mf.pts[0].pos = onOther + nToBall * (sep * 0.5f);
    return 1;
}

// SAT over the face normals of `ref`: deepest separation of `other` and the face giving it. False as soon as an
// axis separates the shapes by at least `margin`.
bool faceAxes(const Shape* ref, const Shape* other, float margin, float& sep, int& face) {
    sep = -1e30f;
    face = -1;
    for (size_t i = 0; i < ref->wn.size(); i++) {
        float s = supportMin(other, ref->wn[i]) - ref->wd[i];
        if (s > sep) {
            sep = s;
            face = (int)i;
        }
        if (sep >= margin) return false;
    }
    return true;
}

// SAT over the cross products of the edge directions, oriented from A to B. Keeps the deepest axis and the edge
// directions that made it; false when an axis separates the shapes by at least `margin`.
bool edgeAxes(const Shape* A, const Shape* B, float margin, float& sepE, Vec3& axisE, Vec3& eA, Vec3& eB) {
    const Body *ba = A->body, *bb = B->body;
    sepE = -1e30f;
    Vec3 cA = ba->worldCom(), cB = bb->worldCom();
    Mat3 Ra = Mat3::fromQuat(ba->rot), Rb = Mat3::fromQuat(bb->rot);
    for (const Vec3& la : A->edgeDirs) {
        Vec3 da = Ra * la;
        for (const Vec3& lb : B->edgeDirs) {
            Vec3 db = Rb * lb;
            Vec3 ax = cross(da, db);
            float l = length(ax);
            if (l < 1e-3f) continue;
            ax = ax / l;
            if (dot(ax, cB - cA) < 0) ax = -ax;
            float s = supportMin(B, ax) - supportMax(A, ax);
            if (s >= margin) return false;
            if (s > sepE) {
                sepE = s;
                axisE = ax;
                eA = da;
                eB = db;
            }
        }
    }
    return true;
}

// An edge of s lying in the support plane dot(axis, x) = level and parallel to dir.
bool findEdge(const Shape* s, const Vec3& axis, float level, const Vec3& dir, Vec3& p0, Vec3& p1) {
    for (auto& e : s->poly.edges) {
        const Vec3 &x = s->wv[e.first], &y = s->wv[e.second];
        float dx = dot(axis, x), dy = dot(axis, y);
        if (std::fabs(dx - level) > 2e-3f || std::fabs(dy - level) > 2e-3f) continue;
        Vec3 ed = normalize(y - x);
        if (length(cross(ed, dir)) > 0.02f) continue;
        p0 = x;
        p1 = y;
        return true;
    }
    return false;
}

// Edge-edge contact on the SAT edge axis: one point between the closest points of the two touching edges. False
// when either edge can't be found, so the caller falls back to a face contact.
bool edgeEdgeContact(const Shape* A, const Shape* B, float sepE, const Vec3& axisE, const Vec3& eA, const Vec3& eB,
                     Manifold& mf) {
    float topA = supportMax(A, axisE), botB = supportMin(B, axisE);
    Vec3 a0, a1, b0, b1;
    if (!findEdge(A, axisE, topA, eA, a0, a1) || !findEdge(B, axisE, botB, eB, b0, b1)) return false;
    Vec3 ca, cb;
    closestSegSeg(a0, a1, b0, b1, ca, cb);
    mf.n = axisE;
    mf.count = 1;
    mf.edgeContact = true;
    mf.pts[0].sep = sepE;
    mf.pts[0].pos = (ca + cb) * 0.5f;
    return true;
}

// Face contact: reference face rf on `ref`, incident face on `inc` (the one most opposed to the reference normal)
// clipped to the reference face's side planes. Keeps up to four points closer than `margin`.
int faceContact(const Shape* ref, const Shape* inc, int rf, bool refIsA, float margin, Manifold& mf) {
    Vec3 nR = ref->wn[rf];
    float dR = ref->wd[rf];
    int fi = 0;
    float mn = 1e30f;
    for (size_t i = 0; i < inc->wn.size(); i++) {
        float d = dot(inc->wn[i], nR);
        if (d < mn) {
            mn = d;
            fi = (int)i;
        }
    }
    std::vector<Vec3> poly;
    for (int idx : inc->poly.f[fi].idx) poly.push_back(inc->wv[idx]);
    const auto& rIdx = ref->poly.f[rf].idx;
    for (size_t i = 0; i < rIdx.size(); i++) {
        const Vec3& v0 = ref->wv[rIdx[i]];
        const Vec3& v1 = ref->wv[rIdx[(i + 1) % rIdx.size()]];
        Vec3 side = cross(nR, v1 - v0); // inward for CCW-from-outside faces
        clipPolygon(poly, normalize(side), v0);
        if (poly.empty()) break;
    }
    std::vector<Pt> pts;
    for (const Vec3& p : poly) {
        float s = dot(nR, p) - dR;
        if (s < margin) pts.push_back({p - nR * (s * 0.5f), s});
    }
    if (pts.empty()) return 0;
    Pt red[4];
    int n = reduce(pts, red);
    mf.n = refIsA ? nR : -nR;
    for (int i = 0; i < n; i++) {
        mf.pts[i].pos = red[i].p;
        mf.pts[i].sep = red[i].sep;
    }
    mf.count = n;
    return n;
}

// Polyhedron vs polyhedron: SAT over both face sets and the edge pairs, then an edge-edge or clipped face contact.
int polyContact(const Shape* A, const Shape* B, float margin, Manifold& mf) {
    float sepA, sepB;
    int fa, fb;
    if (!faceAxes(A, B, margin, sepA, fa) || !faceAxes(B, A, margin, sepB, fb)) return 0;
    float sepE;
    Vec3 axisE, eA, eB;
    if (!edgeAxes(A, B, margin, sepE, axisE, eA, eB)) return 0;
    bool refIsA = true;
    int rf = fa;
    float faceBest = sepA;
    if (sepB > sepA + 1e-3f) {
        refIsA = false;
        rf = fb;
        faceBest = sepB;
    }
    if (sepE > faceBest + 0.005f && sepE > -1e29f && edgeEdgeContact(A, B, sepE, axisE, eA, eB, mf)) return 1;
    return faceContact(refIsA ? A : B, refIsA ? B : A, rf, refIsA, margin, mf);
}

} // namespace

// Returns the number of contact points (0..4). Normal points from A to B.
int World::Impl::generate(Shape* A, Shape* B, float margin, Manifold& mf) {
    mf.a = A;
    mf.b = B;
    mf.count = 0;
    mf.edgeContact = false;
    if (A->body->isBall || B->body->isBall) return ballContact(A, B, margin, dt, mf);
    return polyContact(A, B, margin, mf);
}

// ------------------------------------------------------------------ constraint rows

namespace {

void finishRow(Row& r) {
    float e = 0;
    if (r.a) e += r.a->invMEff * dot(r.lA, r.lA) + dot(r.aA, r.a->invIw * r.aA);
    if (r.b) e += r.b->invMEff * dot(r.lB, r.lB) + dot(r.aB, r.b->invIw * r.aB);
    r.invEff = e > 1e-12f ? 1.0f / e : 0.0f;
}

inline float jv(const Row& r) {
    float s = 0;
    if (r.a) s += dot(r.lA, r.a->vel) + dot(r.aA, r.a->angVel);
    if (r.b) s += dot(r.lB, r.b->vel) + dot(r.aB, r.b->angVel);
    return s;
}

inline void applyImpulse(Row& r, float lambda) {
    if (r.a && r.a->dynamic) {
        r.a->vel += r.lA * (lambda * r.a->invMEff);
        r.a->angVel += (r.a->invIw * r.aA) * lambda;
    }
    if (r.b && r.b->dynamic) {
        r.b->vel += r.lB * (lambda * r.b->invMEff);
        r.b->angVel += (r.b->invIw * r.aB) * lambda;
    }
}

Row linearRow(Body* a, Body* b, const Vec3& axis, const Vec3& ra, const Vec3& rb, float target) {
    Row r;
    r.a = a;
    r.b = b;
    r.lA = -axis;
    r.aA = -cross(ra, axis);
    r.lB = axis;
    r.aB = cross(rb, axis);
    r.target = target;
    finishRow(r);
    return r;
}

Row angularRow(Body* a, Body* b, const Vec3& axis, float target) {
    Row r;
    r.a = a;
    r.b = b;
    r.aA = -axis;
    r.aB = axis;
    r.target = target;
    finishRow(r);
    return r;
}

} // namespace

void World::Impl::addJointRows() {
    const float dtl = dt;
    for (auto& jp : w->joints_) {
        Joint* j = jp.get();
        Body *A = j->a, *B = j->b;
        if (!A->active && !B->active) continue;
        Vec3 pA = A->pointToWorld(j->anchorA), pB = B->pointToWorld(j->anchorB);
        Vec3 rA = pA - A->worldCom(), rB = pB - B->worldCom();
        Body *ra = A->dynamic ? A : nullptr, *rb = B->dynamic ? B : nullptr;
        if (!ra && !rb) continue;
        if (j->type == JointType::Rope) {
            Vec3 dv = pB - pA;
            float dist = length(dv), excess = dist - j->ropeLength;
            if (excess > 0 && dist > 1e-6f) {
                Vec3 n = dv / dist;
                Row r = linearRow(ra, rb, n, rA, rB, -excess);
                float mA = (ra && ra->invMass > 0) ? std::min(1.0f / ra->invMass, 10000.0f) : 10000.0f;
                float mB = (rb && rb->invMass > 0) ? std::min(1.0f / rb->invMass, 10000.0f) : 10000.0f;
                r.hi = 0;
                r.lo = -excess * std::min(mA, mB);
                rows.push_back(r);
            }
            continue;
        }
        Vec3 axA = A->rot.rotate(j->axisA), axB = B->rot.rotate(j->axisB);
        std::vector<Vec3> lin;
        if (j->type == JointType::Slider) {
            Vec3 ax = normalize(axA);
            Vec3 t1 =
                std::fabs(ax.x) < 0.9f ? normalize(cross(ax, Vec3(1, 0, 0))) : normalize(cross(ax, Vec3(0, 1, 0)));
            lin = {t1, cross(ax, t1)};
        } else lin = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
        for (const Vec3& axis : lin) {
            float err = clampf(0.5f * dot(pA - pB, axis), -0.1f, 0.1f);
            rows.push_back(linearRow(ra, rb, axis, rA, rB, err / dtl));
        }
        if (j->type == JointType::Hinge) {
            Vec3 a = normalize(axA);
            Vec3 e1 = std::fabs(a.x) < 0.9f ? normalize(cross(a, Vec3(1, 0, 0))) : normalize(cross(a, Vec3(0, 1, 0)));
            Vec3 e2 = cross(a, e1);
            Vec3 err = cross(normalize(axB), a); // rotation taking B's axis onto A's
            for (const Vec3& e : {e1, e2}) rows.push_back(angularRow(ra, rb, e, 0.5f * dot(err, e) / dtl));
            if (j->motorForce != 0) {
                Row r = angularRow(ra, rb, a, j->motorVel);
                r.hi = std::fabs(j->motorForce);
                r.lo = -std::fabs(j->motorForce);
                rows.push_back(r);
            }
        }
    }
}

void World::Impl::addMotionRows() {
    for (auto& bp : w->bodies_) {
        Body* b = bp.get();
        if (!b->motion || !b->active || !b->dynamic) continue;
        Motion& m = *b->motion;
        float mass = b->invMass > 0 ? 1.0f / b->invMass : 0;
        if (m.linForce > 0) {
            Vec3 err = m.pos - b->pos;
            for (int k = 0; k < 3; k++) {
                Vec3 axis;
                axis[k] = 1;
                float v = clampf(err[k] / dt, -m.linMaxSpeed, m.linMaxSpeed);
                // drive COM velocity along axis: J = [0,0 | axis, 0]
                Row r;
                r.a = nullptr;
                r.b = b;
                r.lB = axis;
                r.target = v;
                finishRow(r);
                float cap = m.linForce * mass;
                r.hi = cap;
                r.lo = -cap;
                if (m.linForce >= FLT_MAX / 2 || cap > 1e30f) {
                    r.hi = FLT_MAX;
                    r.lo = -FLT_MAX;
                }
                rows.push_back(r);
            }
        }
        if (m.angForce != 0) {
            Quat q = m.rot * b->rot.conj();
            if (q.w < 0) q = Quat(-q.x, -q.y, -q.z, -q.w);
            float sinHalf = length(Vec3(q.x, q.y, q.z));
            Vec3 rv;
            if (sinHalf > 1e-6f) rv = Vec3(q.x, q.y, q.z) / sinHalf * (2 * std::atan2(sinHalf, q.w));
            const Joint* hinge = nullptr;
            for (const Joint* j : b->joints)
                if (j->type == JointType::Hinge) {
                    hinge = j;
                    break;
                }
            std::vector<Vec3> axes;
            if (hinge)
                axes = {normalize(
                    (hinge->a == b ? hinge->a : hinge->b)->rot.rotate(hinge->a == b ? hinge->axisA : hinge->axisB))};
            else axes = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
            for (const Vec3& ax : axes) {
                float v = clampf(dot(rv, ax) / dt, -m.angMaxSpeed, m.angMaxSpeed);
                Row r = angularRow(nullptr, b, ax, v);
                float cap = std::fabs(m.angForce) * mass;
                r.hi = cap;
                r.lo = -cap;
                if (std::fabs(m.angForce) >= FLT_MAX / 2 || cap > 1e30f) {
                    r.hi = FLT_MAX;
                    r.lo = -FLT_MAX;
                }
                rows.push_back(r);
            }
        }
    }
}

void World::Impl::addContactRows() {
    for (size_t mi = 0; mi < manifolds.size(); mi++) {
        Manifold& m = manifolds[mi];
        Body *A = m.a->body, *B = m.b->body;
        Body *ra = A->dynamic ? A : nullptr, *rb = B->dynamic ? B : nullptr;
        m.mu = m.a->mat.friction * m.b->mat.friction;
        m.e = std::max(m.a->mat.restitution, m.b->mat.restitution);
        m.cap = std::min(m.a->mat.forceLimit, m.b->mat.forceLimit);
        Vec3 n = m.n;
        Vec3 t1 = std::fabs(n.x) < 0.9f ? normalize(cross(n, Vec3(1, 0, 0))) : normalize(cross(n, Vec3(0, 1, 0)));
        Vec3 t2 = cross(n, t1);
        for (int k = 0; k < m.count; k++) {
            Vec3 P = m.pts[k].pos;
            Vec3 rA = P - A->worldCom(), rB = P - B->worldCom();
            Row r = linearRow(ra, rb, n, rA, rB, 0);
            float dd = m.pts[k].sep - kSkin;
            float d2 = std::max(dd, -kSkin);
            Vec3 vA = A->vel + cross(A->angVel, rA), vB = B->vel + cross(B->angVel, rB);
            float vn = -dot(n, vB - vA); // closing speed
            const bool bounce = m.e > 0 && vn > kSkin / dt;
            if (bounce && d2 >= 0) d2 = 0;
            float bias = std::max(kErp / dt * d2, -kMaxPush);
            r.target = bounce ? -bias + m.e * std::max(vn, 0.0f) : -bias;
            r.lo = 0;
            r.hi = m.cap;
            r.kind = kRowContactNormal;
            r.mf = (int)mi;
            r.mpt = k;
            rows.push_back(r);
        }
        // friction: two tangential rows at the contact centroid, plus a twist row about the normal when there are
        // several points, all clamped to mu times the manifold's total normal impulse
        {
            Vec3 P;
            for (int k = 0; k < m.count; k++) P += m.pts[k].pos;
            P = P / (float)m.count;
            Vec3 rA = P - A->worldCom(), rB = P - B->worldCom();
            // friction anchor: once a contact has held, store the centroid and normal in both body frames, then let the
            // tangential rows pull any drift back at 0.8/dt so resting contacts do not creep. Reset when the stored
            // normals no longer agree.
            const bool sw = std::less<const Shape*>()(m.b, m.a); // the broadphase may list a pair either way round
            Anchor& an = anchors[sw ? ShapePair(m.b, m.a) : ShapePair(m.a, m.b)];
            Body *X = sw ? B : A, *Y = sw ? A : B;
            if (an.seen != solvePass - 1) an.state = kAnchorNone; // no contact in the previous pass: a new pair
            an.seen = solvePass;
            m.anchor = &an;
            float tgt[2] = {0, 0};
            if (an.state == kAnchorActive && dot(X->rot.rotate(an.n[0]), Y->rot.rotate(an.n[1])) < 0.99f)
                an.state = kAnchorNone;
            if (an.state == kAnchorActive) {
                Vec3 drift = Y->pointToWorld(an.p[1]) - X->pointToWorld(an.p[0]);
                if (sw) drift = -drift; // B's anchor point minus A's
                tgt[0] = -kErp / dt * dot(drift, t1);
                tgt[1] = -kErp / dt * dot(drift, t2);
            } else if (an.state == kAnchorArmed) {
                an.p[0] = X->pointToLocal(P);
                an.p[1] = Y->pointToLocal(P);
                an.n[0] = X->rot.conj().rotate(n);
                an.n[1] = Y->rot.conj().rotate(n);
                an.state = kAnchorActive;
            }
            for (int k = 0; k < 2; k++) {
                Row r = linearRow(ra, rb, k ? t2 : t1, rA, rB, tgt[k]);
                r.kind = kRowFriction;
                r.mf = (int)mi;
                r.mpt = 0;
                r.tangent = true;
                rows.push_back(r);
            }
            if (m.count > 1) {
                Row r = angularRow(ra, rb, n, 0);
                r.kind = kRowFriction;
                r.mf = (int)mi;
                r.mpt = 0;
                rows.push_back(r);
            }
        }
    }
}

// A manifold whose tangential impulse ended at the Coulomb bound has slipped and drops its anchor. Otherwise a
// fresh anchor is armed and gets stored on the next pass.
void World::Impl::postSolveAnchors() {
    for (const Row& r : rows) {
        if (!r.tangent) continue;
        Manifold& m = manifolds[r.mf];
        float S = 0;
        for (int k = 0; k < m.count; k++) S += m.pts[k].accN;
        if (std::fabs(r.acc) >= 0.999f * m.mu * S) m.slipped = true;
    }
    for (Manifold& m : manifolds) {
        if (!m.anchor) continue;
        if (m.slipped) m.anchor->state = kAnchorNone;
        else if (m.anchor->state == kAnchorNone) m.anchor->state = kAnchorArmed;
    }
}

void World::Impl::dropAnchors(const Shape* s) {
    for (auto it = anchors.begin(); it != anchors.end();) {
        if (it->first.first == s || it->first.second == s) it = anchors.erase(it);
        else ++it;
    }
}

void World::Impl::solve(int iterations) {
    for (int it = 0; it < iterations; it++) {
        for (Row& r : rows) {
            float lo = r.lo, hi = r.hi;
            if (r.kind == kRowFriction) {
                const Manifold& m = manifolds[r.mf];
                float S = 0;
                for (int k = 0; k < m.count; k++) S += m.pts[k].accN;
                hi = m.mu * S;
                lo = -hi;
            }
            float lambda = (r.target - jv(r)) * r.invEff;
            float na = std::min(std::max(r.acc + lambda, lo), hi);
            lambda = na - r.acc;
            r.acc = na;
            if (lambda != 0) applyImpulse(r, lambda);
            if (r.kind == kRowContactNormal) manifolds[r.mf].pts[r.mpt].accN = r.acc;
        }
    }
}

// ------------------------------------------------------------------ step

void World::Impl::integratePositions() {
    for (auto& bp : w->bodies_) {
        Body* b = bp.get();
        if (!b->dynamic || !b->active) continue;
        Vec3 com = b->worldCom() + b->vel * dt;
        float w2 = lengthSq(b->angVel);
        if (w2 > 0.001f) {
            float ang = std::sqrt(w2) * dt;
            Quat dq = Quat::axisAngle(b->angVel, ang);
            b->rot = (dq * b->rot).normalized();
        }
        b->pos = com - b->rot.rotate(b->localCom);
    }
}

void World::Impl::deactivate() {
    for (auto& bp : w->bodies_) {
        Body* b = bp.get();
        if (!b->dynamic || !b->active || b->motion || !b->joints.empty() || b->shapes.empty()) continue;
        float minZ = 1e30f, maxY = -1e30f;
        for (auto& s : b->shapes) {
            updateShapeCache(s.get(), false);
            minZ = std::min(minZ, s->aabbMin.z);
            maxY = std::max(maxY, s->aabbMax.y);
        }
        if (minZ > w->cullZ || maxY <= -10.0f) b->active = false;
    }
}

void World::step(float dt) {
    Impl& I = *d;
    I.dt = dt;
    for (auto& b : bodies_) {
        b->prevPos = b->pos;
        b->prevRot = b->rot;
        b->age += dt;
        b->touching = false;
        if (b->ballGhost > 0) b->ballGhost--;
    }
    // anchors of pairs without a contact in the last pass would start over anyway
    for (auto it = I.anchors.begin(); it != I.anchors.end();) {
        if (it->second.seen < I.solvePass) it = I.anchors.erase(it);
        else ++it;
    }
    I.deactivate();
    for (int pass = 0; pass < 3; pass++) {
        std::vector<std::pair<Shape*, Shape*>> pairs;
        I.buildPairs(pairs);
        for (auto& bp : bodies_) {
            Body* b = bp.get();
            b->invMEff = (b->dynamic && b->active) ? b->invMass : 0;
            Mat3 R = Mat3::fromQuat(b->rot);
            b->invIw = (b->dynamic && b->active) ? R * b->invInertiaLocal * R.transposed() : Mat3::zero();
            if (pass == 0 && b->dynamic && b->active) b->vel += gravity * dt;
        }
        I.rows.clear();
        I.manifolds.clear();
        I.addMotionRows();
        I.addJointRows();
        for (auto& pr : pairs) {
            Body *A = pr.first->body, *B = pr.second->body;
            float margin = (length(A->vel) + length(B->vel)) * dt + 0.04f;
            Manifold m;
            if (I.generate(pr.first, pr.second, margin, m) > 0) {
                if (A->isBall != B->isBall) {
                    A->lastTouched = B->id;
                    B->lastTouched = A->id;
                }
                if (A->isBall && A->ballState == kBallNitro) A->ballState = kBallExploding;
                if (B->isBall && B->ballState == kBallNitro) B->ballState = kBallExploding;
                I.manifolds.push_back(m);
            }
        }
        I.solvePass++;
        I.addContactRows();
        I.solve(4);
        I.postSolveAnchors();
        if (pass == 0) {
            for (auto& m : I.manifolds) {
                float S = 0;
                for (int k = 0; k < m.count; k++) S += m.pts[k].accN;
                if (S > 0) m.a->body->touching = m.b->body->touching = true;
            }
        }
        if (pass == 2) break;
        if (!I.processBreakage()) break;
    }
    I.integratePositions();
    // queries after the step (camera crash test, lasers, scripts) see the integrated pose, but the bounds still
    // cover the pre-step box because the original game runs its broadphase before integration
    for (auto& bp : bodies_) {
        Body* b = bp.get();
        if (!b->dynamic || !b->active) continue;
        for (auto& sp : b->shapes) {
            Shape* s = sp.get();
            const Vec3 mn = s->aabbMin, mx = s->aabbMax;
            I.updateShapeCache(s, false);
            for (int k = 0; k < 3; k++) {
                s->aabbMin[k] = std::min(s->aabbMin[k], mn[k]);
                s->aabbMax[k] = std::max(s->aabbMax[k], mx[k]);
            }
        }
    }
}

} // namespace sh
