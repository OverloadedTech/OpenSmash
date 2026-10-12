# Changelog

## 0.1.0

First public release.

- The full game: main menu, 12 campaign levels and endless mode, checkpoints, training, classic, expert, zen, versus
  and co-op, powerups, boss rooms, revive, quick resume and the first-run tutorials.
- Physics with breakable glass, debris, joints, ropes and motors.
- OpenGL 3.3 renderer using the game's own shaders: glass refraction, floor reflections, soft shadows, particles,
  lasers, depth of field, split-screen, three quality levels.
- Audio mixer with the music-synced camera, room music crossfades, boss music, per-room reverb and echo, positional
  sound and the powerup flanger.
- Any window size and aspect ratio, HiDPI, fullscreen, mouse, touch, keyboard and gamepads.
- Downloads for Windows x64, Linux x64/ARM64 and macOS (Apple Silicon) with the game data included. The data comes
  from Smash Hit 1.5.14 for Android (APK SHA-256
  3b2ffa02fee8ca762f40648f1a9bec5c308764e881dd6a79cacd6dccdb16e338).

Windows, macOS and Linux ARM64 are built and tested in CI but haven't been play-tested yet. Known differences from
the original are in [docs/DIFFERENCES.md](docs/DIFFERENCES.md).
