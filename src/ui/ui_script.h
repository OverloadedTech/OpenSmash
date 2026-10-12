// Lua UI scripting layer (menu/HUD/popups): object model + the 67 `mg*` host functions.
// Rendering and game state are reached through UiHost so the same code runs in the game and in headless
// script tests.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "core/math.h"
#include "core/vfs.h"
#include "script/lua_host.h"
#include "ui/fonts.h"

namespace sh {

struct UiTexInfo {
    int handle = -1;    // backend texture id (-1 = placeholder)
    int w = 32, h = 32; // logical size (2x stored size for .mtx)
};

// the menu and HUD are laid out on a canvas this many units tall, whatever the window size
constexpr float kGuiHeight = 1536.0f;

class UiHost {
public:
    virtual ~UiHost() = default;
    virtual const Vfs& vfs() const = 0;
    virtual UiTexInfo texture(const std::string& resolvedPath) = 0;
    virtual Vec2 viewport() const = 0;  // window size in pixels
    virtual Mat4 guiToClip() const = 0; // current GUI->clip matrix (2D or menu-3D)
    virtual void drawQuad(int tex, const Mat4& m, const float uv[4], const float rgba[4]) = 0;
    virtual void drawMesh(int tex, const Mat4& m, const TextMesh& mesh, const float rgba[4]) = 0;
    virtual void drawPie(int tex, const Mat4& m, float frac, const float uv[4], const float rgba[4]) = 0;
    virtual void drawBorder(int tex, const Mat4& m, const float uv[4], const float rgba[4]) = 0;
    virtual void fullScreenColor(const float rgba[4], bool half, bool rightHalf) = 0;
    virtual void scissor(bool on, int x, int y, int w, int h) = 0;
    virtual void drawMenuMesh() {}
    virtual int createSound(const std::string& path) { return -1; }
    virtual void playSound(int h, float vol, float pitch) {}
    // input (pixels); only pointer 0 is used for widgets
    virtual bool touchHeld(int i) const { return false; }
    virtual bool touchPressed(int i) const { return false; }
    virtual bool touchReleased(int i) const { return false; }
    virtual Vec2 touchPos(int i) const { return Vec2(); }
    // game
    virtual std::string getProperty(const std::string& key) = 0;
    virtual void setProperty(const std::string& key, const std::string& value) = 0;
    virtual void command(const std::string& cmd) = 0;
    virtual bool highScore(int checkpoint, int& score, int& streak) {
        score = streak = 0;
        return false;
    }
    virtual int bestDistance() { return 0; }
    virtual int latestCheckpoint() { return 0; }
    virtual float powerupFraction(const std::string& name) { return 0; }
    virtual bool checkpointPos(int i, float& x, float& y) {
        x = y = 0;
        return false;
    }
    virtual bool tutorialActive() { return false; }
    virtual bool remoteBool(const std::string& name) { return false; }
    virtual void multiplierColor(int n, float rgb[3]) { rgb[0] = rgb[1] = rgb[2] = 1; }
    virtual bool worldToGui(const Vec3& p, float& x, float& y) {
        x = y = 0;
        return false;
    }
    virtual std::string locale() const { return "en"; }
    virtual void uiLog(const std::string& s);
    virtual void cameraShake(float a) {}
    virtual void setMode(int m) {}
};

// One shared instance for all scripts, since widget press state spans scenes.
struct UiGlobals {
    int pressedId = -1; // unique id of the pressed rect
    int candidateId = -1;
    float bestDist = 1e30f;
    std::vector<int> modal; // Ui object uids (max 5)
    int nextRectId = 1;
    int nextUiUid = 1;
};

struct GuiRect {
    int id = 0;       // unique
    std::string name; // "id" attribute
    std::string cmd;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int group = 0;
    bool radioSelected = false, enabled = true;
};

struct GuiBox {
    std::string texPath;
    UiTexInfo tex, sel;
    bool shade = false, hasSelTex = false, overlayWhole = false;
    float cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0; // crop rect (texture px)
    std::vector<GuiRect> rects;
    std::string outsideCmd;
    bool enabled = true, armedOutside = false;
    std::string selection; // command string consumed by the next tick
    int uid = 0;
};

class UiScript {
public:
    UiScript(UiHost* host, UiGlobals* globals, const std::string& scriptDir);
    ~UiScript();
    // Loads main script + includes. Returns false if the main script cannot be loaded or compiled.
    bool load(const std::string& path, const std::vector<std::string>& includes);
    void unload();
    bool loaded() const { return lua_ != nullptr; }
    void handleCommand(const std::string& cmd);
    void tick(float dt, float stepDt);
    void draw();
    void drawWorld();
    int objectCount() const { return (int)objs_.size(); }

    // --- called by the host function table (public for the static handlers) ---
    struct Obj;
    UiHost* host;
    UiGlobals* globals;
    std::string scriptDir;
    std::vector<std::unique_ptr<Obj>> objs_;
    LuaHost* lua() { return lua_.get(); }
    void call(const char* fn, const char* strArg = nullptr);
    std::string resolve(const std::string& path) const;
    float scriptZ = 0;
    float lastTouchX = 0, lastTouchY = 0;
    int frameCount = 0;
    float scriptTime = 0;
    bool tickedSinceDraw = true; // a Ui's selection is kept until a tick has dispatched it
    struct Delayed {
        std::string code;
        float remaining;
    };
    std::vector<Delayed> delayed;
    // canvas stack
    std::vector<int> canvasStack;
    Mat4 stackM;
    float stackColor[4] = {1, 1, 1, 1};
    bool stackEnabled = true;
    void recomputeStack();
    LocaleData locale;
    std::map<std::string, BitmapFont> fontCache;
    std::map<std::string, std::shared_ptr<UnicodeFont>> ufontCache;
    int dragCanvas = -1;
    Vec2 dragStart, dragStartGui, dragStartPos; // press in canvas-parent space / raw GUI space (drag-abort test)

private:
    std::unique_ptr<LuaHost> lua_;
    void registerApi();
    void callDraw(const char* fn);
    void dispatchSelection(Obj& o);
    void updateCanvas(Obj& c, int index);
};

// Resolves widget presses. Call once per frame, after every scene has been drawn.
void uiEndFrame(UiHost& host, UiGlobals& globals);

} // namespace sh
