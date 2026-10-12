#!/usr/bin/env python3
"""Extract the game's assets from a user-supplied Smash Hit APK into a plain folder.

    extract_assets.py <input.apk> <output_dir> [--no-suffix-strip]

The APK stores most assets under assets/ with an extra ".mp3" suffix (so that
Android does not compress them). This tool copies assets/* to <output_dir>/ and
strips that suffix, so e.g. assets/levels/basic.xml.mp3 -> <out>/levels/basic.xml.
Real mp3 files (none in current APKs) would be kept untouched if the remaining
name does not carry a known inner extension.

The desktop game can read either the APK itself or the extracted folder.
"""
import argparse
import os
import sys
import zipfile

INNER_EXT = {'xml', 'lua', 'glsl', 'png', 'ogg', 'obj', 'mesh', 'mtx', 'ufnt', 'fnt', 'mtl', 'jpg', 'json', 'txt'}


def target_name(rel, strip=True):
    if strip and rel.endswith('.mp3'):
        inner = rel[:-4]
        ext = inner.rsplit('.', 1)[-1].lower() if '.' in inner else ''
        if ext in INNER_EXT:
            return inner
    return rel


def extract(apk, out, strip=True):
    if not zipfile.is_zipfile(apk):
        sys.exit(f'{apk}: not a zip/APK file')
    z = zipfile.ZipFile(apk)
    out = os.path.abspath(out)
    count = 0
    total = 0
    for info in z.infolist():
        if info.is_dir() or not info.filename.startswith('assets/'):
            continue
        rel = info.filename[len('assets/'):]
        if rel.startswith('dexopt/'):
            continue  # Android baseline-profile files, not game data
        dst = os.path.abspath(os.path.join(out, target_name(rel, strip)))
        if not dst.startswith(out + os.sep):
            sys.exit(f'refusing path traversal: {info.filename}')
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with z.open(info) as src, open(dst, 'wb') as f:
            data = src.read()
            f.write(data)
        count += 1
        total += len(data)
    print(f'extracted {count} files ({total/1e6:.1f} MB) to {out}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('apk')
    ap.add_argument('out')
    ap.add_argument('--no-suffix-strip', action='store_true')
    a = ap.parse_args()
    extract(a.apk, a.out, not a.no_suffix_strip)


if __name__ == '__main__':
    main()
