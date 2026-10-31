#!/usr/bin/env python3
"""Writes generated/fixture: the shape of the generator's output with a few trivial functions and no game code, so the
runtime links and totk --selftest runs on a machine or in CI without the game.

    .venv/bin/python tools/make_fixture.py
    .venv/bin/python tools/build_game.py --version fixture --expected generated/fixture/expected.json
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path[:0] = [str(ROOT / 'recomp'), str(ROOT / 'tools')]

import gen  # noqa: E402
import patches  # noqa: E402
from check_content import GENERATED_MARKER  # noqa: E402
from layout import place, write  # noqa: E402

SIZES = {'main': 0x100000, 'subsdk0': 0x100000, 'sdk': 0x100000}
FUNCTIONS = 8   # per module; 16 bytes apart, so the word after each start is not a start, as check_modules wants


def chunk_text(name, addrs):
    lines = [f'// {GENERATED_MARKER}: {name} fixture, written by tools/make_fixture.py; holds no game code.\n',
             '#include "cpu.h"\n\n']
    lines += [f'void {gen.fname(a)}(Cpu *c) {{ c->x[0] = {a:#x}ull; }}\n' for a in addrs]
    return ''.join(lines)


def main():
    out = ROOT / 'generated' / 'fixture'
    mods = place(SIZES)
    write(mods, out)
    gen.write_if_changed(out / 'patches.h', patches.header_text(patches.Table(), GENERATED_MARKER))
    for name, base, _ in mods:
        d = out / name
        d.mkdir(parents=True, exist_ok=True)
        addrs = [base + 0x1000 + 16 * i for i in range(FUNCTIONS)]
        gen.write_if_changed(d / f'{name}_0000.c', chunk_text(name, addrs))
        gen.write_if_changed(d / f'{name}_table.c', gen.table_text(name, addrs))
        gen.write_if_changed(d / 'manifest.json', json.dumps({'module': name, 'marker': GENERATED_MARKER,
                                                              'chunks': [f'{name}_0000.c']}, indent=1) + '\n')
    gen.write_if_changed(out / 'expected.json', json.dumps({'exefs_sha256': {n: '0' * 64 for n, _, _ in mods}}) + '\n')
    print(f'{out}: {len(mods)} modules, {FUNCTIONS} functions each')


if __name__ == '__main__':
    main()
