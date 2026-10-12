// Level simulation: rooms, obstacles, camera, player state, shooting, scoring. Headless (no GL/audio).
#pragma once
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "game/content.h"
#include "game/level_script.h"
#include "physics/world.h"

namespace sh {

class Level;
class Obstacle;
class Room;

constexpr int kCampaignLevels = 12; // levels 0..11; checkpoint i (1..12) is reached by finishing level i - 1
constexpr float kPowerupDuration = 7.0f;

// powerup types, numbered as in the game data
enum Powerup {
    kPowerupNone,
    kPowerupBalls,
    kPowerupBarrel,
    kPowerupSlowMotion,
    kPowerupMultiball,
    kPowerupFrenzy,
    kPowerupNitro,
    kPowerupFreebie,
    kPowerupAntigravity,
    kPowerupRewind,
    kPowerupShield,
    kPowerupHoming,
    kPowerupCount
};
const char* powerupName(int type);            // "" for none or an unknown type
int powerupFromName(const std::string& name); // kPowerupNone if unknown

// out of balls: the revive offer, then the end of the run
enum GameOverState { kGameOverNone, kGameOverOffer, kGameOverDeclined, kGameOverDone };

struct Player {
    int mode = kModeClassic;
    int balls = 25, streak = 0;
    int ballsP[2] = {25, 25}, streakP[2] = {0, 0};
    int roomIdx = 0;
    int distance = 0;          // shown as the score
    std::vector<int> powerups; // stack, max 4
    int ckBalls[kModeCount][kCampaignLevels + 1] = {}, ckStreak[kModeCount][kCampaignLevels + 1] = {};
    int bestDistance[kModeCount] = {};
    // per-run statistics; GameApp folds them into the per-mode banks below
    int statBallsShot = 0, statBallsHit = 0, statBroken = 0, statPeakBalls = 0, statMaxStreak = 0, statRooms = 0;
    double statAccStreak = 0, statAccBalls = 0; // sums of per-room averages
    // lifetime statistics per mode for the score panel; progression.xml saves training, classic, expert and co-op
    struct StatBank {
        long long balls = 0, ballshit = 0, broken = 0, rooms = 0;
        int peakballs = 0, streak = 0;
        double accstreak = 0, accballs = 0;
    } stats[kModeCount];

