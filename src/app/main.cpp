// OpenSmash entry point: command line, finding the game data, the SDL window and the main loop.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h> // WinMain wrapper: the Windows build is a GUI app (no console window)
#ifndef OPENSMASH_VERSION
#define OPENSMASH_VERSION "dev"
#endif
#ifdef _WIN32
void sh_attach_parent_console(); // win_console.cpp: GUI subsystem exe, show --help/--version output in cmd/PowerShell
void sh_detach_parent_console(); // ...and let go of that console before an interactive session starts
#endif
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include "app/game_app.h"
#include "audio/audio.h"
#include "core/vfs.h"
#include "render/game_renderer.h"
#include "render/gl.h"
#include "render/ui_renderer.h"

using namespace sh;

namespace {

struct Args {
    std::string assets;
    std::string level; // start a level directly (skips the menu)
    int mode = 1;
    std::string screenshot; // write PNG after --ticks ticks and exit
    int ticks = 120;
    float runSeconds = 0; // real-time run limit (non-deterministic), takes the screenshot at the end
    bool forceAudio = false;
    bool god = false;
    bool bot = false, offscreen = false, checkShaders = false, fullscreen = false, unlockAll = false, noAudio = false,
         vsync = true;
    int width = 1280, height = 720;
    std::string quality = "high";
    std::string userDir; // overrides the per-user save directory
    bool modeGiven = false, qualityGiven = false;
    std::vector<std::pair<int, std::string>> commands; // scripted game commands (frame, command) for screenshot runs
    struct Click {
        float x, y;
        int frame;
    };
    std::vector<Click> clicks;  // scripted pointer clicks (window pixels) for screenshot runs
    std::vector<Click> touches; // scripted touches (window pixels, one finger each, held 5 frames)

