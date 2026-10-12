# Releasing OpenSmash

Releases are built, tested and published by GitHub Actions ([release.yml](../.github/workflows/release.yml)). Each
release has four downloads, each a complete game with the Smash Hit assets included:

| Package | Contents |
|---|---|
| `OpenSmash-<v>-windows-x64.zip` | `OpenSmash.exe` (MSVC, static runtime, SDL3 linked in) + `assets/` + notices |
| `OpenSmash-<v>-linux-x64.tar.gz`, `-linux-arm64.tar.gz` | `opensmash` (built on Ubuntu 24.04, glibc 2.39+) + `assets/` + notices |
| `OpenSmash-<v>-macos-arm64.zip` | `OpenSmash.app` (assets in `Contents/Resources/assets`) + notices |
| `SHA256SUMS.txt` | checksums of the above |

The game finds the bundled `assets/` folder next to the executable (inside the `.app` on macOS) on its own.

## Game assets in CI

The game data is not in the repository. `release.yml` and the game-data job in `build.yml` download the Smash Hit APK
from the URL in the `ASSETS_APK_URL` repository secret, and `tools/ci_fetch_assets.py` extracts only its `assets/`
tree, never the original native libraries or Java code. Two more secrets are optional:

* `ASSETS_APK_TOKEN`: a bearer token for a URL that needs authentication. For an asset in a private GitHub release,
  use the API URL `https://api.github.com/repos/<owner>/<repo>/releases/assets/<asset-id>` and a fine-grained token
  with read access to that repository's contents.
* `ASSETS_APK_SHA256`: the APK's SHA-256 hash (the first column of `sha256sum` output), so a wrong or corrupted
  download fails the build.

The secrets are set under **Settings > Secrets and variables > Actions**. They are never printed in logs and are not
given to pull requests from forks. For local runs without `ASSETS_APK_URL`, `ci_fetch_assets.py` also accepts an
extracted asset tree in a git-ignored `game-assets/` folder at the repository root.

## Cutting a release

1. Update `CHANGELOG.md` with a `## <version>` section (its text becomes the release notes; mention which Smash Hit
   version the bundled assets come from) and `project(opensmash VERSION ...)` in `CMakeLists.txt` (local builds
   report it; release builds take the version from the tag).
2. Commit, then tag and push:
   ```sh
   git tag v0.1.0 && git push origin v0.1.0
   ```
   Tags with a suffix (`v0.2.0-rc1`) are published as pre-releases.
3. The workflow builds all four platforms, runs the full test suite with the real game data on each one (every
   asset parsed, every level script, the original menu/HUD, a bot playing every campaign room), packages, and only if
   everything passes publishes the release with notes from the changelog.

A release never ships without assets: if no asset source is configured, or the download or checksum fails, the
workflow stops before publishing.

## Dry run

**Actions > release > Run workflow** builds and tests the same packages and attaches them to the workflow run as
artifacts, without creating a release. Use it to try a build before tagging. The artifacts contain the game assets
and are visible to anyone who can see the repository's Actions runs, so treat a dry run like a release.

## Everyday CI

[build.yml](../.github/workflows/build.yml) runs on every push and pull request: all four platforms are built and
unit-tested and asset-free packages are uploaded as artifacts (useful for testers who have their own APK). When the
asset secret is configured, pushes also run the full game-data test suite on Linux.

## Local packaging

```sh
cmake --preset linux-gcc -DOPENSMASH_VERSION_STRING=0.1.0 && cmake --build --preset linux-gcc
python3 tools/make_release.py --platform linux-x64 --binary build/linux-gcc/opensmash --version 0.1.0 \
    --apk /path/to/smashhit.apk            # or --assets <extracted dir>, or neither for an asset-free package
```

A Linux binary linked against an installed SDL3 is packaged as `lib/opensmash` and `lib/libSDL3.so.0`, started by an
`opensmash` launcher script.

macOS packages must be made on a Mac (the packager checks the `.app` links no non-system libraries and ad-hoc signs it
after adding the assets). The `.app` and the Windows `.exe` are not notarized or code-signed: players see a one-time
Gatekeeper / SmartScreen prompt, which the packaged README.txt explains. Proper signing needs an Apple Developer ID
and a Windows code-signing certificate.
