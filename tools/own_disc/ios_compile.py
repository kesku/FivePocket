#!/usr/bin/env python3
"""The iOS compile stage: the kit and this run's stage outputs laid out as one tree, every unit compiled, one executable.

    ios_compile.py --kit DIR --stage NAME=DIR [...] --library NAME=FILE [...] --work NEW_DIR [--xcode DIR] [--jobs N]

Runs ios_kit.py assemble into WORK/tree, then replay_ios_build.py --from-snapshot into WORK/replay with the libraries
given (the run's own builds of the XMA decoder libraries in place of those the kit's link names). The kit carries the
expected code identity of every object and of the executable (ios_kit.py make --reference-replay); the stage fails
(exit 1) when one differs, and the executable is WORK/replay/app/<name>. Exit 2: refused before compiling.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ios_kit  # noqa: E402
import replay_ios_build  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--kit', type=Path, required=True)
    ap.add_argument('--stage', action='append', default=[])
    ap.add_argument('--library', action='append', default=[])
    ap.add_argument('--define-from', action='append', default=[], metavar='OBJECT:NAME=FILE',
                    help='-DNAME="<first word of FILE>" for that unit (the payload manifest\'s SHA-256)')
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--xcode', type=Path)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--development-toolchain', action='store_true')
    ap.add_argument('--emit-dependencies', action='store_true')
    ap.add_argument('--source-root', type=Path,
                    help='use relocated runtime dependencies from this source checkout')
    a = ap.parse_args(argv)
    if a.source_root and not a.development_toolchain:
        ap.error('--source-root requires --development-toolchain')
    if a.work.exists():
        print(f'refused: {a.work} exists', file=sys.stderr)
        return 2
    a.work.mkdir(parents=True)
    tree = ios_kit.assemble(a.kit, dict(ios_kit.pairs(a.stage)), a.work / 'tree', a.source_root)
    args = ['--manifest', str(tree / 'frozen.json'), '--out', str(a.work / 'replay'), '--from-snapshot', '--jobs', str(a.jobs)]
    if a.xcode:
        args += ['--xcode', str(a.xcode)]
    if a.development_toolchain:
        args += ['--development-toolchain']
    if a.emit_dependencies:
        args += ['--emit-dependencies']
    for item in a.library:
        args += ['--link-library', item]
    for item in a.define_from:
        target, _, assignment = item.partition(':')
        name, _, path = assignment.partition('=')
        value = Path(path).read_text(encoding='utf-8').split()[0]
        args += ['--define', f'{target}:{name}="{value}"']
    return replay_ios_build.main(args)


if __name__ == '__main__':
    sys.exit(main())
