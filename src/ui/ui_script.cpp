#include "ui/ui_script.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "core/xml.h"
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#include "script/lua_args.h"

namespace sh {

void UiHost::uiLog(const std::string& s) { fprintf(stderr, "[ui] %s\n", s.c_str()); }

// ===================================================================== animation

namespace {

float bounceTable(float t) {
    static float table[1024];
    static bool init = false;
    if (!init) {
        float x = 0, v = 0;
        for (int k = 0; k < 1024; k++) {
            v = v * 0.98f + (1 - x) * 0.00035f;
            table[k] = x;
            x += v;
        }
        init = true;
    }
    return table[std::min(1023, std::max(0, (int)(t * 1023)))];
}

float curve(int type, float t) {
    switch (type) {
    case 1: return t;
    case 2: return 1 - std::cos(t * kPi / 2);
    case 3: return std::sin(t * kPi / 2);
    case 4: return (1 - std::cos(kPi * t)) / 2;
    case 5: return bounceTable(t);
    }
    return 1;
}

int easeType(const std::string& s) {
    if (s == "linear") return 1;
    if (s == "easein") return 2;
    if (s == "easeout") return 3;
    if (s == "cosine") return 4;
    if (s == "bounce") return 5;
    return 0;
}

struct Anim {
    int n = 1;
    float from[3] = {0, 0, 0}, to[3] = {0, 0, 0};
    float time = 0, duration = 0;
    int type = 0;
    void value(float out[3]) const {
        if (duration == 0 || type == 0) {
            memcpy(out, to, sizeof to);
            return;
        }
        float t = clampf(time / duration, 0, 1), c = curve(type, t);
        for (int i = 0; i < 3; i++) out[i] = from[i] + (to[i] - from[i]) * c;
    }
    void setInstant(const float v[3]) {
        memcpy(to, v, sizeof to);
        memcpy(from, v, sizeof from);
        duration = 0;
        type = 0;
        time = 0;
    }
    void set(const float v[3], const std::string& ease, float dur) {
        float cur[3];
        value(cur);
        memcpy(from, cur, sizeof from);
        memcpy(to, v, sizeof to);
        time = 0;
        duration = dur;
        type = easeType(ease);
    }
};

enum { T_IMAGE = 1, T_UI = 2, T_TEXT = 3, T_SOUND = 4, T_CANVAS = 5, T_UTEXT = 6 };

} // namespace

struct UiScript::Obj {
    int type = 0;
    Anim pos, rot, scale, alpha, color;
    float originX = 0, originY = 0;
    int originMode = 0;
    // image
    UiTexInfo tex;
    float uv[4] = {0, 0, 1, 1};
    // ui
    GuiBox box;
    // text
    BitmapFont* font = nullptr;
    std::shared_ptr<UnicodeFont> ufont;
    TextMesh mesh;
    UiTexInfo fontTex;
    // canvas
    float cw = 0, ch = 0;
    bool enabled = true;
    int movable = 0;
    int win[4] = {0, 0, 0, 0};
    Mat4 parentM;
    Vec2 vel;
    // sound
    int sound = -1;
    float minInterval = 0, timer = 0;
    bool firstPlay = true;
    Obj() {
        pos.n = 2;
        scale.n = 2;
        color.n = 3;
        float one2[3] = {1, 1, 1};
        scale.setInstant(one2);
        alpha.setInstant(one2);
        color.setInstant(one2);
    }
    bool isVisual() const { return type != T_SOUND; }
    void size(float& w, float& h) const {
        w = h = 0;
        switch (type) {
        case T_IMAGE:
            w = (uv[2] - uv[0]) * tex.w;
            h = (uv[3] - uv[1]) * tex.h;
            break;
        case T_UI:
            w = box.cx1 - box.cx0;
            h = box.cy1 - box.cy0;
            break;
        case T_TEXT:
        case T_UTEXT:
            w = mesh.width;
            h = mesh.height;
            break;
        case T_CANVAS: // the size given to mgCreateCanvas
            w = cw;
            h = ch;
            break;
        }
    }
};

// ===================================================================== helpers

namespace {

UiScript* self(lua_State* L) { return (UiScript*)lua_touserdata(L, lua_upvalueindex(1)); }

Mat4 scaleM(float x, float y, float z = 1) { return Mat4::scale(Vec3(x, y, z)); }
Mat4 transM(float x, float y, float z = 0) { return Mat4::translation(Vec3(x, y, z)); }
Mat4 rotZ(float a) {
    Mat4 m;
    float c = std::cos(a), s = std::sin(a);
    m.m[0] = c;
    m.m[1] = s;
    m.m[4] = -s;
    m.m[5] = c;
    return m;
}

} // namespace

// ===================================================================== class

UiScript::UiScript(UiHost* h, UiGlobals* g, const std::string& dir) : host(h), globals(g), scriptDir(dir) {}
UiScript::~UiScript() { unload(); }

std::string UiScript::resolve(const std::string& p) const {
    if (p.rfind("user://", 0) == 0) return p;
    const Vfs& vfs = host->vfs();
    std::string lang = host->locale();
    auto exists = [&](const std::string& n) { return vfs.exists(n) || vfs.exists(n + ".mtx"); };
    if (lang != "en") {
        bool loc = p.find(".png") != std::string::npos || p.find(".jpg") != std::string::npos ||
                   p.find("locale/") != std::string::npos;
        if (loc) {
            size_t sl = p.rfind('/');
            std::string dir = sl == std::string::npos ? "" : p.substr(0, sl + 1),
                        file = sl == std::string::npos ? p : p.substr(sl + 1);
            std::string c = dir + "localize_" + lang + "/" + file;
            if (exists(c)) return c;
            c = scriptDir + "/" + dir + "localize_" + lang + "/" + file;
            if (exists(c)) return c;
        }
    }
    if (exists(p)) return p;
    std::string c = scriptDir + "/" + p;
    if (exists(c)) return c;
    return p;
}

void UiScript::unload() {
    // the modal list is shared by all scenes (menu, HUD): a reloaded or destroyed scene must not leave its modal Uis
    // behind, or every other scene's widgets stop accepting input
    if (globals)
        for (auto& o : objs_)
            if (o && o->type == T_UI) {
                auto& m = globals->modal;
                m.erase(std::remove(m.begin(), m.end(), o->box.uid), m.end());
            }
    objs_.clear();
    lua_.reset();
    delayed.clear();
    canvasStack.clear();
    dragCanvas = -1;
}

void UiScript::call(const char* fn, const char* strArg) {
    if (!lua_) return;
    lua_State* L = lua_->L();
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    int nargs = 0;
    if (strArg) {
        lua_pushstring(L, strArg);
        nargs = 1;
    }
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) {
        host->uiLog(std::string(fn) + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
        lua_pop(L, 1);
    }
}

bool UiScript::load(const std::string& path, const std::vector<std::string>& includes) {
    unload();
    const Vfs& vfs = host->vfs();
    bool ok = false;
    std::string src = vfs.readText(path, &ok);
    if (!ok) {
        host->uiLog("Could not load script " + path);
        return false;
    }
    lua_.reset(new LuaHost);
    locale.load(vfs, host->locale());
    std::string err;
    if (!lua_->run(src, "@" + path, &err)) {
        host->uiLog("Error compiling script " + path + ": " + err);
        lua_.reset();
        return false;
    }
    for (auto& inc : includes) {
        std::string s = vfs.readText(inc, &ok);
        if (!ok) continue;
        if (!lua_->run(s, "@" + inc, &err)) host->uiLog("include " + inc + ": " + err);
    }
    registerApi();
    call("init");
    frameCount = 0;
    scriptTime = 0;
    return true;
}

void UiScript::handleCommand(const std::string& cmd) { call("handleCommand", cmd.c_str()); }

void UiScript::recomputeStack() {
    stackM = Mat4();
    stackColor[0] = stackColor[1] = stackColor[2] = stackColor[3] = 1;
    stackEnabled = true;
    for (int id : canvasStack) {
        Obj& c = *objs_[id];
        float p[3], r[3], s[3], a[3], col[3];
        c.pos.value(p);
        c.rot.value(r);
        c.scale.value(s);
        c.alpha.value(a);
        c.color.value(col);
        Mat4 cm = transM(p[0], p[1]) * rotZ(-r[0]) * scaleM(s[0], s[1]) * transM(-c.originX, -c.originY);
        stackM = stackM * cm;
        for (int i = 0; i < 3; i++) stackColor[i] *= col[i];
        stackColor[3] *= a[0];
        stackEnabled = stackEnabled && c.enabled;
    }
}

// A script error abandons the call mid-way (possibly between mgPushCanvas/mgPopCanvas); never let that leak into
// later calls: every draw entry point starts with an empty canvas stack.
void UiScript::callDraw(const char* fn) {
    auto dropCanvasStack = [&] {
        if (canvasStack.empty()) return false;
        canvasStack.clear();
        host->scissor(false, 0, 0, 0, 0);
        return true;
    };
    dropCanvasStack();
    recomputeStack();
    call(fn);
    if (dropCanvasStack()) recomputeStack();
}
void UiScript::draw() {
    callDraw("draw");
    tickedSinceDraw = false;
}
void UiScript::drawWorld() { callDraw("drawWorld"); }

// ===================================================================== Ui hit testing / drawing

namespace {

struct Pix {
    float x, y;
    bool ok;
};

Pix project(const Mat4& clipM, const Vec2& vp, float x, float y) {
    float cx = clipM.m[0] * x + clipM.m[4] * y + clipM.m[12];
    float cy = clipM.m[1] * x + clipM.m[5] * y + clipM.m[13];
    float cw = clipM.m[3] * x + clipM.m[7] * y + clipM.m[15];
    if (std::fabs(cw) < 1e-6f) return {0, 0, false};
    float nx = cx / cw, ny = cy / cw;
    return {(nx * 0.5f + 0.5f) * vp.x, (1 - (ny * 0.5f + 0.5f)) * vp.y, true};
}

bool inQuad(const Pix q[4], float px, float py) {
    // convex quad point test (either winding)
    int pos = 0, neg = 0;
    for (int i = 0; i < 4; i++) {
        const Pix &a = q[i], &b = q[(i + 1) % 4];
        float cr = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
        if (cr > 0) pos++;
        else if (cr < 0) neg++;
    }
    return pos == 0 || neg == 0;
}

float segDist(float px, float py, const Pix& a, const Pix& b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? clampf(((px - a.x) * dx + (py - a.y) * dy) / l2, 0, 1) : 0;
    float qx = a.x + dx * t - px, qy = a.y + dy * t - py;
    return std::sqrt(qx * qx + qy * qy);
}

} // namespace

// Draws a Ui object and processes pointer input as a side effect.
static void renderUi(UiScript& S, UiScript::Obj& o, const Mat4& M, const float color[4], bool stackEnabled) {
    UiHost& H = *S.host;
    UiGlobals& G = *S.globals;
    GuiBox& b = o.box;
    float w = b.cx1 - b.cx0, h = b.cy1 - b.cy0;
    if (w <= 0 || h <= 0) return;
    float uv[4] = {b.cx0 / std::max(1, b.tex.w), b.cy0 / std::max(1, b.tex.h), b.cx1 / std::max(1, b.tex.w),
                   b.cy1 / std::max(1, b.tex.h)};
    H.drawQuad(b.tex.handle, M, uv, color);
    // a selection waits for the next tick: above 60 fps a drawn frame may run no tick (the original always ticks
    // between two renders)
    if (S.tickedSinceDraw) b.selection.clear();
    Vec2 vp = H.viewport();
    Mat4 clip = H.guiToClip() * M; // unit quad -> clip
    bool modalOk = G.modal.empty() || std::find(G.modal.begin(), G.modal.end(), b.uid) != G.modal.end();
    bool accepts = b.enabled && modalOk && stackEnabled;
    bool held = H.touchHeld(0), pressedNow = H.touchPressed(0), released = H.touchReleased(0);
    Vec2 tp = H.touchPos(0);
    // widget rects get a 45 GUI unit touch margin (the original uses 90 on phones), converted to texture pixels per
    // axis so the projection stays correct for the 3D level buttons
    float gx = std::sqrt(M.m[0] * M.m[0] + M.m[1] * M.m[1] + M.m[2] * M.m[2]) / w; // GUI units per texture px along x
    float gy = std::sqrt(M.m[4] * M.m[4] + M.m[5] * M.m[5] + M.m[6] * M.m[6]) / h;
    const float mx = gx > 1e-6f ? 45.0f / gx : 0.0f, my = gy > 1e-6f ? 45.0f / gy : 0.0f;
    for (GuiRect& r : b.rects) {
        if (!r.enabled) continue;
        // rect in the unit-quad space of this Ui
        auto corner = [&](float lx, float ly) { return project(clip, vp, (lx - b.cx0) / w, (ly - b.cy0) / h); };
        Pix q[4] = {corner(r.x0 - mx, r.y0 - my), corner(r.x1 + mx, r.y0 - my), corner(r.x1 + mx, r.y1 + my),
                    corner(r.x0 - mx, r.y1 + my)};
        Pix q0[4] = {corner(r.x0, r.y0), corner(r.x1, r.y0), corner(r.x1, r.y1), corner(r.x0, r.y1)};
        bool hit = (held || released) && q[0].ok && q[1].ok && q[2].ok && q[3].ok && inQuad(q, tp.x, tp.y);
        if (accepts && hit && pressedNow) {
            float d = 1e30f;
            for (int i = 0; i < 4; i++) d = std::min(d, segDist(tp.x, tp.y, q0[i], q0[(i + 1) % 4]));
            if (inQuad(q0, tp.x, tp.y)) d = -d;
            if (d < G.bestDist) {
                G.bestDist = d;
                G.candidateId = r.id;
            }
        }
        bool highlight = (G.pressedId == r.id && held && hit) || (r.radioSelected && (b.hasSelTex || b.shade));
        if (highlight) {
            Mat4 sub =
                M * transM((r.x0 - b.cx0) / w, (r.y0 - b.cy0) / h) * scaleM((r.x1 - r.x0) / w, (r.y1 - r.y0) / h);
            if (b.shade) {
                float c2[4] = {0.5f, 0.5f, 0.5f, 1.0f}; // fixed grey, ignores the Ui colour and alpha
                float u[4] = {r.x0 / std::max(1, b.tex.w), r.y0 / std::max(1, b.tex.h), r.x1 / std::max(1, b.tex.w),
                              r.y1 / std::max(1, b.tex.h)};
                H.drawQuad(b.tex.handle, sub, u, c2);
            } else if (b.hasSelTex) {
                if (b.overlayWhole) {
                    float u[4] = {0, 0, 1, 1};
                    H.drawQuad(b.sel.handle, sub, u, color);
                } else {
                    // 'selected' texture, but with UVs from the main texture's size (same as the original game)
                    float u[4] = {r.x0 / std::max(1, b.tex.w), r.y0 / std::max(1, b.tex.h), r.x1 / std::max(1, b.tex.w),
                                  r.y1 / std::max(1, b.tex.h)};
                    H.drawQuad(b.sel.handle, sub, u, color);
                }
            }
        }
        if (accepts && hit && G.pressedId == r.id && released) {
            b.selection = r.cmd;
            if (r.group > 0) {
                for (GuiRect& o2 : b.rects)
                    if (o2.group == r.group) o2.radioSelected = false;
                r.radioSelected = true;
            }
        }
    }
    if (accepts && pressedNow && b.selection.empty() && G.pressedId == -1 && G.candidateId == -1) b.armedOutside = true;
    if (accepts && released && G.pressedId == -1 && b.armedOutside) {
        b.selection = b.outsideCmd;
        b.armedOutside = false;
    }
}

// ===================================================================== tick

void UiScript::dispatchSelection(Obj& o) {
    std::string sel = o.box.selection;
    o.box.selection.clear();
    size_t start = 0;
    while (start <= sel.size()) {
        size_t e = sel.find(';', start);
        std::string part = sel.substr(start, e == std::string::npos ? std::string::npos : e - start);
        size_t a = part.find_first_not_of(" \t\r\n"), z = part.find_last_not_of(" \t\r\n");
        part = a == std::string::npos ? "" : part.substr(a, z - a + 1);
        if (!part.empty()) {
            std::string low = part.substr(0, 7);
            for (char& c : low) c = (char)tolower((unsigned char)c);
            if (low == "script:") handleCommand(part.substr(7));
            else host->command(part);
        }
        if (e == std::string::npos) break;
        start = e + 1;
    }
}

void UiScript::updateCanvas(Obj& c, int index) {
    // drag and inertia for movable canvases
    Vec2 vp = host->viewport();
    float s = vp.y / kGuiHeight;
    float wmin[2] = {(float)c.win[0], (float)c.win[1]}, wmax[2] = {(float)c.win[2], (float)c.win[3]};
    float lo[2] = {wmax[0] - c.cw, wmax[1] - c.ch}, hi[2] = {wmin[0], wmin[1]};
    float cur[3];
    c.pos.value(cur);
    bool pressed = host->touchPressed(0), held = host->touchHeld(0);
    Vec2 tg(host->touchPos(0).x / s, host->touchPos(0).y / s);
    // touch in canvas-parent space
    Mat4 inv = c.parentM; // parentM stores the parent matrix; approximate inverse for translate/scale
    float sx = inv.m[0] != 0 ? inv.m[0] : 1, sy = inv.m[5] != 0 ? inv.m[5] : 1;
    Vec2 tl((tg.x - inv.m[12]) / sx, (tg.y - inv.m[13]) / sy);
    int axes = c.movable;
    bool thisDragged = false;
    if (dragCanvas < 0 && pressed) {
        if (tl.x > wmin[0] && tl.x < wmax[0] && tl.y > wmin[1] && tl.y < wmax[1]) {
            dragCanvas = index;
            dragStart = tl;
            dragStartGui = tg;
            dragStartPos = Vec2(cur[0], cur[1]);
        }
    }
    if (dragCanvas == index) {
        if (held) {
            // the press-cancel test uses the raw GUI delta on both axes, before the parent transform and the axis mask,
            // so a vertical slide on an "x" canvas still cancels a button press
            Vec2 raw(tg.x - dragStartGui.x, tg.y - dragStartGui.y);
            if (raw.x * raw.x + raw.y * raw.y > 900.0f) {
                globals->pressedId = -1;
                for (auto& o : objs_)
                    if (o->type == T_UI) o->box.armedOutside = false;
            }
            Vec2 d(tl.x - dragStart.x, tl.y - dragStart.y);
            if (axes == 1) d.y = 0;
            else if (axes == 2) d.x = 0;
            float target[2] = {dragStartPos.x + d.x, dragStartPos.y + d.y}, nt[2];
            for (int k = 0; k < 2; k++) {
                float cl = clampf(target[k], lo[k], hi[k]);
                float ex = target[k] - cl;
                nt[k] = cl + ex / (1 + 0.01f * std::fabs(ex));
            }
            c.vel.x = c.vel.x * 0.8f + (nt[0] - cur[0]) * 0.2f;
            c.vel.y = c.vel.y * 0.8f + (nt[1] - cur[1]) * 0.2f;
            float v[3] = {nt[0], nt[1], 0};
            c.pos.setInstant(v);
            thisDragged = true;
        } else dragCanvas = -1;
        if (!held) return;
    }
    if (!thisDragged && !pressed) {
        float np[2] = {cur[0] + c.vel.x, cur[1] + c.vel.y};
        c.vel.x *= 0.9f;
        c.vel.y *= 0.9f;
        float v[3] = {np[0], np[1], 0};
        for (int k = 0; k < 2; k++) {
            float cl = clampf(np[k], lo[k], hi[k]);
            v[k] = np[k] * 0.7f + cl * 0.3f;
        }
        c.pos.setInstant(v);
    }
}

void UiScript::tick(float dt, float stepDt) {
    tickedSinceDraw = true;
    scriptZ = 0;
    for (int i = 0; i < 32; i++)
        if (host->touchHeld(i)) {
            Vec2 p = host->touchPos(i);
            float s = host->viewport().y / kGuiHeight;
            lastTouchX = p.x / s;
            lastTouchY = p.y / s;
        }
    if (!lua_) return;
    lua_gc(lua_->L(), LUA_GCSTEP, 1);
    call("frame");
    for (size_t i = 0; i < objs_.size(); i++) {
        Obj& o = *objs_[i];
        if (o.isVisual()) {
            o.pos.time += dt;
            o.rot.time += dt;
            o.scale.time += dt;
            o.alpha.time += dt;
            o.color.time += dt;
        }
        if (o.type == T_UI && !o.box.selection.empty()) {
            dispatchSelection(o);
            break;
        }
        if (o.type == T_CANVAS && o.movable != 0 && o.enabled) updateCanvas(o, (int)i);
    }
    for (size_t i = 0; i < delayed.size();) {
        delayed[i].remaining -= stepDt;
        if (delayed[i].remaining < 0) {
            std::string code = delayed[i].code;
            delayed[i] = delayed.back();
            delayed.pop_back();
            lua_State* L = lua_->L();
            // mode "t": source text only, never precompiled bytecode
            if (luaL_loadbufferx(L, code.data(), code.size(), "=delayed", "t") != LUA_OK ||
                lua_pcall(L, 0, 0, 0) != LUA_OK) {
                host->uiLog("delayed: " + std::string(lua_tostring(L, -1) ? lua_tostring(L, -1) : "?"));
                lua_pop(L, 1);
            }
        } else i++;
    }
    frameCount++;
    scriptTime += stepDt;
}

// ===================================================================== host function table

namespace {

#define HANDLER(name) static int name(lua_State* L)
#define PROLOG             \
    UiScript* S = self(L); \
    LuaArgs a(L);          \
    UiHost* H = S->host;   \
    (void)H;               \
    (void)a

UiScript::Obj* getObj(UiScript* S, int id) {
    if (id < 0 || id >= (int)S->objs_.size()) {
        S->host->uiLog("Script: Object not found: " + std::to_string(id));
        return nullptr;
    }
    return S->objs_[id].get();
}
UiScript::Obj* getVisual(UiScript* S, int id) {
    auto* o = getObj(S, id);
    if (o && !o->isVisual()) {
        S->host->uiLog("Script: Object not visual");
        return nullptr;
    }
    return o;
}
UiScript::Obj* getTyped(UiScript* S, int id, int type) {
    auto* o = getObj(S, id);
    if (o && o->type != type) {
        S->host->uiLog("Script: wrong object type");
        return nullptr;
    }
    return o;
}

int addObj(UiScript* S, std::unique_ptr<UiScript::Obj> o) {
    S->objs_.push_back(std::move(o));
    return (int)S->objs_.size() - 1;
}

// matrix of a visual in GUI space, including the canvas stack (maps the unit quad to GUI units)
Mat4 visualMatrix(UiScript* S, UiScript::Obj& o) {
    float p[3], r[3], s[3];
    o.pos.value(p);
    o.rot.value(r);
    o.scale.value(s);
    float w, h;
    o.size(w, h);
    Mat4 m = transM(p[0], p[1], S->scriptZ) * rotZ(-r[0]) * scaleM(s[0], s[1]) * transM(-o.originX, -o.originY) *
             scaleM(w, h);
    return S->stackM * m;
}

bool isVisibleObj(UiScript* S, UiScript::Obj& o) {
    if (o.type == T_CANVAS || o.type == T_SOUND) return false;
    Mat4 M = visualMatrix(S, o);
    Mat4 clip = S->host->guiToClip() * M;
    Vec2 vp = S->host->viewport();
    float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
    for (int i = 0; i < 4; i++) {
        float x = (i & 1) ? 1.0f : 0.0f, y = (i & 2) ? 1.0f : 0.0f;
        Pix p = project(clip, vp, x, y);
        if (!p.ok) return false;
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    return (maxx - minx) >= 1 && (maxy - miny) >= 1 && maxx > 0 && minx < vp.x && maxy > 0 && miny < vp.y;
}

void effColor(UiScript* S, UiScript::Obj& o, float out[4]) {
    float c[3], a[3];
    o.color.value(c);
    o.alpha.value(a);
    out[0] = c[0] * S->stackColor[0];
    out[1] = c[1] * S->stackColor[1];
    out[2] = c[2] * S->stackColor[2];
    out[3] = a[0] * S->stackColor[3];
}

// ---- creation ----
HANDLER(mgCreateImage) {
    PROLOG;
    auto o = std::make_unique<UiScript::Obj>();
    o->type = T_IMAGE;
    o->tex = H->texture(S->resolve(a.s(0)));
    if (a.n >= 2 && o->tex.w > 0) {
        o->uv[0] = a.f(1) / o->tex.w;
        o->uv[1] = a.f(2) / o->tex.h;
        o->uv[2] = a.f(3) / o->tex.w;
        o->uv[3] = a.f(4) / o->tex.h;
    }
    lua_pushinteger(L, addObj(S, std::move(o)));
    return 1;
}

bool initBox(UiScript* S, GuiBox& b, const std::string& path) {
    const Vfs& vfs = S->host->vfs();
    std::string p = S->resolve(path);
    bool ok = false;
    std::string txt = vfs.readText(p, &ok);
    if (!ok) return false;
    auto doc = parseXml(txt);
    if (!doc) return false;
    const XmlNode* root = doc->child("ui");
    if (!root) return false;
    b.texPath = root->attrOr("texture", "");
    b.tex = S->host->texture(S->resolve(b.texPath));
    std::string sel = root->attrOr("selected", ""), ov = root->attrOr("select_overlay", "");
    b.shade = root->attrOr("shade", "") == "true";
    if (!sel.empty()) {
        b.sel = S->host->texture(S->resolve(sel));
        b.hasSelTex = true;
    } else if (!ov.empty()) {
        b.sel = S->host->texture(S->resolve(ov));
        b.hasSelTex = true;
        b.overlayWhole = true;
    }
    b.cx0 = b.cy0 = 0;
    b.cx1 = (float)b.tex.w;
    b.cy1 = (float)b.tex.h;
    if (const std::string* c = root->attr("coords")) sscanf(c->c_str(), "%f %f %f %f", &b.cx0, &b.cy0, &b.cx1, &b.cy1);
    for (auto& ch : root->children) {
        if (ch->name == "rect") {
            GuiRect r;
            r.id = S->globals->nextRectId++;
            r.name = ch->attrOr("id", "");
            r.cmd = ch->attrOr("cmd", "");
            sscanf(ch->attrOr("coords", "0 0 0 0").c_str(), "%f %f %f %f", &r.x0, &r.y0, &r.x1, &r.y1);
            r.group = atoi(ch->attrOr("group", "0").c_str());
            r.radioSelected = ch->attrOr("selected", "") == "true";
            b.rects.push_back(r);
        } else if (ch->name == "outside") b.outsideCmd = ch->attrOr("cmd", "");
    }
    return true;
}

HANDLER(mgCreateUi) {
    PROLOG;
    auto o = std::make_unique<UiScript::Obj>();
    o->type = T_UI;
    o->box.uid = S->globals->nextUiUid++;
    if (!initBox(S, o->box, a.s(0))) {
        o->box.tex = UiTexInfo();
        o->box.tex.w = o->box.tex.h = 0;
    }
    lua_pushinteger(L, addObj(S, std::move(o)));
    return 1;
}

HANDLER(mgCreateText) {
    PROLOG;
    auto o = std::make_unique<UiScript::Obj>();
    o->type = T_TEXT;
    std::string name = a.s(0);
    int type = a.n >= 2 ? a.i(1) : 0;
    o->fontTex = H->texture("fonts/" + name + ".png");
    std::string key = name + ":" + std::to_string(type);
    BitmapFont& f = S->fontCache[key];
    f.load(H->vfs(), name, type, o->fontTex.w, o->fontTex.h);
    o->font = &f;
    lua_pushinteger(L, addObj(S, std::move(o)));
    return 1;
}

HANDLER(mgCreateUText) {
    PROLOG;
    auto o = std::make_unique<UiScript::Obj>();
    o->type = T_UTEXT;
    std::string name = a.s(0), lang = H->locale();
    auto& slot = S->ufontCache[name];
    if (!slot) {
        slot = std::make_shared<UnicodeFont>();
        slot->load(H->vfs(), name, lang);
        UiTexInfo t = H->texture(slot->pngPath);
        slot->texW = t.w;
        slot->texH = t.h;
    }
    o->ufont = slot;
    o->fontTex = H->texture(slot->pngPath);
    lua_pushinteger(L, addObj(S, std::move(o)));
    return 1;
}

void applyOrigin(UiScript::Obj& o, int mode, float ox, float oy) {
    float w, h;
    o.size(w, h);
    o.originMode = mode;
    switch (mode) {
    case 0:
        o.originX = w / 2;
        o.originY = h / 2;
        break;
    case 1:
        o.originX = 0;
        o.originY = 0;
        break;
    case 2:
        o.originX = w;
        o.originY = 0;
        break;
    case 3:
        o.originX = 0;
        o.originY = h;
        break;
    case 4:
        o.originX = w;
        o.originY = h;
        break;
    case 5:
        o.originX = ox;
        o.originY = oy;
        break;
    case 6:
        o.originX = w / 2;
        o.originY = h;
        break;
    case 7:
        o.originX = w / 2;
        o.originY = 0;
        break;
    }
}

HANDLER(mgSetOrigo) {
    PROLOG;
    UiScript::Obj* o = getVisual(S, a.i(0));
    if (!o) return 0;
    static const char* names[] = {"center",      "topleft", "topright",     "bottomleft",
                                  "bottomright", "pixel",   "bottomcenter", "topcenter"};
    std::string m = a.s(1);
    for (int i = 0; i < 8; i++)
        if (m == names[i]) {
            applyOrigin(*o, i, a.f(2), a.f(3));
            break;
        }
    return 0;
}

HANDLER(mgSetCrop) {
    PROLOG;
    UiScript::Obj* o = getObj(S, a.i(0));
    if (!o) return 0;
    if (o->type == T_IMAGE && a.n >= 2 && o->tex.w > 0) {
        o->uv[0] = a.f(1) / o->tex.w;
        o->uv[1] = a.f(2) / o->tex.h;
        o->uv[2] = a.f(3) / o->tex.w;
        o->uv[3] = a.f(4) / o->tex.h;
    } else if (o->type == T_UI) {
        o->box.cx0 = a.f(1);
        o->box.cy0 = a.f(2);
        o->box.cx1 = a.f(3);
        o->box.cy1 = a.f(4);
    }
    return 0;
}

HANDLER(mgSetImage) {
    PROLOG;
    UiScript::Obj* o = getTyped(S, a.i(0), T_IMAGE);
    if (!o) return 0;
    o->tex = H->texture(S->resolve(a.s(1)));
    if (a.n >= 3 && o->tex.w > 0) {
        o->uv[0] = a.f(2) / o->tex.w;
        o->uv[1] = a.f(3) / o->tex.h;
        o->uv[2] = a.f(4) / o->tex.w;
        o->uv[3] = a.f(5) / o->tex.h;
    } else {
        o->uv[0] = o->uv[1] = 0;
        o->uv[2] = o->uv[3] = 1;
    }
    return 0;
}

HANDLER(mgRadioSelect) {
    PROLOG;
    UiScript::Obj* o = getTyped(S, a.i(0), T_UI);
    if (!o) return 0;
    std::string id = a.s(1);
    for (GuiRect& r : o->box.rects)
        if (r.name == id) {
            for (GuiRect& r2 : o->box.rects)
                if (r2.group == r.group) r2.radioSelected = false;
            r.radioSelected = true;
            break;
        }
    return 0;
}
HANDLER(mgSetUiEnabled) {
    PROLOG;
    if (auto* o = getTyped(S, a.i(0), T_UI)) o->box.enabled = a.b(1);
    return 0;
}
HANDLER(mgSetUiModal) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_UI);
    if (!o) return 0;
    auto& m = S->globals->modal;
    auto it = std::find(m.begin(), m.end(), o->box.uid);
    if (a.b(1)) {
        if (it == m.end() && m.size() < 5) m.push_back(o->box.uid);
    } else if (it != m.end()) m.erase(it);
    return 0;
}

HANDLER(mgSetText) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_TEXT);
    if (o && o->font) o->mesh = o->font->build(a.s(1));
    return 0;
}

