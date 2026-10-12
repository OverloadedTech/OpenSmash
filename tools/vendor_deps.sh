#!/usr/bin/env bash
# Download the exact third-party versions OpenSmash builds against into third_party/, so the source tree builds
# offline (CMake uses third_party/<name> when it exists, otherwise downloads at configure time).
# All four libraries are permissively licensed (see THIRD_PARTY_NOTICES.md); their license files are kept.
# Usage: tools/vendor_deps.sh [--force]
set -euo pipefail
cd "$(dirname "$0")/.."
TP=third_party
[ "${1:-}" = "--force" ] && rm -rf "$TP"
mkdir -p "$TP"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

fetch() { # name url sha256
  local name=$1 url=$2 sha=$3
  if [ -d "$TP/$name" ]; then echo "= $name (present)"; return; fi
  echo "+ $name <- $url"
  local f="$tmp/$name.archive"
  curl -fsSL --retry 3 -o "$f" "$url"
  if [ -n "$sha" ]; then echo "$sha  $f" | sha256sum -c --quiet -; fi
  mkdir -p "$tmp/x-$name"
  case "$url" in
    *.zip) unzip -q "$f" -d "$tmp/x-$name" ;;
    *)     tar -xzf "$f" -C "$tmp/x-$name" ;;
  esac
  # archives with a single top-level directory are unwrapped
  local entries=("$tmp/x-$name"/*)
  if [ ${#entries[@]} -eq 1 ] && [ -d "${entries[0]}" ]; then mv "${entries[0]}" "$TP/$name"; else mv "$tmp/x-$name" "$TP/$name"; fi
}

fetch lua52 https://www.lua.org/ftp/lua-5.2.4.tar.gz b9e2e4aad6789b3b63a056d442f7b39f0ecfca3ae0f1fc0ae4e9614401b69f4b
cmake -DLUA_DIR="$TP/lua52" -P cmake/patch_lua52.cmake   # upstream parser fix (idempotent)
fetch miniz https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
fetch stb   https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz 9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
fetch SDL3  https://github.com/libsdl-org/SDL/releases/download/release-3.2.22/SDL3-3.2.22.tar.gz f29d00cbcee273c0a54f3f32f86bf5c595e8823a96b1d92a145aac40571ebfcc

# drop parts the build never uses (tests, examples, IDE projects); license files stay
rm -rf "$TP/SDL3/test" "$TP/SDL3/examples" "$TP/SDL3/Xcode" "$TP/SDL3/VisualC" "$TP/SDL3/VisualC-GDK" \
       "$TP/stb/tests" "$TP/stb/data"

cat > "$TP/README.md" <<'EOF'
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
EOF
du -sh "$TP"/* | sort -h
