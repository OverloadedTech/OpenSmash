#include "game/level.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/random.h"

namespace sh {

static const float kWhite[4] = {1, 1, 1, 1};

void LevelHost::log(const std::string& s) { fprintf(stderr, "%s\n", s.c_str()); }

static const char* const kPowerupNames[kPowerupCount] = {
    "",           "balls",   "barrel",      "slowmotion", "multiball", "ballfrenzy",
    "nitroballs", "freebie", "antigravity", "rewind",     "shield",    "homing"};

const char* powerupName(int type) { return type > 0 && type < kPowerupCount ? kPowerupNames[type] : ""; }
int powerupFromName(const std::string& name) {
    for (int i = 1; i < kPowerupCount; i++)
        if (name == kPowerupNames[i]) return i;
    return kPowerupNone;
}

static Vec3 parseV3(const std::string& s, const Vec3& def = Vec3()) {
    Vec3 v = def;
    sscanf(s.c_str(), "%f %f %f", &v.x, &v.y, &v.z);
    return v;
}

// smallest z of the body's shape bounds: the camera is past the whole body once cam.z <= this
static float minShapeZ(const Body& b) {
    float mz = 1e30f;
    for (auto& s : b.shapes) mz = std::min(mz, s->aabbMin.z);
    return mz;
}

// ===================================================================== Player

void Player::zero() {
    balls = 25;
    streak = 0;
    ballsP[0] = ballsP[1] = 25;
    streakP[0] = streakP[1] = 0;
    powerups.clear();
}

void Player::clearRunStats() {
    statBallsShot = statBallsHit = statBroken = statPeakBalls = statMaxStreak = statRooms = 0;
    statAccStreak = statAccBalls = 0;
}

void Player::loadCheckpoint(int idx) {
    zero();
    if (idx >= 1 && idx <= kCampaignLevels) {
        int b = ckBalls[mode][idx];
        if (b > 0) {
            balls = b;
            ballsP[0] = ballsP[1] = b;
        }
        streak = ckStreak[mode][idx];
    }
}

void Player::pushPowerup(int type) {
    if (powerups.size() >= 4) powerups.erase(powerups.begin());
    powerups.push_back(type);
}

int Player::popPowerup() {
    if (powerups.empty()) return 0;
    int t = powerups.back();
    powerups.pop_back();
    return t;
}

// ===================================================================== Obstacle

Obstacle::Obstacle(Level& lv, Room* r, bool isRoomScript, const Vec3& p, const Quat& q)
    : level(lv), room(r), roomScript(isRoomScript), pos(p), rot(q) {}
Obstacle::~Obstacle() = default;

World& Obstacle::world() { return level.world; }
Body* Obstacle::groundBody() { return room->body; }
void Obstacle::adoptBody(Body* b) {
    b->owner = this;
    bodies.push_back(b);
}
Button* Obstacle::createButton(const Vec3& p) {
    buttons.emplace_back(new Button);
    buttons.back()->pos = p;
    return buttons.back().get();
}
float Obstacle::levelTime() const { return level.time; }
float Obstacle::timeStep() const { return level.dt; }
Vec3 Obstacle::cameraPos() const { return level.cam; }
float Obstacle::cameraSpeed() const { return level.camSpeed; }
float Obstacle::beat(float lookahead) const {
    // beat clock: the music position if music is on (0 until the track starts), else the time since room creation
    float t = level.host && level.host->musicEnabled() ? level.host->musicLocation() : level.time - room->createdAtTime;
    float b = (lookahead * level.timeScale + t) * 2.0f;
    if (room != level.cur.get()) b -= 64.0f;
    return b;
}
std::string Obstacle::getProperty(const std::string& k) { return level.getProperty(k); }
void Obstacle::setProperty(const std::string& k, const std::string& v) { level.setProperty(k, v); }
const Material* Obstacle::findMaterial(const std::string& n) { return level.content.material(n); }
const std::vector<Vec3>* Obstacle::findConvex(const std::string& n) { return level.content.convex(n); }

void Obstacle::clear() {
    clearedAt = std::max(level.time, 1e-4f);
    if (scoreValue <= 0) return;
    int player = -1;
    if (level.player.mode == kModeVersus || level.player.mode == kModeCoop)
        for (Body* b : bodies)
            if (b->lastTouched >= 0) {
                player = b->lastTouched;
                break;
            }
    level.streakInc(player);
    level.host->sound("cleared:" + std::to_string(level.streakSoundStep % 12), 0.7f, 1.0f, false, Vec3());
    level.addScore(scoreValue, player);
}

void Obstacle::cameraShake(float a) { level.addShake(a); }

bool Room::hasBossAlive() const {
    for (auto& d : defs)
        if (d.obj && d.type.rfind("boss", 0) == 0 && d.obj->clearedAt == 0) return true;
    return false;
}
bool Obstacle::hasBoss() const { return room->hasBossAlive(); }
bool Obstacle::isClosestBoss() const {
    const Obstacle* best = nullptr;
    for (auto& d : room->defs)
        if (d.obj && d.type.rfind("boss", 0) == 0 && d.obj->clearedAt == 0 && (!best || d.obj->pos.z > best->pos.z))
            best = d.obj;
    return best == this;
}
// gravity comes from the current room, which is not always the obstacle's own room
float Obstacle::gravityScale() const { return level.cur ? level.cur->gravityScale : room->gravityScale; }
float Obstacle::splitExtraRoomLength() const { return clampf((level.player.roomIdx - 10) * 5.0f, 0, 120); }
bool Obstacle::canPass() const {
    // a ray along the obstacle's local z axis at the camera's flying height (local y = 1), from z = +10 to -10.
    // Passable if it hits none of this obstacle's shapes.
    Vec3 a = pos + rot.rotate(Vec3(0, 1, 10)), b = pos + rot.rotate(Vec3(0, 1, -10));
    RayHit h = level.world.raycast(a, b, 0xff, this);
    return !h.hit;
}
float Obstacle::createSegment(const std::string& name, float zOffset) { return room->createSegment(name, zOffset); }
void Obstacle::roomLength(float l) { room->length = l; }
void Obstacle::roomMusic(const std::string& s) { room->music = s; }
void Obstacle::roomReverb(float a, float b, float c) {
    room->reverb[0] = a;
    room->reverb[1] = b;
    room->reverb[2] = c;
}
void Obstacle::roomLowPass(float f) { room->lowPass = f; }
void Obstacle::roomEcho(float a, float b, float c, float d) {
    room->echo[0] = a;
    room->echo[1] = b;
    room->echo[2] = c;
    room->echo[3] = d;
}
void Obstacle::roomFog(const Vec3& a, const Vec3& b) {
    room->fogA = a;
    room->fogB = b;
}
void Obstacle::roomGravity(float g) { room->gravityScale = g; }
void Obstacle::roomDifficulty(float d) { room->difficulty = d; }
void Obstacle::roomRotation(int turns, float amp) {
    room->rotTurns = turns;
    room->rotAmp = amp;
}
void Obstacle::roomParticles(const std::string& n) { room->particles = n; }
void Obstacle::roomSpeedMultiplier(float f) { room->speedMultiplier = f; }
void Obstacle::roomBox(const Vec3& half, const Vec3& p, const Material& m) {
    level.world.addBox(room->body, half, p, Quat(), m, 0);
}
void Obstacle::addObstacle(const ObstacleDefRequest& r) {
    ObstacleDef d;
    d.type = r.type;
    d.pos = room->body->pointToLocal(pos + rot.rotate(r.pos));
    d.rot = Quat::fromEuler(r.euler.x, r.euler.y, r.euler.z);
    d.params = r.params;
    room->defs.push_back(std::move(d));
}
int Obstacle::createSound(const std::string& path) { return level.host->createSound(path); }
void Obstacle::playSound(int h, float vol, float pitch, const Vec3& p) {
    level.host->playSoundHandle(h, vol, pitch, p);
}
void Obstacle::smoke(const Vec3& p, const Vec3& v) { level.host->particles("smoke", p, v, kWhite, 1); }
void Obstacle::laser(const Vec3& o, const Vec3& d) { level.lasers.push_back({o, d, o + d * 100.0f}); }
void Obstacle::powerUp(const std::string& name, const Vec3& p) {
    if (int type = powerupFromName(name)) room->powerups.push_back({p, type, false});
}
void Obstacle::bodyDecal(const Body* b, const Vec3& off, const Quat& q, int sprite, float w, float h) {
    level.bodyDecals.push_back({b, off, q, sprite, w, h});
}
void Obstacle::log(const std::string& m) { level.host->log("[script " + type + "] " + m); }

// ===================================================================== Room

Room::Room(Level& lv, const RoomDef& def, int defIdx, float startOffset)
    : level(lv), name(def.type), defIdx(defIdx), start(startOffset) {
    createdAtTime = lv.time;
    body = lv.world.createBody(Vec3(0, 0, -startOffset), Quat());
    body->dynamic = false;
    for (auto& p : def.params)
        if (p.first == "difficulty") difficulty = (float)atof(p.second.c_str());
    // room script: rooms/<type>.lua (+ rooms/include.lua appended)
    std::string path = "rooms/" + def.type + ".lua";
    bool ok = false;
    std::string src = lv.content.vfs().readText(path, &ok);
    roomObstacle.reset(new Obstacle(lv, this, true, Vec3(0, 0, -startOffset), Quat()));
    roomObstacle->type = def.type;
    roomObstacle->adoptBody(body);
    if (!ok) {
        lv.host->log("room script not found: " + path);
        return;
    }
    std::string include = lv.content.vfs().readText("rooms/include.lua");
    roomObstacle->script.reset(new LevelScript(roomObstacle.get(), def.params));
    roomObstacle->script->mat.isStatic = 1;
    roomObstacle->script->mat.type = kMaterialRoom;
    roomObstacle->script->mat.category = kCategoryRoom;
    uint32_t seed = lv.player.mode == kModeTraining ? 0u : (uint32_t)lv.rnd_();
    roomObstacle->script->load(src, include, path, seed);
    // mode tint of the fog colours
    auto tint = [&](const Vec3& t) {
        fogA = Vec3(fogA.x * t.x, fogA.y * t.y, fogA.z * t.z);
        fogB = Vec3(fogB.x * t.x, fogB.y * t.y, fogB.z * t.z);
    };
    if (lv.player.mode == kModeExpert) tint(Vec3(1.1f, 0.8f, 0.6f));
    else if (lv.player.mode == kModeTraining) tint(Vec3(1.1f, 1.1f, 0.8f));
}

Room::~Room() {
    for (MeshBatch& b : batches)
        if (b.gpuId >= 0) level.host->releaseBatch(b.gpuId);
    // destroy every body owned by this room's obstacles
    std::vector<Body*> kill;
    for (auto& bp : level.world.bodies()) {
        Obstacle* ow = bp->owner;
        if (!ow) continue;
        if (ow == roomObstacle.get()) kill.push_back(bp.get());
        else
            for (auto& o : obstacles)
                if (o.get() == ow) {
                    kill.push_back(bp.get());
                    break;
                }
    }
    for (Body* b : kill) level.world.destroyBody(b);
}

float Room::createSegment(const std::string& segName, float zOff) {
    const SegmentDef* seg = level.content.segment(segName);
    if (!seg) return 0;
    Content& C = level.content;
    MeshBatch mb;
    mb.path = "segments/" + segName + ".mesh";
    mb.zOffset = zOff;
    mb.zMax = zOff;
    mb.zMin = zOff - seg->size.z;
    batches.push_back(mb);
    int mode = level.player.mode;
    bool versusLike = mode == kModeVersus || mode == kModeCoop;
    bool keepScore = true;
    if (versusLike)
        keepScore = (float)(level.rnd_() % 1000) / 1000.0f < clampf((40.0f - level.player.roomIdx) / 30.0f, 0, 1);

    Material roomMat;
    roomMat.density = 1;
    roomMat.friction = 1;
    roomMat.restitution = 0;
    roomMat.forceLimit = FLT_MAX;
    roomMat.type = kMaterialRoom;
    roomMat.category = kCategoryRoom;
    roomMat.mask = 0xff;
    roomMat.isStatic = 1;
    roomMat.color[0] = roomMat.color[1] = roomMat.color[2] = 1;
    roomMat.color[3] = 0; // invisible: drawn from the baked mesh

    for (auto& cp : seg->root->children) {
        const XmlNode& n = *cp;
        Vec3 p = parseV3(C.prop(n, "pos", "0 0 0"));
        p.z += zOff;
        if (n.name == "box") {
            Vec3 half = parseV3(C.prop(n, "size", "0 0 0"));
            if (half.x <= 0 || half.y <= 0 || half.z <= 0) continue;
            level.world.addBox(body, half, p, Quat(), roomMat, 0);
            // reflection helper geometry (floor-reflection pass)
            if (C.prop(n, "reflection", "0") != "0") {
                float y = p.y + half.y;
                const float cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                for (int i : {0, 1, 2, 0, 2, 3}) {
                    reflMask.push_back(p.x + cs[i][0] * half.x);
                    reflMask.push_back(y);
                    reflMask.push_back(p.z + cs[i][1] * half.z);
                }
            }
            float bottom = p.y - half.y;
            if (C.prop(n, "visible", "1") != "0" && bottom >= -0.1f && bottom < 2.0f) {
                float col[4] = {1, 1, 1, 1};
                int nw = sscanf(C.prop(n, "color", "1 1 1").c_str(), "%f %f %f %f", &col[0], &col[1], &col[2], &col[3]);
                if (nw < 4) col[3] = 1;
                uint8_t cb[4];
                for (int i = 0; i < 4; i++) cb[i] = (uint8_t)(clampf(col[i], 0, 1) * 255 + 0.5f);
                auto vtx = [&](float x, float yy, float z) {
                    float v[3] = {x, yy, z};
                    size_t o = reflGeom.size();
                    reflGeom.resize(o + 16);
                    memcpy(&reflGeom[o], v, 12);
                    memcpy(&reflGeom[o + 12], cb, 4);
                };
                float x0 = p.x - half.x, x1 = p.x + half.x, y0 = p.y - half.y, y1 = p.y + half.y, z0 = p.z - half.z,
                      z1 = p.z + half.z;
                // outward-facing CCW quads: -x, +x, -z, +z
                auto quad = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy,
                                float cz, float dx, float dy, float dz) {
                    vtx(ax, ay, az);
                    vtx(bx, by, bz);
                    vtx(cx, cy, cz);
                    vtx(ax, ay, az);
                    vtx(cx, cy, cz);
                    vtx(dx, dy, dz);
                };
                quad(x0, y0, z0, x0, y0, z1, x0, y1, z1, x0, y1, z0);
                quad(x1, y0, z1, x1, y0, z0, x1, y1, z0, x1, y1, z1);
                quad(x1, y0, z0, x0, y0, z0, x0, y1, z0, x1, y1, z0);
                quad(x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1);
            }
        } else if (n.name == "powerup") {
            float dmin = 0, dmax = 1;
            sscanf(C.prop(n, "difficulty", "0 1").c_str(), "%f %f", &dmin, &dmax);
            if (difficulty < dmin || difficulty > dmax) continue;
            std::string type = C.prop(n, "type", "");
            if (type == "multiball" || type == "balls") continue;
            roomObstacle->powerUp(type, body->pointToWorld(p));
        } else if (n.name == "decal") {
            float dmin = 0, dmax = 1;
            sscanf(C.prop(n, "difficulty", "0 1").c_str(), "%f %f", &dmin, &dmax);
            if (difficulty < dmin || difficulty > dmax) continue;
            if (level.props["game.graphics"] == "low") continue;
            Decal d;
            d.pos = p;
            Vec3 r = parseV3(C.prop(n, "rot", "0 0 0"));
            d.rot = Quat::fromEuler(r.x, r.y, r.z);
            sscanf(C.prop(n, "size", "1 1").c_str(), "%f %f", &d.w, &d.h);
            d.tile = atoi(C.prop(n, "tile", "0").c_str());
            d.color[0] = d.color[1] = d.color[2] = d.color[3] = 1;
            sscanf(C.prop(n, "color", "1 1 1 1").c_str(), "%f %f %f %f", &d.color[0], &d.color[1], &d.color[2],
                   &d.color[3]);
            d.blend = (float)atof(C.prop(n, "blend", "1").c_str());
            decals.push_back(d);
        } else if (n.name == "water") {
            // water entity at the room body position + pos. size = half extents in x/z, resolution is rounded
            Water w;
            w.pos = body->pos + p;
            float rx = 32, ry = 32;
            sscanf(C.prop(n, "size", "4 4").c_str(), "%f %f", &w.sx, &w.sz);
            sscanf(C.prop(n, "resolution", "32 32").c_str(), "%f %f", &rx, &ry);
            w.nx = std::max(2, (int)std::floor(rx + 0.5f));
            w.ny = std::max(2, (int)std::floor(ry + 0.5f));
            w.H.assign((size_t)(w.nx * w.ny), 0.0f);
            w.P = w.H;
            w.V = w.H;
            level.waters.push_back(std::move(w));
        } else if (n.name == "obstacle") {
            float dmin = 0, dmax = 1;
            sscanf(C.prop(n, "difficulty", "0 1").c_str(), "%f %f", &dmin, &dmax);
            if (difficulty < dmin || difficulty > dmax) continue;
            int maskBits = atoi(C.prop(n, "mode", "255").c_str());
            int bit = (mode == kModeZen) ? 2 : (1 << mode);
            if (!(bit & maskBits)) continue;
            std::string type = C.prop(n, "type", "");
            if (type.rfind("score", 0) == 0 && !keepScore) continue;
            ObstacleDef d;
            d.type = type;
            d.pos = p;
            Vec3 r = parseV3(C.prop(n, "rot", "0 0 0"));
            d.rot = Quat::fromEuler(r.x, r.y, r.z);
            for (int i = 0; i < 12; i++) {
                std::string v = C.prop(n, "param" + std::to_string(i), "");
                size_t eq = v.find('=');
                if (eq != std::string::npos) d.params.emplace_back(v.substr(0, eq), v.substr(eq + 1));
            }
            d.tutorial = C.prop(n, "tutorial", "");
            if (!d.tutorial.empty()) {
                d.tutorialOffset = parseV3(C.prop(n, "tutorialOffset", "0 0 0"));
                d.tutorialDistance = (float)atof(C.prop(n, "tutorialDistance", "0").c_str());
            }
            defs.push_back(std::move(d));
        }
    }
    return seg->size.z;
}

