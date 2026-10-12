#include "physics/convex.h"
#include <array>
#include <limits>

namespace sh {

Vec3 Convex::supportLocal(const Vec3& d) const {
    switch (type) {
    case ShapeType::Box: return {d.x >= 0 ? half.x : -half.x, d.y >= 0 ? half.y : -half.y, d.z >= 0 ? half.z : -half.z};
    case ShapeType::Sphere: {
        float l = length(d);
        return l > 1e-12f ? d * (half.x / l) : Vec3(half.x, 0, 0);
    }
    case ShapeType::Cylinder: {
        float lxz = std::sqrt(d.x * d.x + d.z * d.z);
        Vec3 p(0, d.y >= 0 ? half.y : -half.y, 0);
        if (lxz > 1e-12f) {
            p.x = d.x / lxz * half.x;
            p.z = d.z / lxz * half.x;
        }
        return p;
    }
    case ShapeType::Hull: {
        float best = -1e30f;
        Vec3 bv;
        for (auto& v : verts) {
            float s = dot(v, d);
            if (s > best) {
                best = s;
                bv = v;
            }
        }
        return bv;
    }
    }
    return {};
}

Vec3 Convex::support(const Vec3& wd) const {
    Vec3 ld = rot.conj().rotate(wd);
    return rot.rotate(supportLocal(ld)) + pos;
}

namespace {

struct SP { // Minkowski vertex w = a - b with witnesses
    Vec3 w, a, b;
};

SP minkowski(const Convex& A, const Convex& B, const Vec3& d) {
    SP s;
    s.a = A.support(d);
    s.b = B.support(-d);
    s.w = s.a - s.b;
    return s;
}

// GJK simplex: returns true when origin enclosed. Keeps up to 4 points.
struct Simplex {
    std::array<SP, 4> p;
    int n = 0;
    void push(const SP& s) {
        for (int i = std::min(n, 3); i > 0; --i) p[i] = p[i - 1];
        p[0] = s;
        n = std::min(n + 1, 4);
    }
};

bool doSimplex(Simplex& s, Vec3& dir) {
    switch (s.n) {
    case 2: {
        Vec3 a = s.p[0].w, b = s.p[1].w, ab = b - a, ao = -a;
        if (dot(ab, ao) > 0) dir = cross(cross(ab, ao), ab);
        else {
            s.n = 1;
            dir = ao;
        }
        if (lengthSq(dir) < 1e-18f) { // origin on the segment
            Vec3 t = std::fabs(ab.x) < 0.9f * length(ab) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            dir = cross(ab, t);
        }
        return false;
    }
    case 3: {
        Vec3 a = s.p[0].w, b = s.p[1].w, c = s.p[2].w, ab = b - a, ac = c - a, ao = -a, abc = cross(ab, ac);
        if (dot(cross(abc, ac), ao) > 0) {
            if (dot(ac, ao) > 0) {
                s.p[1] = s.p[2];
                s.n = 2;
                dir = cross(cross(ac, ao), ac);
            } else goto abCase;
        } else if (dot(cross(ab, abc), ao) > 0) {
        abCase:
            if (dot(ab, ao) > 0) {
                s.n = 2;
                dir = cross(cross(ab, ao), ab);
            } else {
                s.n = 1;
                dir = ao;
            }
        } else {
            if (dot(abc, ao) > 0) dir = abc;
            else {
                std::swap(s.p[1], s.p[2]);
                dir = -abc;
            }
        }
        if (lengthSq(dir) < 1e-18f) dir = abc;
        return false;
    }
    case 4: {
        Vec3 a = s.p[0].w, b = s.p[1].w, c = s.p[2].w, d = s.p[3].w, ao = -a;
        Vec3 ab = b - a, ac = c - a, ad = d - a;
        Vec3 abc = cross(ab, ac), acd = cross(ac, ad), adb = cross(ad, ab);
        if (dot(abc, d - a) > 0) abc = -abc; // orient outward
        if (dot(acd, b - a) > 0) acd = -acd;
        if (dot(adb, c - a) > 0) adb = -adb;
        if (dot(abc, ao) > 0) {
            s.n = 3;
            dir = abc;
            return false;
        }
        if (dot(acd, ao) > 0) {
            s.p[1] = s.p[2];
            s.p[2] = s.p[3];
            s.n = 3;
            dir = acd;
            return false;
        }
        if (dot(adb, ao) > 0) {
            s.p[2] = s.p[1];
            s.p[1] = s.p[3];
            s.n = 3;
            dir = adb;
            return false;
        }
        return true;
    }
    default: return false;
    }
}

bool gjk(const Convex& A, const Convex& B, Simplex& s) {
    Vec3 d = B.pos - A.pos;
    if (lengthSq(d) < 1e-12f) d = Vec3(1, 0, 0);
    s.push(minkowski(A, B, d));
    d = -s.p[0].w;
    if (lengthSq(d) < 1e-18f) d = Vec3(1, 0, 0);
    for (int it = 0; it < 64; it++) {
        SP np = minkowski(A, B, d);
        if (dot(np.w, d) < 0) return false;
        s.push(np);
        if (doSimplex(s, d)) return true;
        if (lengthSq(d) < 1e-24f) return s.n >= 3; // degenerate: treat as touching
    }
    return false;
}

struct Face {
    int i[3];
    Vec3 n;
    float d;
};

Face makeFace(const std::vector<SP>& v, int a, int b, int c) {
    Face f;
    f.i[0] = a;
    f.i[1] = b;
    f.i[2] = c;
    Vec3 n = cross(v[b].w - v[a].w, v[c].w - v[a].w);
    float l = length(n);
    f.n = l > 1e-20f ? n / l : Vec3(0, 0, 1);
    f.d = dot(f.n, v[a].w);
    if (f.d < 0) {
        f.n = -f.n;
        f.d = -f.d;
        std::swap(f.i[1], f.i[2]);
    }
    return f;
}

bool epa(const Convex& A, const Convex& B, Simplex& s, Contact& out) {
    std::vector<SP> v;
    // make sure we have a tetrahedron around the origin
    for (int i = 0; i < s.n; i++) v.push_back(s.p[i]);
    static const Vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int ax = 0;
    while (v.size() < 4 && ax < 6) {
        if (v.size() == 1) {
            v.push_back(minkowski(A, B, axes[ax++]));
            continue;
        }
        if (v.size() == 2) {
            Vec3 e = v[1].w - v[0].w;
            Vec3 t = std::fabs(e.x) < 0.9f * length(e) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            v.push_back(minkowski(A, B, cross(e, t)));
            continue;
        }
        Vec3 n = cross(v[1].w - v[0].w, v[2].w - v[0].w);
        if (lengthSq(n) < 1e-20f) {
            v.pop_back();
            v.push_back(minkowski(A, B, axes[ax++]));
            continue;
        }
        SP p = minkowski(A, B, n);
        if (std::fabs(dot(p.w - v[0].w, normalize(n))) < 1e-7f) p = minkowski(A, B, -n);
        v.push_back(p);
    }
    if (v.size() < 4) return false;
    std::vector<Face> faces = {makeFace(v, 0, 1, 2), makeFace(v, 0, 3, 1), makeFace(v, 0, 2, 3), makeFace(v, 1, 3, 2)};
    // orient faces to point away from centroid
    Vec3 c = (v[0].w + v[1].w + v[2].w + v[3].w) * 0.25f;
    for (auto& f : faces)
        if (dot(f.n, v[f.i[0]].w - c) < 0) {
            f.n = -f.n;
            f.d = dot(f.n, v[f.i[0]].w);
            std::swap(f.i[1], f.i[2]);
        }
    for (int it = 0; it < 64; it++) {
        int best = 0;
        for (int i = 1; i < (int)faces.size(); i++)
            if (faces[i].d < faces[best].d) best = i;
        Face f = faces[best];
        SP p = minkowski(A, B, f.n);
        float dd = dot(p.w, f.n);
        if (dd - f.d < 1e-4f || it == 63) {
            // barycentric witness points on face
            Vec3 a = v[f.i[0]].w, b = v[f.i[1]].w, cc = v[f.i[2]].w, pt = f.n * f.d;
            Vec3 e0 = b - a, e1 = cc - a, e2 = pt - a;
            float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1), d20 = dot(e2, e0), d21 = dot(e2, e1);
            float den = d00 * d11 - d01 * d01, u = 0, w = 0;
            if (std::fabs(den) > 1e-20f) {
                w = (d11 * d20 - d01 * d21) / den;
                u = (d00 * d21 - d01 * d20) / den;
            }
            float t0 = 1 - w - u;
            out.pointA = v[f.i[0]].a * t0 + v[f.i[1]].a * w + v[f.i[2]].a * u;
            out.pointB = v[f.i[0]].b * t0 + v[f.i[1]].b * w + v[f.i[2]].b * u;
            out.normal = f.n; // outward face normal of (A-B) points from A towards B
            out.depth = f.d;
            return true;
        }
        // remove visible faces, rebuild horizon
        std::vector<std::pair<int, int>> edges;
        auto addEdge = [&](int a, int b) {
            for (auto it2 = edges.begin(); it2 != edges.end(); ++it2)
                if (it2->first == b && it2->second == a) {
                    edges.erase(it2);
                    return;
                }
            edges.push_back({a, b});
        };
        std::vector<Face> keep;
        for (auto& fc : faces) {
            if (dot(fc.n, p.w - v[fc.i[0]].w) > 1e-9f) {
                addEdge(fc.i[0], fc.i[1]);
                addEdge(fc.i[1], fc.i[2]);
                addEdge(fc.i[2], fc.i[0]);
            } else keep.push_back(fc);
        }
        v.push_back(p);
        int pi = (int)v.size() - 1;
        for (auto& e : edges) {
            Face nf = makeFace(v, e.first, e.second, pi);
            if (dot(nf.n, v[e.first].w - c) < 0) {
                nf.n = -nf.n;
                nf.d = dot(nf.n, v[e.first].w);
                std::swap(nf.i[1], nf.i[2]);
            }
            keep.push_back(nf);
        }
        faces.swap(keep);
        if (faces.empty()) return false;
    }
    return false;
}

} // namespace

