#include "game/level_script.h"
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <ctime>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#include "script/lua_args.h"

namespace sh {

void ScriptContext::log(const std::string& msg) { fprintf(stderr, "[script] %s\n", msg.c_str()); }

// ---------------------------------------------------------------- ScriptRng

uint32_t ScriptRng::next() {
    s_ += 0xDC769u;
    uint32_t h = ((s_ >> 16) ^ s_ ^ 0x3Du) * 9u;
    h = ((h >> 4) ^ h) * 0x27D4EB2Du;
    return (h >> 15) ^ h;
}

int ScriptRng::getInt(int a, int b) {
    int64_t span = (int64_t)b - a + 1;
    int v = (int)((span * (uint64_t)next()) / 0xFFFFFFFFull) + a;
    return std::min(std::max(v, a), b);
}

// ---------------------------------------------------------------- helpers

namespace {

LevelScript* self(lua_State* L) { return (LevelScript*)lua_touserdata(L, lua_upvalueindex(1)); }

void pushV3(lua_State* L, const Vec3& v) {
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
}

Quat poseRot(LevelScript* s) { return s->ctx->poseRot(); }

#define HANDLER(name) int name(lua_State* L)
#define PROLOG                 \
    LevelScript* S = self(L);  \
    LuaArgs a(L);              \
    ScriptContext* C = S->ctx; \
    (void)C;                   \
    (void)a

// ---- time / camera ----
HANDLER(mgTime) {
    PROLOG;
    lua_pushnumber(L, C->levelTime());
    return 1;
}
HANDLER(mgGetTimeStep) {
    PROLOG;
    lua_pushnumber(L, C->timeStep());
    return 1;
}
HANDLER(mgObstacleTime) {
    PROLOG;
    lua_pushnumber(L, C->obstacleTime());
    return 1;
}
HANDLER(mgCameraDistance) {
    PROLOG;
    lua_pushnumber(L, length(C->cameraPos() - C->posePos()));
    return 1;
}
HANDLER(mgGetCameraPos) {
    PROLOG;
    pushV3(L, S->toLocal(C->cameraPos()));
    return 3;
}
HANDLER(mgGetCameraSpeed) {
    PROLOG;
    lua_pushnumber(L, C->cameraSpeed());
    return 1;
}
HANDLER(mgGetBeat) {
    PROLOG;
    lua_pushinteger(L, (lua_Integer)std::floor(C->beat(a.f(0))));
    return 1;
}
HANDLER(mgGetBeatFloat) {
    PROLOG;
    lua_pushnumber(L, C->beat(a.f(0)));
    return 1;
}

// ---- parameters / properties ----
HANDLER(mgGetString) {
    PROLOG;
    const std::string* p = S->param(a.s(0));
    lua_pushstring(L, p ? p->c_str() : a.s(1).c_str());
    return 1;
}
HANDLER(mgGetFloat) {
    PROLOG;
    const std::string* p = S->param(a.s(0));
    lua_pushnumber(L, p ? (float)atof(p->c_str()) : a.f(1));
    return 1;
}
HANDLER(mgGetInt) {
    PROLOG;
    const std::string* p = S->param(a.s(0));
    lua_pushinteger(L, p ? atoi(p->c_str()) : a.i(1));
    return 1;
}
HANDLER(mgGetBool) {
    PROLOG;
    const std::string* p = S->param(a.s(0));
    lua_pushboolean(L, p ? *p == "true" : a.b(1));
    return 1;
}
HANDLER(mgGetColor) {
    PROLOG;
    const std::string* p = S->param(a.s(0));
    float c[3] = {a.f(1), a.f(2), a.f(3)};
    if (p) {
        c[0] = c[1] = c[2] = 0;
        const char* s = p->c_str();
        for (int k = 0; k < 3; k++) {
            while (*s == ' ' || *s == '\t') s++;
            char* e;
            float v = strtof(s, &e);
            if (e == s) break;
            c[k] = v;
            s = e;
        }
    }
    lua_pushnumber(L, c[0]);
    lua_pushnumber(L, c[1]);
    lua_pushnumber(L, c[2]);
    return 3;
}
HANDLER(mgGet) {
    PROLOG;
    lua_pushstring(L, C->getProperty(a.s(0)).c_str());
    return 1;
}
HANDLER(mgSet) {
    PROLOG;
    C->setProperty(a.s(0), a.s(1));
    return 0;
}
HANDLER(mgDebug) { return 0; }

// ---- random ----
HANDLER(mgRandomize) {
    PROLOG;
    S->rng.setSeed(a.n > 0 ? (uint32_t)a.i(0) : (uint32_t)time(nullptr));
    return 0;
}
HANDLER(mgRndInt) {
    PROLOG;
    lua_pushinteger(L, S->rng.getInt(a.i(0), a.i(1)));
    return 1;
}
HANDLER(mgRndFloat) {
    PROLOG;
    lua_pushnumber(L, S->rng.getFloat(a.f(0), a.f(1)));
    return 1;
}
HANDLER(mgRndBool) {
    PROLOG;
    float p = a.n > 0 ? a.f(0) : 0.5f;
    lua_pushboolean(L, p > S->rng.getFloat(0, 1));
    return 1;
}

// ---- queries ----
HANDLER(mgRaycast) {
    PROLOG;
    Vec3 p0 = a.v3(0), p1 = a.v3(3);
    int mask = a.n >= 7 ? a.i(6) : 255;
    RayHit h = C->world().raycast(S->toWorld(p0), S->toWorld(p1), mask, nullptr);
    lua_pushboolean(L, h.hit);
    if (h.hit) {
        Vec3 pt = h.point + h.normal * 0.01f;
        pushV3(L, S->toLocal(pt));
        pushV3(L, poseRot(S).conj().rotate(h.normal));
    } else { // a miss returns the end point and a zero normal, as in the original
        pushV3(L, p1);
        pushV3(L, Vec3());
    }
    return 7;
}
HANDLER(mgCanPass) {
    PROLOG;
    lua_pushboolean(L, C->canPass());
    return 1;
}
HANDLER(mgBoxOverlap) {
    PROLOG;
    Vec3 h = a.v3(3) - Vec3(0.02f, 0.02f, 0.02f);
    lua_pushboolean(L, C->world().overlapBox(S->toWorld(a.v3(0)), h));
    return 1;
}
HANDLER(mgBodyContainsPoint) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    lua_pushboolean(L, b && C->world().pointInBody(b, a.v3(1))); // raw (untransformed) point, as in the original
    return 1;
}
HANDLER(mgIsConnected) {
    PROLOG;
    Body* x = (Body*)a.p(0);
    Body* y = (Body*)a.p(1);
    if (!x) x = C->groundBody();
    if (!y) y = C->groundBody();
    lua_pushboolean(L, C->world().connected(x, y));
    return 1;
}
HANDLER(mgBodyIsBroken) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    lua_pushboolean(L, b && b->brokenFlag);
    return 1;
}
HANDLER(mgShapeIsBroken) {
    PROLOG;
    Shape* s = (Shape*)a.p(0);
    lua_pushboolean(L, s && s->broken);
    return 1;
}
HANDLER(mgGetShapeBody) {
    PROLOG;
    Shape* s = (Shape*)a.p(0);
    if (!s) return 0;
    lua_pushlightuserdata(L, s->body);
    return 1;
}
HANDLER(mgGetJointBodies) {
    PROLOG;
    Joint* j = (Joint*)a.p(0);
    if (!j) return 0;
    lua_pushlightuserdata(L, j->a);
    lua_pushlightuserdata(L, j->b);
    return 2;
}
HANDLER(mgGetPos) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    pushV3(L, S->toLocal(b->pos));
    return 3;
}
HANDLER(mgGetRot) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    Mat3 R = Mat3::fromQuat(poseRot(S).conj() * b->rot);
    float sb = clampf(R.m[2], -1, 1);
    float ang_b = std::asin(sb), ang_a = std::atan2(-R.m[5], R.m[8]), ang_c = std::atan2(-R.m[1], R.m[0]);
    lua_pushnumber(L, ang_a);
    lua_pushnumber(L, ang_b);
    lua_pushnumber(L, ang_c);
    return 3;
}
HANDLER(mgBodyPointToWorld) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    pushV3(L, S->toLocal(b->pointToWorld(a.v3(1))));
    return 3;
}
HANDLER(mgBodyVecToWorld) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    pushV3(L, b->rot.rotate(a.v3(1)));
    return 3;
}
HANDLER(mgHasBoss) {
    PROLOG;
    lua_pushboolean(L, C->hasBoss());
    return 1;
}
HANDLER(mgIsClosestBoss) {
    PROLOG;
    lua_pushboolean(L, C->isClosestBoss());
    return 1;
}
HANDLER(mgIsCleared) {
    PROLOG;
    lua_pushboolean(L, C->isCleared());
    return 1;
}
HANDLER(mgGetGravity) {
    PROLOG;
    lua_pushnumber(L, C->gravityScale() * 10.0f);
    return 1;
}
HANDLER(mgGetSplitExtraRoomLength) {
    PROLOG;
    lua_pushnumber(L, C->splitExtraRoomLength());
    return 1;
}

