#!/usr/bin/env python3
"""Package an OpenSmash build into a player-ready archive (runs on Windows, macOS and Linux; used by CI).

    make_release.py --platform windows-x64|linux-x64|linux-arm64|macos-arm64 --binary PATH --version X.Y.Z
                    [--assets DIR | --apk FILE] [--out dist]

--binary   the built game: opensmash.exe, the opensmash ELF, or the OpenSmash.app bundle
--assets   an extracted asset folder (tools/extract_assets.py output) to bundle; --apk extracts one on the fly.
           Without either, players must supply their own APK (the game finds it next to the executable).

Output: <out>/OpenSmash-<version>-<platform>.zip (Windows, macOS) or .tar.gz (Linux). Only the assets/ tree of an
APK is ever bundled - never the original native libraries or Java code.
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import extract_assets  # noqa: E402

PLATFORMS = ('windows-x64', 'linux-x64', 'linux-arm64', 'macos-arm64')


def readme(version, platform, with_assets):
    start = {'windows-x64': 'double-click OpenSmash.exe\n'
                            '  First launch: if Windows SmartScreen says "Windows protected your PC", click\n'
                            '  "More info" > "Run anyway" (the exe is not code-signed).',
             'macos-arm64': 'open OpenSmash.app\n'
                            '  First launch (the app is not notarized): macOS 15 and later block it once; then go to\n'
                            '  System Settings > Privacy & Security and click "Open Anyway". On macOS 12-14,\n'
                            '  right-click OpenSmash.app > Open. Or in Terminal: xattr -dr com.apple.quarantine OpenSmash.app',
             }.get(platform, 'run ./opensmash (from a terminal or your file manager)')
    saves = {'windows-x64': '%APPDATA%\\OpenSmash',
             'macos-arm64': '~/Library/Application Support/OpenSmash',
             }.get(platform, '~/.local/share/OpenSmash')
    if with_assets:
        how = f"""This package includes the Smash Hit game assets (see ASSETS_NOTICE.txt).

How to play
  Start the game: {start}"""
    else:
        how = f"""This package contains NO game content: you need a copy of the Smash Hit Android APK.

How to play
  1. Put your Smash Hit .apk file next to the game (any name ending in .apk), or
     just start the game and pick the APK in the file dialog (it is remembered).
  2. Start the game: {start}"""
        if platform == 'macos-arm64':
            how = f"""This package contains NO game content: you need a copy of the Smash Hit Android APK.

How to play
  1. Start the game: {start}
  2. Pick your Smash Hit .apk in the file dialog (it is remembered).
     An .apk placed next to OpenSmash.app is found too, but only once the app is no longer
     quarantined (moved with Finder, or after the xattr command above): a freshly downloaded
     app runs from a temporary read-only location and cannot see files next to it."""
    return f"""OpenSmash {version} - open-source engine for Smash Hit

{how}

Controls
  Left mouse / touch   throw a ball / press buttons
  Space                throw at the crosshair (keyboard / gamepad aiming)
  Gamepad              sticks aim, A or right trigger throws, B back, Start pauses;
                       a second gamepad is player 2 in versus and co-op
  Esc                  back / pause
  F11 or Alt+Enter     fullscreen

Saves and settings are kept in {saves}.
Run with --user-dir <folder> to keep them next to the game instead (portable install).

OpenSmash is an unofficial fan project, not affiliated with Mediocre AB.
"Smash Hit" is a trademark of Mediocre AB. OpenSmash's own code is MIT licensed
(LICENSE.txt); bundled libraries are listed in THIRD_PARTY_NOTICES.md, their
license texts are in licenses/.
"""


ASSETS_NOTICE = """Smash Hit game assets

The {where} folder of this package contains game content from Smash Hit (levels, scripts, shaders, art, music
and sound), (c) Mediocre AB. It is distributed with Mediocre AB's permission and is NOT covered by OpenSmash's
MIT license: you may not reuse or redistribute it separately from this package.

Only the game's data files are included. The original game's program code (native libraries, Android code) is not.
"""


def text(s, windows):
    return s.replace('\n', '\r\n') if windows else s


PROGRAM_CODE = ('.apk', '.so', '.dex', '.odex', '.vdex', '.oat', '.jar')


def program_code(path):
    """Files of the original game's program code (APK, native libraries, Dalvik code) below path."""
    return [os.path.relpath(os.path.join(dp, f), path) for dp, _, fs in os.walk(path) for f in fs
            if f.lower().endswith(PROGRAM_CODE) or f == 'AndroidManifest.xml']


