#!/usr/bin/env python3
"""Replay a frozen iOS build (frozen-build-apple/1, freeze_ios_build.py) in a new folder and compare it byte for byte.

    replay_ios_build.py --manifest FILE --out NEW_DIR [--from-snapshot] [--map OLD=NEW ...] [--xcode DIR] [--jobs N]

Every unit is compiled again by its recorded command, with only its output moved to NEW_DIR/objects/ (dependency and
diagnostics outputs are dropped; nothing is written next to the sources), and each object's SHA-256 is compared with
the manifest's. The link runs by its recorded command with new file lists of those objects and its output in
NEW_DIR/app/, and the executable is compared with the manifest's unsigned SHA-256. --map rewrites every word that
starts with OLD (longest first) to start with NEW instead: how a replay reads the inputs from the builder's work folder
and the supplied folder instead of the places the first build read them. --xcode replaces the manifest's Xcode folder
the same way; --from-snapshot adds the map of the manifest's snapshot (freeze_ios_build.py --snapshot). Before anything runs, every input and toolchain file is checked by hash at its (mapped) place.

report.json in NEW_DIR lists every unit (equal or not, seconds) and the link. Exit 0: every object and the executable
equal; 1: a difference or a failed command (the folder is kept); 2: refused before running.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macho_sections  # noqa: E402


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def mapper(pairs: list[tuple[str, str]]):
    pairs = sorted(pairs, key=lambda p: -len(p[0]))

    def remap(word: str) -> str:
        for old, new in pairs:
            at = word.find(old)
            # a path, an option's value after = or , (-fprofile-instr-use=…, -Wl,-filelist,…) or attached (-I…, -F…)
            if at == 0 or (at > 0 and (word[at - 1] in '=,' or word.startswith('-'))):
                end = at + len(old)
                if end == len(word) or word[end] == '/':
                    return word[:at] + new + word[end:]
        return word
    return remap


def compile_argv(command: list[str], obj: Path, remap) -> list[str]:
    out, skip = [], False
    for w in command:
        if skip:
            skip = False
            continue
        if w in ('-o', '-MF', '-MT', '-MQ', '--serialize-diagnostics'):
            skip = True
            continue
        if w in ('-MD', '-MMD'):
            continue
        out.append(remap(w))
    return out + ['-o', str(obj)]


def object_name(index: int, obj: str) -> str:
    return f'{index:03d}-{Path(obj).name}'


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--map', action='append', default=[], metavar='OLD=NEW')
    ap.add_argument('--xcode', type=Path)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--from-snapshot', action='store_true')
    ap.add_argument('--development-toolchain', action='store_true',
                    help='compile with the selected Xcode and report identity differences; never certify a release replay')
    ap.add_argument('--emit-dependencies', action='store_true', help='record the headers actually read for each compiled unit')
    ap.add_argument('--define', action='append', default=[], metavar='OBJECT:NAME=VALUE',
                    help='in the unit whose object has this file name, the existing -DNAME=... becomes -DNAME=VALUE')
    ap.add_argument('--link-library', action='append', default=[], metavar='NAME=PATH',
                    help='link this file in place of the static library of that file name (the run\'s own build of it)')
    a = ap.parse_args(argv)
    if a.out.exists():
        print(f'REPLAY_REFUSED: {a.out} exists', file=sys.stderr)
        return 2
    m = json.loads(a.manifest.read_text(encoding='utf-8'))
    if m.get('schema') != 'frozen-build-apple/1':
        print('REPLAY_REFUSED: not a frozen-build-apple/1 manifest', file=sys.stderr)
        return 2
    pairs = [tuple(p.split('=', 1)) for p in a.map]
    if a.from_snapshot:
        if 'snapshot' not in m:
            print('REPLAY_REFUSED: the manifest has no snapshot', file=sys.stderr)
            return 2
        pairs += [tuple(p) for p in m['snapshot']['map']]
    xcode = str(a.xcode.resolve()) if a.xcode else m['xcode']
    if xcode != m['xcode']:
        pairs.append((m['xcode'], xcode))
    remap = mapper(pairs)
    problems = []
    for item in a.define:                               # e.g. the payload manifest's hash the shell checks
        target, _, assignment = item.partition(':')
        name = assignment.split('=', 1)[0]
        units = [u for u in m['units'] if Path(u['object']).name == target]
        hits = [i for u in units for i, w in enumerate(u['command']) if w.startswith(f'-D{name}=')]
        if len(units) != 1 or len(hits) != 1:
            print(f'REPLAY_REFUSED: --define {item}: no one unit {target} with one -D{name}=', file=sys.stderr)
            return 2
        units[0]['command'][hits[0]] = f'-D{assignment}'
    rewritten = m.get('snapshot', {}).get('rewritten', {}) if a.from_snapshot else {}
    for name, rec in m['inputs'].items():
        p = Path(remap(name))
        if name in rewritten:                             # the snapshot's copy names its includes in the snapshot
            if not p.is_file() or sha256(p) != rewritten[name]:
                problems.append(f'input missing or changed: {p}')
        elif not p.is_file() or p.stat().st_size != rec['size'] or sha256(p) != rec['sha256']:
            problems.append(f'input missing or changed: {p}')
    toolchain_differences = []
    for name, rec in m['toolchain_inputs'].items():
        p = Path(xcode) / name
        actual = sha256(p) if p.is_file() else None
        if actual != rec['sha256']:
            toolchain_differences.append({'path': name, 'expected': rec['sha256'], 'actual': actual})
            if not a.development_toolchain:
                problems.append(f'toolchain file missing or changed (another Xcode?): {name}')
    for u in m['units']:
        for i, w in enumerate(u['command']):
            for flag in ('-I', '-iquote', '-isystem', '-F'):
                path = u['command'][i + 1] if w == flag and i + 1 < len(u['command']) else (w[2:] if flag in ('-I', '-F') and w.startswith(flag) and len(w) > 2 else None)
                if path and Path(path).is_dir() and not Path(remap(path) if w != flag else remap(path)).is_dir():
                    problems.append(f'include folder not reached through the map: {path}')
    problems = list(dict.fromkeys(problems))
    if problems:
        print('REPLAY_REFUSED:\n' + '\n'.join(problems[:30]) + (f'\n... {len(problems)} in all' if len(problems) > 30 else ''),
              file=sys.stderr)
        return 2
    objects = a.out / 'objects'
    objects.mkdir(parents=True)
    env = dict(os.environ, DEVELOPER_DIR=xcode)
    by_object = {}

    def run(item):
        i, u = item
        obj = objects / object_name(i, u['object'])
        t = time.time()
        command = compile_argv(u['command'], obj, remap)
        dependency = obj.with_suffix('.d') if a.emit_dependencies else None
        if dependency:
            command += ['-MD', '-MF', str(dependency)]
        r = subprocess.run(command, capture_output=True, text=True, env=env)
        got = sha256(obj) if r.returncode == 0 and obj.is_file() else None
        equal = got == u['object_sha256']
        # the same code and data, other debug information (another working directory, other include spellings); a kit
        # carries the expected identity, since the first build's objects are not on the player's Mac
        if got is None:
            code = False
        elif u.get('object_identity'):
            code = macho_sections.code_identity(obj) == u['object_identity']
        else:
            code = equal or (Path(u['object']).is_file()
                             and macho_sections.code_identity(obj) == macho_sections.code_identity(Path(u['object'])))
        return {'object': u['object'], 'replayed': str(obj), 'origin': u['origin'], 'seconds': round(time.time() - t, 1),
                **({'dependency_file': str(dependency)} if dependency else {}),
                'equal': equal, 'code_equal': code, 'sha256': got, 'error': r.stderr[-800:] if r.returncode else ''}

    started = time.time()
    with ThreadPoolExecutor(a.jobs) as pool:
        units = list(pool.map(run, enumerate(m['units'])))
    for u in units:
        by_object[os.path.realpath(u['object'])] = u['replayed']
    report = {'manifest': str(a.manifest), 'manifest_sha256': sha256(a.manifest), 'xcode': xcode, 'map': pairs,
              'verification': 'development' if a.development_toolchain else 'release',
              'toolchain_differences': toolchain_differences,
              'units': units, 'compile_seconds': round(time.time() - started, 1)}
    link_ok = False
    if all(u['sha256'] for u in units):
        command = m['link']['command']
        app = a.out / 'app'
        app.mkdir()

        def relist(src: str) -> str:
            """a link file list with the replayed objects in the recorded order"""
            dst = a.out / Path(src).name
            # the freeze's record of the list (no snapshot holds a link list); an older manifest: the list where the map puts it
            recorded = m['link'].get('file_lists', {}).get(src)
            lines = recorded if recorded is not None else Path(remap(src)).read_text(encoding='utf-8').splitlines()
            dst.write_text(''.join(by_object[os.path.realpath(o.strip())] + '\n' for o in lines if o.strip()),
                           encoding='utf-8', newline='\n')
            return str(dst)

        libraries = dict(item.split('=', 1) for item in a.link_library)
        new = []
        for i, w in enumerate(command):
            before = command[i - 1] if i else ''
            if before == '-filelist':
                new.append(relist(w))
            elif w.startswith('-Wl,-filelist,'):
                new.append('-Wl,-filelist,' + relist(w.split(',', 2)[2]))
            elif before == '-o':
                new.append(str(app / Path(w).name))
            elif before == '-Xlinker' and i > 1 and command[i - 2] == '-dependency_info':
                new.append(str(a.out / 'dependency_info.dat'))
            elif w.endswith('.a') and Path(w).name in libraries:
                new.append(libraries.pop(Path(w).name))
            else:
                new.append(remap(w))
        if libraries:
            print('REPLAY_REFUSED: --link-library names no library of the link: ' + ', '.join(libraries), file=sys.stderr)
            return 2
        t = time.time()
        r = subprocess.run(new, capture_output=True, text=True, env=env)
        exe = app / Path(command[command.index('-o') + 1]).name
        got = sha256(exe) if r.returncode == 0 and exe.is_file() else None
        link_ok = got == m['link']['unsigned_sha256']
        reference = remap(m['link']['reference']) if m['link'].get('reference') else None
        wanted = m['link'].get('reference_identity') or (macho_sections.executable_identity(Path(reference))
                                                         if reference and Path(reference).is_file() else None)
        if got is not None and m['link'].get('reference_code'):   # a deterministic link (-objc_stubs_small): stubs too
            code = link_ok or macho_sections.code_identity(exe) == m['link']['reference_code']
        else:
            code = link_ok or (got is not None and wanted is not None and macho_sections.executable_identity(exe) == wanted)
        report['link'] = {'argv': new, 'seconds': round(time.time() - t, 1), 'sha256': got, 'equal': link_ok,
                          'code_equal': code, 'error': r.stderr[-1500:] if r.returncode else ''}
    differ = [u for u in units if not u['code_equal']]
    same = sum(u['equal'] for u in units)
    link = report.get('link')
    report['release_equivalent'] = not toolchain_differences and not differ and bool(link and link['code_equal'])
    (a.out / 'report.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
    if a.development_toolchain:
        print('DEVELOPMENT BUILD: source inputs verified; release code identity is not certified.', flush=True)
    print(f'REPLAY {same}/{len(units)} objects byte-equal, {len(units) - len(differ)}/{len(units)} code-equal '
          f'({report["compile_seconds"]} s); link '
          + ('not run' if not link else 'byte-equal' if link['equal'] else 'code-equal' if link['code_equal'] else 'differs')
          + (f' ({link["seconds"]} s)' if link else ''), flush=True)
    for u in differ[:10]:
        print('  differs:', u['object'], (u['error'] or '')[-300:], file=sys.stderr)
    if a.development_toolchain:
        return 0 if all(u['sha256'] for u in units) and link and link['sha256'] else 1
    return 0 if not differ and link and link['code_equal'] else 1


if __name__ == '__main__':
    sys.exit(main())
