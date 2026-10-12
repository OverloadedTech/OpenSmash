# Vendored third-party sources

Copies of the exact versions pinned in `CMakeLists.txt`, created by `tools/vendor_deps.sh` so the project builds
without internet access. Each directory keeps its upstream license file. Unused upstream parts are removed (SDL:
`test/`, `examples/`, `Xcode/`, `VisualC*/`; stb: `tests/`, `data/`). One source file is patched: `lua52/src/lparser.c`
carries the upstream Lua 5.3.5 fix for "Expression list with four or more expressions in a 'for' loop can crash the
interpreter" (applied by `cmake/patch_lua52.cmake`; CMake refuses an unpatched tree).

| Directory | Project | Version | License |
|---|---|---|---|
| `SDL3/` | SDL | 3.2.22 | zlib (`LICENSE.txt`) |
| `lua52/` | Lua | 5.2.4 | MIT (in `doc/readme.html` and `src/lua.h`) |
| `miniz/` | miniz | 3.0.2 | MIT (`LICENSE`) |
| `stb/` | stb | commit 2c980bb | MIT or public domain (`LICENSE`) |