std::vector<std::pair<std::string, std::string>> namedArgs(LuaArgs& a, int from) {
    std::vector<std::pair<std::string, std::string>> r;
    for (int i = from; i + 1 < a.n; i += 2) r.emplace_back(a.s(i), a.s(i + 1));
    return r;
}

HANDLER(mgSetUText) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_UTEXT);
    if (!o || !o->ufont) return 0;
    std::string key = a.s(1);
    if (key.empty()) return 0;
    std::u32string text = S->locale.translate(key, namedArgs(a, 2));
    float sc[3];
    o->scale.value(sc);
    o->mesh = o->ufont->buildLine(text, 0.0f, o->ufont->baseline() / (sc[1] != 0 ? sc[1] : 1.0f));
    if (o->originMode != 1 && o->originMode != 5) applyOrigin(*o, o->originMode, o->originX, o->originY);
    if (o->originMode == 3) o->originX = 0;
    return 0;
}

HANDLER(mgSetUTextArea) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_UTEXT);
    if (!o || !o->ufont) return 0;
    std::string key = a.s(1);
    if (key.empty()) return 0;
    std::u32string text = S->locale.translate(key, namedArgs(a, 4));
    std::string al = a.s(3);
    int align = al == "center" ? 0 : al == "right" ? 2 : 1;
    o->mesh = o->ufont->buildArea(text, a.f(2), align);
    if (o->originMode != 1 && o->originMode != 5) applyOrigin(*o, o->originMode, o->originX, o->originY);
    return 0;
}

