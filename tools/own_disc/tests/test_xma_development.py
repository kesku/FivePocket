# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit development builds still report decoder identity differences."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_xma_decoder as xma

class DevelopmentDecoder(unittest.TestCase):
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

if __name__ == "__main__":
    unittest.main()
