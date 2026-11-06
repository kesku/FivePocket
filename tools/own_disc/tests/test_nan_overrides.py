# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic shader text only; no game input or cached shader output."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build_nan_overrides as overrides

HEADER = '%glsl = OpExtInstImport "GLSL.std.450"\n%float = OpTypeFloat 32\n'


class NaNOverrides(unittest.TestCase):
    def test_clamped_scalar_rsqrt_uses_finite_input(self):
        text = HEADER + '%inv = OpExtInst %float %glsl InverseSqrt %zero\n%out = OpExtInst %float %glsl FClamp %inv %lo %hi\n'
        changed, count, skipped = overrides.patch(text)
        self.assertEqual((count, skipped), (1, 0))
        self.assertIn('%nanfix_1 = OpExtInst %float %glsl FMax %zero %nanfix_fltmin', changed)
        self.assertIn('%inv = OpExtInst %float %glsl InverseSqrt %nanfix_1', changed)
        self.assertIn('%out = OpExtInst %float %glsl FClamp %inv %lo %hi', changed)
        self.assertNotIn('InverseSqrt %zero', changed)

    def test_unclamped_scalar_and_vector_paths_keep_their_operands(self):
        text = HEADER + '%free = OpExtInst %float %glsl InverseSqrt %a\n%vec = OpExtInst %float4 %glsl InverseSqrt %b\n%out = OpExtInst %float4 %glsl FClamp %vec %lo %hi\n'
        changed, count, skipped = overrides.patch(text)
        self.assertEqual((count, skipped), (0, 1))
        for line in text.splitlines():
            self.assertIn(line, changed.splitlines())
        self.assertNotIn('%nanfix_1 =', changed)


if __name__ == '__main__':
    unittest.main()
