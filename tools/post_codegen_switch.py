#!/usr/bin/env python3
"""fnxf_xiii (2026-10-23): the post-codegen passes of the Switch build, only those proven exact on this tree.

Each was checked the same way: the processed tree against a fresh codegen, line by line, every changed line turned
back into its original by the pass's own definition; and the new form against the old on AArch64 with the Switch
compiler and flags (qemu-aarch64):
  - vec_byteswap.py       guest vector byte reversals without the table lookup (66,563 sites) and the mask-table
                          shuffles as one TBL (4,751): every table row, 3.3M comparisons, 0 differences;
  - vperm_one_source.py   a vperm of one register as one TBL (1,525): every control byte in every lane, 0 differences;
  - float_loads.py        single-precision loads swapped in the vector unit (35,567; the load kept volatile, as every
                          guest load here): 16M loads, flush to zero off and on, 0 differences; the integer statement
                          it removes is never read again (checked over all 36,173 sites);
  - vsel_neon.py          vsel as one NEON bit select (3,022): 1M comparisons, 0 differences;
  - vec_neon_forms.py     (2026-10-24) vslw without simde's second count check (485), vupkd3d128 types 0 and 4 as
                          TBL / SXTL (887), the scaled vcsxwfp / vcuxwfp as one SCVTF #n (736): 124M comparisons
                          (every rounding mode, flush to zero off and on), 0 differences
                          (sdk/tests/vec_neon_forms_test.cpp);
  - store_cards.py        (2026-10-24, last) every guest store outside the stack also zeroes its 4 KB page's card
                          (131,243 scalar, 32,195 vector, 445 stwcx./stdcx., 36 dcbz; 134,950 stack stores left plain),
                          so Renut proves its cached vertex, index and texture copies by the cards instead of hashing
                          them again (fnxf_xiii_renut_store_cards). A card is a byte in the guest window's first MB, which
                          the game never uses: the game's own memory is written exactly as before. The host writers of
                          guest memory mark theirs too (the SDK's CRT hooks and file reads, fnxf_xiii_native_crt / _zlib /
                          _vmath, the memexport results); fnxf_xiii_renut_store_cards_verify=true hashes every copy the
                          cards prove and logs any that changed under clean cards ("[cards] ... marks none").
The other steps of tools/post_codegen.py are not run: they were written for an older layout of the generated code and
none has been checked on this one.

Every pass is idempotent; a second run changes nothing. Run on a fresh codegen (build_switch.py does).
  python tools/post_codegen_switch.py --gen app/generated/default [--force]

fnxf_xiii (2026-10-25): skipped when nothing changed since its last run: post_codegen_switch.stamp in the generated
folder holds what the tree looked like when the passes finished (the number of generated .cpp / .h files, the latest
modification time among them, their total size) and a hash of the pass scripts. A codegen that wrote any file, a
changed pass, or a file edited by hand runs them all again (on the whole tree: they are idempotent); --force always.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
PASSES = ['vec_byteswap.py', 'vperm_one_source.py', 'float_loads.py', 'vsel_neon.py', 'vec_neon_forms.py',
          'store_cards.py']


STAMP = 'post_codegen_switch.stamp'


def tree_state(gen):
    """The generated .cpp / .h files: count, latest modification time (ns), total size."""
    count = size = latest = 0
    with os.scandir(gen) as it:
        for e in it:
            if e.is_file() and e.name.endswith(('.cpp', '.h')):
                st = e.stat()
                count += 1
                size += st.st_size
                latest = max(latest, st.st_mtime_ns)
    return {'files': count, 'latest_mtime_ns': latest, 'bytes': size}


def scripts_hash():
    h = hashlib.sha256()
    for name in sorted(PASSES + [os.path.basename(__file__)]):
        with open(os.path.join(TOOLS, name), 'rb') as f:
            h.update(name.encode() + b'\0' + f.read())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--force', action='store_true', help='run the passes even when the stamp says nothing changed')
    a = ap.parse_args()
    stamp_path = os.path.join(a.gen, STAMP)
    want = {'scripts': scripts_hash(), 'tree': tree_state(a.gen)}
    if not a.force and os.path.isfile(stamp_path):
        try:
            with open(stamp_path, encoding='utf-8') as f:
                have = json.load(f)
        except (OSError, ValueError):
            have = None
        if have == want:
            print('post_codegen_switch: up to date (nothing generated or changed since the last run; --force to run)')
            return
    # A run that stops half way must not leave a stamp saying it finished.
    if os.path.isfile(stamp_path):
        os.remove(stamp_path)
    for p in PASSES:
        r = subprocess.run([sys.executable, os.path.join(TOOLS, p), '--gen', a.gen])
        if r.returncode != 0:
            sys.exit('post_codegen_switch: %s failed (%d)' % (p, r.returncode))
    # The generated pch must have every pass's definitions (a pass that rewrote the code without its macro breaks the
    # build only much later).
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    pch = open(os.path.join(a.gen, pchs[0]), encoding='utf-8').read()
    for macro in ('REX_BSWAP128', 'REX_SHUFFLE_TABLE', 'REX_VPERM1', 'REX_LOAD_F32', 'REX_VSEL', 'REX_SLLV31',
                  'REX_CVT_SCALED', 'REX_VUPKD3D_COLOR', 'REX_CARD'):
        if '#define ' + macro not in pch and '#define ' + macro + '(' not in pch:
            sys.exit('post_codegen_switch: the pch has no %s after the passes' % macro)
    with open(stamp_path, 'w', encoding='utf-8') as f:
        json.dump({'scripts': want['scripts'], 'tree': tree_state(a.gen)}, f)
    print('post_codegen_switch: done (%s)' % ', '.join(PASSES))


if __name__ == '__main__':
    main()
