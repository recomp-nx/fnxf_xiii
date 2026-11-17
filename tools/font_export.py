#!/usr/bin/env python3
"""fnxf_xiii (2026-10-25): the game's text fonts as one PNG per character and size, for translators drawing their
language's characters (a language pack, lang/<name>.zip: glyphs/glyphs.ini and its PNGs, described in
app/src/cpu/fnxf_xiii_glyphs.cpp).

    python tools/font_export.py <gui/resident/system.x360.xgr> <gui/resident/system.x360.imgb> <output folder>

Writes <output>/<size>/<code>_<character>.png (white on black, the glyph's box: its width x the font's line height)
for the Western page of each font (codes 0x20-0xFF, Windows-1252, as the game's texts use them: plain ASCII, and
0x85 + (code - 0x21) for 0xC0-0xFF, 0x85 + (code - 0x40) for 0xA0-0xBF), and <output>/fonts.txt with every glyph's
metrics (left bearing, width, right bearing) and place in its atlas, and <output>/glyphs.ini, an example of a pack's.

The fonts (system.x360.xgr, a WPD list): wfnt16 / wfnt18 / wfnt28, DXT1 atlases in the .imgb, tiled the Xbox 360 way,
with their tables wfnt16t / 18t / 28t (wfl): a 32-byte header (u32 little-endian: ?, size, line height, ?, number of
kerning pairs, ...), then 4 bytes per glyph of metrics (s8 left bearing, u8 width, s8 right bearing, 0) starting with
code 0x20, the kerning pairs (4 bytes each), the glyphs' places in the atlas (u16 x, u16 y), code 0x20 first: the set
the game draws; at the table's end the places of a second set (Japanese style, a yen sign for the backslash).
wfnt14t has no Western page.
"""
import os
import struct
import sys

FONTS = ['wfnt16', 'wfnt18', 'wfnt28']

EXAMPLE = """; glyphs/glyphs.ini of a language pack (lang/<name>.zip): one section per new character.
; Pictures: white on black (or on transparent), as the exported ones; width = the character's width, height at most
; the font's line height (16: 23, 18: 24, 28: 37 pixels). A size without a picture keeps the replaced character.
; Pick characters to replace that the language's texts never use (the game's texts then write the new one).

[\u0105]
; the game's character it takes the place of (or its code: 0xA4)
replaces = \u00a4
16 = a_ogonek_16.png
18 = a_ogonek_18.png
28 = a_ogonek_28.png
; extra space after it, in pixels (may be negative)
advance = 0
; moves it right, down in its box, in pixels
offset = 0, 0
"""


def wpd_entries(d):
    if d[:4] != b'WPD\0':
        raise ValueError('not a WPD file')
    n, = struct.unpack('>I', d[4:8])
    out = {}
    for i in range(n):
        e = d[16 + 32 * i:48 + 32 * i]
        name = e[:16].split(b'\0')[0].decode()
        off, size = struct.unpack('>II', e[16:24])
        out[name] = (off, size, e[24:32].split(b'\0')[0].decode())
    return out


def gtex(xgr, entry):
    off, size, ext = entry
    g = xgr[off + 64:off + 96]
    if g[:4] != b'GTEX':
        raise ValueError('no GTEX header')
    fmt = g[6]
    w, h = struct.unpack('>HH', g[10:14])
    data_off, data_size = struct.unpack('>II', g[24:32])
    return fmt, w, h, data_off, data_size


def tiled(x, y, w, pitch):
    aw = (w + 31) & ~31
    lb = (pitch >> 2) + ((pitch >> 1) >> (pitch >> 2))
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << lb
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4)
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) +
            (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> lb


def dxt1_gray(data, w, h):
    """The atlas as 8-bit luminance (the fonts are white on black)."""
    out = bytearray(w * h)
    bw = w // 4
    for by in range(h // 4):
        for bx in range(bw):
            o = tiled(bx, by, bw, 8) * 8
            blk = data[o:o + 8]
            blk = bytes(blk[i ^ 1] for i in range(8))  # 16-bit big-endian words
            c0, c1, bits = struct.unpack('<HHI', blk)

            def lum(c):
                r, g, b = ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31
                return (r * 299 + g * 587 + b * 114) // 1000
            l0, l1 = lum(c0), lum(c1)
            pal = [l0, l1, (2 * l0 + l1) // 3, (l0 + 2 * l1) // 3] if c0 > c1 else [l0, l1, (l0 + l1) // 2, 0]
            for i in range(16):
                out[(by * 4 + (i >> 2)) * w + bx * 4 + (i & 3)] = pal[(bits >> (2 * i)) & 3]
    return out


def cp1252(code):
    try:
        return bytes([code]).decode('cp1252')
    except UnicodeDecodeError:
        return ''


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    from PIL import Image
    xgr = open(sys.argv[1], 'rb').read()
    imgb = open(sys.argv[2], 'rb').read()
    out = sys.argv[3]
    ents = wpd_entries(xgr)
    report = []
    for font in FONTS:
        fmt, w, h, data_off, data_size = gtex(xgr, ents[font])
        if fmt != 24:
            raise ValueError('%s: format %d, not DXT1' % (font, fmt))
        atlas = dxt1_gray(imgb[data_off:data_off + data_size], w, h)
        toff, tsize, _ = ents[font + 't']
        t = xgr[toff:toff + tsize]
        header = struct.unpack('<8I', t[:32])
        size, line = header[1], header[2]
        folder = os.path.join(out, str(size))
        os.makedirs(folder, exist_ok=True)
        report.append('%s: size %d, line height %d, atlas %dx%d' % (font, size, line, w, h))
        for code in range(0x20, 0x100):
            k = code - 0x20
            lb, gw, rb, _ = struct.unpack('<bBbB', t[32 + 4 * k:36 + 4 * k])
            x, y = struct.unpack('<HH', t[32 + 4 * (224 + header[4] + k):36 + 4 * (224 + header[4] + k)])
            ch = cp1252(code)
            if not gw or not ch or (x, y) == (0, 0):
                continue
            img = Image.new('L', (gw, line))
            img.putdata([atlas[(y + j) * w + x + i] for j in range(line) for i in range(gw)])
            safe = ch if ch.isalnum() else ''
            img.save(os.path.join(folder, '%02X%s.png' % (code, '_' + safe if safe else '')))
            report.append('  %02X %s  bearing %d, width %d, bearing %d  at %d,%d' % (code, ch, lb, gw, rb, x, y))
    with open(os.path.join(out, 'fonts.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(report) + '\n')
    with open(os.path.join(out, 'glyphs.ini'), 'w', encoding='utf-8') as f:
        f.write(EXAMPLE)
    print('font_export: %s' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
