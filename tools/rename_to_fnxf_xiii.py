#!/usr/bin/env python3
"""fnxf_xiii (2026-10-29): renames the project's old name everywhere in the project, once.

    python tools/rename_to_fnxf_xiii.py            rename (prints what it changed)
    python tools/rename_to_fnxf_xiii.py --dry-run  only list what would change

The rules (the same as tools/pgo_functions.py's renamed(), which maps profiles made before the rename):
    <old>_nx, <old>-nx, <old>     ->  fnxf_xiii      (lowercase: cvars, namespaces, paths, files, comments)
    <OLD>_NX, <OLD>-NX            ->  FNXF_XIII
    <Old> (as in <Old>App)        ->  FnxfXiii
    <OLD> inside an identifier    ->  FNXF_XIII      (macros, g_<OLD>Shared, set<OLD>Flags ...)
where <old> is the game's six-letter short name. The short name standing alone in text ("<OLD>'s copy kernel",
the cvar categories "<OLD>/Enhancements") is the game's, not the project's: tools/remove_game_name.py takes it out.

Every text file of the project (thirdparty included: the port's own changes there are marked with the old name), the
text files inside lang/*.zip, and the files' and folders' own names. Left alone: out/ and app/generated/ (made again
by the build: run build_switch.py --regen after this), app/assets/ (the game's files), pgo/ (the profiles:
tools/pgo_unpack.py maps their old names), bin/, .git/, and files that are not text.

After it: rename the SD card's /switch/<old>_nx folder to /switch/fnxf_xiii, and in it <old>.toml, <old>_settings.ini
and the other files with the old name (the shader caches, the logs) the same way.
"""
import io
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELF = os.path.abspath(__file__)
OLD = 'ff' + 'xiii'          # split, so this file reads the same after a rename
OLD_UP = OLD.upper()
OLD_CAP = OLD.capitalize()
SKIP_DIRS = {'.git', 'out', 'pgo', 'bin', '__pycache__'}
SKIP_PATHS = {os.path.join('app', 'generated'), os.path.join('app', 'assets')}
IDENT_UP = re.compile(r'(?<=[A-Za-z0-9_])%s|%s(?=[A-Za-z0-9_])' % (OLD_UP, OLD_UP))


def renamed(s):
    for a in (OLD + '_nx', OLD + '-nx', OLD):
        s = s.replace(a, 'fnxf_xiii')
    for a in (OLD_UP + '_NX', OLD_UP + '-NX'):
        s = s.replace(a, 'FNXF_XIII')
    s = s.replace(OLD_CAP, 'FnxfXiii')
    return IDENT_UP.sub('FNXF_XIII', s)


def is_text(data):
    if b'\0' in data[:65536]:
        return None
    try:
        return data.decode('utf-8')
    except UnicodeDecodeError:
        return None


def skipped(rel_dir):
    parts = rel_dir.split(os.sep) if rel_dir else []
    return any(p in SKIP_DIRS for p in parts) or any(rel_dir == s or rel_dir.startswith(s + os.sep) for s in SKIP_PATHS)


def rewrite_zip(path, dry):
    with zipfile.ZipFile(path) as z:
        entries = [(info, z.read(info)) for info in z.infolist()]
    changed = 0
    out = []
    for info, data in entries:
        text = None if info.is_dir() else is_text(data)
        if text is not None and renamed(text) != text:
            data = renamed(text).encode('utf-8')
            changed += 1
        out.append((info, data))
    if changed and not dry:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            for info, data in out:
                z.writestr(info, data, compress_type=info.compress_type)
        with open(path, 'wb') as f:
            f.write(buf.getvalue())
    return changed


def main():
    dry = '--dry-run' in sys.argv[1:]
    files = changes = zips = 0
    renames = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        rel_dir = os.path.relpath(dirpath, ROOT)
        rel_dir = '' if rel_dir == '.' else rel_dir
        dirnames[:] = [d for d in dirnames if not skipped(os.path.join(rel_dir, d))]
        for d in dirnames:
            if renamed(d) != d:
                renames.append(os.path.join(dirpath, d))
        for name in filenames:
            path = os.path.join(dirpath, name)
            if os.path.abspath(path) == SELF:
                continue
            if renamed(name) != name:
                renames.append(path)
            if name.lower().endswith('.zip') and rel_dir.split(os.sep)[0] == 'lang':
                n = rewrite_zip(path, dry)
                if n:
                    zips += 1
                    print('%s: %d files inside' % (os.path.relpath(path, ROOT), n))
                continue
            try:
                with open(path, 'rb') as f:
                    data = f.read()
            except OSError:
                continue
            text = is_text(data)
            if text is None:
                continue
            new = renamed(text)
            if new == text:
                continue
            files += 1
            changes += sum(text.count(a) for a in (OLD, OLD_CAP)) + len(IDENT_UP.findall(text))
            if not dry:
                with open(path, 'wb') as f:
                    f.write(new.encode('utf-8'))
    # The names, deepest first (a folder after what is in it).
    renames.sort(key=lambda p: p.count(os.sep), reverse=True)
    clashes = 0
    for path in renames:
        target = os.path.join(os.path.dirname(path), renamed(os.path.basename(path)))
        if os.path.exists(target):
            print('not renamed (%s exists): %s' % (os.path.relpath(target, ROOT), os.path.relpath(path, ROOT)))
            clashes += 1
            continue
        if dry:
            print('rename %s -> %s' % (os.path.relpath(path, ROOT), os.path.basename(target)))
        else:
            os.rename(path, target)
    print('%s%d text files (%d places), %d zips, %d names%s' % ('would change: ' if dry else 'changed: ', files, changes,
                                                              zips, len(renames) - clashes,
                                                              ', %d not renamed' % clashes if clashes else ''))
    if not dry:
        print('Next: python build_switch.py --regen (the generated code takes the new name), and rename the SD card\'s '
              'folder and files (see the top of this script).')
    return 1 if clashes else 0


if __name__ == '__main__':
    sys.exit(main())
