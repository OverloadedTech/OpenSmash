# Content definitions

* `game.xml`: `<game><levels><level name=".."/>…` ordered list of campaign levels (duplicates intentional: endless).
* `levels/<name>.xml`: `<level><room type="theme/room" length="N" start="bool" end="bool"/>…</level>`. `type` maps to `rooms/<type>.lua`. `start`/`end` are passed to the room script as `mgGetBool("start"/"end")`. (`levels/trailer.xml` references a nonexistent room, a leftover from development.)
* `rooms/**/*.lua`: Lua 5.2 chunks with `init()` and `tick()`; call `mgMusic`, `mgReverb`, `mgEcho`, `mgFogColor`, `confSegment(path, weight)`, `nextSegment()`, `mgSegment(path, z)` (returns segment length), `mgLength`. Shared helpers in `rooms/include.lua`.
* `segments/<theme>/<kind>/<name>.xml`: `<segment size="x y z" template="..">` children: `<box size pos hidden template>`, `<decal pos tile size color blend>`, `<obstacle pos type template mode paramN="key=value">`, `<model>`, `<powerup>`, `<water>`. Counts in 1.5.14: 40062 box, 1638 obstacle, 1112 decal, 59 powerup, 6 model, 1 water.
* `obstacles/*.lua`: `init()` reads parameters via `mgGetFloat/Int/Bool/Color/String(name, default)` and builds physics bodies using `mgCreateBody/Box/Cylinder/Mesh/…Joint`. `obstacles/boss/` for boss fights.
* `materials.xml`: `<material name density friction restitution color type forcelimit static category mask>`; `type` selects shader/sound class (glass, metal, marble…); `forcelimit` is the breakage threshold.
* `templates.xml`: `<template name><properties …/></template>`: named bundles of key=value properties merged into segment/obstacle attributes (color, tile, lighting, fogcolor, softshadow, reflection).
* `meshes/*.xml`: `<convex><v>x y z</v>…` convex hull vertex lists usable by `mgCreateMesh`.
* `mesh_models.xml` + `models/`: OBJ models (skull, gecko, blue_monster) with albedo texture, matcap and shader.
* `ball_styles.xml`, `menu.xml`: cosmetic ball table; menu tile tints.
