// Small GL helpers: textures, dynamic vertex buffers, FBOs.
#pragma once
#include <string>
#include <vector>
#include "assets/image.h"
#include "core/vfs.h"
#include "render/gl.h"
#include "render/shader.h"

namespace sh {

struct Texture {
    GLuint id = 0;
    int w = 0, h = 0;               // as uploaded
    int logicalW = 0, logicalH = 0; // .mtx textures report 2x their stored size
    bool valid() const { return id != 0; }
};

Texture loadTexture(const Vfs& vfs, const std::string& name, bool repeat, std::string* err = nullptr);
Texture makeTexture(const Image& im, bool repeat);
Texture makeBlobTexture();
void destroyTexture(Texture& t);

enum class VLayout { Dyn36, Mesh24, Sprite28, Pos2Tex2, Pos3, Pos3Tex2, Pos3Col16, Ball32, Shadow24 };

// A vertex buffer (+ optional index buffer) that is refilled every frame.
class DynMesh {
public:
    ~DynMesh();
    void init(VLayout layout);
    void upload(const void* verts, size_t vertBytes, const uint32_t* idx, size_t indexCount);
    void draw(GLenum mode = GL_TRIANGLES) const;
    void draw(GLenum mode, GLsizei count, size_t firstIndex) const;
    void drawRange(GLenum mode, size_t firstVertex, size_t vertexCount) const; // non-indexed
    bool empty() const { return count_ == 0; }
    void destroy();

private:
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    size_t count_ = 0;
    bool indexed_ = false;
    size_t stride_ = 0; // bytes per vertex, recorded by init() from the attribute layout
};

struct Fbo {
    GLuint fbo = 0, tex = 0, rb = 0;
    int w = 0, h = 0;
    bool create(int width, int height, bool depthStencil);
    void destroy();
    void bind() const {
        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl::Viewport(0, 0, w, h);
    }
};

} // namespace sh
