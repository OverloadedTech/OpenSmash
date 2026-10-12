// Breakage: detection (contact impulse at or over the force limit), cell fracture, random-plane splits and
// connectivity islands.
#include <algorithm>
#include <functional>
#include <map>
#include "physics/convex.h"
#include "physics/world.h"
#include "physics/world_impl.h"

namespace sh {

namespace {

struct Plane { // inward: dist = dot(n,x) - d > 0 inside the cell
    Vec3 n;
    float d;
};

Vec3 randomUnit(std::mt19937& rng) {
    std::uniform_real_distribution<float> u(-1, 1);
    for (int i = 0; i < 16; i++) {
        float x = u(rng), y = u(rng), z = u(rng); // separate draws: argument evaluation order is unspecified
        Vec3 v(x, y, z);
        float l = length(v);
        if (l > 0.05f && l <= 1) return v / l;
    }
    return Vec3(0, 1, 0);
}

bool straddles(const Polyhedron& p, const Vec3& n, float d) {
    bool hi = false, lo = false;
    for (const Vec3& v : p.v) {
        float s = dot(n, v) - d;
        if (s > 0.06f) hi = true;
        if (s < -0.06f) lo = true;
    }
    return hi && lo;
}

struct Piece {
    Shape* shape; // existing shape object (kept for pointer stability) or nullptr for new
    Shape* src;   // shape this came from (material/origin)
    Polyhedron poly;
};

Shape* detach(Body* from, Shape* s, std::unique_ptr<Shape>& holder) {
    for (auto it = from->shapes.begin(); it != from->shapes.end(); ++it)
        if (it->get() == s) {
            holder = std::move(*it);
            from->shapes.erase(it);
            return s;
        }
    return nullptr;
}

} // namespace

// Moves a shape object into another body (keeps its address valid for script handles).
static void transferShape(Body* from, Body* to, Shape* s, std::function<void(Shape*)> gridDrop) {
    if (from == to) return;
    std::unique_ptr<Shape> holder;
    detach(from, s, holder);
    if (!holder) return;
    gridDrop(s);
    holder->body = to;
    to->shapes.push_back(std::move(holder));
}

static void reassignJoints(Body* B, const std::vector<Body*>& pieces) {
    std::vector<Joint*> js = B->joints;
    for (Joint* j : js) {
        for (int side = 0; side < 2; side++) {
            Body*& slot = side == 0 ? j->a : j->b;
            if (slot != B) continue;
            Vec3 anchor = side == 0 ? j->anchorA : j->anchorB;
            Body* best = B;
            float bestD = 1e30f;
            for (Body* p : pieces)
                for (auto& s : p->shapes) {
                    float dd = s->poly.distanceTo(anchor);
                    if (dd < bestD) {
                        bestD = dd;
                        best = p;
                    }
                }
            if (best != B) {
                slot = best;
                best->joints.push_back(j);
                if (j->a != B && j->b != B)
                    B->joints.erase(std::remove(B->joints.begin(), B->joints.end(), j), B->joints.end());
            }
        }
    }
}

// Splits each of `shapes` (all on src) by one random plane through Pl. The far halves go to a new body, which is
// returned, or nullptr when no shape straddled the plane.
Body* World::splitByRandomPlane(Body* src, const std::vector<Shape*>& shapes, const Vec3& Pl) {
    Vec3 n = randomUnit(rng_);
    float dd = dot(n, Pl);
    Body* C = nullptr;
    for (Shape* s : shapes) {
        Polyhedron hi, lo;
        if (straddles(s->poly, n, dd) && s->poly.split(n, dd, hi, lo)) {
            if (!C) C = cloneWithoutShapes(src);
            s->poly = std::move(lo);
            s->broken = true;
            Shape* ns = addPoly(C, std::move(hi), s->mat, s->origin);
            if (ns) ns->broken = true;
        }
    }
    if (C) {
        refreshShapes(src);
        refreshShapes(C);
    }
    return C;
}

void World::breakBody(Body* B, const Vec3& wp, const Vec3& wn, float limit, float cell) {
    B->brokenFlag = true;
    Vec3 Pl = B->pointToLocal(wp);
    Vec3 Nl = normalize(B->rot.conj().rotate(wn));
    if (lengthSq(Nl) < 0.5f) Nl = Vec3(0, 0, 1);

    std::vector<Plane> planes;
    planes.push_back({-Nl, dot(-Nl, Pl + Nl * cell)});
    planes.push_back({Nl, dot(Nl, Pl - Nl * cell)});
    Vec3 t1 = std::fabs(Nl.y) <= 0.7f ? normalize(cross(Nl, Vec3(0, 1, 0))) : normalize(cross(Nl, Vec3(1, 0, 0)));
    Vec3 t2 = cross(Nl, t1);
    for (const Vec3& t : {t1, t2}) {
        Vec3 n = normalize(t + randomUnit(rng_) * 0.3f);
        planes.push_back({-n, dot(-n, Pl + n * cell)});
        planes.push_back({n, dot(n, Pl - n * cell)});
    }

    // candidate shapes
    std::vector<Shape*> cands;
    for (auto& sp : B->shapes) {
        Shape* s = sp.get();
        if (s->mat.mask == 0 || s->mat.forceLimit > limit) continue;
        bool outside = false;
        for (auto& pl : planes) {
            bool allOut = true;
            for (const Vec3& v : s->poly.v)
                if (dot(pl.n, v) - pl.d >= -0.02f) {
                    allOut = false;
                    break;
                }
            if (allOut) {
                outside = true;
                break;
            }
        }
        if (!outside) cands.push_back(s);
    }
    if (cands.empty()) return;
    for (Shape* s : cands) d->dropAnchors(s); // cut or moved to another body: the pieces' contacts start over

    std::vector<Piece> pieces;
    for (Shape* s : cands) pieces.push_back({s, s, s->poly});
    for (auto& pl : planes) {
        std::vector<Piece> next;
        for (auto& pc : pieces) {
            Polyhedron hi, lo;
            if (straddles(pc.poly, pl.n, pl.d) && pc.poly.split(pl.n, pl.d, hi, lo)) {
                next.push_back({pc.shape, pc.src, std::move(hi)});
                next.push_back({nullptr, pc.src, std::move(lo)});
            } else next.push_back(std::move(pc));
        }
        pieces.swap(next);
    }
    if (pieces.size() == cands.size()) {
        // nothing was cut (impact near an edge): fall back to a single random-plane split
        if (Body* C = splitByRandomPlane(B, cands, Pl)) reassignJoints(B, {B, C});
        return;
    }

    // redistribute: pieces at the hit become a free body; others stay on B
    Body* S = cloneWithoutShapes(B);
    float r = cell * 0.99f;
    std::vector<std::pair<Piece*, bool>> dest;
    for (auto& pc : pieces) {
        float dist = pc.poly.distanceTo(Pl);
        dest.push_back({&pc, dist < r});
    }
    bool anyStay = false;
    for (auto& dp : dest)
        if (!dp.second) anyStay = true;
    // B's non-candidate shapes also count as "staying"
    bool bHasOthers = B->shapes.size() > cands.size();
    bool allToS = !anyStay && !bHasOthers;
    Body* shardBody = allToS ? B : S;
    for (auto& dp : dest) {
        Piece& pc = *dp.first;
        Body* to = dp.second ? shardBody : B;
        if (pc.shape) {
            pc.shape->poly = std::move(pc.poly);
            pc.shape->broken = true;
            if (to != B) transferShape(B, to, pc.shape, [&](Shape* s) { d->gridRemove(s); });
        } else {
            Shape* ns = addPoly(to, std::move(pc.poly), pc.src->mat, pc.src->origin);
            if (ns) ns->broken = true;
        }
    }
    if (allToS) { // B itself became the shard
        destroyBody(S);
        S = nullptr;
        B->splitProcessed = true;
    }
    refreshShapes(B);
    if (S) refreshShapes(S);

    // split what is left of B into connected islands
    std::vector<Body*> parts{B};
    {
        auto& sh = B->shapes;
        int n = (int)sh.size();
        std::vector<int> comp(n);
        for (int i = 0; i < n; i++) comp[i] = i;
        std::function<int(int)> find = [&](int x) { return comp[x] == x ? x : comp[x] = find(comp[x]); };
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                Vec3 amn, amx, bmn, bmx;
                sh[i]->poly.bounds(amn, amx);
                sh[j]->poly.bounds(bmn, bmx);
                bool ov = true;
                for (int k = 0; k < 3; k++)
                    if (amn[k] - 0.04f > bmx[k] || bmn[k] - 0.04f > amx[k]) ov = false;
                if (!ov) continue;
                Convex ca, cb;
                ca.type = cb.type = ShapeType::Hull;
                ca.verts = sh[i]->poly.v;
                cb.verts = sh[j]->poly.v;
                if (distance(ca, cb) < 0.04f) comp[find(i)] = find(j);
            }
        std::map<int, std::vector<Shape*>> groups;
        for (int i = 0; i < n; i++) groups[find(i)].push_back(sh[i].get());
        if (groups.size() > 1) {
            bool first = true;
            for (auto& g : groups) {
                if (first) {
                    first = false;
                    continue;
                }
                Body* nb = cloneWithoutShapes(B);
                for (Shape* s : g.second) transferShape(B, nb, s, [&](Shape* x) { d->gridRemove(x); });
                refreshShapes(nb);
                parts.push_back(nb);
            }
            refreshShapes(B);
        }
    }
    if (S) {
        S->splitProcessed = true;
        parts.push_back(S);
        // one more random-plane cut of the shard cluster
        std::vector<Shape*> sc;
        for (auto& sp : S->shapes) {
            if (sp->mat.mask == 0 || sp->mat.forceLimit > limit) continue;
            sc.push_back(sp.get());
        }
        if (Body* C = splitByRandomPlane(S, sc, Pl)) parts.push_back(C);
    }
    // joints that were on B follow the nearest piece
    reassignJoints(B, parts);
}

