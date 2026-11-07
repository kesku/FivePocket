#!/usr/bin/env python3
"""Build the XMA decoder libraries (FFmpeg's libavcodec and libavutil, decoders only) on any host, without sh or make.

tools/host_guest_trace/build_xma_decoder.sh does this on a Mac with FFmpeg's own configure and make. This tool does
what that make does, from a plan (xma_decoder/plan.json beside this file): the pinned source archive is unpacked into
a new folder, the four files configure and the build scripts write are laid over it (xma_decoder/configured/), each
of the plan's units is compiled with the NDK's clang by the plan's exact arguments, and the two archives are made
with the NDK's llvm-ar in the plan's member order. The result is compared with the plan: on the first machine's kind
of host the libraries are the ones the shell route gives, byte for byte (the plan's hashes).

On another host they may differ from those in one thing, and the plan says how they are recognised then. The NDK's
clang writes its own name into every object (`.comment`), and that name is not the same in the NDK for macOS and the
NDK for Windows ("Android (12470979, +pgo, -bolt, +lto, -mlgo, based on r522817c) clang version 18.0.3 ..." against
"Android (12470979, based on r522817c) clang version 18.0.3 ..."). Measured on 2026-10-09: nothing else differs in
any of the 140 objects. So where the plan gives an identity for a library (elf_sections.archive_identity: its
objects in order, each without that one section), a library with other bytes and that identity is the plan's too.
(Leaving the name out with -fno-ident was tried and is wrong here: it renumbers local symbols in the linked game
library, whose bytes must not change.) identity.json in the output folder holds the identities found; it is the
same file on every host.

    build_xma_decoder.py --source-archive FILE --ndk DIR --output NEW_DIR [--plan FILE] [--configured DIR] [--jobs N]
    build_xma_decoder.py --source-archive FILE --xcode DIR --output NEW_DIR --plan xma_decoder/plan-apple.json
                         --configured xma_decoder/configured-apple [--jobs N] [--record]

An Apple plan (target arm64-apple-ios...) compiles with the clang of the given Xcode (its Developer folder), adding
-target and that Xcode's iPhoneOS SDK, and archives with its ar and ranlib under ZERO_AR_DATE=1 (no time or owner in
the archive). Apple clang's output is the same on a Mac with the same Xcode, so the plan's hashes are pinned for one
Xcode version (plan 'xcode'); --record writes the hashes a first build made into a plan whose 'expect' is empty.

--source-archive is `git archive --format=tar <revision>` of https://github.com/xenia-project/FFmpeg at the plan's
revision. The output folder must not exist. Nothing here reads the game. Exit 0: both libraries equal the plan's;
1: built, and a library differs (report.json says which; the folder is kept for looking); 2: refused before building
(a wrong archive, a changed configured file, an unsafe member name, a missing compiler, an existing output folder).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tarfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import elf_sections  # noqa: E402
SCHEMA = 'xma-decoder-plan/1'
HOST_TAGS = {'darwin': 'darwin-x86_64', 'win32': 'windows-x86_64', 'linux': 'linux-x86_64'}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def is_hash(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def inside(name) -> bool:
    """A name that stays in the build folder: relative, forward slashes, no drive, no backslash, never above the folder."""
    if not isinstance(name, str) or not name or '\\' in name or ':' in name or name.startswith('/'):
        return False
    depth = 0
    for part in PurePosixPath(name).parts:
        depth += -1 if part == '..' else 1
        if depth < 0:
            return False
    return True


def check_plan(plan) -> dict:
    if not isinstance(plan, dict) or plan.get('schema') != SCHEMA:
        raise ValueError('plan refused: it needs the schema ' + SCHEMA)
    if not is_hash(plan.get('source_archive_sha256')) or not isinstance(plan.get('target'), str) \
            or not plan['target'].startswith(('aarch64-linux-android', 'arm64-apple-ios')):
        raise ValueError('plan refused: the source archive hash or the target is missing')
    configured, sets, units, archives, expect = (plan.get(k) for k in ('configured', 'flag_sets', 'units', 'archives', 'expect'))
    if not isinstance(configured, dict) or not configured or not all(inside(n) and is_hash(h) for n, h in configured.items()):
        raise ValueError('plan refused: the configured files need names inside the build folder and hashes')
    if not isinstance(sets, list) or not sets or not all(isinstance(s, list) and all(isinstance(f, str) for f in s) for s in sets):
        raise ValueError('plan refused: flag sets are lists of texts')
    for flags in sets:       # the arguments a unit adds are named by the tool itself; a flag may not name an output or another input
        if any(f in ('-o', '-c', '-MF', '-MT') or f.startswith(('/', '@')) or '..' in f for f in flags):
            raise ValueError('plan refused: a flag set names a file or an output')
    if not isinstance(units, list) or not units:
        raise ValueError('plan refused: no units')
    objects = set()
    for unit in units:
        if not isinstance(unit, dict) or not inside(unit.get('source')) or not inside(unit.get('object')) \
                or not unit['source'].endswith(('.c', '.S')) or not unit['object'].endswith('.o') \
                or type(unit.get('flags')) is not int or not 0 <= unit['flags'] < len(sets) or unit['object'] in objects:
            raise ValueError('plan refused: a unit needs a source, its own object and a flag set')
        objects.add(unit['object'])
    if not isinstance(archives, list) or not archives or not isinstance(expect, dict):
        raise ValueError('plan refused: no archives')
    listed = set()
    for archive in archives:
        if not isinstance(archive, dict) or not inside(archive.get('path')) or not archive['path'].endswith('.a') \
                or not isinstance(archive.get('members'), list) or not archive['members'] \
                or listed & set(archive['members']) or len(set(archive['members'])) != len(archive['members']):
            raise ValueError('plan refused: an object goes into one archive, once')
        listed |= set(archive['members'])
        if expect and not is_hash(expect.get(archive['path'])):
            raise ValueError('plan refused: an archive without an expected hash')
    if listed != objects or (expect and set(expect) != {a['path'] for a in archives}):
        raise ValueError('plan refused: every object goes into one archive, and every archive is expected')
    identity = plan.get('identity')
    if identity is not None and (not isinstance(identity, dict) or set(identity) != set(expect) or not all(is_hash(v) for v in identity.values())):
        raise ValueError('plan refused: identities are given for every archive or for none')
    return plan


def tools(ndk: Path, platform: str = sys.platform) -> dict:
    """The NDK's own clang and archive tools for this host. The NDK's target wrapper is a shell script that calls clang
    with --target; calling clang with --target is the same on every host."""
    tag = HOST_TAGS.get('linux' if platform.startswith('linux') else platform)
    if tag is None:
        raise ValueError(f'no NDK host for {platform}')
    suffix = '.exe' if platform == 'win32' else ''
    folder = ndk / 'toolchains' / 'llvm' / 'prebuilt' / tag / 'bin'
    found = {name: folder / (name + suffix) for name in ('clang', 'llvm-ar', 'llvm-ranlib')}
    for path in found.values():
        if not path.is_file():
            raise ValueError(f'the NDK tool is missing: {path}')
    return found


def unpack(archive: Path, output: Path) -> int:
    """Every regular file and folder of the archive into output. Anything else, or a name that leaves it, refuses all."""
    with tarfile.open(archive, 'r:') as tar:
        members = [m for m in tar.getmembers() if m.name != 'pax_global_header']
        for member in members:
            if not (member.isfile() or member.isdir()) or not inside(member.name.rstrip('/')):
                raise ValueError(f'source archive refused: {member.name!r} is not a plain file or folder inside the build folder')
        output.mkdir(parents=True)
        count = 0
        for member in members:
            target = output.joinpath(*PurePosixPath(member.name).parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as source:
                target.write_bytes(source.read())
            count += 1
    return count


def apple(plan: dict) -> bool:
    return plan['target'].startswith('arm64-apple-ios')


def apple_tools(xcode: Path) -> dict:
    """the clang, ar and ranlib of an Xcode, and its iPhoneOS SDK (named 'sdk')"""
    bin_dir = xcode / 'Toolchains' / 'XcodeDefault.xctoolchain' / 'usr' / 'bin'
    found = {'clang': bin_dir / 'clang', 'llvm-ar': bin_dir / 'ar', 'llvm-ranlib': bin_dir / 'ranlib',
             'sdk': xcode / 'Platforms' / 'iPhoneOS.platform' / 'Developer' / 'SDKs' / 'iPhoneOS.sdk'}
    for path in found.values():
        if not path.exists():
            raise ValueError(f'the Xcode tool is missing: {path} (is the folder Xcode\'s Contents/Developer?)')
    return found


def unit_command(plan: dict, unit: dict, clang: Path, sdk: Path | None = None) -> list[str]:
    obj = unit['object']
    target = ['-target', plan['target'], '-isysroot', str(sdk)] if apple(plan) else ['--target=' + plan['target']]
    return [str(clang), *target, *plan['flag_sets'][unit['flags']],
            '-MMD', '-MF', obj[:-2] + '.d', '-MT', obj, '-c', '-o', obj, unit['source']]


def build(plan: dict, archive: Path, configured: Path, ndk: Path | None, output: Path, jobs: int = 3, platform: str = sys.platform,
          xcode: Path | None = None) -> dict:
    """Unpacks, lays the configured files over, compiles and archives. Returns the report (also written as report.json)."""
    if output.exists():
        raise ValueError('the output folder exists already')
    if sha_file(archive) != plan['source_archive_sha256']:
        raise ValueError('the source archive is not the one the plan names')
    for name, expected in plan['configured'].items():
        path = configured.joinpath(*PurePosixPath(name).parts)
        if not path.is_file() or sha_file(path) != expected:
            raise ValueError(f'the configured file {name} is not the one the plan names')
    if apple(plan):
        if xcode is None:
            raise ValueError('an Apple plan needs --xcode')
        tool = apple_tools(xcode)
    else:
        tool = tools(ndk, platform)
    env = dict(os.environ, ZERO_AR_DATE='1', **({'DEVELOPER_DIR': str(xcode)} if xcode else {}))
    version = subprocess.run([str(tool['clang']), '--version'], capture_output=True, text=True)
    if version.returncode != 0:
        raise ValueError('the NDK compiler does not start')
    files = unpack(archive, output)
    for name in plan['configured']:        # what configure and the build scripts write replaces the revision's own files
        target = output.joinpath(*PurePosixPath(name).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(configured.joinpath(*PurePosixPath(name).parts).read_bytes())

    def compile_unit(unit: dict) -> None:
        output.joinpath(*PurePosixPath(unit['object']).parts[:-1]).mkdir(parents=True, exist_ok=True)
        if not output.joinpath(*PurePosixPath(unit['source']).parts).is_file():
            raise ValueError(f'the source of a unit is not in the archive: {unit["source"]}')
        r = subprocess.run(unit_command(plan, unit, tool['clang'], tool.get('sdk')), cwd=output, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            raise ValueError(f'{unit["source"]} did not compile: ' + (r.stderr or r.stdout).strip()[-600:])

    with ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        list(pool.map(compile_unit, plan['units']))
    report = {'schema': 1, 'status': 'complete', 'revision': plan.get('revision'), 'source_files': files, 'units': len(plan['units']),
              'compiler': version.stdout.splitlines()[0] if version.stdout else '', 'archives': {}}
    for entry in plan['archives']:
        path = output.joinpath(*PurePosixPath(entry['path']).parts)
        ar = [str(tool['llvm-ar']), 'rc' if apple(plan) else 'rcD', entry['path'], *entry['members']]
        for command in (ar, [str(tool['llvm-ranlib']), entry['path']]):
            r = subprocess.run(command, cwd=output, capture_output=True, text=True, env=env)
            if r.returncode != 0:
                raise ValueError(f'{entry["path"]} was not made: ' + (r.stderr or r.stdout).strip()[-600:])
        got = sha_file(path)
        same = got == plan['expect'].get(entry['path'])
        report['archives'][entry['path']] = {'bytes': path.stat().st_size, 'sha256': got, 'as_the_plan_expects': same}
        if plan.get('identity'):         # the same objects, whatever the compiler calls itself on this host
            found, members = elf_sections.archive_identity(path)
            report['archives'][entry['path']].update(identity=found, members=members, same_bytes_as_the_first_machine=same,
                                                     as_the_plan_expects=found == plan['identity'][entry['path']])
        if not report['archives'][entry['path']]['as_the_plan_expects']:
            report['status'] = 'differs'
    if plan.get('identity'):
        found = {path: entry['identity'] for path, entry in sorted(report['archives'].items())}
        (output / 'identity.json').write_text(json.dumps({'schema': 'xma-decoder-identity/1', 'archives': found}, indent=1, sort_keys=True) + '\n',
                                              encoding='utf-8', newline='\n')
    (output / 'report.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--source-archive', type=Path, required=True)
    ap.add_argument('--ndk', type=Path)
    ap.add_argument('--xcode', type=Path)
    ap.add_argument('--record', action='store_true')
    ap.add_argument('--development-toolchain', action='store_true',
                    help='accept a successfully compiled Apple library while reporting release identity differences')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--plan', type=Path, default=HERE / 'xma_decoder' / 'plan.json')
    ap.add_argument('--configured', type=Path, default=HERE / 'xma_decoder' / 'configured')
    ap.add_argument('--jobs', type=int, default=3)
    a = ap.parse_args(argv)
    plan = check_plan(json.loads(a.plan.read_text(encoding='utf-8')))
    if a.development_toolchain and (not apple(plan) or a.record):
        raise ValueError('--development-toolchain requires an Apple plan and cannot be combined with --record')
    if not plan['expect'] and not a.record:
        raise ValueError('the plan expects no hashes yet: run once with --record')
    if (a.ndk is None) == (a.xcode is None):
        raise ValueError('give --ndk (an Android plan) or --xcode (an Apple plan)')
    report = build(plan, a.source_archive, a.configured, a.ndk, a.output.absolute(), a.jobs, xcode=a.xcode)
    if a.development_toolchain:
        report['verification'] = 'development'
        report['release_equivalent'] = report['status'] == 'complete'
        (a.output / 'report.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
        print('DEVELOPMENT BUILD: decoder source verified; release code identity is not certified.', flush=True)
    if a.record:
        if plan['expect']:
            raise ValueError('--record writes into a plan without hashes only')
        plan['expect'] = {path: entry['sha256'] for path, entry in report['archives'].items()}
        if xcode_version := subprocess.run(['/usr/bin/xcodebuild', '-version'], capture_output=True, text=True,
                                           env=dict(os.environ, DEVELOPER_DIR=str(a.xcode))).stdout.split('\n')[:2]:
            plan['xcode'] = ' / '.join(x.strip() for x in xcode_version if x.strip())
        a.plan.write_text(json.dumps(plan, indent=1) + '\n', encoding='utf-8', newline='\n')
        print('XMA_PLAN recorded', ', '.join(f'{p} {h[:16]}' for p, h in plan['expect'].items()), flush=True)
        return 0
    for path, entry in report['archives'].items():
        verdict = 'DIFFERS' if not entry['as_the_plan_expects'] else 'as expected' if entry.get('same_bytes_as_the_first_machine', True) \
            else 'as expected (the first machine\'s objects; this host\'s compiler name in them)'
        print('XMA_ARCHIVE', path, entry['bytes'], entry['sha256'], verdict, flush=True)
    print('XMA_DECODER', report['status'], f'{report["units"]} units from revision {report["revision"]}', flush=True)
    return 0 if report['status'] == 'complete' or a.development_toolchain else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError, tarfile.TarError) as error:      # (a library that is no archive of objects: ValueError too)
        print('XMA_DECODER_REFUSED:', error, file=sys.stderr)
        sys.exit(2)
