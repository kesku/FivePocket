# SPDX-License-Identifier: GPL-3.0-or-later
"""The Duo entry point consumes legacy Mac inventories without choosing their iOS default."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import builder


class DuoBuilder(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        for name in ('ios.json', 'duo.json'):
            (self.root / name).write_text('{}', encoding='utf-8')
        self.release = self.root / 'release.json'
        self.record = {'schema': 'builder-release/1', 'host': builder.host_id(), 'profile': 'ios.json',
                       'profiles': {'ios': 'ios.json', 'duo': 'duo.json'},
                       'files': [{'path': name, 'sha256': builder.db.sha256_file(self.root / name)}
                                 for name in ('ios.json', 'duo.json')]}
        self.release.write_text(json.dumps(self.record), encoding='utf-8')

    def test_legacy_inventory_is_verified_and_duo_is_selected(self):
        release, root = builder.load_release(self.release)
        self.assertEqual(root, self.root)
        self.assertEqual(builder.profile_of(release), 'duo.json')
        (self.root / 'ios.json').write_text('changed', encoding='utf-8')
        with self.assertRaisesRegex(builder.Refusal, 'missing or changed'):
            builder.load_release(self.release)

    def test_default_build_uses_duo_profile_and_persistent_identifier(self):
        seen = []
        def build(args):
            seen.append((args, builder.os.environ['BUILDER_BUNDLE_ID']))
            return 0
        args = ['--release', str(self.release), 'build', '--work', str(self.root / 'work'),
                '--play-disc', 'play.iso', '--install-disc', 'install.iso', '--bundle-id', 'org.example.duo']
        with mock.patch.object(builder, 'check', return_value=self.root / 'ndk'), \
                mock.patch.object(builder.db, 'main', side_effect=build), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(builder.main(args), 0)
        command, identifier = seen[0]
        self.assertEqual(command[command.index('--profile') + 1], str(self.root / 'duo.json'))
        self.assertEqual(identifier, 'org.example.duo')
        self.assertIn('--allow-heavy', command)

    def test_other_app_targets_are_not_accepted(self):
        for target in ('ios', 'thor', 'fold'):
            with self.subTest(target=target), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as refused:
                builder.main(['build', '--play-disc', 'play.iso', '--install-disc', 'install.iso', '--' + target])
            self.assertEqual(refused.exception.code, 2)

    def test_a_release_without_duo_is_refused(self):
        self.record['profiles'].pop('duo')
        self.release.write_text(json.dumps(self.record), encoding='utf-8')
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(builder.main(['--release', str(self.release), 'check']), 2)
        self.assertIn('no Duo profile', output.getvalue())

    def test_instructions_only_offer_duo(self):
        instructions = builder.how_to(self.root / 'missing.json')
        self.assertIn('FivePocket Duo', instructions)
        self.assertIn('--install-disc', instructions)
        for other in ('--ios', '--thor', '--fold', 'iPad', 'Windows', 'builder verify'):
            self.assertNotIn(other, instructions)


if __name__ == '__main__':
    unittest.main()
