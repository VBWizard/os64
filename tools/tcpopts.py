#!/usr/bin/env python3
"""tcpopts.py — what the SYN exchange said, read out of a pcap.

    tools/tcpopts.py net_capture.pcap [--all]

Prints every SYN and SYN-ACK in the capture with its window field and its
options decoded by name (MSS, WS=<shift>, SACKok, TS), because that is the
question window scaling turns on: did WE send the shift, and did THEY
answer with one. tshark would do it, and is not installed on every host
this project builds on; this is the thirty lines it takes with the standard
library. `--all` prints every TCP segment's window field too, which is how
to watch a scaled window walk past 65535 in the capture.

Reads the classic pcap format QEMU's filter-dump writes (both byte orders);
not pcapng.
"""

import struct
import sys


def options(blob):
    names = []
    o = 0
    while o < len(blob):
        kind = blob[o]
        if kind == 0:
            break
        if kind == 1:
            names.append("NOP")
            o += 1
            continue
        if o + 1 >= len(blob):
            names.append("truncated")
            break
        olen = blob[o + 1]
        if olen < 2 or o + olen > len(blob):
            names.append("malformed")
            break
        body = blob[o + 2:o + olen]
        if kind == 2 and olen == 4:
            names.append("MSS=%d" % struct.unpack(">H", body)[0])
        elif kind == 3 and olen == 3:
            names.append("WS=%d" % body[0])
        elif kind == 4:
            names.append("SACKok")
        elif kind == 5:
            names.append("SACK")
        elif kind == 8:
            names.append("TS")
        else:
            names.append("kind%d" % kind)
        o += olen
    return names


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    everything = "--all" in sys.argv
    with open(sys.argv[1], "rb") as f:
        hdr = f.read(24)
        magic = struct.unpack("<I", hdr[:4])[0]
        fmt = "<IIII" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">IIII"
        n = 0
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            _, _, incl, _ = struct.unpack(fmt, h)
            pkt = f.read(incl)
            n += 1
            if len(pkt) < 54 or pkt[12:14] != b"\x08\x00" or pkt[23] != 6:
                continue
            ihl = (pkt[14] & 0xF) * 4
            t = 14 + ihl
            flags = pkt[t + 13]
            syn = bool(flags & 0x02)
            if not syn and not everything:
                continue
            doff = (pkt[t + 12] >> 4) * 4
            win = struct.unpack(">H", pkt[t + 14:t + 16])[0]
            src = "%d.%d.%d.%d:%d" % (*pkt[26:30], struct.unpack(">H", pkt[t:t + 2])[0])
            dst = "%d.%d.%d.%d:%d" % (*pkt[30:34], struct.unpack(">H", pkt[t + 2:t + 4])[0])
            what = "SYN-ACK" if syn and flags & 0x10 else "SYN" if syn else "%s%s%s" % (
                "A" if flags & 0x10 else "", "F" if flags & 0x01 else "", "R" if flags & 0x04 else "")
            line = "#%-6d %-22s -> %-22s %-7s win=%5d" % (n, src, dst, what, win)
            if syn:
                line += " " + " ".join(options(pkt[t + 20:t + doff]))
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
