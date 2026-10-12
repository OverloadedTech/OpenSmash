// Rigid-body world (physics/world.h): resting contact, glass breaking, joints, motors, raycasts and friction.
#include <cstdio>
#include "physics/world.h"
#include "check.h"
using namespace sh;

static Material mat(float density = 1, float friction = 0.9f, float rest = 0, float limit = FLT_MAX, int st = 0) {
    Material m;
    m.density = density;
    m.friction = friction;
    m.restitution = rest;
    m.forceLimit = limit;
    m.isStatic = (uint8_t)st;
    m.color[3] = 1;
    return m;
}

int main() {
    const float dt = 1.0f / 60;
    { // box resting on static floor
        World w;
        Body* floor = w.createBody(Vec3(0, 0, 0), Quat());
        w.addBox(floor, Vec3(10, 0.5f, 10), Vec3(0, -0.5f, 0), Quat(), mat(1, 0.9f, 0, FLT_MAX, 1), 0);
        CHECK(!floor->dynamic);
        Body* b = w.createBody(Vec3(0, 1.0f, 0), Quat());
        w.addBox(b, Vec3(0.5f, 0.5f, 0.5f), Vec3(), Quat(), mat(), 0);
        CHECK(b->dynamic);
        CHECK(std::fabs(b->mass - 1.0f) < 1e-3f);
        for (int i = 0; i < 240; i++) w.step(dt);
        float y = b->pos.y - 0.5f; // bottom
        printf("box rest gap=%.4f v=%.4f w=%.4f\n", y, length(b->vel), length(b->angVel));
        CHECK(y > 0.0f && y < 0.05f);
        CHECK(length(b->vel) < 0.05f);
        CHECK(length(b->angVel) < 0.05f);
        CHECK(std::fabs(b->pos.x) < 0.05f && std::fabs(b->pos.z) < 0.05f);
    }
    { // tilted box settles flat-ish and stays
        World w;
        Body* floor = w.createBody(Vec3(), Quat());
        w.addBox(floor, Vec3(10, 0.5f, 10), Vec3(0, -0.5f, 0), Quat(), mat(1, 0.9f, 0, FLT_MAX, 1), 0);
        Body* b = w.createBody(Vec3(0, 2.0f, 0), Quat::axisAngle(Vec3(1, 0, 1), 0.4f));
        w.addBox(b, Vec3(0.5f, 0.25f, 0.5f), Vec3(), Quat(), mat(), 0);
        for (int i = 0; i < 400; i++) w.step(dt);
        printf("tilted y=%.3f v=%.4f\n", b->pos.y, length(b->vel));
        CHECK(length(b->vel) < 0.05f);
        CHECK(b->pos.y > 0.2f && b->pos.y < 0.4f);
    }
    { // ball (v=20) through a glass pane
        World w;
        w.gravity = Vec3(0, 0, 0);
        Body* pane = w.createBody(Vec3(0, 1, -5), Quat());
        Material g = mat(1, 0.7f, 0.3f, 0.1f);
        g.color[0] = 0.5f;
        g.color[1] = 1;
        g.color[2] = 0.5f;
        g.type = kMaterialGlass;
        Shape* ps = w.addBox(pane, Vec3(1, 1, 0.05f), Vec3(), Quat(), g, 0);
        Body* ball = w.createBody(Vec3(0, 1, 0), Quat());
        ball->isBall = true;
        ball->id = 1;
        Material bm = mat(5, 1, 0.4f);
        bm.category = kCategoryBall;
        w.addBox(ball, Vec3(0.1f, 0.1f, 0.1f), Vec3(), Quat(), bm, 0);
        CHECK(std::fabs(ball->mass - 0.04f) < 1e-4f);
        ball->vel = Vec3(0, 0, -20);
        ball->age = 1;
        pane->age = 1;
        int breaks = 0;
        bool firstByBall = false;
        w.onBreak = [&](const BreakEvent& e) {
            if (breaks++ == 0) firstByBall = e.byBall;
        };
        for (int i = 0; i < 40; i++) w.step(dt);
        printf("ball z=%.2f vz=%.2f breaks=%d bodies=%zu pane.broken=%d\n", ball->pos.z, ball->vel.z, breaks,
               w.bodies().size(), (int)pane->brokenFlag);
        CHECK(breaks >= 1);
        CHECK(firstByBall);
        CHECK(ball->pos.z < -5.5f);
        CHECK(ball->vel.z < -12.0f);
        CHECK(ball->vel.z > -19.0f);
        CHECK(w.bodies().size() > 3);
        CHECK(ps->body != nullptr);
    }
    { // pendulum on a hinge swings and keeps its pivot
        World w;
        Body* beam = w.createBody(Vec3(0, 5, 0), Quat());
        w.addBox(beam, Vec3(0.1f, 1.0f, 0.1f), Vec3(0, -1, 0), Quat(), mat(), 0);
        Joint* j = w.createJoint(JointType::Hinge, w.ground(), beam);
        j->anchorA = Vec3(0, 5, 0);
        j->anchorB = Vec3(0, 0, 0);
        j->axisA = Vec3(0, 0, 1);
        j->axisB = Vec3(0, 0, 1);
        beam->vel = Vec3(2, 0, 0);
        float maxDrift = 0, maxX = 0;
        for (int i = 0; i < 300; i++) {
            w.step(dt);
            maxDrift = std::max(maxDrift, length(beam->pointToWorld(Vec3(0, 0, 0)) - Vec3(0, 5, 0)));
            maxX = std::max(maxX, std::fabs(beam->worldCom().x));
        }
        printf("pendulum drift=%.4f maxX=%.3f\n", maxDrift, maxX);
        CHECK(maxDrift < 0.15f);
        CHECK(maxX > 0.2f);
    }
    { // motor: mgMove drives a body up against gravity, rotation locked (static=2)
        World w;
        Body* lift = w.createBody(Vec3(0, 0, 0), Quat());
        w.addBox(lift, Vec3(1, 0.1f, 1), Vec3(), Quat(), mat(100, 0.7f, 0, FLT_MAX, 2), 0);
        CHECK(lift->dynamic);
        CHECK(lift->rotLocked);
        lift->motion.reset(new Motion);
        lift->motion->pos = Vec3(0, 3, 0);
        lift->motion->rot = Quat();
        lift->motion->linForce = 10;
        lift->motion->linMaxSpeed = 1.5f;
        for (int i = 0; i < 240; i++) w.step(dt);
        printf("lift y=%.3f\n", lift->pos.y);
        CHECK(std::fabs(lift->pos.y - 3) < 0.05f);
    }
    { // rope holds a hanging box
        World w;
        Body* b = w.createBody(Vec3(0, 3, 0), Quat());
        w.addBox(b, Vec3(0.2f, 0.2f, 0.2f), Vec3(), Quat(), mat(), 0);
        Joint* j = w.createJoint(JointType::Rope, w.ground(), b);
        j->anchorA = Vec3(0, 5, 0);
        j->anchorB = Vec3(0, 0.2f, 0);
        j->ropeLength = 1.8f;
        for (int i = 0; i < 300; i++) w.step(dt);
        float len = length(b->pointToWorld(j->anchorB) - Vec3(0, 5, 0));
        printf("rope len=%.3f y=%.3f\n", len, b->pos.y);
        CHECK(len < 2.1f && b->pos.y > 2.5f);
    }
    { // raycast: touching/grazing rays miss, starts more than 1e-4 inside hit at t = 0 with the ray direction as normal
        World w;
        Body* room = w.createBody(Vec3(), Quat());
        // bottom face at y = 0
        w.addBox(room, Vec3(0.5f, 0.125f, 0.5f), Vec3(0, 0.125f, 0), Quat(), mat(1, 0.9f, 0, FLT_MAX, 1), 0);
        CHECK(!w.raycast(Vec3(0, 0, 0), Vec3(5, 0, 0), 0xff, nullptr).hit);  // co-op bar probe: along the bottom face
        CHECK(!w.raycast(Vec3(0, 0, 0), Vec3(0, -5, 0), 0xff, nullptr).hit); // on the face, heading out
        CHECK(!w.raycast(Vec3(0, -1, 0.5f), Vec3(0, 1, 0.5f), 0xff, nullptr).hit); // along the z = 0.5 side face
        RayHit h = w.raycast(Vec3(0, -1, 0), Vec3(0, 5e-5f, 0), 0xff, nullptr);    // ends just past the entry face
        CHECK(h.hit && h.normal.y < -0.99f && std::fabs(h.point.y) < 1e-4f);
        h = w.raycast(Vec3(0, 0.1f, 0), Vec3(3, 0.1f, 0), 0xff, nullptr); // deep inside
        CHECK(h.hit && h.t == 0 && h.normal.x > 0.99f);
        h = w.raycast(Vec3(0, 5e-5f, 0), Vec3(0, 1, 0), 0xff, nullptr); // within 1e-4 of the face, heading in
        CHECK(h.hit && h.t == 0 && h.normal.y > 0.99f);
        CHECK(!w.raycast(Vec3(0.5f, 0.25f, -2), Vec3(0.5f, 0.25f, 2), 0xff, nullptr).hit); // along an edge
    }
    { // friction anchor: resting contacts do not creep (2-box stack, slope below the friction angle)
        World w;
        Body* floor = w.createBody(Vec3(), Quat());
        w.addBox(floor, Vec3(10, 0.5f, 10), Vec3(0, -0.5f, 0), Quat(), mat(1, 0.9f, 0, FLT_MAX, 1), 0);
        Body* lo = w.createBody(Vec3(0, 0.25f, 0), Quat());
        w.addBox(lo, Vec3(0.25f, 0.25f, 0.25f), Vec3(), Quat(), mat(), 0);
        Body* hi = w.createBody(Vec3(0, 0.75f, 0), Quat());
        w.addBox(hi, Vec3(0.25f, 0.25f, 0.25f), Vec3(), Quat(), mat(), 0);
        for (int i = 0; i < 1200; i++) w.step(dt);
        printf("stack top y=%.3f x=%.4f z=%.4f\n", hi->pos.y, hi->pos.x, hi->pos.z);
        CHECK(hi->pos.y > 0.7f);
        CHECK(std::fabs(hi->pos.x) < 0.03f && std::fabs(hi->pos.z) < 0.03f);
        World s;
        Quat q = Quat::axisAngle(Vec3(0, 0, 1), 10 * kPi / 180);
        Body* slope = s.createBody(Vec3(), q);
        s.addBox(slope, Vec3(10, 0.5f, 10), Vec3(0, -0.5f, 0), Quat(), mat(1, 0.7f, 0, FLT_MAX, 1), 0);
        Body* b = s.createBody(q.rotate(Vec3(0, 0.251f, 0)), q);
        s.addBox(b, Vec3(0.25f, 0.25f, 0.25f), Vec3(), Quat(), mat(1, 0.7f), 0);
        for (int i = 0; i < 120; i++) s.step(dt);
        Vec3 p1 = b->pos;
        for (int i = 0; i < 480; i++) s.step(dt);
        printf("slope 10 deg creep %.5f m in 8 s\n", length(b->pos - p1));
        CHECK(length(b->pos - p1) < 0.002f);
    }
    printf(fails ? "world_test: %d failure(s)\n" : "world_test: ok\n", fails);
    return fails ? 1 : 0;
}