void Room::shiftZ(float dz) {
    start -= dz;
    level.world.setTransform(body, body->pos + Vec3(0, 0, dz), body->rot);
    roomObstacle->pos.z += dz;
    // buttons hold world positions, so they move with the recentre like every other entity
    for (auto& bt : roomObstacle->buttons) bt->pos.z += dz;
    for (auto& o : obstacles) {
        o->pos.z += dz;
        for (auto& bt : o->buttons) bt->pos.z += dz;
    }
    for (auto& p : powerups) p.pos.z += dz;
}

void Room::update() {
    Level& L = level;
    float camLocalZ = L.cam.z + start;
    if (roomObstacle->script) roomObstacle->script->tick();
    // spawn obstacles within 30 units (40 for elevators); elevators first
    for (int pass = 0; pass < 2; pass++)
        for (auto& d : defs) {
            if (d.created) continue;
            bool elev = d.type.find("elevator") != std::string::npos;
            if ((pass == 0) != elev) continue;
            float ahead = elev ? 40.0f : 30.0f;
            if (camLocalZ >= d.pos.z + ahead) continue;
            d.created = true;
            std::string path = "obstacles/" + d.type + ".lua";
            bool ok = false;
            std::string src = L.content.vfs().readText(path, &ok);
            if (!ok) {
                L.host->log("obstacle script not found: " + path);
                continue;
            }
            Vec3 wp = body->pointToWorld(d.pos);
            auto ob = std::make_unique<Obstacle>(L, this, false, wp, (body->rot * d.rot).normalized());
            ob->type = d.type;
            ob->script.reset(new LevelScript(ob.get(), d.params));
            uint32_t seed = L.player.mode == kModeTraining ? 0u : (uint32_t)L.rnd_();
            ob->script->load(src, "", path, seed);
            d.obj = ob.get();
            obstacles.push_back(std::move(ob));
        }
    // tick live obstacles
    for (auto& ob : obstacles) {
        if (ob->done) continue;
        ob->time += L.dt;
        if (ob->script) ob->script->tick();
        for (auto& bt : ob->buttons) {
            bt->prevDown = bt->down;
            if (!bt->down && !bt->tutorialRadius && L.host->tutorialActive()) bt->tutorialRadius = true;
            const float radius = bt->tutorialRadius ? 1.0f : 0.5f;
            if (!bt->down)
                for (auto& bp : L.world.bodies())
                    if (bp->isBall && sh::length(bp->pos - bt->pos) < radius) {
                        bp->hitBreakable = true;
                        bt->down = true;
                        L.host->sound("button", 1.0f, 1.0f, false, Vec3());
                        break;
                    }
        }
        // done when it owns bodies and all are behind the camera
        if (!ob->bodies.empty()) {
            bool allBehind = true;
            for (Body* b : ob->bodies) {
                if (b->shapes.empty()) continue;
                // z only, so a body that fell below the world counts as passed once the camera is past it
                if (L.cam.z > minShapeZ(*b)) {
                    allBehind = false;
                    break;
                }
            }
            if (allBehind) ob->passed = true;
        }
    }
    // scoring / miss for passed obstacles; destroy them
    for (auto& ob : obstacles) {
        if (!ob->passed || ob->done) continue;
        ob->done = true;
        if (ob->scoreValue != 0) {
            if (ob->clearedAt == 0) {
                L.streakAbort(-1);
                const char* side = ob->pos.x < -1 ? "left" : ob->pos.x > 1 ? "right" : "center";
                L.host->hudCommand(std::string("miss ") + side);
            }
        }
    }
    for (size_t i = 0; i < obstacles.size();) {
        Obstacle* ob = obstacles[i].get();
        if (!ob->done) {
            i++;
            continue;
        }
        std::vector<Body*> kill;
        for (auto& bp : L.world.bodies())
            if (bp->owner == ob) kill.push_back(bp.get());
        for (Body* b : kill) L.world.destroyBody(b);
        for (auto& d : defs)
            if (d.obj == ob) d.obj = nullptr;
        obstacles.erase(obstacles.begin() + i);
    }
    checkTutorialTriggers();
    // the current room's ambient particles run every tick until 90% of the room (the original thins them to every
    // 8th frame only while capturing video)
    if (L.cur.get() == this && !particles.empty() && progress(L.cam.z) < 0.9f) ambientParticles();
}

