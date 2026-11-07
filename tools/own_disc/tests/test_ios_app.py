#!/usr/bin/env python3
"""Duo app packaging and payload validation on synthetic inputs."""
import hashlib
import json
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ios_app  # noqa: E402

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


if __name__ == "__main__":
    unittest.main()
