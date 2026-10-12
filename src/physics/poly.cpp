#include "physics/poly.h"
#include <algorithm>
#include <map>
#include <set>

namespace sh {

namespace {

void orderFace(const std::vector<Vec3>& v, std::vector<int>& idx, const Vec3& n) {
    Vec3 c;
    for (int i : idx) c += v[i];
    c = c / (float)idx.size();
    Vec3 u = std::fabs(n.x) < 0.9f ? cross(n, Vec3(1, 0, 0)) : cross(n, Vec3(0, 1, 0));
    u = normalize(u);
    Vec3 w = cross(n, u);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        Vec3 da = v[a] - c, db = v[b] - c;
        return std::atan2(dot(da, w), dot(da, u)) < std::atan2(dot(db, w), dot(db, u));
    });
}

// Welds vertices and builds a polyhedron from loops of positions.
struct Builder {
    std::vector<Vec3> v;
    std::vector<std::vector<int>> faces;
    float tol;
    explicit Builder(float t) : tol(t) {}
    int vertex(const Vec3& p) {
        for (size_t i = 0; i < v.size(); i++)
            if (lengthSq(v[i] - p) <= tol * tol) return (int)i;
        v.push_back(p);
        return (int)v.size() - 1;
    }
    void face(const std::vector<Vec3>& loop) {
        std::vector<int> idx;
        for (auto& p : loop) {
            int i = vertex(p);
            if (idx.empty() || idx.back() != i) idx.push_back(i);
        }
        while (idx.size() > 1 && idx.front() == idx.back()) idx.pop_back();
        // skip faces that collapsed below a triangle after welding
        if (idx.size() >= 3) faces.push_back(idx);
    }
    Polyhedron finish() {
        Polyhedron p;
        p.v = v;
        for (auto& idx : faces) {
            PolyFace f;
            f.idx = idx;
            p.f.push_back(f);
        }
        p.rebuildDerived();
        return p;
    }
};

} // namespace

void Polyhedron::rebuildDerived() {
    std::set<std::pair<int, int>> es;
    Vec3 centroid;
    for (auto& p : v) centroid += p;
    centroid = centroid / (float)std::max<size_t>(v.size(), 1);
    for (auto& fc : f) {
        // Newell normal
        Vec3 n;
        for (size_t i = 0; i < fc.idx.size(); i++) {
            const Vec3& a = v[fc.idx[i]];
            const Vec3& b = v[fc.idx[(i + 1) % fc.idx.size()]];
            n.x += (a.y - b.y) * (a.z + b.z);
            n.y += (a.z - b.z) * (a.x + b.x);
            n.z += (a.x - b.x) * (a.y + b.y);
        }
        float l = length(n);
        fc.n = l > 1e-20f ? n / l : Vec3(0, 1, 0);
        fc.d = dot(fc.n, v[fc.idx[0]]);
        for (size_t i = 0; i < fc.idx.size(); i++) {
            int a = fc.idx[i], b = fc.idx[(i + 1) % fc.idx.size()];
            es.insert({std::min(a, b), std::max(a, b)});
        }
    }
    edges.assign(es.begin(), es.end());
    vn.assign(v.size(), Vec3());
    for (auto& fc : f)
        for (int i : fc.idx) vn[(size_t)i] += fc.n;
    for (auto& n : vn) {
        float l = length(n);
        n = l > 1e-20f ? n / l : Vec3(1, 0, 0);
    }
}