    bool capture() const { return offscreen && !screenshot.empty(); }
    // a fixed 1/60 s step per frame, so captures come out the same every time
    bool deterministic() const { return !screenshot.empty() && runSeconds <= 0; }
};

void usage() {
    printf("OpenSmash " OPENSMASH_VERSION " - open-source engine for Smash Hit\n"
           "usage: opensmash [--assets <apk|folder>] [options]\n"
           "\n"
           "  --assets PATH          Smash Hit .apk or unzipped asset folder (default: bundled, remembered or picked)\n"
           "  --level N|level:name   start a level directly instead of the main menu\n"
           "  --mode 0..5            training, classic, expert, zen, versus, coop\n"
           "  --quality Q            low, medium or high\n"
           "  --width W --height H   window size\n"
           "  --fullscreen           start in fullscreen\n"
           "  --no-vsync             do not wait for vertical sync\n"
           "  --no-audio             no sound\n"
           "  --unlock-all           every checkpoint available\n"
           "  --user-dir DIR         keep settings and progress in DIR (portable install)\n"
           "  --check-shaders        compile every game shader and exit\n"
           "  --version              print the version and exit\n"
           "\n"
           "testing and captures:\n"
           "  --offscreen            no visible window (implies --no-audio unless --audio is given)\n"
           "  --screenshot FILE      write a PNG after --ticks frames (fixed 1/60 s steps) and exit\n"
           "  --ticks N              frames to run before the screenshot (default 120)\n"
           "  --run-seconds S        run in real time for S seconds, then exit (or take the screenshot)\n"
           "  --bot                  throw balls automatically\n"
           "  --god                  never run out of balls, never get hit\n"
           "  --click X,Y@FRAME      click at window pixel X,Y on that frame (repeatable)\n"
           "  --touch X,Y@FRAME      the same as a touchscreen finger (repeatable)\n"
           "  --cmd [FRAME:]COMMAND  run a game command, e.g. \"300:level.restart\" (repeatable)\n"
           "\n"
           "environment: SH_ASSETS (default for --assets), SH_LOCALE (language code, e.g. de),\n"
           "SH_UI_LOG_ALL (print every menu/HUD script error, not just the first of each)\n");
}

// Fatal startup error: stderr plus a message box (the Windows build has no console, macOS launches from Finder).
bool g_noDialogs = false; // automated runs (offscreen captures, --check-shaders) never block on a dialog
void fatal(SDL_Window* w, const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    if (!g_noDialogs) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenSmash", msg.c_str(), w);
}

// Gamepad face buttons by their printed label, so A/Cross throws and B/Circle goes back on every layout: SDL names
// buttons by position, and on Nintendo pads (Switch Pro, Joy-Con, 8BitDo in Switch mode) A is EAST and B is SOUTH.
enum PadRole { kPadNone, kPadThrow, kPadBack };
PadRole padRole(SDL_Gamepad* gp, Uint8 b) {
    switch (SDL_GetGamepadButtonLabel(gp, (SDL_GamepadButton)b)) {
    case SDL_GAMEPAD_BUTTON_LABEL_A:
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return kPadThrow;
    case SDL_GAMEPAD_BUTTON_LABEL_B:
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return kPadBack;
    default: return b == SDL_GAMEPAD_BUTTON_SOUTH ? kPadThrow : b == SDL_GAMEPAD_BUTTON_EAST ? kPadBack : kPadNone;
    }
}

std::string joinPath(const std::string& dir, const std::string& name) {
    bool sep = dir.empty() || dir.back() == '/' || dir.back() == '\\';
    return dir + (sep ? "" : "/") + name;
}

// ---------------------------------------------------------------- command line

// Returns -1 to continue, otherwise the exit code.
int parseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        if (s.rfind("-psn_", 0) == 0)
            continue; // macOS LaunchServices process serial number (Finder / first quarantined launch)
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "--assets") a.assets = next();
        else if (s == "--level") a.level = next();
        else if (s == "--mode") {
            a.mode = atoi(next().c_str());
            a.modeGiven = true;
        } else if (s == "--screenshot") a.screenshot = next();
        else if (s == "--ticks") a.ticks = atoi(next().c_str());
        else if (s == "--bot") a.bot = true;
        else if (s == "--god") a.god = true;
        else if (s == "--offscreen") a.offscreen = true;
        else if (s == "--check-shaders") a.checkShaders = true;
        else if (s == "--fullscreen") a.fullscreen = true;
        else if (s == "--unlock-all") a.unlockAll = true;
        else if (s == "--no-audio") a.noAudio = true;
        else if (s == "--no-vsync") a.vsync = false;
        else if (s == "--quality") {
            a.quality = next();
            for (char& ch : a.quality) ch = (char)std::tolower((unsigned char)ch);
            if (a.quality != "low" && a.quality != "medium" && a.quality != "high") {
                usage();
                fprintf(stderr, "--quality must be low, medium or high\n");
                return 2;
            }
            a.qualityGiven = true;
        } else if (s == "--width") a.width = atoi(next().c_str());
        else if (s == "--height") a.height = atoi(next().c_str());
        else if (s == "--cmd") {
            std::string c = next();
            int f = -1;
            size_t colon = c.find(':');
            if (colon != std::string::npos && sscanf(c.c_str(), "%d:", &f) == 1) c = c.substr(colon + 1);
            a.commands.emplace_back(f, c);
        } else if (s == "--click" || s == "--touch") {
            Args::Click c{0, 0, 0};
            sscanf(next().c_str(), "%f,%f@%d", &c.x, &c.y, &c.frame);
            (s == "--click" ? a.clicks : a.touches).push_back(c);
        } else if (s == "--run-seconds") a.runSeconds = (float)atof(next().c_str());
        else if (s == "--audio") a.forceAudio = true;
        else if (s == "--user-dir") a.userDir = next();
        else if (s == "--version") {
            printf("OpenSmash %s\n", OPENSMASH_VERSION);
            return 0;
        } else if (s == "--help" || s == "-h") {
            usage();
            return 0;
        } else if (s.rfind("--", 0) == 0) {
            usage();
            fprintf(stderr, "unknown option '%s'\n", s.c_str());
            return 2;
        } else
            fprintf(stderr, "ignoring unexpected argument '%s'\n", s.c_str()); // e.g. arguments added by a launcher/OS
    }
    if (a.assets.empty()) {
        const char* e = getenv("SH_ASSETS");
        if (e) a.assets = e;
    }
    return -1;
}

// Portable mode: create the --user-dir folder so settings, progress and the asset path persist.
// Returns a warning when that fails.
std::string prepareUserDir(const Args& a) {
    if (a.userDir.empty()) return "";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(a.userDir), ec);
    if (std::filesystem::is_directory(std::filesystem::path(a.userDir), ec)) return "";
    std::string warning = "Cannot create the folder given with --user-dir ('" + a.userDir +
                          "'): settings and progress will not be saved.";
    fprintf(stderr, "warning: %s\n", warning.c_str()); // still attached to a parent console here
    return warning;
}

// Where settings, progress and the remembered asset location live: --user-dir if given, nothing for headless
// captures, otherwise the per-user data folder. Ends with a separator unless empty.
std::string userDataDir(const Args& a) {
    std::string dir;
    if (!a.userDir.empty()) {
        dir = a.userDir;
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    } else if (!a.capture()) {
        if (char* pref = SDL_GetPrefPath("", "OpenSmash")) {
            dir = pref;
            SDL_free(pref);
        }
    }
    return dir;
}

// ---------------------------------------------------------------- finding the game data

// every candidate must hold Smash Hit data, so a foreign APK or a stale remembered path is skipped
bool isGameData(const std::string& path) {
    Vfs t;
    return mountGame(t, path);
}