// ---- visual setters/getters ----
HANDLER(mgSetPos) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3] = {a.f(1), a.f(2), 0};
    o->pos.set(v, a.s(3), a.f(4));
    return 0;
}
HANDLER(mgSetRot) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3] = {a.f(1), 0, 0};
    o->rot.set(v, a.s(2), a.f(3));
    return 0;
}
HANDLER(mgSetScale) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float sx = a.f(1), sy = a.n <= 2 ? sx : a.f(2);
    float v[3] = {sx, sy, 0};
    o->scale.set(v, a.s(3), a.f(4));
    return 0;
}
HANDLER(mgSetAlpha) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3] = {a.f(1), 0, 0};
    o->alpha.set(v, a.s(2), a.f(3));
    return 0;
}
HANDLER(mgSetColor) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3] = {a.f(1), a.f(2), a.f(3)};
    o->color.set(v, a.s(4), a.f(5));
    return 0;
}
HANDLER(mgGetPos) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3];
    o->pos.value(v);
    lua_pushnumber(L, v[0]);
    lua_pushnumber(L, v[1]);
    return 2;
}
HANDLER(mgGetRot) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3];
    o->rot.value(v);
    lua_pushnumber(L, v[0]);
    return 1;
}
HANDLER(mgGetScale) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3];
    o->scale.value(v);
    lua_pushnumber(L, v[0]);
    lua_pushnumber(L, v[1]);
    return 2;
}
HANDLER(mgGetAlpha) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3];
    o->alpha.value(v);
    lua_pushnumber(L, v[0]);
    return 1;
}
HANDLER(mgGetColor) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o) return 0;
    float v[3];
    o->color.value(v);
    lua_pushnumber(L, v[0]);
    lua_pushnumber(L, v[1]);
    lua_pushnumber(L, v[2]);
    return 3;
}
HANDLER(mgGetSize) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    float w = 0, h = 0;
    if (o) o->size(w, h);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    return 2;
}
HANDLER(mgIsVisible) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    lua_pushboolean(L, o && isVisibleObj(S, *o));
    return 1;
}

