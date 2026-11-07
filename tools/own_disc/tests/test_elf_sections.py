# SPDX-License-Identifier: GPL-3.0-or-later
"""ELF and archive identity checks used by the XMA decoder build."""
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

TOOLS = str(Path(__file__).resolve().parents[1])
sys.path.insert(0, TOOLS)
import elf_sections
from elf_fixtures import *

BASE = [('.text', PROGBITS, ALLOC | EXEC, b'\x1f\x20\x03\xd5' * 4), ('.rela.text', RELA, 0, b'R' * 24),
        ('.comment', PROGBITS, 0, b'\0compiler on host A\0'), ('.debug_info', PROGBITS, 0, b'/home/a/x.cpp'),
        ('.rela.debug_info', RELA, 0, b'D' * 24), ('.bss', NOBITS, ALLOC | 1, 4096)]

HOST_B = [(n, k, f, b'\0compiler on host B, named at more length\0' if n == '.comment' else c) for n, k, f, c in BASE]

OTHER = [('.text', PROGBITS, ALLOC | EXEC, b'\x00\x00\x80\xd2' * 4)] + BASE[1:]


def changed(name, content=None, flags=None, rename=None):
    out = []
    for n, kind, f, c in BASE:
        if n == name:
            out.append((rename or n, kind, f if flags is None else flags, c if content is None else content))
        else:
            out.append((n, kind, f, c))
    return out


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def file(self, name, data: bytes) -> str:
        path = os.path.join(self.tmp.name, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        return path

    def identity(self, name, sections, **kw):
        return elf_sections.code_identity(self.file(name, elf(sections, **kw)))


class CodeIdentity(Files):
    def test_the_compilers_own_name_is_not_part_of_the_identity_but_changes_the_file(self):
        a = self.file('a.o', elf(BASE))
        b = self.file('b.o', elf(changed('.comment', b'\0compiler on host B, longer\0')))
        self.assertNotEqual(elf_sections.file_sha256(a), elf_sections.file_sha256(b))
        self.assertEqual(elf_sections.code_identity(a)[0], elf_sections.code_identity(b)[0])

    def test_debug_sections_and_their_relocations_are_left_out_and_named(self):
        a, left = self.identity('a.o', BASE)
        b, _ = self.identity('b.o', changed('.debug_info', b'D:/recomp/x.cpp'))
        c, _ = self.identity('c.o', changed('.rela.debug_info', b'E' * 48))
        self.assertEqual(a, b)
        self.assertEqual(a, c)
        self.assertEqual(left, ['.comment', '.debug_info', '.rela.debug_info'])

    def test_one_changed_instruction_changes_the_identity(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.text', b'\x1f\x20\x03\xd5' * 3 + b'\xc0\x03\x5f\xd6'))[0])

    def test_relocations_of_code_count_as_code(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.rela.text', b'R' * 23 + b'S'))[0])

    def test_only_the_exact_names_are_left_out(self):
        for name in ('.comment.keep', '.debugger_note'):
            extra = BASE + [(name, PROGBITS, 0, b'one')]
            other = BASE + [(name, PROGBITS, 0, b'two')]
            self.assertNotEqual(self.identity('a.o', extra)[0], self.identity('b.o', other)[0], name)
            self.assertNotIn(name, self.identity('a.o', extra)[1])

    def test_section_flags_are_part_of_the_identity(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.text', flags=ALLOC))[0])

    def test_a_section_without_file_bytes_counts_by_its_size(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.bss', 8192))[0])
        self.assertEqual([s['size'] for s in elf_sections.sections(self.file('c.o', elf(BASE))) if s['name'] == '.bss'], [4096])

    def test_more_sections_than_the_header_can_count(self):
        plain = elf_sections.sections(self.file('a.o', elf(BASE)))
        extended = elf_sections.sections(self.file('b.o', elf(BASE, extended=True)))
        self.assertEqual([(s['name'], s['sha256']) for s in plain[1:]], [(s['name'], s['sha256']) for s in extended[1:]])
        self.assertEqual(len(extended), len(BASE) + 2)

    def test_a_cut_off_file_is_refused(self):
        whole = elf(BASE)
        with self.assertRaises(ValueError):      # the section table itself is incomplete
            elf_sections.sections(self.file('a.o', whole[:-40]))
        table, = struct.unpack_from('<Q', whole, 0x28)
        longer = bytearray(whole)                # the first section claims bytes beyond the end of the file
        struct.pack_into('<Q', longer, table + 64 + 32, len(whole))
        with self.assertRaises(ValueError):
            elf_sections.sections(self.file('b.o', bytes(longer)))

    def test_a_file_that_is_not_elf_is_refused(self):
        with self.assertRaises(ValueError):
            elf_sections.sections(self.file('a.o', b'MZ' + b'\0' * 200))

    def test_diff_names_what_differs_and_what_is_missing(self):
        a = self.file('a.o', elf(BASE))
        b = self.file('b.o', elf([s for s in changed('.text', b'\0' * 16) if s[0] != '.debug_info'] + [('.note', PROGBITS, 0, b'n')]))
        self.assertEqual(elf_sections.diff(a, b), ['.text', '.debug_info', '.shstrtab', '.note'])
        self.assertEqual(elf_sections.diff(a, a), [])


class ArchiveIdentity(Files):
    MEMBERS = [('short.o', BASE), ('a_name_longer_than_the_field.o', OTHER), ('odd.o', BASE[:1] + [('.data', PROGBITS, ALLOC | 1, b'odd')] + BASE[2:3])]

    def library(self, name, members, **kw):
        return self.file(name, ar([(n, elf(s)) for n, s in members], **kw))

    def test_the_same_objects_with_another_compiler_name_are_another_file_and_the_same_library(self):
        a = self.library('a.a', self.MEMBERS)
        b = self.library('b.a', [(n, [(x, k, f, b'\0compiler on host B, named at more length\0' if x == '.comment' else c) for x, k, f, c in s]) for n, s in self.MEMBERS])
        self.assertNotEqual(elf_sections.file_sha256(a), elf_sections.file_sha256(b))
        self.assertNotEqual(os.path.getsize(a), os.path.getsize(b))
        self.assertEqual(elf_sections.archive_identity(a), elf_sections.archive_identity(b))
        self.assertEqual(elf_sections.archive_identity(a)[1], 3)

    def test_members_are_read_by_name_in_order_and_the_archives_own_records_are_no_members(self):
        path = self.library('a.a', self.MEMBERS)
        self.assertEqual([(n, len(b)) for n, b in elf_sections.archive_members(path)],
                         [(n, len(elf(s))) for n, s in self.MEMBERS])                     # the long name through the name table, the odd length padded
        self.assertEqual(elf_sections.archive_members(path)[2][1], elf(self.MEMBERS[2][1]))
        wide = self.file('w.a', ar([(n, elf(s)) for n, s in self.MEMBERS]).replace(b'/               0', b'/SYM64/         0', 1))
        self.assertEqual(elf_sections.archive_identity(wide), elf_sections.archive_identity(path))
        bare = self.library('n.a', self.MEMBERS, symbol_table=None)
        self.assertEqual(elf_sections.archive_identity(bare), elf_sections.archive_identity(path))

    def test_another_object_another_order_or_another_name_is_another_library(self):
        base = elf_sections.archive_identity(self.library('a.a', self.MEMBERS))[0]
        cases = {'a changed instruction': [self.MEMBERS[0], ('a_name_longer_than_the_field.o', BASE), self.MEMBERS[2]],
                 'another order': [self.MEMBERS[1], self.MEMBERS[0], self.MEMBERS[2]],
                 'another name': [('other.o', BASE)] + self.MEMBERS[1:],
                 'a member less': self.MEMBERS[:2]}
        for index, (what, members) in enumerate(cases.items()):
            self.assertNotEqual(elf_sections.archive_identity(self.library(f'{index}.a', members))[0], base, what)

    def test_what_is_no_archive_of_objects_is_refused(self):
        whole = ar([(n, elf(s)) for n, s in self.MEMBERS])
        cases = {'not an ar archive': b'PK' + whole[2:], 'truncated member': whole[:-40], 'damaged member header': whole.replace(b'`\n', b'`\r', 1),
                 'no members': ar([]), 'not an ELF64': ar([('text.o', b'no object at all')]),
                 'another archive format': ar([('short.o', elf(BASE))]).replace(b'short.o/ ', b'#1/20    ', 1),
                 'outside the name table': whole.replace(b'/0  ', b'/999', 1)}
        for index, (word, data) in enumerate(cases.items()):
            with self.subTest(case=word), self.assertRaisesRegex(ValueError, word):
                elf_sections.archive_identity(self.file(f'{index}.a', data))
        size_text = whole.replace(b'644     7 ', b'644     x ', 1)
        self.assertNotEqual(size_text, whole)
        with self.assertRaisesRegex(ValueError, 'damaged member header'):
            elf_sections.archive_identity(self.file('s.a', size_text))

    def test_the_command_line_prints_file_hash_identity_and_member_count(self):
        import subprocess
        path = self.library('a.a', self.MEMBERS)
        done = subprocess.run([sys.executable, '-B', os.path.join(TOOLS, 'elf_sections.py'), '--archive', path], capture_output=True, text=True)
        identity, count = elf_sections.archive_identity(path)
        self.assertEqual(done.stdout.split(), [elf_sections.file_sha256(path), identity, str(count), path])


if __name__ == "__main__":
    unittest.main()