bool collide(const Convex& A, const Convex& B, Contact& out) {
    // sphere-sphere fast path
    if (A.type == ShapeType::Sphere && B.type == ShapeType::Sphere) {
        Vec3 d = B.pos - A.pos;
        float l = length(d), r = A.half.x + B.half.x;
        if (l >= r) return false;
        out.normal = l > 1e-9f ? d / l : Vec3(0, 1, 0);
        out.depth = r - l;
        out.pointA = A.pos + out.normal * A.half.x;
        out.pointB = B.pos - out.normal * B.half.x;
        return true;
    }
    Simplex s;
    if (!gjk(A, B, s)) return false;
    return epa(A, B, s, out);
}

float distance(const Convex& A, const Convex& B, Vec3* pa, Vec3* pb) {
    // GJK closest-point via iterative simplex reduction on the Minkowski difference.
    Vec3 d = B.pos - A.pos;
    if (lengthSq(d) < 1e-12f) d = Vec3(1, 0, 0);
    SP cur = minkowski(A, B, d);
    std::vector<SP> sim = {cur};
    Vec3 closest = cur.w, ca = cur.a, cb = cur.b;
    for (int it = 0; it < 64; it++) {
        if (lengthSq(closest) < 1e-12f) {
            if (pa) *pa = ca;
            if (pb) *pb = cb;
            return 0;
        }
        SP np = minkowski(A, B, -closest);
        if (dot(closest, closest) - dot(closest, np.w) < 1e-6f * dot(closest, closest) + 1e-9f) break;
        sim.push_back(np);
        // brute-force closest point on simplex (<=4 points): test all sub-simplices of size 1..3
        float bestD = 1e30f;
        Vec3 bw, ba, bb;
        std::vector<SP> bestSub;
        int n = (int)sim.size();
        for (int mask = 1; mask < (1 << n); mask++) {
            std::vector<SP> sub;
            for (int i = 0; i < n; i++)
                if (mask >> i & 1) sub.push_back(sim[i]);
            if (sub.size() > 3) continue;
            Vec3 w, a, b;
            bool ok = false;
            if (sub.size() == 1) {
                w = sub[0].w;
                a = sub[0].a;
                b = sub[0].b;
                ok = true;
            } else if (sub.size() == 2) {
                Vec3 e = sub[1].w - sub[0].w;
                float ee = dot(e, e);
                if (ee > 1e-20f) {
                    float t = -dot(sub[0].w, e) / ee;
                    if (t > 0 && t < 1) {
                        w = sub[0].w + e * t;
                        a = lerp(sub[0].a, sub[1].a, t);
                        b = lerp(sub[0].b, sub[1].b, t);
                        ok = true;
                    }
                }
            } else {
                Vec3 n3 = cross(sub[1].w - sub[0].w, sub[2].w - sub[0].w);
                float nn = dot(n3, n3);
                if (nn > 1e-20f) {
                    float t = dot(n3, sub[0].w) / nn;
                    Vec3 pt = n3 * t; // projection of the origin onto the plane
                    Vec3 e0 = sub[1].w - sub[0].w, e1 = sub[2].w - sub[0].w, e2 = pt - sub[0].w;
                    float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1), d20 = dot(e2, e0), d21 = dot(e2, e1);
                    float den = d00 * d11 - d01 * d01;
                    if (std::fabs(den) > 1e-20f) {
                        float v1 = (d11 * d20 - d01 * d21) / den, v2 = (d00 * d21 - d01 * d20) / den, v0 = 1 - v1 - v2;
                        if (v0 > 0 && v1 > 0 && v2 > 0) {
                            w = pt;
                            a = sub[0].a * v0 + sub[1].a * v1 + sub[2].a * v2;
                            b = sub[0].b * v0 + sub[1].b * v1 + sub[2].b * v2;
                            ok = true;
                        }
                    }
                }
            }
            if (ok && lengthSq(w) < bestD) {
                bestD = lengthSq(w);
                bw = w;
                ba = a;
                bb = b;
                bestSub = sub;
            }
        }
        if (bestSub.empty()) break;
        closest = bw;
        ca = ba;
        cb = bb;
        sim = bestSub;
    }
    if (pa) *pa = ca;
    if (pb) *pb = cb;
    return length(closest);
}