// ---- drawing ----
HANDLER(mgDraw) {
    PROLOG;
    auto* o = getVisual(S, a.i(0));
    if (!o || o->type == T_CANVAS) return 0;
    if (!isVisibleObj(S, *o)) return 0;
    float col[4];
    effColor(S, *o, col);
    float sc[3];
    o->scale.value(sc);
    if (col[3] <= 0 || sc[0] * sc[0] + sc[1] * sc[1] == 0) return 0;
    switch (o->type) {
    case T_IMAGE: H->drawQuad(o->tex.handle, visualMatrix(S, *o), o->uv, col); break;
    case T_UI: renderUi(*S, *o, visualMatrix(S, *o), col, S->stackEnabled); break;
    case T_TEXT:
    case T_UTEXT: {
        float w, h;
        o->size(w, h);
        Mat4 m = visualMatrix(S, *o) * scaleM(w > 0 ? 1.0f / w : 0, h > 0 ? 1.0f / h : 0);
        H->drawMesh(o->fontTex.handle, m, o->mesh, col);
        break;
    }
    }
    return 0;
}
HANDLER(mgDrawPie) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_IMAGE);
    if (!o || !isVisibleObj(S, *o)) return 0;
    float col[4];
    effColor(S, *o, col);
    if (col[3] > 0) H->drawPie(o->tex.handle, visualMatrix(S, *o), clampf(a.f(1), 0, 1), o->uv, col);
    return 0;
}
HANDLER(mgDrawBorder) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_IMAGE);
    if (!o || !isVisibleObj(S, *o)) return 0;
    float col[4];
    effColor(S, *o, col);
    if (col[3] > 0) H->drawBorder(o->tex.handle, visualMatrix(S, *o), o->uv, col);
    return 0;
}
HANDLER(mgFullScreenColor) {
    PROLOG;
    float c[4] = {a.f(0), a.f(1), a.f(2), a.f(3)};
    if (c[3] > 0) H->fullScreenColor(c, a.n >= 5 && a.b(4), H->getProperty("level.pain_side") == "1");
    return 0;
}