// Ambient particle presets, spawned around the camera into the "ambient" system ("bubbles" has its own), white, at
// exact positions and velocities. rnd is uniform (irnd excludes b), nrm is the original's centre-weighted random.
void Room::ambientParticles() {
    Level& L = level;
    std::mt19937& g = L.fxRnd_;
    auto rnd = [&](float a, float b) { return randRange(g, a, b); };
    auto nrm = [&](float a, float b) { return randCentered(g, a, b); };
    auto irnd = [&](int a, int b) { return a + (int)(g() % (uint32_t)(b - a)); };
    auto amb = [&](const Vec3& p, const Vec3& v) { L.host->spawnParticle("ambient", p, v, -1); };
    const Vec3 c = L.cam;
    const std::string& n = particles;
    const bool every90 = L.tickCount % 90 == 0;
    if (n == "bubbles") {
        if (!every90) return;
        Vec3 o = c + Vec3{rnd(-4, 4), 0, rnd(-5, 5) - 20};
        for (int i = irnd(20, 150); i > 0; i--) {
            Vec3 p = o + Vec3{nrm(-1, 1), nrm(-6, -4), nrm(-1, 1)};
            Vec3 v{nrm(-0.3f, 0.3f), nrm(-1, 3), nrm(-0.3f, 0.3f)};
            L.host->spawnParticle("bubbles", p, v, -1);
        }
    } else if (n == "sides") { // no shipped room uses it
        amb(c + Vec3{rnd(-4, -2), rnd(-5, 5), rnd(-5, 5) - 15}, Vec3());
        amb(c + Vec3{rnd(2, 4), rnd(-5, 5), rnd(-5, 5) - 15}, Vec3());
        float x = rnd(0, 1) > 0.5f ? rnd(-4, -2) : rnd(2, 4);
        amb(c + Vec3{x, rnd(-5, 5), rnd(-5, 5) - 15}, Vec3());
    } else if (n == "lowrising") amb(c + Vec3{rnd(-4, 4), rnd(-5, -3), rnd(-5, 5) - 20}, Vec3(0, 0.5f, 0));
    else if (n == "lowrising2") amb(c + Vec3{rnd(-2, 2), rnd(-5, -3), rnd(-5, 5) - 20}, Vec3());
    else if (n == "lowrising3") amb(c + Vec3{rnd(-1.2f, 1.2f), rnd(-5, -3), rnd(-5, 5) - 20}, Vec3(0, 0.7f, 0));
    else if (n == "sidesrising") {
        float x = rnd(0, 1) > 0.5f ? rnd(-4, -2) : rnd(2, 4);
        amb(c + Vec3{x, rnd(-5, -1), rnd(-5, 5) - 15}, Vec3(0, 1, 0));
    } else if (n == "falling") amb(c + Vec3{rnd(-4, 4), rnd(-2, 5), rnd(-5, 5) - 20}, Vec3(0, -0.5f, 0));
    else if (n == "fallinglite") {
        if (rnd(0, 1) > 0.5f) amb(c + Vec3{rnd(-6, 6), rnd(-2, 5), rnd(-5, 5) - 20}, Vec3(0, -0.5f, 0));
    } else if (n == "dustyfalling") {
        if (!every90) return;
        Vec3 o = c + Vec3{rnd(-4, 4), rnd(6, 7), rnd(-5, 5) - 25};
        for (int i = irnd(20, 60); i > 0; i--) {
            Vec3 p = o + Vec3{nrm(-0.1f, 0.1f), nrm(-0.1f, 0.1f), nrm(-0.1f, 0.1f)};
            Vec3 v{nrm(-0.3f, 0.3f), nrm(0, -3), nrm(-0.3f, 0.3f)};
            amb(p, v);
        }
    } else if (n == "starfield") { // 4 per tick on a ring of radius 1..5 around the view axis, flying at the camera
        for (int i = 0; i < 4; i++) {
            float a = rnd(0, 2 * kPi), r = rnd(1, 5);
            L.host->spawnParticle("ambient", Vec3{c.x + r * std::cos(a), c.y + r * std::sin(a), c.z + nrm(-20, -25)},
                                  Vec3(0, 0, 5), 1);
        }
    }
}

int tutorialIdFromName(const std::string& n) { return n == "crystal" ? 1 : n == "bar" ? 2 : n == "button" ? 3 : 0; }
const char* tutorialName(int id) { return id == 1 ? "crystal" : id == 2 ? "bar" : id == 3 ? "button" : ""; }

// the obstacle's first body was hit or broken
static bool obstacleAlreadyHit(const Obstacle& o) {
    if (o.bodies.empty() || !o.bodies[0]) return false;
    return o.bodies[0]->hitBreakable || o.bodies[0]->brokenFlag;
}

bool Room::tutorialTarget(const std::string& name, Vec3* pos, bool* hit) const {
    for (const auto& d : defs) {
        if (!d.obj || d.obj->done || d.tutorial != name) continue;
        if (pos) *pos = d.obj->pos + d.tutorialOffset; // plain add, not rotated
        if (hit) *hit = obstacleAlreadyHit(*d.obj);
        return true;
    }
    return false;
}

// tutorial triggers, training and classic only
void Room::checkTutorialTriggers() {
    Level& L = level;
    if (L.player.mode >= kModeExpert) return;
    for (auto& d : defs) {
        if (!d.obj || d.obj->done || d.tutorial.empty()) continue;
        int id = tutorialIdFromName(d.tutorial);
        if (!id || L.host->tutorialCompleted(id)) continue;
        if (obstacleAlreadyHit(*d.obj)) L.host->markTutorialCompleted(id);
        else if (std::fabs(L.cam.z - d.obj->pos.z) < d.tutorialDistance) {
            L.host->startTutorial(id);
            L.host->markTutorialCompleted(id);
        }
    }
}

// ===================================================================== Level

Level::Level(Content& c, LevelHost* h) : content(c), host(h) {
    world.onBreak = [this](const BreakEvent& e) {
        if (e.firstBreak) {
            player.statBroken++;
            brokenThisRun++;
        }
        // up to 8 debris pieces (32 per breakage pass) in the colour of the body's last breakable shape, white for
        // scoring obstacles. Then 10 glints and 2 pale smoke puffs in the colour of its last glass shape, white if
        // none. The particles inherit half the body's velocity.
        if (e.firstInPass) breakPieces_ = 0;
        const int pieces = std::min(8, 32 - breakPieces_);
        if (pieces > 0) {
            spawnDebrisAt(e.body, e.point, e.body->owner && e.body->owner->scoreValue != 0 ? kWhite : e.color, pieces);
            breakPieces_ += pieces;
        }
        float pc[3] = {1, 1, 1};
        for (auto& s : e.body->shapes)
            if (s->mat.type == kMaterialGlass)
                for (int c = 0; c < 3; c++) pc[c] = s->mat.color[c];
        const Vec3 hv = e.bodyVel * 0.5f;
        const float glint[4] = {pc[0], pc[1], pc[2], 0.4f};
        const float pale[4] = {pc[0] * 0.5f + 0.5f, pc[1] * 0.5f + 0.5f, pc[2] * 0.5f + 0.5f, 0.6f};
        host->particleBurst("break_debris", e.point, 0.4f, e.normal * 2.0f + hv, 5.0f, glint, 10);
        host->particleBurst("break_smoke", e.point, 0.4f, hv, 0.4f, pale, 2);
    };
    // one shatter sound per breakage pass, at full volume
    world.onBreakSound = [this](const Vec3& p, bool plate) {
        host->sound(plate ? "glass_break" : "rubble", 1.0f, 1.0f, true, p);
    };
}
Level::~Level() = default;

