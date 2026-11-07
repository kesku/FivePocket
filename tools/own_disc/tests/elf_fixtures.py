# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored ELF and ar containers for decoder identity tests."""
import struct

PROGBITS, RELA, NOBITS, STRTAB = 1, 4, 8, 3
ALLOC, EXEC = 2, 4

def elf(sections, extended=False, cut=0) -> bytes:
    """An ELF64 little-endian relocatable file with these (name, type, flags, content-or-size) sections."""
    names = b'\0'
    offsets = {}
    for name, *_ in list(sections) + [('.shstrtab',)]:
        offsets[name] = len(names)
        names += name.encode() + b'\0'
    body = b''
    heads = [struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for name, kind, flags, content in list(sections) + [('.shstrtab', STRTAB, 0, names)]:
        size = content if kind == NOBITS else len(content)
        heads.append(struct.pack('<IIQQQQIIQQ', offsets[name], kind, flags, 0, 64 + len(body), size, 0, 0, 1, 0))
        if kind != NOBITS:
            body += content
    count, strings = len(heads), len(heads) - 1
    if extended:  # the form for more than 65,279 sections: the counts live in section 0
        heads[0] = struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, count, strings, 0, 0, 0)
    header = b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HHIQQQIHHHHHH', 1, 183, 1, 0, 0, 64 + len(body), 0, 64, 0, 0, 64,
                                                             0 if extended else count, 0xFFFF if extended else strings)
    data = header + body + b''.join(heads)
    return data[:len(data) - cut] if cut else data


def ar(members, symbol_table=b'SYMBOLS') -> bytes:
    """A GNU-format `ar` archive: the symbol table, the table of long names when one is needed, the members."""
    def record(name: bytes, body: bytes) -> bytes:
        head = name.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6) + b'644'.ljust(8) + str(len(body)).encode().ljust(10) + b'`\n'
        return head + body + (b'\n' if len(body) & 1 else b'')
    table, fields = b'', []
    for name, _ in members:
        if len(name) > 15:
            fields.append(b'/' + str(len(table)).encode())
            table += name.encode() + b'/\n'
        else:
            fields.append(name.encode() + b'/')
    out = b'!<arch>\n'
    if symbol_table is not None:
        out += record(b'/', symbol_table)
    if table:
        out += record(b'//', table)
    return out + b''.join(record(field, body) for field, (_, body) in zip(fields, members))
