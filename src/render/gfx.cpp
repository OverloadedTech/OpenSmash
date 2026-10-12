#include "render/gfx.h"
#include <cmath>

namespace sh {

Texture makeTexture(const Image& im, bool repeat) {
    Texture t;
    gl::GenTextures(1, &t.id);
    gl::BindTexture(GL_TEXTURE_2D, t.id);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.w, im.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, im.rgba.data());
    gl::GenerateMipmap(GL_TEXTURE_2D);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    t.w = t.logicalW = im.w;
    t.h = t.logicalH = im.h;
    return t;
}

Texture loadTexture(const Vfs& vfs, const std::string& name, bool repeat, std::string* err) {
    Image im;
    if (!decodeImage(vfs, name, im, err)) return Texture();
    Texture t = makeTexture(im, repeat);
    if (!vfs.exists(name) && vfs.exists(name + ".mtx")) { // .mtx textures ship at half size
        t.logicalW = t.w * 2;
        t.logicalH = t.h * 2;
    }
    return t;
}

Texture makeBlobTexture() {
    Image im;
    im.w = im.h = 128;
    im.rgba.assign(128 * 128 * 4, 255);
    for (int j = 0; j < 128; j++)
        for (int i = 0; i < 128; i++) {
            float dx = (i - 64) / 64.0f, dy = (j - 64) / 64.0f, d = std::sqrt(dx * dx + dy * dy);
            float v = d <= 1 ? std::pow(1 - d * d, 3.0f) * 1.5666814f * 0.5f : 0;
            uint8_t a = (uint8_t)(std::min(std::max(v, 0.0f), 1.0f) * 255);
            uint8_t* p = &im.rgba[(j * 128 + i) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = a;
        }
    return makeTexture(im, false);
}

void destroyTexture(Texture& t) {
    if (t.id && gl::DeleteTextures) gl::DeleteTextures(1, &t.id);
    t = Texture();
}

void DynMesh::init(VLayout layout) {
    gl::GenVertexArrays(1, &vao_);
    gl::GenBuffers(1, &vbo_);
    gl::GenBuffers(1, &ebo_);
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    auto attr = [&](int loc, int n, GLenum type, bool norm, int stride, int off) {
        stride_ = (size_t)stride;
        gl::EnableVertexAttribArray(loc);
        gl::VertexAttribPointer(loc, n, type, norm ? GL_TRUE : GL_FALSE, stride, (const void*)(intptr_t)off);
    };
    switch (layout) {
    case VLayout::Dyn36:
        attr(ATTR_POSITION, 3, GL_FLOAT, false, 36, 0);
        attr(ATTR_NORMAL, 3, GL_FLOAT, false, 36, 12);
        attr(ATTR_TEXCOORD, 2, GL_FLOAT, false, 36, 24);
        attr(ATTR_COLOR, 4, GL_UNSIGNED_BYTE, true, 36, 32);
        break;
    case VLayout::Mesh24:
        attr(ATTR_POSITION, 3, GL_FLOAT, false, 24, 0);
        attr(ATTR_TEXCOORD, 2, GL_FLOAT, false, 24, 12);
        attr(ATTR_COLOR, 4, GL_UNSIGNED_BYTE, true, 24, 20);
        break;
    case VLayout::Sprite28:
        attr(ATTR_POSITION, 3, GL_FLOAT, false, 28, 0);
        attr(ATTR_TEXCOORD, 2, GL_FLOAT, false, 28, 12);
        attr(ATTR_COLOR, 4, GL_UNSIGNED_BYTE, true, 28, 20);
        attr(ATTR_ALPHA, 1, GL_FLOAT, false, 28, 24);
        break;
    case VLayout::Pos2Tex2:
        attr(ATTR_POSITION, 2, GL_FLOAT, false, 16, 0);
        attr(ATTR_TEXCOORD, 2, GL_FLOAT, false, 16, 8);
        break;
    case VLayout::Pos3: attr(ATTR_POSITION, 3, GL_FLOAT, false, 12, 0); break;
    case VLayout::Pos3Col16:
        attr(ATTR_POSITION, 3, GL_FLOAT, false, 16, 0);
        attr(ATTR_COLOR, 4, GL_UNSIGNED_BYTE, true, 16, 12);
        break;
    case VLayout::Ball32: // pos3f, texcoord 3 normalized ubytes, colour 4f
        attr(ATTR_POSITION, 3, GL_FLOAT, false, 32, 0);
        attr(ATTR_TEXCOORD, 3, GL_UNSIGNED_BYTE, true, 32, 12);
        attr(ATTR_COLOR, 4, GL_FLOAT, false, 32, 16);
        break;
    case VLayout::Shadow24:
        attr(ATTR_POSITION, 3, GL_FLOAT, false, 24, 0);
        attr(ATTR_TEXCOORD, 2, GL_FLOAT, false, 24, 12);
        attr(ATTR_ALPHA, 1, GL_FLOAT, false, 24, 20);
        break;
    case VLayout::Pos3Tex2:
        attr(ATTR_POSITION, 3, GL_FLOAT, false, 20, 0);
        attr(ATTR_TEXCOORD, 2, GL_FLOAT, false, 20, 12);
        break;
    }
    gl::BindVertexArray(0);
}

void DynMesh::upload(const void* verts, size_t vertBytes, const uint32_t* idx, size_t indexCount) {
    gl::BindVertexArray(vao_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl::BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertBytes, verts, GL_STREAM_DRAW);
    indexed_ = idx != nullptr;
    if (indexed_) {
        gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        gl::BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indexCount * 4), idx, GL_STREAM_DRAW);
        count_ = indexCount;
    } else {
        count_ = stride_ ? vertBytes / stride_ : 0;
    }
    gl::BindVertexArray(0);
}

