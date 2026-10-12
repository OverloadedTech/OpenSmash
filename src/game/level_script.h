// Lua 5.2 host for room and obstacle scripts (rooms/*.lua, obstacles/*.lua) and their `mg*` API.
// The engine is reached through ScriptContext, so the same code runs in the game, in headless
// simulation and in unit tests.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "core/math.h"
#include "physics/world.h"
#include "script/lua_host.h"

namespace sh {

struct Button {
    Vec3 pos;
    bool down = false, prevDown = false;
    bool tutorialRadius = false; // sticky once a tutorial was active: 1.0 m press radius instead of 0.5
};

struct ObstacleDefRequest { // mgObstacle
    std::string type;
    Vec3 pos, euler;
    std::vector<std::pair<std::string, std::string>> params;
};

// Services the scripts need from the engine. One context per script instance (room or obstacle).
class ScriptContext {
public:
    virtual ~ScriptContext() = default;
    virtual World& world() = 0;
    virtual bool isRoom() const = 0;
    virtual Vec3 posePos() const = 0; // obstacle (or room) pose
    virtual Quat poseRot() const = 0;
    virtual Body* groundBody() = 0;  // static body used for nil joint ends
    virtual void adoptBody(Body*) {} // the body now belongs to this obstacle
    virtual Button* createButton(const Vec3& worldPos) = 0;

    virtual float levelTime() const = 0;
    virtual float timeStep() const = 0;
    virtual float obstacleTime() const { return 0; }
    virtual Vec3 cameraPos() const = 0;
    virtual float cameraSpeed() const { return 0; }
    virtual float beat(float lookahead) const { return 0; }

    virtual std::string getProperty(const std::string& key) = 0;
    virtual void setProperty(const std::string& key, const std::string& value) = 0;
    virtual const Material* findMaterial(const std::string& name) = 0;
    virtual const std::vector<Vec3>* findConvex(const std::string& name) = 0;

    // scoring / obstacle state
    virtual void setScore(int points, bool hasPos, const Vec3& localPos) {}
    virtual void setGlow(const Vec3& p) {}
    virtual bool isCleared() const { return false; }
    virtual void clear() {}
    virtual void cameraShake(float a) {}
    virtual bool hasBoss() const { return false; }
    virtual bool isClosestBoss() const { return false; }
    virtual float gravityScale() const { return 1; }
    virtual float splitExtraRoomLength() const { return 0; }
    virtual bool canPass() const { return true; }

    // room-level
    virtual float createSegment(const std::string& name, float zOffset) { return 0; }
    virtual void roomLength(float l) {}
    virtual void roomMusic(const std::string& s) {}
    virtual void roomReverb(float a, float b, float c) {}
    virtual void roomLowPass(float f) {}
    virtual void roomEcho(float a, float b, float c, float d) {}
    virtual void roomFog(const Vec3& a, const Vec3& b) {}
    virtual void roomGravity(float g) {}
    virtual void roomDifficulty(float d) {}
    virtual void roomRotation(int turns, float amp) {}
    virtual void roomParticles(const std::string& n) {}
    virtual void roomSpeedMultiplier(float f) {}
    virtual void roomBox(const Vec3& half, const Vec3& pos, const Material& m) {}
    virtual void addObstacle(const ObstacleDefRequest& r) {}

    // effects
    virtual int createSound(const std::string& path) { return -1; }
    virtual void playSound(int handle, float vol, float pitch, const Vec3& worldPos) {}
    virtual void smoke(const Vec3& worldPos, const Vec3& worldVel) {}
    virtual void laser(const Vec3& worldOrigin, const Vec3& worldDir) {}
    virtual void powerUp(const std::string& name, const Vec3& worldPos) {}
    virtual void bodyDecal(const Body* b, const Vec3& off, const Quat& rot, int sprite, float w, float h) {}
    virtual void log(const std::string& msg);
};

// Per-script random generator, giving the original game's sequence for a given seed.
class ScriptRng {
public:
    void setSeed(uint32_t s) { s_ = s; }
    uint32_t next();
    float getFloat(float a, float b) { return a + (float)next() * 2.3283064e-10f * (b - a); }
    int getInt(int a, int b);

private:
    uint32_t s_ = 0;
};

class LevelScript {
public:
    using Param = std::pair<std::string, std::string>;
    LevelScript(ScriptContext* ctx, std::vector<Param> params);
    ~LevelScript();

    // runs source followed by prelude (rooms/include.lua for room scripts), then registers the mg* API and calls init()
    bool load(const std::string& source, const std::string& prelude, const std::string& chunkName, uint32_t seed);
    void tick();
    void draw();

    // --- state used by the mg* handlers (public: the handlers are free functions) ---
    ScriptContext* ctx;
    std::vector<Param> params;
    Material mat; // material for the next shapes built
    Vec3 shapeScale{1, 1, 1};
    float chamfer = 0;
    ScriptRng rng;
    std::vector<int> sounds;

    const std::string* param(const std::string& name) const;
    Vec3 toWorld(const Vec3& p) const;
    Vec3 dirToWorld(const Vec3& v) const;
    Vec3 toLocal(const Vec3& w) const;
    void call(const char* fn);

private:
    std::unique_ptr<LuaHost> host_;
    void registerApi();
};

} // namespace sh
