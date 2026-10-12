#include "render/shader.h"
#include <cctype>
#include <cstring>
#include <vector>

namespace sh {

std::string Shader::translate(const std::string& src, bool vertex) {
    std::string out = "#version 330 core\n";
    out += vertex ? "#define VERTEX 1\n#define attribute in\n#define varying out\n"
                  : "#define FRAGMENT 1\n#define varying in\nout vec4 shFragColor;\n#define gl_FragColor shFragColor\n";
    out += "#define texture2D texture\n#define lowp\n#define mediump\n#define highp\n";
    // GLSL ES allows "precision" statements; strip them (core has none needed).
    size_t i = 0;
    while (i < src.size()) {
        size_t e = src.find('\n', i);
        if (e == std::string::npos) e = src.size();
        std::string line = src.substr(i, e - i);
        size_t p = line.find_first_not_of(" \t\r");
        if (p != std::string::npos && line.compare(p, 9, "precision") == 0) line = "";
        // HLSL-style casts "(type)expr" are accepted by some mobile drivers; rewrite to constructor form.
        for (const char* ct : {"(mat3)", "(mat4)", "(vec3)", "(vec4)", "(float)", "(int)"}) {
            size_t k;
            while ((k = line.find(ct)) != std::string::npos) {
                size_t b = k + strlen(ct), end = b;
                while (end < line.size() && (isalnum((unsigned char)line[end]) || line[end] == '_')) end++;
                if (end < line.size() && line[end] == '(') { // function call: consume balanced parens
                    int depth = 0;
                    for (; end < line.size(); end++) {
                        if (line[end] == '(') depth++;
                        else if (line[end] == ')' && --depth == 0) {
                            end++;
                            break;
                        }
                    }
                }
                line = line.substr(0, k) + std::string(ct + 1, strlen(ct) - 2) + "(" + line.substr(b, end - b) + ")" +
                       line.substr(end);
            }
        }
        out += line;
        out += '\n';
        i = e + 1;
    }
    return out;
}

static GLuint compile(GLenum type, const std::string& text, const std::string& name, std::string* err) {
    GLuint s = gl::CreateShader(type);
    const char* p = text.c_str();
    gl::ShaderSource(s, 1, &p, nullptr);
    gl::CompileShader(s);
    GLint ok = 0;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        gl::GetShaderInfoLog(s, sizeof log, nullptr, log);
        if (err) *err = name + (type == GL_VERTEX_SHADER ? " (vertex): " : " (fragment): ") + log;
        gl::DeleteShader(s);
        return 0;
    }
    return s;
}

bool Shader::build(const std::string& source, const std::string& name, std::string* err) {
    GLuint vs = compile(GL_VERTEX_SHADER, translate(source, true), name, err);
    if (!vs) return false;
    GLuint fs = compile(GL_FRAGMENT_SHADER, translate(source, false), name, err);
    if (!fs) {
        gl::DeleteShader(vs);
        return false;
    }
    prog_ = gl::CreateProgram();
    gl::AttachShader(prog_, vs);
    gl::AttachShader(prog_, fs);
    gl::BindAttribLocation(prog_, ATTR_POSITION, "aPosition");
    gl::BindAttribLocation(prog_, ATTR_NORMAL, "aNormal");
    gl::BindAttribLocation(prog_, ATTR_TEXCOORD, "aTexCoord");
    gl::BindAttribLocation(prog_, ATTR_COLOR, "aColor");
    gl::BindAttribLocation(prog_, ATTR_TEXCOORD1, "aTexCoord1");
    gl::BindAttribLocation(prog_, ATTR_ALPHA, "aAlpha");
    gl::LinkProgram(prog_);
    gl::DeleteShader(vs);
    gl::DeleteShader(fs);
    GLint ok = 0;
    gl::GetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        gl::GetProgramInfoLog(prog_, sizeof log, nullptr, log);
        if (err) *err = name + " link: " + log;
        gl::DeleteProgram(prog_);
        prog_ = 0;
        return false;
    }
    return true;
}

GLint Shader::uniform(const char* n) const {
    auto it = cache_.find(n);
    if (it != cache_.end()) return it->second;
    GLint l = gl::GetUniformLocation(prog_, n);
    cache_[n] = l;
    return l;
}

Shader::~Shader() {
    if (prog_ && gl::DeleteProgram) gl::DeleteProgram(prog_);
}

} // namespace sh
