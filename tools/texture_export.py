#!/usr/bin/env python3
"""fnxf_xiii (2026-10-25): the game's textures as PNGs, for a language pack's translated pictures (its images/ folder,
app/src/cpu/fnxf_xiii_images.cpp).

    python tools/texture_export.py <.xgr / .xfv, or a folder searched for them> <output folder>

Each texture of each file (its .imgb next to it) becomes <output>/<file>/<texture>.png, and <output>/images.ini
lists them, one line a picture:

    tutorial020_us/snap_p02_0.png = 7F3A0C11D2E4B6A8 DXT1 512x288

the texture's original data (XXH3 of its pixels as in the .imgb: the runtime finds the texture by it), its format and
size. A pack's images/ holds the PNGs it changes and those lines of images.ini (the PNG the same size).

The files: a WPD list (xgr: txbh entries; xfv: vtex entries) whose texture headers ("GTEX") give the format (24 DXT1,
26 DXT5, 4 A8R8G8B8), the size, tiled (Xbox 360 tiling) or linear, and the data's place in the .imgb; rows and
columns padded to 32 blocks, 16-bit big-endian words (32-bit for A8R8G8B8).
"""
import os
import struct
import sys

FORMATS = {24: ('DXT1', 8), 26: ('DXT5', 16), 4: ('A8R8G8B8', 4)}


def wpd_entries(d):
    n, = struct.unpack('>I', d[4:8])
    out = []
    for i in range(n):
        e = d[16 + 32 * i:48 + 32 * i]
        off, size = struct.unpack('>II', e[16:24])
        out.append((e[:16].split(b'\0')[0].decode(), off, size, e[24:32].split(b'\0')[0].decode()))
    return out


def textures(d):
    """(name, format, width, height, tiled, data offset, data size) of each texture of a WPD file."""
    out = []
    if d[:4] != b'WPD\0':
        return out
    for name, off, size, ext in wpd_entries(d):
        if ext not in ('txbh', 'vtex'):
            continue
        g0 = d.find(b'GTEX', off, off + size)
        if g0 < 0:
            continue
        g = d[g0:g0 + 32]
        w, h = struct.unpack('>HH', g[10:14])
        data_off, data_size = struct.unpack('>II', g[24:32])
        out.append((name, g[6], w, h, bool(g[9] & 4), data_off, data_size))
    return out


def tiled_offset(x, y, width, lb):
    aw = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb


def unit_count(fmt, w, h):
    """Blocks (DXT) or pixels (A8R8G8B8) across and down."""
    return (w, h) if fmt == 4 else ((w + 3) // 4, (h + 3) // 4)


def hash_size(fmt, w, h):
    """The bytes the runtime hashes: the unpadded size, from the data's start."""
    bw, bh = unit_count(fmt, w, h)
    return bw * bh * FORMATS[fmt][1]


def units(data, fmt, w, h, tiled, big_endian=True):
    """The texture's blocks (or pixels) in row order, little-endian (as a PC reads them)."""
    bw, bh = unit_count(fmt, w, h)
    bs = FORMATS[fmt][1]
    lb = {4: 2, 8: 3, 16: 4}[bs]
    pitch = (bw + 31) & ~31
    out = []
    for y in range(bh):
        for x in range(bw):
            i = tiled_offset(x, y, bw, lb) if tiled else y * (pitch if big_endian else bw) + x
            u = data[i * bs:i * bs + bs]
            if big_endian:
                if fmt == 4:
                    u = u[::-1]  # ARGB big-endian -> BGRA
                else:
                    u = bytes(u[k ^ 1] for k in range(bs))
            out.append(u)
    return out


def _rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def to_rgba(units_, fmt, w, h):
    """RGBA bytes (w x h)."""
    out = bytearray(w * h * 4)
    if fmt == 4:
        bw = w
        for i, u in enumerate(units_):
            x, y = i % bw, i // bw
            b, g, r, a = u
            o = (y * w + x) * 4
            out[o:o + 4] = bytes((r, g, b, a))
        return bytes(out)
    bw = (w + 3) // 4
    for i, blk in enumerate(units_):
        bx, by = i % bw, i // bw
        alpha = None
        if fmt == 26:
            a0, a1 = blk[0], blk[1]
            bits = int.from_bytes(blk[2:8], 'little')
            if a0 > a1:
                al = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
            else:
                al = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
            alpha = [al[(bits >> (3 * k)) & 7] for k in range(16)]
            blk = blk[8:]
        c0, c1, bits = struct.unpack('<HHI', blk[:8])
        p0, p1 = _rgb565(c0), _rgb565(c1)
        if c0 > c1 or fmt == 26:
            pal = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
        else:
            pal = [p0, p1, tuple((a + b) // 2 for a, b in zip(p0, p1)), (0, 0, 0)]
        for k in range(16):
            x, y = bx * 4 + (k & 3), by * 4 + (k >> 2)
            if x >= w or y >= h:
                continue
            idx = (bits >> (2 * k)) & 3
            a = alpha[k] if alpha else (0 if fmt == 24 and c0 <= c1 and idx == 3 else 255)
            o = (y * w + x) * 4
            out[o:o + 4] = bytes(pal[idx] + (a,))
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    import xxhash
    from PIL import Image
    src, out = sys.argv[1:]
    files = [src] if os.path.isfile(src) else sorted(
        os.path.join(r, f) for r, _, fs in os.walk(src) for f in fs if f.endswith(('.xgr', '.xfv')))
    lines = []
    for path in files:
        d = open(path, 'rb').read()
        imgb_path = path.rsplit('.', 1)[0] + '.imgb'
        if not os.path.exists(imgb_path):
            continue
        imgb = open(imgb_path, 'rb').read()
        stem = os.path.basename(path).split('.')[0]
        if stem == 'veffs':  # vfx: named after its folder
            stem = os.path.basename(os.path.dirname(path))
        for name, fmt, w, h, tiled, off, size in textures(d):
            if fmt not in FORMATS:
                continue
            data = imgb[off:off + size]
            rgba = to_rgba(units(data, fmt, w, h, tiled), fmt, w, h)
            rel = '%s/%s.png' % (stem, name)
            os.makedirs(os.path.join(out, stem), exist_ok=True)
            Image.frombytes('RGBA', (w, h), rgba).save(os.path.join(out, rel))
            digest = xxhash.xxh3_64_intdigest(imgb[off:off + hash_size(fmt, w, h)])
            lines.append('%s = %016X %s %dx%d' % (rel, digest, FORMATS[fmt][0], w, h))
    with open(os.path.join(out, 'images.ini'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('; picture = the original texture (XXH3 of its data) format size\n' + '\n'.join(lines) + '\n')
    print('texture_export: %d textures -> %s' % (len(lines), out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
