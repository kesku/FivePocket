# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic microcode only: exercise derived index math and refusal boundaries."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import vfi_derive as v


def program(instructions, fetches=()):
    controls = []
    clauses = (len(instructions) + 5) // 6
    prefix = (clauses + 1 + 1) // 2
    controls.append((12 << 44) | (1 << 41))
    for start in range(0, len(instructions), 6):
        count = min(6, len(instructions) - start)
        opcode = 2 if start + count == len(instructions) else 1
        sequence = sum(1 << (2 * (at - start)) for at in fetches if start <= at < start + count)
        controls.append((opcode << 44) | (1 << 41) | ((prefix + start)) | (count << 12) | (sequence << 16))
    if len(controls) % 2:
        controls.append(0)
    words = []
    for a, b in zip(controls[::2], controls[1::2]):
        words += [a & 0xffffffff, (a >> 32) | ((b & 65535) << 16), b >> 16]
    for instruction in instructions:
        words.extend(instruction)
    return struct.pack('>' + 'I' * len(words), *words), prefix


def alu(op=2, dst=1, mask=1, src=(0, 0, 0), temporary=(True, True, True),
        swizzle=(0, 0, 0), negate=(False, False, False), export=False):
    a = (50 << 26) | dst | (mask << 16) | (int(export) << 15)
    b = sum(swizzle[i] << (16 - 8 * i) for i in range(3)) | sum(int(negate[i]) << (26 - i) for i in range(3))
    c = (op << 24) | sum(src[i] << (16 - 8 * i) for i in range(3)) | sum(int(temporary[i]) << (31 - i) for i in range(3))
    return a, b, c


def fetch(src=1, dst=2, component=0, mini=False, predicated=False, rounded=False):
    return ((1 << 19) | (src << 5) | (dst << 12) | (component << 30),
            0x688 | (38 << 16) | (1 << 12) | (1 << 13) | (int(mini) << 30)
            | (int(predicated) << 31) | (int(rounded) << 15), 4)


def output(src=2):
    return alu(dst=62, mask=15, src=(src, src, src), export=True)


def scale_program():
    return program([alu(op=1, src=(0, 250, 0), temporary=(True, False, True)), fetch(), output()], [1])


def container(code, address):
    virtual = 256
    constants = struct.pack('>4f', .25, 4, 0, 1)
    data = bytearray(virtual) + code + constants
    struct.pack_into('>9I', data, 0, 0x102a1101, virtual, len(code) + len(constants), 0, 36, 160, 80, 0, 0)
    struct.pack_into('>I', data, 36, 28)
    struct.pack_into('>9I', data, 80, 0, len(code), 0, 0, 0, 0, 0, 1, 0)
    struct.pack_into('>I', data, 116, address)
    struct.pack_into('>I', data, 176, 20)
    struct.pack_into('>HHI', data, 180, 250, 4, len(code))
    return bytes(data)