// ---- properties / commands / time ----
HANDLER(mgGet) {
    PROLOG;
    lua_pushstring(L, H->getProperty(a.s(0)).c_str());
    return 1;
}
HANDLER(mgSet) {
    PROLOG;
    H->setProperty(a.s(0), a.s(1));
    return 0;
}
HANDLER(mgCommand) {
    PROLOG;
    H->command(a.s(0));
    lua_pushstring(L, "");
    return 1;
}
HANDLER(mgScriptTime) {
    PROLOG;
    lua_pushnumber(L, S->scriptTime);
    return 1;
}
HANDLER(mgScriptFrame) {
    PROLOG;
    lua_pushinteger(L, S->frameCount);
    return 1;
}

// ---- sound ----
HANDLER(mgCreateSound) {
    PROLOG;
    auto o = std::make_unique<UiScript::Obj>();
    o->type = T_SOUND;
    o->sound = H->createSound(S->resolve(a.s(0))); // script-relative names ("camera_pan.ogg" -> menu/...)
    o->minInterval = a.f(1);
    lua_pushinteger(L, addObj(S, std::move(o)));
    return 1;
}
HANDLER(mgPlaySound) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_SOUND);
    if (!o) return 0;
    float vol = a.n >= 2 ? clampf(a.f(1), 0, 1) : 1.0f, pitch = a.n >= 3 ? clampf(a.f(2), 0.01f, 100) : 1.0f;
    float now = S->scriptTime;
    if (o->firstPlay || now - o->timer > o->minInterval) {
        H->playSound(o->sound, vol, pitch);
        o->firstPlay = false;
        o->timer = now;
        if (o->minInterval < 0) o->minInterval = 1e30f;
    }
    return 0;
}