// ---- scoring ----
HANDLER(mgScore) {
    PROLOG;
    C->setScore(a.i(0), a.n >= 2, a.v3(1));
    return 0;
}
HANDLER(mgGlow) {
    PROLOG;
    C->setGlow(a.v3(0));
    return 0;
}
HANDLER(mgClear) {
    PROLOG;
    C->clear();
    return 0;
}
HANDLER(mgCameraShake) {
    PROLOG;
    C->cameraShake(a.f(0));
    return 0;
}
// the original only sets a room flag that nothing reads
HANDLER(mgDoorOpened) { return 0; }

// ---- builder state ----
HANDLER(mgShapeReset) {
    PROLOG;
    Material m;
    S->mat = m;
    S->shapeScale = Vec3(1, 1, 1);
    return 0;
}
HANDLER(mgMaterial) {
    PROLOG;
    if (const Material* m = C->findMaterial(a.s(0))) S->mat = *m;
    return 0;
}
HANDLER(mgDensity) {
    PROLOG;
    S->mat.density = a.f(0);
    return 0;
}
HANDLER(mgFriction) {
    PROLOG;
    S->mat.friction = a.f(0);
    return 0;
}
HANDLER(mgRestitution) {
    PROLOG;
    S->mat.restitution = a.f(0);
    return 0;
}
HANDLER(mgForceLimit) {
    PROLOG;
    S->mat.forceLimit = a.f(0);
    return 0;
}
HANDLER(mgRoundness) {
    PROLOG;
    S->mat.roundness = a.f(0);
    return 0;
}
HANDLER(mgCategory) {
    PROLOG;
    S->mat.category = (uint8_t)a.i(0);
    return 0;
}
HANDLER(mgMask) {
    PROLOG;
    S->mat.mask = (uint8_t)a.i(0);
    return 0;
}
HANDLER(mgStatic) {
    PROLOG;
    S->mat.isStatic = (uint8_t)a.i(0);
    return 0;
}
HANDLER(mgColor) {
    PROLOG;
    S->mat.color[0] = a.f(0);
    S->mat.color[1] = a.f(1);
    S->mat.color[2] = a.f(2);
    S->mat.color[3] = a.n >= 4 ? a.f(3) : 1.0f;
    return 0;
}
HANDLER(mgTexture) {
    PROLOG;
    S->mat.texEnabled = a.b(0);
    S->mat.texScale[0] = a.f(1);
    S->mat.texScale[1] = a.f(2);
    S->mat.texOffset[0] = a.f(3);
    S->mat.texOffset[1] = a.f(4);
    return 0;
}
HANDLER(mgShapeScale) {
    PROLOG;
    if (a.n == 3) S->shapeScale = Vec3(a.f(0), a.f(1), a.f(2));
    else S->shapeScale = Vec3(a.f(0), a.f(0), a.f(0));
    return 0;
}
HANDLER(mgShapeChamfer) {
    PROLOG;
    S->chamfer = a.f(0);
    return 0;
}
HANDLER(mgHideEdges) {
    PROLOG;
    S->mat.hideEdges = a.b(0);
    return 0;
}

