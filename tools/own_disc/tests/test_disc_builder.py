#!/usr/bin/env python3
"""Tests of disc_builder.py at its command-line boundary, with small synthetic fixtures (no game data).

Each test builds a throw-away "disc" (a folder or a synthetic XISO image), a profile whose command stages are a tiny
fixture script that logs every execution, and calls main([...]) as the command line would. DISC_BUILDER=path selects
the module under test (the mutant runner, mutants.py beside this file, points it at mutated copies).

The XISO fixture is written from the format description. Stage outputs are authored byte strings, so tests check
publication, receipts and recovery without a compiler or game data.
"""
from __future__ import annotations

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(os.environ.get('DISC_BUILDER') or Path(__file__).resolve().parents[1] / 'disc_builder.py')
spec = importlib.util.spec_from_file_location('disc_builder_under_test', TOOL)
db = importlib.util.module_from_spec(spec)
spec.loader.exec_module(db)

XEX = b'FAKE-EXECUTABLE-' + bytes(range(256)) * 3
OTHER_XEX = b'OTHER-EXECUTABLE-' + bytes(range(256)) * 2

# The stand-in for every pipeline tool: step.py LOG NAME KIND OUT [INPUT...]. KIND says which output rule of a real
# tool it imitates.
STEP = r'''
import hashlib, json, os, sys
from pathlib import Path
log, name, kind, out, *ins = sys.argv[1:]
with open(log, 'a') as f:
    f.write(name + '\n')
if any(k.endswith('_MUTANT') for k in os.environ):
    sys.exit(11)                     # a test switch of this project leaked into the tool's environment
h = hashlib.sha256(name.encode())
for i in ins:
    p = Path(i)
    for q in ([p] if p.is_file() else sorted(x for x in p.rglob('*') if x.is_file())):
        h.update(q.read_bytes())
text = h.hexdigest() + ' ' + name
out = Path(out)
if kind == 'none':                   # a tool that reports success without writing anything
    sys.exit(0)
if kind == 'report':                 # writes down how the builder started it
    out.parent.mkdir(parents=True)
    out.write_text(json.dumps({'nice': os.getpriority(os.PRIO_PROCESS, 0), 'seed': os.environ.get('PYTHONHASHSEED'),
                               'cwd': os.getcwd()}))
    sys.exit(0)
if kind == 'fail-late':              # a tool that writes its output and then reports a failure
    out.parent.mkdir(parents=True)
    out.write_text(text)
    sys.exit(7)
if kind == 'strict':                 # tools/xex_image: the output's folder must exist, the output must not
    if not out.parent.is_dir() or out.exists():
        print('output parent directory does not exist, or the output exists')
        sys.exit(2)
    out.write_text(text)
elif kind == 'rewrite':              # a builder that writes its result in place, at a path of its own
    out.write_text(text)
else:                                # the generator wrapper and the recipe tool: the output's folder must be new
    if out.parent.exists():
        sys.exit(9)
    out.parent.mkdir(parents=True)
    if kind == 'dir':
        out.mkdir()
        (out / 'a.txt').write_text(h.hexdigest())
        (out / 'b.txt').write_text(name)
    else:
        out.write_text(text)
'''


def run_main(argv: list[str]) -> tuple[int, str]:
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        code = db.main(argv)
    return code, buffer.getvalue()


