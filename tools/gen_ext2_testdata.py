#!/usr/bin/env python3
"""Generate the ext2 test-partition content — on the HOST, with real tools.

The point of this data is that os64 never wrote a byte of it: the files are
created here by Linux, packed into the image by e2fsprogs' debugfs, and the
kernel's ext2 driver has to read them back cold. That is the honest test of
an on-disk format: parse what someone else wrote.

pattern.bin is the block-map workout. At the 1KB block size the build pins
(mkfs.ext2 -b 1024), an inode addresses:
    direct blocks   : file bytes        0 ..     12,287   (12 x 1KB)
    single indirect : file bytes   12,288 ..    274,431   (+256 x 1KB)
    double indirect : file bytes  274,432 .. 67,383,295   (+256^2 x 1KB)
so a 1.5MB file forces the driver through all three mechanisms. Every
16-byte record is self-describing ("00001234:os64e2\n" = record 0x...): the
kernel test seeks into each region and checks the record SAYS its own
offset, which catches every off-by-one a block-map walk can commit.
"""

import os
import sys

RECORD = "%08d:os64e2\n"          # exactly 16 bytes
PATTERN_RECORDS = 98304            # x16 bytes = 1.5MB — deep into double-indirect

def main(outdir):
    os.makedirs(outdir, exist_ok=True)

    with open(os.path.join(outdir, "hello.txt"), "w") as f:
        f.write("Hello from a real ext2 filesystem — written by Linux, read by os64!\n")

    with open(os.path.join(outdir, "deep.txt"), "w") as f:
        f.write("the deep file, three directories down\n")

    with open(os.path.join(outdir, "pattern.bin"), "w") as f:
        for i in range(PATTERN_RECORDS):
            f.write(RECORD % i)

    # The debugfs script that packs the files in. debugfs writes to an
    # unmounted image as an ordinary user — no loop devices, no sudo.
    with open(os.path.join(outdir, "debugfs.cmds"), "w") as f:
        f.write(f"write {outdir}/hello.txt hello.txt\n")
        f.write(f"write {outdir}/pattern.bin pattern.bin\n")
        f.write("mkdir dir1\n")
        f.write("mkdir dir1/dir2\n")
        f.write(f"write {outdir}/deep.txt dir1/dir2/deep.txt\n")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "disk/ext2_staging")