// ---- bodies / shapes ----
HANDLER(mgCreateBody) {
    PROLOG;
    Vec3 p = a.n >= 1 ? a.v3(0) : Vec3();
    Quat q = a.n >= 4 ? Quat::fromEuler(a.f(3), a.f(4), a.f(5)) : Quat();
    Body* b = C->world().createBody(S->toWorld(p), (poseRot(S) * q).normalized());
    C->adoptBody(b);
    lua_pushlightuserdata(L, b);
    return 1;
}
HANDLER(mgCreateBox) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    Vec3 half(std::fabs(a.f(1)), std::fabs(a.f(2)), std::fabs(a.f(3)));
    Vec3 off = a.n >= 5 ? a.v3(4) : Vec3();
    Quat q = a.n >= 8 ? Quat::fromEuler(a.f(7), a.f(8), a.f(9)) : Quat();
    Polyhedron p = Polyhedron::box(half, S->chamfer);
    p.transform(S->shapeScale, q, off);
    Shape* sh = C->world().addPoly(b, std::move(p), S->mat);
    if (sh) {
        sh->chamfer = S->chamfer;
        C->world().computeMass(b);
        lua_pushlightuserdata(L, sh);
        return 1;
    }
    return 0;
}
HANDLER(mgCreateCylinder) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    Vec3 off = a.n >= 4 ? a.v3(3) : Vec3();
    Quat q = a.n >= 7 ? Quat::fromEuler(a.f(6), a.f(7), a.f(8)) : Quat();
    Polyhedron p = Polyhedron::cylinderX(std::fabs(a.f(1)), std::fabs(a.f(2)), 24, S->chamfer);
    p.transform(S->shapeScale, q, off);
    Shape* sh = C->world().addPoly(b, std::move(p), S->mat);
    if (sh) {
        sh->chamfer = S->chamfer;
        if (sh->mat.forceLimit >= FLT_MAX) { // unbreakable cylinders render round
            sh->mat.roundness = 1;
            sh->cyl = true;
            sh->cylAxis = normalize(q.rotate(Vec3(1, 0, 0)));
            sh->cylCentre = off;
            sh->cylHalfLen = std::fabs(a.f(2)) * std::fabs(S->shapeScale.x);
        }
        C->world().computeMass(b);
        lua_pushlightuserdata(L, sh);
        return 1;
    }
    return 0;
}
HANDLER(mgCreateMesh) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    const std::vector<Vec3>* hull = C->findConvex(a.s(1));
    if (!hull) return 0;
    Vec3 off = a.n >= 3 ? a.v3(2) : Vec3();
    Quat q = a.n >= 6 ? Quat::fromEuler(a.f(5), a.f(6), a.f(7)) : Quat();
    Polyhedron p = Polyhedron::hull(*hull);
    if (!p.valid()) return 0;
    p.transform(S->shapeScale, q, off);
    if (C->world().addPoly(b, std::move(p), S->mat)) C->world().computeMass(b);
    return 0;
}
HANDLER(mgClearShapes) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (b) C->world().clearShapes(b);
    return 0;
}
HANDLER(mgRemoveShape) {
    PROLOG;
    Shape* s = (Shape*)a.p(0);
    if (s) C->world().removeShape(s);
    return 0;
}
HANDLER(mgTranslateShape) {
    PROLOG;
    Shape* s = (Shape*)a.p(0);
    if (!s) return 0;
    Vec3 t = a.v3(1);
    bool all = a.n >= 5 && a.b(4);
    Body* b = s->body;
    for (auto& sp : b->shapes)
        if (sp.get() == s || (all && sp->origin == s)) {
            for (auto& v : sp->poly.v) v += t;
            sp->poly.rebuildDerived();
        }
    C->world().refreshShapes(b);
    return 0;
}
HANDLER(mgSetShapeColor) {
    PROLOG;
    Shape* s = (Shape*)a.p(0);
    if (!s) return 0;
    for (auto& bp : C->world().bodies())
        for (auto& sp : bp->shapes)
            if (sp.get() == s || sp->origin == s) {
                sp->mat.color[0] = a.f(1);
                sp->mat.color[1] = a.f(2);
                sp->mat.color[2] = a.f(3);
            }
    return 0;
}
HANDLER(mgSetShapeMask) {
    PROLOG;
    Shape* s = (Shape*)a.p(0);
    if (!s) return 0;
    s->mat.mask = (uint8_t)a.i(1);
    if (a.n >= 3) s->mat.category = (uint8_t)a.i(2);
    return 0;
}
HANDLER(mgSetPos) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (b) C->world().setTransform(b, S->toWorld(a.v3(1)), b->rot);
    return 0;
}
HANDLER(mgSetVel) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (b) b->vel = S->dirToWorld(a.v3(1));
    return 0;
}
HANDLER(mgSetAngVel) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (b) b->angVel = S->dirToWorld(a.v3(1));
    return 0;
}
HANDLER(mgClonePose) {
    PROLOG;
    Body *d = (Body*)a.p(0), *s = (Body*)a.p(1);
    if (d && s) {
        C->world().setTransform(d, s->pos, s->rot);
        d->vel = s->vel;
        d->angVel = s->angVel;
    }
    return 0;
}
Motion* motionOf(Body* b) {
    if (!b->motion) {
        b->motion.reset(new Motion);
        b->motion->pos = b->pos;
        b->motion->rot = Quat();
    }
    return b->motion.get();
}
HANDLER(mgMove) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    Motion* m = motionOf(b);
    m->pos = S->toWorld(a.v3(1));
    m->linForce = a.n < 5 ? FLT_MAX : a.f(4);
    m->linMaxSpeed = a.n >= 6 ? a.f(5) : FLT_MAX;
    return 0;
}
HANDLER(mgRotate) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    Motion* m = motionOf(b);
    m->rot = (poseRot(S) * Quat::fromEuler(a.f(1), a.f(2), a.f(3))).normalized();
    m->angForce = a.n < 5 ? FLT_MAX : a.f(4);
    m->angMaxSpeed = a.n >= 6 ? a.f(5) : FLT_MAX;
    return 0;
}
HANDLER(mgReleaseMotion) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (b) b->motion.reset();
    return 0;
}
HANDLER(mgBodyDecal) {
    PROLOG;
    Body* b = (Body*)a.p(0);
    if (!b) return 0;
    Vec3 off = a.n >= 5 ? a.v3(4) : Vec3();
    Quat q = a.n >= 8 ? Quat::fromEuler(a.f(7), a.f(8), a.f(9)) : Quat();
    // world pose = body.pos + body.rot*off, body.rot*q (resolved when drawn)
    C->bodyDecal(b, off, q, a.i(1), a.f(2), a.f(3));
    return 0;
}
HANDLER(mgCreateButton) {
    PROLOG;
    Button* bt = C->createButton(S->toWorld(a.v3(0)));
    lua_pushlightuserdata(L, bt);
    return 1;
}
HANDLER(mgIsButtonDown) {
    PROLOG;
    Button* b = (Button*)a.p(0);
    bool edge = a.n >= 2 && a.b(1);
    lua_pushboolean(L, b && (edge ? (b->down && !b->prevDown) : b->down));
    return 1;
}
HANDLER(mgIsButtonUp) {
    PROLOG;
    Button* b = (Button*)a.p(0);
    bool edge = a.n >= 2 && a.b(1);
    lua_pushboolean(L, b && (edge ? (!b->down && b->prevDown) : !b->down));
    return 1;
}
HANDLER(mgSetButtonDown) {
    PROLOG;
    Button* b = (Button*)a.p(0);
    if (b) b->down = b->prevDown = a.b(1);
    return 0;
}

