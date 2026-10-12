// GL drawing for the Lua UI layer (images, text meshes, pie/border, full-screen colour, scissor).
#pragma once
#include "render/game_renderer.h"
#include "ui/ui_script.h"

namespace sh {

class UiRenderer {
public:
    explicit UiRenderer(GameRenderer& gr) : gr_(gr) {}
    bool init(std::string* err);
    void shutdown();
    UiTexInfo texture(const std::string& path);
    // Sets the GUI->clip matrix used by the draw calls (2D pixel mode or menu 3D mode).
    void setClip(const Mat4& m, bool depth) {
        clip_ = m;
        depth_ = depth;
    }
    const Mat4& clip() const { return clip_; }
    void drawQuad(int tex, const Mat4& m, const float uv[4], const float rgba[4]);
    void drawMesh(int tex, const Mat4& m, const TextMesh& mesh, const float rgba[4]);
    void drawPie(int tex, const Mat4& m, float frac, const float uv[4], const float rgba[4]);
    void drawBorder(int tex, const Mat4& m, const float uv[4], const float rgba[4]);
    void fullScreenColor(const float rgba[4], bool half, bool rightHalf);
    void scissor(bool on, int x, int y, int w, int h);
    void beginFrame(int w, int h);
    void drawCrosshair(float px, float py, float size); // pixels, used for gamepad/keyboard aiming

private:
    GameRenderer& gr_;
    DynMesh quad_, mesh_, fan_;
    Mat4 clip_;
    bool depth_ = false;
    int w_ = 1, h_ = 1;
    void state();
    void use2dTex(int tex, const Mat4& m, const float rgba[4], float u0, float v0, float su, float sv);
};

} // namespace sh
