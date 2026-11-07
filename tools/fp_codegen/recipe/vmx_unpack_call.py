#!/usr/bin/env python3
"""vmx_unpack_call.py IN.cpp OUT.cpp: NORMPACKED32 unpack as the corpus's helper call (design gap G4, no parameters).

Patch 0017 makes XenonRecomp translate `vupkd3d128 vD,vS,type 2` inline (8 lines through temp/vTemp). The validated
corpus, generated before that patch, carries the in-place repair
    recomp::vmx::unpackNormPacked32(ctx.vD.u32, ctx.vS.u32);
(runtime/host/guest_vmx_packed.h; same lanes and NaN encoding, proven by the patch's own fixtures). This pass
replaces every exact 0017 block by that call and then applies the generator's declaration rule to the touched functions:
`\\tPPCVRegister vTemp{};` / `\\tPPCRegister temp{};` at the function start are removed when no other line of the
function uses vTemp / temp (the pre-0017 generator declared a local only for an instruction that used it). Every other
line is copied unchanged. Contains no game data.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402

V = r'ctx\.v(\d+)'
LANE = r'\tvTemp\.u32\[{lane}\] = temp\.u32 == 0x200U \? 0x7FC00000U : 0x40400000U \+ \(\(temp\.u32 \^ 0x200U\) - 0x200U\);\n'
BLOCK = [re.compile(r'\ttemp\.u32 = \(' + V + r'\.u32\[0\] >> 0\) & 0x3FFU;\n'), re.compile(LANE.format(lane=3)),
         re.compile(r'\ttemp\.u32 = \(' + V + r'\.u32\[0\] >> 10\) & 0x3FFU;\n'), re.compile(LANE.format(lane=2)),
         re.compile(r'\ttemp\.u32 = \(' + V + r'\.u32\[0\] >> 20\) & 0x3FFU;\n'), re.compile(LANE.format(lane=1)),
         re.compile(r'\tvTemp\.u32\[0\] = 0x3F800000U \+ \(' + V + r'\.u32\[0\] >> 30\);\n'),
         re.compile(r'\t' + V + r' = vTemp;\n')]
DECL = {'vTemp': '\tPPCVRegister vTemp{};\n', 'temp': '\tPPCRegister temp{};\n'}


def match_block(lines: list[str], k: int) -> tuple[int, int] | None:
    """(vD, vS) when lines[k:k+8] is exactly patch 0017's NORMPACKED32 unpack"""
    if k + len(BLOCK) > len(lines):
        return None
    ms = [rx.fullmatch(lines[k + i]) for i, rx in enumerate(BLOCK)]
    if not all(ms):
        return None
    src = {ms[i].group(1) for i in (0, 2, 4, 6)}
    if len(src) != 1:
        return None
    return int(ms[7].group(1)), int(src.pop())


def apply(text: str) -> tuple[str, dict]:
    lines = text.splitlines(keepends=True)
    out_parts, stats = [], {'calls': 0, 'decl_removed': 0}
    last = 0
    for i, j, _name in textpass.functions(lines):
        body, k, changed = [], i, False
        while k < j:
            m = match_block(lines, k)
            if m:
                body.append(f'\trecomp::vmx::unpackNormPacked32(ctx.v{m[0]}.u32, ctx.v{m[1]}.u32);\n')
                k += len(BLOCK)
                stats['calls'] += 1
                changed = True
                continue
            body.append(lines[k])
            k += 1
        if changed:
            for name, decl in DECL.items():
                rx = re.compile(rf'\b{name}\b')
                users = [x for x in body if rx.search(x) and x != decl]
                if not users and decl in body[:12]:
                    body.remove(decl)
                    stats['decl_removed'] += 1
        out_parts += lines[last:i] + body
        last = j
    out_parts += lines[last:]
    return ''.join(out_parts), stats


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    text, stats = apply(textpass.read_text(Path(sys.argv[1])))
    textpass.write_new(Path(sys.argv[2]), text)
    print(f'{Path(sys.argv[1]).name}: {stats}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