def make_xiso(path: Path, files: dict[str, bytes], partition: int = 0, trailer: bool = True,
              root_sector: int | None = None, damaged_links: bool = False) -> dict[str, tuple[int, int]]:
    """A minimal XDVDFS volume: a root directory with the given files and one subdirectory 'sub' holding
    'inner.bin'. Each directory table is a binary search tree by name, as on a pressed disc: an entry's left and
    right links are offsets (in units of 4 bytes) of its subtrees, so some files are reached only through left
    links and others only through right links. Directory tables come first, file data after them in the order
    given. damaged_links: in the root table one leaf entry links back to its parent (a loop) and another links to a
    place behind the end of the table. Returns {name: (first sector, size)}."""
    sector = 2048
    blobs: dict[int, bytes] = {}
    next_sector = 40

    def place(data: bytes) -> int:
        nonlocal next_sector
        start = next_sector
        blobs[start] = data
        next_sector += max(1, -(-len(data) // sector))
        return start

    def table(entries: dict[str, tuple[int, int, int]], damaged: bool = False) -> bytes:
        names = sorted(entries)
        order: list[str] = []                       # the order the records are written in: a node before its subtrees
        links: dict[str, tuple[str | None, str | None]] = {}
        parent: dict[str, str] = {}

        def build(lo: int, hi: int, above: str | None) -> str | None:
            if lo >= hi:
                return None
            mid = (lo + hi) // 2
            order.append(names[mid])
            parent[names[mid]] = above
            links[names[mid]] = (build(lo, mid, names[mid]), build(mid + 1, hi, names[mid]))
            return names[mid]

        build(0, len(names), None)
        offsets, at = {}, 0
        for name in order:
            offsets[name] = at
            at += -(-(14 + len(name)) // 4) * 4
        raw = bytearray(b'\xff' * (-(-at // sector) * sector))
        leaves = [n for n in order if links[n] == (None, None)]
        looping = next(n for n in leaves if offsets[parent[n]]) if damaged else None
        for name in order:
            start, size, attr = entries[name]
            left, right = (offsets[n] // 4 if n else 0 for n in links[name])
            if name == looping:
                right = offsets[parent[name]] // 4          # back to its parent
            elif damaged and name == leaves[-1]:
                left = 0x1FFF                               # 32 KiB into a 2 KiB table
            struct.pack_into('<HHIIBB', raw, offsets[name], left, right, start, size, attr, len(name))
            raw[offsets[name] + 14: offsets[name] + 14 + len(name)] = name.encode('latin-1')
        return bytes(raw)

    root_at, sub_at = place(b'\0' * sector), place(b'\0' * sector)         # one sector each, filled in below
    layout = {'sub/inner.bin': (place(b'nested contents'), len(b'nested contents'))}
    for name, data in files.items():
        layout[name] = (place(data), len(data))
    blobs[sub_at] = table({'inner.bin': (*layout['sub/inner.bin'], 0x20)})
    blobs[root_at] = table({'sub': (sub_at, sector, 0x10), **{n: (*layout[n], 0x20) for n in files}}, damaged_links)
    head = bytearray(sector)
    head[:20] = b'MICROSOFT*XBOX*MEDIA'
    struct.pack_into('<II', head, 20, root_at if root_sector is None else root_sector, sector)
    if trailer:
        head[-20:] = b'MICROSOFT*XBOX*MEDIA'
    with open(path, 'wb') as f:
        f.seek(partition + 32 * sector)
        f.write(head)
        for start, data in blobs.items():
            f.seek(partition + start * sector)
            f.write(data)
        f.truncate(partition + next_sector * sector)
    return layout


class Fixture:
    def __init__(self, tmp: Path):
        self.tmp = tmp
        self.disc = tmp / 'disc'
        self.disc.mkdir()
        (self.disc / 'Default.XEX').write_bytes(XEX)          # case differs on purpose
        self.side = tmp / 'side'
        self.side.mkdir()
        self.step = self.side / 'step.py'
        self.step.write_text(STEP)
        self.helpers = self.side / 'helpers'                  # a tool that is a folder of modules
        self.helpers.mkdir()
        (self.helpers / 'pass_one.py').write_text('RULE = 1\n')
        self.config = self.side / 'config.toml'
        self.config.write_text('flag = 1\n')
        self.pack = self.side / 'pack.bin'
        self.pack.write_bytes(b'shader pack bytes')
        self.log = tmp / 'runs.log'
        self.work = tmp / 'work'
        self.profile_path = tmp / 'profile.json'
        self.report = tmp / 'report.json'
        self.profile = {
            'schema': 'own-disc-pipeline/1', 'id': 'fixture', 'flavour': 'generic',
            'paths': {'repo': str(self.side), 'data': str(self.side)},
            'games': [{'id': 'fake-game', 'label': 'Fake Game (disc)', 'executable': 'default.xex',
                       'executable_sha256': hashlib.sha256(XEX).hexdigest(), 'executable_size': len(XEX)}],
            'prerequisites': [{'id': 'python', 'kind': 'python', 'min': '3.9'}],
            'stages': [
                {'id': 'extract', 'adapter': 'disc-file', 'disc': 'play', 'path': 'default.xex',
                 'outputs': {'xex': 'input/default.xex'}, 'expect': {'xex': '${game:executable_sha256}'}},
                self.command('decode', 'file', 'decode/image.bin', ['${out:extract:xex}']),
                self.command('codegen', 'dir', 'codegen/ppc', ['${out:decode:out}', '${repo}/config.toml'],
                             tools=['${repo}/step.py', '${repo}/helpers']),
                {'id': 'shaders', 'adapter': 'supplied', 'source': '${data}/pack.bin', 'outputs': {'pack': 'shaders/pack.bin'}},
                self.command('link', 'file', 'link/libgame.so', ['${out:codegen:out}']),
            ],
            'output': {'name': 'result.ipa', 'stage': 'link', 'output': 'out'},
        }

    def command(self, sid: str, kind: str, out: str, inputs: list[str], tools: list[str] | None = None) -> dict:
        target = out if out.startswith('/') else '${work}/' + out
        return {'id': sid, 'adapter': 'command', 'tools': tools or ['${repo}/step.py'],
                'inputs': {f'in{i}': v for i, v in enumerate(inputs)}, 'outputs': {'out': out},
                'clean': [] if out.startswith('/') else [out.split('/')[0]],
                'argv': [sys.executable, '${repo}/step.py', str(self.log), sid, kind, target, *inputs]}

    def stage(self, sid: str) -> dict:
        return next(s for s in self.profile['stages'] if s['id'] == sid)

    def run(self, *extra: str, disc: Path | None = None, report: bool = True, nice: str | None = '0') -> tuple[int, str, dict]:
        self.profile_path.write_text(json.dumps(self.profile))
        self.report.unlink(missing_ok=True)
        code, text = run_main(['build', '--profile', str(self.profile_path), '--play-disc', str(disc or self.disc),
                               '--work', str(self.work), *(['--nice', nice] if nice else []),
                               *(['--report', str(self.report)] if report else []), *extra])
        return code, text, (json.loads(self.report.read_text()) if self.report.exists() else {})

    def executed(self) -> list[str]:
        return self.log.read_text().split() if self.log.exists() else []

    def actions(self, report: dict) -> dict[str, str]:
        return {r['stage']: r['action'] for r in report.get('stages', [])}

    def artifact(self) -> Path:
        return self.work / 'out' / 'result.ipa'


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.fx = Fixture(Path(self._tmp.name).resolve())

    def assert_refused_before_any_work(self, code_wanted: int, needle: str, *extra: str, **kw) -> dict:
        code, text, report = self.fx.run(*extra, **kw)
        self.assertEqual(code, code_wanted, text)
        self.assertIn(needle, text)
        self.assertEqual(self.fx.executed(), [])
        self.assertFalse(self.fx.work.exists(), 'the work directory was created')
        return report


class Identification(Base):
    def test_known_executable_builds_from_a_folder(self):
        code, text, report = self.fx.run()
        self.assertEqual(code, 0, text)
        self.assertEqual(self.fx.executed(), ['decode', 'codegen', 'link'])
        self.assertEqual((self.fx.work / 'input/default.xex').read_bytes(), XEX)

    def test_wrong_executable_is_refused_before_any_work(self):
        (self.fx.disc / 'Default.XEX').write_bytes(XEX[:-1] + b'\x00')        # same size, one byte differs
        self.assert_refused_before_any_work(db.EXIT_GAME, 'Unsupported game version')

    def test_disc_without_executable_is_refused(self):
        (self.fx.disc / 'Default.XEX').unlink()
        self.assert_refused_before_any_work(db.EXIT_GAME, 'No game executable')

    def test_identify_command(self):
        self.fx.profile_path.write_text(json.dumps(self.fx.profile))
        argv = ['identify', '--profile', str(self.fx.profile_path), '--play-disc', str(self.fx.disc)]
        code, text = run_main(argv)
        self.assertEqual(code, 0, text)
        self.assertIn(f'Fake Game (disc): default.xex {hashlib.sha256(XEX).hexdigest()[:12]}, {len(XEX)} bytes', text)
        (self.fx.disc / 'Default.XEX').write_bytes(OTHER_XEX)
        self.assertEqual(run_main(argv)[0], db.EXIT_GAME)


class DiscImage(Base):
    FILES = {'other.dat': b'x' * 5000, 'zz.bin': b'tail', 'default.xex': XEX}

    def test_image_layouts_trimmed_and_the_three_full_disc_partitions(self):
        # Game partition offsets of a trimmed image and of full XGD3, XGD2 and XGD1 dumps. With the names above the
        # root table's tree has default.xex behind two left links and zz.bin behind a right link.
        for partition in (0, 0x02080000, 0x0FD90000, 0x18300000):
            with self.subTest(partition=hex(partition)):
                iso = self.fx.tmp / f'play-{partition:x}.iso'
                make_xiso(iso, self.FILES, partition)
                disc = db.open_disc(iso)
                self.assertEqual(db.disc_hash(disc, 'DEFAULT.XEX'), (hashlib.sha256(XEX).hexdigest(), len(XEX)))
                self.assertEqual(db.disc_hash(disc, 'zz.bin'), (hashlib.sha256(b'tail').hexdigest(), 4))
                self.assertEqual(db.disc_hash(disc, 'sub/inner.bin'), (hashlib.sha256(b'nested contents').hexdigest(), 15))
                self.assertIsNone(disc.size('missing.bin'))
                self.assertIsNone(disc.size('sub'))                    # a directory is not a file
                iso.unlink()

    def test_damaged_links_in_a_directory_table_neither_hang_nor_crash_the_reader(self):
        iso = self.fx.tmp / 'links.iso'
        make_xiso(iso, self.FILES, damaged_links=True)
        disc = db.open_disc(iso)
        self.assertEqual([disc.size(n) for n in ('default.xex', 'other.dat', 'zz.bin', 'nothing')], [len(XEX), 5000, 4, None])

    def test_build_from_an_image(self):
        iso = self.fx.tmp / 'play.iso'
        make_xiso(iso, self.FILES, 0x02080000)
        code, text, _ = self.fx.run(disc=iso)
        self.assertEqual(code, 0, text)
        self.assertIn('xiso input', text)
        self.assertEqual((self.fx.work / 'input/default.xex').read_bytes(), XEX)

    def test_image_with_other_executable_is_refused(self):
        iso = self.fx.tmp / 'other.iso'
        make_xiso(iso, {'default.xex': XEX + b'!'})
        self.assert_refused_before_any_work(db.EXIT_GAME, 'Unsupported game version', disc=iso)

    def test_files_that_are_no_disc_image_are_refused(self):
        junk = self.fx.tmp / 'junk.iso'
        junk.write_bytes(b'\0' * 200000)
        self.assert_refused_before_any_work(db.EXIT_GAME, 'no Xbox disc file system', disc=junk)
        make_xiso(junk, self.FILES, trailer=False)                      # the volume record must end with the magic too
        self.assert_refused_before_any_work(db.EXIT_GAME, 'no Xbox disc file system', disc=junk)

    def test_cut_off_image_is_refused_and_named_as_such(self):
        iso = self.fx.tmp / 'cut.iso'
        layout = make_xiso(iso, self.FILES)
        with open(iso, 'r+b') as f:
            f.truncate(layout['default.xex'][0] * 2048 + 100)            # ends inside the executable's data
        code, text, _ = self.fx.run(disc=iso)
        self.assertNotEqual(code, 0)
        self.assertIn('the image is cut off inside this file', text)
        self.assertEqual(self.fx.executed(), [])
        make_xiso(iso, self.FILES, root_sector=5000)                     # the root directory lies behind the end
        code, text, _ = self.fx.run(disc=iso)
        self.assertNotEqual(code, 0)
        self.assertIn('directory outside the image', text)


class Prerequisites(Base):
    def assert_stopped(self, needle: str, *extra: str, **kw) -> dict:
        report = self.assert_refused_before_any_work(db.EXIT_PREREQ, needle, *extra, **kw)
        return report

    def test_missing_tool_of_a_late_stage_stops_everything(self):
        self.fx.stage('link')['tools'] = ['${repo}/linker-that-is-not-installed']
        self.assert_stopped('stage link tool: missing')

    def test_missing_input_file_stops_everything(self):
        self.fx.config.unlink()
        self.assert_stopped('stage codegen input in1: missing')

    def test_pinned_tool_with_other_hash_stops_everything(self):
        self.fx.stage('decode')['tools'] = [{'path': '${repo}/step.py', 'sha256': '0' * 64}]
        self.assert_stopped('not the pinned version')

    def test_tool_that_must_be_executable_and_is_not_stops_everything(self):
        self.fx.stage('decode')['tools'] = [{'path': '${repo}/step.py', 'executable': True}]
        self.assert_stopped('stage decode tool: not executable')
        self.fx.step.chmod(0o755)
        self.assertEqual(self.fx.run()[0], 0)

    def test_version_command_checks(self):
        python = [sys.executable, '-c']
        for label, argv, needle in (
                ('another version', python + ['print("clang 17.0.1")'], 'does not report the expected version'),
                ('the command fails', python + ['print("clang 18.0.3"); raise SystemExit(3)'], 'exited with 3'),
                ('the command is not installed', [str(self.fx.side / 'no-such-compiler'), '--version'], 'cannot run')):
            with self.subTest(label):
                self.fx.profile['prerequisites'] = [{'id': 'compiler', 'kind': 'command', 'argv': argv, 'match': r'clang 18\.'}]
                self.assert_stopped('prerequisite compiler: ')
                self.assert_stopped(needle)
        self.fx.profile['prerequisites'] = [{'id': 'compiler', 'kind': 'command', 'match': r'clang 18\.',
                                             'argv': python + ['print("clang 18.0.3")']}]
        self.assertEqual(self.fx.run()[0], 0)

    def test_apple_toolchain_comes_from_developer_dir_and_must_hold_the_iphoneos_sdk(self):
        from unittest import mock
        xcode = self.fx.side / 'Xcode.app'
        clang = xcode / 'Contents/Developer' / db.XCODE_PARTS['appleclang']
        clang.parent.mkdir(parents=True)
        clang.write_text('')
        self.fx.stage('link')['tools'] = ['${appleclang}']
        clean = {k: v for k, v in os.environ.items() if k not in ('DEVELOPER_DIR', 'BUILDER_XCODE')}
        with mock.patch.dict(os.environ, clean, clear=True):
            self.assert_stopped('Xcode missing: set DEVELOPER_DIR')
        # a Developer folder without the iPhoneOS platform (what xcode-select names when it points at the Command Line Tools)
        with mock.patch.dict(os.environ, dict(clean, DEVELOPER_DIR=str(xcode / 'Contents/Developer')), clear=True):
            self.assert_stopped('holds no iPhoneOS SDK')
        (xcode / 'Contents/Developer' / db.XCODE_PARTS['iossdk']).mkdir(parents=True)
        for given in (xcode / 'Contents/Developer', xcode):          # the Developer folder, or the .app itself
            with self.subTest(given=given.name), mock.patch.dict(os.environ, dict(clean, DEVELOPER_DIR=str(given)), clear=True):
                self.assertEqual(self.fx.run()[0], 0)
        with mock.patch.dict(os.environ, dict(clean, DEVELOPER_DIR=str(self.fx.side), BUILDER_XCODE=str(xcode)), clear=True):
            self.assertEqual(self.fx.run()[0], 0, 'BUILDER_XCODE wins over DEVELOPER_DIR')

    def test_output_profile_publishes_the_exact_stage_file(self):
        self.fx.profile['output'] = {'name': 'Game-${flavour}.ipa', 'stage': 'link', 'output': 'out'}
        code, text, report = self.fx.run()
        self.assertEqual(code, 0, text)
        out = self.fx.work / 'out' / 'Game-generic.ipa'
        self.assertEqual(out.read_bytes(), (self.fx.work / 'link/libgame.so').read_bytes())
        self.assertEqual(report['output']['sha256'], hashlib.sha256(out.read_bytes()).hexdigest())
        self.assertEqual(report['output']['size'], out.stat().st_size)
        for broken, needle in (({'name': 'x.ipa', 'stage': 'link', 'output': 'nope'}, 'names no output of a stage'),
                               ({'name': '../x.ipa', 'stage': 'link', 'output': 'out'}, 'bad output name')):
            with self.subTest(needle=needle):
                self.fx.profile['output'] = broken
                self.assertEqual(self.fx.run()[0], db.EXIT_USAGE)

    def test_python_version_and_unknown_kinds_stop_everything(self):
        self.fx.profile['prerequisites'] = [{'id': 'python', 'kind': 'python', 'min': '99.1'}]
        self.assert_stopped('Python 99.1 or newer needed')
        self.fx.profile['prerequisites'] = [{'id': 'gpu', 'kind': 'graphics-card'}]
        self.assert_stopped("prerequisite gpu: unknown kind 'graphics-card'")

    def test_too_little_disk_space_stops_everything(self):
        self.fx.profile['min_free_bytes'] = 1 << 60
        self.assert_stopped('disk space')

    def test_supplied_files(self):
        self.fx.pack.unlink()
        self.assert_stopped('stage shaders supplied file: missing')
        del self.fx.stage('shaders')['source']
        self.assert_stopped('stage shaders: nothing supplied')
        self.fx.pack.write_bytes(b'shader pack bytes')
        self.assert_stopped('--supply link: no stage of the "supplied" kind', '--supply', f'link={self.fx.pack}',
                            '--supply', f'shaders={self.fx.pack}')
        self.assertEqual(self.fx.run('--supply', f'shaders={self.fx.pack}')[0], 0)

    def test_all_failures_are_listed_together(self):
        self.fx.config.unlink()
        self.fx.pack.unlink()
        report = self.assert_stopped('stage codegen input in1: missing')
        self.assertEqual(sorted(f.split(':')[0] for f in report['prerequisite_failures']),
                         ['stage codegen input in1', 'stage shaders supplied file'])

    def test_stopped_run_without_a_report_option_writes_nothing_at_all(self):
        self.fx.config.unlink()
        before = sorted(p.name for p in self.fx.tmp.iterdir())
        self.assert_stopped('Stopped before any stage ran', report=False)
        self.assertEqual(sorted(p.name for p in self.fx.tmp.iterdir()), sorted(before + ['profile.json']))

    def test_other_disc_files_listed_for_the_game(self):
        game = self.fx.profile['games'][0]
        (self.fx.disc / 'common.rpf').write_bytes(b'archive')
        install = self.fx.tmp / 'install'
        install.mkdir()
        (install / 'content.bin').write_bytes(b'package')
        rpf = {'disc': 'play', 'path': 'common.rpf', 'size': 7, 'sha256': hashlib.sha256(b'archive').hexdigest()}
        pkg = {'disc': 'install', 'path': 'content.bin', 'size': 7}
        for label, files, extra, needle in (
                ('missing on the play disc', [dict(rpf, path='x64.rpf')], [], 'play disc: x64.rpf is missing'),
                ('another size', [dict(rpf, size=8)], [], 'play disc: common.rpf has 7 bytes, expected 8'),
                ('another hash', [dict(rpf, sha256='0' * 64)], ['--hash-disc-files'], 'common.rpf has a different SHA-256'),
                ('missing on the install disc', [dict(pkg, path='other.bin')], ['--install-disc', str(install)],
                 'install disc: other.bin is missing')):
            with self.subTest(label):
                game['disc_files'] = files
                self.assert_stopped(needle, *extra)
        game['disc_files'] = [rpf, pkg]
        code, text, _ = self.fx.run('--hash-disc-files', '--install-disc', str(install))
        self.assertEqual(code, 0, text)

    def test_stage_that_reads_a_disc_that_was_not_given(self):
        self.fx.stage('extract')['disc'] = 'install'
        self.assert_stopped('stage extract: needs the install disc')


class Plan(Base):
    """Mistakes in the profile or the command line stop the run before any stage, not after hours of work."""

    def test_mistakes_in_stages_and_options_are_refused_before_any_work(self):
        def link_input(value):
            self.fx.stage('link')['inputs']['in0'] = value

        cases = (
            ('misspelt variable in the last stage', lambda: link_input('${dat}/config.toml'), [], 'unknown variable ${dat}'),
            ('misspelt variable on a command line', lambda: self.fx.stage('link')['argv'].append('${wrok}/x'), [],
             'unknown variable ${wrok}'),
            ('unknown game field', lambda: link_input('${game:title}'), [], 'unknown game field'),
            ('input taken from a later stage', lambda: self.fx.stage('decode')['inputs'].update(in0='${out:link:out}'), [],
             'names no output of an earlier stage'),
            ('input taken from the stage itself', lambda: link_input('${out:link:out}'), [],
             'names no output of an earlier stage'),
            ('output outside the work directory', lambda: self.fx.stage('decode')['outputs'].update(out='../decoded.bin'), [],
             'is outside the work directory'),
            ('output that is the work directory', lambda: self.fx.stage('decode')['outputs'].update(out='.'), [],
             'is the work directory itself'),
            ('folder to make outside the work directory', lambda: self.fx.stage('decode').update(make_dirs=['../made']), [],
             "folder to make '../made' is outside the work directory"),
            ('misspelt --stop-after', lambda: None, ['--stop-after', 'decod'], '--stop-after decod: the profile has no such stage'),
            ('--supply without a path', lambda: None, ['--supply', 'shaders'], '--supply wants STAGE=PATH'),
        )
        for label, change, extra, needle in cases:
            with self.subTest(label):
                saved = copy.deepcopy(self.fx.profile)
                change()
                self.assert_refused_before_any_work(db.EXIT_USAGE, needle, *extra)
                self.assertFalse((self.fx.tmp / 'made').exists())
                self.fx.profile = saved

    def test_invalid_final_output_is_refused_before_any_stage(self):
        cases = [
            (None, 'output must name a stage file'),
            ([], 'output must name a stage file'),
            ({'name': '../escape.ipa', 'stage': 'link', 'output': 'out'}, 'bad output name'),
            ({'name': 'result.ipa.part', 'stage': 'link', 'output': 'out'}, 'bad output name'),
            ({'name': 'result.ipa', 'stage': 'absent', 'output': 'out'}, 'names no output of a stage'),
            ({'name': 'result.ipa', 'stage': 'link', 'output': 'absent'}, 'names no output of a stage'),
        ]
        for output, message in cases:
            with self.subTest(output=output):
                self.fx.profile['output'] = output
                self.assert_refused_before_any_work(db.EXIT_USAGE, message)

    def test_malformed_profiles_are_refused(self):
        stages = self.fx.profile['stages']
        cases = (
            ('another schema', lambda p: p.update(schema='own-disc-pipeline/2'), 'schema must be own-disc-pipeline/1'),
            ('stage id with a slash', lambda p: p['stages'][1].update(id='../decode'), 'bad stage'),
            ('unknown adapter', lambda p: p['stages'][1].update(adapter='download'), 'bad stage'),
            ('no outputs', lambda p: p['stages'][1].update(outputs={}), 'needs outputs'),
            ('output name with a colon', lambda p: p['stages'][1].update(outputs={'a:b': 'x'}), 'needs outputs'),
            ('command without a command line', lambda p: p['stages'][1].pop('argv'), 'has no argv'),
            ('copy stage with two outputs', lambda p: p['stages'][3]['outputs'].update(second='shaders/b.bin'),
             'declares exactly one output'),
            ('disc stage without a path', lambda p: p['stages'][0].pop('path'), 'names no path on the disc'),
            ('stage id used twice', lambda p: p['stages'][4].update(id='decode'), 'stage ids must be unique'),
            ('no games', lambda p: p.update(games=[]), 'no games table'),
            ('game without a full hash', lambda p: p['games'][0].update(executable_sha256='0123456789ab'), '64 hex digits'),
        )
        self.assertEqual(len(stages), 5)
        for label, change, needle in cases:
            with self.subTest(label):
                saved = copy.deepcopy(self.fx.profile)
                change(self.fx.profile)
                self.assert_refused_before_any_work(db.EXIT_USAGE, needle)
                self.fx.profile = saved

    def test_example_profile_beside_the_tool_is_a_valid_profile(self):
        # The template has placeholder hashes and tool paths: with the fixture's executable hash put in, a dry run
        # must get as far as listing the tools it does not find, and plan all four of its stages.
        example = json.loads((Path(__file__).resolve().parents[1] / 'pipeline-profile.example.json').read_text())
        example['games'][0]['executable_sha256'] = hashlib.sha256(XEX).hexdigest()
        example['paths'] = {'repo': str(self.fx.side), 'data': str(self.fx.side)}
        self.fx.profile = example
        code, text, report = self.fx.run('--dry-run')
        self.assertEqual(code, db.EXIT_PREREQ, text)
        self.assertEqual([r['stage'] for r in report['stages']], ['extract-xex', 'decode-xex', 'shader-pack', 'compile-link'])
        self.assertIn('stage decode-xex tool: missing', text)

    def test_a_stage_can_be_handed_the_disc_given_on_the_command_line(self):
        self.fx.profile['stages'].insert(1, self.fx.command('listing', 'file', 'listing/out.bin', ['${disc:play}/Default.XEX']))
        code, text, _ = self.fx.run()
        self.assertEqual(code, 0, text)
        expected = hashlib.sha256(b'listing')
        expected.update(XEX)
        self.assertEqual((self.fx.work / 'listing/out.bin').read_text().split()[0], expected.hexdigest())

    def test_a_disc_that_was_not_given_cannot_be_named(self):
        self.fx.profile['stages'].insert(1, self.fx.command('listing', 'file', 'listing/out.bin', ['${disc:install}/x']))
        self.assert_refused_before_any_work(db.EXIT_USAGE, '${disc:install} needs that disc on the command line')

    def test_stop_after_runs_up_to_that_stage_and_writes_no_output(self):
        code, text, report = self.fx.run('--stop-after', 'decode')
        self.assertEqual(code, 0, text)
        self.assertEqual(self.fx.executed(), ['decode'])
        self.assertEqual(self.fx.actions(report), {'extract': 'ran', 'decode': 'ran'})
        self.assertFalse(self.fx.artifact().exists())


class Receipts(Base):
    def rerun(self, *extra: str) -> tuple[str, dict]:
        self.fx.log.unlink(missing_ok=True)
        code, text, report = self.fx.run(*extra)
        self.assertEqual(code, 0, text)
        return text, report

    def test_unchanged_stages_are_skipped_and_output_is_identical(self):
        self.assertEqual(self.fx.run()[0], 0)
        first = self.fx.artifact().read_bytes()
        text, report = self.rerun()
        self.assertEqual(self.fx.executed(), [])                       # no command ran a second time
        self.assertEqual(set(self.fx.actions(report).values()), {'skipped'})
        self.assertEqual(self.fx.artifact().read_bytes(), first)

    def test_changed_input_reruns_that_stage_and_all_later_ones(self):
        self.assertEqual(self.fx.run()[0], 0)
        old_lib = (self.fx.work / 'link/libgame.so').read_text()
        self.fx.config.write_text('flag = 2\n')
        text, report = self.rerun()
        self.assertEqual(self.fx.actions(report), {'extract': 'skipped', 'decode': 'skipped', 'codegen': 'ran',
                                                   'shaders': 'ran', 'link': 'ran'})
        self.assertEqual(self.fx.executed(), ['codegen', 'link'])
        self.assertNotEqual((self.fx.work / 'link/libgame.so').read_text(), old_lib)
        self.assertIn('input in1 changed', text)

    def test_changed_tool_reruns_from_the_first_stage_that_uses_it(self):
        self.assertEqual(self.fx.run()[0], 0)
        self.fx.step.write_text(STEP + '\n# new tool version\n')
        _, report = self.rerun()
        self.assertEqual(self.fx.actions(report)['extract'], 'skipped')
        self.assertEqual(self.fx.executed(), ['decode', 'codegen', 'link'])

    def test_tool_folder_counts_its_modules_but_not_interpreter_caches(self):
        self.assertEqual(self.fx.run()[0], 0)
        cache = self.fx.helpers / '__pycache__'
        cache.mkdir()
        (cache / 'pass_one.cpython-313.pyc').write_bytes(b'written by whichever Python ran the tool')
        (self.fx.helpers / '.DS_Store').write_bytes(b'written by the file manager')
        self.rerun()
        self.assertEqual(self.fx.executed(), [])
        (self.fx.helpers / 'pass_one.py').write_text('RULE = 2\n')
        text, _ = self.rerun()
        self.assertEqual(self.fx.executed(), ['codegen', 'link'])
        self.assertIn('tool ${repo}/helpers changed', text)

    def test_changed_stage_definition_reruns_that_stage_and_all_later_ones(self):
        self.assertEqual(self.fx.run()[0], 0)
        self.fx.stage('codegen').update(description='New words about this stage.', note='And a note.')
        self.rerun()
        self.assertEqual(self.fx.executed(), [])                        # words for the reader are no reason to rebuild
        self.fx.stage('codegen')['argv'].append(str(self.fx.pack))      # one more argument on its command line
        text, report = self.rerun()
        self.assertEqual(self.fx.executed(), ['codegen', 'link'])
        self.assertEqual(self.fx.actions(report)['decode'], 'skipped')
        self.assertIn('the stage definition changed', text)

    def test_another_game_executable_reruns_stages_that_do_not_read_the_disc(self):
        # A pipeline whose stages read only side files: the identified executable alone must separate two games.
        profile = self.fx.profile
        profile['games'].append({'id': 'other-game', 'executable_sha256': hashlib.sha256(OTHER_XEX).hexdigest()})
        profile['stages'] = [self.fx.command('decode', 'file', 'decode/image.bin', ['${repo}/config.toml'])]
        profile['output']['stage'] = 'decode'
        self.assertEqual(self.fx.run()[0], 0)
        self.rerun()
        self.assertEqual(self.fx.executed(), [])
        (self.fx.disc / 'Default.XEX').write_bytes(OTHER_XEX)
        text, _ = self.rerun()
        self.assertEqual(self.fx.executed(), ['decode'])
        self.assertIn('a different game executable', text)

    def test_changed_disc_file_reruns_the_stage_that_copies_it(self):
        (self.fx.disc / 'extra.dat').write_bytes(b'one')
        self.fx.profile['stages'].insert(1, {'id': 'extras', 'adapter': 'disc-file', 'disc': 'play', 'path': 'extra.dat',
                                             'outputs': {'file': 'input/extra.dat'}})
        self.assertEqual(self.fx.run()[0], 0)
        (self.fx.disc / 'extra.dat').write_bytes(b'two')
        text, report = self.rerun()
        self.assertEqual((self.fx.actions(report)['extract'], self.fx.actions(report)['extras']), ('skipped', 'ran'))
        self.assertEqual((self.fx.work / 'input/extra.dat').read_bytes(), b'two')
        self.assertIn('input disc:extra.dat changed', text)

    def test_changed_supplied_file_reruns_it_and_later_stages_only(self):
        self.assertEqual(self.fx.run()[0], 0)
        other = self.fx.tmp / 'other-pack.bin'
        other.write_bytes(b'another pack')
        _, report = self.rerun('--supply', f'shaders={other}')
        self.assertEqual(self.fx.executed(), ['link'])
        stage = next(row for row in report['stages'] if row['stage'] == 'shaders')
        self.assertEqual(stage['provenance'], 'supplied')
        self.assertEqual((self.fx.work / 'shaders/pack.bin').read_bytes(), b'another pack')

    def test_modified_output_reruns_its_stage(self):
        self.assertEqual(self.fx.run()[0], 0)
        good = (self.fx.work / 'decode/image.bin').read_text()
        (self.fx.work / 'decode/image.bin').write_text('tampered')
        text, _ = self.rerun()
        self.assertEqual(self.fx.executed(), ['decode'])               # same result again: later receipts still hold
        self.assertEqual((self.fx.work / 'decode/image.bin').read_text(), good)
        self.assertIn('output out was modified', text)
        (self.fx.work / 'decode/image.bin').unlink()
        text, _ = self.rerun()
        self.assertEqual(self.fx.executed(), ['decode'])
        self.assertIn('output out is missing', text)

    def test_modified_file_inside_a_directory_output_reruns_its_stage(self):
        self.assertEqual(self.fx.run()[0], 0)
        (self.fx.work / 'codegen/ppc/b.txt').write_text('edited by hand')
        self.rerun()
        self.assertEqual(self.fx.executed(), ['codegen'])

    def test_receipt_of_another_format_is_not_trusted(self):
        self.assertEqual(self.fx.run()[0], 0)
        path = self.fx.work / 'receipts/decode.json'
        path.write_text(json.dumps(dict(json.loads(path.read_text()), schema='own-disc-receipt/0')))
        self.rerun()
        self.assertEqual(self.fx.executed(), ['decode'])

    def test_moved_work_directory_keeps_its_receipts(self):
        self.assertEqual(self.fx.run()[0], 0)
        first = self.fx.artifact().read_bytes()
        self.fx.work = self.fx.work.rename(self.fx.tmp / 'work-moved')
        self.rerun()
        self.assertEqual(self.fx.executed(), [])
        self.assertEqual(self.fx.artifact().read_bytes(), first)

    def test_failed_stage_stops_and_leaves_no_receipt(self):
        self.fx.stage('codegen')['argv'][4] = 'fail-late'               # the tool writes its output, then exits with 7
        code, text, report = self.fx.run()
        self.assertEqual(code, db.EXIT_STAGE, text)
        self.assertIn('stage codegen: command exited with 7', text)
        self.assertEqual(self.fx.executed(), ['decode', 'codegen'])     # link never started
        self.assertFalse((self.fx.work / 'receipts/codegen.json').exists())
        self.assertFalse(self.fx.artifact().exists())

    def test_command_that_succeeds_without_writing_its_output_fails_the_stage(self):
        self.fx.stage('decode')['argv'][4] = 'none'
        code, text, _ = self.fx.run()
        self.assertEqual(code, db.EXIT_STAGE, text)
        self.assertIn('output out was not produced', text)
        self.assertEqual(self.fx.executed(), ['decode'])
        self.assertFalse((self.fx.work / 'receipts/decode.json').exists())

    def test_output_that_differs_from_the_expected_hash_fails_the_stage(self):
        self.fx.stage('decode')['expect'] = {'out': 'ab' * 32}
        code, text, _ = self.fx.run()
        self.assertEqual(code, db.EXIT_STAGE, text)
        self.assertIn('the profile expects', text)
        self.assertFalse((self.fx.work / 'receipts/decode.json').exists())
        self.assertEqual(self.fx.executed(), ['decode'])

    def test_project_test_switches_do_not_reach_the_tools(self):
        os.environ['HOST_TRACE_MUTANT'] = '1'
        self.addCleanup(os.environ.pop, 'HOST_TRACE_MUTANT')
        code, text, _ = self.fx.run()
        self.assertEqual(code, 0, text)


class ToolStart(Base):
    def test_tools_run_niced_in_the_repository_folder_with_a_fixed_hash_seed(self):
        decode = self.fx.stage('decode')
        decode['argv'][4] = 'report'
        own = os.getpriority(os.PRIO_PROCESS, 0)                         # these tests may themselves run niced
        code, text, _ = self.fx.run('--stop-after', 'decode', nice=None)        # the default niceness
        self.assertEqual(code, 0, text)
        self.assertEqual(json.loads((self.fx.work / 'decode/image.bin').read_text()),
                         {'nice': min(20, own + 10), 'seed': '0', 'cwd': str(self.fx.side)})
        decode['cwd'] = '${work}/input'
        code, text, _ = self.fx.run('--stop-after', 'decode')
        self.assertEqual(code, 0, text)
        self.assertEqual(json.loads((self.fx.work / 'decode/image.bin').read_text()),
                         {'nice': own, 'seed': '0', 'cwd': str(self.fx.work / 'input')})


class ToolOutputRules(Base):
    def test_tool_that_wants_an_existing_output_folder_gets_it_from_make_dirs(self):
        # tools/xex_image refuses to run unless the output's folder exists and the output does not.
        self.fx.stage('decode')['argv'][4] = 'strict'
        code, text, _ = self.fx.run()
        self.assertEqual(code, db.EXIT_STAGE, text)                    # the fixture really has the rule
        self.assertIn('stage decode: command exited with 2', text)
        self.fx.stage('decode')['make_dirs'] = ['decode']
        self.fx.work = self.fx.tmp / 'work-fresh'
        self.fx.log.unlink()
        code, text, _ = self.fx.run()
        self.assertEqual(code, 0, text)
        self.fx.step.write_text(STEP + '\n# changed\n')                  # rerun over the old output: it is removed first
        code, text, _ = self.fx.run()
        self.assertEqual(code, 0, text)
        self.assertEqual(self.fx.executed(), ['decode', 'codegen', 'link', 'decode', 'codegen', 'link'])


class ExternalOutputs(Base):
    """A tool that can only write its result at a place of its own, outside the work directory."""

    def setUp(self):
        super().setUp()
        self.library = self.fx.tmp / 'attempt' / 'build' / 'libgame.so'
        self.library.parent.mkdir(parents=True)
        self.library.write_text('library of an older build')
        self.fx.profile['stages'][4] = dict(self.fx.command('link', 'rewrite', str(self.library), ['${out:codegen:out}']),
                                            external_outputs=True)

    def test_rewritten_file_is_taken_and_the_one_it_replaced_is_kept(self):
        code, text, _ = self.fx.run()
        self.assertEqual(code, 0, text)
        new = self.library.read_bytes()
        self.assertNotEqual(new, b'library of an older build')
        kept = list((self.fx.work / 'backup' / 'link').iterdir())
        self.assertEqual([p.read_bytes() for p in kept], [b'library of an older build'])
        self.assertEqual(self.fx.artifact().read_bytes(), new)
        receipt = json.loads((self.fx.work / 'receipts/link.json').read_text())
        self.assertEqual(receipt['outputs']['out']['sha256'], hashlib.sha256(new).hexdigest())
        self.assertEqual(receipt['provenance'], 'built')
        self.fx.log.unlink()
        code, text, report = self.fx.run()                              # unchanged: covered by the receipt
        self.assertEqual((code, self.fx.executed()), (0, []), text)
        self.library.write_text('replaced by somebody else')            # changed behind the builder's back: rebuilt
        code, text, _ = self.fx.run()
        self.assertEqual((code, self.fx.executed()), (0, ['link']), text)
        self.assertEqual(self.library.read_bytes(), new)
        self.assertEqual(sorted(p.read_bytes() for p in (self.fx.work / 'backup' / 'link').iterdir()),
                         [b'library of an older build', b'replaced by somebody else'])

    def test_older_file_the_command_did_not_write_is_not_taken_as_built(self):
        self.fx.stage('link')['argv'][4] = 'none'
        code, text, _ = self.fx.run()
        self.assertEqual(code, db.EXIT_STAGE, text)
        self.assertIn('an older file there is not accepted as built by this run', text)
        self.assertEqual(self.library.read_text(), 'library of an older build')
        self.assertFalse((self.fx.work / 'receipts/link.json').exists())
        self.assertFalse(self.fx.artifact().exists())

    def test_only_a_stage_that_says_so_may_write_outside_the_work_directory(self):
        del self.fx.stage('link')['external_outputs']
        self.assert_refused_before_any_work(db.EXIT_USAGE, 'output out of stage link')
        self.fx.stage('link')['external_outputs'] = True
        self.fx.stage('shaders').update(outputs={'pack': str(self.fx.tmp / 'attempt' / 'pack.bin')}, external_outputs=True)
        self.assert_refused_before_any_work(db.EXIT_USAGE, 'output pack of stage shaders')     # a copy stage never may


class WorkDirectory(Base):
    def test_clean_target_outside_the_work_directory_is_refused_and_untouched(self):
        self.fx.stage('decode')['clean'] = ['../side']
        self.assert_refused_before_any_work(db.EXIT_USAGE, "clean target '../side' is outside the work directory")
        self.assertTrue(self.fx.step.exists())

    def test_folder_with_somebody_elses_files_is_not_taken_as_work_directory(self):
        notes = self.fx.work / 'codegen' / 'my-notes' / 'keep.txt'      # "codegen" is a folder the pipeline cleans
        notes.parent.mkdir(parents=True)
        notes.write_text('mine')
        for extra in ([], ['--dry-run']):
            code, text, _ = self.fx.run(*extra)
            self.assertEqual(code, db.EXIT_USAGE, text)
            self.assertIn('already holds files and was not made by this builder', text)
        self.assertEqual((notes.read_text(), self.fx.executed()), ('mine', []))
        self.assertEqual([p.name for p in self.fx.work.iterdir()], ['codegen'])

    def test_empty_folder_and_the_builders_own_folder_are_taken(self):
        self.fx.work.mkdir()
        code, text, _ = self.fx.run(report=False)
        self.assertEqual(code, 0, text)
        self.assertEqual(json.loads((self.fx.work / 'last-run.json').read_text())['exit_code'], 0)
        self.assertEqual(self.fx.run()[0], 0)

    def test_nothing_game_derived_is_written_into_a_git_checkout_outside_local_analysis(self):
        repo = self.fx.tmp / 'checkout'
        (repo / '.git').mkdir(parents=True)
        private = repo / 'local-analysis'
        library = repo / 'src' / 'libgame.so'
        library.parent.mkdir()

        def external_link():
            self.fx.profile['stages'][4] = dict(self.fx.command('link', 'rewrite', str(library), ['${out:codegen:out}']),
                                                external_outputs=True)

        cases = (('work directory', lambda: setattr(self.fx, 'work', repo / 'build'), [], 'work directory'),
                 ('output directory', lambda: None, ['--out', str(repo / 'dist')], 'output directory'),
                 ('report file', lambda: setattr(self.fx, 'report', repo / 'docs' / 'run.json'), [], 'report file'),
                 ('external stage output', external_link, [], 'output out of stage link'))
        for label, change, extra, needle in cases:
            with self.subTest(label):
                saved = copy.deepcopy(self.fx.profile)
                self.fx.work, self.fx.report = private / 'build', self.fx.tmp / 'report.json'
                change()
                code, text, _ = self.fx.run(*extra)
                self.assertEqual(code, db.EXIT_USAGE, text)
                self.assertIn(needle, text)
                self.assertIn('refusing to write game-derived files there', text)
                self.assertEqual(self.fx.executed(), [])
                self.assertEqual(sorted(p.name for p in repo.rglob('*')), ['.git', 'src'])
                self.fx.profile = saved
        self.fx.work, self.fx.report = private / 'build', private / 'reports' / 'run.json'
        code, text, _ = self.fx.run('--out', str(private / 'dist'))
        self.assertEqual(code, 0, text)
        self.assertTrue((private / 'dist' / 'result.ipa').is_file())


class HeavyAndDryRun(Base):
    def test_heavy_stage_is_not_run_without_permission(self):
        self.fx.stage('codegen')['heavy'] = True
        code, text, report = self.fx.run()
        self.assertEqual(code, db.EXIT_HEAVY, text)
        self.assertEqual(self.fx.executed(), ['decode'])
        self.assertFalse(self.fx.artifact().exists())
        code, text, _ = self.fx.run('--allow-heavy')
        self.assertEqual(code, 0, text)
        self.assertEqual(self.fx.executed(), ['decode', 'codegen', 'link'])
        code, text, report = self.fx.run()                              # a receipt covers it: no permission needed
        self.assertEqual(code, 0, text)
        self.assertEqual(self.fx.actions(report)['codegen'], 'skipped')

    def test_dry_run_runs_and_writes_nothing(self):
        code, text, report = self.fx.run('--dry-run')
        self.assertEqual(code, 0, text)
        self.assertEqual(self.fx.executed(), [])
        self.assertFalse(self.fx.work.exists())
        self.assertEqual(set(self.fx.actions(report).values()), {'would-run'})

    def test_dry_run_after_a_build_reports_what_a_change_would_rerun(self):
        self.assertEqual(self.fx.run()[0], 0)
        self.fx.config.write_text('flag = 3\n')
        self.fx.log.unlink()
        code, text, report = self.fx.run('--dry-run')
        self.assertEqual(code, 0, text)
        self.assertEqual(self.fx.actions(report), {'extract': 'skipped', 'decode': 'skipped', 'codegen': 'would-run',
                                                   'shaders': 'would-run', 'link': 'would-run'})
        self.assertEqual(self.fx.executed(), [])

    def test_dry_run_reports_missing_prerequisites_with_exit_2(self):
        self.fx.config.unlink()
        code, text, report = self.fx.run('--dry-run')
        self.assertEqual(code, db.EXIT_PREREQ, text)
        self.assertEqual(len(report['prerequisite_failures']), 1)
        self.assertEqual(len(report['stages']), 5)                      # the plan is still shown


if __name__ == '__main__':
    unittest.main(verbosity=2)