    void zero();
    void clearRunStats();
    void loadCheckpoint(int idx);
    void pushPowerup(int type);
    int popPowerup();
    int& ballsOf(int player) { return (mode == kModeVersus && player >= 0 && player < 2) ? ballsP[player] : balls; }
};

// Host callbacks (HUD/menu commands, audio, effects). Defaults do nothing so the sim runs headless.
struct LevelHost {
    virtual ~LevelHost() = default;
    virtual void hudCommand(const std::string&) {}
    virtual void menuCommand(const std::string&) {}
    virtual void sound(const std::string& name, float vol, float pitch, bool positional, const Vec3& pos) {}
    virtual float musicLocation() { return -1.0f; } // seconds into level music (0 without a stream)
    virtual bool musicEnabled() { return false; }   // music on: the level clock follows the music clock
    virtual float menuTransition() { return 0.0f; } // menu slide progress, 1 = menu covers the screen
    virtual void playLevelMusic(const std::string& name) {}
    virtual void queueNextLevelMusic(const std::string& name) {}
    virtual void swapLevelMusic() {}
    virtual bool levelMusicContinues() { return true; } // stream alive and the queued track already mixing in
    virtual void playBossLoop(const std::string& p) {}
    virtual void setNextBossLoop(const std::string& p, bool once) {}
    virtual bool bossLoopPlaying() { return false; }
    virtual std::string bossLoopName() { return ""; }
    virtual void stopLevelMusicChannel() {}
    virtual void setReverb(float lowPass, float a, float b, float c, float e0, float e1, float e2, float e3) {}
    virtual int createSound(const std::string& path) { return -1; }
    virtual void playSoundHandle(int h, float vol, float pitch, const Vec3& pos) {}
    virtual void particles(const std::string& effect, const Vec3& pos, const Vec3& vel, const float rgba[4],
                           int count) {}
    // one white particle exactly at pos/vel, frame -1 = random tile (room ambient presets)
    virtual void spawnParticle(const std::string& effect, const Vec3& pos, const Vec3& vel, int frame) {}
    // `count` particles around pos/vel, offsets of random direction and length randCentered(0, posSpread / velSpread)
    virtual void particleBurst(const std::string& effect, const Vec3& pos, float posSpread, const Vec3& vel,
                               float velSpread, const float rgba[4], int count) {}
    virtual void gameOver(int distance) {}
    // quick save: written 3 ticks after entering each campaign room, cleared outside campaign play
    virtual void quickSave(int room, int score, int balls, int streak) {}
    virtual void clearQuickSave() {}
    // looping sound emitters (0 laser hum, 1 ball roll, 2 object slide), called every tick per emitter
    virtual void loopSound(int slot, const Vec3& pos, float vol) {}
    // tutorials are owned by the app, the level only triggers them
    virtual bool tutorialCompleted(int id) { return true; }
    virtual void markTutorialCompleted(int id) {}
    virtual void startTutorial(int id) {}
    virtual bool tutorialActive() { return false; }
    virtual void shiftWorldZ(float dz) {}
    virtual void releaseBatch(int gpuId) {}
    virtual void setPausedRequest(bool paused) {}
    // the credits and more-games rooms end themselves. viaCommand goes through the "game.menu" command (bowling-lane
    // end), otherwise straight back to the menu without the transition sound
    virtual void returnToMenu(bool viaCommand) {}
    virtual void powerupPushed() {}     // HUD "pushPowerup": refresh the stack icons and the activate button
    virtual void checkpointReached() {} // record the checkpoint and save, before the HUD "checkpoint" command
    // the level of the room just entered, which level.restart starts again
    virtual void levelEntered(int levelIdx) {}
    virtual void log(const std::string& s);
};

struct DebrisPiece {
    Vec3 pos;
    Quat rot;
    Vec3 vel, angVel;
    Vec3 corner[4];
    uint32_t color = 0xffffffff;
    bool alive = true;
    float floorY = -10;
};

struct ObstacleDef {
    std::string type;
    Vec3 pos; // room-local
    Quat rot;
    std::vector<LevelScript::Param> params;
    bool created = false;
    Obstacle* obj = nullptr;
    std::string tutorial; // tutorial target: "crystal", "bar" or "button"
    Vec3 tutorialOffset;
    float tutorialDistance = 0;
};

// tutorial ids: 1 crystal, 2 bar, 3 button, 0 = none
int tutorialIdFromName(const std::string& name);
const char* tutorialName(int id);

struct Decal {
    Vec3 pos; // room-local (z already includes segment offset)
    Quat rot;
    float w = 1, h = 1;
    int tile = 0;
    float color[4] = {1, 1, 1, 1};
    float blend = 1;
};

// <water> surface: a wave simulation on a height grid
struct Water {
    Vec3 pos;                   // world
    float sx = 4, sz = 4;       // half extents along x / z (`size`)
    int nx = 32, ny = 32;       // grid resolution
    std::vector<float> H, P, V; // height, previous height and vertical speed, nx*ny each
};

struct MeshBatch {
    std::string path; // segments/.../x.mesh
    float zOffset = 0, zMin = 0, zMax = 0;
    int gpuId = -1; // renderer-owned handle
};

struct PowerupEnt {
    Vec3 pos; // world
    int type = 0;
    bool taken = false;
};

struct Laser {
    Vec3 origin, dir, end; // end: beam end after the hit test
};

// An obstacle instance (own Lua state) and the ScriptContext it runs in.
class Obstacle : public ScriptContext {
public:
    Obstacle(Level& lv, Room* room, bool isRoomScript, const Vec3& pos, const Quat& rot);
    ~Obstacle() override;
    Level& level;
    Room* room;
    bool roomScript;
    Vec3 pos;
    Quat rot;
    std::string type;
    std::unique_ptr<LevelScript> script;
    std::vector<Body*> bodies;
    std::vector<std::unique_ptr<Button>> buttons;
    float time = 0;
    float clearedAt = 0; // 0 = not cleared
    int scoreValue = 0;
    Vec3 scorePos, glowPos;
    bool passed = false;
    bool done = false;