Vec3 Level::gravity() const {
    float g = cur ? cur->gravityScale : 1.0f;
    return roll.rotate(Vec3(0, -10.0f * g, 0));
}

// The score panel's ball total and hit rate count balls as they are destroyed. A ball counts as a hit once it
// broke something, pressed a button or took a powerup.
void Level::registerBall(const Body* b) {
    if (b->registered) return; // already counted when the run's statistics were folded
    player.statBallsShot++;
    if (b->hitBreakable) player.statBallsHit++;
}

void Level::registerLiveBalls() {
    for (auto& bp : world.bodies())
        if (bp->isBall && !bp->registered) {
            registerBall(bp.get());
            bp->registered = true;
        }
}

void Level::clearWorld() {
    cur.reset();
    next.reset();
    world.clearAll();
    debris.clear();
    lasers.clear();
    bodyDecals.clear();
    waters.clear();
}

void Level::stop() {
    clearWorld();
    active = false;
}

bool Level::start(const std::string& selector, int mode) {
    stop();
    if (mode < kModeTraining || mode > kModeCoop) mode = kModeClassic; // per-mode arrays are indexed by it
    player.mode = mode;
    props["player.mode"] = std::to_string(mode);
    roomDefs.clear();
    realLevel = false;
    moregamesLevel = selector.find("moregames") != std::string::npos;
    creditsLevel = moregamesLevel || selector.find("credits") != std::string::npos;
    int startRoom = 0;
    bool allowBoss = selector.find(':') != std::string::npos;
    if (selector.rfind("level:", 0) == 0) {
        std::string name = selector.substr(6);
        if (!content.loadLevelFile(name, 0, mode, allowBoss, roomDefs)) {
            host->log("Could not load level " + name);
            return false;
        }
        player.zero();
    } else {
        realLevel = true;
        int n = selector == "quickload" ? 0 : atoi(selector.c_str());
        auto names = content.levelNames();
        for (int i = 0; i < (int)names.size(); i++) {
            if (i == n) startRoom = (int)roomDefs.size();
            size_t before = roomDefs.size();
            int xmlTotal = 0; // every <room> length, including boss rooms skipped outside expert
            if (!content.loadLevelFile(names[i], i, mode, allowBoss, roomDefs, &xmlTotal)) {
                host->log("Could not load level " + names[i]);
                return false;
            }
            // each campaign level spans 1000 units
            if (i < kCampaignLevels && xmlTotal > 0 && xmlTotal != 1000 && roomDefs.size() > before) {
                const float f = 1000.0f / (float)xmlTotal;
                int acc = 0;
                for (size_t k = before; k < roomDefs.size(); k++) {
                    if (k + 1 == roomDefs.size()) roomDefs[k].length = 1000 - acc;
                    else {
                        roomDefs[k].length = (int)((float)roomDefs[k].length * f);
                        acc += roomDefs[k].length;
                    }
                }
            }
        }
        player.zero();
    }
    if (roomDefs.empty()) return false;
    if (mode == kModeVersus || mode == kModeCoop) {
        for (auto& d : roomDefs) d.length = 200;
        // two players: shuffle with 500 random swaps, keeping the first 10 rooms in place
        if (roomDefs.size() > 11) {
            const uint32_t span = (uint32_t)roomDefs.size() - 10;
            for (int k = 0; k < 500; k++) {
                size_t a = 10 + rnd_() % span, b = 10 + rnd_() % span;
                if (a != b) std::swap(roomDefs[a], roomDefs[b]);
            }
        }
    }
    const bool quickload = selector == "quickload" && resume.room >= 0;
    if (quickload) startRoom = resume.room;
    player.roomIdx = std::min(startRoom, (int)roomDefs.size() - 1);
    if (quickload) {
        player.balls = resume.balls;
        player.streak = resume.streak;
    } else if (realLevel) player.loadCheckpoint(roomDefs[player.roomIdx].levelIdx);
    resume = Resume();
    active = true;
    reset();
    return true;
}

void Level::reset() {
    clearWorld();
    brokenThisRun = 0;
    roomAccStreak = roomAccBalls = 0;
    roomAccTicks = 0;
    adjustedTouchPending = false;
    roll = Quat();
    cam = Vec3(0, 1, 0);
    time = 0;
    camSpeed = 0;
    timeScale = 1;
    pauseFade = 0;
    outOfBalls = 0;
    pain = 0;
    for (float& t : powerupTimer) t = 0;
    painTimer[0] = painTimer[1] = painTimer[2] = 0;
    ended = false;
    gameOverState = kGameOverNone;
    runTime = 0;
    immuneUntil = 0;
    extraBallsUsed = 0;
    hintPowerupSent = hintFrenzySent = false;
    distanceBase = 0;
    for (int i = 0; i < player.roomIdx; i++) distanceBase += roomDefs[i].length;
    distance = distanceBase;
    musicT = 0;
    smoothLen = 0;
    roomClock = 0;
    speedBlend = 0;
    noMusicTimer = 0;
    musicStarted = false;
    tickCount = 0;
    cameraShake = 0;
    playerShake[0] = playerShake[1] = 0;
    props["level.egg"] = "0";
    props["level.bossattack"] = "0";
    player.distance = (int)distance;
    const RoomDef& d = roomDefs[player.roomIdx];
    cur.reset(new Room(*this, d, player.roomIdx, 0.0f));
    enterRoom(cur.get(), true); // smoothed length and speed blend stay 0: a run starts from rest
    world.cullZ = cam.z;
    world.gravity = gravity();
}

void Level::enterRoom(Room* r, bool fresh) {
    host->setReverb(r->lowPass, r->reverb[0], r->reverb[1], r->reverb[2], r->echo[0], r->echo[1], r->echo[2],
                    r->echo[3]);
    // if the level-music channel is not playing (stopped by a boss room, or the queued track never started mixing in)
    // the new room's track starts fresh, otherwise the queued track takes over. The first room's track is started by
    // update() once the menu has slid away.
    if (!fresh) {
        if (!host->levelMusicContinues()) { // e.g. a boss room stopped the music
            host->playLevelMusic(r->music);
            noMusicTimer = 0;
        } else host->swapLevelMusic();
    }
    r->lastOfLevel = player.roomIdx + 1 < (int)roomDefs.size() &&
                     roomDefs[player.roomIdx].levelIdx != roomDefs[player.roomIdx + 1].levelIdx;
    if (realLevel) {
        quickSaveCountdown = 3;
        host->levelEntered(roomDefs[player.roomIdx].levelIdx);
    } else {
        host->clearQuickSave();
        quickSaveCountdown = 0;
    }
}

void Level::loadNextRoom() {
    int idx = std::min(player.roomIdx + 1, (int)roomDefs.size() - 1);
    next.reset(new Room(*this, roomDefs[idx], idx, cur->start + cur->length));
    host->queueNextLevelMusic(next->music);
}

std::string Level::getProperty(const std::string& k) {
    if (k == "player.mode") return std::to_string(player.mode);
    auto it = props.find(k);
    return it == props.end() ? "" : it->second;
}
void Level::setProperty(const std::string& k, const std::string& v) { props[k] = v; }

// ---------------------------------------------------------------- scoring

void Level::streakInc(int p) {
    int mode = player.mode;
    if (mode == kModeCoop && p >= 0 && p < 2) {
        player.streakP[p] = std::min(4, player.streakP[p]) + 1;
        if (player.streakP[0] == 5 && player.streakP[1] == 5) {
            host->sound("multiplier_up", 1, 1, false, Vec3());
            player.streak += 10;
            player.streakP[0] = player.streakP[1] = 0;
        }
    } else if (mode == kModeCoop) {
        // co-op clear with no known player: no streak change
    } else if (mode == kModeVersus && p >= 0 && p < 2) {
        int n = ++player.streakP[p];
        if (n < 50 && n % 10 == 0) host->sound("multiplier_up", 1, 1, false, Vec3());
    } else {
        player.streak++;
        player.statMaxStreak = std::max(player.statMaxStreak, player.streak);
        if (player.streak < 50 && player.streak % 10 == 0) host->sound("multiplier_up", 1, 1, false, Vec3());
    }
    streakSoundStep = streakSoundStep <= 10 ? streakSoundStep + 1 : 0;
}

void Level::streakAbort(int p) {
    int v = streakSoundStep;
    if (player.mode == kModeVersus) {
        if (p >= 0 && p < 2) {
            if (player.streakP[p] >= 10) host->sound("streak_lost", 1, 1, false, Vec3());
            player.streakP[p] = 0;
        } else {
            if (player.streakP[0] >= 10 || player.streakP[1] >= 10) host->sound("streak_lost", 1, 1, false, Vec3());
            player.streakP[0] = player.streakP[1] = 0;
        }
    } else {
        if (player.streak >= 10) host->sound("streak_lost", 1, 1, false, Vec3());
        player.streak = 0;
        player.streakP[0] = player.streakP[1] = 0;
    }
    streakSoundStep = v > 2 ? v - 3 : v + 9;
}

void Level::addScore(int value, int p) {
    if (p < 0 || player.mode != kModeVersus) {
        player.balls += value;
        if (player.mode == kModeTraining) player.balls = std::min(player.balls, 500);
    } else {
        player.ballsP[p] += value;
    }
    player.statPeakBalls = std::max(player.statPeakBalls, player.balls);
    host->hudCommand("pickup " + std::to_string(value));
}

void Level::hitSomething(int p) {
    if (player.mode == kModeZen) return;
    if (immuneUntil > runTime) return;
    painRight = p != 0; // anything but player 0 tints the right half, -1 included, same as the original
    // versus shakes only the hit player's view, every other mode the shared camera
    float& shake = (player.mode == kModeVersus && p >= 0) ? playerShake[p] : cameraShake;
    shake = std::max(shake, 1.0f);
    int& pool = (player.mode == kModeVersus && p >= 0) ? player.ballsP[p] : player.balls;
    int before = pool;
    pool = std::max(pool, 10) - 10;
    int lost = before - pool;
    painTimer[p < 0 ? 0 : p + 1] = 1.0f;
    pain = 1.0f;
    streakAbort(p);
    host->sound("hit", 1, 1, true, cam);
    host->hudCommand("hit");
    for (int i = 0; i < std::min(lost, 10); i++) {
        float w = lastView.width, h = lastView.height;
        float px = w / 2 + (float)((int)(rnd_() % 1000) - 500) / 1000.0f * (2.0f * w / 3.0f); // +-w/3 around the centre
        float py = 2 * h / 3 + (float)((int)(rnd_() % 1000) - 500) / 1000.0f * h / 2;
        shoot(px, py, camSpeed + 4.0f + (rnd_() % 200) / 100.0f, false, p, lastView);
    }
}

