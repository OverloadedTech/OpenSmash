#!/usr/bin/env python3
"""Print GitHub release notes for a version: its CHANGELOG.md section plus download instructions.

    release_notes.py <version>        (e.g. 0.1.0 or 0.2.0-rc1)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def absolutize(md, version):
    """Relative links (fine in CHANGELOG.md) would resolve against the release page: point them at the tagged tree."""
    repo = os.environ.get('GITHUB_REPOSITORY')
    if not repo:
        return md
    base = f"{os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}/{repo}/blob/v{version}/"
    return re.sub(r'\]\((?![a-zA-Z][a-zA-Z0-9+.-]*:|#|/)([^)\s]+)\)', lambda m: f']({base}{m.group(1)})', md)


def changelog_section(version):
    with open(os.path.join(ROOT, 'CHANGELOG.md'), encoding='utf-8') as f:
        text = f.read()
    base = version.split('-')[0]
    for v in (version, base):
        m = re.search(rf'^## {re.escape(v)}\b.*?$\n(.*?)(?=^## |\Z)', text, re.M | re.S)
        if m:
            return absolutize(m.group(1).strip(), version)
    return 'See CHANGELOG.md for details.'


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    v = sys.argv[1]
    print(f"""{changelog_section(v)}

### Downloads

| Platform | File | How to start |
|---|---|---|
| Windows 10 (1903+) / 11 x64 | `OpenSmash-{v}-windows-x64.zip` | unzip, double-click `OpenSmash.exe` (first launch: SmartScreen > More info > Run anyway; not code-signed) |
| Linux x64 | `OpenSmash-{v}-linux-x64.tar.gz` | extract, run `./opensmash` |
| Linux ARM64 | `OpenSmash-{v}-linux-arm64.tar.gz` | extract, run `./opensmash` |
| macOS 12+ (Apple Silicon) | `OpenSmash-{v}-macos-arm64.zip` | unzip, open `OpenSmash.app`; first launch (not notarized): System Settings > Privacy & Security > Open Anyway (macOS 15+), right-click > Open on 12-14 |

Every package is complete: the game assets are included (see `ASSETS_NOTICE.txt`), nothing else to install.
Linux builds need glibc 2.39 or newer (Ubuntu 24.04+, Debian 13+, Fedora 40+). `SHA256SUMS.txt` lists checksums.

OpenSmash is an unofficial fan project, not affiliated with Mediocre AB. Smash Hit and its assets are (c) Mediocre AB,
included with permission; "Smash Hit" is a trademark of Mediocre AB. OpenSmash's own code is MIT licensed.""")


if __name__ == '__main__':
    main()
