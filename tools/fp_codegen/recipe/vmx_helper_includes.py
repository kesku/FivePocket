#!/usr/bin/env python3
"""vmx_helper_includes.py IN.cpp OUT.cpp: include the incremental VMX helper headers a chunk calls (design gap G4).

The validated corpus was generated with a context header that lacks the vector pack/unpack helpers which the full
XenonRecomp patch queue (0015-0019) embeds in ppc_context.h; its repaired chunks instead include the incremental headers
from runtime/host. Rule (derived from the text, no parameters): a chunk that calls
  recomp::vmx::packNormShort4 / unpackNormShort4   includes "guest_vmx_short.h",
  recomp::vmx::packNormPacked32 / unpackNormPacked32  "guest_vmx_packed.h",
  recomp::vmx::packHalf / unpackHalf                "guest_vmx_half.h",
each on its own line right after the first line (`#include "ppc_recomp_shared.h"`), in the order short, packed, half
(the order the corpus's successive in-place inserts left). A chunk that already includes any of them is refused (the pass
ran twice?). No game data.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402

FAMILIES = (('guest_vmx_short.h', ('packNormShort4', 'unpackNormShort4')),
            ('guest_vmx_packed.h', ('packNormPacked32', 'unpackNormPacked32')),
            ('guest_vmx_half.h', ('packHalf', 'unpackHalf')))
FIRST = '#include "ppc_recomp_shared.h"\n'


def apply(text: str) -> tuple[str, dict]:
    used = set(re.findall(r'\brecomp::vmx::(\w+)', text))
    incs = [f'#include "{h}"\n' for h, names in FAMILIES if used & set(names)]
    if not incs:
        return text, {'includes': 0}
    if any(f'#include "{h}"' in text for h, _ in FAMILIES):
        raise SystemExit('a VMX helper header is already included')
    if not text.startswith(FIRST):
        raise SystemExit('the chunk does not start with the shared generator include')
    return FIRST + ''.join(incs) + text[len(FIRST):], {'includes': len(incs)}


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
