#!/usr/bin/env python3
"""fnxf_xiii (2026-10-25): the game's text files (.ztr) as a language pack's text files, for translators.

    python tools/ztr_to_texts.py <a .ztr, or a folder searched for them> <output folder> [--glyphs <glyphs.ini>]

Each <name>.ztr (or txtres_<language>.ztr: named after its folder) becomes <output>/<its path>.txt, one line a text:

    $key = text

Reads the game's own files (header u64 1: compressed pages) and the uncompressed ones of the community tools
(u32 little-endian count, then per text the offsets of its key and of its text).

The texts' encoding (what the game's font draws): ASCII as is; the characters 0x80-0xFF of Windows-1252 as two bytes,
0x85 and X (X = code - 0x40 for 0x80-0xBE, 0x80 for 0xBF, code - 0x21 for 0xC0-0xFF); other two-byte codes are the
game's own (0xF0-0xF9 X: variables, button icons, colours; 0x81 X: full-width symbols); "@r" is a new line. In the
text files those characters are themselves (UTF-8), and anything else is written as its bytes in hex in braces:
{F1 40} (the A button), {00 01} (a NUL inside a text: names, plurals), {7B} and {7D} for the braces themselves.
The pack's runtime (app/src/cpu/fnxf_xiii_texts.cpp) writes them back the same way.

--glyphs: a language pack's glyphs/glyphs.ini (app/src/cpu/fnxf_xiii_glyphs.cpp): the codes its new characters replace
are written as the new characters (a Polish translation's Ê is its Ł).

A key in several files (an event's text copied into another's file, a zone's file in two archives) is written once:
into the file whose text is not empty, then the one whose path names it ($efark040_... -> ev_fark_040, a zone's
$m_z006... -> z006 in white_z006u), then the first. The runtime has one text a key.
"""
import os
import re
import struct
import sys


def dictof(d, s):
    n, = struct.unpack('>I', d[s:s + 4])
    dic = {}
    for j in range(n // 3):
        k, a, b = d[s + 4 + 3 * j:s + 7 + 3 * j]
        dic[k] = (a, b)
    return dic, s + 4 + n


def expand(dic, c, depth=0):
    if c in dic and depth < 64:
        a, b = dic[c]
        return expand(dic, a, depth + 1) + expand(dic, b, depth + 1)
    return bytes([c])


def load_game(d):
    """The game's .ztr: keys and texts in order."""
    _, lines, ids_size, chunks = struct.unpack('>QIII', d[:20])
    offs = struct.unpack('>%dI' % chunks, d[20:20 + 4 * chunks])
    p = 20 + 4 * chunks + lines * 4
    ids = bytearray()
    while len(ids) < ids_size:
        dic, p = dictof(d, p)
        page = bytearray()
        while len(page) < 4096 and len(ids) + len(page) < ids_size:
            page += expand(dic, d[p])
            p += 1
        ids += page
    keys = bytes(ids).split(b'\0')[:lines]
    bounds = list(offs) + [len(d) - p]
    text = bytearray()
    for i in range(chunks):
        s, e = p + bounds[i], p + bounds[i + 1]
        if s < e:
            dic, q = dictof(d, s)
            text += b''.join(expand(dic, c) for c in d[q:e])
    texts = bytes(text).split(b'\0\0')[:lines]
    return list(zip(keys, texts))


def load_raw(d):
    """A community tool's uncompressed .ztr."""
    n, = struct.unpack('<I', d[:4])
    # fnxf_xiii (2026-11-01): the count from where the first key starts when the header's is wrong (the CosmoFF release's
    # ev_bodm_345 says 0 for its 23 texts: the cutscene stayed untranslated).
    if len(d) >= 8:
        first, = struct.unpack('<I', d[4:8])
        if first >= 4 and (first - 4) % 8 == 0 and (first - 4) // 8 != n:
            n = (first - 4) // 8
    v = struct.unpack('<%dI' % (2 * n), d[4:4 + 8 * n])
    starts = sorted(set(v)) + [len(d)]
    out = []
    for i in range(n):
        a, b = v[2 * i], v[2 * i + 1]
        end = starts[starts.index(b) + 1]  # the next key or text (texts end with one or two NULs)
        out.append((d[a:d.index(b'\0', a)], d[b:end].rstrip(b'\0')))
    return out


def load(path):
    d = open(path, 'rb').read()
    return load_game(d) if d[:8] == b'\0\0\0\0\0\0\0\x01' else load_raw(d)


def code_of_x(x):
    if 0x40 <= x <= 0x7E:
        return x + 0x40
    if x == 0x80:
        return 0xBF
    if x >= 0x9F:
        return x + 0x21
    return None


def glyph_map(path):
    """glyphs.ini: replaced code -> new character."""
    out, section = {}, None
    for line in open(path, encoding='utf-8-sig'):
        line = line.strip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].strip()
        elif section and '=' in line and line.split('=')[0].strip() == 'replaces':
            rep = line.split('=', 1)[1].strip()
            code = int(rep, 16) if rep.lower().startswith('0x') else rep.encode('cp1252')[0]
            out[code] = section
    return out


