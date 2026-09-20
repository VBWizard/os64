#!/usr/bin/env python3
"""Remove U+2500..259F cmap entries from pinned DejaVu Sans Mono, for F3 tests.

Rebuild the SFNT directory/checksums with a BMP format-12 cmap. Glyph outlines
are unchanged; the font intentionally cannot resolve the line/block characters.
The source fixture's DejaVu license applies. No third-party Python dependencies.
"""
import struct
from pathlib import Path


def make_fixture(source, target):
    data = Path(source).read_bytes()
    u16 = lambda p: struct.unpack_from('>H', data, p)[0]
    u32 = lambda p: struct.unpack_from('>I', data, p)[0]
    tables = {}
    for i in range(u16(4)):
        p = 12 + i * 16
        tables[data[p:p+4]] = data[u32(p+8):u32(p+8)+u32(p+12)]
    cmap = tables[b'cmap']
    mapping = {}
    for i in range(struct.unpack_from('>H', cmap, 2)[0]):
        platform, encoding, offset = struct.unpack_from('>HHI', cmap, 4+8*i)
        if platform != 3 or encoding != 1 or struct.unpack_from('>H', cmap, offset)[0] != 4:
            continue
        count = struct.unpack_from('>H', cmap, offset+6)[0] // 2
        word = lambda p: struct.unpack_from('>H', cmap, p)[0]
        for j in range(count):
            end = word(offset+14+j*2)
            start = word(offset+16+count*2+j*2)
            delta = word(offset+16+count*4+j*2)
            rp = offset+16+count*6+j*2
            ro = word(rp)
            for cp in range(start, min(end, 0xfffe)+1):
                glyph = word(rp+ro+2*(cp-start)) if ro else cp
                if not ro or glyph:
                    glyph = (glyph+delta) & 0xffff
                if glyph and not 0x2500 <= cp <= 0x259f:
                    mapping[cp] = glyph
        break
    assert all(cp in mapping for cp in range(32, 127))
    groups = b''.join(struct.pack('>III', cp, cp, gid) for cp, gid in sorted(mapping.items()))
    tables[b'cmap'] = struct.pack('>HHHHI', 0, 1, 3, 10, 12) + struct.pack('>HHIII', 12, 0, 16+len(groups), 0, len(mapping)) + groups
    head = bytearray(tables[b'head']); head[8:12] = b'\0'*4; tables[b'head'] = head
    n = len(tables); power = 1 << (n.bit_length()-1)
    result = bytearray(struct.pack('>IHHHH', 0x10000, n, power*16, power.bit_length()-1, n*16-power*16))
    result.extend(b'\0' * (n*16))
    checksum = lambda b: sum(struct.unpack('>'+'I'*((len(b)+3)//4), bytes(b)+b'\0'*((-len(b))%4))) & 0xffffffff
    for i, (tag, body) in enumerate(sorted(tables.items())):
        offset = len(result)
        struct.pack_into('>4sIII', result, 12+i*16, tag, checksum(body), offset, len(body))
        result.extend(body); result.extend(b'\0'*((-len(result))%4))
        if tag == b'head': head_offset = offset
    struct.pack_into('>I', result, head_offset+8, (0xb1b0afba-checksum(result)) & 0xffffffff)
    assert checksum(result) == 0xb1b0afba
    Path(target).write_bytes(result)

if __name__ == '__main__':
    import sys
    make_fixture(*sys.argv[1:])
