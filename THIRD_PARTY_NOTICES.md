# Third-party software

These libraries are compiled into OpenSmash. In the source repository they are in `third_party/`, unmodified except
for one upstream Lua 5.3.5 parser fix in `lua52/src/lparser.c` (see `third_party/README.md` there). SDL includes the
yuv2rgb converter by Adrien Descamps (BSD-3-Clause).

| Library | Version | Used for | License |
|---|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) | 3.2.22 (or the system package) | window, OpenGL context, input, audio, file dialog | zlib |
| [Lua](https://www.lua.org) | 5.2.4 | runs the game's own room/obstacle/menu/HUD scripts | MIT |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | reading the APK (zip) and zlib-compressed meshes | MIT |
| [stb](https://github.com/nothings/stb) (`stb_image`, `stb_vorbis`, `stb_image_write`) | commit 2c980bb | PNG/JPEG textures, Ogg Vorbis audio, screenshots | Public domain / MIT |

Release packages link all of them statically. Their full license texts are in the package's `licenses/` folder (in the
source tree: `third_party/*/LICENSE*`, and `third_party/lua52/src/lua.h` for Lua).

Release packages also contain the Smash Hit game assets (`assets/`), © Mediocre AB, distributed with permission and
not covered by any open-source license; see `ASSETS_NOTICE.txt` in the package.
