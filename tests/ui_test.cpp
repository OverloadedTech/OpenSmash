// Runs the original menu/HUD Lua scripts against a mock UiHost for a number of frames and reports script errors.
#include <cstdio>
#include <map>
#include <set>
#include "core/vfs.h"
#include "ui/ui_script.h"
using namespace sh;

struct MockHost : UiHost {
    Vfs* v;
    std::map<std::string, std::string> props;
    std::vector<std::string> commands;
    std::set<std::string> errors;
    int quads = 0;
    const Vfs& vfs() const override { return *v; }
    UiTexInfo texture(const std::string& p) override {
        UiTexInfo t;
        t.handle = 1;
        t.w = 512;
        t.h = 512;
        if (!v->exists(p) && !v->exists(p + ".mtx")) t.handle = -1;
        return t;
    }
    Vec2 viewport() const override { return Vec2(1920, 1080); }
    Mat4 guiToClip() const override {
        float s = 1080.0f / 1536.0f;
        return Mat4::ortho(0, 1920 / s, 1536, 0, -1, 1);
    }
    void drawQuad(int, const Mat4&, const float*, const float*) override { quads++; }
    void drawMesh(int, const Mat4&, const TextMesh&, const float*) override { quads++; }
    void drawPie(int, const Mat4&, float, const float*, const float*) override { quads++; }
    void drawBorder(int, const Mat4&, const float*, const float*) override { quads++; }
    void fullScreenColor(const float*, bool, bool) override {}
    void scissor(bool, int, int, int, int) override {}
    std::string getProperty(const std::string& k) override {
        auto it = props.find(k);
        return it == props.end() ? "" : it->second;
    }
    void setProperty(const std::string& k, const std::string& val) override { props[k] = val; }
    void command(const std::string& c) override { commands.push_back(c); }
    void uiLog(const std::string& s) override { errors.insert(s); }
    bool checkpointPos(int i, float& x, float& y) override {
        x = 300 + i * 600;
        y = 700;
        return true;
    }
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
    int failures = 0;
    for (int scene = 0; scene < 2; scene++) {
        MockHost h;
        h.v = &vfs;
        h.props = {{"game.loaded", "1"},
                   {"game.frame", "200"},
                   {"game.menutransition", "1"},
                   {"game.premium", "1"},
                   {"game.deploy", "1"},
                   {"game.platform", "android"},
                   {"game.graphics", "high"},
                   {"game.uiscale", "1"},
                   {"game.locale", "en"},
                   {"game.showui", "1"},
                   {"game.paused", "0"},
                   {"player.mode", "1"},
                   {"audio.musicEnabled", "1"},
                   {"audio.soundEnabled", "1"},
                   {"display.visibleLeft", "0"},
                   {"display.visibleTop", "0"},
                   {"display.visibleRight", "2731"},
                   {"display.visibleBottom", "1536"},
                   {"level.score", "123"},
                   {"level.ballcount", "25"},
                   {"level.streak", "12"},
                   {"level.distance", "123"},
                   {"level.progress", "0.3"},
                   {"level.pain", "0"},
                   {"level.credits", "0"},
                   {"level.moregames", "0"},
                   {"level.egg", "0"},
                   {"game.levelpos", "0"},
                   {"game.purchasing", "0"},
                   {"game.premiumprice", ""},
                   {"game.premiumorgprice", ""},
                   {"game.appleArcade", "0"},
                   {"game.signedin", "0"},
                   {"game.trymode", "0"},
                   {"game.level", "0"},
                   {"game.selectedLevel", "0"},
                   {"game.new_ballcount_feedback", "-1"},
                   {"game.new_ballcount_feedback_delay", "0.5"},
                   {"game.hint_mask_x", "0"},
                   {"game.hint_mask_y", "0"},
                   {"game.challengeType", "distance"},
                   {"game.challengeResultAmount", "0"},
                   {"game.out_of_balls_ad_reward", "10"},
                   {"game.lastdistance", "0"},
                   {"player.startcount", "5"},
                   {"player.rated", "0"},
                   {"player.adsfront", "0"},
                   {"player.adsshown", "0"},
                   {"player.adsonlyfree", "0"},
                   {"player.adssale", "0"},
                   {"player.powerup0", ""},
                   {"player.powerup1", ""},
                   {"player.powerup2", ""},
                   {"player.powerup3", ""},
                   {"level.ballcount0", "25"},
                   {"level.ballcount1", "25"},
                   {"level.streak0", "0"},
                   {"level.streak1", "0"},
                   {"level.lastpickup", "0"},
                   {"stats.distance", "100"},
                   {"stats.streak", "10"},
                   {"stats.peakballs", "30"},
                   {"stats.balls", "100"},
                   {"stats.ballshit", "50"},
                   {"stats.accstreak", "5"},
                   {"stats.accballs", "9"},
                   {"stats.rooms", "7"},
                   {"stats.broken", "40"}};
        UiGlobals g;
        UiScript s(&h, &g, scene == 0 ? "menu" : "hud");
        bool ok = scene == 0 ? s.load("menu/main.lua", {"include/menu_popups.lua"})
                             : s.load("hud/main.lua", {"include/hud_popups.lua"});
        if (!ok) {
            printf("FAIL load scene %d\n", scene);
            failures++;
            continue;
        }
        if (scene == 0) {
            // real startup: the menu shows its loading screen (game.loaded == 0) until the game sends "load"
            h.props["game.loaded"] = "0";
            for (int f = 0; f < 90; f++) {
                h.props["game.frame"] = std::to_string(f);
                s.tick(1.0f / 60, 1.0f / 60);
                s.drawWorld();
                s.draw();
                uiEndFrame(h, g);
            }
            s.handleCommand("load");
            h.props["game.loaded"] = "1";
        }
        for (int f = 0; f < 120; f++) {
            h.props["game.frame"] = std::to_string(200 + f);
            s.tick(1.0f / 60, 1.0f / 60);
            s.drawWorld();
            s.draw();
            uiEndFrame(h, g);
            if (f == 30)
                for (const char* c :
                     {"showoptions",  "hideoptions",    "showscore",      "hidescore", "showmode",      "hidemode",
                      "showupgrade",  "hideupgrade",    "showmore",       "hidemore",  "gameover",      "hidegameover",
                      "level 3",      "play",           "hideplaymenu",   "pickup 5",  "showpausemenu", "hidepausemenu",
                      "checkpoint 2", "showoutofballs", "hideoutofballs", "miss left", "pushPowerup",   "popPowerup",
                      "showwinner",   "hidewinner"}) {
                    s.handleCommand(c);
                    s.tick(1.0f / 60, 1.0f / 60);
                    s.drawWorld();
                    s.draw();
                }
        }
        printf("%s: objects=%d quads=%d commands=%zu errors=%zu\n", scene == 0 ? "menu" : "hud", s.objectCount(),
               h.quads, h.commands.size(), h.errors.size());
        // known benign errors that the original engine also swallows every frame (uninitialised globals in the shipped
        // script)
        const char* benign[] = {"menu/main.lua:818: attempt to compare number with nil"};
        int bad = 0;
        for (auto& e : h.errors) {
            bool b = false;
            for (const char* k : benign)
                if (e.find(k) != std::string::npos) b = true;
            if (!b) {
                printf("  ERR %s\n", e.c_str());
                bad++;
            }
        }
        if (bad) failures++;
    }
    return failures ? 1 : 0;
}
