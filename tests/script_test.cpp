// Runs init() + a few tick()s of every room/obstacle script against a real physics World with a mock context.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include "core/vfs.h"
#include "game/content.h"
#include "game/level_script.h"

using namespace sh;

struct MockCtx : ScriptContext {
    World w;
    Vec3 pos{0, 0, -20};
    bool room = false;
    Content* content = nullptr;
    std::set<std::string> errors;
    std::map<std::string, int> calls;
    float t = 0;
    World& world() override { return w; }
    bool isRoom() const override { return room; }
    Vec3 posePos() const override { return pos; }
    Quat poseRot() const override { return Quat(); }
    Body* groundBody() override { return w.ground(); }
    std::vector<std::unique_ptr<Button>> buttons;
    Button* createButton(const Vec3& p) override {
        buttons.emplace_back(new Button);
        buttons.back()->pos = p;
        return buttons.back().get();
    }
    float levelTime() const override { return t; }
    float timeStep() const override { return 1.0f / 60; }
    Vec3 cameraPos() const override { return Vec3(0, 1, 5 - t * 10); }
    std::string getProperty(const std::string& k) override {
        if (k == "player.mode") return "1";
        return "0";
    }
    void setProperty(const std::string&, const std::string&) override {}
    const Material* findMaterial(const std::string& n) override { return content->material(n); }
    const std::vector<Vec3>* findConvex(const std::string& n) override { return content->convex(n); }
    float createSegment(const std::string& n, float z) override {
        calls["segment"]++;
        return 16;
    }
    void log(const std::string& m) override { errors.insert(m); }
};

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : getenv("SH_ASSETS");
    if (!path) {
        printf("SKIP: pass an APK/asset dir or set SH_ASSETS\n");
        return 77;
    }
    Vfs vfs;
    std::string err;
    if (!vfs.mount(path, &err)) {
        printf("mount: %s\n", err.c_str());
        return 1;
    }
    Content content(vfs);
    if (!content.ok()) {
        printf("FAIL: materials.xml or game.xml did not load\n");
        return 1;
    }
    std::string roomInclude = vfs.readText("rooms/include.lua");
    int failures = 0, scripts = 0;
    for (const std::string& f : vfs.list("")) {
        bool isObs = f.rfind("obstacles/", 0) == 0 && f.size() > 4 && f.compare(f.size() - 4, 4, ".lua") == 0 &&
                     f != "obstacles/include.lua";
        bool isRoom = f.rfind("rooms/", 0) == 0 && f.size() > 4 && f.compare(f.size() - 4, 4, ".lua") == 0 &&
                      f != "rooms/include.lua";
        if (!isObs && !isRoom) continue;
        MockCtx ctx;
        ctx.content = &content;
        ctx.room = isRoom;
        LevelScript s(&ctx, {});
        if (!s.load(vfs.readText(f), isRoom ? roomInclude : "", f, 1)) {
            printf("FAIL load %s: %s\n", f.c_str(), ctx.errors.empty() ? "?" : ctx.errors.begin()->c_str());
            failures++;
            continue;
        }
        for (int i = 0; i < 120; i++) {
            ctx.t = i / 60.0f;
            s.tick();
            if (!isRoom) ctx.w.step(1.0f / 60);
        }
        s.draw();
        scripts++;
        if (!ctx.errors.empty()) {
            printf("FAIL %s: %s\n", f.c_str(), ctx.errors.begin()->c_str());
            failures++;
        }
    }
    printf("scripts=%d failures=%d\n", scripts, failures);
    return failures ? 1 : 0;
}
