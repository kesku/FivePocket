"""Synthetic connected checks of the portable builder (the host helper, the front end, the engine, the tools they
call). No game data. PACKAGE_SRC redirects to a source mutant overlay (mutants_package.py)."""
from __future__ import annotations
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

REPO = Path(__file__).resolve().parents[3]
SRC = Path(os.environ.get('PACKAGE_SRC', REPO))
SCRATCH = REPO / 'local-analysis' / 'test-tmp'      # inside the checkout's private folder: the tools refuse any other place in it
sys.path.insert(0, str(SRC / 'tools/own_disc'))
import builder as b
import host_support as hs
import disc_builder as db


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec); sys.modules[name] = mod; spec.loader.exec_module(mod)
    return mod


# Existing project fixtures are synthetic, not the owner's game.
os.environ['DISC_BUILDER'] = str(SRC / 'tools/own_disc/disc_builder.py')
fixture = load('package_fixtures', REPO / 'tools/own_disc/tests/test_disc_builder.py')
sys.path.insert(0, str(REPO / 'tools/iter'))
rp = load('package_recipe', SRC / 'tools/fp_codegen/recipe_build.py')


class Package(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix='synthetic-étrange space-', dir=SCRATCH)
        self.addCleanup(self.tmp.cleanup); self.root = Path(self.tmp.name)
        self.archive = self.root / 'ndk-fixture.zip'
        with zipfile.ZipFile(self.archive, 'w') as z:
            z.writestr('android-ndk-fixture/bin/compiler.txt', b'SYNTHETIC compiler, not executable')
        self.ndk = dict(url='https://dl.google.com/android/repository/android-ndk-fixture.zip',
                        sha256=db.sha256_file(self.archive), size=self.archive.stat().st_size,
                        directory='android-ndk-fixture', unpacked_bytes=4096,
                        license_url='https://developer.android.com/ndk/downloads')
        self.cache = self.root / 'cache'; self.release = {'ndk': self.ndk}

    def fetch(self):
        with contextlib.redirect_stdout(io.StringIO()):
            b.fetch(self.release, self.cache, True, self.archive)


    def test_02_drive_dependency_names(self):
        self.assertEqual(hs.dep_paths('C:\\obj\\a.o: C:\\src\\name\\ file.h D:/inc/a.h\nC:/phony.h:\n'),
                         ['C:\\src\\name file.h', 'D:/inc/a.h'])
        with self.assertRaises(ValueError): hs.dep_paths('malformed depfile')

    def test_03_windows_priority_no_shell(self):
        with mock.patch.object(hs.sys, 'platform', 'win32'), mock.patch.object(hs.shutil, 'which', side_effect=AssertionError('must not find nice')):
            argv, kwargs = hs.low_priority(['C:\\sp ace\\clang++.exe', 'é.cpp'])
        self.assertEqual(argv, ['C:\\sp ace\\clang++.exe', 'é.cpp'])
        self.assertEqual(kwargs, {'creationflags': 0x4000})

    def test_04_copy_without_macos_command(self):
        src, dest = self.root / 'src', self.root / 'copy'; src.write_bytes(b'synthetic')
        with mock.patch.object(hs.sys, 'platform', 'win32'), mock.patch.object(hs.subprocess, 'run', side_effect=AssertionError('cp is unavailable')):
            hs.clone_or_copy(src, dest)
        self.assertEqual(src.read_bytes(), dest.read_bytes())


    def test_06_recipe_subprocess_connection(self):
        tool = self.root / 'pass.py'
        tool.write_text('import sys\nfrom pathlib import Path\nPath(sys.argv[2]).write_bytes(Path(sys.argv[1]).read_bytes()+b"changed\\n")\n')
        pass_obj = object.__new__(rp.Pass); pass_obj.id = 'fixture'; pass_obj.tmp = self.root; pass_obj.tool = tool
        real_run = subprocess.run
        def checked_run(argv, **kwargs):
            self.assertNotEqual(Path(argv[0]).name, 'nice')
            return real_run(argv, **kwargs)
        with mock.patch.object(hs.shutil, 'which', return_value=None), mock.patch.object(rp.subprocess, 'run', side_effect=checked_run):
            self.assertEqual(pass_obj.run(['{in}', '{out}'], 'source é\n'), 'source é\nchanged\n')

    def test_07_generator_private_boundary(self):
        checkout = self.root / 'checkout'; (checkout / '.git').mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'local-analysis'):
            hs.private_output(checkout / 'src/generated')
        hs.private_output(checkout / 'local-analysis/work')
        hs.private_output(self.root / 'standalone/work')
        with mock.patch.object(hs.os.path, 'relpath', side_effect=ValueError('different drive')):
            self.assertEqual(hs.relative_or_absolute(self.root / 'input', self.root / 'work'), str(self.root / 'input'))

    def test_08_fetch_requires_consent(self):
        with mock.patch.object(b.urllib.request, 'build_opener', side_effect=AssertionError('no network')):
            with self.assertRaisesRegex(b.Refusal, 'licence not accepted'):
                b.fetch(self.release, self.cache, False)
        self.assertFalse(self.cache.exists())

    def test_09_fetch_hash_before_extraction(self):
        self.ndk['sha256'] = '0' * 64
        with self.assertRaisesRegex(b.Refusal, 'hash/size mismatch'):
            b.fetch(self.release, self.cache, True, self.archive)
        self.assertFalse((self.cache / ('0' * 16)).exists())

    def test_10_fetch_to_compiler_identity(self):
        self.fetch(); root = b.existing_ndk(self.cache, self.ndk)
        self.assertEqual((root / 'bin/compiler.txt').read_bytes(), b'SYNTHETIC compiler, not executable')
        (root / 'bin/compiler.txt').write_bytes(b'changed')
        with self.assertRaisesRegex(b.Refusal, 'files changed'): b.existing_ndk(self.cache, self.ndk)

    def test_11_archive_name_guards(self):
        for name in ('../escape', 'C:/escape', 'a/../b', 'a\\b', 'a/NUL.txt', 'a/space ', 'a/trail.', '/abs', 'a//b'):
            with self.subTest(name=name), self.assertRaises(b.Refusal): b.safe_name(name)
        self.assertEqual(str(b.safe_name('répertoire/file name')), 'répertoire/file name')

    def test_12_zip_case_alias_refused(self):
        with zipfile.ZipFile(self.archive, 'w') as z:
            z.writestr('android-ndk-fixture/a', b'one'); z.writestr('android-ndk-fixture/A', b'two')
        self.ndk.update(size=self.archive.stat().st_size, sha256=db.sha256_file(self.archive))
        with self.assertRaisesRegex(b.Refusal, 'duplicate case-insensitive'):
            b.fetch(self.release, self.cache, True, self.archive)
        self.assertFalse((self.cache / self.ndk['sha256'][:16]).exists())

    def test_13_release_payload_hash(self):
        profile = self.root / 'profile.json'; profile.write_bytes(b'{}')
        release = dict(schema='builder-release/1', host=b.host_id(), profile='profile.json', profiles={'duo': 'profile.json'},
                       files=[dict(path='profile.json', sha256=db.sha256_file(profile))])
        path = self.root / 'release.json'; path.write_text(json.dumps(release))
        b.load_release(path); profile.write_bytes(b'{"changed":true}')
        with self.assertRaisesRegex(b.Refusal, 'missing or changed'): b.load_release(path)


    def make_release(self):
        fx = fixture.Fixture(self.root / 'fixture')
        # Folder is created by the caller; copy-source is supplied to synthetic fixture only.
        for stage in fx.profile['stages']:
            if stage['adapter'] == 'command': stage['argv'][0] = '${python}'
        fx.stage('shaders')['source'] = '${repo}/pack.bin'
        fx.profile_path.write_text(json.dumps(fx.profile))
        shutil.copyfile(fx.profile_path, fx.side / 'profile.json')
        self.release.update(schema='builder-release/1', host=b.host_id(), profile='profile.json', profiles={'duo': 'profile.json'},
                            files=[dict(path=p.relative_to(fx.side).as_posix(), sha256=db.sha256_file(p)) for p in fx.side.rglob('*') if p.is_file()],
                            estimates=dict(min_free_bytes=1, build_time='synthetic fixture only'))
        release = fx.side / 'release.json'; release.write_text(json.dumps(self.release))
        return fx, release

    def test_15_frontend_build_resumes_and_reproduces_output(self):
        self.fetch(); (self.root / 'fixture').mkdir(); fx, release = self.make_release()
        argv = ['--release', str(release), '--cache', str(self.cache), 'build', '--work', str(fx.work), '--play-disc', str(fx.disc), '--install-disc', str(fx.disc)]
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(argv), 0, out.getvalue())
            digest = db.sha256_file(fx.artifact())
            self.assertEqual(b.main(argv), 0, out.getvalue())
            self.assertEqual(fx.executed(), ['decode', 'codegen', 'link'])
            self.assertEqual(db.sha256_file(fx.artifact()), digest)
        # Corrupt only a produced intermediate; producer rebuilds it, consumers
        # whose inputs still have identical content correctly reuse their receipts.
        (fx.work / 'decode/image.bin').write_bytes(b'corruption')
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(argv), 0, out.getvalue())
        self.assertEqual(fx.executed(), ['decode', 'codegen', 'link', 'decode'])
        self.assertEqual(db.sha256_file(fx.artifact()), digest)

    def test_16_case_ambiguity_refused(self):
        folder = db.FolderDisc(self.root)
        one, two = self.root / 'FILE', self.root / 'file'
        one.write_bytes(b'a'); two.write_bytes(b'b')
        # APFS here is case-insensitive. Exercise two directory entries with a
        # mocked listing instead of claiming the host is case-sensitive.
        with mock.patch.object(Path, 'iterdir', return_value=iter([one, two])):
            with self.assertRaisesRegex(db.BuilderError, 'ambiguous'): folder.size('file')

    def test_17_portable_context(self):
        ctx = db.Context({'_dir': self.root}, self.root / 'work')
        with mock.patch.dict(os.environ, {'BUILDER_PYTHON': 'C:\\Python é\\python.exe', 'BUILDER_NDK_ROOT': 'C:\\NDK'}), mock.patch.object(db.sys, 'platform', 'win32'):
            self.assertEqual(ctx.expand('${python}|${ndk}|tool${exe}'), 'C:\\Python é\\python.exe|C:\\NDK|tool.exe')
        with mock.patch.dict(os.environ, {'BUILDER_HOST_TOOLS': 'D:\\tools é'}), mock.patch.object(db.sys, 'platform', 'win32'), \
             mock.patch.object(hs.sys, 'platform', 'win32'):
            self.assertEqual(ctx.expand('${hosttools}/xex_image${exe}|${ndk_}'.replace('${ndk_}', '${ndkhost}')), 'D:\\tools é/xex_image.exe|windows-x86_64')
        with mock.patch.object(hs.sys, 'platform', 'darwin'), mock.patch.object(db.sys, 'platform', 'darwin'), mock.patch.dict(os.environ, {'BUILDER_HOST_TOOLS': '/t'}):
            self.assertEqual(ctx.expand('${hosttools}/x${exe} ${ndkhost}'), '/t/x darwin-x86_64')
        self.assertEqual([hs.ndk_host_tag(p) for p in ('darwin', 'win32', 'linux', 'linux2')], ['darwin-x86_64', 'windows-x86_64', 'linux-x86_64', 'linux-x86_64'])
        with self.assertRaisesRegex(ValueError, 'no NDK host'): hs.ndk_host_tag('sunos5')
        with mock.patch.dict(os.environ):
            os.environ.pop('BUILDER_HOST_TOOLS', None)
            with self.assertRaisesRegex(db.BuilderError, 'host tools missing'): ctx.expand('${hosttools}')

    def test_18_lf_recipe_source_contract(self):
        # Check actual Path call syntax, not a weak text search. Windows text-mode
        # writes must be explicit at every patched recipe producer.
        import ast
        for rel in ('recipe_build.py', 'leaf_locals.py', 'gpr_fastpath.py', 'stitch_split_functions.py', 'recipe/textpass.py'):
            tree = ast.parse((SRC / 'tools/fp_codegen' / rel).read_text())
            for node in ast.walk(tree):
                if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and node.func.attr == 'write_text':
                    kwargs = {k.arg: ast.literal_eval(k.value) for k in node.keywords}
                    self.assertEqual(kwargs.get('newline'), '\n', (rel, node.lineno))
                    self.assertEqual(kwargs.get('encoding'), 'utf-8', (rel, node.lineno))

    def test_19_engine_identity_in_receipt(self):
        ctx = db.Context({'_dir': self.root}, self.root / 'work'); ctx.game = {'executable_sha256': '0' * 64}
        stage = dict(adapter='command', id='synthetic', tools=[], inputs={})
        parts = db.stage_key_parts(stage, ctx, 'start', {})
        self.assertEqual(parts['tools']['host-support'], db.sha256_file(Path(hs.__file__)))
        self.assertEqual(parts['tools']['builder-engine'], db.sha256_file(Path(db.__file__)))

    def test_20_missing_pins_fail_closed(self):
        self.release['ndk']['sha256'] = None
        with self.assertRaisesRegex(b.Refusal, 'not pinned'): b.fetch(self.release, self.cache, True, self.archive)
        self.assertFalse(self.cache.exists())

    def test_21_equal_inputs_reproduce_output_in_different_work_folders(self):
        first, second = self.root / 'a', self.root / 'c'
        first.mkdir(); second.mkdir()
        a, b = fixture.Fixture(first), fixture.Fixture(second)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(a.run()[0], 0)
            self.assertEqual(b.run()[0], 0)
        self.assertEqual(a.artifact().read_bytes(), b.artifact().read_bytes())

    def test_23_zipapp_build_delivers_stage_output(self):
        self.fetch(); (self.root / 'fixture').mkdir(); fx, release = self.make_release()
        # Rebuild zipapp from the selected overlay so mutants exercise the code
        # shipped inside the artifact, not a stale previously built archive.
        artifact = self.root / 'builder.pyz'
        with zipfile.ZipFile(artifact, 'w') as z:
            for name in ('builder.py', 'disc_builder.py', 'host_support.py'):
                z.write(SRC / 'tools/own_disc' / name, name)
            z.writestr('__main__.py', 'import builder\nraise SystemExit(builder.main())\n')
        result = subprocess.run([sys.executable, str(artifact), '--release', str(release), '--cache', str(self.cache),
                                 'build', '--work', str(fx.work), '--play-disc', str(fx.disc), '--install-disc', str(fx.disc)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(fx.artifact().read_bytes(), (fx.work / 'link/libgame.so').read_bytes())

    def test_24_archive_links_materialized_safely(self):
        import stat
        with zipfile.ZipFile(self.archive, 'w') as z:
            z.writestr('android-ndk-fixture/bin/real', b'tool')
            link = zipfile.ZipInfo('android-ndk-fixture/bin/compiler')
            link.create_system = 3; link.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(link, 'real')
        self.ndk.update(size=self.archive.stat().st_size, sha256=db.sha256_file(self.archive))
        self.fetch(); installed = b.existing_ndk(self.cache, self.ndk) / 'bin/compiler'
        self.assertFalse(installed.is_symlink()); self.assertEqual(installed.read_bytes(), b'tool')

    def test_25_recipe_helper_identity(self):
        with mock.patch.object(rp, 'TOOLS_ROOT', SRC):
            closure = rp.closure(SRC / 'tools/fp_codegen/recipe_build.py')
        self.assertIn(SRC / 'tools/own_disc/host_support.py', closure)

    def test_26_package_root_override(self):
        with mock.patch.dict(os.environ, {'BUILDER_PACKAGE_ROOT': str(self.root)}):
            self.assertEqual(hs.package_root(str(SRC / 'tools/iter/iterate.py')), self.root)
        self.assertEqual(hs.package_root(str(SRC / 'tools/iter/iterate.py')), SRC)

    def test_27_shader_records_root(self):
        pack = load('package_shader_pack', SRC / 'tools/iter/generic_shader_pack.py')
        out = self.root / 'pack.bin'
        with mock.patch.object(sys, 'argv', ['generic_shader_pack.py', '--root', str(self.root), '--corpus', 'empty-*', '--out', str(out)]):
            pack.main()
        self.assertEqual(pack.ROOT, self.root)
        self.assertEqual(out.read_bytes()[:4], b'GSP2')

    def test_28_nm_failure_not_empty_hook_set(self):
        mod = load('package_direct', SRC / 'tools/fp_codegen/direct_calls.py')
        (self.root / 'synthetic.o').write_bytes(b'synthetic')
        def process(argv, **kwargs):
            self.assertTrue(kwargs.get('check'), 'failed nm must never become an empty hook list')
            self.assertNotIn('/opt/homebrew', argv[0])
            raise subprocess.CalledProcessError(7, argv)
        with mock.patch.object(mod.subprocess, 'run', side_effect=process), self.assertRaises(subprocess.CalledProcessError):
            mod.hooked_from(self.root)

    def test_29_regenerator_without_checkout(self):
        mod = load('package_regen', SRC / 'tools/own_disc/regen_check.py')
        source, config, exe, context = [self.root / s for s in ('synthetic-input', 'config.toml', 'generator-standin', 'context.h')]
        for f in (source, exe, context): f.write_bytes(b'synthetic')
        config.write_text('file_path = "synthetic-input"\nout_directory_path = "./old"\n')
        work, report = self.root / 'regen-work', self.root / 'regen.json'
        def process(argv, **kwargs):
            self.assertEqual(argv[0], str(exe))
            (work / 'ppc/ppc_context.h').write_bytes(b'synthetic support')
            return subprocess.CompletedProcess(argv, 0)
        with mock.patch.object(mod.subprocess, 'run', side_effect=process), mock.patch.object(hs.shutil, 'which', return_value=None), \
             mock.patch.object(sys, 'argv', ['regen_check.py', '--generator', str(exe), '--context', str(context), '--config-from', str(config), '--work', str(work), '--report', str(report), '--keep']):
            self.assertEqual(mod.main(), 0)
        self.assertEqual(json.loads(report.read_text())['generated_files'], 1)


    def test_31_tree_identity_order(self):
        class CasefoldPath(type(Path())):
            def __lt__(self, other): return str(self).casefold() < str(other).casefold()
        # Mimic Windows Path ordering on this host, without claiming NTFS access.
        upper, lower = self.root / 'Z.txt', self.root / 'a.txt'
        upper.write_bytes(b'upper'); lower.write_bytes(b'lower')
        paths = [CasefoldPath(lower), CasefoldPath(upper)]
        expected = hashlib.sha256()
        for p in (upper, lower):
            expected.update(p.name.encode() + b'\0' + db.sha256_file(p).encode() + b'\n')
        with mock.patch.object(Path, 'rglob', return_value=iter(paths)):
            digest, count = db.sha256_tree(self.root)
        self.assertEqual((digest, count), (expected.hexdigest(), 2))


    def test_34_priority_on_one_scale(self):
        # the system's own nice value here; on Windows the priority class, with "below normal" (what low_priority starts) as 10
        self.assertEqual(hs.effective_nice(), os.getpriority(os.PRIO_PROCESS, 0))
        self.assertEqual([hs.nice_of_priority_class(c) for c in (0x40, 0x4000, 0x20, 0x8000, 0x80, 0x100)], [19, 10, 0, -5, -10, -20])
        with self.assertRaisesRegex(ValueError, 'unknown process priority class'): hs.nice_of_priority_class(0x1234)
        with mock.patch.object(hs.sys, 'platform', 'win32'), mock.patch.object(hs, '_windows_priority_class', return_value=0x4000), \
             mock.patch.object(hs.os, 'getpriority', side_effect=AssertionError('no POSIX call on Windows')):
            self.assertEqual(hs.effective_nice(), 10)
            self.assertEqual(hs.low_priority(['tool'], 10), (['tool'], {'creationflags': 0x4000}))   # the class asked for is the class read back as 10

    def test_35_positioned_read_without_pread(self):
        path = self.root / 'data.bin'; path.write_bytes(bytes(range(256)) * 40)
        fd = os.open(path, os.O_RDONLY | getattr(os, 'O_BINARY', 0))
        try:
            for size, offset in ((0, 0), (16, 0), (300, 250), (4096, 9000), (500, 10100)):   # the last one runs past the end
                self.assertEqual(hs.pread_by_seek(fd, size, offset), os.pread(fd, size, offset), (size, offset))
            self.assertEqual(len(hs.pread_by_seek(fd, 500, 10100)), 140)
            # two threads reading at different places never get each other's bytes
            from concurrent.futures import ThreadPoolExecutor
            jobs = [(64, (i * 37) % 10000) for i in range(400)]
            with ThreadPoolExecutor(8) as pool:
                got = list(pool.map(lambda j: hs.pread_by_seek(fd, *j), jobs))
            self.assertEqual(got, [os.pread(fd, *j) for j in jobs])
        finally:
            os.close(fd)
        self.assertIs(hs.pread, os.pread)        # this system has its own; the fallback is for one that has none

    def test_36_release_at_the_package_root(self):
        # started by the launcher (it names the package root): the inventory there; without one: beside the front end
        with mock.patch.dict(os.environ, {'BUILDER_PACKAGE_ROOT': str(self.root)}):
            self.assertEqual(b.default_release(), self.root / 'release.json')
        environment = {k: v for k, v in os.environ.items() if k != 'BUILDER_PACKAGE_ROOT'}
        with mock.patch.dict(os.environ, environment, clear=True):
            self.assertEqual(b.default_release(), Path(b.__file__).with_name('release.json'))
        # and main() takes the package root's file when it is there: a release for another host is refused by name
        (self.root / 'release.json').write_text(json.dumps({'schema': 'builder-release/1', 'host': 'elsewhere-x'}))
        with mock.patch.dict(os.environ, {'BUILDER_PACKAGE_ROOT': str(self.root)}), contextlib.redirect_stdout(io.StringIO()) as said:
            self.assertEqual(b.main(['fetch']), 2)
        self.assertIn('wrong or missing release', said.getvalue())

    def test_37_check_passes_a_profile_whose_command_lines_name_the_discs(self):
        # the plan is checked before any disc is given: a stage's command line may still name one
        self.fetch(); (self.root / 'fixture').mkdir()
        fx = fixture.Fixture(self.root / 'fixture')
        for stage in fx.profile['stages']:
            if stage['adapter'] == 'command': stage['argv'][0] = '${python}'
        fx.stage('shaders')['source'] = '${repo}/pack.bin'
        fx.stage('codegen')['argv'] += ['--play', '${disc:play}', '--install', '${disc:install}']
        fx.profile_path.write_text(json.dumps(fx.profile))
        shutil.copyfile(fx.profile_path, fx.side / 'profile.json')
        self.release.update(schema='builder-release/1', host=b.host_id(), profile='profile.json', profiles={'duo': 'profile.json'},
                            files=[dict(path=p.relative_to(fx.side).as_posix(), sha256=db.sha256_file(p)) for p in fx.side.rglob('*') if p.is_file()],
                            estimates=dict(min_free_bytes=1, build_time='synthetic fixture only'))
        release = fx.side / 'release.json'; release.write_text(json.dumps(self.release))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(['--release', str(release), '--cache', str(self.cache), 'check', '--work', str(fx.work)]), 0, out.getvalue())
        self.assertIn('Payload, compiler and stage prerequisites verified', out.getvalue())
        self.assertFalse(fx.work.exists())                    # nothing ran, nothing was written
        # a disc the engine does not know is still a mistake of the profile
        fx.stage('codegen')['argv'] += ['${disc:third}']
        fx.profile_path.write_text(json.dumps(fx.profile)); shutil.copyfile(fx.profile_path, fx.side / 'profile.json')
        self.release['files'] = [dict(path=p.relative_to(fx.side).as_posix(), sha256=db.sha256_file(p)) for p in fx.side.rglob('*') if p.is_file() and p.name != 'release.json']
        release.write_text(json.dumps(self.release))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(['--release', str(release), '--cache', str(self.cache), 'check', '--work', str(fx.work)]), db.EXIT_USAGE, out.getvalue())
        self.assertIn('needs that disc on the command line', out.getvalue())

    def packaged(self, change=None):
        self.fetch(); (self.root / 'fixture').mkdir()
        fx = fixture.Fixture(self.root / 'fixture')
        for stage in fx.profile['stages']:
            if stage['adapter'] == 'command': stage['argv'][0] = '${python}'
        fx.stage('shaders')['source'] = '${repo}/pack.bin'
        if change: change(fx)
        fx.profile_path.write_text(json.dumps(fx.profile))
        shutil.copyfile(fx.profile_path, fx.side / 'profile.json')
        self.release.update(schema='builder-release/1', host=b.host_id(), profile='profile.json', profiles={'duo': 'profile.json'},
                            files=[dict(path=p.relative_to(fx.side).as_posix(), sha256=db.sha256_file(p)) for p in fx.side.rglob('*') if p.is_file()],
                            estimates=dict(min_free_bytes=3 << 30, build_time='synthetic fixture only'))
        release = fx.side / 'release.json'; release.write_text(json.dumps(self.release))
        return fx, release

    def test_38_a_build_without_a_work_folder_uses_the_package_s_own(self):
        fx, release = self.packaged()
        self.assertEqual(b.default_work(fx.side), fx.side / 'local-analysis' / 'builder' / 'work')
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(['--release', str(release), '--cache', str(self.cache), 'build', '--play-disc', str(fx.disc), '--install-disc', str(fx.disc)]), 0, out.getvalue())
        made = fx.side / 'local-analysis' / 'builder' / 'work' / 'out' / 'result.ipa'
        self.assertTrue(made.is_file())
        self.assertFalse(fx.work.exists())
        with contextlib.redirect_stdout(io.StringIO()) as out:          # and the check alone looks at the same folder
            self.assertEqual(b.main(['--release', str(release), '--cache', str(self.cache), 'check']), 0, out.getvalue())
        with contextlib.redirect_stdout(io.StringIO()) as out:          # a folder that is named is the one used
            self.assertEqual(b.main(['--release', str(release), '--cache', str(self.cache), 'build', '--work', str(fx.work), '--play-disc', str(fx.disc), '--install-disc', str(fx.disc)]), 0, out.getvalue())
        self.assertTrue(fx.artifact().is_file())

    def test_39_a_build_runs_its_long_stages_without_a_further_switch(self):
        fx, release = self.packaged(lambda fx: fx.stage('codegen').update(heavy=True))
        argv = ['--release', str(release), '--cache', str(self.cache), 'build', '--work', str(fx.work), '--play-disc', str(fx.disc), '--install-disc', str(fx.disc)]
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(argv), 0, out.getvalue())
        self.assertEqual(fx.executed(), ['decode', 'codegen', 'link'])
        with contextlib.redirect_stdout(io.StringIO()) as out:          # the old switch is still taken
            self.assertEqual(b.main(argv + ['--allow-heavy']), 0, out.getvalue())


    # The shader compiler the builder fetches from its publisher instead of carrying it (a synthetic archive here).

    def make_dmg(self, build):
        """a synthetic macOS NDK disk image (hdiutil, offline): build(folder) fills AndroidNDK1.app/Contents/NDK"""
        src = self.root / 'dmg-src'; ndk = src / 'AndroidNDK1.app/Contents/NDK'; ndk.mkdir(parents=True)
        build(ndk)
        dmg = self.root / 'ndk-fixture.dmg'
        subprocess.run(['/usr/bin/hdiutil', 'create', '-quiet', '-fs', 'HFS+', '-format', 'UDZO', '-srcfolder', str(src),
                        '-volname', 'NDK', str(dmg)], check=True)
        self.archive = dmg
        self.ndk.update(url='https://dl.google.com/android/repository/android-ndk-fixture-darwin.dmg', size=dmg.stat().st_size,
                        sha256=db.sha256_file(dmg), image_root='AndroidNDK1.app/Contents/NDK')

    @unittest.skipUnless(sys.platform == 'darwin', 'disk images are read on macOS only')
    def test_48_macos_disk_image_installs_the_declared_folder_under_the_zip_rules(self):
        def build(ndk):
            (ndk / 'bin').mkdir(); (ndk / 'bin/clang').write_bytes(b'SYNTHETIC compiler'); (ndk / 'bin/clang').chmod(0o755)
            (ndk / 'bin/clang++').symlink_to('clang')
            (ndk / 'NOTICE').write_text('synthetic notice', encoding='utf-8', newline='\n')
        self.make_dmg(build)
        self.fetch(); root = b.existing_ndk(self.cache, self.ndk)
        self.assertEqual((root / 'bin/clang++').read_bytes(), b'SYNTHETIC compiler')    # the link, as a verified copy
        self.assertFalse((root / 'bin/clang++').is_symlink())
        self.assertTrue(os.access(root / 'bin/clang', os.X_OK))
        self.assertEqual((root / 'NOTICE').read_text(encoding='utf-8'), 'synthetic notice')
        self.assertFalse(any(p.name.endswith('.app') for p in (self.cache / self.ndk['sha256'][:16]).rglob('*')))
        mounted = subprocess.run(['/usr/bin/hdiutil', 'info'], capture_output=True, text=True).stdout
        self.assertNotIn(str(self.cache), mounted, 'the image was detached')

    @unittest.skipUnless(sys.platform == 'darwin', 'disk images are read on macOS only')
    def test_49_macos_disk_image_refusals(self):
        def escaping(ndk):
            (ndk / 'bin').mkdir(); (ndk / 'bin/clang').symlink_to('../../../../outside')
        self.make_dmg(escaping)
        with self.assertRaisesRegex(b.Refusal, 'link escapes its root|invalid package path'), contextlib.redirect_stdout(io.StringIO()):
            b.fetch(self.release, self.cache, True, self.archive)
        self.assertFalse((self.cache / self.ndk['sha256'][:16]).exists())
        self.ndk['image_root'] = 'AndroidNDK2.app/Contents/NDK'
        with self.assertRaisesRegex(b.Refusal, 'no NDK folder at the declared root'), contextlib.redirect_stdout(io.StringIO()):
            b.fetch(self.release, self.cache, True, self.archive)
        for root in ('', '../x', '/abs'):
            self.ndk['image_root'] = root
            with self.subTest(image_root=root), self.assertRaises(b.Refusal): b.ndk_spec(self.release)
        mounted = subprocess.run(['/usr/bin/hdiutil', 'info'], capture_output=True, text=True).stdout
        self.assertNotIn(str(self.cache), mounted, 'every image was detached')


    def test_51_sign_unpacks_signs_and_writes_a_new_ipa(self):
        app = self.root / 'Payload' / 'FivePocket.app'; app.mkdir(parents=True)
        (app / 'GamePort').write_bytes(b'\xcf\xfa\xed\xfe synthetic'); (app / 'GamePort').chmod(0o755)
        (app / 'Info.plist').write_bytes(b'plist')
        sys.path.insert(0, str(REPO / 'tools/own_disc'))
        import ipa_writer, ios_sign
        unsigned = self.root / 'u.ipa'
        with contextlib.redirect_stdout(io.StringIO()):
            ipa_writer.write_ipa(app, unsigned)
        seen = []

        def fake_sign(app_dir, identity, profile, entitlements):
            seen.append((app_dir.name, identity, os.access(app_dir / 'GamePort', os.X_OK)))
            (app_dir / '_CodeSignature').mkdir(); (app_dir / '_CodeSignature' / 'CodeResources').write_bytes(b'sig')
            return ['.']
        with mock.patch.object(ios_sign, 'sign', fake_sign), contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(['sign', '--ipa', str(unsigned), '--identity', '-', '--out', str(self.root / 's.ipa')]), 0, out.getvalue())
        self.assertEqual(seen, [('FivePocket.app', '-', True)])
        names = zipfile.ZipFile(self.root / 's.ipa').namelist()
        self.assertIn('Payload/FivePocket.app/_CodeSignature/CodeResources', names)
        self.assertIn('Payload/FivePocket.app/GamePort', zipfile.ZipFile(unsigned).namelist())
        with contextlib.redirect_stdout(io.StringIO()) as out:              # never over an existing file
            self.assertEqual(b.main(['sign', '--ipa', str(unsigned), '--identity', '-', '--out', str(self.root / 's.ipa')]), 2)
        bad = self.root / 'bad.ipa'
        with zipfile.ZipFile(bad, 'w') as z:
            z.writestr('../escape', b'x')
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(b.main(['sign', '--ipa', str(bad), '--identity', '-', '--out', str(self.root / 'b.ipa')]), 2)
        self.assertIn('invalid package path', out.getvalue())


if __name__ == '__main__': unittest.main()