def to_text(b, glyphs):
    out, i = [], 0
    while i < len(b):
        c = b[i]
        if c == 0x85 and i + 1 < len(b) and code_of_x(b[i + 1]) is not None:
            code = code_of_x(b[i + 1])
            ch = glyphs.get(code)
            if ch is None:
                try:
                    ch = bytes([code]).decode('cp1252')
                except UnicodeDecodeError:
                    ch = None
            out.append(ch if ch and ch not in '{}' else '{85 %02X}' % b[i + 1])
            i += 2
        elif (c >= 0x80 or c == 0) and i + 1 < len(b):
            out.append('{%02X %02X}' % (c, b[i + 1]))
            i += 2
        elif c < 0x20 or c >= 0x7F or c in b'{}':
            out.append('{%02X}' % c)
            i += 1
        else:
            out.append(chr(c))
            i += 1
    return ''.join(out)


def home_score(key, path, rel):
    """How well a file's place names a key: $efark040_x -> ev_fark_040, $avpek021_x -> ac_vpek_021; a zone's key
    ($m_z016...) -> the zone's own archive (white_z016u, or a first folder named z016), not another zone's copy."""
    k = key.decode('latin-1').lstrip('$')
    p = path.replace('\\', '/').lower()
    m = re.match(r'([ea])([a-z]{4})(\d{3})_', k)
    if m and ('%s_%s_%s' % ('ev' if m.group(1) == 'e' else 'ac', m.group(2), m.group(3))) in p:
        return 2
    m = re.search(r'z(\d{3})', k)
    first = rel.replace('\\', '/').split('/')[0].lower()
    if m and (('white_z%su' % m.group(1)) in p or first in ('z%s' % m.group(1), 'white_z%su_img' % m.group(1))):
        return 1
    return 0


def main():
    args = sys.argv[1:]
    glyphs = {}
    if '--glyphs' in args:
        i = args.index('--glyphs')
        glyphs = glyph_map(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    src, out = args
    if os.path.isfile(src):
        files = [(os.path.dirname(src), src)]
    else:
        files = sorted((src, os.path.join(r, f)) for r, _, fs in os.walk(src) for f in fs if f.endswith('.ztr'))
    # Every file's texts, then each key's file chosen.
    loaded = []
    for root, path in files:
        rel = os.path.relpath(path, root).replace('\\', '/')
        name = os.path.basename(rel)
        if name.startswith('txtres') or name.startswith('txtres_'):
            rel = os.path.dirname(rel) or os.path.splitext(name)[0]
        else:
            rel = os.path.splitext(rel)[0]
        loaded.append((rel + '.txt', path, load(path)))
    owner = {}
    for n, (rel, path, entries) in enumerate(loaded):
        for key, text in entries:
            score = (text != b'', home_score(key, path, rel), -n)
            if key not in owner or score > owner[key][0]:
                owner[key] = (score, n)
    written = 0
    for n, (rel, path, entries) in enumerate(loaded):
        lines = []
        for key, text in entries:
            if owner[key][1] != n:
                continue
            owner[key] = (owner[key][0], -1)  # a key twice in one file: its first text
            lines.append('%s = %s' % (key.decode('latin-1'), to_text(text, glyphs)))
        if not lines:
            continue
        target = os.path.join(out, rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        written += len(lines)
    print('ztr_to_texts: %d files, %d texts -> %s' % (len(loaded), written, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
