#!/usr/bin/env python3
"""build_xma_decoder.py on a synthetic source archive, plan and stand-in NDK tools (no FFmpeg, no compiler, no game).
XMA_DECODER_TOOL names another copy of the tool (the mutant runner beside this file). The stand-in clang writes what
it was asked into the object, so the tests see the exact arguments, the working folder and the files a unit read."""
import copy
import hashlib
import importlib.util
import inspect
import io
import json
import os
import stat
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOL = Path(os.environ.get('XMA_DECODER_TOOL', Path(__file__).resolve().parents[1] / 'build_xma_decoder.py'))
spec = importlib.util.spec_from_file_location('build_xma_decoder_under_test', TOOL)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

CLANG = '''#!/usr/bin/env python3
import os, sys
a = sys.argv[1:]
if a == ['--version']:
    print('stand-in clang 1.0'); sys.exit(0)
src, obj, dep = a[-1], a[a.index('-o') + 1], a[a.index('-MF') + 1]
text = open(src, 'rb').read()
if b'BROKEN' in text:
    sys.stderr.write('stand-in: cannot compile ' + src); sys.exit(1)
config = open('config.h', 'rb').read()
open(obj, 'wb').write(('OBJ cwd=' + os.path.basename(os.getcwd()) + ' args=' + ' '.join(a) + '\\n').encode() + text + config)
open(dep, 'w').write(obj + ': ' + src + ' config.h\\n')
'''
AR = '''#!/usr/bin/env python3
import sys
a = sys.argv[1:]
assert a[0] == 'rcD', a
open(a[1], 'wb').write(b''.join(('MEMBER ' + n + '\\n').encode() + open(n, 'rb').read() for n in a[2:]))
'''
RANLIB = '''#!/usr/bin/env python3
import sys
open(sys.argv[1], 'ab').write(b'INDEXED\\n')
'''
sha = lambda data: hashlib.sha256(data).hexdigest()
sys.path.insert(0, str(Path(__file__).resolve().parent))
import elf_fixtures as replay_tests
# Stand-ins that write real objects and a real archive: the object holds what the compiler was asked and read, and, for
# a unit compiled from C, the compiler's own name as the file compiler-name beside the stand-in gives it (the host).
CLANG_OBJECTS = '''#!/usr/bin/env python3
import os, struct, sys
PROGBITS, RELA, NOBITS, STRTAB = 1, 4, 8, 3
ALLOC, EXEC = 2, 4
%s
a = sys.argv[1:]
if a == ['--version']:
    print('stand-in clang 1.0'); sys.exit(0)
src, obj, dep = a[-1], a[a.index('-o') + 1], a[a.index('-MF') + 1]
text = (' '.join(a) + '\\n').encode() + open(src, 'rb').read() + open('config.h', 'rb').read()
sections = [('.text', PROGBITS, ALLOC | EXEC, text)]
if src.endswith('.c'):
    name = open(os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), 'compiler-name'), 'rb').read()
    sections.append(('.comment', PROGBITS, 0, b'\\0' + name + b'\\0'))
open(obj, 'wb').write(elf(sections))
open(dep, 'w').write(obj + ': ' + src + ' config.h\\n')
''' % inspect.getsource(replay_tests.elf)
AR_OBJECTS = '''#!/usr/bin/env python3
import sys
%s
a = sys.argv[1:]
assert a[0] == 'rcD', a
open(a[1], 'wb').write(ar([(n.rsplit('/', 1)[-1], open(n, 'rb').read()) for n in a[2:]]))
''' % inspect.getsource(replay_tests.ar)
RANLIB_NOTHING = '#!/usr/bin/env python3\n'