class Derivation(unittest.TestCase):
    def test_scale_from_initial_vertex_index_and_container_literal(self):
        code, prefix = scale_program()
        blob = container(code, prefix + 1)
        const = v.float_definitions(blob)
        result = v.derive(code, const, {prefix + 1: 0})
        rule = result['fetches'][0]
        self.assertEqual((rule['form'], rule['k'], rule['location']), ('Scale', v.float_bits(.25), 0))
        self.assertEqual(result['literals'], [(250, 0, v.float_bits(.25))])

    def test_plain_indices_require_no_repair(self):
        code, prefix = program([fetch(src=0), output()], [0])
        self.assertIsNone(v.derive(code, {}, {prefix: 0}))

    def test_modulo_keeps_the_microcode_operation_order(self):
        instructions = [alu(op=1, src=(0, 250, 0), temporary=(True, False, True)),
                        alu(op=9, src=(1, 1, 1)),
                        alu(op=11, src=(1, 251, 0), temporary=(True, False, True), negate=(True, False, False)),
                        fetch(), output()]
        code, prefix = program(instructions, [3])
        result = v.derive(code, {(250, 0): v.float_bits(1/36), (251, 0): v.float_bits(36)}, {prefix + 3: 0})
        r = result['fetches'][0]
        self.assertEqual((r['form'], r['k'], r['n']), ('Mod', v.float_bits(1/36), v.float_bits(36)))

    def test_fractional_modulo_and_remap(self):
        code, prefix = program([alu(op=1, src=(0, 250, 0), temporary=(True, False, True)),
                                alu(op=8, src=(1, 1, 1)),
                                alu(op=1, src=(1, 251, 0), temporary=(True, False, True)), fetch(), output()], [3])
        r = v.derive(code, {(250, 0): v.float_bits(.25), (251, 0): v.float_bits(4)}, {prefix + 3: 0})['fetches'][0]
        self.assertEqual((r['form'], r['k'], r['n']), ('Fmod', v.float_bits(.25), v.float_bits(4)))
        code, prefix = program([fetch(src=0, dst=1),
                                alu(op=1, dst=2, src=(1, 250, 0), temporary=(True, False, True), swizzle=(0xaa, 0, 0)),
                                fetch(src=2, dst=3), output(3)], [0, 2])
        rows = v.derive(code, {(250, 0): v.float_bits(255)}, {prefix: 0, prefix + 2: 1})['fetches']
        self.assertEqual((rows[1]['form'], rows[1]['via'], rows[1]['via_component'], rows[1]['k']),
                         ('Remap', 0, 2, v.float_bits(255)))

    def test_exported_vertex_id_requires_final_module_support(self):
        code, prefix = program([alu(op=1, src=(0, 250, 0), temporary=(True, False, True)), fetch(), output(0)], [1])
        with self.assertRaisesRegex(v.Unsupported, 'exported vertex-index'):
            v.derive(code, {(250, 0): v.float_bits(.25)}, {prefix + 1: 0})
        self.assertEqual(v.derive(code, {(250, 0): v.float_bits(.25)}, {prefix + 1: 0}, vertex_index=True)['fetches'][0]['form'], 'Scale')

    def test_unrecognized_rounded_and_predicated_indices_refuse(self):
        for instruction, message in [(fetch(rounded=True), 'rounded'), (fetch(predicated=True), 'predicated')]:
            code, prefix = program([alu(op=1, src=(0, 250, 0), temporary=(True, False, True)), instruction, output()], [1])
            with self.assertRaisesRegex(v.Unsupported, message):v.derive(code, {(250, 0): v.float_bits(.25)}, {prefix + 1: 0})
        code, prefix = program([alu(op=0, src=(0, 250, 0), temporary=(True, False, True)), fetch(), output()], [1])
        with self.assertRaisesRegex(v.Unsupported, 'index expression'):v.derive(code, {(250, 0): v.float_bits(.25)}, {prefix + 1: 0})

    def test_mini_fetch_inherits_full_index_before_destination_overwrite(self):
        code, prefix = program([alu(op=1, src=(0, 250, 0), temporary=(True, False, True)),
                                fetch(dst=1), fetch(src=63, dst=2, mini=True), output()], [1, 2])
        rows = v.derive(code, {(250, 0): v.float_bits(.25)}, {prefix + 1: 0, prefix + 2: 1})['fetches']
        self.assertEqual([r['form'] for r in rows], ['Scale', 'Scale'])
        self.assertEqual(rows[0]['k'], rows[1]['k'])

    def test_guarded_values_only_resolve_under_the_same_predicate(self):
        predicate = v.Expr('predicate', (8,))
        original = v.Expr('fetch', (0, 2))
        alternate = v.UNKNOWN
        expr = v.guarded(original, alternate, (predicate, False))
        self.assertEqual(v.restricted(expr, [(predicate, False)]), original)
        self.assertEqual(v.restricted(expr, [(predicate, True)]), alternate)
        with self.assertRaises(v.Unsupported):v.form(v.restricted(expr, [(v.Expr('predicate', (9,)), False)]))

    def test_nonnegative_index_comparison_folds_without_guessing_fetched_values(self):
        scaled = v.operation('mul', v.I, v.literal(v.float_bits(4)))
        self.assertEqual(v.value(v.operation('greater_equal', scaled, v.operation('negate', scaled))), 1)
        unknown = v.Expr('fetch', (0, 0))
        self.assertEqual(v.operation('greater_equal', unknown, unknown).op, 'greater_equal')

    def test_conflicting_conditional_index_is_not_fabricated(self):
        instructions = [alu(op=1, src=(0, 250, 0), temporary=(True, False, True)),
                        alu(op=1, src=(0, 251, 0), temporary=(True, False, True)), fetch(), output()]
        cf = [(12 << 44) | (1 << 41), (1 << 44) | 2 | (1 << 12),
              (3 << 44) | 3 | (1 << 12), (2 << 44) | 4 | (2 << 12) | (1 << 16)]
        words = []
        for a, b in zip(cf[::2], cf[1::2]):
            words.extend([a & 0xffffffff, (a >> 32) | ((b & 65535) << 16), b >> 16])
        for ins in instructions:words.extend(ins)
        code = struct.pack('>' + 'I' * len(words), *words)
        with self.assertRaisesRegex(v.Unsupported, 'index expression'):
            v.derive(code, {(250, 0): v.float_bits(.25), (251, 0): v.float_bits(.5)}, {4: 0})

    def test_predicated_fetch_recovers_the_value_for_its_own_branch(self):
        set_predicate = ((28 << 26), 0, (1 << 29) | 1)
        remap = alu(op=1, dst=2, src=(1, 250, 0), temporary=(True, False, True))
        overwrite = list(alu(dst=2, src=(250, 250, 0), temporary=(False, False, True), swizzle=(0x55, 0x55, 0)))
        overwrite[1] |= (1 << 28) | (1 << 27)
        code, prefix = program([fetch(src=0, dst=1), set_predicate, remap, tuple(overwrite),
                                fetch(src=2, dst=3, predicated=True), output(3)], [0, 4])
        rows = v.derive(code, {(250, 0): v.float_bits(255), (250, 1): v.float_bits(7)},
                        {prefix: 0, prefix + 4: 1}, allow_predicated=True)['fetches']
        self.assertEqual(rows[1]['form'], 'Remap')
        self.assertEqual(rows[1]['k'], v.float_bits(255))

    def test_coissued_scalar_reads_before_vector_store(self):
        copy = alu(src=(0, 0, 0))
        # Vector writes r1.x=I*.5 while scalar MUL_CONST1 reads old r1.x.
        both = list(alu(op=1, src=(0, 250, 251), temporary=(True, False, False), swizzle=(0, 0x55, 0)))
        both[0] = (both[0] & ((1 << 26) - 1)) | (43 << 26) | (2 << 8) | (1 << 20)
        code, prefix = program([copy, tuple(both), fetch(src=2), output()], [2])
        row = v.derive(code, {(250, 1): v.float_bits(.5), (251, 3): v.float_bits(.25)}, {prefix + 2: 0})['fetches'][0]
        self.assertEqual((row['form'], row['k']), ('Scale', v.float_bits(.25)))

    def test_range_proofs_include_float32_rounding_to_zero(self):
        x = v.operation('add', v.I, v.literal(1))
        half = v.operation('mul', x, v.literal(v.float_bits(.5)))
        self.assertEqual(v.interval(half)[0], 0)
        self.assertNotEqual(v.value(v.operation('greater', half, v.literal(0))), 1)

    def test_predicated_zero_export_cannot_erase_an_id_dependent_other_path(self):
        zero = ((50 << 26) | 62 | (1 << 15) | (1 << 14), (1 << 28) | (1 << 27), 2 << 24)
        code, prefix = program([alu(op=1, src=(0, 250, 0), temporary=(True, False, True)),
                                fetch(), output(0), zero], [1])
        with self.assertRaisesRegex(v.Unsupported, 'exported vertex-index'):
            v.derive(code, {(250, 0): v.float_bits(.25)}, {prefix + 1: 0})

    def test_cloth_metadata_alone_never_authorizes_predicated_fetches(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name);code, prefix = scale_program();blob = container(code, prefix + 1)
            (root/'shader.xenos').write_bytes(blob)
            canonical = v.canonical_microcode(code);key = v.fnv1a64(canonical)
            row = {'file': 'shader.xenos', 'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest(),
                   'stage': 'vertex', 'key': f'{key:016x}', 'canonical_sha256': hashlib.sha256(canonical).hexdigest(),
                   'origins': [{'origin': 'synthetic:/shader.fxc', 'offset': 0}]}
            (root/'inventory.json').write_text(json.dumps({'schema': 1, 'source': 'disc', 'key_files_used': False, 'containers': [row]}))
            (root/'generic_ptx_overrides.inc').write_text('')
            (root/'generic_cloth_overrides.inc').write_text(
                '{0x%016xull,1u,missing,0x0000000000000000ull,{{1u,23u},{2u,24u}}}' % key)
            # Even if a future module parser tolerates a metadata-only row, it cannot grant predication permission.
            with mock.patch('build_shader_fixes.table_modules', return_value={}):
                with self.assertRaisesRegex(ValueError, 'no matching override module'):
                    v.main(['--inventory', str(root), '--shader-fixes', str(root), '--out', str(root/'out.inc')])
            self.assertFalse((root/'out.inc').exists())

    def test_bad_definition_table_and_control_flow_are_bounded(self):
        code, prefix = scale_program()
        blob = container(code, prefix + 1)
        for offset, value in [(20, 255), (184, 0xffffffff), (180, 0xffffffff)]:
            bad = bytearray(blob);struct.pack_into('>I', bad, offset, value)
            with self.assertRaises(ValueError):v.float_definitions(bytes(bad))
        for code in [b'', b'bad', struct.pack('>3I', 0, 0, 0)]:
            with self.assertRaises(v.Unsupported):v.controls(code)

    def test_cli_checks_inventory_identity_and_writes_deterministic_output(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name);code, prefix = scale_program();blob = container(code, prefix + 1)
            (root/'shader.xenos').write_bytes(blob)
            canon = v.canonical_microcode(code);key = v.fnv1a64(canon)
            row = {'file': 'shader.xenos', 'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest(),
                   'stage': 'vertex', 'key': f'{key:016x}', 'canonical_sha256': hashlib.sha256(canon).hexdigest(),
                   'origins': [{'origin': 'synthetic:/shader.fxc', 'offset': 0}]}
            (root/'inventory.json').write_text(json.dumps({'schema': 1, 'source': 'disc', 'key_files_used': False, 'containers': [row]}))
            args = ['--inventory', str(root), '--out', str(root/'result.inc'), '--report', str(root/'report.json')]
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(v.main(args), 0)
                first = (root/'result.inc').read_bytes()
                self.assertEqual(v.main(args), 0)
                self.assertEqual(first, (root/'result.inc').read_bytes())
            self.assertIn('Form::Scale', first.decode())
            self.assertEqual(json.loads((root/'report.json').read_text())['rules'], 1)
            (root/'shader.xenos').write_bytes(blob + b'changed')
            with self.assertRaisesRegex(ValueError, 'identity'):v.main(args)


if __name__ == '__main__':
    unittest.main()
