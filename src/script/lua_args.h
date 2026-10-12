// Reads the arguments of a Lua C function by 0-based index; missing arguments read as 0, false or "".
#pragma once
#include <string>
extern "C" {
#include <lua.h>
}
#include "core/math.h"

namespace sh {

struct LuaArgs {
    lua_State* L;
    int n;
    explicit LuaArgs(lua_State* l) : L(l), n(lua_gettop(l)) {}
    float f(int i) const { return i < n ? (float)lua_tonumberx(L, i + 1, nullptr) : 0.0f; }
    int i(int k) const { return k < n ? (int)lua_tointegerx(L, k + 1, nullptr) : 0; }
    bool b(int k) const { return k < n && lua_toboolean(L, k + 1) == 1; }
    std::string s(int k) const {
        if (k >= n) return "";
        const char* p = lua_tolstring(L, k + 1, nullptr);
        return p ? p : "";
    }
    void* p(int k) const { return k < n ? lua_touserdata(L, k + 1) : nullptr; }
    Vec3 v3(int k) const { return Vec3(f(k), f(k + 1), f(k + 2)); }
};

} // namespace sh