def has_game_xml(path):
    return any(os.path.isfile(os.path.join(path, n)) for n in ('game.xml', 'game.xml.mp3'))


def check_assets(path):
    if not has_game_xml(path):
        sys.exit(f'{path}: not an extracted Smash Hit asset folder (no game.xml)')
    code = program_code(path)
    if code:   # packages promise to ship game data only, never the original program code
        sys.exit(f'::error::{path} contains program code, not just game data: {", ".join(code[:5])}; '
                 'package only the output of tools/extract_assets.py')


def dynamic_sdl_path(binary):
    """Linux: path of the libSDL3.so the binary links dynamically (local builds against a system SDL3), else None."""
    try:
        out = subprocess.run(['ldd', binary], capture_output=True, text=True, check=False).stdout
    except OSError:
        return None
    for line in out.splitlines():
        if 'libSDL3.so' in line and '=>' in line:
            return line.split('=>')[1].split('(')[0].strip() or None
    return None


def finish_macos_app(app):
    """Refuse non-system dylibs (e.g. a Homebrew SDL3) and ad-hoc sign the finished bundle, assets included.

    Without a bundle signature, a downloaded (quarantined) arm64 app is reported as "damaged" by Gatekeeper with no
    way to open it; with an ad-hoc signature the user gets the normal "Open Anyway" path. Must run on macOS."""
    if sys.platform != 'darwin':
        sys.exit('macOS packages must be built on macOS (the .app needs otool and codesign)')
    exe = os.path.join(app, 'Contents', 'MacOS', 'OpenSmash')
    deps = subprocess.run(['otool', '-L', exe], capture_output=True, text=True, check=True).stdout.splitlines()[1:]
    bad = [d.strip().split(' (')[0] for d in deps
           if d.strip() and not d.strip().startswith(('/System/Library/', '/usr/lib/'))]
    if bad:
        sys.exit('macOS: the executable links non-system libraries (configure with -DSH_SYSTEM_SDL3=OFF): ' + ', '.join(bad))
    subprocess.run(['xattr', '-cr', app], check=True)
    subprocess.run(['codesign', '--force', '--deep', '--sign', '-', '--timestamp=none', app], check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', '--verbose=2', app], check=True)


def write_licenses(top, win):
    """Full license texts of the libraries linked into the binary (their licenses require shipping them)."""
    lic = os.path.join(top, 'licenses')
    os.makedirs(lic)
    tp = os.path.join(ROOT, 'third_party')
    files = {'SDL3.txt': os.path.join(tp, 'SDL3', 'LICENSE.txt'),
             'miniz.txt': os.path.join(tp, 'miniz', 'LICENSE'),
             'stb.txt': os.path.join(tp, 'stb', 'LICENSE'),
             'SDL3-yuv2rgb.txt': os.path.join(tp, 'SDL3', 'src', 'video', 'yuv2rgb', 'LICENSE')}
    for dst, src in files.items():
        with open(src, encoding='utf-8') as fi, open(os.path.join(lic, dst), 'w', newline='', encoding='utf-8') as fo:
            fo.write(text(fi.read(), win))
    with open(os.path.join(tp, 'lua52', 'src', 'lua.h'), encoding='utf-8') as f:   # Lua keeps its license in lua.h
        h = f.read()
    start = h.index('* Copyright (C) 1994')          # the license comment block at the end of lua.h
    body = h[start:h.index('*****', start)]
    lines = [l.strip()[1:].strip() if l.strip().startswith('*') else l.strip() for l in body.splitlines()]
    lua = 'Lua 5.2.4\n\n' + '\n'.join(lines).strip() + '\n'
    with open(os.path.join(lic, 'Lua.txt'), 'w', newline='', encoding='utf-8') as fo:
        fo.write(text(lua, win))


def stage(args, staging):
    """Lay out the package tree under staging/<name>/ and return that directory."""
    win = args.platform.startswith('windows')
    name = f'OpenSmash-{args.version}-{args.platform}'
    top = os.path.join(staging, name)
    os.makedirs(top)
    if args.platform == 'macos-arm64':
        if not args.binary.rstrip('/').endswith('.app') or not os.path.isdir(args.binary):
            sys.exit('macOS: --binary must be the OpenSmash.app bundle')
        app = os.path.join(top, 'OpenSmash.app')
        shutil.copytree(args.binary, app, symlinks=True)
        asset_dst = os.path.join(app, 'Contents', 'Resources', 'assets')
    elif win:
        shutil.copy2(args.binary, os.path.join(top, 'OpenSmash.exe'))
        asset_dst = os.path.join(top, 'assets')
    else:
        lib = dynamic_sdl_path(args.binary)
        if lib:   # dynamic SDL3: binary + library in lib/, started through a launcher script
            os.makedirs(os.path.join(top, 'lib'))
            shutil.copy2(args.binary, os.path.join(top, 'lib', 'opensmash'))
            shutil.copy2(os.path.realpath(lib), os.path.join(top, 'lib', 'libSDL3.so.0'))
            launcher = os.path.join(top, 'opensmash')
            with open(launcher, 'w', newline='\n', encoding='utf-8') as f:
                f.write('#!/bin/sh\nhere=$(dirname "$(readlink -f "$0")")\n'
                        '# OPENSMASH_HOME: the game looks for assets/ and *.apk here (its own folder is lib/)\n'
                        'OPENSMASH_HOME="$here" LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" '
                        'exec "$here/lib/opensmash" "$@"\n')
            os.chmod(launcher, 0o755)
        else:
            shutil.copy2(args.binary, os.path.join(top, 'opensmash'))
            os.chmod(os.path.join(top, 'opensmash'), 0o755)
        asset_dst = os.path.join(top, 'assets')

    with_assets = bool(args.assets or args.apk)
    if args.apk:
        extract_assets.extract(args.apk, asset_dst)
    elif args.assets:
        check_assets(args.assets)
        shutil.copytree(args.assets, asset_dst)
    if with_assets:
        check_assets(asset_dst)
        with open(os.path.join(top, 'ASSETS_NOTICE.txt'), 'w', newline='', encoding='utf-8') as f:
            where = 'OpenSmash.app/Contents/Resources/assets/' if args.platform == 'macos-arm64' else 'assets/'
            f.write(text(ASSETS_NOTICE.format(where=where), win))
    if args.platform == 'macos-arm64':
        finish_macos_app(app)

    with open(os.path.join(top, 'README.txt'), 'w', newline='', encoding='utf-8') as f:
        f.write(text(readme(args.version, args.platform, with_assets), win))
    for src, dst in (('LICENSE', 'LICENSE.txt'), ('THIRD_PARTY_NOTICES.md', 'THIRD_PARTY_NOTICES.md')):
        with open(os.path.join(ROOT, src), encoding='utf-8') as fi, open(os.path.join(top, dst), 'w', newline='', encoding='utf-8') as fo:
            fo.write(text(fi.read(), win))
    write_licenses(top, win)
    return top


def archive(top, out_dir, platform):
    base = os.path.basename(top)
    parent = os.path.dirname(top)
    if platform.startswith('linux'):
        path = os.path.join(out_dir, base + '.tar.gz')
        with tarfile.open(path, 'w:gz') as t:
            t.add(top, arcname=base)
        return path
    path = os.path.join(out_dir, base + '.zip')
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dirpath, dirnames, filenames in os.walk(top):
            dirnames.sort()
            for fn in sorted(filenames):
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, parent).replace(os.sep, '/')
                st = os.lstat(full)
                info = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.create_system = 3  # unix: keep permission bits (macOS executables)
                if stat.S_ISLNK(st.st_mode):
                    info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    z.writestr(info, os.readlink(full))
                    continue
                mode = 0o755 if (st.st_mode & 0o111 or fn.endswith('.exe')) else 0o644
                info.external_attr = (stat.S_IFREG | mode) << 16
                with open(full, 'rb') as f:
                    z.writestr(info, f.read())
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--platform', required=True, choices=PLATFORMS)
    ap.add_argument('--binary', required=True)
    ap.add_argument('--version', required=True)
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--assets')
    g.add_argument('--apk')
    ap.add_argument('--out', default=os.path.join(ROOT, 'dist'))
    args = ap.parse_args()
    args.version = args.version[1:] if args.version.startswith('v') else args.version
    if not os.path.exists(args.binary):
        sys.exit(f'{args.binary}: not found')
    os.makedirs(args.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as staging:
        top = stage(args, staging)
        path = archive(top, args.out, args.platform)
    print(path)


if __name__ == '__main__':
    main()