// Assets shipped with a release package: <exe dir>/assets (Windows/Linux), OpenSmash.app/Contents/Resources/assets
// (SDL's base path inside a macOS bundle), or ./assets.
std::string findBundledAssets() {
    std::vector<std::string> roots;
    const char* home = getenv("OPENSMASH_HOME"); // set by the Linux launcher script
    if (home && *home) roots.push_back(home);
    if (const char* bp = SDL_GetBasePath()) roots.push_back(bp);
    roots.push_back(".");
    for (auto& r : roots) {
        std::string p = joinPath(r, "assets");
        if (isGameData(p)) return p;
    }
    return "";
}

// The location saved after the last successful start.
std::string rememberedAssets(const std::string& dataDir) {
    std::string p;
    if (FILE* f = fopen((dataDir + "assets_path.txt").c_str(), "rb")) {
        char buf[4096] = {0};
        if (fgets(buf, sizeof buf, f)) {
            p = buf;
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
        }
        fclose(f);
    }
    return !p.empty() && isGameData(p) ? p : "";
}

// Any .apk next to the executable or in the working directory (macOS: next to the .app bundle).
std::string findApkNearby() {
    std::vector<std::string> dirs = {"."};
    if (const char* home = getenv("OPENSMASH_HOME"); home && *home) dirs.push_back(home);
    if (const char* bp = SDL_GetBasePath()) {
        dirs.push_back(bp);
        std::string b = bp;
        const std::string tail = ".app/Contents/Resources/";
        if (b.size() > tail.size() && b.compare(b.size() - tail.size(), tail.size(), tail) == 0) {
            b.resize(b.size() - tail.size()); // .../OpenSmash
            b.resize(b.rfind('/') + 1);       // .../ (the folder holding the .app)
            dirs.push_back(b);
        }
    }
    for (auto& d : dirs) {
        int n = 0;
        char** files = SDL_GlobDirectory(d.c_str(), "*.apk", SDL_GLOB_CASEINSENSITIVE, &n); // also "SmashHit.APK"
        std::string found;
        for (int i = 0; files && i < n && found.empty(); i++) {
            std::string p = joinPath(d, files[i]);
            if (isGameData(p)) found = p;
        }
        if (files) SDL_free(files);
        if (!found.empty()) return found;
    }
    return "";
}

// Ask the user. A file that is not Smash Hit data is refused and the dialog opens again until it is cancelled.
std::string askForApk() {
    struct Pick {
        std::atomic<bool> done{false};
        std::string path;
    };
    for (;;) {
        // SDL may answer from another thread, and after a quit request we stop waiting: the state lives on the heap
        auto* pick = new Pick;
        SDL_DialogFileFilter filter = {"Smash Hit APK", "apk"};
        SDL_ShowOpenFileDialog(
            [](void* ud, const char* const* list, int) {
                auto* p = (Pick*)ud;
                if (list && list[0]) p->path = list[0];
                p->done.store(true, std::memory_order_release);
            },
            pick, nullptr, &filter, 1, nullptr, false);
        bool quit = false;
        while (!quit && !pick->done.load(std::memory_order_acquire)) {
            SDL_Event e;
            while (SDL_PollEvent(&e))
                if (e.type == SDL_EVENT_QUIT) quit = true;
            SDL_Delay(30);
        }
        if (quit) return ""; // the dialog may still answer: pick is deliberately left alive
        const std::string path = pick->path;
        delete pick;
        if (path.empty()) return "";
        Vfs t;
        std::string why;
        if (mountGame(t, path, &why)) return path;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "OpenSmash",
                                 ("This is not a Smash Hit APK.\n\n" + why).c_str(), nullptr);
    }
}

// Mounts the game data: --assets (or SH_ASSETS), assets bundled with a release package, the remembered location, an
// APK next to the game, or one picked in a file dialog. On success a.assets holds the path and rememberFile is where
// to save it once the game has started. Returns -1 on success, otherwise the exit code.
int mountAssets(Args& a, Vfs& vfs, std::string& rememberFile) {
    const std::string dataDir = userDataDir(a);
    std::string err;
    if (!a.assets.empty()) {
        if (!mountGame(vfs, a.assets, &err)) {
            fatal(nullptr, err);
            return 1;
        }
    } else {
        std::string p = findBundledAssets();
        const bool bundled = !p.empty();
        if (p.empty() && !dataDir.empty()) p = rememberedAssets(dataDir);
        if (p.empty()) p = findApkNearby();
        if (p.empty() && !g_noDialogs) p = askForApk();
        if (p.empty() || !vfs.mount(p, &err)) {
            usage();
            fatal(nullptr,
#ifdef __APPLE__
                  "No Smash Hit assets found.\n\nThis copy of OpenSmash does not include the game assets. Pick a Smash "
                  "Hit .apk in the file dialog, or put it next to OpenSmash.app after moving the app with Finder (a "
                  "freshly downloaded app cannot see files next to it).");
#else
                  "No Smash Hit assets found.\n\nThis copy of OpenSmash does not include the game assets. Put a Smash "
                  "Hit .apk next to the game, or pick it in the file dialog (or run with --assets <apk|folder>).");
#endif
            return 2;
        }
        a.assets = p;
        if (bundled) return -1; // a package's own assets are found again without remembering them
    }
    if (!dataDir.empty()) rememberFile = dataDir + "assets_path.txt";
    return -1;
}