    // ScriptContext
    World& world() override;
    bool isRoom() const override { return roomScript; }
    Vec3 posePos() const override { return pos; }
    Quat poseRot() const override { return rot; }
    Body* groundBody() override;
    void adoptBody(Body* b) override;
    Button* createButton(const Vec3& p) override;
    float levelTime() const override;
    float timeStep() const override;
    float obstacleTime() const override { return time; }
    Vec3 cameraPos() const override;
    float cameraSpeed() const override;
    float beat(float lookahead) const override;
    std::string getProperty(const std::string& k) override;
    void setProperty(const std::string& k, const std::string& v) override;
    const Material* findMaterial(const std::string& n) override;
    const std::vector<Vec3>* findConvex(const std::string& n) override;
    void setScore(int points, bool hasPos, const Vec3& p) override {
        scoreValue = points;
        if (hasPos) scorePos = p;
    }
    void setGlow(const Vec3& p) override { glowPos = p; }
    bool isCleared() const override { return clearedAt != 0; }
    void clear() override;
    void cameraShake(float a) override;
    bool hasBoss() const override;
    bool isClosestBoss() const override;
    float gravityScale() const override;
    float splitExtraRoomLength() const override;
    bool canPass() const override;
    float createSegment(const std::string& name, float zOffset) override;
    void roomLength(float l) override;
    void roomMusic(const std::string& s) override;
    void roomReverb(float a, float b, float c) override;
    void roomLowPass(float f) override;
    void roomEcho(float a, float b, float c, float d) override;
    void roomFog(const Vec3& a, const Vec3& b) override;
    void roomGravity(float g) override;
    void roomDifficulty(float d) override;
    void roomRotation(int turns, float amp) override;
    void roomParticles(const std::string& n) override;
    void roomSpeedMultiplier(float f) override;
    void roomBox(const Vec3& half, const Vec3& p, const Material& m) override;
    void addObstacle(const ObstacleDefRequest& r) override;
    int createSound(const std::string& path) override;
    void playSound(int handle, float vol, float pitch, const Vec3& worldPos) override;
    void smoke(const Vec3& worldPos, const Vec3& worldVel) override;
    void laser(const Vec3& o, const Vec3& d) override;
    void powerUp(const std::string& name, const Vec3& worldPos) override;
    void bodyDecal(const Body* b, const Vec3& off, const Quat& q, int sprite, float w, float h) override;
    void log(const std::string& m) override;
};

// mgBodyDecal: offset/rotation in the body frame, resolved against the body's (interpolated) pose when drawn
struct BodyDecalSprite {
    const Body* body;
    Vec3 off;
    Quat rot;
    int sprite;
    float w, h;
};

class Room {
public:
    Room(Level& lv, const RoomDef& def, int defIdx, float startOffset);
    ~Room();
    Level& level;
    std::string name;
    int defIdx = 0;     // index into Level::roomDefs
    float start = 0;    // room start, measured along -z
    float length = 100; // geometric length (mgLength)
    float difficulty = 0;
    bool lastOfLevel = false;
    std::string music = "silent";
    float reverb[3] = {0.3f, 0.5f, 0.3f}; // mgReverb a, b, c
    float lowPass = 0;
    float echo[4] = {0.2f, 0.5f, 0.3f, 0.8f};
    Vec3 fogA{1, 1, 1}, fogB{1, 1, 1};
    float gravityScale = 1;
    int rotTurns = 0;
    float rotAmp = 0;
    std::string particles;
    float speedMultiplier = 1; // mgSpeedMultiplier: stored like the original, which never reads it either
    Body* body = nullptr;      // static room body
    std::vector<ObstacleDef> defs;
    std::vector<std::unique_ptr<Obstacle>> obstacles;
    std::vector<Decal> decals;
    std::vector<MeshBatch> batches;
    std::vector<PowerupEnt> powerups;
    std::vector<float> reflMask;            // floor-top quads (x,y,z triples, 2 triangles each) of <box reflection=...>
    std::vector<uint8_t> reflGeom;          // mirrored box sides: pos3f + rgba8 per vertex (16 bytes)
    std::unique_ptr<Obstacle> roomObstacle; // runs rooms/<type>.lua
    float createdAtTime = 0;

    float zToRoom(float z) const { return -z - start; }
    float progress(float camZ) const { return (-camZ - start) / length; }
    float createSegment(const std::string& name, float zOffset);
    void update();
    bool tutorialTarget(const std::string& name, Vec3* pos, bool* hit) const; // first live def with that tutorial
    void checkTutorialTriggers();
    void ambientParticles(); // mgParticles preset, run at the end of update()
    void shiftZ(float dz);   // floating-origin shift
    bool hasBossAlive() const;
};

struct InputTap {
    bool down = false, pressed = false, released = false;
    float x = 0, y = 0; // pixels
};

struct ViewParams {
    float width = 1280, height = 720;
    float fovDeg() const {
        float a = width / height;
        return a > 2 ? 30.0f * a : 60.0f;
    }
};

class Level {
public:
    Level(Content& content, LevelHost* host);
    ~Level();

    // --- configuration / lifecycle ---
    Content& content;
    LevelHost* host;
    World world;
    Player player;
    bool start(const std::string& selector, int mode); // "0".."14" (campaign), "level:<name>", "quickload"
    void stop();
    void update(float dt); // one tick (call at fixed 60 Hz)
    void handleInput(const InputTap* taps, int count, const ViewParams& vp);

