// Thin owner of a Lua 5.2 state. The `mg*` host API is registered by the game code.
#pragma once
#include <string>

struct lua_State;

namespace sh {

class LuaHost {
public:
    LuaHost();
    ~LuaHost();
    LuaHost(const LuaHost&) = delete;
    LuaHost& operator=(const LuaHost&) = delete;
    lua_State* L() const { return L_; }
    // Compile (without running) a chunk; returns false and fills err on syntax error.
    bool check(const std::string& src, const std::string& chunkName, std::string* err);
    // Compile and run a chunk in the global environment.
    bool run(const std::string& src, const std::string& chunkName, std::string* err);

private:
    lua_State* L_;
};

} // namespace sh
