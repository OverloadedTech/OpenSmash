#!/usr/bin/env python3
"""CI helper: obtain the Smash Hit game assets and extract them into a folder.

    ci_fetch_assets.py <out_dir> [--require]

Sources, in order:
  1. env ASSETS_APK_URL      direct download link to the APK (a GitHub secret; never printed).
     env ASSETS_APK_TOKEN    optional bearer token for that URL (e.g. a private GitHub release asset API URL).
     env ASSETS_APK_SHA256   optional checksum the download must match (the 64-hex hash; a pasted
                             `sha256sum` output line is accepted too).
  2. game-assets/ at the repository root, for local runs only (it is git-ignored and never committed): an extracted
     asset tree (tools/extract_assets.py output, with game.xml). An .apk is deliberately not accepted there: it
     contains the original game's program code.

Writes `have=true|false` and `dir=<out_dir>` to $GITHUB_OUTPUT (when set). Exits non-zero if --require is given and
no source is configured, or if a configured source fails (a broken download never silently produces an asset-less
release).
"""
import hashlib
import os
import re
import shutil
import sys
import tempfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import extract_assets  # noqa: E402
import make_release  # noqa: E402


def output(**kv):
    path = os.environ.get('GITHUB_OUTPUT')
    if path:
        with open(path, 'a', encoding='utf-8') as f:
            for k, v in kv.items():
                f.write(f'{k}={v}\n')


def download(url, token, dst):
    req = urllib.request.Request(url, headers={'User-Agent': 'opensmash-ci'})
    if token:   # not copied to redirect targets (e.g. a pre-signed storage URL on another host)
        req.add_unredirected_header('Authorization', f'Bearer {token}')
        if 'api.github.com' in url:
            req.add_header('Accept', 'application/octet-stream')
    h = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=300) as r, open(dst, 'wb') as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            f.write(chunk)
    return h.hexdigest()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = os.path.abspath(sys.argv[1])
    require = '--require' in sys.argv[2:]
    if os.path.exists(out):
        shutil.rmtree(out)

    url = os.environ.get('ASSETS_APK_URL', '').strip()
    local = os.path.join(ROOT, 'game-assets')
    if url:
        with tempfile.TemporaryDirectory() as tmp:
            apk = os.path.join(tmp, 'game.apk')
            try:
                digest = download(url, os.environ.get('ASSETS_APK_TOKEN', '').strip(), apk)
            except Exception as e:  # the URL is a secret: report the failure without echoing it
                sys.exit(f'::error::downloading the APK from ASSETS_APK_URL failed: {type(e).__name__}')
            raw = os.environ.get('ASSETS_APK_SHA256', '').strip()
            m = re.search(r'\b[0-9a-fA-F]{64}\b', raw)
            if raw and not m:
                sys.exit('::error::ASSETS_APK_SHA256 is not a SHA-256 hash (expected 64 hex characters)')
            want = m.group(0).lower() if m else ''
            if want and digest != want:
                sys.exit(f'::error::APK checksum mismatch: got {digest}, ASSETS_APK_SHA256 expects {want}')
            print(f'downloaded APK, sha256 {digest}')
            extract_assets.extract(apk, out)
    elif os.path.isdir(local):
        code = make_release.program_code(local)
        if code:
            sys.exit('::error::game-assets/ contains original program code (' + ', '.join(code[:5]) +
                     '); use only the output of tools/extract_assets.py, never the .apk')
        if make_release.has_game_xml(local):
            shutil.copytree(local, out)
        else:
            sys.exit('::error::game-assets/ is not an extracted asset tree (no game.xml); use the output of '
                     'tools/extract_assets.py, not the .apk')
    else:
        msg = 'no game assets configured (set the ASSETS_APK_URL secret)'
        if require:
            sys.exit(f'::error::{msg}')
        print(f'::notice::{msg}; packages will ask players for their own APK')
        output(have='false', dir='')
        return

    if not make_release.has_game_xml(out):
        sys.exit('::error::extracted assets have no game.xml - is this a Smash Hit APK?')
    print(f'game assets ready in {out}')
    output(have='true', dir=out)


if __name__ == '__main__':
    main()