    // --- state ---
    std::vector<RoomDef> roomDefs;
    std::unique_ptr<Room> cur, next;
    bool active = false;
    bool realLevel = false;                            // the campaign playlist (any selector but level:<name>)
    bool creditsLevel = false, moregamesLevel = false; // creditsLevel is also set for moregames
    void registerBall(const Body* b);                  // count a ball for the statistics as it is destroyed
    void registerLiveBalls();                          // count the balls still in flight (the run is ending)
    int brokenThisRun = 0;
    long long roomAccStreak = 0, roomAccBalls = 0; // per-tick sums in the current room
    int roomAccTicks = 0;
    Vec3 cam{0, 1, 0};
    Vec3 prevCam{0, 1, 0}; // camera at the start of the last tick (render interpolation)
    Quat roll;
    float time = 0, camSpeed = 0, timeScale = 1, rawDt = 1.0f / 60, dt = 1.0f / 60;
    float pauseFade = 0;
    float outOfBalls = 0;                     // out-of-balls fade, 0..1
    float pain = 0, painTimer[3] = {0, 0, 0}; // HUD flash, per-player invulnerability
    bool painRight = false; // versus pain tint on the right half (last hit was not player 0), kept across levels
    float powerupTimer[kPowerupCount] = {};
    float rapidFireCd = 0;
    int rapidFireCount = 0;
    bool ended = false;
    bool hintPowerupSent = false, hintFrenzySent = false; // one-shot HUD hints per run
    GameOverState gameOverState = kGameOverNone;
    float runTime = 0, immuneUntil = 0;
    float distance = 0, distanceBase = 0;
    float musicT = 0, smoothLen = 0, roomClock = 0, speedBlend = 0, noMusicTimer = 0;
    bool musicStarted = false; // the first room's track waits for the menu to slide away
    int tickCount = 0;
    float cameraShake = 0;         // shared by all views
    float playerShake[2] = {0, 0}; // versus: per-player shake, applied to that player's view only
    int quickSaveCountdown = 0;
    struct Resume {
        int room = -1, balls = 25, streak = 0;
    } resume; // used by start("quickload")
    int streakSoundStep = 0;
    std::map<std::string, std::string> props; // generic property bag ("level.egg" etc.)

    // entities / effects consumed by the renderer
    std::vector<Laser> lasers;
    std::vector<DebrisPiece> debris;
    std::vector<BodyDecalSprite> bodyDecals;
    std::vector<Water> waters; // live water entities (destroyed once the camera has passed them)

    // --- API used by obstacles/rooms ---
    Vec3 gravity() const;
    void streakInc(int p);
    void streakAbort(int p);
    void addScore(int value, int p);
    void hitSomething(int p);
    void explosion(const Vec3& pos, float strength);
    void activatePowerup(int type);
    Body* createBall(int pl);
    Body* shoot(float px, float py, float speed, bool fromPlayer, int pl, const ViewParams& vp);
    bool checkHit(const Vec3& p);
    void centerCamera();
    void loopCamera(float d);
    void updateBoss();
    void processLasers();
    void impactSounds();
    void triggerGameOver();
    void onOutOfBallsReward(int balls);
    void onOutOfBallsSkip();
    int extraBallsUsed = 0;
    void loadNextRoom();
    void enterRoom(Room* r, bool fresh = true);
    void reset();
    std::string getProperty(const std::string& k);
    void setProperty(const std::string& k, const std::string& v);
    void addShake(float a) { cameraShake = std::max(cameraShake, std::min(a, 2.0f)); }
    Vec3 adjustedTouch; // the next fired ball aims here (tutorial tap)
    bool adjustedTouchPending = false;
    ViewParams lastView;
    void seedRandom(uint32_t s) {
        rnd_.seed(s);
        fxRnd_.seed(s ^ 0x9E3779B9u);
    }

private:
    void clearWorld();
    void updateCamera();
    void updateEntities();
    void updateDebris();
    void updateWaters();
    void updateRoll(float progress);
    void updateOutOfBalls();
    void advanceRooms();
    void explodeNitroBalls();
    void checkGameOver();
    void updateCreditsRoom();
    void spawnNitroTrail();
    void decayShake();
    void spawnDebrisAt(Body* b, const Vec3& pt, const float rgba[4], int count);
    int breakPieces_ = 0; // debris pieces spawned in the current breakage pass (capped at 32)
    std::mt19937 rnd_{7};
    std::mt19937 fxRnd_{11};   // cosmetic only (room ambient particles): rnd_ seeds the obstacle scripts
    std::mt19937 laserRnd_{5}; // cosmetic: laser hum volume and sparks, not reseeded by seedRandom()
    friend class Room;
};

} // namespace sh