// ---------------------------------------------------------------- balls

Body* Level::createBall(int pl) {
    Vec3 p = cam + Vec3(0, -0.1f, -0.5f);
    if (player.mode == kModeVersus || player.mode == kModeCoop) {
        if (pl == 0) p.x -= 1;
        else if (pl == 1) p.x += 1;
    }
    Body* b = world.createBody(p, Quat());
    b->isBall = true;
    b->id = pl;
    Material m;
    m.density = 5.0f;
    m.friction = 1.0f;
    m.restitution = 0.4f;
    m.category = kCategoryBall;
    m.mask = 0xff;
    m.color[0] = m.color[1] = m.color[2] = m.color[3] = 1;
    world.addBox(b, Vec3(0.1f, 0.1f, 0.1f), Vec3(), Quat(), m, 0);
    return b;
}

Body* Level::shoot(float px, float py, float speed, bool fromPlayer, int pl, const ViewParams& vp) {
    // credits: past distance 100 balls are slow and heavy
    const bool slowCredits = creditsLevel && distance > 100.0f;
    if (slowCredits) speed *= 0.4f;
    Body* ball = createBall(pl); // density 5: 0.04 kg
    if (slowCredits && !ball->shapes.empty()) {
        // the original overwrites density and friction after the mass is computed, so the mass stays 0.04 kg
        ball->shapes[0]->mat.density = 10.0f;
        ball->shapes[0]->mat.friction = 0.0f;
    }
    // camera ray through the pixel
    float aspect = vp.width / vp.height;
    float tx = std::tan(vp.fovDeg() * kPi / 360.0f);
    float nx = 2 * px / vp.width - 1, ny = 1 - 2 * py / vp.height;
    Vec3 d = normalize(roll.rotate(Vec3(nx * tx, ny * tx / aspect, -1.0f)));
    Vec3 g = gravity();
    Vec3 comp = g * 0.01f;
    float extra = camSpeed > 5.0f ? camSpeed - 5.0f : 0.0f;
    ball->vel = (d - comp) * (speed + extra);
    if (fromPlayer && powerupTimer[kPowerupNitro] > 0) ball->ballState = kBallNitro;

    return ball;
}

void Level::handleInput(const InputTap* taps, int count, const ViewParams& vp) {
    lastView = vp;
    if (!active || ended) return;
    bool frenzy = powerupTimer[kPowerupFrenzy] > 0;
    int activeTouches = 0;
    for (int i = 0; i < count; i++)
        if (taps[i].down) activeTouches++;
    int k = 0;
    for (int i = 0; i < count; i++) {
        const InputTap& t = taps[i];
        bool fire = false;
        int pl = -1;
        float px = t.x, py = t.y;
        ViewParams evp = vp;
        if (player.mode == kModeVersus || player.mode == kModeCoop) {
            // two-player split: each half shows a portrait eye viewport (H x W/2) rotated -/+90 deg;
            // map the window pixel into that eye's pixel space
            evp.width = vp.height;
            evp.height = vp.width * 0.5f;
            if (t.x >= vp.width * 0.5f) {
                pl = 1;
                px = vp.height - t.y;
                py = t.x - vp.width * 0.5f;
            } else {
                pl = 0;
                px = t.y;
                py = vp.width * 0.5f - t.x;
            }
        }
        if (!frenzy) {
            fire = t.pressed && player.ballsOf(pl) > 0 && outOfBalls < 1.0f;
            if (fire && player.mode != kModeZen) player.ballsOf(pl) -= 1;
        } else {
            if (t.released) rapidFireCount = 0;
            if (t.down && rapidFireCd <= 0) {
                if (activeTouches > 0 && rapidFireCount % activeTouches == k) {
                    rapidFireCount++;
                    rapidFireCd = 0.1f;
                    fire = true;
                }
                k++;
            }
        }
        // the tutorial's aim correction belongs to this press only, fired or not (e.g. no balls left)
        const bool adjusted = adjustedTouchPending && t.pressed;
        if (t.pressed) adjustedTouchPending = false;
        if (!fire) continue;
        if (adjusted) {
            px = adjustedTouch.x;
            py = adjustedTouch.y;
        }
        int streak = pl >= 0 && player.mode == kModeVersus ? player.streakP[pl] : player.streak;
        float s = evp.width / 30.0f;
        std::vector<Vec3> pts;
        bool nitroOrFrenzy = powerupTimer[kPowerupNitro] > 0 || powerupTimer[kPowerupFrenzy] > 0;
        if (streak >= 10 && !nitroOrFrenzy) {
            if (streak < 20) pts = {{px - 0.4f * s, py, 0}, {px + 0.4f * s, py, 0}};
            else if (streak < 30)
                pts = {{px - 0.5f * s, py + 0.5f * s, 0}, {px + 0.5f * s, py + 0.5f * s, 0}, {px, py - 0.5f * s, 0}};
            else if (streak < 40)
                pts = {{px - 0.7f * s, py, 0}, {px + 0.7f * s, py, 0}, {px, py + 0.7f * s, 0}, {px, py - 0.7f * s, 0}};
            else
                pts = {{px, py, 0},
                       {px - 0.8f * s, py - 1.0f * s, 0},
                       {px + 0.8f * s, py - 1.0f * s, 0},
                       {px - 0.8f * s, py + 1.0f * s, 0},
                       {px + 0.8f * s, py + 1.0f * s, 0}};
        } else pts = {{px, py, 0}};
        for (auto& q : pts) shoot(q.x, q.y, 20.0f, true, pl, evp);
        host->sound("shoot", 0.4f, 1.0f, false, Vec3());
    }
}

bool Level::checkHit(const Vec3& p) {
    static const float off[5][2] = {{0, 0}, {0.01f, -0.01f}, {-0.01f, -0.01f}, {0.01f, 0.01f}, {-0.01f, 0.01f}};
    float len = std::max(camSpeed * dt, 0.001f);
    const Shape* last = nullptr;
    for (auto& o : off) {
        Vec3 a(p.x + o[0], p.y + o[1], p.z), b(p.x + o[0], p.y + o[1], p.z - len);
        RayHit h = world.raycast(a, b, 0xdf, nullptr); // the original's mask; no shape uses 0x20
        if (!h.hit) return false;                      // all five rays must hit
        last = h.shape;                                // the original tests only the 5th ray's shape
    }
    // the body needs an owner obstacle (balls have none) that is not cleared, and must not be a fracture shard
    const Body* bd = last->body;
    return bd->owner && bd->owner->clearedAt == 0 && !bd->splitProcessed;
}

void Level::explosion(const Vec3& pos, float strength) {
    addShake(3.0f / std::max(length(cam - pos), 1e-3f));
    std::vector<Body*> bodies, brk;
    world.collect(pos - Vec3(2, 2, 2), pos + Vec3(2, 2, 2), kMaskNoRoom, bodies);
    for (Body* b : bodies) {
        bool breakable = false;
        for (auto& s : b->shapes)
            if (s->mat.forceLimit < FLT_MAX) breakable = true;
        if (breakable) brk.push_back(b);
    }
    // the point on each breakable body nearest to the blast
    std::vector<Vec3> cps;
    if (!brk.empty()) {
        // one shatter sound for everything in the box, played before breaking (even if nothing then breaks). Its size
        // comes from the summed volume of each body's shape nearest the blast.
        float vol = 0;
        for (Body* b : brk) {
            Vec3 lp = b->pointToLocal(pos), q = lp;
            float best = FLT_MAX;
            const Shape* cs = nullptr;
            for (auto& s : b->shapes) {
                Vec3 qq;
                float dd = s->poly.distanceTo(lp, &qq);
                if (dd < best) {
                    best = dd;
                    cs = s.get();
                    q = qq;
                }
            }
            if (cs) vol += cs->volume;
            cps.push_back(cs ? b->pointToWorld(q) : b->worldCom());
        }
        host->sound(vol <= 0.1f   ? "bank:breakable/glass_s"
                    : vol <= 0.5f ? "bank:breakable/glass_m"
                                  : "bank:breakable/glass_l",
                    0.5f, 1.0f, true, brk[0]->pos);
    }
    // each body breaks around the point where the blast touches it (a 1.6 m cell there)
    for (size_t i = 0; i < brk.size(); i++) {
        const Vec3 cp = cps[i];
        float d = length(cp - pos);
        Vec3 dir = (cp - pos) / (d + FLT_EPSILON);
        float limit = strength / std::pow(std::max(d, 1.0f), 2.0f);
        world.breakBody(brk[i], cp, dir, limit, 0.8f);
    }
    for (auto& bp : world.bodies()) { // impulse on every dynamic body within 2 m of the blast, balls included
        Body* b = bp.get();
        if (!b->dynamic || !b->active) continue;
        Vec3 dv = b->worldCom() - pos;
        float d = length(dv);
        if (d > 2.5f) continue;
        float f = clampf(1.0f - 0.5f * d, 0, 1);
        Vec3 imp = dv / (d + 1e-3f) * (f * f * strength * b->invMass * 0.03f);
        if (lengthSq(imp) > 49.0f) imp = normalize(imp) * 7.0f;
        b->vel += imp;
    }
    host->sound("explosion", 1.0f, 1.0f, true, pos);
    host->particles("explosion_white", pos, Vec3(), kWhite, 6);
    host->particles("explosion_dark", pos, Vec3(0, 1, 0), kWhite, 8);
    host->particles("explosion_dust", pos, Vec3(0, 1, 0), kWhite, 8);
    host->particles("spark", pos, Vec3(), kWhite, 10);
}

