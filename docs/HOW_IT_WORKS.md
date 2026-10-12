# How OpenSmash works

A short tour for anyone who wants to read or change the code.

## Content and engine

Smash Hit is very data-driven. Most of what you see is defined by files in the APK's `assets/` folder:

| Files | What they are | Handled by |
|---|---|---|
| `segments/**/*.mesh` | room geometry with the lighting baked into vertex colours | `GameRenderer` |
| `segments/**/*.xml`, `levels/`, `materials.xml`, `templates.xml` | room layouts, level order, physics materials | `Content`, `Room` |
| `rooms/**/*.lua`, `obstacles/**/*.lua` | scripts that build each room and animate the obstacles | `LevelScript` (Lua 5.2) |
| `menu/main.lua`, `hud/main.lua` | the whole menu and HUD | `UiScript` (Lua 5.2) |
| `shaders/*.glsl` | the game's GLSL ES shaders | `Shader`, translated to GLSL 3.30 at load time |
| `music/`, `snd/`, `gfx/`, `fonts/`, `*.mtx` | audio, sprites, fonts, textures | `AudioSystem`, `Texture` (`.mtx` in `src/assets/image.cpp`), `BitmapFont`/`UnicodeFont` |

Most files in the APK's `assets/` folder carry an extra `.mp3` suffix (it stops the Android packager from
compressing them; the PNGs, which it leaves alone anyway, don't have it). `Vfs` strips it and reads straight from
the APK, an extracted folder or a plainly unzipped APK.

What isn't in the data is the engine itself: physics, glass fracture, scoring rules, the camera, rendering, audio and
the host functions the scripts call. That is the part OpenSmash implements.

```
smashhit.apk ──► Vfs
                  │
   ┌──────────────┼──────────────────────────┐
   ▼              ▼                          ▼
Content       LevelScript                UiScript
(XML)         rooms/obstacles,           menu + HUD,
   │          103 mg* functions          67 mg* functions
   ▼              ▼                          │
Room ───────► World (physics)                │
   │              │                          │
   └────► Level (rules, camera, scoring) ◄───┘
                  │
     GameRenderer · UiRenderer · AudioSystem
                  │
         SDL3 window, OpenGL 3.3, audio
```

## Source tree

```
src/core      math, random helpers, the asset VFS, a small XML parser
src/assets    PNG/JPEG and .mtx texture decoding
src/physics   convex polyhedra, contacts and solver, fracture, joints, ray casts
src/script    Lua state wrapper
src/game      content loading, rooms and obstacles, level rules, the level script API, particles
src/ui        the menu/HUD script API, UI objects and canvases, fonts and locale
src/render    OpenGL loader, shader translation, textures and FBOs, 3D and 2D renderers
src/audio     Ogg streaming and the software mixer
src/app       game state and saves (game_app), SDL front-end and input (main), headless runner
tools/        asset extraction and validation, release packaging
tests/        unit tests and tests against the real game data
```

`core`, `assets`, `physics`, `script`, `game`, `ui` and the Ogg decoder form the `shcore` library, which has no SDL or
OpenGL dependency. That is what makes the headless runner and most tests possible.

## A frame

The loop in `src/app/main.cpp` turns SDL input (mouse, touch, keyboard, gamepads) into pointer states and calls
`GameApp::frame()` (`src/app/game_app.cpp`) once per displayed frame:

1. The pointers are handed to the menu, the HUD and the level.
2. The simulation runs in fixed 1/60 s ticks, at most two per frame like the original. Each tick runs the menu and
   HUD scripts, then `Level::update()` (`src/game/level.cpp`): camera, room and obstacle scripts, lasers, powerups,
   bosses, `World::step()`, sounds and debris, the crash test and the game-over rules.
3. `GameRenderer::drawLevel()` draws the 3D scene, then the menu and HUD are drawn from the scripts' UI objects. Body
   poses and the camera are interpolated between ticks, so motion is smooth at any refresh rate.
