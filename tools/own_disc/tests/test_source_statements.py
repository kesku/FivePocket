# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import source_statements as statements


def fingerprint(line):
    return hashlib.sha256(statements.dense(line).encode()).hexdigest()


class SourceStatements(unittest.TestCase):
    def test_word_rotate_mask_is_derived_from_translated_operands(self):
        source='ctx.r4.u64 = __builtin_rotateleft64(ctx.r2.u32 | (ctx.r2.u64 << 32), 3) & 0xFFFFFFF8;'
        expected='ctx.r4.u64 = (ctx.r2.u32 << 3) & 0xFFFFFFF8U;'
        found=statements.candidates(source)
        self.assertEqual(found[fingerprint(expected)],statements.dense(expected))
        unsafe=source.replace('0xFFFFFFF8','0xFFFFFFFF')
        self.assertNotIn(fingerprint(expected),statements.candidates(unsafe))

    def test_forward_guard_retains_its_condition(self):
        source='if (ctx.cr1.eq) goto loc_100;\nPPC_STORE_U32(ctx.r4.u32 + 0, ctx.r5.u32);\nloc_100:\n'
        expected='if (!ctx.cr1.eq) PPC_STORE_U32(ctx.r4.u32, ctx.r5.u32);'
        self.assertIn(fingerprint(expected),statements.candidates(source))

    def test_unproven_statement_is_refused(self):
        records={'chunks':{'0':{'bodies':{'100':{'lines':[{'generated_line':{'sha256':'0'*64,'length':10,'layout':[]}}]}}}}}
        with self.assertRaisesRegex(ValueError,'cannot supply'):
            statements.resolve_records(records,'ctx.r3.u64 = 2;')

    def test_explicit_tag_policy_requires_the_matching_mask(self):
        repair={'runtime_repair':{'operation':'return_masked_address','input':2,'temporary':4,'output':3,'clear_low_bits':4}}
        body={'lines':['ctx.r4.u64 = ctx.r2.u32 & 0xFFFFFFF0U;',repair]}
        records={'chunks':{'0':{'bodies':{'100':body}}}}
        self.assertEqual(statements.resolve_records(records,''),1)
        self.assertEqual(body['lines'][1],'ctx.r3.u64 = ctx.r4.u64;')
        body['lines']=['ctx.r4.u64 = ctx.r2.u32 & 0xFFFFFFF8U;',repair]
        with self.assertRaisesRegex(ValueError,'verified mask'):
            statements.resolve_records(records,'')

    def test_committed_tail_records_contain_no_translated_statements(self):
        import json
        path=Path(statements.__file__).with_name('config')/'error-sites-published-1.json'
        records=json.loads(path.read_text())
        for chunk in records['chunks'].values():
            for body in chunk['bodies'].values():
                self.assertTrue(all(isinstance(item,dict) for item in body.get('lines',[])))


if __name__=='__main__':
    unittest.main()