// ---- joints ----
Body* jb(LuaArgs& a, int i, ScriptContext* C) {
    Body* b = (Body*)a.p(i);
    return b ? b : C->groundBody();
}
HANDLER(mgCreateBallJoint) {
    PROLOG;
    Body *A = jb(a, 0, C), *B = jb(a, 1, C);
    Vec3 p = S->toWorld(a.v3(2));
    Joint* j = C->world().createJoint(JointType::Ball, A, B);
    j->anchorA = A->pointToLocal(p);
    j->anchorB = B->pointToLocal(p);
    lua_pushlightuserdata(L, j);
    return 1;
}
int makeAxisJoint(lua_State* L, JointType t) {
    PROLOG;
    Body *A = jb(a, 0, C), *B = jb(a, 1, C);
    Vec3 p = S->toWorld(a.v3(2));
    Vec3 ax = normalize(S->dirToWorld(a.v3(5)));
    Joint* j = C->world().createJoint(t, A, B);
    j->anchorA = A->pointToLocal(p);
    j->anchorB = B->pointToLocal(p);
    j->axisA = A->rot.conj().rotate(ax);
    j->axisB = B->rot.conj().rotate(ax);
    lua_pushlightuserdata(L, j);
    return 1;
}
HANDLER(mgCreateHingeJoint) { return makeAxisJoint(L, JointType::Hinge); }
HANDLER(mgCreateSliderJoint) { return makeAxisJoint(L, JointType::Slider); }
HANDLER(mgCreateRope) {
    PROLOG;
    Body *A = jb(a, 0, C), *B = jb(a, 1, C);
    Vec3 p0 = S->toWorld(a.v3(2)), p1 = S->toWorld(a.v3(5));
    Joint* j = C->world().createJoint(JointType::Rope, A, B);
    j->anchorA = A->pointToLocal(p0);
    j->anchorB = B->pointToLocal(p1);
    j->ropeLength = length(p0 - p1) + a.f(8);
    lua_pushlightuserdata(L, j);
    return 1;
}
HANDLER(mgJointMotor) {
    PROLOG;
    Joint* j = (Joint*)a.p(0);
    if (j && j->type == JointType::Hinge) {
        j->motorVel = a.f(1);
        j->motorForce = a.f(2);
    }
    return 0;
}