4. Taps go to the UI first and only reach the level if no button took them. `Level::handleInput()` decides how many
   balls the streak allows and `Level::shoot()` turns each into a ray from the camera.

The camera is tied to the music. Each level's soundtrack is streamed and the camera's position along the track is
derived from the playback position, which is why the game feels locked to the beat. With audio disabled the clock
runs freely.

## Glass

`src/physics/` is a small rigid-body engine: convex polyhedra, separating-axis contacts with face clipping, and a
sequential-impulse solver with 4 iterations. Breakable materials in `materials.xml` (wood and glass) have a
`forcelimit`, and scripts can set one with `mgForceLimit`; shapes without one never break. The limit caps the
impulse any contact on that shape can take, and when the accumulated impulse reaches it the body breaks. `fracture.cpp`
cuts the body into cells with jittered planes around the impact point, frees the pieces close to it as shards and
regroups the rest by connectivity. Small pieces become debris that flies off and fades out without taking part in
the simulation.

Because the cap is small compared with the momentum of a ball, a ball keeps most of its speed when it goes through a
pane, which is a big part of how the game feels.

## Scripts

Room, obstacle, menu and HUD scripts call host functions such as `mgCreateBody`, `mgCreateBox`, `mgSetPos`,
`mgCreateUi` or `mgGet("player.balls")`. `src/game/level_script.cpp` and `src/ui/ui_script.cpp` register C++
implementations with the same names and argument conventions. Since the scripts are untouched, their bugs come along
too: the original menu's uninitialised `glowT` raises the same error it does on a phone.

Scripts come from a user-supplied file, so the Lua states are sandboxed: no `io`, `package`, `debug`, `os.execute` or
binary chunks.

## Rendering

The game targets OpenGL ES 2.0. OpenSmash uses an OpenGL 3.3 core profile and translates the shaders as they load
(`attribute`/`varying` to `in`/`out`, `texture2D` to `texture`, precision qualifiers). `GameRenderer::drawView` draws
in the original's order: background, baked rooms, plain shapes, glass (refracting a quarter-resolution copy of the
previous frame), debris, metal, floor reflections through the stencil buffer, soft shadows, the chrome ball discs,
sprites and particles, the best-distance marker, and depth of field on high quality. Versus and co-op render two
rotated portrait views, the way the tablet version is laid out for two players facing each other.

## Audio

`src/audio/audio_system.cpp` is a software mixer on an SDL3 audio stream: pooled world and UI voices, looping
channels for laser hum, rolling and sliding, a two-slot music stream that crossfades between rooms, the boss music
chain, per-room reverb, echo and low-pass with one-second transitions, and a flanger during powerups. Slow motion
lowers the pitch and volume of the music and the world sounds together.

## Testing without a screen

- `ctest` runs physics, polyhedron, world and Lua tests without any game data. With `SH_ASSETS=<apk>` it also parses
  every asset, runs all room and obstacle scripts, loads the real menu and HUD, and lets a bot play.
- `opensmash_headless --level all --full` plays every campaign room with a bot in god mode and fails on script errors
  or a level that gets stuck.
- `opensmash --offscreen --screenshot out.png --ticks N` renders deterministic frames. The README screenshots were
  made that way, on a machine without a GPU.

## How the behaviour was worked out

The original engine's source was never published. Its behaviour was worked out by studying the game: decoding the
asset formats (see [formats/](formats/README.md)), reading the scripts, and analysing the Android build's native
library, which still contains C++ symbol names. OpenSmash's code was written from those notes; it contains no original
program code and never loads the original binary.

## Where to start

| To... | Read |
|---|---|
| follow a frame | `GameApp::frame`, `Level::update` |
| change physics or fracture | `src/physics/solver.cpp`, `src/physics/fracture.cpp` |
| fix a script incompatibility | `src/game/level_script.cpp`, `src/ui/ui_script.cpp` |
| change a render pass | `GameRenderer::drawView` |
| port to another platform | `src/app/main.cpp`, `CMakePresets.json`, `.github/workflows/build.yml` |
