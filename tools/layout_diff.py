#!/usr/bin/env python3
"""fnxf_xiii (2026-10-25): a translation's changes to the game's UI layouts (.ykd entries of a WPD file such as
gui/resident/system.x360.xgr) as a language pack's layouts/layouts.ini (app/src/cpu/fnxf_xiii_layouts.cpp).

    python tools/layout_diff.py <the game's .xgr> <the translation's .xgr> [layout names to leave out ...]

Prints, for each layout the two files both have, of the same size, that differs:

    [gr_btlfont05]
    original = 5D0E6C1F2A3B4C5D                  the game's layout (XXH3): only that one is changed
    sprite 0x2AC0 = btl_number01 590 45 50 37 50 37
    colours 0x2AA0 = 808080FF 202020FF 808080FF 202020FF
    u32 0x0788 = 7
    float 0x0690 = 144

"sprite": a sprite record, at the offset of its texture's name: the texture, then x, y, width, height of its part of
the texture and the drawn width, height (pixels). "colours": a sprite's four corner colours (RGBA, as stored).
"u32" / "float": any other changed word (little-endian, as the layouts are). Offsets in the layout.
"""
import struct
import sys

import xxhash


def wpd(d):
    n, = struct.unpack('>I', d[4:8])
    out = {}
    for i in range(n):
        e = d[16 + 32 * i:48 + 32 * i]
        off, size = struct.unpack('>II', e[16:24])
        out[(e[:16].split(b'\0')[0].decode(), e[24:32].split(b'\0')[0].decode())] = d[off:off + size]
    return out


def floaty(v):
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    return f if v > 0x10000000 and 1e-3 < abs(f) < 1e6 else None


def diff(a, b, textures):
    lines, done = [], set()
    # Sprite records: a texture's name (16 bytes) then 8 words; the four corner colours 32 bytes before the name.
    for name in sorted(textures):
        key = name.encode().ljust(16, b'\0')
        i = a.find(key)
        while i >= 0:
            if a[i:i + 48] != b[i:i + 48] and i + 48 <= len(a):
                nb = b[i:i + 16].split(b'\0')[0].decode()
                vals = struct.unpack('<6I', b[i + 16:i + 40])
                lines.append('sprite 0x%04X = %s %s' % (i, nb, ' '.join(str(v) for v in vals)))
                done.update(range(i, i + 40))
            c = i - 32
            if c >= 0 and a[c:c + 16] != b[c:c + 16]:
                lines.append('colours 0x%04X = %s' % (c, ' '.join(b[c + 4 * k:c + 4 * k + 4].hex().upper() for k in range(4))))
                done.update(range(c, c + 16))
            i = a.find(key, i + 16)
    for o in range(0, len(a) - 3, 4):
        if a[o:o + 4] == b[o:o + 4] or all(k in done for k in range(o, o + 4)):
            continue
        old, new = struct.unpack('<I', a[o:o + 4])[0], struct.unpack('<I', b[o:o + 4])[0]
        fo, fn = floaty(old), floaty(new)
        if fo is not None and fn is not None:
            lines.append('float 0x%04X = %g' % (o, fn))
        else:
            lines.append(('u32 0x%04X = 0x%08X' if new > 0xFFFF else 'u32 0x%04X = %d') % (o, new))
    return lines


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a_all, b_all = wpd(open(sys.argv[1], 'rb').read()), wpd(open(sys.argv[2], 'rb').read())
    skip = set(sys.argv[3:])
    textures = {n for (n, x) in a_all if x == 'txbh'}
    for (name, ext), a in a_all.items():
        b = b_all.get((name, ext))
        if ext != 'ykd' or name in skip or b is None or len(a) != len(b) or a == b:
            continue
        print('[%s]' % name)
        print('original = %016X' % xxhash.xxh3_64_intdigest(a))
        for line in diff(a, b, textures):
            print(line)
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
