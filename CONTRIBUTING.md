# Contributing

Thanks for your interest! Things that help the most right now:

- **Platform reports.** If you run OpenSmash on Windows, macOS or Linux ARM64, open an issue (there's a template) and
  say whether it starts, plays and sounds right, plus your OS and GPU.
- **Comparisons with the phone version.** Anything that looks, sounds or plays differently. A short recording of the
  same spot on both helps a lot. [docs/DIFFERENCES.md](docs/DIFFERENCES.md) lists the known differences.
- **Fixes**, especially for platform problems and differences from the phone version.

## Rules

- Never commit or attach game files: no APKs, extracted assets or anything decompiled from the original binary.
  Release packages get their assets from a private source configured in CI, not from the repository.
- Write the code yourself. Describing how the original behaves is fine; pasting decompiler output is not.
- Keep the simulation (`src/core`, `src/physics`, `src/game`, `src/script`, `src/ui`) free of SDL and OpenGL so it
  can be tested headlessly.
- Format C++ with the repository's `.clang-format` and match the surrounding code.

## Working on the code

```sh
cmake --preset linux-gcc && cmake --build --preset linux-gcc
SH_ASSETS=/path/to/smashhit.apk ctest --test-dir build/linux-gcc
build/linux-gcc/opensmash_headless --assets smashhit.apk --level all --full
build/linux-gcc/opensmash --assets smashhit.apk --offscreen --level 3 --ticks 900 --screenshot out.png
```

The offscreen mode (`--offscreen --screenshot`, together with `--ticks`, `--click x,y@frame`, `--cmd "frame:command"`,
`--bot` and `--god`) renders deterministic frames without a display. For changes to rendering, please include before
and after screenshots in the pull request.

## Pull requests

- One topic per pull request.
- All `ctest` suites should pass. If you have the game data, run them with `SH_ASSETS` and mention it.
- If a change makes OpenSmash behave differently from the original on purpose, add it to `docs/DIFFERENCES.md`.
- Contributions are accepted under the project's MIT License.
