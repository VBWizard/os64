#!/usr/bin/env python3
"""Generate guest regressions for VMAs overlapping the NULL guard.

Run: python3 tools/gen_page_zero_fixtures.py OUTDIR
Copy OUTDIR's contents to /home/pagezero in a scratch guest, then run:
    /bin/husk /home/pagezero/run.husk /home/pagezero

The ELF files contain a normal code segment and a low data segment. Reads
from file-backed and anonymous VMAs at 0 and 0xfff must exit 139; a read
at 0x1000 must succeed. Syscall copies in both directions must return -2
for first-page buffers, with successful adjacent-page controls.
A broken demand pager can fault-loop and exhaust
memory, so run these fixtures on scratch disks with a host-side timeout.
"""

import argparse
from pathlib import Path
import struct


def elf_image(address, file_backed, operation="load"):
    # Intel: mov rax, address; mov al, [rax]; mov eax, 90; ret.
    # The return reaches the task's exit trampoline if the read succeeds.
    code = b"\x48\xb8" + struct.pack("<Q", address)
    code += b"\x8a\x00\xb8\x5a\x00\x00\x00\xc3"
    data = b"\x5a"
    if operation != "load":
        # debug_log copies an empty string IN; ticks copies a 16-byte
        # structure OUT. Return 90 only when the syscall result matches.
        number = 1 if operation == "copyin" else 26
        expected = -2 if address < 0x1000 else 0
        code = b"\xb8" + struct.pack("<I", number)
        code += b"\x48\xbf" + struct.pack("<Q", address)  # mov rdi, address
        code += b"\x31\xf6\x0f\x05"  # xor esi, esi; syscall
        code += b"\x48\x83\xf8" + struct.pack("b", expected)
        code += b"\x0f\x94\xc0\x0f\xb6\xc0\x6b\xc0\x5a\xc3"
        data = bytes(16)
    code_offset, code_address = 0x3000, 0x400000
    data_offset = 0x1000 + (address & 0xfff)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack(
        "<16sHHIQQQIHHHHHH", ident, 2, 62, 1, code_address,
        64, 0, 0, 64, 56, 2, 0, 0, 0,
    )
    data_segment = struct.pack(
        "<IIQQQQQQ", 1, 6, data_offset, address, address,
        len(data) if file_backed else 0, len(data), 0x1000,
    )
    code_segment = struct.pack(
        "<IIQQQQQQ", 1, 5, code_offset, code_address, code_address,
        len(code), len(code), 0x1000,
    )
    image = bytearray(code_offset + len(code))
    image[:len(header)] = header
    image[64:176] = data_segment + code_segment
    image[data_offset:data_offset + len(data)] = data
    image[code_offset:] = code
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("outdir", type=Path)
    args = parser.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    cases = (
        ("file_zero", 0, True, 139),
        ("file_edge", 0xfff, True, 139),
        ("anon_zero", 0, False, 139),
        ("anon_edge", 0xfff, False, 139),
        ("file_next", 0x1000, True, 90),
        ("anon_next", 0x1000, False, 90),
    )
    script = ['cd "$1" || exit 1']
    for name, address, file_backed, expected in cases:
        fixture = args.outdir / name
        fixture.write_bytes(elf_image(address, file_backed))
        fixture.chmod(0o755)
        script += [
            f"./{name}",
            "result=$?",
            f'if test "$result" -ne {expected}; then',
            f'    echo "FAIL {name}: got $result, expected {expected}"',
            "    exit 1",
            "fi",
            f'echo "PASS {name}"',
        ]
    for operation in ("copyin", "copyout"):
        for backing, file_backed in (("file", True), ("anon", False)):
            for location, address in (("zero", 0), ("one", 1),
                                      ("edge", 0xfff), ("next", 0x1000)):
                name = f"{operation}_{backing}_{location}"
                fixture = args.outdir / name
                fixture.write_bytes(elf_image(address, file_backed, operation))
                fixture.chmod(0o755)
                script += [
                    f"./{name}",
                    "result=$?",
                    'if test "$result" -ne 90; then',
                    f'    echo "FAIL {name}: got $result, expected 90"',
                    "    exit 1",
                    "fi",
                    f'echo "PASS {name}"',
                ]
    script += ['echo "PASS page-zero VMA regressions"']
    (args.outdir / "run.husk").write_text("\n".join(script) + "\n")


if __name__ == "__main__":
    main()
