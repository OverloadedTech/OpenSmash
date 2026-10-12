#include "script/lua_host.h"
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace sh {

namespace {
// base load() with the mode forced to "t": source text only, never a precompiled chunk (crafted bytecode can break out
// of the VM). menu/main.lua replaces the global with its own load() anyway; this keeps text loading for mods.
int loadText(lua_State* L) {
    if (lua_gettop(L) < 3) lua_settop(L, 3);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}
} // namespace

LuaHost::LuaHost() : L_(luaL_newstate()) {
    // The original opens every library, but it runs inside the Android sandbox. Here the scripts come from a
    // user-supplied APK, so nothing that reaches files, processes or bytecode is opened: no io, package or debug, no
    // dofile, loadfile or string.dump, and os only keeps clock, date, difftime and time. The shipped scripts only use
    // math, string, tonumber and type.
    static const luaL_Reg kLibs[] = {{"_G", luaopen_base},
                                     {LUA_COLIBNAME, luaopen_coroutine},
                                     {LUA_TABLIBNAME, luaopen_table},
                                     {LUA_STRLIBNAME, luaopen_string},
                                     {LUA_BITLIBNAME, luaopen_bit32},
                                     {LUA_MATHLIBNAME, luaopen_math},
                                     {LUA_OSLIBNAME, luaopen_os}};
    for (const luaL_Reg& l : kLibs) {
        luaL_requiref(L_, l.name, l.func, 1);
        lua_pop(L_, 1);
    }
    for (const char* g : {"dofile", "loadfile", "loadstring"}) {
        lua_pushnil(L_);
        lua_setglobal(L_, g);
    }
    lua_getglobal(L_, "load");
    lua_pushcclosure(L_, loadText, 1);
    lua_setglobal(L_, "load");
    lua_getglobal(L_, LUA_STRLIBNAME);
    lua_pushnil(L_);
    lua_setfield(L_, -2, "dump");
    lua_pop(L_, 1);
    lua_getglobal(L_, LUA_OSLIBNAME);
    for (const char* k : {"execute", "exit", "getenv", "remove", "rename", "tmpname", "setlocale"}) {
        lua_pushnil(L_);
        lua_setfield(L_, -2, k);
    }
    lua_pop(L_, 1);
}
LuaHost::~LuaHost() { lua_close(L_); }

bool LuaHost::check(const std::string& src, const std::string& name, std::string* err) {
    int rc = luaL_loadbufferx(L_, src.data(), src.size(), name.c_str(), "t"); // text only (no precompiled chunks)
    if (rc != LUA_OK) {
        if (err) *err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    lua_pop(L_, 1);
    return true;
}

bool LuaHost::run(const std::string& src, const std::string& name, std::string* err) {
    if (luaL_loadbufferx(L_, src.data(), src.size(), name.c_str(), "t") != LUA_OK || lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        if (err) *err = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

} // namespace sh
