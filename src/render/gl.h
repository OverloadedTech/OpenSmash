// Minimal OpenGL 3.3 core loader (functions resolved through SDL_GL_GetProcAddress).
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define SH_GLAPI __stdcall
#else
#define SH_GLAPI
#endif

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_DEPTH_BUFFER_BIT 0x100
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 4
#define GL_LINES 1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DEPTH_TEST 0xB71
#define GL_CULL_FACE 0xB44
#define GL_BLEND 0xBE2
#define GL_SRC_ALPHA 0x302
#define GL_ONE_MINUS_SRC_ALPHA 0x303
#define GL_ONE 1
#define GL_ZERO 0
#define GL_LEQUAL 0x203
#define GL_TEXTURE_2D 0xDE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VIEWPORT 0xBA2
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_UNPACK_ALIGNMENT 0xCF5
#define GL_RENDERBUFFER 0x8D41
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_STENCIL_TEST 0xB90
#define GL_STENCIL_BUFFER_BIT 0x400
#define GL_ALWAYS 0x207
#define GL_EQUAL 0x202
#define GL_KEEP 0x1E00
#define GL_REPLACE 0x1E01
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
#define GL_SCISSOR_TEST 0xC11
#define GL_FRONT 0x404
#define GL_BACK 0x405

#define SH_GL_FUNCS(X)                                                                              \
    X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                         \
    X(void, Clear, GLbitfield)                                                                      \
    X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                               \
    X(void, ReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)                      \
    X(void, Enable, GLenum)                                                                         \
    X(void, Disable, GLenum)                                                                        \
    X(void, DepthFunc, GLenum)                                                                      \
    X(void, DepthMask, GLboolean)                                                                   \
    X(void, BlendFunc, GLenum, GLenum)                                                              \
    X(void, PixelStorei, GLenum, GLint)                                                             \
    X(const unsigned char*, GetString, GLenum)                                                      \
    X(void, GetIntegerv, GLenum, GLint*)                                                            \
    X(void, GenTextures, GLsizei, GLuint*)                                                          \
    X(void, DeleteTextures, GLsizei, const GLuint*)                                                 \
    X(void, BindTexture, GLenum, GLuint)                                                            \
    X(void, ActiveTexture, GLenum)                                                                  \
    X(void, TexParameteri, GLenum, GLenum, GLint)                                                   \
    X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) \
    X(void, GenerateMipmap, GLenum)                                                                 \
    X(void, GenBuffers, GLsizei, GLuint*)                                                           \
    X(void, DeleteBuffers, GLsizei, const GLuint*)                                                  \
    X(void, BindBuffer, GLenum, GLuint)                                                             \
    X(void, BufferData, GLenum, GLsizeiptr, const void*, GLenum)                                    \
    X(void, GenVertexArrays, GLsizei, GLuint*)                                                      \
    X(void, DeleteVertexArrays, GLsizei, const GLuint*)                                             \
    X(void, BindVertexArray, GLuint)                                                                \
    X(void, EnableVertexAttribArray, GLuint)                                                        \
    X(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)            \
    X(void, DrawArrays, GLenum, GLint, GLsizei)                                                     \
    X(void, DrawElements, GLenum, GLsizei, GLenum, const void*)                                     \
    X(GLuint, CreateShader, GLenum)                                                                 \
    X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)                      \
    X(void, CompileShader, GLuint)                                                                  \
    X(void, GetShaderiv, GLuint, GLenum, GLint*)                                                    \
    X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                   \
    X(void, DeleteShader, GLuint)                                                                   \
    X(GLuint, CreateProgram)                                                                        \
    X(void, AttachShader, GLuint, GLuint)                                                           \
    X(void, BindAttribLocation, GLuint, GLuint, const GLchar*)                                      \
    X(void, LinkProgram, GLuint)                                                                    \
    X(void, GetProgramiv, GLuint, GLenum, GLint*)                                                   \
    X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                  \
    X(void, UseProgram, GLuint)                                                                     \
    X(void, DeleteProgram, GLuint)                                                                  \
    X(GLint, GetUniformLocation, GLuint, const GLchar*)                                             \
    X(void, Uniform1i, GLint, GLint)                                                                \
    X(void, Uniform2f, GLint, GLfloat, GLfloat)                                                     \
    X(void, Uniform3f, GLint, GLfloat, GLfloat, GLfloat)                                            \
    X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat)                                   \
    X(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*)                            \
    X(void, GenFramebuffers, GLsizei, GLuint*)                                                      \
    X(void, DeleteFramebuffers, GLsizei, const GLuint*)                                             \
    X(void, BindFramebuffer, GLenum, GLuint)                                                        \
    X(void, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)                            \
    X(GLenum, CheckFramebufferStatus, GLenum)                                                       \
    X(void, GenRenderbuffers, GLsizei, GLuint*)                                                     \
    X(void, DeleteRenderbuffers, GLsizei, const GLuint*)                                            \
    X(void, BindRenderbuffer, GLenum, GLuint)                                                       \
    X(void, RenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)                                  \
    X(void, FramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)                                \
    X(void, PolygonOffset, GLfloat, GLfloat)                                                        \
    X(void, LineWidth, GLfloat)                                                                     \
    X(void, GetFloatv, GLenum, GLfloat*)                                                            \
    X(void, StencilFunc, GLenum, GLint, GLuint)                                                     \
    X(void, StencilOp, GLenum, GLenum, GLenum)                                                      \
    X(void, ColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                                  \
    X(void, CullFace, GLenum)                                                                       \
    X(void, Scissor, GLint, GLint, GLsizei, GLsizei)

namespace sh::gl {
#define X(ret, name, ...) extern ret(SH_GLAPI* name)(__VA_ARGS__);
SH_GL_FUNCS(X)
#undef X
// Load every function via `getProc` (e.g. SDL_GL_GetProcAddress). Returns false if any is missing.
bool load(void* (*getProc)(const char*));
} // namespace sh::gl