void Level::activatePowerup(int type) {
    if (type <= kPowerupNone || type >= kPowerupCount) return;
    for (float& t : powerupTimer) t = 0;
    powerupTimer[type] = kPowerupDuration;
}

// ---------------------------------------------------------------- debris

// Debris shards are small random tetrahedra thrown off with the body's velocity, in the shape colour at half
// brightness. No new pieces once 401 exist.
void Level::spawnDebrisAt(Body* b, const Vec3& pt, const float rgba[4], int count) {
    for (int k = 0; k < count && debris.size() <= 400; k++) {
        DebrisPiece d;
        d.pos = pt;
        d.rot = Quat();
        Vec3 v = randCubeDir(rnd_, 2.0f) + b->vel * 1.2f;
        d.vel = v * randCentered(rnd_, 0.75f, 1.5f);
        Vec3 spin = randCubeDir(rnd_, 1.0f);
        d.angVel = Vec3(spin.x * 4, spin.y * 4, v.x * 4);
        float q1 = randCentered(rnd_, 0, 1), a = 0.1f * q1 * q1;
        float q2 = randCentered(rnd_, 0, 1), f = 0.1f * q2 * q2;
        if (randRange(rnd_, 0, 1) < 0.6f) {
            a *= 0.5f;
            f *= 2.0f;
        }
        const float A = (a + 0.02f) * 0.9f, B = (randRange(rnd_, 0.2f * a, 0.5f * a) + 0.02f) * 0.9f,
                    fz = (f + 0.02f) * 0.9f;
        d.corner[0] = Vec3(-A, -B, fz);
        d.corner[1] = Vec3(A, -B, 0);
        d.corner[2] = Vec3(-A, -B, -fz);
        d.corner[3] = Vec3(0, B, 0);
        uint32_t r = (uint32_t)(clampf(rgba[0] * 0.5f, 0, 1) * 255),
                 gg = (uint32_t)(clampf(rgba[1] * 0.5f, 0, 1) * 255),
                 bl = (uint32_t)(clampf(rgba[2] * 0.5f, 0, 1) * 255);
        d.color = r | (gg << 8) | (bl << 16) | ((uint32_t)(clampf(rgba[3], 0, 1) * 255) << 24);
        debris.push_back(d);
    }
}

void Level::updateDebris() {
    Vec3 g = gravity();
    for (size_t i = 0; i < debris.size();) {
        DebrisPiece& d = debris[i];
        bool remove = d.pos.z > cam.z || d.pos.y <= -10.0f || (!d.alive && debris.size() > 200);
        if (remove) {
            debris[i] = debris.back();
            debris.pop_back();
            continue;
        }
        if (d.alive) {
            d.vel += g * dt;
            d.pos += d.vel * dt;
            float wl = length(d.angVel);
            if (wl > 1e-4f) d.rot = (Quat::axisAngle(d.angVel, wl * dt) * d.rot).normalized();
        }
        if ((tickCount + (int)i) % 5 == 0) {
            RayHit h = world.raycast(d.pos + Vec3(0, 0.5f, 0), d.pos + Vec3(0, -0.5f, 0), kCategoryRoom, nullptr);
            d.floorY = h.hit ? h.point.y + 0.05f : -10.0f;
        }
        if (d.alive && d.floorY >= d.pos.y && d.vel.y < 0) {
            d.rot = Quat::axisAngle(Vec3(0, 1, 0), (float)(rnd_() % 6283) / 1000.0f);
            d.pos.y = d.floorY;
            d.angVel = Vec3();
            d.vel.x *= 0.9f;
            d.vel.z *= 0.9f;
            d.vel.y = -d.vel.y * 0.3f;
            if (lengthSq(d.vel) < 0.5f) {
                d.alive = false;
                d.vel = Vec3();
            }
        }
        i++;
    }
}

// Water, once per tick before physics: a wave step on the height grid, then every non-room shape crossing the surface
// inside [pos.x +- sx] x [pos.y-10, pos.y] x [pos.z +- sz] pushes the grid and splashes (after 1 s out of the water).
// Non-ball shapes also get buoyancy and drag. The original's splash particles are never visible because of its draw
// order, so they are left out. A water is removed once the camera has passed it.
void Level::updateWaters() {
    const float s = std::min(timeScale, 1.0f);
    for (size_t wi = 0; wi < waters.size();) {
        Water& w = waters[wi];
        const int nx = w.nx, ny = w.ny;
        std::vector<float>&H = w.H, &P = w.P, &V = w.V;
        for (int j = 0; j < ny; j++) {
            V[j * nx] = H[j * nx] = 0;
            V[j * nx + nx - 1] = H[j * nx + nx - 1] = 0;
        }
        for (int i = 0; i < nx; i++) {
            V[i] = H[i] = 0;
            V[(ny - 1) * nx + i] = H[(ny - 1) * nx + i] = 0;
        }
        for (int j = 1; j < ny - 1; j++)
            for (int i = 1; i < nx - 1; i++) {
                const int k = j * nx + i;
                float v = ((P[k - 1] + P[k + 1] + P[k - nx] + P[k + nx]) * 0.25f - P[k]) * 0.5f * s + V[k];
                v = (P[k] * -0.05f + v) * 0.99f;
                V[k] = v;
                H[k] += v * s;
            }
        P = H;
        std::vector<Body*> bodies;
        const Vec3 mn(w.pos.x - w.sx, w.pos.y - 10.0f, w.pos.z - w.sz), mx(w.pos.x + w.sx, w.pos.y, w.pos.z + w.sz);
        world.collect(mn, mx, kMaskNoRoom, bodies);
        const float y = w.pos.y;
        for (Body* b : bodies)
            for (auto& sp : b->shapes) {
                Shape* sh = sp.get();
                if (!(sh->mat.category & kMaskNoRoom) || sh->aabbMax.x < mn.x || sh->aabbMin.x > mx.x ||
                    sh->aabbMax.y < mn.y || sh->aabbMin.y > mx.y || sh->aabbMax.z < mn.z || sh->aabbMin.z > mx.z)
                    continue;
                const Vec3 com = b->worldCom();
                auto pointVel = [&](const Vec3& p) { return b->vel + cross(b->angVel, p - com); };
                const float cx = (sh->aabbMin.x + sh->aabbMax.x) * 0.5f, cz = (sh->aabbMin.z + sh->aabbMax.z) * 0.5f;
                if (sh->aabbMin.y < y && y < sh->aabbMax.y) {
                    const Vec3 sp3(cx, y, cz);
                    if (b->splashTime + 1.0f < time) {
                        float m = b->invMass > 0 ? 1.0f / b->invMass : FLT_MAX;
                        int idx = m > 0.1f ? 3 : m > 0.05f ? 2 : m > 0.01f ? 1 : 0;
                        host->sound("splash:" + std::to_string(idx), 1.0f, 1.0f, true, sp3);
                    }
                    b->splashTime = time;
                    int gi = (int)(((cx - w.pos.x) + w.sx) * (float)(nx - 1) / (w.sx + w.sx));
                    int gj = (int)((w.sz - (cz - w.pos.z)) * (float)(ny - 1) / (w.sz + w.sz));
                    int k = gj * nx + gi;
                    if (k > 0 && k < nx * ny)
                        V[k] = clampf(V[k] * 0.5f + pointVel(sp3).y * 0.5f * 0.04f, -0.16f, 0.16f);
                }
                if (b->isBall) continue;
                // buoyancy from the submerged volume, applied between the centre of mass and the submerged centroid,
                // then a 0.98^s drag per collected shape
                Mat3 R = Mat3::fromQuat(b->rot);
                Vec3 nl = R.transposed() * Vec3(0, 1, 0);
                float dl = y - b->pos.y, vol = 0;
                Vec3 cl;
                Polyhedron above, below;
                Mat3 I1;
                if (sh->poly.split(nl, dl, above, below)) below.massProps(1.0f, vol, cl, I1);
                else {
                    bool allBelow = !sh->poly.v.empty();
                    for (const Vec3& v : sh->poly.v)
                        if (dot(nl, v) - dl >= -1e-6f) {
                            allBelow = false;
                            break;
                        }
                    if (allBelow) sh->poly.massProps(1.0f, vol, cl, I1);
                }
                if (vol > 1e-6f && b->dynamic && b->active && sh->mat.density > 0) {
                    Vec3 at = com * 0.7f + (b->pos + R * cl) * 0.3f;
                    Vec3 J(0, vol / sh->mat.density * 10.0f * dt, 0);
                    b->vel += J * b->invMass;
                    if (!b->rotLocked) b->angVel += (R * b->invInertiaLocal * R.transposed()) * cross(at - com, J);
                }
                const float f = std::pow(0.98f, s);
                b->vel = b->vel * f;
                b->angVel = b->angVel * f;
            }
        if (cam.z < w.pos.z - w.sz) {
            waters.erase(waters.begin() + (long)wi);
            continue;
        }
        wi++;
    }
}

// ---------------------------------------------------------------- tick

void Level::centerCamera() {
    float dz = -cam.z;
    if (dz == 0) return;
    for (auto& bp : world.bodies()) {
        Body* b = bp.get();
        if ((cur && b == cur->body) || (next && b == next->body)) continue; // moved by Room::shiftZ
        if (b == world.ground()) continue;
        if (!b->dynamic) {
            world.setTransform(b, b->pos + Vec3(0, 0, dz), b->rot);
        } else {
            b->pos.z += dz;
            b->prevPos.z += dz;
            if (b->motion) b->motion->pos.z += dz;
        }
    }
    if (cur) cur->shiftZ(dz);
    if (next) next->shiftZ(dz);
    for (auto& d : debris) d.pos.z += dz;
    for (auto& l : lasers) {
        l.origin.z += dz;
        l.end.z += dz;
    }
    for (auto& w : waters) w.pos.z += dz; // every entity is translated (loopCamera moves only dynamic bodies)
    host->shiftWorldZ(dz);
    cam.z += dz;
    prevCam.z += dz;
    for (auto& bp : world.bodies())
        if (bp->dynamic) {
            Vec3 pp = bp->prevPos;
            Quat pr = bp->prevRot;
            world.setTransform(bp.get(), bp->pos, bp->rot);
            bp->prevPos = pp;
            bp->prevRot = pr;
        }
}