bool raycast(const Convex& s, const Vec3& o, const Vec3& d, float maxT, float& tOut, Vec3& nOut) {
    Vec3 lo = s.rot.conj().rotate(o - s.pos), ld = s.rot.conj().rotate(d);
    auto finish = [&](float t, Vec3 nLocal) {
        tOut = t;
        nOut = s.rot.rotate(nLocal);
        return true;
    };
    switch (s.type) {
    case ShapeType::Box: {
        float t0 = 0, t1 = maxT;
        Vec3 n0;
        for (int a = 0; a < 3; a++) {
            if (std::fabs(ld[a]) < 1e-12f) {
                if (std::fabs(lo[a]) > s.half[a]) return false;
                continue;
            }
            float inv = 1.0f / ld[a], ta = (-s.half[a] - lo[a]) * inv, tb = (s.half[a] - lo[a]) * inv;
            float sign = -1;
            if (ta > tb) {
                std::swap(ta, tb);
                sign = 1;
            }
            if (ta > t0) {
                t0 = ta;
                n0 = Vec3();
                n0[a] = sign;
            }
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        return finish(t0, n0);
    }
    case ShapeType::Sphere: {
        float r = s.half.x, b = dot(lo, ld), c = dot(lo, lo) - r * r, a = dot(ld, ld);
        float disc = b * b - a * c;
        if (disc < 0 || a < 1e-20f) return false;
        float t = (-b - std::sqrt(disc)) / a;
        if (c < 0) t = 0;
        if (t < 0 || t > maxT) return false;
        return finish(t, normalize(lo + ld * t));
    }
    case ShapeType::Cylinder: {
        float r = s.half.x, h = s.half.y, best = 1e30f;
        Vec3 bn;
        float a = ld.x * ld.x + ld.z * ld.z, b = lo.x * ld.x + lo.z * ld.z, c = lo.x * lo.x + lo.z * lo.z - r * r;
        if (a > 1e-12f) {
            float disc = b * b - a * c;
            if (disc >= 0) {
                float t = (-b - std::sqrt(disc)) / a, y = lo.y + ld.y * t;
                if (t >= 0 && std::fabs(y) <= h && t < best) {
                    best = t;
                    bn = normalize(Vec3(lo.x + ld.x * t, 0, lo.z + ld.z * t));
                }
            }
        }
        if (std::fabs(ld.y) > 1e-12f)
            for (float sg : {-1.0f, 1.0f}) {
                float t = (sg * h - lo.y) / ld.y;
                float x = lo.x + ld.x * t, z = lo.z + ld.z * t;
                if (t >= 0 && x * x + z * z <= r * r && t < best) {
                    best = t;
                    bn = Vec3(0, sg, 0);
                }
            }
        if (c < 0 && std::fabs(lo.y) < h) {
            best = 0;
            bn = Vec3(0, 1, 0);
        }
        if (best > maxT) return false;
        return finish(best, bn);
    }
    case ShapeType::Hull: {
        // distance march of a tiny sphere along the ray
        Convex pt;
        pt.type = ShapeType::Sphere;
        pt.half = Vec3(1e-4f, 0, 0);
        float dl = length(d);
        if (dl < 1e-12f) return false;
        Vec3 dir = d / dl;
        float t = 0, limit = maxT * dl;
        for (int it = 0; it < 128; it++) {
            pt.pos = o + dir * t;
            Vec3 pa, pb;
            float dist = distance(pt, s, &pa, &pb);
            if (dist < 3e-4f) {
                tOut = t / dl;
                nOut = dist > 1e-9f ? normalize(pa - pb) : -dir;
                return true;
            }
            t += std::max(dist * 0.9f, 1e-5f);
            if (t > limit) return false;
        }
        return false;
    }
    }
    return false;
}

} // namespace sh