// ---- effects ----
HANDLER(mgCreateSound) {
    PROLOG;
    int h = C->createSound(a.s(0));
    S->sounds.push_back(h);
    lua_pushinteger(L, (lua_Integer)S->sounds.size() - 1);
    return 1;
}
HANDLER(mgPlaySound) {
    PROLOG;
    int idx = a.i(0);
    if (idx < 0 || idx >= (int)S->sounds.size() || S->sounds[idx] < 0) return 0;
    float vol = 1, pitch = 1;
    if (a.n >= 2) vol = clampf(a.f(1), 0, 10);
    if (a.n > 2) pitch = clampf(a.f(2), 0.01f, 100);
    C->playSound(S->sounds[idx], vol, pitch, C->posePos());
    return 0;
}
HANDLER(mgSmoke) {
    PROLOG;
    C->smoke(S->toWorld(a.v3(0)), S->dirToWorld(a.v3(3)));
    return 0;
}
HANDLER(mgLaser) {
    PROLOG;
    C->laser(S->toWorld(a.v3(0)), S->dirToWorld(a.v3(3)));
    return 0;
}
HANDLER(mgPowerUp) {
    PROLOG;
    C->powerUp(a.s(0), S->toWorld(a.v3(1)));
    return 0;
}

// ---- room ----
HANDLER(mgSegment) {
    PROLOG;
    if (!C->isRoom()) return 0;
    lua_pushnumber(L, C->createSegment(a.s(0), a.f(1)));
    return 1;
}
HANDLER(mgLength) {
    PROLOG;
    C->roomLength(a.f(0));
    return 0;
}
HANDLER(mgMusic) {
    PROLOG;
    C->roomMusic(a.s(0));
    return 0;
}
HANDLER(mgReverb) {
    PROLOG;
    C->roomReverb(a.f(0), a.f(1), a.f(2));
    return 0;
}
HANDLER(mgLowPass) {
    PROLOG;
    C->roomLowPass(a.f(0));
    return 0;
}
HANDLER(mgEcho) {
    PROLOG;
    C->roomEcho(a.f(0), a.f(1), a.f(2), a.f(3));
    return 0;
}
HANDLER(mgFogColor) {
    PROLOG;
    Vec3 c1 = a.v3(0);
    Vec3 c2 = a.n >= 4 ? a.v3(3) : c1;
    C->roomFog(c1, c2);
    return 0;
}
HANDLER(mgGravity) {
    PROLOG;
    C->roomGravity(a.f(0));
    return 0;
}
HANDLER(mgSetDifficulty) {
    PROLOG;
    C->roomDifficulty(a.f(0));
    return 0;
}
HANDLER(mgSetRotation) {
    PROLOG;
    C->roomRotation((int)a.f(0), a.f(1));
    return 0;
}
HANDLER(mgParticles) {
    PROLOG;
    C->roomParticles(a.s(0));
    return 0;
}
HANDLER(mgSpeedMultiplier) {
    PROLOG;
    C->roomSpeedMultiplier(a.f(0));
    return 0;
}
HANDLER(mgBox) {
    PROLOG;
    C->roomBox(a.v3(0), a.v3(3), S->mat);
    return 0;
}
HANDLER(mgObstacle) {
    PROLOG;
    ObstacleDefRequest r;
    r.type = a.s(0);
    r.pos = a.v3(1);
    if (a.n >= 5) r.euler = a.v3(4);
    for (int i = 4; i < a.n; i++) {
        std::string s = a.s(i);
        size_t eq = s.find('=');
        if (eq != std::string::npos) r.params.emplace_back(s.substr(0, eq), s.substr(eq + 1));
    }
    C->addObstacle(r);
    return 0;
}

