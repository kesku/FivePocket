# SPDX-License-Identifier: GPL-3.0-or-later
"""An authored single-file PIRS package used by the Duo catalog stages."""
import hashlib
import struct
from pathlib import Path

TITLE = 0x12345678
BLOCK = 4096
END = 0xFFFFFF


def fixture(path: Path) -> None:
    payload = bytes((i * 17 + 3) % 256 for i in range(2 * BLOCK - 3))
    directory = bytearray(BLOCK)
    directory[:8] = b'test.bin'
    directory[40] = 8
    directory[41:44] = (2).to_bytes(3, 'little')
    directory[44:47] = (2).to_bytes(3, 'little')
    directory[47:50] = (1).to_bytes(3, 'little')
    directory[50:52] = (0xFFFF).to_bytes(2, 'big')
    directory[52:56] = len(payload).to_bytes(4, 'big')
    blocks = [bytes(directory), payload[:BLOCK], payload[BLOCK:].ljust(BLOCK, b'\0')]
    table = bytearray(BLOCK)
    for index, block in enumerate(blocks):
        table[index * 24:index * 24 + 20] = hashlib.sha1(block).digest()
        struct.pack_into('>I', table, index * 24 + 20, 0x80000000 | (2 if index == 1 else END))
    header = bytearray(0xA000)
    header[:4] = b'PIRS'
    struct.pack_into('>III', header, 0x340, 0x971A, 2, 1)
    struct.pack_into('>I', header, 0x360, TITLE)
    volume = bytearray(36)
    volume[:3] = b'\x24\0\x01'
    volume[3:5] = (1).to_bytes(2, 'little')
    volume[8:28] = hashlib.sha1(table).digest()
    volume[28:32] = (3).to_bytes(4, 'big')
    header[0x379:0x39D] = volume
    header[0x411:0x419] = 'Test'.encode('utf-16-be')
    path.write_bytes(header + table + b''.join(blocks))
