// Headless simulation: loads levels and plays them with a simple bot for N seconds. Used by CI to catch
// crashes/regressions (no window, GL or audio). Usage:
//   opensmash_headless --assets <apk|dir> [--level N|all|level:name] [--mode 0..5] [--seconds S] [--bot] [--god]
//   [--until-room name] [--full] [--verbose]
// --full: campaign checkpoints run (god mode + bot) until they reach the next checkpoint's first room or the level
// ends, so `--level all --full` visits every campaign room.
// --seconds is then a per-run cap (default 600, 120 for zen and endless).
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include "core/vfs.h"
#include "game/content.h"
#include "game/level.h"

using namespace sh;

namespace {
struct CountingHost : LevelHost {
    int errors = 0, loops[3] = {0, 0, 0};
    bool outOfBalls = false;
    std::set<std::string> logs;
    bool verbose = false;
    void hudCommand(const std::string& s) override {
        if (s == "showoutofballs") outOfBalls = true;
        if (verbose) printf("  hud: %s\n", s.c_str());
    }
    int createSound(const std::string&) override { return 1; }
    void loopSound(int slot, const Vec3&, float) override {
        if (slot >= 0 && slot < 3) loops[slot]++;
    }
    void log(const std::string& s) override {
        errors++;
        if (logs.insert(s).second) fprintf(stderr, "%s\n", s.c_str());
    }
};
} // namespace

int main(int argc, char** argv) {
    std::string assets, levelArg = "0";
    int mode = 1;
    float seconds = 20;
    bool bot = false, verbose = false, god = false, full = false, secondsGiven = false;
    std::string untilRoom;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--assets") assets = next();
        else if (a == "--level") levelArg = next();
        else if (a == "--mode") mode = atoi(next().c_str());
        else if (a == "--seconds") {
            seconds = (float)atof(next().c_str());
            secondsGiven = true;
        } else if (a == "--bot") bot = true;
        else if (a == "--god") god = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--until-room") untilRoom = next();
        else if (a == "--full") full = true;
        else {
            printf("usage: opensmash_headless --assets <apk|dir> [--level N|all|level:name] [--mode 0..5] [--seconds "
                   "S] [--bot] [--god] [--until-room name] [--full] [--verbose]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    if (assets.empty()) {
        const char* e = getenv("SH_ASSETS");
        if (e) assets = e;
    }
    if (assets.empty()) {
        printf("SKIP: no assets (use --assets or SH_ASSETS)\n");
        return 77;
    }
    Vfs vfs;
    std::string err;
    if (!mountGame(vfs, assets, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Content content(vfs);
    if (!content.ok()) {
        fprintf(stderr, "content failed to load\n");
        return 1;
    }

    std::vector<std::string> selectors;
    if (levelArg == "all") {
        for (int i = 0; i < kCampaignLevels; i++) selectors.push_back(std::to_string(i)); // campaign starts
        selectors.push_back("level:zen");
        selectors.push_back("level:endless");
    } else selectors.push_back(levelArg);

    if (full) {
        god = bot = true;
        if (!secondsGiven) seconds = 600;
    }

    // First room index of each selector (only meaningful for campaign checkpoints "0".."11").
    auto startRoom = [&](const std::string& sel) {
        CountingHost host;
        Level level(content, &host);
        int idx = level.start(sel, mode) ? level.player.roomIdx : -1;
        level.stop();
        return idx;
    };

    int failures = 0;
    for (size_t si = 0; si < selectors.size(); si++) {
        const std::string& sel = selectors[si];
        bool campaign = !sel.empty() && isdigit((unsigned char)sel[0]);
        int stopAtRoom = -1; // --full: stop once this room index is reached
        if (full && campaign) stopAtRoom = startRoom(std::to_string(atoi(sel.c_str()) + 1));
        CountingHost host;
        host.verbose = verbose;
        Level level(content, &host);
        if (!level.start(sel, mode)) {
            printf("FAIL %s: could not start\n", sel.c_str());
            failures++;
            continue;
        }
        ViewParams vp;
        auto t0 = std::chrono::steady_clock::now();
        float cap = (full && !campaign && !secondsGiven) ? 120 : seconds; // zen/endless have no checkpoints to reach
        int ticks = (int)(cap * 60), ran = 0;
        size_t peakBodies = 0, peakDebris = 0;
        for (int t = 0; t < ticks; t++) {
            InputTap tap;
            if (bot && t % 12 == 0 && level.player.balls > 0) {
                tap.pressed = tap.down = true;
                tap.x = vp.width * (0.3f + 0.4f * ((t / 12) % 5) / 4.0f);
                tap.y = vp.height * 0.55f;
            }
            if (god) {
                level.player.balls = std::max(level.player.balls, 50);
                level.immuneUntil = 1e9f;
            }
            level.handleInput(&tap, 1, vp);
            level.update(1.0f / 60);
            ran = t + 1;
            if (host.outOfBalls) { // headless: always decline the revive popup
                host.outOfBalls = false;
                level.onOutOfBallsSkip();
            }
            peakBodies = std::max(peakBodies, level.world.bodies().size());
            peakDebris = std::max(peakDebris, level.debris.size());
            if (level.ended) break;
            if (stopAtRoom >= 0 && level.player.roomIdx >= stopAtRoom) break;
            if (!untilRoom.empty() && level.cur && level.cur->name.find(untilRoom) != std::string::npos) {
                printf("reached room %s at tick %d\n", level.cur->name.c_str(), t);
                break;
            }
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("%-14s mode=%d ticks=%d room=%s roomIdx=%d dist=%d balls=%d streak=%d broken=%d bodies(peak)=%zu "
               "debris(peak)=%zu loops=%d/%d/%d ended=%d goState=%d oob=%.2f  %.0f ms (%.2f ms/tick)\n",
               sel.c_str(), mode, ran, level.cur ? level.cur->name.c_str() : "?", level.player.roomIdx,
               level.player.distance, level.player.balls, level.player.streak, level.world.brokenBodyCount, peakBodies,
               peakDebris, host.loops[0], host.loops[1], host.loops[2], (int)level.ended, (int)level.gameOverState,
               level.outOfBalls, ms, ms / std::max(1, ran));
        if (full && campaign && !level.ended && stopAtRoom >= 0 && level.player.roomIdx < stopAtRoom) {
            printf("  FAIL: stuck before room %d (cap %.0f s)\n", stopAtRoom, cap);
            failures++;
        }
        if (host.errors) {
            printf("  %d script/log errors\n", host.errors);
            failures++;
        }
        level.stop();
    }
    return failures ? 1 : 0;
}