struct Reg {
    const char* name;
    lua_CFunction fn;
};
const Reg kApi[] = {
    {"mgTime", mgTime},
    {"mgGetTimeStep", mgGetTimeStep},
    {"mgObstacleTime", mgObstacleTime},
    {"mgCameraDistance", mgCameraDistance},
    {"mgGetCameraPos", mgGetCameraPos},
    {"mgGetCameraSpeed", mgGetCameraSpeed},
    {"mgGetBeat", mgGetBeat},
    {"mgGetBeatFloat", mgGetBeatFloat},
    {"mgGetString", mgGetString},
    {"mgGetFloat", mgGetFloat},
    {"mgGetInt", mgGetInt},
    {"mgGetBool", mgGetBool},
    {"mgGetColor", mgGetColor},
    {"mgGet", mgGet},
    {"mgSet", mgSet},
    {"mgDebug", mgDebug},
    {"mgRandomize", mgRandomize},
    {"mgRndInt", mgRndInt},
    {"mgRndFloat", mgRndFloat},
    {"mgRndBool", mgRndBool},
    {"mgRaycast", mgRaycast},
    {"mgCanPass", mgCanPass},
    {"mgBoxOverlap", mgBoxOverlap},
    {"mgBodyContainsPoint", mgBodyContainsPoint},
    {"mgIsConnected", mgIsConnected},
    {"mgBodyIsBroken", mgBodyIsBroken},
    {"mgShapeIsBroken", mgShapeIsBroken},
    {"mgGetShapeBody", mgGetShapeBody},
    {"mgGetJointBodies", mgGetJointBodies},
    {"mgGetPos", mgGetPos},
    {"mgGetRot", mgGetRot},
    {"mgBodyPointToWorld", mgBodyPointToWorld},
    {"mgBodyVecToWorld", mgBodyVecToWorld},
    {"mgHasBoss", mgHasBoss},
    {"mgIsClosestBoss", mgIsClosestBoss},
    {"mgIsCleared", mgIsCleared},
    {"mgGetGravity", mgGetGravity},
    {"mgGetSplitExtraRoomLength", mgGetSplitExtraRoomLength},
    {"mgScore", mgScore},
    {"mgGlow", mgGlow},
    {"mgClear", mgClear},
    {"mgCameraShake", mgCameraShake},
    {"mgDoorOpened", mgDoorOpened},
    {"mgShapeReset", mgShapeReset},
    {"mgMaterial", mgMaterial},
    {"mgDensity", mgDensity},
    {"mgFriction", mgFriction},
    {"mgRestitution", mgRestitution},
    {"mgForceLimit", mgForceLimit},
    {"mgRoundness", mgRoundness},
    {"mgCategory", mgCategory},
    {"mgMask", mgMask},
    {"mgStatic", mgStatic},
    {"mgColor", mgColor},
    {"mgTexture", mgTexture},
    {"mgShapeScale", mgShapeScale},
    {"mgShapeChamfer", mgShapeChamfer},
    {"mgHideEdges", mgHideEdges},
    {"mgCreateBody", mgCreateBody},
    {"mgCreateBox", mgCreateBox},
    {"mgCreateCylinder", mgCreateCylinder},
    {"mgCreateMesh", mgCreateMesh},
    {"mgClearShapes", mgClearShapes},
    {"mgRemoveShape", mgRemoveShape},
    {"mgTranslateShape", mgTranslateShape},
    {"mgSetShapeColor", mgSetShapeColor},
    {"mgSetShapeMask", mgSetShapeMask},
    {"mgSetPos", mgSetPos},
    {"mgSetVel", mgSetVel},
    {"mgSetAngVel", mgSetAngVel},
    {"mgClonePose", mgClonePose},
    {"mgMove", mgMove},
    {"mgRotate", mgRotate},
    {"mgReleaseMotion", mgReleaseMotion},
    {"mgBodyDecal", mgBodyDecal},
    {"mgCreateButton", mgCreateButton},
    {"mgIsButtonDown", mgIsButtonDown},
    {"mgIsButtonUp", mgIsButtonUp},
    {"mgSetButtonDown", mgSetButtonDown},
    {"mgCreateBallJoint", mgCreateBallJoint},
    {"mgCreateHingeJoint", mgCreateHingeJoint},
    {"mgCreateSliderJoint", mgCreateSliderJoint},
    {"mgCreateRope", mgCreateRope},
    {"mgJointMotor", mgJointMotor},
    {"mgCreateSound", mgCreateSound},
    {"mgPlaySound", mgPlaySound},
    {"mgSmoke", mgSmoke},
    {"mgLaser", mgLaser},
    {"mgPowerUp", mgPowerUp},
    {"mgSegment", mgSegment},
    {"mgLength", mgLength},
    {"mgMusic", mgMusic},
    {"mgReverb", mgReverb},
    {"mgLowPass", mgLowPass},
    {"mgEcho", mgEcho},
    {"mgFogColor", mgFogColor},
    {"mgGravity", mgGravity},
    {"mgSetDifficulty", mgSetDifficulty},
    {"mgSetRotation", mgSetRotation},
    {"mgParticles", mgParticles},
    {"mgSpeedMultiplier", mgSpeedMultiplier},
    {"mgBox", mgBox},
    {"mgObstacle", mgObstacle},
};

} // namespace