void rememberAssets(const std::string& file, const std::string& assets) {
    if (file.empty()) return;
    if (FILE* f = fopen(file.c_str(), "wb")) {
        std::error_code ec;
        std::string abs = std::filesystem::absolute(assets, ec).lexically_normal().string();
        fprintf(f, "%s\n", ec ? assets.c_str() : abs.c_str());
        fclose(f);
    }
}

// ---------------------------------------------------------------- window

// An OpenGL 3.3 core window. Returns false after reporting the error.
bool createWindow(const Args& a, SDL_Window*& win, SDL_GLContext& ctx) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                   (a.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    win = SDL_CreateWindow("OpenSmash", a.width, a.height, flags);
    if (!win) {
        fatal(nullptr, std::string("Cannot create a window: ") + SDL_GetError());
        return false;
    }
    // floating windows stay 4:3 or wider (narrower ones are letterboxed)
    SDL_SetWindowAspectRatio(win, 4.0f / 3.0f, 0.0f);
    ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        fatal(win, std::string("OpenSmash needs OpenGL 3.3 (core profile). Update your graphics driver.\n\n") +
                       SDL_GetError());
        return false;
    }
    if (!gl::load([](const char* n) { return (void*)SDL_GL_GetProcAddress(n); })) {
        fatal(win,
              "Your OpenGL driver lacks functions OpenSmash needs (OpenGL 3.3 core). Update your graphics driver.");
        return false;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("GL %s | %s\n", gl::GetString(GL_VERSION), gl::GetString(GL_RENDERER));
    SDL_GL_SetSwapInterval(a.vsync ? 1 : 0);
    return true;
}

// --check-shaders: compile every shader in the game data.
int checkShaders(const Vfs& vfs) {
    int bad = 0, n = 0;
    for (const std::string& f : vfs.list("shaders/")) {
        if (f.size() < 5 || f.compare(f.size() - 5, 5, ".glsl") != 0) continue;
        Shader sh;
        std::string err;
        n++;
        if (!sh.build(vfs.readText(f), f, &err)) {
            if (f == "shaders/model_orig.glsl") {
                printf("known-unused invalid shader skipped: %s\n", f.c_str());
                n--;
                continue;
            }
            printf("FAIL %s\n", err.c_str());
            bad++;
        }
    }
    printf("shaders: %d compiled, %d failed\n", n - bad, bad);
    return bad ? 1 : 0;
}

void saveScreenshot(const std::string& path, int w, int h) {
    std::vector<uint8_t> px((size_t)w * h * 4);
    gl::ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    stbi_flip_vertically_on_write(1);
    stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
}

// ---------------------------------------------------------------- input

// Pointers. The primary one drives the UI and throws: mouse, Space, the first gamepad and the first touch finger. A
// second gamepad and further fingers only throw (the split-screen players).
// Button edges are queued and applied one per frame, so every press gets a frame of its own before its release (a
// touchpad tap delivers both in one batch; the UI needs to see the press held). Motion that arrives while edges are
// queued stays behind them: each edge's frame sees the edge's own position, the next frame the motion that followed.
class Input {
public:
    Input(float x, float y) {
        primary.p.x = x;
        primary.p.y = y;
    }

    void beginFrame() {
        primary.p.pressed = primary.p.released = false;
        second.p.pressed = second.p.released = false;
        for (auto& f : fingers) f.second.p.pressed = f.second.p.released = false;
    }

    void handleEvent(const SDL_Event& e, GameApp& app, SDL_Window* win, int ww, int wh, bool& running);

    void applyEdges() {
        applyEdge(primary);
        applyEdge(second);
        for (auto& f : fingers) applyEdge(f.second);
    }

    void updatePads(GameApp& app, int ww, int wh, double dt);

    // The pointers for this frame in drawable pixels: the primary one, plus the split-screen players' pointers.
    PointerState pointers(GameApp& app, float sx, float sy, std::vector<PointerState>& extra) const {
        app.extraCrosshairs.clear();
        if (pads[1].gp && pads[1].used) app.extraCrosshairs.emplace_back(second.p.x * sx, second.p.y * sy);
        auto scaled = [&](PointerState q) {
            q.x *= sx;
            q.y *= sy;
            return q;
        };
        if (pads[1].gp) extra.push_back(scaled(second.p));
        for (auto& f : fingers) extra.push_back(scaled(f.second.p));
        return scaled(primary.p);
    }

