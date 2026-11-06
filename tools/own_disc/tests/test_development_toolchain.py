# SPDX-License-Identifier: GPL-3.0-or-later
"""Development mode changes identity certification, not input or compiler failure checks."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_xma_decoder as xma
import ios_compile
import replay_ios_build as replay


class DevelopmentReplay(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.source = self.root / 'source.cpp'; self.source.write_bytes(b'int source;')
        self.manifest = self.root / 'manifest.json'
        self.data = {'schema': 'frozen-build-apple/1', 'xcode': str(self.root / 'xcode'),
                     'inputs': {str(self.source): {'size': self.source.stat().st_size,
                       'sha256': hashlib.sha256(self.source.read_bytes()).hexdigest()}},
                     'toolchain_inputs': {'old-sdk.h': {'sha256': '0' * 64}},
                     'units': [{'source': str(self.source), 'object': '/old/source.o', 'origin': 'test',
                        'object_sha256': '0' * 64, 'object_identity': {'old': True},
                        'command': ['clang', '-c', str(self.source), '-o', '/old/source.o']}],
                     'link': {'command': ['clang', '/old/source.o', '-o', '/old/App'],
                              'unsigned_sha256': '0' * 64, 'reference_code': {'old': True}}}
        self.manifest.write_text(json.dumps(self.data))

    def run_replay(self, development, fail=None):
        def command(argv, **kwargs):
            kind = 'compile' if '-c' in argv else 'link'
            if kind == fail:
                return subprocess.CompletedProcess(argv, 1, '', 'synthetic failure')
            Path(argv[argv.index('-o') + 1]).write_bytes(b'new code')
            return subprocess.CompletedProcess(argv, 0, '', '')
        args = ['--manifest', str(self.manifest), '--out', str(self.root / 'out')]
        if development: args.append('--development-toolchain')
        with mock.patch.object(replay.subprocess, 'run', side_effect=command), \
             mock.patch.object(replay.macho_sections, 'code_identity', return_value={'new': True}):
            return replay.main(args)

    def test_release_mode_still_refuses_changed_sdk(self):
        self.assertEqual(self.run_replay(False), 2)
        self.assertFalse((self.root / 'out').exists())

    def test_development_mode_records_differences_without_claiming_equivalence(self):
        self.assertEqual(self.run_replay(True), 0)
        report = json.loads((self.root / 'out/report.json').read_text())
        self.assertEqual(report['verification'], 'development')
        self.assertFalse(report['release_equivalent'])
        self.assertFalse(report['units'][0]['code_equal'])
        self.assertEqual(report['toolchain_differences'][0]['path'], 'old-sdk.h')

    def test_development_mode_refuses_modified_source(self):
        self.source.write_bytes(b'changed source')
        self.assertEqual(self.run_replay(True), 2)
        self.assertFalse((self.root / 'out').exists())

    def test_compile_failure_is_not_accepted(self):
        self.assertEqual(self.run_replay(True, 'compile'), 1)

    def test_link_failure_is_not_accepted(self):
        self.assertEqual(self.run_replay(True, 'link'), 1)


class DevelopmentPlan(unittest.TestCase):

    def test_local_sources_require_explicit_development_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaises(SystemExit):
                ios_compile.main(['--kit', str(root / 'kit'), '--work', str(root / 'work'),
                                  '--source-root', str(root / 'source')])
            self.assertFalse((root / 'work').exists())
            with mock.patch.object(ios_compile.ios_kit, 'assemble', return_value=root / 'tree') as assemble, \
                 mock.patch.object(ios_compile.replay_ios_build, 'main', return_value=0):
                self.assertEqual(ios_compile.main(['--kit', str(root / 'kit'), '--work', str(root / 'work'),
                                                  '--source-root', str(root / 'source'), '--development-toolchain']), 0)
            self.assertEqual(assemble.call_args.args[3], root / 'source')

    def test_compile_stage_forwards_explicit_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with mock.patch.object(ios_compile.ios_kit, 'assemble', return_value=root / 'tree'), \
                 mock.patch.object(ios_compile.replay_ios_build, 'main', return_value=0) as run:
                self.assertEqual(ios_compile.main(['--kit', str(root / 'kit'), '--work', str(root / 'work'),
                                                   '--development-toolchain']), 0)
            self.assertIn('--development-toolchain', run.call_args.args[0])

    def test_xma_development_mode_preserves_difference_in_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); plan = root / 'plan.json'; plan.write_text('{}')
            output = root / 'out'
            record = {'status': 'differs', 'revision': 'fixture', 'units': 1,
                      'archives': {'lib.a': {'sha256': '1' * 64, 'bytes': 1, 'as_the_plan_expects': False}}}
            def build(*args, **kwargs):
                output.mkdir(exist_ok=True)
                return dict(record)
            argv = ['--plan', str(plan), '--source-archive', str(root / 'source.tar'),
                    '--output', str(output), '--xcode', str(root / 'xcode')]
            with mock.patch.object(xma, 'check_plan', return_value={'target': 'arm64-apple-ios17.4', 'expect': {'lib.a': '0' * 64}}), \
                 mock.patch.object(xma, 'build', side_effect=build):
                self.assertEqual(xma.main(argv), 1)
                self.assertEqual(xma.main(argv + ['--development-toolchain']), 0)
            report = json.loads((output / 'report.json').read_text())
            self.assertEqual(report['status'], 'differs')
            self.assertFalse(report['release_equivalent'])


if __name__ == '__main__':
    unittest.main()