// ---------------------------------------------------------------- LevelScript

LevelScript::LevelScript(ScriptContext* c, std::vector<Param> p) : ctx(c), params(std::move(p)) {}
LevelScript::~LevelScript() = default;

const std::string* LevelScript::param(const std::string& name) const {
    const std::string* r = nullptr;
    for (auto& p : params)
        if (p.first == name) r = &p.second; // last match wins
    return r;
}
Vec3 LevelScript::toWorld(const Vec3& p) const { return ctx->posePos() + ctx->poseRot().rotate(p); }
Vec3 LevelScript::dirToWorld(const Vec3& v) const { return ctx->poseRot().rotate(v); }
Vec3 LevelScript::toLocal(const Vec3& w) const { return ctx->poseRot().conj().rotate(w - ctx->posePos()); }

void LevelScript::registerApi() {
    lua_State* L = host_->L();
    for (const Reg& r : kApi) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, r.fn, 1);
        lua_setglobal(L, r.name);
    }
}

bool LevelScript::load(const std::string& source, const std::string& prelude, const std::string& chunkName,
                       uint32_t seed) {
    host_.reset(new LuaHost);
    std::string chunk = source + "\n" + prelude;
    std::string err;
    // The chunk runs BEFORE the mg* functions exist: top-level statements may only define things.
    if (!host_->run(chunk, "@" + chunkName, &err)) {
        ctx->log(chunkName + ": " + err);
        host_.reset();
        return false;
    }
    registerApi();
    rng.setSeed(seed);
    call("init");
    return true;
}

void LevelScript::call(const char* fn) {
    if (!host_) return;
    lua_State* L = host_->L();
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        ctx->log(std::string(fn) + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
        lua_pop(L, 1);
    }
}

void LevelScript::tick() { call("tick"); }
void LevelScript::draw() { call("draw"); }

} // namespace sh
