#!/usr/bin/env python3
"""The iOS build kit: what a player's Mac needs besides the builder's own stage outputs, and the tree a build runs in.

    ios_kit.py make --manifest FROZEN.json --stage NAME=DIR [...] --out NEW_KIT_DIR [--reference-replay DIR]
                    [--no-pgo] [--leave-out-library NAME ...] [--link-flag WORD ...]
    ios_kit.py assemble --kit KIT_DIR --stage NAME=DIR [...] --out NEW_TREE_DIR
    ios_kit.py neutral --kit KIT_DIR --out NEW_KIT_DIR

make: FROZEN.json is a frozen-build-apple/1 manifest with a snapshot (freeze_ios_build.py --snapshot). Every input whose
bytes a stage of a run writes (found by SHA-256 below the given stage folders: the recipe's chunks, the generator's
headers, the native packs, the shader tables, ...) comes from that stage; every other file of the snapshot (sources,
overlays, the files they stand in for, libraries, links) is copied into the kit. kit.json names, for every place of the
snapshot, where its file comes from: {"kit": REL} or {"stage": NAME, "path": REL, "sha256": H}, plus the frozen manifest
rewritten to the kit's own root names. A snapshot file the compile does not read gets the same treatment (2026-11-06: the
source folders held unread copies of the shader tables; a kit never carries a stage's bytes). A file that differs from a
stage file of the same name only in its first line, a // comment (the generator's name), comes from that stage file with
the kit's first line put back: {"stage", "path", "stage_sha256", "first_line", "sha256"}, both hashes checked on assembly
(the shader tables the build compiles, whose bytes the stages make from the player's discs).
--no-pgo: the public kit's variant (main, 2026-11-05): the profile data a command names (-fprofile-instr-use, recorded from
play: game-derived) is left out of the kit and out of every command, with its two -Wno companions; the code is then not the
private build's, so the expected identities come from a replay of the no-PGO kit (make it once without
--reference-replay, assemble, replay, then make again with it). --leave-out-library: a static library the build links from
its own stage instead (ios_compile.py --library), left out of the kit.
--link-flag: a word put into the kit's link command after the compiler driver (and in a reference replay's link, by the
same make without --reference-replay first). -Wl,-objc_stubs_small makes ld-1267's link deterministic (2026-11-05: 32 of 32
links one file; without it two files, the __objc_stubs and __stubs built in parallel in two orders): with it the kit records
the executable's whole code identity, stubs included (reference_code), and a replay is checked against that.
Give it as --link-flag=-Wl,-objc_stubs_small (a separate word that starts with - reads as an option).
neutral: a copy of the kit without the building Mac's folder names (the owner-detail gate of a published kit): the snapshot
folder becomes /fp-kit/snapshot and every original root of the snapshot map /fp-src/rN (longest first), in kit.json and in
every kit file that names one; the inputs among those files are pinned to their new bytes. Assembly and the replay map the
new names exactly as the old ones; the expected identities are code identities, which no path enters (2026-11-05: the replay
of kit-243d-np in another folder was code-equal unit by unit).
assemble: lays the tree out: kit files by clone, stage files from the run's stage folders (each checked by hash: a run
whose stage wrote other bytes is refused), links as they were. Then replay_ios_build.py --manifest TREE/frozen.json
--from-snapshot compiles in it. An explicit source root can supply the relocated host, bridge and GPU dependencies. Their bytes are pinned in the
assembled manifest; generated game inputs still come only from the verified stages.
Exit 0: done; 2: refused.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import posixpath
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macho_sections  # noqa: E402


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def clone(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['/bin/cp', '-c', '-p', str(src), str(dst)], check=True)


def stage_index(stages: list[tuple[str, Path]], sizes: set[int]) -> dict[str, tuple[str, str]]:
    """sha256 -> (stage, path below its folder) for every file of the stage folders with a wanted size"""
    found: dict[str, tuple[str, str]] = {}
    for name, folder in stages:
        for p in sorted(folder.rglob('*')):
            if p.is_file() and not p.is_symlink() and p.stat().st_size in sizes:
                found.setdefault(sha256(p), (name, p.relative_to(folder).as_posix()))
    return found


def first_line_variant(path: Path, candidates) -> dict | None:
    """{"stage", "path", "stage_sha256", "first_line"} when PATH equals a candidate stage file but for its first line, a //
    comment in both; None otherwise"""
    if not candidates or path.stat().st_size > (1 << 26):
        return None
    data = path.read_bytes()
    first, sep, rest = data.partition(b'\n')
    if not sep or not first.startswith(b'//') or b'\r' in first:
        return None
    for stage, q, rel in candidates:
        other = q.read_bytes()
        head, sep2, tail = other.partition(b'\n')
        if sep2 and head.startswith(b'//') and tail == rest:
            return {'stage': stage, 'path': rel, 'stage_sha256': sha256(q), 'first_line': first.decode('utf-8')}
    return None


PGO_WORDS = ('-Wno-profile-instr-unprofiled', '-Wno-backend-plugin')


def make(manifest_path: Path, stages: list[tuple[str, Path]], out: Path, reference_replay: Path | None = None,
         no_pgo: bool = False, leave_out_libraries: tuple[str, ...] = (), link_flags: tuple[str, ...] = ()) -> dict:
    m = json.loads(manifest_path.read_text(encoding='utf-8'))
    m['link']['command'][1:1] = list(link_flags)
    left_out_names = set()
    if no_pgo:
        for u in m['units']:
            u['command'] = [w for w in u['command'] if not w.startswith('-fprofile-instr-use=') and w not in PGO_WORDS]
        for prof in m.get('profiles', []):
            left_out_names.add(str(Path(prof).resolve()))
            m['inputs'].pop(str(Path(prof).resolve()), None)
        m['profiles'] = []
    for name in leave_out_libraries:
        hits = [p for p in m['libraries'] if Path(p).name == name]
        if len(hits) != 1:
            raise SystemExit(f'--leave-out-library {name}: not one library of the link')
        left_out_names.add(hits[0])
    snap = m.get('snapshot')
    if not snap:
        raise SystemExit('the manifest has no snapshot (freeze_ios_build.py --snapshot)')
    root = Path(snap['dir'])
    rewritten = snap.get('rewritten', {})
    snapshot_files = [p for p in sorted(root.rglob('*')) if p.is_file() and not p.is_symlink()]
    index = stage_index(stages, {rec['size'] for rec in m['inputs'].values()} | {p.stat().st_size for p in snapshot_files})
    by_name: dict[str, list[tuple[str, Path, str]]] = {}
    for stage_name, folder in stages:
        for q in sorted(folder.rglob('*')):
            if q.is_file() and not q.is_symlink():
                by_name.setdefault(q.name, []).append((stage_name, q, q.relative_to(folder).as_posix()))
    places: dict[str, dict] = {}
    by_place = {}
    for name in m['inputs']:
        for old, new in snap['map']:
            if name == old or name.startswith(old + '/'):
                by_place[name] = (new + name[len(old):])[len(str(root)) + 1:]
                break
    # the reference executable is the build's own game code: never in a kit; its code identity stands in for it
    reference = m['link'].get('reference')
    reference_rel = None
    if reference:
        for old, new in snap['map']:
            if reference.startswith(old + '/'):
                reference_rel = (new + reference[len(old):])[len(str(root)) + 1:]
                break
    left_out = set()
    for name in left_out_names:
        for old, new in snap['map']:
            if name.startswith(old + '/'):
                left_out.add((new + name[len(old):])[len(str(root)) + 1:])
                break
    rewritten_rels = {by_place[name] for name in rewritten if name in by_place}
    out.mkdir(parents=True)
    from_stage = 0
    for name, rec in m['inputs'].items():
        rel = by_place[name]
        if name not in rewritten and rec['sha256'] in index:
            stage, path = index[rec['sha256']]
            places[rel] = {'stage': stage, 'path': path, 'sha256': rec['sha256']}
            from_stage += 1
    for p in sorted(root.rglob('*')):
        rel = p.relative_to(root).as_posix()
        if rel in places or rel == reference_rel or rel in left_out:
            continue
        if p.is_symlink():
            places[rel] = {'link': os.readlink(p)}
        elif p.is_dir() and not any(p.iterdir()):
            places[rel] = {'folder': True}                  # an include folder of the build that supplied no file
        elif p.is_file():
            h = sha256(p)
            if rel not in rewritten_rels and h in index:     # a stage's bytes the compile does not read
                stage, path = index[h]
                places[rel] = {'stage': stage, 'path': path, 'sha256': h}
                from_stage += 1
                continue
            variant = None if rel in rewritten_rels else first_line_variant(p, by_name.get(p.name, ()))
            if variant:
                places[rel] = dict(variant, sha256=h)
                from_stage += 1
                continue
            clone(p, out / 'files' / rel)
            places[rel] = {'kit': rel, 'sha256': sha256(p)}
            if p.stat().st_size < (1 << 24) and str(root).encode() in p.read_bytes():
                places[rel]['names_the_tree'] = True        # an overlay or a rewritten include: rewritten on assembly
    kit_manifest = json.loads(json.dumps(m))
    if reference_rel:
        kit_manifest['link']['reference_identity'] = macho_sections.executable_identity(root / reference_rel)
        kit_manifest['link'].pop('reference')
    kit_manifest['snapshot'] = dict(snap, dir='@TREE')
    kit_manifest['snapshot']['map'] = [[old, '@TREE' + new[len(str(root)):]] for old, new in snap['map']]
    if reference_replay:
        # the expected code of every object and of the executable: those of a replay of this build in the same tree
        # (with the run's own libraries), so a player's build is checked without the first build's files
        report = json.loads((reference_replay / 'report.json').read_text(encoding='utf-8'))
        replayed = {u['object']: u['replayed'] for u in report['units']}
        for u in kit_manifest['units']:
            u['object_identity'] = macho_sections.code_identity(Path(replayed[u['object']]))
        argv = report['link']['argv']
        exe = Path(argv[argv.index('-o') + 1])
        stubs_small = '-Wl,-objc_stubs_small' in kit_manifest['link']['command']
        if stubs_small and '-Wl,-objc_stubs_small' not in argv:
            raise SystemExit('--reference-replay: its link ran without -Wl,-objc_stubs_small (make the kit without '
                             '--reference-replay first, assemble and replay it)')
        kit_manifest['link']['reference_identity'] = macho_sections.executable_identity(exe)
        if stubs_small:
            kit_manifest['link']['reference_code'] = macho_sections.code_identity(exe)
        kit_manifest['link']['libraries_replaced'] = [Path(w).name for w in argv if w.endswith('.a')]
    record = {'schema': 'ios-kit/1', 'frozen': kit_manifest, 'snapshot_dir': str(root), 'places': places,
              'stages': [name for name, _ in stages], 'no_pgo': no_pgo, 'left_out': sorted(left_out),
              'link_flags': list(link_flags)}
    (out / 'kit.json').write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8', newline='\n')
    kit_files = sum(1 for v in places.values() if 'kit' in v)
    print(f'KIT {kit_files} files in the kit, {from_stage} from stages, '
          f'{sum(1 for v in places.values() if "link" in v)} links -> {out}', flush=True)
    return record


# These names are part of the archived kit format, not the current source layout.
SOURCE_ROOTS = {
    'repo/android/app/src/main/cpp/': 'runtime/host',
    'repo/tools/host_guest_trace/': 'runtime/bridge',
    'repo/tools/vulkan_probe/': 'runtime/gpu',
}


def input_places(record: dict) -> set[str]:
    mappings = sorted(record['frozen']['snapshot']['map'], key=lambda pair: -len(pair[0]))
    links = sorted(((path, entry['link']) for path, entry in record['places'].items() if 'link' in entry),
                   key=lambda pair: -len(pair[0]))
    result = set()
    for name in record['frozen']['inputs']:
        for old, new in mappings:
            if name == old or name.startswith(old + '/'):
                relative = posixpath.normpath((new + name[len(old):]).removeprefix('@TREE/'))
                for _ in range(len(links) + 1):
                    for path, destination in links:
                        if relative == path or relative.startswith(path + '/'):
                            target = posixpath.normpath(posixpath.join(posixpath.dirname(path), destination))
                            relative = target + relative[len(path):]
                            break
                    else:
                        result.add(relative)
                        break
                else:
                    raise SystemExit('cyclic kit source aliases')
                break
    return result


def local_sources(kit: Path, record: dict, root: Path | None) -> dict:
    if root is None:
        return {}
    root = root.resolve()
    sources = {}
    needed = input_places(record)
    for rel, entry in record['places'].items():
        if 'kit' not in entry or rel not in needed:
            continue
        for old, new in SOURCE_ROOTS.items():
            if not rel.startswith(old):
                continue
            base = root / new
            path = base / rel[len(old):]
            if not path.resolve().is_relative_to(base.resolve()) or path.is_symlink() or not path.is_file():
                raise SystemExit(f'relocated source missing or outside source root: {path}')
            original = kit / 'files' / entry['kit']
            if not original.is_file() or sha256(original) != entry['sha256']:
                raise SystemExit(f'source-kit input changed: {rel}')
            data = path.read_bytes()
            sources[rel] = {'data': data, 'path': path.relative_to(root).as_posix(),
                            'sha256': hashlib.sha256(data).hexdigest()}
            break
    return sources


def assemble(kit: Path, stages: dict[str, Path], out: Path, source_root: Path | None = None) -> Path:
    record = json.loads((kit / 'kit.json').read_text(encoding='utf-8'))
    missing = [s for s in record['stages'] if s not in stages]
    if missing:
        raise SystemExit(f'stage folders not given: {", ".join(missing)}')
    sources = local_sources(kit, record, source_root)
    tree = out.resolve()
    out.mkdir(parents=True)
    old_root = record['snapshot_dir']
    problems = []
    for rel, src in sorted(record['places'].items(), key=lambda item: 'link' in item[1]):
        dst = tree / rel
        if 'link' in src:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.symlink_to(src['link'])
        elif 'folder' in src:
            dst.mkdir(parents=True, exist_ok=True)
        elif 'kit' in src:
            path = kit / 'files' / src['kit']
            if rel in sources:
                data = sources[rel]['data']
                if src.get('names_the_tree'):
                    data = data.replace(old_root.encode(), str(tree).encode())
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(data)
            elif src.get('names_the_tree'):
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(path.read_bytes().replace(old_root.encode(), str(tree).encode()))
            else:
                clone(path, dst)
        else:
            path = stages[src['stage']] / src['path']
            if 'first_line' in src:                     # the stage's table under the kit's first line
                if not path.is_file() or sha256(path) != src['stage_sha256']:
                    problems.append(f'stage {src["stage"]} did not write the expected {src["path"]}')
                    continue
                data = src['first_line'].encode('utf-8') + b'\n' + path.read_bytes().partition(b'\n')[2]
                if hashlib.sha256(data).hexdigest() != src['sha256']:
                    problems.append(f'{rel}: the stage file under the kit\'s first line is not the recorded file')
                    continue
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(data)
                continue
            if not path.is_file() or sha256(path) != src['sha256']:
                problems.append(f'stage {src["stage"]} did not write the expected {src["path"]}')
                continue
            clone(path, dst)
    if problems:
        raise SystemExit('\n'.join(problems[:20]))
    frozen = json.loads(json.dumps(record['frozen']).replace('@TREE', str(tree)))
    source_audit = {}
    for rel, source in sources.items():
        source_audit[rel] = {'source': source['path'], 'source_sha256': source['sha256'],
                             'assembled_sha256': sha256(tree / rel)}
    if source_audit:
        frozen['source_overrides'] = source_audit
        for name in frozen['inputs']:
            for old, new in frozen['snapshot']['map']:
                if name == old or name.startswith(old + '/'):
                    path = Path(new + name[len(old):])
                    rel = path.resolve().relative_to(tree).as_posix()
                    if rel in sources:
                        frozen['inputs'][name] = {'size': path.stat().st_size, 'sha256': sha256(path)}
                    break
    rewritten = {}
    for name, h in frozen['snapshot'].get('rewritten', {}).items():
        for old, new in frozen['snapshot']['map']:
            if name.startswith(old + '/'):
                rewritten[name] = sha256(Path(new + name[len(old):]))
                break
    frozen['snapshot']['rewritten'] = rewritten
    (tree / 'frozen.json').write_text(json.dumps(frozen, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f'TREE {len(record["places"])} places -> {tree}', flush=True)
    return tree


def neutral(kit: Path, out: Path) -> dict:
    record = json.loads((kit / 'kit.json').read_text(encoding='utf-8'))
    roots = sorted({old for old, _new in record['frozen']['snapshot']['map']}, key=len, reverse=True)
    names = [(record['snapshot_dir'], '/fp-kit/snapshot')] + [(r, f'/fp-src/r{i}') for i, r in enumerate(roots, 1)]
    if any(new in json.dumps(record) for _old, new in names):
        raise SystemExit('the kit already names /fp-kit or /fp-src: is it neutral already?')

    def swap(text: str) -> str:
        for old, new in names:                          # the snapshot folder first: it lies below an original root
            text = text.replace(old, new)
        return text
    out.mkdir(parents=True)
    changed = {}
    for rel, src in record['places'].items():
        if 'kit' not in src:
            continue
        path, dst = kit / 'files' / src['kit'], out / 'files' / src['kit']
        data = path.read_bytes()
        new = data
        for old, token in names:
            new = new.replace(old.encode(), token.encode())
        dst.parent.mkdir(parents=True, exist_ok=True)
        if new == data:
            clone(path, dst)
        else:
            dst.write_bytes(new)
            changed[rel] = (sha256(dst), len(new))
            src['sha256'] = changed[rel][0]
    text = swap(json.dumps(record))
    record = json.loads(text)
    record['snapshot_dir'] = '/fp-kit/snapshot'
    frozen = record['frozen']
    rewritten = frozen['snapshot'].get('rewritten', {})
    for name in frozen['inputs']:
        for old, new in frozen['snapshot']['map']:
            if name.startswith(old + '/'):
                rel = (new + name[len(old):])[len('@TREE') + 1:]
                if rel in changed and name not in rewritten:
                    frozen['inputs'][name] = {'sha256': changed[rel][0], 'size': changed[rel][1]}
                break
    left = [w for w in ('/Users/', '/Volumes/') if w in json.dumps(record)]
    (out / 'kit.json').write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f'NEUTRAL {len(changed)} kit files rewritten, {len(roots)} roots -> {out}'
          + (f' (still named in kit.json: {left})' if left else ''), flush=True)
    return record


def pairs(items: list[str]) -> list[tuple[str, Path]]:
    out = []
    for item in items:
        name, _, folder = item.partition('=')
        if not name or not folder:
            raise SystemExit(f'--stage NAME=DIR, not {item!r}')
        out.append((name, Path(folder).resolve()))
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='command', required=True)
    mk = sub.add_parser('make')
    mk.add_argument('--manifest', type=Path, required=True)
    mk.add_argument('--stage', action='append', default=[])
    mk.add_argument('--out', type=Path, required=True)
    mk.add_argument('--reference-replay', type=Path)
    mk.add_argument('--no-pgo', action='store_true')
    mk.add_argument('--leave-out-library', action='append', default=[])
    mk.add_argument('--link-flag', action='append', default=[])
    asm = sub.add_parser('assemble')
    asm.add_argument('--kit', type=Path, required=True)
    asm.add_argument('--stage', action='append', default=[])
    asm.add_argument('--out', type=Path, required=True)
    asm.add_argument('--source-root', type=Path)
    neu = sub.add_parser('neutral')
    neu.add_argument('--kit', type=Path, required=True)
    neu.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(argv)
    if a.out.exists():
        print(f'refused: {a.out} exists', file=sys.stderr)
        return 2
    if a.command == 'make':
        make(a.manifest, pairs(a.stage), a.out, a.reference_replay, a.no_pgo, tuple(a.leave_out_library), tuple(a.link_flag))
    elif a.command == 'neutral':
        neutral(a.kit, a.out)
    else:
        assemble(a.kit, dict(pairs(a.stage)), a.out, a.source_root)
    return 0


if __name__ == '__main__':
    sys.exit(main())