    // a lifted finger is gone once its release was seen
    void endFrame() {
        for (auto it = fingers.begin(); it != fingers.end();)
            if (it->second.p.released && !it->second.p.held && it->second.edges.empty()) it = fingers.erase(it);
            else ++it;
    }

private:
    struct Edge {
        bool down, hasPos;
        float x, y;
        bool moved = false;
        float mx = 0, my = 0;
    };
    struct Source {
        PointerState p;
        std::deque<Edge> edges;
        bool follow = false;
        float fx = 0, fy = 0;
    };
    struct Pad {
        SDL_Gamepad* gp = nullptr;
        bool trigger = false;
        bool used = false;
    };

    static void pushEdge(Source& s, bool down, bool hasPos = false, float x = 0, float y = 0) {
        if (s.edges.size() >= 8) s.edges.erase(s.edges.begin(), s.edges.begin() + 2); // no backlog
        s.edges.push_back({down, hasPos, x, y});
    }
    static void moveTo(Source& s, float x, float y) {
        if (!s.edges.empty()) {
            Edge& b = s.edges.back();
            b.moved = true;
            b.mx = x;
            b.my = y;
        } else {
            s.p.x = x;
            s.p.y = y;
            s.follow = false;
        }
    }
    static void applyEdge(Source& s) {
        if (s.follow) {
            s.p.x = s.fx;
            s.p.y = s.fy;
            s.follow = false;
        }
        if (s.edges.empty()) return;
        Edge ed = s.edges.front();
        s.edges.pop_front();
        if (ed.hasPos) {
            s.p.x = ed.x;
            s.p.y = ed.y;
        }
        if (ed.moved) {
            s.follow = true;
            s.fx = ed.mx;
            s.fy = ed.my;
        }
        if (ed.down) s.p.held = s.p.pressed = true;
        else {
            s.p.held = false;
            s.p.released = true;
        }
    }
    static bool isFinger(const SDL_TouchFingerEvent& f) {
        // pens and mice already arrive as mouse events (pen/mouse touch emulation would double them)
        return f.touchID != SDL_PEN_TOUCHID && f.touchID != SDL_MOUSE_TOUCHID;
    }

    int padSlot(SDL_JoystickID id) const {
        for (int i = 0; i < 2; i++)
            if (pads[i].gp && SDL_GetGamepadID(pads[i].gp) == id) return i;
        return -1;
    }
    Source& padSource(int k) { return k == 0 ? primary : second; }
    void openPads(int ww, int wh);

    Source primary, second;
    Pad pads[2]; // pad 0: primary pointer, pad 1: second player's pointer
    bool primaryFingerDown = false;
    SDL_FingerID primaryFinger = 0;
    std::map<SDL_FingerID, Source> fingers; // further fingers
    int seenRun = -1;
};

// fill free slots from the connected gamepads
void Input::openPads(int ww, int wh) {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) {
        if (padSlot(ids[i]) >= 0) continue;
        for (int k = 0; k < 2; k++)
            if (!pads[k].gp) {
                if (SDL_Gamepad* g = SDL_OpenGamepad(ids[i])) {
                    pads[k] = Pad{g};
                    if (k == 1) { // two pads: one player per half (Level::handleInput splits taps at the centre)
                        second = Source();
                        second.p.x = ww * 0.75f;
                        second.p.y = wh * 0.5f;
                        primary.p.x = ww * 0.25f;
                        primary.p.y = wh * 0.5f;
                    }
                }
                break;
            }
    }
    SDL_free(ids);
}

