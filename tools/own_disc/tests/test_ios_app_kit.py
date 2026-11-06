#!/usr/bin/env python3
"""ios_app.py and ios_kit.py on synthetic folders (no game data, no Xcode: the Xcode fields are stood in for).

ios_app: the app gets the executable, the template's keys with the player's bundle id, the payload from the stages with
its manifest; art or sound in the app files, a bad bundle id and a stage file with other bytes are refused.
ios_kit: make keeps stage-made files out of the kit; assemble puts them back from the stages and refuses other bytes.

Run: python3 tools/own_disc/tests/test_ios_app_kit.py
"""
import hashlib
import json
import plistlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ios_app  # noqa: E402
import ios_kit  # noqa: E402

FIELDS = {'DTXcode': '2650', 'DTXcodeBuild': '17F42', 'DTSDKName': 'iphoneos26.5', 'DTPlatformVersion': '26.5',
          'DTSDKBuild': '23F73', 'DTPlatformBuild': '23F73'}
sha = lambda b: hashlib.sha256(b).hexdigest()


class App(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.files = self.root / 'app-files'; self.files.mkdir()
        with (self.files / 'Info.plist').open('wb') as f:
            plistlib.dump({'CFBundleExecutable': 'Game', 'CFBundleIdentifier': 'owner.private.id', 'BuildMachineOSBuild': 'X',
                          'UIDeviceFamily': [1, 2], 'UISupportedInterfaceOrientations~ipad': ['portrait']}, f)
        (self.files / 'Font.ttf').write_bytes(b'font')
        self.exe = self.root / 'exe'; self.exe.write_bytes(b'\xcf\xfa\xed\xfe' + b'\0' * 28)
        self.stage = self.root / 'decode'; self.stage.mkdir(); (self.stage / 'image.bin').write_bytes(b'IMAGE')
        self.map = {'schema': 'ios-payload/1', 'switches': ['private-trace/diag-b', 'private-trace/diag-a'],
                    'files': {'private-trace/run-1/decoded.image.bin': {'stage': 'decode', 'path': 'image.bin', 'sha256': sha(b'IMAGE')}}}

    def payload(self, out='payload'):
        return ios_app.build_payload(self.map, {'decode': self.stage}, self.root / out)

    def build(self, bundle='org.example.player', out='A.app'):
        if not (self.root / 'payload').exists():
            self.payload()
        with mock.patch.object(ios_app, 'xcode_fields', return_value=FIELDS):
            return ios_app.build_app(self.exe, self.files, self.root / 'payload', bundle, Path('/x'), self.root / out)

    def test_the_payload_is_sorted_with_its_switches_and_its_manifest_hash(self):
        digest = self.payload()
        manifest = (self.root / 'payload/manifest.txt').read_text()
        self.assertEqual(manifest, f'{sha(b"1")} 1 private-trace/diag-a\n{sha(b"1")} 1 private-trace/diag-b\n'
                                   f'{sha(b"IMAGE")} 5 private-trace/run-1/decoded.image.bin\n')
        self.assertEqual(digest, sha(manifest.encode()))
        self.assertEqual((self.root / 'payload/manifest.sha256').read_text(), digest + '\n')
        (self.stage / 'image.bin').write_bytes(b'OTHER')
        with self.assertRaisesRegex(SystemExit, 'not the expected file'):
            self.payload('p2')
        self.map['switches'].append('.hidden')
        with self.assertRaisesRegex(SystemExit, 'not plain'):
            self.payload('p3')

    def test_the_app_is_the_executable_the_template_and_the_payload(self):
        self.build()
        app = self.root / 'A.app'
        with (app / 'Info.plist').open('rb') as f:
            info = plistlib.load(f)
        self.assertEqual(info['CFBundleIdentifier'], 'org.example.player')
        self.assertEqual(info['DTSDKName'], 'iphoneos26.5')
        self.assertNotIn('BuildMachineOSBuild', info)
        self.assertEqual(info['UIDeviceFamily'], [1])
        self.assertNotIn('UISupportedInterfaceOrientations~ipad', info)
        self.assertEqual((app / 'Game').read_bytes(), self.exe.read_bytes())
        self.assertEqual((app / 'payload/r/private-trace/run-1/decoded.image.bin').read_bytes(), b'IMAGE')
        self.assertEqual((app / 'payload/manifest.txt').read_bytes(), (self.root / 'payload/manifest.txt').read_bytes())
        self.assertEqual((app / 'Font.ttf').read_bytes(), b'font')

    def test_refusals(self):
        with self.assertRaisesRegex(SystemExit, 'not a bundle identifier'):
            self.build(bundle='no dots')
        for art in ('alert-restart.png', 'sound.m4a'):
            (self.files / art).write_bytes(b'x')
            with self.subTest(art=art), self.assertRaisesRegex(SystemExit, 'art or sound'):
                self.build(out='C.app')
            (self.files / art).unlink()
        self.assertFalse((self.root / 'C.app').exists())


class Kit(unittest.TestCase):
    def test_relocated_sources_are_pinned_without_changing_the_original_kit(self):
        self.assertIn('repo/android/app/src/main/cpp/', ios_kit.SOURCE_ROOTS)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            kit, source = root / 'kit', root / 'source'
            places, inputs = {}, {}
            for old, new in ios_kit.SOURCE_ROOTS.items():
                rel = old + 'shared.h'
                original = kit / 'files' / rel
                original.parent.mkdir(parents=True)
                original.write_bytes(b'original source')
                local = source / new / 'shared.h'
                local.parent.mkdir(parents=True)
                local.write_bytes(b'local source')
                places[rel] = {'kit': rel, 'sha256': sha(b'original source')}
                inputs['/r/' + rel.removeprefix('repo/')] = {'size': 15, 'sha256': sha(b'original source')}
            record = {'stages': [], 'snapshot_dir': '/old/tree', 'places': places,
                      'frozen': {'inputs': inputs, 'snapshot': {'map': [['/r', '@TREE/repo']], 'rewritten': {}}}}
            (kit / 'kit.json').write_text(json.dumps(record))
            with mock.patch.object(ios_kit, 'clone', side_effect=shutil.copyfile):
                tree = ios_kit.assemble(kit, {}, root / 'tree', source)
            frozen = json.loads((tree / 'frozen.json').read_text())
            self.assertEqual(len(frozen['source_overrides']), 3)
            for rel in places:
                self.assertEqual((tree / rel).read_bytes(), b'local source')
                self.assertEqual((kit / 'files' / rel).read_bytes(), b'original source')
            self.assertTrue(all(r == {'size': 12, 'sha256': sha(b'local source')}
                                for r in frozen['inputs'].values()))

    def test_source_override_refuses_missing_local_or_changed_kit_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            kit, source = root / 'kit', root / 'source'
            rel = 'repo/android/app/src/main/cpp/shared.h'
            original = kit / 'files' / rel
            original.parent.mkdir(parents=True)
            original.write_bytes(b'original')
            record = {'stages': [], 'places': {rel: {'kit': rel, 'sha256': sha(b'original')}},
                      'frozen': {'inputs': {'/r/android/app/src/main/cpp/shared.h': {}},
                                 'snapshot': {'map': [['/r', '@TREE/repo']]}}}
            (kit / 'kit.json').write_text(json.dumps(record))
            with self.assertRaisesRegex(SystemExit, 'relocated source missing'):
                ios_kit.assemble(kit, {}, root / 'missing', source)
            self.assertFalse((root / 'missing').exists())
            local = source / 'runtime/host/shared.h'
            local.parent.mkdir(parents=True)
            local.write_bytes(b'local')
            original.write_bytes(b'changed')
            with self.assertRaisesRegex(SystemExit, 'source-kit input changed'):
                ios_kit.assemble(kit, {}, root / 'changed', source)
            self.assertFalse((root / 'changed').exists())

    def test_unused_kit_copies_are_not_required_from_the_source_checkout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            kit = root / 'kit'; kit.mkdir()
            rel = 'repo/tools/host_guest_trace/unused.inc'
            record = {'places': {rel: {'kit': rel}},
                      'frozen': {'inputs': {}, 'snapshot': {'map': []}}}
            self.assertEqual(ios_kit.local_sources(kit, record, root / 'source'), {})

    def test_stage_files_stay_out_of_the_kit_and_come_back_checked(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            snap = t / 'snap'; (snap / 'repo/src').mkdir(parents=True)
            (snap / 'repo/src/own.cpp').write_bytes(b'our code')
            (snap / 'repo/src/chunk.cpp').write_bytes(b'generated chunk')
            (snap / 'repo/src/empty').mkdir()
            (snap / 'repo/link').symlink_to('src')
            stage = t / 'recipe'; stage.mkdir(); (stage / 'c.cpp').write_bytes(b'generated chunk')
            manifest = {'schema': 'frozen-build-apple/1', 'link': {'command': ['ld', '-o', '/r/out']},
                        'inputs': {'/r/src/own.cpp': {'sha256': sha(b'our code'), 'size': 8},
                                   '/r/src/chunk.cpp': {'sha256': sha(b'generated chunk'), 'size': 15}},
                        'snapshot': {'dir': str(snap), 'map': [['/r', str(snap / 'repo')]], 'rewritten': {}}}
            (t / 'm.json').write_text(json.dumps(manifest))
            import contextlib, io
            with contextlib.redirect_stdout(io.StringIO()):
                ios_kit.make(t / 'm.json', [('recipe', stage)], t / 'kit')
            kit_files = {p.relative_to(t / 'kit/files').as_posix() for p in (t / 'kit/files').rglob('*') if p.is_file()}
            self.assertEqual(kit_files, {'repo/src/own.cpp'})
            with contextlib.redirect_stdout(io.StringIO()):
                tree = ios_kit.assemble(t / 'kit', {'recipe': stage}, t / 'tree')
            self.assertEqual((tree / 'repo/src/chunk.cpp').read_bytes(), b'generated chunk')
            self.assertTrue((tree / 'repo/src/empty').is_dir() and (tree / 'repo/link').is_symlink())
            frozen = json.loads((tree / 'frozen.json').read_text())
            self.assertEqual(frozen['snapshot']['map'], [['/r', str(tree / 'repo')]])
            (stage / 'c.cpp').write_bytes(b'another run')
            with self.assertRaisesRegex(SystemExit, 'did not write the expected'), contextlib.redirect_stdout(io.StringIO()):
                ios_kit.assemble(t / 'kit', {'recipe': stage}, t / 'tree2')

    def test_unread_stage_copies_and_first_line_variants_stay_out_of_the_kit(self):
        # 2026-11-06: the source folders held an unread copy of a shader table and a table the compile reads whose only
        # difference from the stage's file is its first comment line; neither may be carried in the kit
        import contextlib, io
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            snap = t / 'snap'; (snap / 'repo/src').mkdir(parents=True)
            table = b'{0x1234,5,{0xd1}},\n{0x5678,4,{0x88}},\n'
            (snap / 'repo/src/own.cpp').write_bytes(b'our code')
            (snap / 'repo/src/unread_table.inc').write_bytes(b'// stage header\n' + table)
            (snap / 'repo/src/read_table.inc').write_bytes(b'// Generated by an old private tool\n' + table)
            stage = t / 'fixes'; stage.mkdir()
            (stage / 'unread_table.inc').write_bytes(b'// stage header\n' + table)
            (stage / 'read_table.inc').write_bytes(b'// Generated by the stage tool\n' + table)
            read = (snap / 'repo/src/read_table.inc').read_bytes()
            manifest = {'schema': 'frozen-build-apple/1', 'link': {'command': ['ld', '-o', '/r/out']},
                        'inputs': {'/r/src/own.cpp': {'sha256': sha(b'our code'), 'size': 8},
                                   '/r/src/read_table.inc': {'sha256': sha(read), 'size': len(read)}},
                        'snapshot': {'dir': str(snap), 'map': [['/r', str(snap / 'repo')]], 'rewritten': {}}}
            (t / 'm.json').write_text(json.dumps(manifest))
            with contextlib.redirect_stdout(io.StringIO()):
                rec = ios_kit.make(t / 'm.json', [('fixes', stage)], t / 'kit')
            kit_files = {p.relative_to(t / 'kit/files').as_posix() for p in (t / 'kit/files').rglob('*') if p.is_file()}
            self.assertEqual(kit_files, {'repo/src/own.cpp'})
            self.assertEqual(rec['places']['repo/src/unread_table.inc']['stage'], 'fixes')
            self.assertEqual(rec['places']['repo/src/read_table.inc']['first_line'], '// Generated by an old private tool')
            with contextlib.redirect_stdout(io.StringIO()):
                tree = ios_kit.assemble(t / 'kit', {'fixes': stage}, t / 'tree')
            self.assertEqual((tree / 'repo/src/read_table.inc').read_bytes(), read)
            self.assertEqual((tree / 'repo/src/unread_table.inc').read_bytes(), b'// stage header\n' + table)
            (stage / 'read_table.inc').write_bytes(b'// Generated by the stage tool\n{0x1234,5,{0xd2}},\n')
            with self.assertRaisesRegex(SystemExit, 'did not write the expected'), contextlib.redirect_stdout(io.StringIO()):
                ios_kit.assemble(t / 'kit', {'fixes': stage}, t / 'tree2')

    def test_a_link_flag_goes_after_the_driver_and_a_reference_linked_without_it_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            snap = t / 'snap'; (snap / 'repo').mkdir(parents=True); (snap / 'repo/own.cpp').write_bytes(b'our code')
            manifest = {'schema': 'frozen-build-apple/1', 'units': [], 'link': {'command': ['clang++', '-O2', '-o', '/r/out']},
                        'inputs': {'/r/own.cpp': {'sha256': sha(b'our code'), 'size': 8}},
                        'snapshot': {'dir': str(snap), 'map': [['/r', str(snap / 'repo')]], 'rewritten': {}}}
            (t / 'm.json').write_text(json.dumps(manifest))
            import contextlib, io
            with contextlib.redirect_stdout(io.StringIO()):
                rec = ios_kit.make(t / 'm.json', [], t / 'kit', link_flags=('-Wl,-objc_stubs_small',))
            self.assertEqual(rec['frozen']['link']['command'], ['clang++', '-Wl,-objc_stubs_small', '-O2', '-o', '/r/out'])
            self.assertEqual(rec['link_flags'], ['-Wl,-objc_stubs_small'])
            ref = t / 'ref'; ref.mkdir()
            (ref / 'report.json').write_text(json.dumps({'units': [], 'link': {'argv': ['clang++', '-O2', '-o', str(ref / 'out')]}}))
            with self.assertRaisesRegex(SystemExit, 'without -Wl,-objc_stubs_small'), contextlib.redirect_stdout(io.StringIO()):
                ios_kit.make(t / 'm.json', [], t / 'kit2', reference_replay=ref, link_flags=('-Wl,-objc_stubs_small',))

    def test_a_neutral_kit_names_no_folder_of_the_building_mac_and_assembles_the_same(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            snap = t / 'snap'; (snap / 'repo/src').mkdir(parents=True)
            (snap / 'repo/src/own.cpp').write_bytes(b'#include "' + str(snap).encode() + b'/repo/src/h.h"\n')
            (snap / 'repo/src/plain.cpp').write_bytes(b'// was /home/builder/repo/src\n')
            manifest = {'schema': 'frozen-build-apple/1', 'units': [], 'link': {'command': ['ld', '-o', '/home/builder/repo/out']},
                        'inputs': {'/home/builder/repo/src/own.cpp': {'sha256': sha((snap / 'repo/src/own.cpp').read_bytes()), 'size': 1},
                                   '/home/builder/repo/src/plain.cpp': {'sha256': sha(b'// was /home/builder/repo/src\n'), 'size': 30}},
                        'snapshot': {'dir': str(snap), 'map': [['/home/builder/repo', str(snap / 'repo')]],
                                     'rewritten': {'/home/builder/repo/src/own.cpp': 'x'}}}
            (t / 'm.json').write_text(json.dumps(manifest))
            import contextlib, io
            with contextlib.redirect_stdout(io.StringIO()):
                ios_kit.make(t / 'm.json', [], t / 'kit')
                rec = ios_kit.neutral(t / 'kit', t / 'kit-n')
            every = (t / 'kit-n/kit.json').read_text() + ''.join(p.read_text() for p in (t / 'kit-n/files').rglob('*') if p.is_file())
            self.assertNotIn('/home/builder', every)
            self.assertNotIn(str(snap), every)
            self.assertEqual(rec['frozen']['inputs']['/fp-src/r1/src/plain.cpp'],
                             {'sha256': sha(b'// was /fp-src/r1/src\n'), 'size': len(b'// was /fp-src/r1/src\n')})
            with contextlib.redirect_stdout(io.StringIO()):
                tree = ios_kit.assemble(t / 'kit-n', {}, t / 'tree')
            self.assertEqual((tree / 'repo/src/own.cpp').read_bytes(), b'#include "' + str(tree).encode() + b'/repo/src/h.h"\n')
            self.assertEqual(json.loads((tree / 'frozen.json').read_text())['snapshot']['map'], [['/fp-src/r1', str(tree / 'repo')]])
            with self.assertRaisesRegex(SystemExit, 'neutral already'), contextlib.redirect_stdout(io.StringIO()):
                ios_kit.neutral(t / 'kit-n', t / 'kit-nn')

    def test_the_replay_reads_the_link_file_list_where_the_map_puts_it(self):
        import replay_ios_build as rb, subprocess as sp
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            (t / 'here').mkdir(); (t / 'here/shell.LinkFileList').write_text('')       # only the mapped place exists
            manifest = {'schema': 'frozen-build-apple/1', 'xcode': str(t), 'units': [], 'inputs': {}, 'toolchain_inputs': {},
                        'link': {'command': ['clang++', '-filelist', '/first-mac/repo/shell.LinkFileList', '-o', '/first-mac/repo/app/GamePort'],
                                 'unsigned_sha256': '0' * 64}}
            (t / 'm.json').write_text(json.dumps(manifest))
            seen = []

            def fake_run(argv, **kw):
                seen.append(argv)
                Path(argv[argv.index('-o') + 1]).write_bytes(b'linked')
                return sp.CompletedProcess(argv, 0, '', '')
            import contextlib, io
            with mock.patch.object(rb.subprocess, 'run', fake_run), contextlib.redirect_stdout(io.StringIO()):
                rb.main(['--manifest', str(t / 'm.json'), '--out', str(t / 'out'), '--map', f'/first-mac/repo={t / "here"}'])
            self.assertEqual(seen[0][1:3], ['-filelist', str(t / 'out' / 'shell.LinkFileList')])
            self.assertEqual(json.loads((t / 'out/report.json').read_text())['link']['error'], '')
            # the freeze's own record of the list, with no list file anywhere (a player's Mac)
            (t / 'here/shell.LinkFileList').unlink()
            manifest['link']['file_lists'] = {'/first-mac/repo/shell.LinkFileList': []}
            (t / 'm.json').write_text(json.dumps(manifest))
            with mock.patch.object(rb.subprocess, 'run', fake_run), contextlib.redirect_stdout(io.StringIO()):
                rb.main(['--manifest', str(t / 'm.json'), '--out', str(t / 'out2'), '--map', f'/first-mac/repo={t / "here"}'])
            self.assertEqual((t / 'out2/shell.LinkFileList').read_text(), '')
            self.assertEqual(json.loads((t / 'out2/report.json').read_text())['link']['error'], '')

    def test_an_icon_that_is_not_an_opaque_1024_png_is_refused_before_actool(self):
        import struct, zlib
        def png(w, h, color_type):
            ihdr = struct.pack('>IIBBBBB', w, h, 8, color_type, 0, 0, 0)
            chunk = lambda kind, data: struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
            return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IEND', b'')
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            for name, data, why in (('small.png', png(512, 512, 2), '1024x1024'), ('alpha.png', png(1024, 1024, 6), 'alpha'),
                                    ('text.png', b'not a png', '1024x1024')):
                (t / name).write_bytes(data)
                with self.subTest(name), self.assertRaisesRegex(SystemExit, why), mock.patch.object(ios_app.subprocess, 'run') as run:
                    ios_app.compile_icon(t / name, t, t, '17.4')
                run.assert_not_called()

    def test_private_build_text_in_the_executable_is_found_in_any_case(self):
        self.assertEqual(ios_app.private_markers(b'\0FivePocket\0v507 Apple Port\0Private Build\0'), ['private build', 'apple port', 'v507 apple'])
        self.assertEqual(ios_app.private_markers(b'\0FivePocket\0Controller\0About\0'), [])
        self.assertEqual(ios_app.private_markers('\0v507 · Apple Port\0'.encode('utf-16le')), ['apple port', 'v507 apple'][:1])


if __name__ == '__main__':
    unittest.main()