// Lasers: segments re-added every tick by obstacle scripts. Balls are vaporised, the camera takes a hit.
void Level::processLasers() {
    std::uniform_real_distribution<float> hum(0.1f, 0.2f);
    for (auto& l : lasers) {
        host->loopSound(0, l.origin, hum(laserRnd_));
        Vec3 a = l.origin, b = l.origin + l.dir * 100.0f;
        RayHit h = world.raycast(a, b, 0xff, nullptr);
        Vec3 end = b;
        if (h.hit) {
            end = h.point;
            l.end = end;
            if (h.shape->body->isBall) {
                host->sound("bank:laser/discharge", 1.0f, 1.0f, true, h.point);
                host->particles("laserspark", h.point, Vec3(), kWhite, 25);
                host->particles("explosion_white", h.point, Vec3(), kWhite, 5);
                Body* ball = h.shape->body;
                registerBall(ball);
                world.destroyBody(ball);
            } else if ((laserRnd_() % 100) < 10) host->particles("laserspark", h.point, Vec3(), kWhite, 1);
        }
        if (powerupTimer[kPowerupShield] > 0 || ended) continue;
        float segLen = length(end - a);
        float rad = lerp(0.2f, 0.6f, (clampf(camSpeed, 5, 15) - 5) / 10.0f);
        auto test = [&](const Vec3& cp, int pl, float& timer) {
            if (length(cp - a) - 2.1f >= segLen) return;
            Vec3 d = end - a;
            float t = segLen > 1e-6f ? clampf(dot(cp - a, d) / dot(d, d), 0, 1) : 0;
            if (lengthSq(a + d * t - cp) < rad * rad && timer <= 0) hitSomething(pl);
        };
        if (player.mode == kModeVersus || player.mode == kModeCoop) {
            test(Vec3(cam.x - 1, cam.y, cam.z), 0, painTimer[1]);
            test(Vec3(cam.x + 1, cam.y, cam.z), 1, painTimer[2]);
        } else test(cam, -1, painTimer[0]);
    }
}

// Impact sounds from the per-tick speed drop of dynamic bodies.
void Level::impactSounds() {
    for (auto& bp : world.bodies()) {
        Body* b = bp.get();
        if (!b->dynamic || !b->active) continue;
        float sp = lengthSq(b->vel);
        float drop = b->prevSpeedSq - sp;
        b->prevSpeedSq = sp;
        // rolling / sliding loop while moving, nearly level and in contact
        if (b->touching && sp > 0.01f && std::fabs(b->vel.y) < 0.1f)
            host->loopSound(b->isBall ? 1 : 2, b->worldCom(), (b->isBall ? 0.1f : 0.4f) * std::sqrt(sp));
        if (b->motion) continue;
        // no volume gate and no camera cull, the positional falloff already makes sounds behind the camera quieter
        float thr = b->isBall ? 1.0f : 0.2f;
        if (drop <= thr) continue;
        float vol = 0.5f * clampf((drop - thr) / 10.0f, 0, 1);
        Vec3 pos = b->worldCom();
        if (b->isBall) {
            std::string bank = "ball/object";
            bool otherBall = false;
            for (auto& op : world.bodies())
                if (op.get() != b && op->isBall && length(op->worldCom() - pos) < 0.5f) {
                    otherBall = true;
                    break;
                }
            if (otherBall) bank = "ball/ball";
            else {
                Vec3 pt, n;
                Shape* sh = nullptr;
                if (world.closestPoint(pos, 0.5f, kMaskNoBall, &pt, &n, &sh) && sh && !sh->body->dynamic) {
                    // a fast ball grazing static geometry adds a ricochet half the time. std::rand, like the bank
                    // variant pick, so the gameplay random stream is untouched
                    float v = std::sqrt(sp);
                    if (v > 10.0f && std::fabs(dot(b->vel / (v + FLT_EPSILON), n)) < 0.3f &&
                        (float)std::rand() / (float)RAND_MAX > 0.5f)
                        host->sound("bank:ball/ricochet", 0.3f, 1.0f, true, pos);
                    bank = n.y > 0.9f ? "ball/wall" : "ball/floor";
                }
            }
            host->sound("bank:" + bank, vol, 1.0f, true, pos);
        } else {
            bool metal = !b->shapes.empty() && b->shapes[0]->mat.type == kMaterialMetal;
            host->sound(metal          ? "bank:object/metal"
                        : drop <= 2.0f ? "bank:object/hit_s"
                                       : "bank:object/hit_l",
                        vol, 1.0f, true, pos);
        }
    }
}

// Boss rooms: keep the camera >= 10 units from a live boss by moving it back, with the dynamic entities moving along.
void Level::loopCamera(float d) {
    roomClock = 5.0f;
    cam.z += d;
    prevCam.z += d;
    for (auto& bp : world.bodies()) {
        Body* b = bp.get();
        if (!b->dynamic) continue;
        Vec3 pp = b->prevPos + Vec3(0, 0, d);
        Quat pr = b->prevRot;
        world.setTransform(b, b->pos + Vec3(0, 0, d), b->rot);
        b->prevPos = pp;
        b->prevRot = pr;
        if (b->motion) b->motion->pos.z += d;
    }
    for (auto& dp : debris) dp.pos.z += d;
    for (auto& l : lasers) {
        l.origin.z += d;
        l.end.z += d;
    }
    // camera, dynamic bodies, debris and particles move together while obstacle anchors and room geometry stay put,
    // so the camera keeps circling 10-18 units in front of the boss
    host->shiftWorldZ(d);
}

void Level::updateBoss() {
    if (!cur || cur->name.find("boss") == std::string::npos) return;
    bool alive = false, near = false;
    for (auto& dfn : cur->defs)
        if (dfn.obj && dfn.type.rfind("boss", 0) == 0 && dfn.obj->clearedAt == 0) {
            alive = true;
            if (cam.z - 10.0f < dfn.obj->pos.z) near = true;
        }
    if (realLevel) {
        if (!alive) {
            if (host->bossLoopPlaying()) host->setNextBossLoop("music/boss_end.ogg", true);
        } else if (!host->bossLoopPlaying()) host->playBossLoop("music/boss_loop0.ogg");
        else {
            std::string n = host->bossLoopName();
            const char* chain[4] = {"music/boss_loop0.ogg", "music/boss_loop1.ogg", "music/boss_loop2.ogg",
                                    "music/boss_loop3.ogg"};
            for (int i = 0; i < 4; i++)
                if (n == chain[i]) {
                    host->setNextBossLoop(chain[(i + 1) % 4], false);
                    break;
                }
        }
    }
    if (near) {
        loopCamera(8.0f);
        host->stopLevelMusicChannel();
    }
}

void Level::updateCamera() {
    Room* r = cur.get();
    // music-locked clock
    float traveled = r->zToRoom(cam.z);
    bool locked = false;
    // with music enabled the clock is pulled toward the music position, even while that is <= 0 (no stream yet, or
    // the -0.15 s latency right after the start), until 4 s pass without music. Otherwise it runs freely.
    if (host->musicEnabled() && r->name != "credits/withcoffee") { // that room always free-runs
        float loc = host->musicLocation();
        if (loc <= 0) {
            noMusicTimer += dt;
        } else noMusicTimer = 0;
        if (loc > 0 || noMusicTimer <= 4.0f) {
            musicT = 0.9f * musicT + 0.1f * loc;
            locked = true;
        }
    }
    if (!locked) musicT += dt * ((1.0f - outOfBalls) * timeScale);
    bool boss = r->name.find("boss") != std::string::npos;
    if (boss) musicT = roomClock;
    smoothLen = 0.995f * (boss ? 160.0f : smoothLen) + 0.005f * r->length;
    if (r->name == "credits/withcoffee") smoothLen = 112.0f;
    float target = std::max(0.0f, smoothLen * musicT / 32.0f);
    if (r->name == "credits/2") target = std::min(target, 1.5f); // park at the bowling lane
    if (ended && !boss) {
        camSpeed = 0;
        return;
    }
    if (boss) {
        float d = clampf(5.0f - camSpeed, -0.05f, 0.05f);
        camSpeed += d;
        cam.z -= dt * camSpeed;
        return;
    }
    float err = (1.0f - pauseFade) * (traveled - target);
    if (speedBlend <= 0) {
        camSpeed = -err / dt;
        cam.z += err;
    } else {
        float b = std::max(0.0f, speedBlend - 0.2f * dt);
        speedBlend = b;
        camSpeed = 5.0f * b - (1.0f - b) * (err / dt);
        cam.z -= camSpeed * dt;
    }
}

void Level::updateEntities() {
    // powerups
    for (Room* r : {cur.get(), next.get()}) {
        if (!r) continue;
        for (auto& p : r->powerups) {
            if (p.taken) continue;
            if (cam.z - p.pos.z < 30.0f) {
                RayHit h = world.raycast(p.pos, p.pos + Vec3(0, -1, 0), kCategoryRoom, nullptr);
                float dy = h.hit ? (h.point.y + 0.5f) - p.pos.y : -0.1f;
                p.pos.y += clampf(dy, -0.05f, 0.05f);
            }
            for (auto& bp : world.bodies()) {
                if (!bp->isBall || length(bp->pos - p.pos) >= 0.5f) continue;
                bp->hitBreakable = true;
                p.taken = true;
                host->sound("powerup", 0.7f, 1.0f, false, Vec3());
                if (p.type == kPowerupBarrel) explosion(p.pos, 20.0f);
                else {
                    player.pushPowerup(p.type);
                    host->powerupPushed();
                }
                break;
            }
        }
    }
    // free entities behind the camera: balls
    std::vector<Body*> kill;
    for (auto& bp : world.bodies()) {
        Body* b = bp.get();
        if (!b->isBall) continue;
        if (b->shapes.empty() || cam.z <= minShapeZ(*b) || !b->active) kill.push_back(b);
    }
    for (Body* b : kill) {
        registerBall(b);
        world.destroyBody(b);
    }
}

