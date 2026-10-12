# Differences from the original

OpenSmash aims to play exactly like Smash Hit 1.5.14 for Android. Its behaviour was worked out from the original's
scripts, data and native library (see [HOW_IT_WORKS.md](HOW_IT_WORKS.md)) and is checked with tests and rendered
frames. It has not yet been compared frame by frame with the phone version, so recordings from a real device are
very useful. The item marked *needs checking* is the one most worth comparing.

## On purpose

- **No store, ads or online services.** Premium content and all modes are unlocked. When you run out of balls you
  get one free revive per run (the premium behaviour), except in versus.
- **Shards and the ball.** Freshly broken shards ignore balls for two ticks. Following the original rules literally
  in this solver made a ball lose far too much speed on a single pane; with this change it loses about 2.5 m/s per
  pane as intended. *Needs checking:* ball speed after going through a window.
- **Sandboxed scripts.** The scripts come from a file the player supplies, so the Lua states don't get `io`,
  `package`, `debug`, `os.execute`, `dofile`/`loadfile` or binary chunks. The game's own scripts don't use any of them.
- **HUD after a run.** The HUD keeps fading out after you leave a run, as in the original, but it no longer accepts
  taps while it does, so a stray tap can't pause the menu or restart the level.
- **Narrow windows** (portrait, square) are letterboxed to 4:3, the narrowest shape the original layouts were made
  for.
- **Muted sound effects stay muted** during slow motion. The original briefly overrides the effects volume with the
  time scale.
- **Checkpoint rewind** restarts the level at the first checkpoint. On a phone it is only reachable through the
  rewarded-ad flow, which doesn't exist here.

## Approximations

- **Contacts.** The original uses GJK/EPA with persistent contact manifolds; OpenSmash uses separating-axis tests with
  face clipping and rebuilds contacts every tick. Stacks and resting objects behave the same in the tests, but small
  differences in feel are possible.
- **Menu corridor.** The 3D corridor behind the level buttons is rebuilt from the menu meshes with an estimated camera,
  and the per-mode tint is approximate.
- **Rolling and sliding sounds** start whenever a body has a loaded contact, which may differ slightly from the
  original's rule.

## Not implemented

- Ball skins, 3D models and matcaps. The data is in the APK, but the original never uses it either.
- Water splash particles. The original spawns them but they are never visible.

## Two-player modes

Versus and co-op use the original's tabletop layout: each half of the screen is a portrait view rotated towards one of
two players sitting across a tablet. On a monitor both views are therefore sideways. An upright layout would need
changes to the original HUD scripts, so it was left as is.