Polyhedron Polyhedron::hull(const std::vector<Vec3>& in) {
    // Deduplicate
    std::vector<Vec3> pts;
    for (auto& p : in) {
        bool dup = false;
        for (auto& q : pts)
            if (lengthSq(p - q) < 1e-10f) {
                dup = true;
                break;
            }
        if (!dup) pts.push_back(p);
    }
    Polyhedron out;
    int n = (int)pts.size();
    if (n < 4) return out;
    float scale = 0;
    for (auto& p : pts) scale = std::max(scale, length(p));
    float eps = 1e-5f * std::max(scale, 1.0f);
    std::vector<std::vector<int>> faceSets;
    std::vector<Vec3> faceNormals;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            for (int k = j + 1; k < n; k++) {
                Vec3 nn = cross(pts[j] - pts[i], pts[k] - pts[i]);
                float l = length(nn);
                if (l < 1e-9f) continue;
                nn = nn / l;
                float d = dot(nn, pts[i]);
                bool pos = false, neg = false;
                for (int m = 0; m < n && !(pos && neg); m++) {
                    float s = dot(nn, pts[m]) - d;
                    if (s > eps) pos = true;
                    else if (s < -eps) neg = true;
                }
                if (pos && neg) continue;
                if (!pos && !neg) continue; // all coplanar: degenerate
                if (pos) {
                    nn = -nn;
                    d = -d;
                }
                // coplanar set
                std::vector<int> set;
                for (int m = 0; m < n; m++)
                    if (std::fabs(dot(nn, pts[m]) - d) <= eps) set.push_back(m);
                bool dupFace = false;
                for (auto& fs : faceSets)
                    if (fs == set) {
                        dupFace = true;
                        break;
                    }
                if (dupFace) continue;
                faceSets.push_back(set);
                faceNormals.push_back(nn);
            }
    out.v = pts;
    for (size_t a = 0; a < faceSets.size(); a++) {
        PolyFace fc;
        fc.idx = faceSets[a];
        orderFace(out.v, fc.idx, faceNormals[a]);
        out.f.push_back(fc);
    }
    out.rebuildDerived();
    return out;
}

Polyhedron Polyhedron::box(const Vec3& h, float c) {
    if (c > 1e-6f && c < std::min(h.x, std::min(h.y, h.z))) {
        std::vector<Vec3> pts;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sz = -1; sz <= 1; sz += 2) {
                    pts.push_back({sx * h.x, sy * (h.y - c), sz * (h.z - c)});
                    pts.push_back({sx * (h.x - c), sy * h.y, sz * (h.z - c)});
                    pts.push_back({sx * (h.x - c), sy * (h.y - c), sz * h.z});
                }
        return hull(pts);
    }
    Polyhedron p;
    for (int i = 0; i < 8; i++)
        p.v.push_back({(i & 1 ? 1.f : -1.f) * h.x, (i & 2 ? 1.f : -1.f) * h.y, (i & 4 ? 1.f : -1.f) * h.z});
    // faces CCW from outside: -X,+X,-Y,+Y,-Z,+Z
    auto F = [&](int a, int b, int c2, int d) {
        PolyFace f;
        f.idx = {a, b, c2, d};
        p.f.push_back(f);
    };
    F(0, 4, 6, 2);
    F(1, 3, 7, 5);
    F(0, 1, 5, 4);
    F(2, 6, 7, 3);
    F(0, 2, 3, 1);
    F(4, 5, 7, 6);
    p.rebuildDerived();
    return p;
}

Polyhedron Polyhedron::cylinderX(float r, float hl, int sides, float c) {
    std::vector<Vec3> pts;
    float rr = std::max(r - c, 1e-4f);
    for (int i = 0; i < sides; i++) {
        float a = 2 * kPi * i / sides, cy = std::cos(a) * rr, sz = std::sin(a) * rr;
        if (c > 1e-6f) {
            pts.push_back({-(hl - c), cy * (1 + c / rr), sz * (1 + c / rr)});
            pts.push_back({hl - c, cy * (1 + c / rr), sz * (1 + c / rr)});
            pts.push_back({-hl, cy, sz});
            pts.push_back({hl, cy, sz});
        } else {
            pts.push_back({-hl, cy, sz});
            pts.push_back({hl, cy, sz});
        }
    }
    if (c <= 1e-6f) {
        // analytic construction (hull brute force is wasteful for 48 points)
        Polyhedron p;
        p.v = pts; // index 2i = -x ring, 2i+1 = +x ring
        for (int i = 0; i < sides; i++) {
            int j = (i + 1) % sides;
            PolyFace f;
            f.idx = {2 * i, 2 * j, 2 * j + 1, 2 * i + 1};
            p.f.push_back(f);
        }
        PolyFace capNeg, capPos;
        for (int i = sides - 1; i >= 0; i--) capNeg.idx.push_back(2 * i);
        for (int i = 0; i < sides; i++) capPos.idx.push_back(2 * i + 1);
        p.f.push_back(capNeg);
        p.f.push_back(capPos);
        p.rebuildDerived();
        // verify orientation of the side faces (outward): flip all if needed
        Vec3 cen;
        bool bad = false;
        for (auto& fc : p.f)
            if (dot(fc.n, (p.v[fc.idx[0]] - cen)) < -1e-6f) bad = true;
        if (bad) {
            for (auto& fc : p.f) std::reverse(fc.idx.begin(), fc.idx.end());
            p.rebuildDerived();
        }
        return p;
    }
    return hull(pts);
}

