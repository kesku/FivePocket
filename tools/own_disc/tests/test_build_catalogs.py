#!/usr/bin/env python3
"""The catalog stage on synthetic discs and package listings: names and sizes must come from the disc, timestamps and
extra files from the table, and the bytes must be what the runtime's own readers accept. BUILD_CATALOGS names the
tool (the mutant runner points it at changed copies). No game data."""
import importlib.util
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
for directory in ('tools/own_disc', 'tools/own_disc/tests', 'tools/host_guest_trace', 'tools/host_guest_trace/tests'):
    sys.path.insert(0, str(ROOT / directory))
TARGET = Path(os.environ.get('BUILD_CATALOGS', HERE.parent / 'build_catalogs.py'))
spec = importlib.util.spec_from_file_location('catalogs_under_test', TARGET)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
import file_metadata_catalog as metadata  # noqa: E402
import content_catalog  # noqa: E402
from disc_builder import XisoDisc  # noqa: E402
from test_disc_builder import make_xiso  # noqa: E402


def stamps(n, base=1000):
    return [[base + 10 * i + k for k in range(4)] for i in range(n)]


def display(text):
    return text.encode('utf-16-be').ljust(256, b'\0')


class PlayCatalog(unittest.TestCase):
    def listing(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        image = Path(tmp.name) / 'play.iso'
        made = make_xiso(image, {'b.bin': bytes(3000), 'A.TXT': b'x'})
        return m.disc_listing(XisoDisc(image)), made

    def test_disc_listing_has_every_folder_and_file_with_its_size(self):
        listing, _ = self.listing()
        self.assertEqual(sorted((p, f) for p, _, f in listing), [('a.txt', False), ('b.bin', False), ('sub', True), ('sub/inner.bin', False)])
        sizes = {p: n for p, n, f in listing if not f}
        self.assertEqual((sizes['a.txt'], sizes['b.bin']), (1, 3000))

    def test_records_come_in_path_order_with_sizes_allocation_and_the_tables_times(self):
        listing, _ = self.listing()
        extras = [('zz.image', 2048, False), ('extra.bin', 5, False)]
        data = m.metadata_catalog('game:', listing + extras, stamps(7))
        records = metadata.parse_catalog(data)
        self.assertEqual(list(records), ['game:\\', 'game:\\a.txt', 'game:\\b.bin', 'game:\\extra.bin', 'game:\\sub',
                                         'game:\\sub\\inner.bin', 'game:\\zz.image'])
        for i, (path, values) in enumerate(records.items()):
            self.assertEqual(list(values[:4]), stamps(7)[i], path)
        self.assertEqual(records['game:\\b.bin'][4:], (4096, 3000, 1))        # allocation, size, attribute
        self.assertEqual(records['game:\\a.txt'][4:], (2048, 1, 1))
        self.assertEqual(records['game:\\zz.image'][4:], (2048, 2048, 1))
        self.assertEqual(records['game:\\sub'][4:], (0, 0, 0x11))
        self.assertEqual(records['game:\\'][4:], (0, 0, 0x11))

    def test_a_table_with_another_number_of_rows_is_refused(self):
        listing, _ = self.listing()
        for rows in (4, 6):
            with self.assertRaises(ValueError):
                m.metadata_catalog('game:', listing, stamps(rows))
        m.metadata_catalog('game:', listing, stamps(5))

    def test_a_row_that_is_not_four_whole_timestamps_is_refused(self):
        listing, _ = self.listing()
        for bad in ([1, 2, 3], [1, 2, 3, 4, 5], [1, 2, 3, -4], [1, 2, 3, 4.0], [1, 2, 3, 1 << 63], 'abcd'):
            table = stamps(5)
            table[2] = bad
            with self.assertRaises(ValueError, msg=repr(bad)):
                m.metadata_catalog('game:', listing, table)

    def test_an_extra_file_that_is_also_on_the_disc_is_refused(self):
        listing, _ = self.listing()
        with self.assertRaises(ValueError):
            m.metadata_catalog('game:', listing + [('B.BIN', 7, False)], stamps(5))   # five distinct paths: only the clash can refuse


class Packages(unittest.TestCase):
    PACKAGES = [('ab01', display('second'), [('part1.rpf', 5000, False), ('part1.timestamp', 14, False)]),
                ('ab00', display('first'), [('part0.rpf', 9, False), ('part0.timestamp', 14, False)])]

    def table(self, packages=2, extras=()):
        return {'schema': 'catalog-times/1', 'play': {'extras': [dict(name=n, bytes=b) for n, b in extras], 'times': stamps(2 + len(extras))},
                'packages': [{'times': stamps(3, 5000 + 100 * i)} for i in range(packages)]}

    def test_packages_are_numbered_and_listed_by_upper_case_name(self):
        out = m.build([('only.bin', 4, False)], self.PACKAGES, self.table(), 0x12345678)
        self.assertEqual(sorted(out), ['file-metadata.bin', 'installed-content.bin', 'installed-package-1.bin', 'installed-package-2.bin'])
        self.assertEqual(content_catalog.parse_catalog(out['installed-content.bin']), (0x12345678, 2))
        body = out['installed-content.bin'][16:]
        self.assertEqual([body[i * 308 + 264:(i + 1) * 308].rstrip(b'\0') for i in range(2)], [b'AB00', b'AB01'])
        self.assertEqual([body[i * 308 + 8:i * 308 + 264] for i in range(2)], [display('first'), display('second')])
        first = metadata.parse_catalog(out['installed-package-1.bin'])
        self.assertEqual({p: v[5] for p, v in first.items()}, {'content:\\': 0, 'content:\\part0.rpf': 9, 'content:\\part0.timestamp': 14})
        self.assertEqual(list(first['content:\\part0.rpf'][:4]), stamps(3, 5000)[1])
        second = metadata.parse_catalog(out['installed-package-2.bin'])
        self.assertEqual(second['content:\\part1.rpf'][5], 5000)
        self.assertEqual(list(second['content:\\'][:4]), stamps(3, 5100)[0])

    def test_extra_files_of_the_table_are_listed_on_the_play_disc(self):
        out = m.build([('only.bin', 4, False)], self.PACKAGES, self.table(extras=[('ours.image', 31)]), 1)
        records = metadata.parse_catalog(out['file-metadata.bin'])
        self.assertEqual({p: v[5] for p, v in records.items()}, {'game:\\': 0, 'game:\\only.bin': 4, 'game:\\ours.image': 31})

    def test_another_number_of_packages_than_the_table_describes_is_refused(self):
        with self.assertRaises(ValueError):
            m.build([('only.bin', 4, False)], self.PACKAGES, self.table(packages=1), 1)
        with self.assertRaises(ValueError):
            m.build([('only.bin', 4, False)], self.PACKAGES[:1], self.table(packages=2), 1)

    def test_content_records_need_name_order_and_whole_display_names(self):
        with self.assertRaises(ValueError):
            m.content_records(1, [('B', display('b')), ('A', display('a'))])
        with self.assertRaises(ValueError):
            m.content_records(1, [('A', b'short')])
        self.assertEqual(len(m.content_records(1, [('A', display('a')), ('B', display('b'))])), 16 + 2 * 308)


class WholeStage(unittest.TestCase):
    """The command itself: a synthetic play disc, an extraction folder holding one real (synthetic) package."""
    SCRATCH = ROOT / 'local-analysis/catalog-stage-tests'

    def run_stage(self, expected=None, times_rows=2):
        import subprocess
        from stfs_fixture import fixture as stfs_package, TITLE
        self.SCRATCH.mkdir(parents=True, exist_ok=True)
        tmp = tempfile.TemporaryDirectory(dir=self.SCRATCH)
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        make_xiso(root / 'play.iso', {'game.bin': bytes(10)})
        (root / 'extracted/packages').mkdir(parents=True)
        stfs_package(root / 'extracted/packages/package-000')
        size = (root / 'extracted/packages/package-000').stat().st_size
        (root / 'extracted/inventory.json').write_text(json.dumps({'packages': [
            {'origin': 'install:/content/0000/ab00', 'file': 'packages/package-000', 'bytes': size}]}))
        table = {'schema': 'catalog-times/1', 'play': {'extras': [{'name': 'ours.image', 'bytes': 77}], 'times': stamps(5)},
                 'packages': [{'times': stamps(times_rows, 9000)}]}
        if expected is not None:
            table['expected'] = expected
        (root / 'times.json').write_text(json.dumps(table))
        done = subprocess.run([sys.executable, '-B', str(TARGET), '--play', str(root / 'play.iso'), '--inventory', str(root / 'extracted'),
                               '--times', str(root / 'times.json'), '--title-id', hex(TITLE), '--output', str(root / 'out')],
                              capture_output=True, text=True)
        return root, done

    def test_catalogs_come_from_the_disc_and_the_extracted_package(self):
        root, done = self.run_stage()
        self.assertEqual(done.returncode, 0, done.stderr)
        play = metadata.parse_catalog((root / 'out/file-metadata.bin').read_bytes())
        self.assertEqual({p: v[5] for p, v in play.items()}, {'game:\\': 0, 'game:\\game.bin': 10, 'game:\\ours.image': 77,
                                                              'game:\\sub': 0, 'game:\\sub\\inner.bin': play['game:\\sub\\inner.bin'][5]})
        package = metadata.parse_catalog((root / 'out/installed-package-1.bin').read_bytes())
        self.assertEqual(sorted(package), ['content:\\', 'content:\\test.bin'])
        self.assertEqual(package['content:\\test.bin'][5], 2 * 4096 - 3)
        content = (root / 'out/installed-content.bin').read_bytes()
        self.assertEqual(content[16 + 264:16 + 308].rstrip(b'\0'), b'AB00')
        self.assertEqual(content[16 + 8:16 + 16], 'Test'.encode('utf-16-be'))
        report = json.loads((root / 'out/report.json').read_text())
        self.assertEqual((report['status'], sorted(report['outputs'])), ('complete', ['file-metadata.bin', 'installed-content.bin', 'installed-package-1.bin']))

    def test_the_command_fails_when_an_expected_hash_is_not_met(self):
        root, done = self.run_stage(expected={'installed-content.bin': '0' * 64})
        self.assertEqual(done.returncode, 1)
        self.assertEqual(json.loads((root / 'out/report.json').read_text())['status'], 'differs')

    def test_the_command_refuses_a_table_that_does_not_fit_and_writes_nothing(self):
        root, done = self.run_stage(times_rows=3)
        self.assertEqual(done.returncode, 2)
        self.assertIn('CATALOG_STAGE_REFUSED', done.stderr)
        self.assertFalse((root / 'out').exists())


class TableAndOutput(unittest.TestCase):
    def folder(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Path(tmp.name)

    def test_tables_of_another_shape_are_refused(self):
        good = {'schema': 'catalog-times/1', 'play': {'times': []}, 'packages': [{'times': []}]}
        root = self.folder()
        cases = [dict(good, schema='catalog-times/2'), dict(good, play=[]), dict(good, packages=[]), dict(good, packages={}),
                 dict(good, play={'times': [], 'extras': [{'name': 'a/b', 'bytes': 1}]}),
                 dict(good, play={'times': [], 'extras': [{'name': 'a', 'bytes': '1'}]}), []]
        (root / 'good.json').write_text(json.dumps(good))
        self.assertEqual(m.load_times(root / 'good.json')['schema'], 'catalog-times/1')
        for i, bad in enumerate(cases):
            (root / f'{i}.json').write_text(json.dumps(bad))
            with self.assertRaises(ValueError, msg=json.dumps(bad)):
                m.load_times(root / f'{i}.json')

    def test_outputs_are_written_with_their_hashes(self):
        out = self.folder() / 'catalogs'
        report = m.write_outputs(out, {'a.bin': b'one', 'b.bin': b'two'}, {'a.bin': m.sha(b'one')}, 'f' * 64)
        self.assertEqual((report['status'], (out / 'b.bin').read_bytes()), ('complete', b'two'))
        self.assertEqual(json.loads((out / 'report.json').read_text())['outputs']['a.bin'], {'bytes': 3, 'sha256': m.sha(b'one')})

    def test_an_unexpected_hash_or_a_missing_catalog_is_reported(self):
        report = m.write_outputs(self.folder() / 'c', {'a.bin': b'one'}, {'a.bin': '0' * 64}, '')
        self.assertEqual((report['status'], report['outputs']['a.bin']['expected_sha256']), ('differs', '0' * 64))
        report = m.write_outputs(self.folder() / 'c', {'a.bin': b'one'}, {'a.bin': m.sha(b'one'), 'gone.bin': '1' * 64}, '')
        self.assertEqual((report['status'], report['missing']), ('differs', ['gone.bin']))

    def test_an_existing_output_folder_is_never_written_into(self):
        out = self.folder()
        (out / 'earlier.txt').write_text('evidence')
        with self.assertRaises(FileExistsError):
            m.write_outputs(out, {'a.bin': b'one'}, {}, '')
        self.assertEqual(sorted(p.name for p in out.iterdir()), ['earlier.txt'])


if __name__ == '__main__':
    unittest.main()
