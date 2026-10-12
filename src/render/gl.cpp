#include "render/gl.h"
#include <cstdio>

namespace sh::gl {
#define X(ret, name, ...) ret(SH_GLAPI* name)(__VA_ARGS__) = nullptr;
SH_GL_FUNCS(X)
#undef X

bool load(void* (*getProc)(const char*)) {
    bool ok = true;
#define X(ret, name, ...)                                                      \
    name = reinterpret_cast<ret(SH_GLAPI*)(__VA_ARGS__)>(getProc("gl" #name)); \
    if (!name) {                                                               \
        fprintf(stderr, "missing GL function gl%s\n", #name);                  \
        ok = false;                                                            \
    }
    SH_GL_FUNCS(X)
#undef X
    return ok;
}
} // namespace sh::gl