void DynMesh::draw(GLenum mode) const {
    if (!count_) return;
    gl::BindVertexArray(vao_);
    if (indexed_) gl::DrawElements(mode, (GLsizei)count_, GL_UNSIGNED_INT, nullptr);
    else gl::DrawArrays(mode, 0, (GLsizei)count_);
}
void DynMesh::draw(GLenum mode, GLsizei count, size_t firstIndex) const {
    if (!count_) return;
    gl::BindVertexArray(vao_);
    gl::DrawElements(mode, count, GL_UNSIGNED_INT, (const void*)(firstIndex * 4));
}

void DynMesh::drawRange(GLenum mode, size_t first, size_t count) const {
    if (!count_ || !count) return;
    gl::BindVertexArray(vao_);
    gl::DrawArrays(mode, (GLint)first, (GLsizei)count);
}

void DynMesh::destroy() {
    if (vao_) {
        gl::DeleteVertexArrays(1, &vao_);
        gl::DeleteBuffers(1, &vbo_);
        gl::DeleteBuffers(1, &ebo_);
    }
    vao_ = vbo_ = ebo_ = 0;
    count_ = 0;
}
DynMesh::~DynMesh() {
    if (gl::DeleteBuffers) destroy();
}

bool Fbo::create(int width, int height, bool depthStencil) {
    destroy();
    w = std::max(width, 1);
    h = std::max(height, 1);
    gl::GenTextures(1, &tex);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::GenFramebuffers(1, &fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (depthStencil) {
        gl::GenRenderbuffers(1, &rb);
        gl::BindRenderbuffer(GL_RENDERBUFFER, rb);
        gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    }
    bool ok = gl::CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

void Fbo::destroy() {
    if (!gl::DeleteFramebuffers) return;
    if (fbo) gl::DeleteFramebuffers(1, &fbo);
    if (tex) gl::DeleteTextures(1, &tex);
    if (rb) gl::DeleteRenderbuffers(1, &rb);
    fbo = tex = rb = 0;
}

} // namespace sh
