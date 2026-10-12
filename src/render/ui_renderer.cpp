#include "render/ui_renderer.h"
#include <cmath>

namespace sh {

bool UiRenderer::init(std::string* err) {
    quad_.init(VLayout::Pos2Tex2);
    mesh_.init(VLayout::Pos2Tex2);
    fan_.init(VLayout::Pos2Tex2);
    static const float qv[] = {0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 0};
    quad_.upload(qv, sizeof qv, nullptr, 0);
    if (!gr_.shader("2dtex") || !gr_.shader("2d")) {
        if (err) *err = "2d shaders missing";
        return false;
    }
    return true;
}

void UiRenderer::shutdown() {
    quad_.destroy();
    mesh_.destroy();
    fan_.destroy();
}

UiTexInfo UiRenderer::texture(const std::string& path) {
    UiTexInfo t;
    Texture& tx = gr_.tex(path);
    if (!tx.valid()) {
        t.handle = -1;
        return t;
    }
    t.handle = (int)tx.id;
    t.w = tx.logicalW;
    t.h = tx.logicalH;
    return t;
}

void UiRenderer::beginFrame(int w, int h) {
    w_ = w;
    h_ = h;
}

void UiRenderer::state() {
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl::Disable(GL_CULL_FACE);
    // no depth test for script UI, in the 2D and the 3D-menu pass alike, so it composes in script draw order over the
    // menu corridor (which depth-tests itself in drawMenuStrips)
    gl::Disable(GL_DEPTH_TEST);
}

// 2dtex shader with matrix clip_ * m, colour rgba, texture offset (u0, v0) and scale (su, sv), `tex` on unit 0
void UiRenderer::use2dTex(int tex, const Mat4& m, const float rgba[4], float u0, float v0, float su, float sv) {
    Shader* s = gr_.shader("2dtex");
    state();
    s->use();
    Mat4 mvp = clip_ * m;
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, mvp.m);
    gl::Uniform4f(s->uniform("uColor"), rgba[0], rgba[1], rgba[2], rgba[3]);
    gl::Uniform2f(s->uniform("uTexOffset0"), u0, v0);
    gl::Uniform2f(s->uniform("uTexScale0"), su, sv);
    gl::ActiveTexture(GL_TEXTURE0);
    gl::BindTexture(GL_TEXTURE_2D, (GLuint)tex);
    gl::Uniform1i(s->uniform("uTexture0"), 0);
}

void UiRenderer::drawQuad(int tex, const Mat4& m, const float uv[4], const float rgba[4]) {
    if (tex < 0) return;
    use2dTex(tex, m, rgba, uv[0], uv[1], uv[2] - uv[0], uv[3] - uv[1]);
    quad_.draw();
}

void UiRenderer::drawMesh(int tex, const Mat4& m, const TextMesh& mesh, const float rgba[4]) {
    if (tex < 0 || mesh.idx.empty()) return;
    use2dTex(tex, m, rgba, 0, 0, 1, 1);
    mesh_.upload(mesh.verts.data(), mesh.verts.size() * 4, mesh.idx.data(), mesh.idx.size());
    mesh_.draw();
}

void UiRenderer::drawPie(int tex, const Mat4& m, float f, const float uv[4], const float rgba[4]) {
    if (tex < 0 || f <= 0) return;
    int n = (int)(29 * f);
    std::vector<float> v;
    auto push = [&](float x, float y) {
        v.push_back(x);
        v.push_back(y);
        v.push_back(uv[0] + x * (uv[2] - uv[0]));
        v.push_back(uv[1] + y * (uv[3] - uv[1]));
    };
    push(0.5f, 0.5f);
    for (int k = 0; k <= n + 1; k++) {
        float ang = -kPi / 2 + k * (2 * kPi * f) / (n + 1);
        push(0.5f + 0.5f * std::cos(ang), 0.5f + 0.5f * std::sin(ang));
    }
    std::vector<uint32_t> idx;
    for (int k = 1; k <= n + 1; k++) {
        idx.push_back(0);
        idx.push_back(k);
        idx.push_back(k + 1);
    }
    use2dTex(tex, m, rgba, 0, 0, 1, 1);
    fan_.upload(v.data(), v.size() * 4, idx.data(), idx.size());
    fan_.draw();
}

void UiRenderer::drawBorder(int tex, const Mat4& m, const float uv[4], const float rgba[4]) {
    if (tex < 0) return;
    // ring of 4 quads between the unit square and the inner square [0.15,0.85]^2 (UV = position)
    const float a = 0.15f, b = 0.85f;
    float quads[4][4] = {{0, 0, 1, a}, {0, b, 1, 1}, {0, a, a, b}, {b, a, 1, b}};
    std::vector<float> v;
    std::vector<uint32_t> idx;
    for (auto& q : quads) {
        uint32_t base = (uint32_t)v.size() / 4;
        float xs[4] = {q[0], q[0], q[2], q[2]}, ys[4] = {q[1], q[3], q[3], q[1]};
        for (int i = 0; i < 4; i++) {
            v.push_back(xs[i]);
            v.push_back(ys[i]);
            v.push_back(uv[0] + xs[i] * (uv[2] - uv[0]));
            v.push_back(uv[1] + ys[i] * (uv[3] - uv[1]));
        }
        for (uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + k);
    }
    use2dTex(tex, m, rgba, 0, 0, 1, 1);
    fan_.upload(v.data(), v.size() * 4, idx.data(), idx.size());
    fan_.draw();
}

void UiRenderer::fullScreenColor(const float rgba[4], bool half, bool rightHalf) {
    Shader* s = gr_.shader("2d");
    state();
    s->use();
    // NDC rectangle covering the whole (or half) viewport; unit quad is [0,1]^2
    Mat4 m;
    float x0 = -1, w = 2;
    if (half) {
        w = 1;
        x0 = rightHalf ? 0 : -1;
    }
    m = Mat4::translation(Vec3(x0, -1, 0)) * Mat4::scale(Vec3(w, 2, 1));
    gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, m.m);
    gl::Uniform4f(s->uniform("uColor"), rgba[0], rgba[1], rgba[2], rgba[3]);
    quad_.draw();
    // a full-screen colour starts a modal popup (mode dialog, score panel): what follows must draw over the 3D menu
    // corridor/logo already in the depth buffer instead of being depth-tested against it
    gl::Clear(GL_DEPTH_BUFFER_BIT);
}

void UiRenderer::drawCrosshair(float px, float py, float size) {
    Shader* s = gr_.shader("2d");
    state();
    s->use();
    gl::Uniform4f(s->uniform("uColor"), 1, 1, 1, 0.85f);
    auto rect = [&](float x, float y, float w, float h) {
        // pixel rect -> NDC unit-quad matrix
        float nx = 2 * x / w_ - 1, ny = 1 - 2 * (y + h) / h_, sw = 2 * w / w_, sh = 2 * h / h_;
        Mat4 m = Mat4::translation(Vec3(nx, ny, 0)) * Mat4::scale(Vec3(sw, sh, 1));
        gl::UniformMatrix4fv(s->uniform("uMvpMatrix"), 1, GL_FALSE, m.m);
        quad_.draw();
    };
    float t = std::max(2.0f, size * 0.12f);
    rect(px - size, py - t / 2, size * 2, t);
    rect(px - t / 2, py - size, t, size * 2);
}

void UiRenderer::scissor(bool on, int x, int y, int w, int h) {
    if (on) {
        gl::Enable(GL_SCISSOR_TEST);
        gl::Scissor(x, y, w, h);
    } else gl::Disable(GL_SCISSOR_TEST);
}

} // namespace sh
