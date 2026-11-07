#!/usr/bin/env python3
"""make_release.py: the inventory of a package folder, written and read back by the front end's own reader; what is
listed, what is left out, what refuses the run. Synthetic packages, no game data. MAKE_RELEASE names another copy of
the tool (the mutant runner beside this file)."""
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
TOOL = Path(os.environ.get('MAKE_RELEASE', HERE.parent / 'make_release.py'))
sys.path.insert(0, str(HERE.parent))
spec = importlib.util.spec_from_file_location('make_release_under_test', TOOL)
mr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mr)
import builder                     # the front end the inventory is for
import disc_builder as db


NDK = {'url': 'https://dl.google.com/android/repository/android-ndk-r27c-darwin.zip', 'sha256': 'ab' * 32, 'size': 10,
       'directory': 'android-ndk-r27c', 'unpacked_bytes': 100}
FILES = {'builder': b'launcher', 'python/python3': b'python', 'python/Lib/os.py': b'# os', 'tools/own_disc/builder.py': b'# front end',
         'profile/profile.json': b'{}', 'host-tools/xenos/main/XenosRecomp': b'translator', 'host-tools/notes.txt': b'notes',
         'bin/run': b'#!/bin/sh\n'}


class Release(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.package = Path(tmp.name).resolve() / 'package'
        for name, data in FILES.items():
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        (self.package / 'bin/run').chmod(0o755)
        (self.package / 'tools/own_disc/__pycache__').mkdir()
        (self.package / 'tools/own_disc/__pycache__/builder.cpython-312.pyc').write_bytes(b'cache')
        self.out = self.package / 'release.json'

    def make(self, host=None, **changed):
        given = dict(package=self.package, host=host or builder.host_id(), profile='profile/profile.json', python='python/python3',
                     ndk=dict(NDK), min_free_bytes=1 << 30, build_time='about half an hour', output=self.out)
        given.update(changed)
        return mr.make(**given)

    def test_the_inventory_is_what_the_front_end_reads(self):
        release = self.make()
        on_disk = json.loads(self.out.read_text(encoding='utf-8'))
        self.assertEqual(on_disk, release)
        self.assertEqual([r['path'] for r in release['files']], sorted(FILES))            # every file, by name; no cache, not itself
        self.assertEqual({r['path']: r['sha256'] for r in release['files']}, {n: db.sha256_file(self.package / n) for n in FILES})
        self.assertEqual((release['schema'], release['host'], release['profile'], release['python']),
                         ('builder-release/1', builder.host_id(), 'profile/profile.json', 'python/python3'))
        self.assertEqual(release['ndk'], dict(NDK, license_url='https://developer.android.com/ndk/downloads'))
        self.assertEqual(release['estimates'], {'min_free_bytes': 1 << 30, 'build_time': 'about half an hour'})
        loaded, root = builder.load_release(self.out)                                      # the consumer's own reader
        self.assertEqual((loaded, root), (release, self.package))
        self.assertEqual(builder.ndk_spec(loaded), release['ndk'])
        (self.package / 'host-tools/notes.txt').write_bytes(b'changed')
        with self.assertRaisesRegex(builder.Refusal, 'missing or changed'):
            builder.load_release(self.out)

    def test_a_package_without_its_own_python(self):
        release = self.make(python=None)
        self.assertNotIn('python', release)
        self.assertNotIn('python', builder.load_release(self.out)[0])

    def test_target_profiles_are_written_and_must_be_files_of_the_package(self):
        release = self.make(targets={'duo': 'profile/profile.json'})
        self.assertEqual(release['profiles'], {'duo': 'profile/profile.json'})
        self.assertEqual(builder.load_release(self.out)[0]['profiles'], {'duo': 'profile/profile.json'})
        self.out.unlink()
        with self.assertRaisesRegex(ValueError, 'duo profile is not a file'):
            self.make(targets={'duo': 'profile/duo.json'})
        with self.assertRaisesRegex(ValueError, 'only the duo target'):
            self.make(targets={'tv': 'profile/profile.json'})
        self.assertFalse(self.out.exists())

    def test_programs_are_recorded_by_executable_mode(self):
        files = self.make()['files']
        self.assertEqual({r['path'] for r in files if r.get('executable')}, {'bin/run'})
        self.assertTrue(all(set(r) <= {'path', 'sha256', 'executable'} for r in files))

    def test_no_time_estimate_no_field(self):
        self.assertEqual(self.make(build_time=None)['estimates'], {'min_free_bytes': 1 << 30})

    def refused(self, message, **changed):
        with self.assertRaisesRegex(ValueError, message):
            self.make(**changed)
        self.assertFalse(self.out.exists())

    def test_what_refuses_the_run_and_writes_nothing(self):
        self.refused('profile is not a file of the package', profile='profile/other.json')
        self.refused('Python is not a file of the package', python='python/python3.exe')
        self.refused('not pinned as the front end wants it', ndk=dict(NDK, url='https://example.org/android-ndk.zip'))
        self.refused('not pinned as the front end wants it', ndk=dict(NDK, sha256='zz'))
        for count in (0, -5, 1.5, None):
            self.refused('positive number of bytes', min_free_bytes=count)
        self.refused('package folder is not there', package=self.package / 'absent', output=self.package / 'absent' / 'release.json')
        self.refused('lies at the package root', output=self.package / 'tools' / 'release.json')
        (self.package / 'host-tools' / 'link.txt').symlink_to(self.package / 'host-tools' / 'notes.txt')
        self.refused('a link is in the package: host-tools/link.txt')
        (self.package / 'host-tools' / 'link.txt').unlink()
        (self.package / 'host-tools' / 'folder-link').symlink_to(self.package / 'bin', target_is_directory=True)
        self.refused('a link is in the package: host-tools/folder-link')
        (self.package / 'host-tools' / 'folder-link').unlink()
        (self.package / 'host-tools' / 'bad:name.txt').write_bytes(b'x')
        self.refused('front end would refuse this name: host-tools/bad:name.txt')

    def test_an_empty_package_is_refused(self):
        empty = self.package.parent / 'empty'; empty.mkdir()
        with self.assertRaisesRegex(ValueError, 'package is empty'):
            self.make(package=empty, output=empty / 'release.json')

    def test_an_existing_inventory_is_never_written_over(self):
        self.out.write_text('kept', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'exists already'):
            self.make()
        self.assertEqual(self.out.read_text(encoding='utf-8'), 'kept')

    def test_an_inventory_the_front_end_refuses_does_not_stay(self):
        with mock.patch.object(mr.builder, 'load_release', side_effect=builder.Refusal('not for this reader')):
            with self.assertRaisesRegex(ValueError, 'front end refuses the inventory just written: not for this reader'):
                self.make()
        self.assertFalse(self.out.exists())
        # for another host the reader cannot run here: it is not asked
        with mock.patch.object(mr.builder, 'load_release', side_effect=AssertionError('not this host')):
            self.make(host='win32-x86_64' if builder.host_id() != 'win32-x86_64' else 'linux-x86_64')
        self.assertTrue(self.out.exists())


    def test_a_macos_release_names_the_ndk_folder_inside_the_disk_image(self):
        dmg = dict(NDK, url='https://dl.google.com/android/repository/android-ndk-r27c-darwin.dmg')
        with self.assertRaisesRegex(ValueError, 'image root not declared'):
            self.make(host='darwin-arm64', ndk=dmg)
        self.assertFalse(self.out.exists())
        release = self.make(host='darwin-arm64', ndk=dict(dmg, image_root='AndroidNDK12479018.app/Contents/NDK'))
        self.assertEqual(release['ndk']['image_root'], 'AndroidNDK12479018.app/Contents/NDK')
        self.out.unlink()
        argv = ['--package', str(self.package), '--host', 'darwin-arm64', '--profile', 'profile/profile.json', '--python', 'python/python3',
                '--ndk-url', dmg['url'], '--ndk-sha256', NDK['sha256'], '--ndk-size', '10', '--ndk-directory', NDK['directory'],
                '--ndk-unpacked-bytes', '100', '--min-free-bytes', str(1 << 30), '--ndk-image-root', 'AndroidNDK12479018.app/Contents/NDK']
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mr.main(argv), 0)
        self.assertEqual(json.loads(self.out.read_text(encoding='utf-8'))['ndk']['image_root'], 'AndroidNDK12479018.app/Contents/NDK')

    def test_the_command_line(self):
        argv = ['--package', str(self.package), '--host', 'darwin-arm64', '--profile', 'profile/profile.json', '--python', 'python/python3',
                '--ndk-url', NDK['url'], '--ndk-sha256', NDK['sha256'], '--ndk-size', '10', '--ndk-directory', NDK['directory'],
                '--ndk-unpacked-bytes', '100', '--min-free-bytes', str(1 << 30)]
        with contextlib.redirect_stdout(io.StringIO()) as said:
            self.assertEqual(mr.main(argv), 0)
        self.assertIn(f'RELEASE {len(FILES)} files, 1 programs', said.getvalue())
        release = json.loads(self.out.read_text(encoding='utf-8'))                         # the default output: the package root
        self.assertEqual((release['ndk']['size'], release['ndk']['unpacked_bytes'], release['estimates']), (10, 100, {'min_free_bytes': 1 << 30}))
        with contextlib.redirect_stderr(io.StringIO()) as said:
            self.assertEqual(mr.main(argv), 2)                                             # it is there now
        self.assertIn('RELEASE_REFUSED: the inventory exists already', said.getvalue())
        self.out.unlink()
        elsewhere = self.package / 'named.json'
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mr.main(argv + ['--output', str(elsewhere), '--build-time', 'an hour']), 0)
        self.assertEqual(json.loads(elsewhere.read_text(encoding='utf-8'))['estimates']['build_time'], 'an hour')
        self.assertNotIn('dxc', json.loads(elsewhere.read_text(encoding='utf-8')))


if __name__ == '__main__':
    unittest.main()