void Level::update(float rawDtIn) {
    if (!active || !cur) return;
    // the first room's music starts once the menu transition drops below 0.1. Until then the music position is 0
    // and the camera clock holds.
    if (!musicStarted && host->menuTransition() < 0.1f) {
        musicStarted = true;
        host->playLevelMusic(cur->music);
    }
    rawDt = rawDtIn;
    prevCam = cam;
    Room* r = cur.get();
    float p = r->progress(cam.z);
    updateRoll(p);
    // per-room accumulators for the score panel's average streak and ball count
    roomAccStreak += player.streak;
    roomAccBalls += player.balls;
    roomAccTicks++;
    // one-shot HUD hints until checkpoint 2 of this mode has been reached: the arrow at the powerup button and
    // the ball-frenzy swipe tutorial (the HUD pauses until a tap)
    if (player.ckBalls[player.mode][2] == 0) {
        if (!hintPowerupSent && !player.powerups.empty()) {
            host->hudCommand("hintpowerup");
            hintPowerupSent = true;
        }
        if (!hintFrenzySent && powerupTimer[kPowerupFrenzy] > 0) {
            host->hudCommand("hintfrenzy");
            hintFrenzySent = true;
        }
    }
    player.distance = (int)distance;
    if (quickSaveCountdown > 0 && --quickSaveCountdown == 0 && player.mode != kModeZen && !ended)
        host->quickSave(player.roomIdx, player.distance, player.balls, player.streak);
    pain = std::max(0.0f, pain - rawDt);
    updateOutOfBalls();
    // slow motion eases the time scale down to 1/3 and back
    if (powerupTimer[kPowerupSlowMotion] > 0) timeScale = std::max(0.3333f, timeScale - 0.01f);
    else if (timeScale < 1) timeScale = std::min(1.0f, timeScale + 0.01f);
    float S = (1.0f - pauseFade) * timeScale;
    dt = rawDt * S;
    for (int i = 1; i < kPowerupCount; i++) powerupTimer[i] -= dt;
    rapidFireCd -= dt;
    roomClock += rawDt;
    // camera
    if (!ended) runTime += dt;
    updateCamera();
    time += dt;
    distance = r->progress(cam.z) * roomDefs[r->defIdx].length + distanceBase;
    if (distance < distanceBase) distance = distanceBase;
    for (float& t : painTimer)
        if (t > 0) t -= dt;
    advanceRooms();
    // rooms and obstacles
    lasers.clear();
    bodyDecals.clear();
    cur->update();
    if (next) next->update();
    processLasers();
    // floating origin: recentre the world around the camera
    if (cam.z <= -50.0f) centerCamera();
    // entities
    updateEntities();
    updateBoss();
    updateWaters(); // with the other entities, before the physics step
    // physics
    world.cullZ = cam.z;
    world.gravity = gravity();
    world.step(dt);
    explodeNitroBalls();
    impactSounds();
    updateDebris();
    // camera collision
    if (powerupTimer[kPowerupShield] <= 0 && !ended) {
        if (player.mode == kModeVersus || player.mode == kModeCoop) {
            if (checkHit(Vec3(cam.x - 1, cam.y, cam.z)) && painTimer[1] <= 0) hitSomething(0);
            if (checkHit(Vec3(cam.x + 1, cam.y, cam.z)) && painTimer[2] <= 0) hitSomething(1);
        } else if (painTimer[0] <= 0 && checkHit(cam)) hitSomething(-1);
    }
    checkGameOver();
    updateCreditsRoom();
    spawnNitroTrail();
    decayShake();
    props["level.score"] = std::to_string(player.distance);
    tickCount++;
}

// camera roll over the room: eased in over the first 10% and out over the last 10%
void Level::updateRoll(float p) {
    const Room* r = cur.get();
    float q = p;
    if (q < 0.1f) q = 20 * q * q - 100 * q * q * q;
    if (q > 0.9f) {
        float c = (q - 0.9f) * 10;
        q = (1 - c) * q + c * (1 - 10 * (1 - q) * (1 - q));
    }
    float angle = q * 2 * kPi * r->rotTurns;
    if (r->rotAmp != 0) angle = std::sin(q * 2 * kPi * r->rotTurns) * r->rotAmp;
    roll = Quat::axisAngle(Vec3(0, 0, 1), angle);
}

// out-of-balls fade: rises while no balls are left, falls once there are some again
void Level::updateOutOfBalls() {
    bool recover;
    if (player.mode == kModeVersus) recover = player.ballsP[0] != 0 && player.ballsP[1] != 0;
    else recover = player.balls != 0;
    if (!recover && powerupTimer[kPowerupFrenzy] > 0) recover = true;
    if (recover) outOfBalls = std::max(0.0f, outOfBalls - 0.005f);
    else {
        if (outOfBalls <= 0) host->sound("out_of_balls_warning", 0.7f, 1, false, Vec3());
        outOfBalls = std::min(1.0f, outOfBalls + 0.005f);
    }
}

// loads the next room ahead of time and switches to it once the camera has passed the current one
void Level::advanceRooms() {
    Room* r = cur.get();
    float traveled = r->zToRoom(cam.z);
    float loc = host->musicLocation();
    if (!next && (traveled > r->length - 30.0f || loc > 28.0f)) loadNextRoom();
    if (next && traveled > r->length) {
        distanceBase += roomDefs[r->defIdx].length;
        if (realLevel && r->lastOfLevel) {
            int lvl = roomDefs[player.roomIdx].levelIdx;
            if (lvl >= 0 && lvl + 1 <= kCampaignLevels) {
                player.ckBalls[player.mode][lvl + 1] = std::max(player.ckBalls[player.mode][lvl + 1], player.balls);
                player.ckStreak[player.mode][lvl + 1] = std::max(player.ckStreak[player.mode][lvl + 1], player.streak);
                host->checkpointReached();
            }
            host->hudCommand("checkpoint " + std::to_string(lvl + 1));
        }
        if (roomAccTicks > 0) { // fold this room's averages into the run statistics
            player.statAccStreak += (double)roomAccStreak / roomAccTicks;
            player.statAccBalls += (double)roomAccBalls / roomAccTicks;
            player.statRooms++;
        }
        roomAccStreak = roomAccBalls = 0;
        roomAccTicks = 0;
        speedBlend = r->name.find("boss") != std::string::npos ? 1.0f : 0.0f; // the 5 u/s blend follows a boss room
        cur = std::move(next);
        player.roomIdx = std::min(player.roomIdx + 1, (int)roomDefs.size() - 1);
        enterRoom(cur.get(), false);
        musicT = 0;
    }
}

// nitro balls that touched something explode at the end of the step
void Level::explodeNitroBalls() {
    std::vector<Body*> boom;
    for (auto& bp : world.bodies())
        if (bp->isBall && bp->active && bp->ballState == kBallExploding) boom.push_back(bp.get());
    for (Body* b : boom) {
        explosion(b->pos, 10.0f);
        registerBall(b);
        world.destroyBody(b);
    }
}

// out of balls: offer the revive once, otherwise end the run. Credits rooms keep the balls topped up.
void Level::checkGameOver() {
    if (ended || outOfBalls < 1.0f) return;
    if (gameOverState == kGameOverDeclined) triggerGameOver();
    else if (gameOverState == kGameOverNone) {
        // the original makes this offer in every mode, but in versus the reward would land in the unused shared pool
        if (extraBallsUsed < 1 && player.mode != kModeVersus) {
            extraBallsUsed++;
            gameOverState = kGameOverOffer;
            host->hudCommand("showoutofballs");
            host->setPausedRequest(true);
        } else triggerGameOver();
    }
}

// credits and more-games rooms: balls stay topped up and the room ends by itself
void Level::updateCreditsRoom() {
    if (!creditsLevel || !active) return;
    player.balls = 25;
    if (ended || !cur) return;
    bool leave, viaCommand = false;
    if (moregamesLevel) leave = distance > 89.0f;
    else if (brokenThisRun < 16) leave = distance > 137.0f; // a player who is not playing along
    else {                                                  // after the bowling lane
        leave = cur->name == "credits/2" && musicT > 31.0f;
        viaCommand = true;
    }
    if (leave) {
        ended = true;
        host->returnToMenu(viaCommand);
    }
}

// nitro trail: a white particle at each nitro ball every other tick
void Level::spawnNitroTrail() {
    if (tickCount & 1) return;
    std::uniform_real_distribution<float> u(-1, 1);
    for (auto& bp : world.bodies())
        if (bp->isBall && bp->ballState == kBallNitro) {
            float x = u(rnd_), y = u(rnd_), z = u(rnd_);
            Vec3 v(x, y, z); // random direction, length 0.2
            float l = length(v);
            host->particles("trail", bp->pos, l > 0 ? v * (0.2f / l) : Vec3(), kWhite, 1);
        }
}

// shake decays by the scaled step every tick, clamped to [0, 2]. In split screen the original also decays it
// once per view pass (three per frame) by the raw step.
void Level::decayShake() {
    const bool splitView = player.mode == kModeVersus || player.mode == kModeCoop;
    const float shakeDecay = dt + (splitView ? 3.0f * rawDt : 0.0f);
    cameraShake = clampf(cameraShake - shakeDecay, 0.0f, 2.0f);
    for (float& s : playerShake) s = clampf(s - shakeDecay, 0.0f, 2.0f);
}

void Level::onOutOfBallsReward(int balls) {
    if (gameOverState != kGameOverOffer) return;
    gameOverState = kGameOverNone;
    player.balls = balls;
    immuneUntil = runTime + 3.0f;
    host->setPausedRequest(false);
}
void Level::onOutOfBallsSkip() {
    if (gameOverState != kGameOverOffer) return;
    gameOverState = kGameOverDeclined;
    host->setPausedRequest(false);
}

void Level::triggerGameOver() {
    gameOverState = kGameOverDone;
    ended = true;
    quickSaveCountdown = 0; // a finished run must not be quick-saved (and auto-resumed) afterwards
    player.bestDistance[player.mode] = std::max(player.bestDistance[player.mode], player.distance);
    host->gameOver(player.distance);
    if (player.mode == kModeVersus) host->hudCommand("showwinner"); // winner screen, a tap returns to the menu
    else host->menuCommand("gameover");
}

} // namespace sh