// ------------------------------------------------------------------ breakage detection

bool World::Impl::processBreakage() {
    struct Q {
        Body* body;
        Vec3 pt, n;
        float limit;
        Body* other;
        const Shape* shape; // the one over its limit
    };
    std::vector<Q> queue;
    auto queued = [&](Body* b) {
        for (auto& q : queue)
            if (q.body == b) return true;
        return false;
    };
    for (const Manifold& m : manifolds) {
        if (m.count == 0) continue;
        if (m.a->mat.forceLimit >= FLT_MAX && m.b->mat.forceLimit >= FLT_MAX) continue;
        float S = 0;
        for (int k = 0; k < m.count; k++) S += m.pts[k].accN;
        const Shape* sh[2] = {m.a, m.b};
        for (int i = 0; i < 2; i++) {
            const Shape* X = sh[i];
            Body* xb = X->body;
            Body* other = sh[1 - i]->body;
            if (X->mat.forceLimit < FLT_MAX && X->mat.forceLimit <= S && xb->age > 0.5f) {
                if (queued(xb)) continue;
                Vec3 n = i == 0 ? m.n : -m.n; // away from the breaking shape
                queue.push_back({xb, m.pts[0].pos, n, X->mat.forceLimit, other, X});
                if (other->isBall) other->hitBreakable = true;
            }
        }
    }
    if (queue.empty()) return false;
    // one sound for the whole pass, played before anything breaks at the first queued body's position:
    // glass_pane if any breaking shape is a thin pane, otherwise glass_s
    bool anyPlate = false;
    for (auto& q : queue) anyPlate |= q.shape->plate;
    if (w->onBreakSound) w->onBreakSound(queue[0].body->pos, anyPlate);
    for (size_t qi = 0; qi < queue.size(); qi++) {
        auto& q = queue[qi];
        BreakEvent ev{};
        ev.firstInPass = qi == 0;
        ev.body = q.body;
        ev.point = q.pt;
        ev.normal = q.n;
        ev.bodyVel = q.body->vel;
        ev.firstBreak = !q.body->brokenFlag;
        ev.byBall = q.other && q.other->isBall;
        ev.color[0] = ev.color[1] = ev.color[2] = ev.color[3] = 1;
        for (auto& s : q.body->shapes)
            if (s->mat.forceLimit < FLT_MAX) {
                for (int c = 0; c < 4; c++) ev.color[c] = s->mat.color[c];
                if (ev.color[3] <= 0) ev.color[3] = 1;
            }
        if (ev.firstBreak) w->brokenBodyCount++;
        if (w->onBreak) w->onBreak(ev);
        w->breakBody(q.body, q.pt, q.n, q.limit, 0.25f);
    }
    return true;
}

} // namespace sh