class Fixture:
    def __init__(self, root: Path, files=None, extra_members=(), compiler_name=None):
        self.root = root
        self.ndk = root / 'ndk'
        bin_dir = self.ndk / 'toolchains/llvm/prebuilt' / m.HOST_TAGS['linux' if sys.platform.startswith('linux') else sys.platform] / 'bin'
        bin_dir.mkdir(parents=True)
        tools = (('clang', CLANG), ('llvm-ar', AR), ('llvm-ranlib', RANLIB)) if compiler_name is None else \
            (('clang', CLANG_OBJECTS), ('llvm-ar', AR_OBJECTS), ('llvm-ranlib', RANLIB_NOTHING))
        for name, text in tools:
            (bin_dir / name).write_text(text, encoding='utf-8', newline='\n')
            (bin_dir / name).chmod(0o755)
        if compiler_name is not None:
            (bin_dir / 'compiler-name').write_bytes(compiler_name)
        self.files = files or {'config.h': b'#define REVISION_OWN 1\n', 'lib/one.c': b'int one;\n', 'lib/arm/two.S': b'two:\n',
                               'util/three.c': b'int three;\n', 'util/list.c': b'/* the revision\'s own list */\n', 'README': b'read me\n'}
        self.archive = root / 'source.tar'
        with tarfile.open(self.archive, 'w') as tar:
            for name, data in self.files.items():
                info = tarfile.TarInfo(name); info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
            for info, data in extra_members:
                tar.addfile(info, io.BytesIO(data) if data is not None else None)
        self.configured = root / 'configured'
        made = {'config.h': b'#define CONFIGURED 1\n', 'util/list.c': b'/* made by configure */\n'}
        for name, data in made.items():
            (self.configured / name).parent.mkdir(parents=True, exist_ok=True)
            (self.configured / name).write_bytes(data)
        self.plan = {'schema': m.SCHEMA, 'revision': 'r1', 'source_archive_sha256': sha(self.archive.read_bytes()),
                     'target': 'aarch64-linux-android26', 'configured': {n: sha(d) for n, d in made.items()},
                     'flag_sets': [['-I.', '-DA=1', '-O2'], ['-I.', '-DASM']],
                     'units': [{'source': 'lib/one.c', 'object': 'lib/one.o', 'flags': 0},
                               {'source': 'lib/arm/two.S', 'object': 'lib/arm/two.o', 'flags': 1},
                               {'source': 'util/three.c', 'object': 'util/../util/three.o', 'flags': 0},
                               {'source': 'util/list.c', 'object': 'util/list.o', 'flags': 0}],
                     'archives': [{'path': 'lib/libone.a', 'members': ['lib/one.o', 'lib/arm/two.o']},      # make's order, not the order of the names
                                  {'path': 'util/libthree.a', 'members': ['util/../util/three.o', 'util/list.o']}],
                     'expect': {}}
        self.output = root / 'out-folder'

    def expected(self):
        """What the stand-in tools give for this plan: the libraries' bytes, worked out here from the plan itself."""
        objects = {}
        for unit in self.plan['units']:
            obj = unit['object']
            args = ['--target=' + self.plan['target'], *self.plan['flag_sets'][unit['flags']], '-MMD', '-MF', obj[:-2] + '.d', '-MT', obj, '-c', '-o', obj, unit['source']]
            source = (self.configured / unit['source']).read_bytes() if unit['source'] in self.plan['configured'] else self.files[unit['source']]
            objects[obj] = ('OBJ cwd=out-folder args=' + ' '.join(args) + '\n').encode() + source + (self.configured / 'config.h').read_bytes()
        return {a['path']: b''.join(('MEMBER ' + n + '\n').encode() + objects[n] for n in a['members']) + b'INDEXED\n' for a in self.plan['archives']}

    def seal(self):
        self.plan['expect'] = {path: sha(data) for path, data in self.expected().items()}
        return self

    def build(self, **named):
        return m.build(m.check_plan(self.plan), self.archive, self.configured, self.ndk, self.output, **named)