// ---- touch ----
HANDLER(mgIsTouched) {
    PROLOG;
    float x = a.f(0), y = a.f(1), r = a.f(2);
    bool hit = false;
    for (int i = 0; i < 32; i++)
        if (H->touchHeld(i) || H->touchReleased(i)) {
            Vec2 p = H->touchPos(i);
            float dx = p.x * 0.5f - x, dy = p.y * 0.5f - y;
            if (std::sqrt(dx * dx + dy * dy) < r) hit = true;
        }
    lua_pushboolean(L, hit);
    return 1;
}
HANDLER(mgGetLastTouch) {
    PROLOG;
    lua_pushnumber(L, S->lastTouchX);
    lua_pushnumber(L, S->lastTouchY);
    return 2;
}
HANDLER(mgGetScreenCoord) {
    PROLOG;
    float x, y;
    H->worldToGui(Vec3(a.f(0), a.f(1), a.f(2)), x, y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}

// ---- canvases ----
HANDLER(mgCreateCanvas) {
    PROLOG;
    auto o = std::make_unique<UiScript::Obj>();
    o->type = T_CANVAS;
    o->cw = a.f(0);
    o->ch = a.f(1);
    lua_pushinteger(L, addObj(S, std::move(o)));
    return 1;
}
HANDLER(mgPushCanvas) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_CANVAS);
    if (!o) return 0;
    o->parentM = S->stackM;
    S->canvasStack.push_back(a.i(0));
    S->recomputeStack();
    if (o->win[0] || o->win[1] || o->win[2] || o->win[3]) {
        Vec2 vp = H->viewport();
        Mat4 clip = H->guiToClip() * o->parentM;
        Pix p0 = project(clip, vp, (float)o->win[0], (float)o->win[1]),
            p1 = project(clip, vp, (float)o->win[2], (float)o->win[3]);
        // clip matrix maps GUI units; project() expects unit coordinates of the matrix input, which here are GUI units
        float x0 = std::min(p0.x, p1.x), x1 = std::max(p0.x, p1.x), y0 = std::min(p0.y, p1.y),
              y1 = std::max(p0.y, p1.y);
        H->scissor(true, (int)x0, (int)(vp.y - y1), (int)(x1 - x0), (int)(y1 - y0));
    }
    return 0;
}
HANDLER(mgPopCanvas) {
    PROLOG;
    if (S->canvasStack.empty()) return 0;
    UiScript::Obj* o = S->objs_[S->canvasStack.back()].get();
    if (o->win[0] || o->win[1] || o->win[2] || o->win[3]) H->scissor(false, 0, 0, 0, 0);
    S->canvasStack.pop_back();
    S->recomputeStack();
    return 0;
}
HANDLER(mgSetCanvasEnabled) {
    PROLOG;
    if (auto* o = getTyped(S, a.i(0), T_CANVAS)) o->enabled = a.b(1);
    return 0;
}
HANDLER(mgSetCanvasWindow) {
    PROLOG;
    if (auto* o = getTyped(S, a.i(0), T_CANVAS)) {
        for (int i = 0; i < 4; i++) o->win[i] = a.i(i + 1);
    }
    return 0;
}
HANDLER(mgSetCanvasMovable) {
    PROLOG;
    if (auto* o = getTyped(S, a.i(0), T_CANVAS)) {
        std::string m = a.s(1);
        o->movable = m == "x" ? 1 : m == "y" ? 2 : m == "xy" ? 3 : 0;
    }
    return 0;
}
HANDLER(mgSetCanvasSize) {
    PROLOG;
    if (auto* o = getTyped(S, a.i(0), T_CANVAS)) {
        o->cw = a.f(1);
        o->ch = a.f(2);
    }
    return 0;
}

