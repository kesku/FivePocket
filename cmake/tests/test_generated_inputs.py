#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise private source-input validation without game data or the iOS SDK."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / 'GeneratedInputs.cmake'


class GeneratedSourceContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.game = self.root / 'generated'
        (self.game / 'ppc').mkdir(parents=True)
        (self.game / 'include').mkdir()
        (self.game / 'ppc/chunk.cpp').write_text('int synthetic() { return 42; }\n')

    def run_contract(self, manifest):
        (self.game / 'build-inputs.cmake').write_text(manifest)
        script = self.root / 'check.cmake'
        script.write_text(
            f'set(FIVEPOCKET_GAME_DIR [==[{self.game.as_posix()}]==])\n'
            f'include([==[{MODULE.as_posix()}]==])\n'
            'fivepocket_load_generated_inputs()\n'
            'file(WRITE "${FIVEPOCKET_GAME_DIR}/resolved.txt" '
            '"${FIVEPOCKET_GENERATED_SOURCES}\\n${FIVEPOCKET_GENERATED_RESOURCE_ROOT}\\n'
            '${FIVEPOCKET_GENERATED_RESOURCE_PREFIX}\\n${FIVEPOCKET_PAYLOAD_MANIFEST_SHA256}\\n")\n')
        return subprocess.run([shutil.which('cmake') or 'cmake', '-P', str(script)],
                              capture_output=True, text=True)

    def test_sources_and_payload_remain_explicit_and_hierarchical(self):
        payload = self.game / 'stages/ios-payload'
        (payload / 'r/settings').mkdir(parents=True)
        (payload / 'r/settings/setting.txt').write_text('synthetic setting\n')
        (payload / 'manifest.txt').write_text('synthetic manifest\n')
        result = self.run_contract('''set(FIVEPOCKET_GENERATED_SOURCES ppc/chunk.cpp)
set(FIVEPOCKET_GENERATED_INCLUDE_DIRS include)
set(FIVEPOCKET_GENERATED_RESOURCES stages/ios-payload/manifest.txt stages/ios-payload/r/settings/setting.txt)
set(FIVEPOCKET_GENERATED_RESOURCE_ROOT stages/ios-payload)
set(FIVEPOCKET_GENERATED_RESOURCE_PREFIX payload)
''')
        self.assertEqual(result.returncode, 0, result.stderr)
        resolved = (self.game / 'resolved.txt').read_text().splitlines()
        self.assertEqual(resolved[:3], [str(self.game / 'ppc/chunk.cpp'), str(payload), 'payload'])
        self.assertEqual(resolved[3], hashlib.sha256((payload / 'manifest.txt').read_bytes()).hexdigest())

    def test_prebuilt_runtime_is_not_a_generated_source(self):
        (self.game / 'ppc/prebuilt.o').write_bytes(b'synthetic object')
        result = self.run_contract('set(FIVEPOCKET_GENERATED_SOURCES ppc/prebuilt.o)\n'
                                   'set(FIVEPOCKET_GENERATED_INCLUDE_DIRS include)\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not prebuilt objects or libraries', result.stderr)

    def test_generated_source_cannot_resolve_outside_its_root(self):
        (self.root / 'outside.cpp').write_text('int outside;\n')
        (self.game / 'ppc/link.cpp').symlink_to(self.root / 'outside.cpp')
        result = self.run_contract('set(FIVEPOCKET_GENERATED_SOURCES ppc/link.cpp)\n'
                                   'set(FIVEPOCKET_GENERATED_INCLUDE_DIRS include)\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing or external generated', result.stderr)

    def test_missing_inputs_do_not_produce_a_build(self):
        result = self.run_contract('set(FIVEPOCKET_GENERATED_SOURCES ppc/missing.cpp)\n'
                                   'set(FIVEPOCKET_GENERATED_INCLUDE_DIRS include)\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('missing or external generated', result.stderr)


if __name__ == '__main__':
    unittest.main()
