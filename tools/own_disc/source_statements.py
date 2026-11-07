# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve statement fingerprints from fresh bounded PPC translations."""
from __future__ import annotations

import hashlib
import re


def dense(text):
    return re.sub(r'\s+', '', text)


def negate(condition):
    return condition[1:] if condition.startswith('!') else '!' + condition


def variants(line):
    text=dense(line)
    found={text}
    simple=re.sub(r'\+0(?=[,)])','',text)
    simple=simple.replace('+-','-')
    found.add(simple)
    rotated=re.fullmatch(r'(ctx\.r\d+\.u64)=__builtin_rotateleft64\((ctx\.r\d+)\.u32\|\(\2\.u64<<32\),(\d+)\)&(0x[0-9A-Fa-f]+);',simple)
    if rotated:
        target,source,shift,mask=rotated.groups()
        shift=int(shift);mask=int(mask,16)&0xFFFFFFFF
        if shift==0:
            found.add(f'{target}={source}.u32&0x{mask:X}U;')
        elif shift<32 and not (mask&((1<<shift)-1)):
            found.add(f'{target}=({source}.u32<<{shift})&0x{mask:X}U;')
    inserted=re.fullmatch(r'(ctx\.r\d+\.u64)=\(__builtin_rotateleft32\((ctx\.r\d+\.u32),(\d+)\)&(0x[0-9A-Fa-f]+)\)\|\(\1&(0x[0-9A-Fa-f]+)\);',simple)
    if inserted:
        target,source,shift,mask,preserve=inserted.groups()
        if not (int(mask,16)&((1<<int(shift))-1)):
            found.add(f'{target}=({target}&{preserve}ULL)|(uint32_t({source}<<{shift})&{mask}U);')
    for item in tuple(found):
        found.add(re.sub(r'0x([0-9A-Fa-f]+)(?![\w])',lambda m:m[0]+('ULL' if len(m[1])>8 else 'U'),item))
    return found


def candidates(text):
    lines=[dense(line) for line in text.splitlines() if line.strip() and not line.strip().startswith('//')]
    output=set()
    for line in lines:
        output.update(variants(line))
    labels={line[:-1]:i for i,line in enumerate(lines) if line.startswith('loc_') and line.endswith(':')}
    for i,line in enumerate(lines):
        branch=re.fullmatch(r'if\((!?ctx\.cr\d+\.\w+)\)goto(loc_[0-9A-F]+);',line)
        if not branch or branch[2] not in labels:continue
        end=labels[branch[2]]
        if end<=i:continue
        middle=lines[i+1:end]
        # A forward conditional branch over ordinary statements becomes a conditional block.
        if middle and all(not x.startswith(('if(','loc_','PPC_FUNC','PPC_WEAK')) for x in middle):
            condition=negate(branch[1])
            output.add(f'if({condition}){{')
            if len(middle)==1:
                for statement in variants(middle[0]):output.add(f'if({condition})'+statement)
        # Two guards converge on one assignment: fold their shared fallthrough without copying guest text.
        nested=[(j,re.fullmatch(r'if\((!?ctx\.cr\d+\.\w+)\)goto(loc_[0-9A-F]+);',x)) for j,x in enumerate(middle)]
        for j,second in nested:
            if not second or second[1]!=branch[1] or second[2] not in labels:continue
            finish=labels[second[2]]
            assignment=lines[end+1:finish]
            if finish>end and len(assignment)==1:
                for statement in variants(assignment[0]):
                    output.add(f'if({branch[1]}){{{statement}}}else{{')
                    output.add(f'if({negate(second[1])})'+statement)
    return {hashlib.sha256(value.encode()).hexdigest():value for value in output}


def restore_layout(value, layout):
    positions={}
    for offset,spacing in layout:
        if not 0<=offset<=len(value) or spacing.strip():
            raise ValueError('invalid statement whitespace layout')
        positions[offset]=positions.get(offset,'')+spacing
    return ''.join(positions.get(index,'')+character for index,character in enumerate(value))+positions.get(len(value),'')


def resolve_records(records, translated):
    known=candidates(translated)
    resolved=0
    for chunk in records['chunks'].values():
        for body in chunk['bodies'].values():
            for index,item in enumerate(body.get('lines',[])):
                if isinstance(item,dict) and 'runtime_repair' in item:
                    repair=item['runtime_repair']
                    if repair.get('operation')!='return_masked_address' or index==0:
                        raise ValueError('unsupported authored runtime repair')
                    source,temporary,target,bits=(repair[key] for key in ('input','temporary','output','clear_low_bits'))
                    if any(type(r) is not int or not 0<=r<32 for r in (source,temporary,target)) or not 0<bits<32:
                        raise ValueError('invalid authored address-mask repair')
                    mask=(0xFFFFFFFF<<bits)&0xFFFFFFFF
                    expected=f'ctx.r{temporary}.u64=ctx.r{source}.u32&0x{mask:X}U;'
                    if not isinstance(body['lines'][index-1],str) or dense(body['lines'][index-1])!=expected:
                        raise ValueError('address-mask repair is not preceded by the verified mask operation')
                    body['lines'][index]=f'ctx.r{target}.u64 = ctx.r{temporary}.u64;'
                    resolved+=1
                    continue
                if not isinstance(item,dict) or 'generated_line' not in item:continue
                spec=item['generated_line'];value=known.get(spec['sha256'])
                if value is None or len(value)!=spec['length']:
                    raise ValueError('fresh translation cannot supply statement '+spec['sha256'])
                body['lines'][index]=restore_layout(value,spec['layout'])
                resolved+=1
    return resolved