void Polyhedron::transform(const Vec3& s, const Quat& q, const Vec3& t) {
    for (auto& p : v) p = q.rotate(Vec3(p.x * s.x, p.y * s.y, p.z * s.z)) + t;
    // mirrored scale flips winding
    if (s.x * s.y * s.z < 0)
        for (auto& fc : f) std::reverse(fc.idx.begin(), fc.idx.end());
    rebuildDerived();
}

void Polyhedron::bounds(Vec3& mn, Vec3& mx) const {
    mn = Vec3(1e30f, 1e30f, 1e30f);
    mx = Vec3(-1e30f, -1e30f, -1e30f);
    for (auto& p : v)
        for (int i = 0; i < 3; i++) {
            mn[i] = std::min(mn[i], p[i]);
            mx[i] = std::max(mx[i], p[i]);
        }
}

bool Polyhedron::split(const Vec3& n, float d, Polyhedron& pos, Polyhedron& neg, float eps) const {
    std::vector<float> dist(v.size());
    bool anyPos = false, anyNeg = false;
    for (size_t i = 0; i < v.size(); i++) {
        dist[i] = dot(n, v[i]) - d;
        if (std::fabs(dist[i]) < eps) dist[i] = 0;
        if (dist[i] > 0) anyPos = true;
        if (dist[i] < 0) anyNeg = true;
    }
    if (!anyPos || !anyNeg) return false;
    Builder bp(1e-4f), bn(1e-4f);
    std::vector<Vec3> cut; // points on plane
    for (auto& fc : f) {
        std::vector<Vec3> lp, ln;
        size_t m = fc.idx.size();
        for (size_t i = 0; i < m; i++) {
            int a = fc.idx[i], b = fc.idx[(i + 1) % m];
            float da = dist[a], db = dist[b];
            const Vec3& pa = v[a];
            const Vec3& pb = v[b];
            if (da >= 0) lp.push_back(pa);
            if (da <= 0) ln.push_back(pa);
            if ((da > 0 && db < 0) || (da < 0 && db > 0)) {
                float t = da / (da - db);
                Vec3 x = pa + (pb - pa) * t;
                lp.push_back(x);
                ln.push_back(x);
                cut.push_back(x);
            }
            if (da == 0) cut.push_back(pa);
        }
        if (lp.size() >= 3) bp.face(lp);
        if (ln.size() >= 3) bn.face(ln);
    }
    // cap faces from the cut points
    std::vector<Vec3> uniq;
    for (auto& p : cut) {
        bool dup = false;
        for (auto& q : uniq)
            if (lengthSq(p - q) < 1e-8f) {
                dup = true;
                break;
            }
        if (!dup) uniq.push_back(p);
    }
    if (uniq.size() < 3) return false;
    {
        // order around plane normal
        Vec3 c;
        for (auto& p : uniq) c += p;
        c = c / (float)uniq.size();
        Vec3 u = std::fabs(n.x) < 0.9f ? cross(n, Vec3(1, 0, 0)) : cross(n, Vec3(0, 1, 0));
        u = normalize(u);
        Vec3 w = cross(n, u);
        std::sort(uniq.begin(), uniq.end(), [&](const Vec3& a, const Vec3& b) {
            return std::atan2(dot(a - c, w), dot(a - c, u)) < std::atan2(dot(b - c, w), dot(b - c, u));
        });
        // CCW about +n  => outward normal +n (cap of the negative half, which faces +n)
        std::vector<Vec3> capNeg = uniq, capPos(uniq.rbegin(), uniq.rend());
        bn.face(capNeg);
        bp.face(capPos);
    }
    pos = bp.finish();
    neg = bn.finish();
    return pos.valid() && neg.valid();
}