class XmaDecoderPlan(unittest.TestCase):
    def fixture(self, **named):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        return Fixture(Path(tmp.name).resolve(), **named)

    def test_units_are_compiled_by_the_plans_arguments_and_archived_in_its_order(self):
        f = self.fixture().seal()
        report = f.build()
        self.assertEqual(report['status'], 'complete')
        for path, data in f.expected().items():
            self.assertEqual((f.output / path).read_bytes(), data, path)        # arguments, working folder, member order, ranlib
            self.assertEqual(report['archives'][path], {'bytes': len(data), 'sha256': sha(data), 'as_the_plan_expects': True})
        self.assertEqual((report['units'], report['source_files'], report['revision'], report['compiler']), (4, 6, 'r1', 'stand-in clang 1.0'))
        self.assertEqual(json.loads((f.output / 'report.json').read_text(encoding='utf-8')), report)
        self.assertEqual((f.output / 'README').read_bytes(), b'read me\n')       # the whole revision is there for whoever reads its headers

    def test_what_configure_made_replaces_the_revisions_own_files(self):
        f = self.fixture().seal()
        f.build()
        self.assertEqual((f.output / 'config.h').read_bytes(), b'#define CONFIGURED 1\n')
        self.assertEqual((f.output / 'util/list.c').read_bytes(), b'/* made by configure */\n')
        self.assertIn(b'/* made by configure */', (f.output / 'util/list.o').read_bytes())
        self.assertIn(b'#define CONFIGURED 1', (f.output / 'lib/one.o').read_bytes())

    def test_a_library_that_differs_is_reported_and_fails_the_run(self):
        f = self.fixture().seal()
        f.plan['expect']['util/libthree.a'] = '0' * 64
        report = f.build()
        self.assertEqual(report['status'], 'differs')
        self.assertEqual([e['as_the_plan_expects'] for _, e in sorted(report['archives'].items())], [True, False])
        f2 = self.fixture().seal(); f2.plan['expect']['util/libthree.a'] = '0' * 64
        plan = f2.root / 'plan.json'; plan.write_text(json.dumps(f2.plan), encoding='utf-8')
        argv = ['--source-archive', str(f2.archive), '--ndk', str(f2.ndk), '--output', str(f2.output), '--plan', str(plan), '--configured', str(f2.configured)]
        with mock.patch('sys.stdout', new=io.StringIO()) as out:
            self.assertEqual(m.main(argv), 1)
        self.assertIn('DIFFERS', out.getvalue())
        f3 = self.fixture().seal()
        plan = f3.root / 'plan.json'; plan.write_text(json.dumps(f3.plan), encoding='utf-8')
        argv = ['--source-archive', str(f3.archive), '--ndk', str(f3.ndk), '--output', str(f3.output), '--plan', str(plan), '--configured', str(f3.configured)]
        with mock.patch('sys.stdout', new=io.StringIO()) as out:
            self.assertEqual(m.main(argv), 0)
        self.assertIn('XMA_DECODER complete 4 units', out.getvalue())

    def refused(self, f, word):
        with self.assertRaisesRegex(ValueError, word):
            f.build()

    A, B = b'compiler as host A names itself', b'compiler, host B'
    PATHS = ('lib/libone.a', 'util/libthree.a')

    def first_machine(self):
        """The plan as its author leaves it after a build on host A: the libraries' hashes there, and their identities."""
        f = self.fixture(compiler_name=self.A)
        f.plan['expect'] = {p: '0' * 64 for p in self.PATHS}
        f.plan['identity'] = {p: '0' * 64 for p in self.PATHS}
        built = f.build()['archives']
        return {'expect': {p: built[p]['sha256'] for p in self.PATHS}, 'identity': {p: built[p]['identity'] for p in self.PATHS}}

    def on_host(self, name, sealed, change=None):
        f = self.fixture(compiler_name=name)
        f.plan.update(copy.deepcopy(sealed))
        if change: change(f.plan)
        return f

    def test_a_library_with_this_hosts_compiler_name_in_it_is_the_plans_by_its_objects(self):
        sealed = self.first_machine()
        a = self.on_host(self.A, sealed); here = a.build()
        self.assertEqual(here['status'], 'complete')
        for p in self.PATHS:
            self.assertEqual({k: here['archives'][p][k] for k in ('sha256', 'identity', 'as_the_plan_expects', 'same_bytes_as_the_first_machine')},
                             {'sha256': sealed['expect'][p], 'identity': sealed['identity'][p], 'as_the_plan_expects': True, 'same_bytes_as_the_first_machine': True})
        self.assertEqual([here['archives'][p]['members'] for p in self.PATHS], [2, 2])
        b = self.on_host(self.B, sealed); there = b.build()
        self.assertEqual(there['status'], 'complete')
        for p in self.PATHS:
            entry = there['archives'][p]
            self.assertEqual((entry['identity'], entry['as_the_plan_expects'], entry['same_bytes_as_the_first_machine']), (sealed['identity'][p], True, False))
        self.assertNotEqual(there['archives']['util/libthree.a']['sha256'], sealed['expect']['util/libthree.a'])      # two C units: other bytes
        # the identities found are written down, and the file is the same on both hosts
        record = (a.output / 'identity.json').read_bytes()
        self.assertEqual(record, (b.output / 'identity.json').read_bytes())
        self.assertEqual(json.loads(record), {'schema': 'xma-decoder-identity/1', 'archives': sealed['identity']})
        self.assertNotIn(b'\r', record)
        # and the command line says which case it was
        c = self.on_host(self.B, sealed)
        plan = c.root / 'plan.json'; plan.write_text(json.dumps(c.plan), encoding='utf-8')
        argv = ['--source-archive', str(c.archive), '--ndk', str(c.ndk), '--output', str(c.output), '--plan', str(plan), '--configured', str(c.configured)]
        with mock.patch('sys.stdout', new=io.StringIO()) as out:
            self.assertEqual(m.main(argv), 0)
        self.assertIn('as expected (the first machine\'s objects; this host\'s compiler name in them)', out.getvalue())
        self.assertNotIn('DIFFERS', out.getvalue())

    def test_other_objects_are_not_the_plans_whatever_the_compiler_is_called(self):
        sealed = self.first_machine()
        for name in (self.A, self.B):
            f = self.on_host(name, sealed, lambda plan: plan['flag_sets'][0].append('-DOTHER=1'))      # the C units come out as other objects
            report = f.build()
            self.assertEqual(report['status'], 'differs', name)
            self.assertEqual([report['archives'][p]['as_the_plan_expects'] for p in self.PATHS], [False, False])
            self.assertEqual(json.loads((f.output / 'identity.json').read_text(encoding='utf-8'))['archives'],
                             {p: report['archives'][p]['identity'] for p in self.PATHS})                 # what was found, not what was hoped
        f = self.on_host(self.B, sealed, lambda plan: plan['flag_sets'][0].append('-DOTHER=1'))
        plan = f.root / 'plan.json'; plan.write_text(json.dumps(f.plan), encoding='utf-8')
        with mock.patch('sys.stdout', new=io.StringIO()) as out:
            self.assertEqual(m.main(['--source-archive', str(f.archive), '--ndk', str(f.ndk), '--output', str(f.output), '--plan', str(plan),
                                     '--configured', str(f.configured)]), 1)
        self.assertIn('DIFFERS', out.getvalue())

    def test_a_plan_without_identities_goes_by_the_bytes_alone_and_writes_no_identity(self):
        sealed = self.first_machine()
        f = self.on_host(self.B, {'expect': sealed['expect']})
        report = f.build()
        self.assertEqual(report['status'], 'differs')                       # other bytes, and nothing to recognise them by
        self.assertEqual(set(report['archives']['util/libthree.a']), {'bytes', 'sha256', 'as_the_plan_expects'})
        self.assertFalse((f.output / 'identity.json').exists())

    def test_identities_are_for_every_library_or_for_none_and_only_of_objects(self):
        sealed = self.first_machine()
        for what, change in (('one library only', lambda p: p['identity'].pop('lib/libone.a')), ('a library the plan does not make', lambda p: p['identity'].update({'x/y.a': '0' * 64})),
                             ('no hash', lambda p: p['identity'].update({'lib/libone.a': 'zz'})), ('no table', lambda p: p.update(identity=['a']))):
            f = self.on_host(self.A, sealed, change)
            with self.subTest(case=what), self.assertRaisesRegex(ValueError, 'identities are given for every archive or for none'):
                m.check_plan(f.plan)
        text_objects = self.fixture()                                         # the stand-ins that write text, not objects
        text_objects.seal().plan['identity'] = {p: '0' * 64 for p in self.PATHS}
        with self.assertRaisesRegex(ValueError, 'not an ar archive'):
            text_objects.build()

    def test_another_source_archive_is_refused_before_anything_is_written(self):
        f = self.fixture().seal()
        with f.archive.open('ab') as stream: stream.write(b'\0' * 512)
        self.refused(f, 'source archive is not the one')
        self.assertFalse(f.output.exists())

    def test_a_changed_or_missing_configured_file_is_refused_before_anything_is_written(self):
        f = self.fixture().seal()
        (f.configured / 'util/list.c').write_bytes(b'/* edited */\n')
        self.refused(f, 'configured file util/list.c')
        self.assertFalse(f.output.exists())
        f = self.fixture().seal()
        (f.configured / 'config.h').unlink()
        self.refused(f, 'configured file config.h')
        self.assertFalse(f.output.exists())

    def test_an_existing_output_folder_is_refused(self):
        f = self.fixture().seal()
        f.output.mkdir(); (f.output / 'kept.txt').write_text('evidence')
        self.refused(f, 'exists already')
        self.assertEqual((f.output / 'kept.txt').read_text(), 'evidence')

    def test_an_archive_member_that_is_no_plain_file_or_leaves_the_folder_refuses_everything(self):
        def link():
            info = tarfile.TarInfo('lib/link.h'); info.type = tarfile.SYMTYPE; info.linkname = '/etc/passwd'; return info, None
        def named(name):
            info = tarfile.TarInfo(name); info.size = 1; return info, b'x'
        for member in (link(), named('../outside.c'), named('/abs.c'), named('lib/../../up.c'), named('lib\\back.c'), named('c:drive.c')):
            with self.subTest(member=member[0].name):
                f = self.fixture(extra_members=[member]).seal()
                self.refused(f, 'source archive refused')
                self.assertFalse(f.output.exists())
                self.assertFalse((f.root / 'outside.c').exists() or (f.root / 'up.c').exists())

    def test_a_unit_that_does_not_compile_names_itself(self):
        f = self.fixture(files={'config.h': b'x\n', 'lib/one.c': b'BROKEN\n', 'lib/arm/two.S': b't\n', 'util/three.c': b'3\n', 'util/list.c': b'l\n', 'README': b'r\n'})
        f.plan['expect'] = {'lib/libone.a': '0' * 64, 'util/libthree.a': '0' * 64}
        self.refused(f, 'lib/one.c did not compile: stand-in: cannot compile lib/one.c')
        self.assertFalse((f.output / 'report.json').exists())
        f = self.fixture().seal()
        f.plan['units'][0]['source'] = 'lib/absent.c'
        self.refused(f, 'not in the archive: lib/absent.c')

    def test_a_missing_ndk_tool_is_refused_before_anything_is_written(self):
        f = self.fixture().seal()
        next((f.ndk / 'toolchains/llvm/prebuilt').glob('*/bin/llvm-ar')).unlink()
        self.refused(f, 'NDK tool is missing')
        self.assertFalse(f.output.exists())

    def test_the_hosts_own_ndk_tools_are_chosen(self):
        with tempfile.TemporaryDirectory() as tmp:
            ndk = Path(tmp)
            for tag, suffix in (('windows-x86_64', '.exe'), ('darwin-x86_64', ''), ('linux-x86_64', '')):
                for name in ('clang', 'llvm-ar', 'llvm-ranlib'):
                    path = ndk / 'toolchains/llvm/prebuilt' / tag / 'bin' / (name + suffix); path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'')
            self.assertEqual(m.tools(ndk, 'win32')['clang'], ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe')
            self.assertEqual(m.tools(ndk, 'win32')['llvm-ranlib'].name, 'llvm-ranlib.exe')
            self.assertEqual(m.tools(ndk, 'darwin')['llvm-ar'], ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar')
            self.assertEqual(m.tools(ndk, 'linux')['clang'], ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin/clang')
            with self.assertRaisesRegex(ValueError, 'no NDK host'):
                m.tools(ndk, 'sunos5')

    def test_the_command_of_a_unit_is_clang_with_the_target_and_nothing_of_this_machine(self):
        f = self.fixture().seal()
        command = m.unit_command(f.plan, f.plan['units'][1], Path('X/clang'))
        self.assertEqual(command, ['X/clang', '--target=aarch64-linux-android26', '-I.', '-DASM', '-MMD', '-MF', 'lib/arm/two.d', '-MT', 'lib/arm/two.o',
                                   '-c', '-o', 'lib/arm/two.o', 'lib/arm/two.S'])

    def test_plans_of_another_shape_are_refused(self):
        good = self.fixture().seal().plan
        m.check_plan(copy.deepcopy(good))
        def changed(edit):
            plan = copy.deepcopy(good); edit(plan); return plan
        cases = {
            'schema': lambda p: p.update(schema='xma-decoder-plan/0'),
            'archive hash': lambda p: p.update(source_archive_sha256='abc'),
            'target': lambda p: p.update(target='x86_64-pc-windows'),
            'configured name': lambda p: p['configured'].update({'../config.h': '0' * 64}),
            'configured hash': lambda p: p['configured'].update({'config.h': 'zz'}),
            'no configured': lambda p: p.update(configured={}),
            'flag names an output': lambda p: p['flag_sets'][0].append('-o'),
            'flag names a file': lambda p: p['flag_sets'][0].append('/etc/hosts'),
            'flag reads a response file': lambda p: p['flag_sets'][0].append('@args'),
            'flag climbs': lambda p: p['flag_sets'][0].append('-I../..'),
            'unit source outside': lambda p: p['units'][0].update(source='../x.c'),
            'unit source absolute': lambda p: p['units'][0].update(source='/x.c'),
            'unit source kind': lambda p: p['units'][0].update(source='lib/one.py'),
            'unit object kind': lambda p: p['units'][0].update(object='lib/one.exe'),
            'unit object outside': lambda p: (p['units'][0].update(object='../../one.o'), p['archives'][0].update(members=['../../one.o', 'lib/arm/two.o'])),
            'unit flags': lambda p: p['units'][0].update(flags=2),
            'unit flags kind': lambda p: p['units'][0].update(flags=True),
            'two units one object': lambda p: (p['units'][1].update(object='lib/one.o'), p['archives'][0].update(members=['lib/one.o'])),
            'archive member unknown': lambda p: p['archives'][0]['members'].append('lib/other.o'),
            'archive member twice': lambda p: p['archives'][0]['members'].append('lib/one.o'),
            'object in two archives': lambda p: p['archives'][1]['members'].append('lib/one.o'),
            'object in no archive': lambda p: p['archives'][1]['members'].remove('util/list.o'),
            'archive path': lambda p: (p['archives'][0].update(path='../lib.a'), p['expect'].update({'../lib.a': p['expect'].pop('lib/libone.a')})),
            'expected hash malformed': lambda p: p['expect'].update({'lib/libone.a': 'zz'}),
            'archive kind': lambda p: p['archives'][0].update(path='lib/libone.so'),
            'archive not expected': lambda p: p['expect'].pop('lib/libone.a'),
            'expected and not made': lambda p: p['expect'].update({'lib/extra.a': '0' * 64}),
            'no units': lambda p: p.update(units=[]),
        }
        for name, edit in cases.items():
            with self.subTest(case=name), self.assertRaises(ValueError):
                m.check_plan(changed(edit))

    def test_the_tracked_plan_is_well_formed_and_its_configured_files_are_the_ones_it_names(self):
        folder = Path(__file__).resolve().parents[1] / 'xma_decoder'
        plan = m.check_plan(json.loads((folder / 'plan.json').read_text(encoding='utf-8')))
        self.assertEqual((len(plan['units']), [len(a['members']) for a in plan['archives']]), (141, [61, 80]))
        self.assertEqual(plan['revision'], '15ece0882e8d5875051ff5b73c5a8326f7cee9f5')
        for name, expected in plan['configured'].items():
            self.assertEqual(m.sha_file(folder / 'configured' / name), expected, name)
        self.assertEqual(plan['expect'], {'libavcodec/libavcodec.a': '97ff91329c5f61d9a18d55dc7c93cb78eadc381d4b335b4a36ef18870bfa8fc9',
                                          'libavutil/libavutil.a': '1f65ccea4618bf8c7b3b035ed20effce9a5e9a50a774421c0939d629570418ac'})
        text = (folder / 'plan.json').read_text(encoding='utf-8')
        self.assertNotIn('/Users/', text); self.assertNotIn('/opt/', text)         # the commands name nothing of the machine they were read on


if __name__ == '__main__':
    unittest.main()