void Input::handleEvent(const SDL_Event& e, GameApp& app, SDL_Window* win, int ww, int wh, bool& running) {
    switch (e.type) {
    case SDL_EVENT_QUIT: running = false; break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) break;
        if (e.key.key == SDLK_ESCAPE) app.backPressed();
        if (e.key.key == SDLK_SPACE) {
            pushEdge(primary, true);
            app.showCrosshair = true;
        }
        if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
            bool fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0;
            SDL_SetWindowFullscreen(win, !fs);
        }
        break;
    case SDL_EVENT_KEY_UP:
        if (e.key.key == SDLK_SPACE) pushEdge(primary, false);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button == SDL_BUTTON_LEFT && e.button.which != SDL_TOUCH_MOUSEID)
            pushEdge(primary, true, true, e.button.x, e.button.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT && e.button.which != SDL_TOUCH_MOUSEID) pushEdge(primary, false);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.which != SDL_TOUCH_MOUSEID) {
            moveTo(primary, e.motion.x, e.motion.y);
            app.showCrosshair = false;
        }
        break;
    case SDL_EVENT_FINGER_DOWN: {
        if (!isFinger(e.tfinger)) break;
        float fx = e.tfinger.x * ww, fy = e.tfinger.y * wh;
        if (!primaryFingerDown) {
            primaryFingerDown = true;
            primaryFinger = e.tfinger.fingerID;
            pushEdge(primary, true, true, fx, fy);
            app.showCrosshair = false;
        } else {
            Source& s = fingers[e.tfinger.fingerID];
            s.p.x = fx;
            s.p.y = fy;
            pushEdge(s, true, true, fx, fy);
        }
        break;
    }
    case SDL_EVENT_FINGER_MOTION: {
        if (!isFinger(e.tfinger)) break;
        float fx = e.tfinger.x * ww, fy = e.tfinger.y * wh;
        if (primaryFingerDown && e.tfinger.fingerID == primaryFinger) moveTo(primary, fx, fy);
        else {
            auto it = fingers.find(e.tfinger.fingerID);
            if (it != fingers.end()) moveTo(it->second, fx, fy);
        }
        break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (!isFinger(e.tfinger)) break;
        if (primaryFingerDown && e.tfinger.fingerID == primaryFinger) {
            primaryFingerDown = false;
            pushEdge(primary, false);
        } else {
            auto it = fingers.find(e.tfinger.fingerID);
            if (it != fingers.end()) pushEdge(it->second, false);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: app.setFocus(false); break;
    case SDL_EVENT_GAMEPAD_ADDED: openPads(ww, wh); break;
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int k = padSlot(e.gdevice.which);
        if (k < 0) break;
        Source& s = padSource(k);
        if (s.p.held || (!s.edges.empty() && s.edges.back().down)) pushEdge(s, false); // let go of a held throw
        SDL_CloseGamepad(pads[k].gp);
        pads[k] = Pad();
        if (k == 0 && pads[1].gp) { // the remaining pad drives the UI
            pads[0] = pads[1];
            pads[1] = Pad();
        }
        openPads(ww, wh);
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        int k = padSlot(e.gbutton.which);
        if (k < 0) break;
        const PadRole role = padRole(pads[k].gp, e.gbutton.button);
        if (role == kPadThrow) {
            pushEdge(padSource(k), true);
            pads[k].used = true;
            if (k == 0) app.showCrosshair = true;
        } else if (role == kPadBack) app.backPressed();
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START && app.state() == GameApp::State::Play)
            app.backPressed(); // pause (never quits from the menu)
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int k = padSlot(e.gbutton.which);
        if (k >= 0 && padRole(pads[k].gp, e.gbutton.button) == kPadThrow) pushEdge(padSource(k), false);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        int k = padSlot(e.gaxis.which);
        if (k < 0 || e.gaxis.axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) break;
        if (e.gaxis.value > 16000 && !pads[k].trigger) {
            pads[k].trigger = true;
            pads[k].used = true;
            pushEdge(padSource(k), true);
            if (k == 0) app.showCrosshair = true;
        } else if (e.gaxis.value < 8000 && pads[k].trigger) {
            pads[k].trigger = false;
            pushEdge(padSource(k), false);
        }
        break;
    }
    default: break;
    }
}

// Gamepad sticks move their pointer (right stick aims, left stick works too): about 0.9 view heights per second at
// full tilt, whatever the frame rate.
// With two pads in a versus / co-op run, each starts in its own half when a run starts and, while playing, its stick
// keeps it there, so each pad always throws for its own player.
void Input::updatePads(GameApp& app, int ww, int wh, double dt) {
    const bool twoPads = pads[0].gp && pads[1].gp && app.level() && app.level()->active &&
                         (app.level()->player.mode == kModeVersus || app.level()->player.mode == kModeCoop);
    if (app.runSerial != seenRun && app.state() == GameApp::State::Play) {
        seenRun = app.runSerial;
        if (twoPads) {
            primary.p.x = ww * 0.25f;
            primary.p.y = wh * 0.5f;
            second.p.x = ww * 0.75f;
            second.p.y = wh * 0.5f;
        }
    }
    const bool splitPads = twoPads && app.state() == GameApp::State::Play && app.getProperty("game.paused") == "0";
    for (int k = 0; k < 2; k++) {
        if (!pads[k].gp) continue;
        float ax = 0, ay = 0;
        for (auto pr : {std::make_pair(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY),
                        std::make_pair(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY)}) {
            float x = SDL_GetGamepadAxis(pads[k].gp, pr.first) / 32767.0f,
                  y = SDL_GetGamepadAxis(pads[k].gp, pr.second) / 32767.0f;
            if (std::fabs(x) > 0.2f) ax += x;
            if (std::fabs(y) > 0.2f) ay += y;
        }
        PointerState& q = padSource(k).p;
        float lo = splitPads && k == 1 ? ww * 0.5f : 0.0f, hi = splitPads && k == 0 ? ww * 0.5f - 1.0f : (float)ww;
        // back in its own half after a pause / popup / resize, before its next throw; the primary pointer only while
        // a pad or the keyboard drives it (mouse and touch carry their own position)
        if (splitPads && (k == 1 || app.showCrosshair)) q.x = std::min(std::max(q.x, lo), hi);
        if (ax == 0 && ay == 0) continue;
        float view = std::min((float)wh, ww * 0.75f); // height of the 4:3-or-wider game band
        float step = 0.9f * view * (float)std::min(dt, 0.1);
        q.x = std::min(std::max(q.x + ax * step, lo), hi);
        q.y = std::min(std::max(q.y + ay * step, 0.0f), (float)wh);
        pads[k].used = true;
        if (k == 0) app.showCrosshair = true;
    }
}