// ---- ui selection ----
HANDLER(mgGetUiSelectionCount) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_UI);
    if (!o) return 0;
    lua_pushinteger(L, (lua_Integer)o->box.rects.size());
    return 1;
}
HANDLER(mgGetUiSelectionInfo) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_UI);
    int i = a.i(1);
    if (!o || i < 0 || i >= (int)o->box.rects.size()) return 0;
    GuiRect& r = o->box.rects[i];
    lua_pushstring(L, r.cmd.c_str());
    lua_pushinteger(L, (lua_Integer)r.x0);
    lua_pushinteger(L, (lua_Integer)r.y0);
    lua_pushinteger(L, (lua_Integer)r.x1);
    lua_pushinteger(L, (lua_Integer)r.y1);
    return 5;
}
HANDLER(mgSetUiSelectionEnabled) {
    PROLOG;
    auto* o = getTyped(S, a.i(0), T_UI);
    if (!o) return 0;
    std::string id = a.s(1);
    for (GuiRect& r : o->box.rects)
        if (r.name == id) r.enabled = a.b(2);
    return 0;
}

// ---- delayed ----
HANDLER(mgRunDelayed) {
    PROLOG;
    if (a.n == 2) S->delayed.push_back({a.s(0), a.f(1)});
    return 0;
}
HANDLER(mgClearRunDelayed) {
    PROLOG;
    S->delayed.clear();
    return 0;
}

