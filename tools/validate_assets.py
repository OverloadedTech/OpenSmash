#!/usr/bin/env python3
"""Parse every asset in an extracted asset folder and report failures.

    validate_assets.py <assets_dir>     (exit code 1 on any failure)

Checks: XML well-formedness, Lua syntax (Lua 5.2 via lupa, if installed),
segment .mesh layout, shader #ifdef structure, PNG/Ogg/JPEG signatures,
.mtx containers, .ufnt glyph tables, and that every room a level lists has a script.
"""
import os
import struct
import sys
import zlib
import xml.etree.ElementTree as ET
from collections import Counter

try:
    import lupa.lua52 as lupa
    LUA = lupa.LuaRuntime()
except Exception:
    LUA = None

fails = []
# levels shipped in the APK that reference rooms which do not exist (dev leftovers)
KNOWN_DANGLING = {('levels/trailer.xml', 'trailer')}
stats = Counter()


def fail(path, msg):
    fails.append((path, msg))


def read(p):
    with open(p, 'rb') as f:
        return f.read()


def parse_xml(rel, data):
    # no XML prolog, CR line endings, and a few files have more than one root element: retry inside a wrapper
    try:
        return ET.fromstring(data)
    except ET.ParseError:
        return ET.fromstring(b'<_root>' + data + b'</_root>')


def check_mesh(rel, data):
    u = zlib.decompress(data)
    n = struct.unpack_from('<I', u, 0)[0]
    off = 4 + 24 * n
    nt = struct.unpack_from('<I', u, off)[0]
    if off + 4 + 12 * nt != len(u):
        raise ValueError('size mismatch')
    if nt:
        idx = struct.unpack_from('<%dI' % (3 * nt), u, off + 4)
        if max(idx) >= n:
            raise ValueError('index out of range')


def check_shader(rel, data):
    t = data.decode('ascii')
    if '#ifdef VERTEX' not in t or '#ifdef FRAGMENT' not in t:
        raise ValueError('missing VERTEX/FRAGMENT sections')
    if t.count('#if') != t.count('#endif'):
        raise ValueError('unbalanced preprocessor')


def check_lua(rel, data):
    if LUA is None:
        return
    LUA.eval('function(s,n) local f,e=load(s,n); if not f then error(e) end end')(data.decode('utf-8', 'replace'), rel)


def check_mtx(rel, data):
    flag, size, zero = struct.unpack_from('<III', data, 0)
    if zero != 0 or 12 + size != len(data):
        raise ValueError('bad mtx header')
    if flag == 0:
        if data[12:15] != b'\xff\xd8\xff':
            raise ValueError('no jpeg')
    elif flag == 1:
        one, w, h, jl = struct.unpack_from('<4I', data, 12)
        if one != 1 or data[28:31] != b'\xff\xd8\xff':
            raise ValueError('bad mtx rgb+alpha header')
        al = struct.unpack_from('<I', data, 28 + jl)[0]
        if 32 + jl + al != len(data):
            raise ValueError('bad alpha length')
        if len(zlib.decompress(data[32 + jl:])) != w * h:
            raise ValueError('alpha size mismatch')
    else:
        raise ValueError('unknown mtx flag')


def check_ufnt(rel, data):
    # line height, baseline, glyph count N, then N "codepoint index" pairs and N rects of 7 numbers
    t = data.decode('ascii').split()
    n = int(t[2])
    if n <= 0 or len(t) < 3 + 9 * n:
        raise ValueError('glyph table truncated')
    for v in t[:2] + t[3:3 + 9 * n]:
        float(v)


def expect(cond, msg):
    if not cond:
        raise ValueError(msg)


def check_png(rel, data):
    expect(data[:8] == b'\x89PNG\r\n\x1a\n', 'bad png')


def check_ogg(rel, data):
    expect(data[:4] == b'OggS', 'bad ogg')


def check_obj(rel, data):
    expect(b'\nv ' in data, 'no vertices')


CHECKS = {'xml': parse_xml, 'lua': check_lua, 'mesh': check_mesh, 'glsl': check_shader, 'png': check_png,
          'ogg': check_ogg, 'mtx': check_mtx, 'ufnt': check_ufnt, 'obj': check_obj}


def main(root):
    for dp, _, fns in os.walk(root):
        for fn in fns:
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, root)
            ext = fn.rsplit('.', 1)[-1].lower()
            try:
                d = read(p)
                check = CHECKS.get(ext)
                if check:
                    check(rel, d)
                    stats[ext] += 1
                else:
                    stats['other:' + ext] += 1
            except Exception as e:
                fail(rel, f'{type(e).__name__}: {e}')

    # reference integrity: levels -> rooms (rooms/<type>.lua must exist)
    lv = os.path.join(root, 'levels')
    if os.path.isdir(lv):
        for fn in os.listdir(lv):
            if not fn.endswith('.xml'):
                continue
            try:
                for r in parse_xml('levels/' + fn, read(os.path.join(lv, fn))).iter('room'):
                    t = r.get('type')
                    if t and not os.path.exists(os.path.join(root, 'rooms', t + '.lua')) and ('levels/' + fn, t) not in KNOWN_DANGLING:
                        fail('levels/' + fn, f'missing room script {t}')
            except Exception as e:
                fail('levels/' + fn, str(e))

    print('parsed:', dict(stats))
    if LUA is None:
        print('note: lupa not installed, Lua syntax not checked')
    for p, m in fails:
        print('FAIL', p, m)
    print(f'{len(fails)} failure(s)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