// ---------------------------------------------------------------- scripted input for captures

// --touch: real finger events, so they take the same path as a touchscreen
void pushScriptedTouches(const Args& a, SDL_Window* win, int frame) {
    for (size_t i = 0; i < a.touches.size(); i++) {
        const Args::Click& c = a.touches[i];
        if (frame != c.frame && frame != c.frame + 5) continue;
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        SDL_Event fe{};
        fe.type = frame == c.frame ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP;
        fe.tfinger.touchID = 1;
        fe.tfinger.fingerID = (SDL_FingerID)(i + 1);
        fe.tfinger.x = pw > 0 ? c.x / pw : 0;
        fe.tfinger.y = ph > 0 ? c.y / ph : 0;
        fe.tfinger.pressure = 1;
        SDL_PushEvent(&fe);
    }
}

// --bot taps across the screen while it has balls to spare; --click holds the primary pointer for 5 frames
void applyScriptedClicks(const Args& a, GameApp& app, int frame, int pw, int ph, PointerState& p) {
    if (a.bot && app.level() && app.level()->active && frame % 24 == 0 && app.level()->player.balls > 8) {
        p.pressed = p.held = true;
        p.x = pw * (0.3f + 0.4f * ((frame / 12) % 5) / 4.0f);
        p.y = ph * 0.55f;
    }
    for (auto& c : a.clicks) {
        if (frame == c.frame) {
            p.x = c.x;
            p.y = c.y;
            p.pressed = p.held = true;
        } else if (frame > c.frame && frame <= c.frame + 4) {
            p.x = c.x;
            p.y = c.y;
            p.held = true;
        } else if (frame == c.frame + 5) {
            p.x = c.x;
            p.y = c.y;
            p.released = true;
            p.held = false;
        }
    }
}

// --cmd: a command with a frame number runs on that frame, the others one every 30 frames from frame 150
class ScriptedCommands {
public:
    void run(const Args& a, GameApp& app, int frame) {
        for (auto& c : a.commands)
            if (c.first == frame) app.command(c.second);
        if (next_ < a.commands.size() && a.commands[next_].first < 0 && frame >= 150 && (++counter_ % 30) == 0)
            app.command(a.commands[next_++].second);
        else if (next_ < a.commands.size() && a.commands[next_].first >= 0) next_++;
    }

private:
    size_t next_ = 0;
    int counter_ = 0;
};

// ---------------------------------------------------------------- main loop

void runLoop(const Args& a, GameApp& app, SDL_Window* win) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); // touches are handled as fingers
    SDL_SetHint(SDL_HINT_PEN_TOUCH_EVENTS, "0");   // a pen arrives as mouse events (SDL_PEN_MOUSEID)
    Input input(a.width * 0.5f, a.height * 0.5f);
    ScriptedCommands commands;
    const bool deterministic = a.deterministic();
    const auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    bool running = true;
    int frames = 0;
    while (running && !app.quitRequested()) {
        int pw, ph, ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        input.beginFrame();
        pushScriptedTouches(a, win, frames);
        SDL_Event e;
        while (SDL_PollEvent(&e)) input.handleEvent(e, app, win, ww, wh, running);
        input.applyEdges();

        // a minimized window's buffer swap does not wait for vsync on every platform: keep to 60 Hz
        // (the game keeps running, so a run's focus-loss pause and its audio pause still complete)
        if (!deterministic &&
            (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED))) {
            double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count();
            if (since < 1.0 / 60) SDL_DelayNS((Uint64)((1.0 / 60 - since) * 1e9));
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        const double stepDt = deterministic ? 1.0 / 60 : dt;

        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        SDL_GetWindowSize(win, &ww, &wh);
        float sx = ww > 0 ? (float)pw / ww : 1, sy = wh > 0 ? (float)ph / wh : 1;
        input.updatePads(app, ww, wh, stepDt);
        std::vector<PointerState> extra;
        PointerState p = input.pointers(app, sx, sy, extra);
        applyScriptedClicks(a, app, frames, pw, ph, p);
        if (deterministic) commands.run(a, app, frames);
        if (a.god && app.level() && app.level()->active) {
            app.level()->player.balls = std::max(app.level()->player.balls, 50);
            app.level()->immuneUntil = 1e9f;
        }
        app.frame(stepDt, p, pw, ph, deterministic, extra.empty() ? nullptr : &extra);
        input.endFrame();
        SDL_GL_SwapWindow(win);
        frames++;

        bool timeUp = a.runSeconds > 0 &&
                      std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() >= a.runSeconds;
        if (timeUp && a.screenshot.empty()) break;
        if ((deterministic && frames >= a.ticks) || (timeUp && !a.screenshot.empty())) {
            saveScreenshot(a.screenshot, pw, ph);
            printf("wrote %s (frame %d, music clock %.2f)\n", a.screenshot.c_str(), frames,
                   app.level() ? app.level()->musicT : 0.0f);
            break;
        }
    }
}