// ---- game queries ----
HANDLER(mgGetHighScore) {
    PROLOG;
    if (a.n == 0) {
        lua_pushinteger(L, H->bestDistance());
        return 1;
    }
    int i = a.i(0), score = 0, streak = 0;
    if (i >= 1 && i <= 12) H->highScore(i, score, streak);
    lua_pushinteger(L, score);
    lua_pushinteger(L, streak);
    return 2;
}
HANDLER(mgGetCheckpointPos) {
    PROLOG;
    float x = 0, y = 0;
    H->checkpointPos(a.i(0), x, y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}
HANDLER(mgLatestAvailableCheckpoint) {
    PROLOG;
    lua_pushinteger(L, H->latestCheckpoint());
    return 1;
}
HANDLER(mgGetPowerupFraction) {
    PROLOG;
    lua_pushnumber(L, H->powerupFraction(a.s(0)));
    return 1;
}
HANDLER(mgDrawMenuMesh) {
    PROLOG;
    H->drawMenuMesh();
    return 0;
}
HANDLER(mgDrawMenuGrid) { return 0; }
HANDLER(mgSetZ) {
    PROLOG;
    S->scriptZ = a.f(0);
    return 0;
}
HANDLER(mgGetMultiplierColor) {
    PROLOG;
    float c[3] = {1, 1, 1};
    H->multiplierColor((int)clampf(a.f(0), 0, 9), c);
    lua_pushnumber(L, c[0]);
    lua_pushnumber(L, c[1]);
    lua_pushnumber(L, c[2]);
    return 3;
}
HANDLER(mgCameraShake) {
    PROLOG;
    H->cameraShake(a.f(0));
    return 0;
}
HANDLER(mgSetMode) {
    PROLOG;
    H->setMode(a.i(0));
    return 0;
}
HANDLER(mgIsCheckpointAdLoaded) {
    lua_pushboolean(L, 0);
    return 1;
}
HANDLER(mgIsOutOfBallsAdLoaded) {
    lua_pushboolean(L, 0);
    return 1;
}
HANDLER(mgGetRemoteConfigBoolParameter) {
    PROLOG;
    lua_pushboolean(L, H->remoteBool(a.s(0)));
    return 1;
}
HANDLER(mgGetAspectMultiplier) {
    PROLOG;
    Vec2 v = H->viewport();
    lua_pushnumber(L, (v.x / std::max(v.y, 1.0f)) / (4.0f / 3.0f));
    return 1;
}
HANDLER(mgGetNormalizedAspect) {
    PROLOG;
    Vec2 v = H->viewport();
    float asp = v.x / std::max(v.y, 1.0f);
    lua_pushnumber(L, clampf((asp - 4.0f / 3.0f) / 2.2222222f, 0, 1));
    return 1;
}
HANDLER(mgIsTutorialActive) {
    PROLOG;
    lua_pushboolean(L, H->tutorialActive());
    return 1;
}

struct Reg {
    const char* name;
    lua_CFunction fn;
};
const Reg kApi[] = {
    {"mgCreateImage", mgCreateImage},
    {"mgCreateUi", mgCreateUi},
    {"mgCreateText", mgCreateText},
    {"mgCreateUText", mgCreateUText},
    {"mgSetOrigo", mgSetOrigo},
    {"mgSetCrop", mgSetCrop},
    {"mgSetImage", mgSetImage},
    {"mgRadioSelect", mgRadioSelect},
    {"mgSetUiEnabled", mgSetUiEnabled},
    {"mgSetUiModal", mgSetUiModal},
    {"mgSetText", mgSetText},
    {"mgSetUText", mgSetUText},
    {"mgSetUTextArea", mgSetUTextArea},
    {"mgSetPos", mgSetPos},
    {"mgSetRot", mgSetRot},
    {"mgSetScale", mgSetScale},
    {"mgSetAlpha", mgSetAlpha},
    {"mgSetColor", mgSetColor},
    {"mgGetPos", mgGetPos},
    {"mgGetRot", mgGetRot},
    {"mgGetScale", mgGetScale},
    {"mgGetAlpha", mgGetAlpha},
    {"mgGetColor", mgGetColor},
    {"mgGetSize", mgGetSize},
    {"mgIsVisible", mgIsVisible},
    {"mgDraw", mgDraw},
    {"mgDrawPie", mgDrawPie},
    {"mgDrawBorder", mgDrawBorder},
    {"mgGet", mgGet},
    {"mgSet", mgSet},
    {"mgCommand", mgCommand},
    {"mgScriptTime", mgScriptTime},
    {"mgScriptFrame", mgScriptFrame},
    {"mgFullScreenColor", mgFullScreenColor},
    {"mgCreateSound", mgCreateSound},
    {"mgPlaySound", mgPlaySound},
    {"mgIsTouched", mgIsTouched},
    {"mgGetLastTouch", mgGetLastTouch},
    {"mgGetScreenCoord", mgGetScreenCoord},
    {"mgCreateCanvas", mgCreateCanvas},
    {"mgPushCanvas", mgPushCanvas},
    {"mgPopCanvas", mgPopCanvas},
    {"mgSetCanvasEnabled", mgSetCanvasEnabled},
    {"mgSetCanvasWindow", mgSetCanvasWindow},
    {"mgSetCanvasMovable", mgSetCanvasMovable},
    {"mgSetCanvasSize", mgSetCanvasSize},
    {"mgGetUiSelectionCount", mgGetUiSelectionCount},
    {"mgGetUiSelectionInfo", mgGetUiSelectionInfo},
    {"mgSetUiSelectionEnabled", mgSetUiSelectionEnabled},
    {"mgRunDelayed", mgRunDelayed},
    {"mgClearRunDelayed", mgClearRunDelayed},
    {"mgGetHighScore", mgGetHighScore},
    {"mgGetCheckpointPos", mgGetCheckpointPos},
    {"mgLatestAvailableCheckpoint", mgLatestAvailableCheckpoint},
    {"mgGetPowerupFraction", mgGetPowerupFraction},
    {"mgDrawMenuMesh", mgDrawMenuMesh},
    {"mgDrawMenuGrid", mgDrawMenuGrid},
    {"mgSetZ", mgSetZ},
    {"mgGetMultiplierColor", mgGetMultiplierColor},
    {"mgCameraShake", mgCameraShake},
    {"mgSetMode", mgSetMode},
    {"mgIsCheckpointAdLoaded", mgIsCheckpointAdLoaded},
    {"mgIsOutOfBallsAdLoaded", mgIsOutOfBallsAdLoaded},
    {"mgGetRemoteConfigBoolParameter", mgGetRemoteConfigBoolParameter},
    {"mgGetAspectMultiplier", mgGetAspectMultiplier},
    {"mgGetNormalizedAspect", mgGetNormalizedAspect},
    {"mgIsTutorialActive", mgIsTutorialActive},
};

} // namespace

void UiScript::registerApi() {
    lua_State* L = lua_->L();
    for (const Reg& r : kApi) {
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, r.fn, 1);
        lua_setglobal(L, r.name);
    }
}

void uiEndFrame(UiHost& host, UiGlobals& g) {
    if (g.candidateId != -1) { // every press on a widget rect clicks
        g.pressedId = g.candidateId;
        host.playSound(host.createSound("snd/clickdown0.ogg"), 0.5f, 1.0f);
    }
    if (host.touchReleased(0)) g.pressedId = -1;
    g.candidateId = -1;
    g.bestDist = 1e30f;
    if (!host.touchHeld(0)) g.pressedId = -1;
}

} // namespace sh