float Polyhedron::volume() const {
    float vol = 0;
    for (auto& fc : f)
        for (size_t i = 1; i + 1 < fc.idx.size(); i++)
            vol += dot(v[fc.idx[0]], cross(v[fc.idx[i]], v[fc.idx[i + 1]])) / 6.0f;
    return vol;
}

void Polyhedron::massProps(float density, float& mass, Vec3& com, Mat3& I) const {
    double vol = 0, cx = 0, cy = 0, cz = 0;
    double C[9] = {0};
    for (auto& fc : f)
        for (size_t i = 1; i + 1 < fc.idx.size(); i++) {
            const Vec3 &a = v[fc.idx[0]], &b = v[fc.idx[i]], &c = v[fc.idx[i + 1]];
            double tv = (double)dot(a, cross(b, c)) / 6.0;
            vol += tv;
            cx += tv * (a.x + b.x + c.x) / 4.0;
            cy += tv * (a.y + b.y + c.y) / 4.0;
            cz += tv * (a.z + b.z + c.z) / 4.0;
            // second moments of tetra (0,a,b,c): tv/20 * (sum_i p_i p_i^T + (sum p)(sum p)^T)
            const Vec3* P[3] = {&a, &b, &c};
            double S[3] = {a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z};
            for (int r = 0; r < 3; r++)
                for (int q = 0; q < 3; q++) {
                    double s = 0;
                    for (int k = 0; k < 3; k++) s += (double)(*P[k])[r] * (*P[k])[q];
                    C[r * 3 + q] += tv / 20.0 * (s + S[r] * S[q]);
                }
        }
    if (vol <= 1e-12) {
        mass = 0;
        com = Vec3();
        I = Mat3::zero();
        return;
    }
    Vec3 cm((float)(cx / vol), (float)(cy / vol), (float)(cz / vol));
    double M = vol * density;
    double dens = density;
    // covariance about COM
    double cc[3] = {cm.x, cm.y, cm.z};
    for (int r = 0; r < 3; r++)
        for (int q = 0; q < 3; q++) C[r * 3 + q] = dens * C[r * 3 + q] - M * cc[r] * cc[q];
    double tr = C[0] + C[4] + C[8];
    Mat3 res = Mat3::zero();
    for (int r = 0; r < 3; r++)
        for (int q = 0; q < 3; q++) res.m[r * 3 + q] = (float)((r == q ? tr : 0) - C[r * 3 + q]);
    mass = (float)M;
    com = cm;
    I = res;
}

Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    Vec3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    Vec3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float den = 1.0f / (va + vb + vc);
    return a + ab * (vb * den) + ac * (vc * den);
}

float Polyhedron::distanceTo(const Vec3& p, Vec3* closest) const {
    // inside test
    float maxd = -1e30f;
    for (auto& fc : f) maxd = std::max(maxd, dot(fc.n, p) - fc.d);
    if (maxd <= 0) {
        if (closest) *closest = p;
        return 0;
    }
    float best = 1e30f;
    Vec3 bp;
    for (auto& fc : f)
        for (size_t i = 1; i + 1 < fc.idx.size(); i++) {
            Vec3 q = closestPointOnTriangle(p, v[fc.idx[0]], v[fc.idx[i]], v[fc.idx[i + 1]]);
            float dd = lengthSq(p - q);
            if (dd < best) {
                best = dd;
                bp = q;
            }
        }
    if (closest) *closest = bp;
    return std::sqrt(best);
}

} // namespace sh