GameApp::Options gameOptions(const Args& a, SDL_Window* win) {
    GameApp::Options opt;
    opt.userDir = userDataDir(a);
    opt.startLevel = a.level;
    opt.startMode = a.mode;
    opt.unlockAll = a.unlockAll;
    opt.quality = a.quality;
    opt.openUrls = !g_noDialogs; // captures never launch a browser
    // seeded from the clock once per launch like the original (room layouts, obstacle offsets, Lua math.random).
    // Captures and tests keep the fixed seeds.
    if (a.screenshot.empty() || a.runSeconds > 0)
        opt.randomSeed = ((uint32_t)time(nullptr) ^ std::random_device{}()) | 1u;
    opt.qualityExplicit = a.qualityGiven;
    opt.modeExplicit = a.modeGiven;
    SDL_SyncWindow(win); // a --fullscreen size request has been applied
    SDL_GetWindowSizeInPixels(win, &opt.width, &opt.height);
    return opt;
}

// Renderer, UI and game on an open window. Returns the exit code.
int runGame(const Args& a, Vfs& vfs, SDL_Window* win, const std::string& rememberFile) {
    std::string err;
    GameRenderer renderer;
    if (!renderer.init(vfs, &err)) {
        fatal(win, "Renderer initialisation failed: " + err);
        renderer.shutdown();
        return 1;
    }
    renderer.setQuality(a.quality);
    int exitCode = 0;
    {
        UiRenderer ui(renderer);
        if (!ui.init(&err)) {
            fatal(win, "UI initialisation failed: " + err);
            exitCode = 1;
        } else {
            GameApp app(vfs, renderer, ui, gameOptions(a, win));
            // audio exists before init: a --level start plays its first room's music during init
            std::unique_ptr<AudioSystem> audio;
            if (!a.noAudio) {
                audio.reset(createSdlAudio(vfs));
                if (!audio) fprintf(stderr, "audio unavailable, continuing silently\n");
                else app.setAudio(audio.get());
            }
            if (!app.init(&err)) {
                fatal(win, "Game initialisation failed: " + err);
                exitCode = 1;
                app.setAudio(nullptr);
            } else {
                // remember where the assets were found, now that the renderer, UI and game all started with them
                rememberAssets(rememberFile, a.assets);
                runLoop(a, app, win);
                app.shutdown();
                app.setAudio(nullptr);
                audio.reset();
            }
        }
        ui.shutdown();
    }
    renderer.shutdown();
    return exitCode;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    sh_attach_parent_console();
#endif
    Args a;
    if (int code = parseArgs(argc, argv, a); code >= 0) return code;
    g_noDialogs = a.offscreen || a.checkShaders || !a.screenshot.empty();
    const std::string userDirWarning = prepareUserDir(a);
#ifdef _WIN32
    if (!g_noDialogs) sh_detach_parent_console(); // interactive session: closing the parent console must not kill us
#endif
    if (!userDirWarning.empty() && !g_noDialogs) // launches without a console (Explorer, shortcuts, Finder)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "OpenSmash", userDirWarning.c_str(), nullptr);

    if (a.offscreen) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!a.forceAudio) a.noAudio = true;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fatal(nullptr, std::string("SDL_Init failed: ") + SDL_GetError());
        return 1;
    }
    Vfs vfs;
    std::string rememberFile; // written only once the game has started with these assets
    if (int code = mountAssets(a, vfs, rememberFile); code >= 0) return code;

    SDL_Window* win = nullptr;
    SDL_GLContext ctx = nullptr;
    if (!createWindow(a, win, ctx)) return 1;
    int exitCode = a.checkShaders ? checkShaders(vfs) : runGame(a, vfs, win, rememberFile);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return exitCode;
}
