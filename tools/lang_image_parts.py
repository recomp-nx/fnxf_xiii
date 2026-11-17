#!/usr/bin/env python3
"""fnxf_xiii (2026-10-31): a language pack's translated pictures cut down to what the translators changed (their
parts), so that the pack does not carry the game's own pictures (app/src/cpu/fnxf_xiii_images.cpp: "at X,Y").

    python tools/lang_image_parts.py <pack folder> <picture>=<original.png> [...]
    python tools/lang_image_parts.py <pack folder> <picture>@<x>,<y>,<w>,<h> [...]

<pack folder>: the pack unpacked (its images/images.ini and images/). <picture>: a picture's name as images.ini has it
(title_logo/vcc0d314fea3028.png). Either:
  =<original.png>  the game's texture (tools/texture_export.py's PNG of it): the 4x4 blocks where the translated picture
                   differs from it (a larger picture: also every block past the original's size that is not
                   transparent) are grouped into rectangles, each a part;
  @x,y,w,h         one rectangle given (the title logo's credits: nothing of the game's around it).
Each part becomes <picture>_partN.png next to the picture, its line in images.ini the picture's line with " at X,Y";
the picture and its line are removed. Rectangles are in whole 4x4 blocks (DXT).

The difference: a pixel differs when its alpha, or a colour channel weighted by alpha, differs by more than
--threshold (default 24: the translators' copy is the same picture compressed again, with small differences all over).
"""
import argparse
import os
import sys

from PIL import Image


def changed_blocks(orig, tr, threshold):
    """The 4x4 blocks of the translated picture (its size) that differ from the original."""
    tw, th = tr.size
    ow, oh = orig.size
    t = tr.load()
    o = orig.load()
    bw, bh = (tw + 3) // 4, (th + 3) // 4
    out = set()
    for by in range(bh):
        for bx in range(bw):
            hit = False
            for y in range(by * 4, min(by * 4 + 4, th)):
                for x in range(bx * 4, min(bx * 4 + 4, tw)):
                    tp = t[x, y]
                    if x >= ow or y >= oh:
                        hit = tp[3] > 0
                    else:
                        op = o[x, y]
                        hit = abs(tp[3] - op[3]) > threshold or any(
                            abs(tp[c] * tp[3] - op[c] * op[3]) / 255 > threshold for c in range(3))
                    if hit:
                        break
                if hit:
                    break
            if hit:
                out.add((bx, by))
    return out


def rectangles(blocks):
    """The changed blocks grouped (8-neighbour), each group's bounding box; overlapping boxes merged. In blocks."""
    left = set(blocks)
    boxes = []
    while left:
        stack = [left.pop()]
        x0 = x1 = stack[0][0]
        y0 = y1 = stack[0][1]
        while stack:
            x, y = stack.pop()
            x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    n = (x + dx, y + dy)
                    if n in left:
                        left.remove(n)
                        stack.append(n)
        boxes.append([x0, y0, x1, y1])
    merged = True
    while merged:
        merged = False
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                a, b = boxes[i], boxes[j]
                if a[0] <= b[2] and b[0] <= a[2] and a[1] <= b[3] and b[1] <= a[3]:
                    boxes[i] = [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3])]
                    del boxes[j]
                    merged = True
                    break
            if merged:
                break
    return sorted(boxes, key=lambda b: (b[1], b[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pack')
    ap.add_argument('pictures', nargs='+')
    ap.add_argument('--threshold', type=int, default=24)
    args = ap.parse_args()
    images = os.path.join(args.pack, 'images')
    ini_path = os.path.join(images, 'images.ini')
    with open(ini_path, encoding='utf-8', newline='') as f:
        lines = f.read().splitlines(keepends=True)
    for spec in args.pictures:
        if '=' in spec:
            name, original = spec.split('=', 1)
            rect = None
        elif '@' in spec:
            name, r = spec.split('@', 1)
            rect = [int(v) for v in r.split(',')]
            if len(rect) != 4 or any(v % 4 for v in rect):
                sys.exit(f'{spec}: x,y,w,h, each a multiple of 4')
            original = None
        else:
            sys.exit(f'{spec}: <picture>=<original.png> or <picture>@x,y,w,h')
        at = [i for i, l in enumerate(lines) if l.split(' = ')[0] == name]
        if len(at) != 1:
            sys.exit(f'{name}: not a line of images.ini (or more than one)')
        line = lines[at[0]].rstrip('\r\n')
        eol = lines[at[0]][len(line):]
        if ' at ' in line:
            sys.exit(f'{name}: already a part')
        tr = Image.open(os.path.join(images, name)).convert('RGBA')
        if rect:
            boxes = [[rect[0] // 4, rect[1] // 4, (rect[0] + rect[2]) // 4 - 1, (rect[1] + rect[3]) // 4 - 1]]
        else:
            orig = Image.open(original).convert('RGBA')
            boxes = rectangles(changed_blocks(orig, tr, args.threshold))
        stem = name[:-4] if name.lower().endswith('.png') else name
        new = []
        area = 0
        for n, (x0, y0, x1, y1) in enumerate(boxes, 1):
            x, y, w, h = x0 * 4, y0 * 4, (x1 - x0 + 1) * 4, (y1 - y0 + 1) * 4
            w, h = min(w, tr.size[0] - x), min(h, tr.size[1] - y)
            part = f'{stem}_part{n}.png'
            tr.crop((x, y, x + w, y + h)).save(os.path.join(images, part), optimize=True)
            new.append(f'{part} = {line.split(" = ", 1)[1]} at {x},{y}{eol}')
            area += w * h
            print(f'{name}: part {n} at {x},{y}, {w}x{h}')
        lines[at[0]:at[0] + 1] = new
        os.remove(os.path.join(images, name))
        print(f'{name}: {len(new)} parts, {100.0 * area / (tr.size[0] * tr.size[1]):.1f} % of the picture')
    with open(ini_path, 'w', encoding='utf-8', newline='') as f:
        f.writelines(lines)


if __name__ == '__main__':
    main()
