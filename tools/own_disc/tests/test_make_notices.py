#!/usr/bin/env python3
"""make_notices.py: the notices of a package are written from pinned terms files and nothing else; what is written,
what refuses the run. Synthetic trees, no game data. MAKE_NOTICES names another copy of the tool (the mutant runner
beside this file)."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import re
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = Path(os.environ.get('MAKE_NOTICES', HERE.parent / 'make_notices.py'))
spec = importlib.util.spec_from_file_location('make_notices_under_test', TOOL)
mn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mn)
TABLE = HERE.parent / 'notices' / 'components.json'

MIT = b'Permission is granted.\r\nNo warranty.\n'
HEAD = b'/* notice line one\n   notice line two */\n#include <x>\nint f();\n'
LGPL = b'LESSER TERMS\nsecond line\n'


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.tree = self.dir / 'tree'
        (self.tree / 'sub').mkdir(parents=True)
        (self.tree / 'LICENSE').write_bytes(MIT)
        (self.tree / 'sub' / 'code.c').write_bytes(HEAD)
        self.tar = self.dir / 'src.tar'
        (self.dir / 'COPYING.LIB').write_bytes(LGPL)
        with tarfile.open(self.tar, 'w') as tar:
            tar.add(self.dir / 'COPYING.LIB', arcname='COPYING.LIB')
        self.parts = [
            {'id': 'alpha', 'name': 'Alpha', 'origin': 'https://example.invalid/alpha', 'terms': 'MIT', 'in_package': ['bin/alpha.exe'],
             'texts': [{'root': 'tree', 'path': 'LICENSE', 'sha256': digest(MIT)}]},
            {'id': 'beta', 'name': 'Beta', 'origin': 'somewhere', 'revision': 'r7', 'terms': 'notice in the file', 'in_package': ['bin/alpha.exe', 'lib/*'], 'note': 'A note.',
             'texts': [{'root': 'tree', 'path': 'sub/code.c', 'lines': [1, 2], 'sha256': digest(HEAD)}, {'root': 'archive', 'member': 'COPYING.LIB', 'sha256': digest(LGPL)}]},
            {'id': 'gamma', 'name': 'Gamma', 'origin': 'its publisher', 'terms': 'its own', 'in_package': ['python/*'], 'travels': 'python/LICENSE.txt'},
            {'id': 'delta', 'name': 'Delta', 'origin': 'its publisher', 'terms': 'its own', 'in_package': ['fetched/*'], 'fetched': 'the fetch step takes it'},
        ]
        self.roots = ['--root', f'tree={self.tree}', '--root', f'archive={self.tar}']

    def tearDown(self):
        self.tmp.cleanup()

    def table(self, parts=None):
        path = self.dir / 'table.json'
        path.write_text(json.dumps({'schema': 'builder-notices/1', 'components': self.parts if parts is None else parts}), encoding='utf-8')
        return path

    def run_tool(self, *args, parts=None, roots=None):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = mn.main(['--components', str(self.table(parts))] + (self.roots if roots is None else roots) + list(args))
        return code, out.getvalue(), err.getvalue()

    def write(self, *args, **kw):
        out = self.dir / f'out-{len(list(self.dir.glob("out-*")))}'
        code, stdout, stderr = self.run_tool('--out', str(out), *args, **kw)
        return code, out, stderr

    # ---- what is written
    def test_writes_notices_texts_and_record(self):
        code, out, stderr = self.write()
        self.assertEqual(code, 0, stderr)
        notices = (out / 'NOTICES.txt').read_text(encoding='utf-8')
        self.assertFalse(notices.startswith('INCOMPLETE'))
        for needle in ('  1. Alpha (MIT)', '  2. Beta (notice in the file)', 'From: somewhere at r7', 'In this package: bin/alpha.exe, lib/*', 'A note.',
                       'Its terms travel with it: python/LICENSE.txt', 'Not carried by this package: the fetch step takes it',
                       'Permission is granted.\nNo warranty.', '/* notice line one\n   notice line two */', 'LESSER TERMS\nsecond line'):
            self.assertIn(needle, notices)
        self.assertNotIn('#include', notices)                      # only the named lines of the source file
        self.assertNotIn('\r', notices)
        self.assertEqual((out / 'licences/alpha/1-LICENSE').read_bytes(), MIT)             # byte for byte, CR LF kept
        self.assertEqual((out / 'licences/beta/1-code.c.lines-1-2.txt').read_bytes(), b'/* notice line one\n   notice line two */\n')
        self.assertEqual((out / 'licences/beta/2-COPYING.LIB').read_bytes(), LGPL)
        record = json.loads((out / 'notices.json').read_text())
        self.assertTrue(record['complete'])
        self.assertEqual(record['notices_sha256'], digest((out / 'NOTICES.txt').read_bytes()))
        self.assertEqual([c['id'] for c in record['components']], ['alpha', 'beta', 'gamma', 'delta'])
        beta = record['components'][1]['texts']
        self.assertEqual(beta[0], {'file': 'licences/beta/1-code.c.lines-1-2.txt', 'sha256': digest(b'/* notice line one\n   notice line two */\n'), 'source_sha256': digest(HEAD)})
        self.assertEqual(beta[1]['sha256'], digest(LGPL))

    def test_same_input_same_output(self):
        _, first, _ = self.write()
        _, second, _ = self.write()
        for name in ('NOTICES.txt', 'notices.json'):
            self.assertEqual((first / name).read_bytes(), (second / name).read_bytes())

    def test_check_writes_nothing(self):
        before = sorted(p.name for p in self.dir.iterdir())
        code, stdout, stderr = self.run_tool('--check')
        self.assertEqual(code, 0, stderr)
        self.assertIn('4 parts, 3 terms texts', stdout)
        self.assertEqual(sorted(p.name for p in self.dir.iterdir() if p.name != 'table.json'), [n for n in before if n != 'table.json'])

    # ---- what refuses
    def refused(self, needle, *args, **kw):
        code, out, stderr = self.write(*args, **kw)
        self.assertEqual(code, 2, 'the run was not refused')
        self.assertIn(needle, stderr)
        self.assertFalse(out.exists(), 'something was written by a refused run')

    def test_another_terms_file_is_refused(self):
        (self.tree / 'LICENSE').write_bytes(MIT + b'One more clause.\n')
        self.refused('is not the pinned file')

    def test_another_archive_member_is_refused(self):
        self.parts[1]['texts'][1]['sha256'] = digest(b'other')
        self.refused('is not the pinned file')

    def test_missing_terms_file_is_refused(self):
        (self.tree / 'LICENSE').unlink()
        self.refused('is not there')

    def test_missing_archive_member_is_refused(self):
        self.parts[1]['texts'][1]['member'] = 'COPYING.OTHER'
        self.refused('has no member')

    def test_lines_outside_the_file_are_refused(self):
        self.parts[1]['texts'][0]['lines'] = [3, 99]
        self.refused('has no lines')
        self.parts[1]['texts'][0]['lines'] = [0, 2]
        self.refused('has no lines')
        self.parts[1]['texts'][0]['lines'] = [2, 1]
        self.refused('has no lines')

    def test_empty_extract_is_refused(self):
        blank = b'\n\n\ncode\n'
        (self.tree / 'sub' / 'code.c').write_bytes(blank)
        self.parts[1]['texts'][0].update(sha256=digest(blank), lines=[1, 2])
        self.refused('are empty')

    def test_unknown_root_is_refused(self):
        self.refused('no --root was given for "archive"', roots=['--root', f'tree={self.tree}'])

    def test_root_given_twice_is_refused(self):
        self.refused('given twice', roots=self.roots + ['--root', f'tree={self.dir}'])

    def test_two_parts_with_one_id_are_refused(self):
        self.parts[2]['id'] = 'alpha'
        self.refused('share the id')

    def test_part_without_text_or_reason_is_refused(self):
        del self.parts[2]['travels']
        self.refused('names no terms text')

    def test_part_without_a_place_is_refused(self):
        self.parts[0]['in_package'] = []
        self.refused('lacks "in_package"')

    def test_text_that_is_not_utf8_is_refused(self):
        bad = b'Copyright \xa9 somebody\n'
        (self.tree / 'LICENSE').write_bytes(bad)
        self.parts[0]['texts'][0]['sha256'] = digest(bad)
        self.refused('is not UTF-8')

    def test_existing_output_folder_is_refused(self):
        out = self.dir / 'taken'
        out.mkdir()
        code, _, stderr = self.run_tool('--out', str(out))
        self.assertEqual(code, 2)
        self.assertIn('exists already', stderr)
        self.assertEqual(list(out.iterdir()), [])

    def test_out_and_check_exclude_each_other(self):
        self.assertEqual(self.run_tool()[0], 2)
        self.assertEqual(self.run_tool('--check', '--out', str(self.dir / 'x'))[0], 2)
        self.assertFalse((self.dir / 'x').exists())

    def test_wrong_table_is_refused(self):
        path = self.dir / 'table.json'
        code = None
        with contextlib.redirect_stderr(io.StringIO()) as err:
            path.write_text(json.dumps({'schema': 'something-else/1', 'components': self.parts}))
            code = mn.main(['--components', str(path)] + self.roots + ['--check'])
        self.assertEqual(code, 2)
        self.assertIn('is not a builder-notices/1 table', err.getvalue())

    # ---- parts whose text is not collected yet
    def test_pending_part_stops_the_run(self):
        self.parts.append({'id': 'late', 'name': 'Late', 'origin': 'o', 'terms': 't', 'in_package': ['bin/late.exe'], 'pending': 'on another machine'})
        self.refused('still to collect for: late')

    def test_pending_part_allowed_marks_the_notices_incomplete(self):
        self.parts.append({'id': 'late', 'name': 'Late', 'origin': 'o', 'terms': 't', 'in_package': ['bin/late.exe'], 'pending': 'on another machine'})
        code, out, stderr = self.write('--allow-pending')
        self.assertEqual(code, 0, stderr)
        notices = (out / 'NOTICES.txt').read_text()
        self.assertTrue(notices.startswith('INCOMPLETE: the terms texts of these parts are still to be collected: late.'))
        self.assertIn('TEXT STILL TO BE COLLECTED: on another machine', notices)
        record = json.loads((out / 'notices.json').read_text())
        self.assertFalse(record['complete'])
        self.assertEqual(record['pending'], ['late'])

    # ---- every program of the package is covered
    def package_list(self, *names):
        path = self.dir / 'list.tsv'
        path.write_text(''.join(name + '\t12\tabcdef\treason\n' for name in names), encoding='utf-8')
        return path

    def test_covered_programs_pass(self):
        code, _, stderr = self.write('--package-list', str(self.package_list('bin/alpha.exe', 'lib/deep/one.dll', 'python\\Lib\\x.pyd', 'docs/readme.txt', 'tools/a.py')))
        self.assertEqual(code, 0, stderr)

    def test_uncovered_program_is_refused(self):
        self.refused('no part covers: bin/other.exe', '--package-list', str(self.package_list('bin/alpha.exe', 'bin/other.exe')))
        self.refused('no part covers: extra/tool.DLL', '--package-list', str(self.package_list('extra/tool.DLL')))

    def test_general_part_does_not_cover_a_program(self):
        self.parts.append({'id': 'runtime', 'name': 'Runtime', 'origin': 'o', 'terms': 't', 'in_package': ['bin/*.exe'], 'general': True, 'travels': 'bin/RUNTIME.txt'})
        self.refused('no part covers: bin/other.exe', '--package-list', str(self.package_list('bin/alpha.exe', 'bin/other.exe')))

    def test_uncovered_program_is_refused_by_check_too(self):
        code, _, stderr = self.run_tool('--check', '--package-list', str(self.package_list('bin/other.exe')))
        self.assertEqual(code, 2)
        self.assertIn('no part covers', stderr)


class TrackedTable(unittest.TestCase):
    """The table that is tracked beside the tool, against the script that builds the Windows host programs: every
    program that script puts into the package must lie at a place a part names."""


    def test_a_new_program_is_not_covered_by_the_table_as_it_is(self):
        # the runtimes linked into every program are marked general in the table, so they cannot stand in for a part
        parts = mn.load_table(TABLE)
        self.assertEqual(mn.uncovered_programs(parts, ['host-tools/a-new-tool.exe']), ['host-tools/a-new-tool.exe'])


if __name__ == '__main__':
    unittest.main()
