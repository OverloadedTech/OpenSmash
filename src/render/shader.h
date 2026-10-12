// GLSL program built from the game's single-file shaders (GLSL ES 1.00 with #ifdef VERTEX / FRAGMENT).
#pragma once
#include <string>
#include <unordered_map>
#include "render/gl.h"

namespace sh {

// Attribute locations are fixed so every mesh layout can share VAO setup.
enum AttribLoc {
    ATTR_POSITION = 0,
    ATTR_NORMAL = 1,
    ATTR_TEXCOORD = 2,
    ATTR_COLOR = 3,
    ATTR_TEXCOORD1 = 4,
    ATTR_ALPHA = 5
};

class Shader {
public:
    ~Shader();
    bool build(const std::string& source, const std::string& name, std::string* err);
    void use() const { gl::UseProgram(prog_); }
    GLint uniform(const char* name) const; // cached; -1 if absent
    GLuint id() const { return prog_; }

    // Converts GLSL ES 1.00 source into stage-specific GLSL 330 core text (exposed for tests).
    static std::string translate(const std::string& source, bool vertex);

private:
    GLuint prog_ = 0;
    mutable std::unordered_map<std::string, GLint> cache_;
};

} // namespace sh
