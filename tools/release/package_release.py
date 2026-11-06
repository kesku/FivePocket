#!/usr/bin/env python3
"""Package the Mac Duo builder and its source with integrity and release checks.

    package_release.py --version V --out NEW_DIR [--builder-macos DIR]
                       [--retail-list FILE [--generated-list FILE]] [--extra PRIVATE_LITERALS]
                       [--key-file FILE ...] [--report-dir DIR] [--taint-recheck DIR]

The source archive comes from a clean Git revision. Prepared builder payloads are copied
with their notices and then checked before deterministic archives are written. No IPA or
game-derived build output is published by this tool.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FIXED = (2026, 1, 1, 0, 0, 0)
FIXED_EPOCH = 1767225600
TAINT_PART_BYTES = 50 << 20  # the taint scan indexes every published window: 100 MB at once swapped a busy 16 GB Mac
COMMON = ('COPYING', 'LEGAL.md', 'THIRD-PARTY-NOTICES.md', 'LICENSES')
REPO = 'https://github.com/marcm0de/FivePocket'


def files_under(folder: Path):
    return sorted((p for p in folder.rglob('*') if p.is_file()), key=lambda p: p.relative_to(folder).as_posix())


def stage(work: Path, name: str, readme: str, version: str, payload: dict, extra_readmes=()) -> Path:
    """payload: {name inside the download: source file or folder}; extra_readmes: more packaging/README-*.txt to
    put next to README.txt under their own names (the Mac builder also makes the iOS and Duo apps)."""
    top = work / name
    top.mkdir()
    for inside, src in payload.items():
        src = Path(src)
        if src.is_dir():
            shutil.copytree(src, top / inside, symlinks=False)
        else:
            shutil.copy2(src, top / inside)
    for item in COMMON:
        src = ROOT / item
        (shutil.copytree if src.is_dir() else shutil.copy2)(src, top / item)
    for target, template in [('README.txt', readme)] + [(r, r) for r in extra_readmes]:
        text = (ROOT / 'packaging' / template).read_text().replace('{version}', version)
        (top / target).write_text(text.replace('\n', '\r\n') if 'windows' in name else text)
    return top


def taint_parts(target: Path, work: Path, limit: int = TAINT_PART_BYTES) -> list:
    """Split TARGET into folders of at most LIMIT bytes (hard links, same relative paths) for the taint scan; a
    single file over LIMIT gets a part of its own. Returns [TARGET] when it is small enough already."""
    def size(p):
        return p.stat().st_size if p.is_file() else sum(f.stat().st_size for f in p.rglob('*') if f.is_file())
    if size(target) <= limit:
        return [target]
    units, todo = [], [target]
    while todo:
        for child in sorted(todo.pop().iterdir()):
            (todo if child.is_dir() and size(child) > limit else units).append(child)
    bins, current, used = [], [], 0
    for unit in units:
        n = size(unit)
        if current and used + n > limit:
            bins.append(current)
            current, used = [], 0
        current.append(unit)
        used += n
    bins.append(current)
    parts = []
    for i, group in enumerate(bins):
        part = work / f'{target.name}.part{i + 1:02d}'
        for unit in group:
            for f in ([unit] if unit.is_file() else sorted(x for x in unit.rglob('*') if x.is_file())):
                link = part / f.relative_to(target)
                link.parent.mkdir(parents=True, exist_ok=True)
                os.link(f, link)
        parts.append(part)
    return parts


def check(folder: Path, retail_list, extra, key_files=(), generated_list=None, report_dir=None) -> list:
    """All checks on a staged download; every .apk in it is also unpacked and its contents checked."""
    failures = []
    keys = [a for k in key_files for a in ('--key-file', k)]
    targets = [folder]
    for apk in sorted(folder.rglob('*.apk')):
        inner = folder.parent / f'{folder.name}.unpacked-{apk.stem}'
        with zipfile.ZipFile(apk) as z:
            z.extractall(inner)
        targets.append(inner)
    steps = [(folder, ['scan_names.py', str(folder)])]  # (what is checked, command)
    for target in targets:
        steps += [(target, ['scan_secrets.py', str(target), '--download', *keys]),
                  (target, ['scan_owner.py', str(target)] + (['--extra', extra] if extra else []))]
        if retail_list:
            steps += taint_steps(target, folder.parent, retail_list, generated_list)
    for target, step in steps:
        failures += run_step(target, step, report_dir)
    for inner in targets[1:] + sorted(folder.parent.glob(f'{folder.name}*.part[0-9][0-9]')):
        shutil.rmtree(inner)
    return failures


def taint_steps(target: Path, work: Path, retail_list, generated_list) -> list:
    return [(part, ['taint_scan.py', '--published', str(part), '--retail-list', retail_list] +
             (['--generated-list', generated_list] if generated_list else [])) for part in taint_parts(target, work)]


def run_step(target: Path, step: list, report_dir) -> list:
    out = subprocess.run([sys.executable, str(HERE / step[0]), *step[1:]], capture_output=True, text=True)
    if report_dir:
        (Path(report_dir) / f'{target.name}.{Path(step[0]).stem}.txt').write_text(out.stdout + out.stderr)
    last = (out.stdout.strip().splitlines() or ['(no output)'])[-1]
    print(f'  {target.name}: {step[0]}: {last}', flush=True)
    return [f'{target.name}: {step[0]}: {last}'] if out.returncode else []


def staged_files(folders) -> list:
    """(download, path inside it, SHA-256, bytes or source file) for every file and every .apk member (as
    '<apk path>.unpacked/<member>')."""
    rows = []
    for folder in folders:
        for f in files_under(folder):
            rel = f.relative_to(folder).as_posix()
            data = f.read_bytes()
            rows.append((folder.name, rel, hashlib.sha256(data).hexdigest(), f))
            if f.suffix.lower() == '.apk':
                with zipfile.ZipFile(f) as z:
                    for info in z.infolist():
                        if info.is_dir():
                            continue
                        name = Path(info.filename)
                        if name.is_absolute() or '..' in name.parts:
                            raise ValueError(f'{f.name}: member name {info.filename!r} leaves the archive')
                        inner = z.read(info)
                        rows.append((folder.name, f'{rel}.unpacked/{info.filename}', hashlib.sha256(inner).hexdigest(), inner))
    return rows


def taint_elsewhere(rows, evidence: dict, allowed, lines, failures) -> set:
    """Files of a download scanned by another run of the same checks on the same tree (--taint-elsewhere NAME=FILE). FILE
    lists '#report PATH' lines (that run's taint reports: every one must have found all its controls, and each FINDING is
    rechecked here from the staged bytes with the current allow list), optional '#covers REPORT_NAME PATH' lines (that
    report rescanned PATH, so it replaces the other reports' verdicts for PATH) and 'PATH<TAB>SHA256' lines (the scanned
    tree, as paths inside the download). Returns the (download, path, SHA-256) rows it covers."""
    import re
    sys.path.insert(0, str(HERE))
    import taint_scan
    covered = set()
    for name, file in evidence.items():
        reports, tree, covers = [], {}, {}
        for line in Path(file).read_text().splitlines():
            if line.startswith('#report '):
                reports.append(Path(line[len('#report '):]))
            elif line.startswith('#covers '):
                rname, rel = line[len('#covers '):].split(' ', 1)
                covers.setdefault(rel, set()).add(rname)
            elif line and not line.startswith('#'):
                rel, digest = line.split('\t')
                tree[rel] = digest
        mine = {r[1]: r for r in rows if r[0] == name}
        same = {rel for rel, r in mine.items() if tree.get(rel) == r[2]}
        lines.append(f'{name}: {len(same)} of {len(mine)} staged files identical (path and SHA-256) to the tree scanned by {file}')
        if not reports:
            failures.append(f'taint elsewhere: {file} names no report')
        named = {r.name for r in reports}
        for rel, rnames in covers.items():
            if not rnames <= named:
                failures.append(f'taint elsewhere: {rel} is said to be covered by an unnamed report {sorted(rnames - named)}')
        by_suffix = sorted(tree, key=len, reverse=True)
        for report in reports:
            text = report.read_text(errors='replace')
            verdicts = [v for v in text.splitlines() if v.startswith('TAINT ')]
            lines.append(f'{name}: {report}: ' + ' | '.join(verdicts))
            for v in verdicts or ['(no verdict)']:
                c = re.search(r'controls=(\d+)/(\d+)', v)
                if not c or c.group(1) != c.group(2) or c.group(2) == '0':
                    failures.append(f'taint elsewhere: {report.name}: controls not all found ({v})')
            for m in re.finditer(r'^FINDING (.+) offset (\d+) length (\d+) shared with (\S+) sha256 ([0-9a-f]+)$', text, re.M):
                norm = lambda s: s.replace('/.builder/', '/builder/')     # local-analysis/builder links to .builder
                rel = next((r for r in by_suffix if norm(m.group(1)).endswith(norm('/' + r.split('/', 1)[-1]))), None)
                if covers.get(rel, set()) - {report.name}:     # rescanned by another named report: its verdict counts
                    lines.append(f'REPLACED {name}/{rel} offset {m.group(2)} in {report.name}: rescanned by '
                                 f'{", ".join(sorted(covers[rel] - {report.name}))}')
                    continue
                row = mine.get(rel)
                if row is None:
                    failures.append(f'taint elsewhere: FINDING in {m.group(1)} is not a staged file of {name}')
                    continue
                whole = row[3].read_bytes() if isinstance(row[3], Path) else row[3]
                off, n = int(m.group(2)), int(m.group(3))
                run = whole[off:off + n]
                digest = hashlib.sha256(run).hexdigest()
                ok = digest == m.group(5) and (taint_scan.is_allowed(allowed.get(digest), f'/{rel}', run) or
                                               taint_scan.file_allowed(allowed, f'/{rel}', hashlib.sha256(whole).hexdigest()))
                lines.append(f'{"ALLOWED" if ok else "NOT ALLOWED"} {name}/{rel} offset {off} length {n} sha256 {digest}')
                if not ok:
                    failures.append(f'taint elsewhere: {rel} offset {off} not allowed')
        covered |= {(name, rel, mine[rel][2]) for rel in same}
    return covered


def taint_recheck(rows, work: Path, earlier: Path, report_dir, retail_list, generated_list, evidence=None) -> list:
    """The earlier run's taint verdicts carried over to the staged files it scanned, with the current allow list; files
    it did not scan (new path or bytes) are scanned now (see --taint-recheck)."""
    import re
    sys.path.insert(0, str(HERE))
    import taint_scan
    lines, failures = [], []
    before = [tuple(line.split('\t')) for line in (earlier / 'staged-files.tsv').read_text().splitlines()]
    taken = {}                                  # the earlier union's layout: (path, SHA-256) -> tree number
    for _d, rel, dg in before:
        slot = taken.setdefault(rel.casefold(), {})
        slot.setdefault(dg, len(slot))
    incomplete, members = set(), {}             # parts whose scan did not find all its controls: their files are scanned now
    for report in earlier.glob('taint-union-*.taint_scan.txt'):
        verdict = (report.read_text(errors='replace').strip().splitlines() or [''])[-1]
        c = re.search(r'controls=(\d+)/(\d+)', verdict)
        if not c or c.group(1) != c.group(2) or c.group(2) == '0':
            incomplete.add(report.name[:-len('.taint_scan.txt')])
    if incomplete and (earlier / 'taint-parts.tsv').exists():
        for line in (earlier / 'taint-parts.tsv').read_text().splitlines():
            part, rel = line.split('\t', 1)
            if part in incomplete:
                members.setdefault(int(re.match(r'taint-union-(\d+)', part).group(1)) - 1, set()).add(rel.casefold())
    scanned = set(before)
    elsewhere = taint_elsewhere(rows, evidence or {}, taint_scan.load_allow(str(HERE / 'taint-allow.txt')), lines, failures)
    def carried(r):                             # the scan's result depends only on the path inside a download and the bytes
        if (r[0], r[1], r[2]) in elsewhere:
            return True
        tree = taken.get(r[1].casefold(), {}).get(r[2])
        return tree is not None and r[1].casefold() not in members.get(tree, ())
    same = [r for r in rows if carried(r)]
    changed = [r for r in rows if not carried(r)]
    lines.append(f'staged files: {len(same)} identical (path inside the download and SHA-256) to a file the scanned run or the '
                 f'--taint-elsewhere evidence covered in a part with all controls found (path and '
                 f'SHA-256), {len(changed)} new, changed or from an incomplete part ({", ".join(sorted(incomplete)) or "none"})')
    lines += [f'NEW OR CHANGED {d}/{rel} sha256 {dg}' for d, rel, dg, _ in changed]
    current = {(rel.casefold(), dg): src for _d, rel, dg, src in rows}
    trees = {n for slot in taken.values() for n in slot.values()}
    allowed = taint_scan.load_allow(str(HERE / 'taint-allow.txt'))
    total = left = 0
    reports = sorted(earlier.glob('taint-union-*.taint_scan.txt'))
    covered = {int(re.match(r'taint-union-(\d+)', r.name).group(1)) - 1 for r in reports}
    if covered != trees:
        failures.append(f'taint recheck: reports cover trees {sorted(covered)}, the scanned run had {sorted(trees)}')
    for report in reports:
        text = report.read_text(errors='replace')
        verdict = (text.strip().splitlines() or [''])[-1]
        lines.append(f'{report.name}: {verdict}')
        if report.name[:-len('.taint_scan.txt')] in incomplete:
            if not members:
                failures.append(f'taint recheck: {report.name}: controls not all found and no taint-parts.tsv to rescan its files')
            continue                            # its files are scanned now
        for m in re.finditer(r'^FINDING (.+) offset (\d+) length (\d+) shared with (\S+) sha256 ([0-9a-f]+)$', text, re.M):
            total += 1
            tree = int(re.search(r'/taint-union-(\d+)', m.group(1)).group(1)) - 1
            rel = re.sub(r'^.*/taint-union-\d+(\.part\d+)?/', '', m.group(1))
            src = next((current.get((rel.casefold(), dg)) for dg, n in taken.get(rel.casefold(), {}).items() if n == tree), None)
            if src is None:                     # not in this staging, or staged with other bytes (then scanned now)
                lines.append(f'NOT STAGED {rel} offset {m.group(2)}: the scanned bytes are not part of this release')
                continue
            off, n = int(m.group(2)), int(m.group(3))
            whole = src.read_bytes() if isinstance(src, Path) else src
            run = whole[off:off + n]
            digest = hashlib.sha256(run).hexdigest()
            ok = digest == m.group(5) and (taint_scan.is_allowed(allowed.get(digest), f'/{rel}', run) or
                                           taint_scan.file_allowed(allowed, f'/{rel}', hashlib.sha256(whole).hexdigest()))
            lines.append(f'{"ALLOWED" if ok else "NOT ALLOWED"} {rel} offset {off} length {n} sha256 {digest}')
            left += not ok
    if left:
        failures.append(f'taint recheck: {left} of {total} findings not allowed')
    if changed:
        roots = taint_union(changed, work / 'taint-new')
        for root in roots:
            for part, step in taint_steps(root, work / 'taint-new', retail_list, generated_list):
                failures += run_step(part, step, report_dir)
                lines.append(f'scanned now: {part.name} (see its report)')
    verdict = f'TAINT RECHECK {"PASS" if not failures else "FAIL"} carried={len(same)} findings={total} left={left} scanned_now={len(changed)}'
    if report_dir:
        (Path(report_dir) / 'taint-recheck.txt').write_text('\n'.join(lines + failures + [verdict]) + '\n')
    print(f'  {verdict}', flush=True)
    return failures


def taint_union(rows, work: Path, map_file=None) -> list:
    """The taint scan's input for several downloads: each (path inside a download, SHA-256) once, under
    work/taint-union-N/<path> (hard links; .apk members written out). The published path still ends with the path
    inside the download, so path-scoped allowances apply as in the download itself. MAP_FILE gets one line per
    scanned file: its union path and the downloads it stands for."""
    slots, roots, where = {}, [], {}
    for download, rel, digest, src in rows:
        taken = slots.setdefault(rel.casefold(), {})   # paths that differ only in case are one file on many disks
        if digest in taken:
            where[(taken[digest], rel.casefold())][1].append(download)
            continue
        n = taken[digest] = len(taken)
        where[(n, rel.casefold())] = (rel, [download])
        while len(roots) <= n:
            roots.append(work / f'taint-union-{len(roots) + 1}')
        dest = roots[n] / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(src, Path):
            os.link(src, dest)
        else:
            dest.write_bytes(src)
    if map_file:
        Path(map_file).write_text(''.join(f'taint-union-{n + 1}/{rel}\t{" ".join(sorted(set(d)))}\n'
                                          for (n, _), (rel, d) in sorted(where.items())))
    return roots


def write_zip(folder: Path, archive: Path):
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in files_under(folder):
            info = zipfile.ZipInfo(f'{folder.name}/{p.relative_to(folder).as_posix()}', FIXED)
            info.external_attr = (0o100755 if os.access(p, os.X_OK) else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, p.read_bytes())


def write_tar_xz(folder: Path, archive: Path):
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.PAX_FORMAT) as t:
        for p in files_under(folder):
            info = tarfile.TarInfo(f'{folder.name}/{p.relative_to(folder).as_posix()}')
            info.size = p.stat().st_size
            info.mtime = FIXED_EPOCH
            info.mode = 0o755 if os.access(p, os.X_OK) else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            with p.open('rb') as f:
                t.addfile(info, f)
    archive.write_bytes(lzma.compress(raw.getvalue(), preset=9))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--builder-macos')
    ap.add_argument('--retail-list')
    ap.add_argument('--generated-list')
    ap.add_argument('--extra')
    ap.add_argument('--key-file', action='append', default=[])
    ap.add_argument('--report-dir')
    ap.add_argument('--taint-recheck')
    ap.add_argument('--taint-elsewhere', action='append', default=[],
                    help='DOWNLOAD=EVIDENCE_FILE: that download\'s files scanned by another run (see taint_elsewhere)')
    a = ap.parse_args(argv)
    out = Path(a.out)
    inputs = {'--builder-macos': a.builder_macos}
    missing = [f'{flag} {path}' for flag, path in inputs.items() if path and not Path(path).exists()]
    if missing:
        print('package_release: missing input: ' + ', '.join(missing), file=sys.stderr)
        return 2
    if a.report_dir:
        Path(a.report_dir).mkdir(parents=True, exist_ok=True)
    if out.exists():
        print(f'package_release: {out} exists', file=sys.stderr)
        return 2
    if (ROOT / 'VERSION').read_text().strip() != a.version:
        print('package_release: --version differs from VERSION', file=sys.stderr)
        return 2
    if subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain'], capture_output=True, text=True).stdout.strip():
        print('package_release: the checkout has uncommitted changes', file=sys.stderr)
        return 2
    commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                            check=True).stdout.strip()
    v = a.version
    plans = []  # (staged name, readme template, payload, archive kind[, extra readmes])
    if a.builder_macos:
        plans.append((f'FivePocket-Duo-Builder-{v}-macos-arm64', 'README-builder-macos.txt',
                      {'builder': a.builder_macos}, 'zip', ('README-duo.txt',)))

    with tempfile.TemporaryDirectory(prefix='fivepocket-release-') as tmp:
        work = Path(tmp)
        staged = [(stage(work, name, readme, v, payload, *rest[1:]), rest[0]) for name, readme, payload, *rest in plans]
        source = work / f'FivePocket-{v}-source'
        archive = subprocess.run(['git', '-C', str(ROOT), 'archive', '--format=tar', f'--prefix={source.name}/', commit],
                                 capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(archive)) as t:
            t.extractall(work, filter='data')
        (source / 'SOURCES.txt').write_text(
            f'FivePocket {v} source, commit {commit}.\n'
            f'Repository: {REPO} (release v{v}: {REPO}/releases/tag/v{v}).\n'
            'The upstream revisions and complete changed sources of the translators the builder compiles are in the\n'
            'builder downloads\' builder/sources/ folders; docs/BUILDING.md explains how to build everything.\n')
        staged.append((source, 'tar.xz'))

        failures = []
        for folder, _ in staged:
            failures += check(folder, None, a.extra, a.key_file, None, a.report_dir)
        rows = staged_files([folder for folder, _ in staged])
        if a.report_dir:
            (Path(a.report_dir) / 'staged-files.tsv').write_text(
                ''.join(f'{download}\t{rel}\t{digest}\n' for download, rel, digest, _ in rows))
        if a.taint_recheck:
            if not a.retail_list:
                print('package_release: --taint-recheck needs --retail-list (files it did not scan are scanned now)', file=sys.stderr)
                return 2
            evidence = dict(e.split('=', 1) for e in a.taint_elsewhere)
            failures += taint_recheck(rows, work, Path(a.taint_recheck), a.report_dir, a.retail_list, a.generated_list, evidence)
        elif a.retail_list:
            roots = taint_union(rows, work, Path(a.report_dir) / 'taint-union.tsv' if a.report_dir else None)
            print(f'  taint: {len({(r, d) for _, r, d, _ in rows})} distinct files of {len(rows)} in {len(roots)} trees', flush=True)
            membership = []
            for root in roots:
                for part, step in taint_steps(root, work, a.retail_list, a.generated_list):
                    membership += [f'{part.name}\t{f.relative_to(part).as_posix()}\n' for f in files_under(part)]
                    failures += run_step(part, step, a.report_dir)
            if a.report_dir:                    # which files each scanned part held (for --taint-recheck)
                (Path(a.report_dir) / 'taint-parts.tsv').write_text(''.join(membership))
        if failures:
            print('package_release: checks failed, nothing packed:\n  ' + '\n  '.join(failures))
            return 1
        out.mkdir(parents=True)
        sums = []
        for folder, kind in staged:
            target = out / f'{folder.name}.{kind}'
            (write_zip if kind == 'zip' else write_tar_xz)(folder, target)
            sums.append(f'{hashlib.sha256(target.read_bytes()).hexdigest()}  {target.name}')
        (out / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
        body = (ROOT / 'packaging' / 'RELEASE-NOTES.md').read_text().replace('{version}', v)  # the release page's text
        (out / 'RELEASE-NOTES.md').write_text(
            body + f'\n## Downloads\n\nBuilt from commit [`{commit}`]({REPO}/commit/{commit}).\n\n| Download | SHA-256 |\n|---|---|\n' +
            ''.join(f'| `{s.split("  ")[1]}` | `{s.split("  ")[0]}` |\n' for s in sums) +
            f'\nRelease checks: names, secrets, personal details{", taint (own discs)" if a.retail_list or a.taint_recheck else ""}: PASS.\n')
    print(f'package_release: {len(sums)} downloads in {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
