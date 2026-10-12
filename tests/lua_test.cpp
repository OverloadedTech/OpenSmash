// Lua host without game data: the script sandbox (lua_host.cpp) and the patched 5.2.4 parser (cmake/patch_lua52.cmake).
#include <cstdio>
#include <string>
#include "script/lua_host.h"
#include "check.h"
using namespace sh;

static bool runs(LuaHost& h, const char* src) {
    std::string err;
    bool ok = h.run(src, "=test", &err);
    if (!ok) printf("  lua: %s\n", err.c_str());
    return ok;
}

int main() {
    LuaHost h;
    // sandbox: nothing that reaches files, processes or bytecode
    CHECK(runs(h, "assert(io == nil and package == nil and debug == nil and require == nil)"));
    CHECK(runs(h, "assert(dofile == nil and loadfile == nil and string.dump == nil)"));
    CHECK(runs(
        h,
        "assert(os.execute == nil and os.remove == nil and os.rename == nil and os.exit == nil and os.getenv == nil)"));
    CHECK(runs(
        h,
        "assert(type(os.clock()) == 'number' and type(math.floor) == 'function' and type(bit32.band) == 'function')"));
    CHECK(runs(h, "assert(load('return 1 + 1')() == 2)"));
    CHECK(
        runs(h, "local f, e = load('\\27Lua', 'x', 'b') assert(f == nil and e:find('binary'))")); // mode forced to "t"
    std::string err;
    CHECK(!h.run("\x1bLua", "=bin", &err));
    // upstream Lua 5.3.5 fix: a 'for ... in' list with 4+ expressions overflowed the parser's register array
    CHECK(runs(h, "local f = load('for _ in _,_,_,_ do local function _() end end') assert(f)"));
    CHECK(runs(
        h,
        "local n = 0 for k in next, {1}, nil, 9 do local z = k * 100 assert(z == 100) n = n + 1 end assert(n == 1)"));
    CHECK(runs(h, "local x, y = 1, 2, 3, 4 assert(x == 1 and y == 2) local a, b, c = (function() return 7, 8 end)() "
                  "assert(a == 7 and b == 8 and c == nil)"));
    CHECK(runs(h, "local t = {} t[1], t[2] = 5, 6, 7 assert(t[1] == 5 and t[2] == 6)"));
    printf(fails ? "lua_test: %d failure(s)\n" : "lua_test: ok\n", fails);
    return fails ? 1 : 0;
}
