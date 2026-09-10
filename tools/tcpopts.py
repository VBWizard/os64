#!/usr/bin/env python3
"""tcpopts.py — what the SYN exchange said, read out of a pcap.

    tools/tcpopts.py net_capture.pcap [--all]

Prints every SYN and SYN-ACK in the capture with its window field and its
options decoded by name (MSS, WS=<shift>, SACKok, TS), because that is the
question window scaling turns on: did WE send the shift, and did THEY
answer with one. tshark would do it, and is not installed on every host
this project builds on; this is what it takes with the standard library.

`--all` prints every TCP segment's window too. The field is always shown
raw, and once BOTH SYNs of a flow have been seen and both carried a shift,
the bytes the field means follow it (`win=32768 = 4194304`) — the sender's
own shift, which is the rule RFC 7323 §2.3 states: a window is scaled by
the count its sender offered, and only if the other side offered one back.
A flow whose handshake is not in the capture is marked `(no SYN seen)`,
because its shift is unknowable from the segments alone.

Reads the classic pcap format QEMU's filter-dump writes (both byte orders);
not pcapng. Non-initial IPv4 fragments carry no TCP header and are skipped.
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


def tcp_segment(pkt):
    """The TCP header's offset and the end of the TCP payload, or None for
    anything that is not an initial IPv4 fragment carrying a whole TCP
    header: a non-initial fragment has protocol 6 too, but its payload is
    the middle of somebody's segment, not a header."""
    if len(pkt) < 34 or pkt[12:14] != b"\x08\x00" or pkt[23] != 6:
        return None
    ihl = (pkt[14] & 0xF) * 4
    if ihl < 20:
        return None
    total = struct.unpack(">H", pkt[16:18])[0]
    fragment_offset = struct.unpack(">H", pkt[20:22])[0] & 0x1FFF
    if fragment_offset:
        return None
    t = 14 + ihl
    end = min(len(pkt), 14 + total)
    if end < t + 20:
        return None
    doff = (pkt[t + 12] >> 4) * 4
    if doff < 20 or t + doff > end:
        return None
    return t, doff


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    everything = "--all" in sys.argv
    shift = {}   # (src, dst) -> the shift src offered toward dst; None if its SYN carried none
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
            seg = tcp_segment(pkt)
            if seg is None:
                continue
            t, doff = seg
            flags = pkt[t + 13]
            syn = bool(flags & 0x02)
            if not syn and not everything:
                continue
            win = struct.unpack(">H", pkt[t + 14:t + 16])[0]
            src = "%d.%d.%d.%d:%d" % (*pkt[26:30], struct.unpack(">H", pkt[t:t + 2])[0])
            dst = "%d.%d.%d.%d:%d" % (*pkt[30:34], struct.unpack(">H", pkt[t + 2:t + 4])[0])
            what = "SYN-ACK" if syn and flags & 0x10 else "SYN" if syn else "%s%s%s" % (
                "A" if flags & 0x10 else "", "F" if flags & 0x01 else "", "R" if flags & 0x04 else "")
            line = "#%-6d %-22s -> %-22s %-7s win=%5d" % (n, src, dst, what, win)
            if syn:
                names = options(pkt[t + 20:t + doff])
                ws = next((int(x[3:]) for x in names if x.startswith("WS=")), None)
                shift[(src, dst)] = ws
                line += " " + " ".join(names)
            elif (src, dst) not in shift or (dst, src) not in shift:
                line += " (no SYN seen)"
            elif shift[(src, dst)] is not None and shift[(dst, src)] is not None:
                line += " = %d" % (win << shift[(src, dst)])
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
