// Small header-only math library (right-handed, column-major matrices, -Z forward like the original).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sh {

constexpr float kPi = 3.14159265358979f;

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    Vec3& operator-=(const Vec3& o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    Vec3& operator*=(float s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lengthSq(const Vec3& v) { return dot(v, v); }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float l = length(v);
    return l > 1e-20f ? v / l : Vec3(0, 0, 0);
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    static Quat axisAngle(Vec3 axis, float a) {
        axis = normalize(axis);
        float s = std::sin(a * 0.5f);
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(a * 0.5f)};
    }
    // rotation about x, then y, then z (radians), as the game's scripts and segment files specify it
    static Quat fromEuler(float x, float y, float z) {
        return axisAngle(Vec3(1, 0, 0), x) * axisAngle(Vec3(0, 1, 0), y) * axisAngle(Vec3(0, 0, 1), z);
    }
    // Rotation whose matrix has columns bx, by, bz (orthonormal, right-handed).
    static Quat fromBasis(const Vec3& bx, const Vec3& by, const Vec3& bz) {
        float t = bx.x + by.y + bz.z;
        Quat q;
        if (t > 0) {
            float s = std::sqrt(t + 1) * 2;
            q = {(by.z - bz.y) / s, (bz.x - bx.z) / s, (bx.y - by.x) / s, 0.25f * s};
        } else if (bx.x > by.y && bx.x > bz.z) {
            float s = std::sqrt(1 + bx.x - by.y - bz.z) * 2;
            q = {0.25f * s, (by.x + bx.y) / s, (bz.x + bx.z) / s, (by.z - bz.y) / s};
        } else if (by.y > bz.z) {
            float s = std::sqrt(1 + by.y - bx.x - bz.z) * 2;
            q = {(by.x + bx.y) / s, 0.25f * s, (bz.y + by.z) / s, (bz.x - bx.z) / s};
        } else {
            float s = std::sqrt(1 + bz.z - bx.x - by.y) * 2;
            q = {(bz.x + bx.z) / s, (bz.y + by.z) / s, 0.25f * s, (bx.y - by.x) / s};
        }
        return q.normalized();
    }
    Quat operator*(const Quat& o) const {
        return {w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w, w * o.w - x * o.x - y * o.y - z * o.z};
    }
    Quat conj() const { return {-x, -y, -z, w}; }
    Quat normalized() const {
        float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > 0 ? Quat(x / l, y / l, z / l, w / l) : Quat();
    }
    Vec3 rotate(const Vec3& v) const {
        Vec3 q(x, y, z);
        Vec3 t = cross(q, v) * 2.0f;
        return v + t * w + cross(q, t);
    }
};

struct Mat3 {
    float m[9]; // row-major: m[row*3+col]
    Mat3() {
        std::memset(m, 0, sizeof m);
        m[0] = m[4] = m[8] = 1;
    }
    static Mat3 zero() {
        Mat3 r;
        std::memset(r.m, 0, sizeof r.m);
        return r;
    }
    static Mat3 diag(float a, float b, float c) {
        Mat3 r = zero();
        r.m[0] = a;
        r.m[4] = b;
        r.m[8] = c;
        return r;
    }
    static Mat3 fromQuat(const Quat& q) {
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z,
              wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        Mat3 r;
        r.m[0] = 1 - 2 * (yy + zz);
        r.m[1] = 2 * (xy - wz);
        r.m[2] = 2 * (xz + wy);
        r.m[3] = 2 * (xy + wz);
        r.m[4] = 1 - 2 * (xx + zz);
        r.m[5] = 2 * (yz - wx);
        r.m[6] = 2 * (xz - wy);
        r.m[7] = 2 * (yz + wx);
        r.m[8] = 1 - 2 * (xx + yy);
        return r;
    }
    Vec3 operator*(const Vec3& v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z};
    }
    Mat3 operator*(const Mat3& o) const {
        Mat3 r = zero();
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int k = 0; k < 3; k++) r.m[i * 3 + j] += m[i * 3 + k] * o.m[k * 3 + j];
        return r;
    }
    Mat3 operator+(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 9; i++) r.m[i] = m[i] + o.m[i];
        return r;
    }
    Mat3 operator*(float s) const {
        Mat3 r;
        for (int i = 0; i < 9; i++) r.m[i] = m[i] * s;
        return r;
    }
    Mat3 transposed() const {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i * 3 + j] = m[j * 3 + i];
        return r;
    }
    float det() const {
        return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
    }
    Mat3 inverse() const {
        float d = det();
        Mat3 r = zero();
        if (std::fabs(d) < 1e-30f) return r;
        float id = 1.0f / d;
        r.m[0] = (m[4] * m[8] - m[5] * m[7]) * id;
        r.m[1] = (m[2] * m[7] - m[1] * m[8]) * id;
        r.m[2] = (m[1] * m[5] - m[2] * m[4]) * id;
        r.m[3] = (m[5] * m[6] - m[3] * m[8]) * id;
        r.m[4] = (m[0] * m[8] - m[2] * m[6]) * id;
        r.m[5] = (m[2] * m[3] - m[0] * m[5]) * id;
        r.m[6] = (m[3] * m[7] - m[4] * m[6]) * id;
        r.m[7] = (m[1] * m[6] - m[0] * m[7]) * id;
        r.m[8] = (m[0] * m[4] - m[1] * m[3]) * id;
        return r;
    }
};

struct Mat4 {
    float m[16]; // column-major: m[col*4+row]
    Mat4() {
        std::memset(m, 0, sizeof m);
        m[0] = m[5] = m[10] = m[15] = 1;
    }
    static Mat4 translation(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }
    static Mat4 scale(const Vec3& s) {
        Mat4 r;
        r.m[0] = s.x;
        r.m[5] = s.y;
        r.m[10] = s.z;
        return r;
    }
    static Mat4 fromQuat(const Quat& q) {
        Mat4 r;
        float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z,
              wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        r.m[0] = 1 - 2 * (yy + zz);
        r.m[1] = 2 * (xy + wz);
        r.m[2] = 2 * (xz - wy);
        r.m[4] = 2 * (xy - wz);
        r.m[5] = 1 - 2 * (xx + zz);
        r.m[6] = 2 * (yz + wx);
        r.m[8] = 2 * (xz + wy);
        r.m[9] = 2 * (yz - wx);
        r.m[10] = 1 - 2 * (xx + yy);
        return r;
    }
    static Mat4 perspective(float fovY, float aspect, float n, float f) {
        Mat4 r;
        std::memset(r.m, 0, sizeof r.m);
        float t = 1.0f / std::tan(fovY * 0.5f);
        r.m[0] = t / aspect;
        r.m[5] = t;
        r.m[10] = (f + n) / (n - f);
        r.m[11] = -1;
        r.m[14] = 2 * f * n / (n - f);
        return r;
    }
    static Mat4 ortho(float l, float r_, float b, float t, float n, float f) {
        Mat4 r;
        r.m[0] = 2 / (r_ - l);
        r.m[5] = 2 / (t - b);
        r.m[10] = -2 / (f - n);
        r.m[12] = -(r_ + l) / (r_ - l);
        r.m[13] = -(t + b) / (t - b);
        r.m[14] = -(f + n) / (f - n);
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[k * 4 + rr] * o.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
    Vec3 transformPoint(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }
    Vec3 transformDir(const Vec3& p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z, m[1] * p.x + m[5] * p.y + m[9] * p.z,
                m[2] * p.x + m[6] * p.y + m[10] * p.z};
    }
};

} // namespace sh
