#!/usr/bin/env python3
"""Deterministic fixture mutations with a wall-clock budget and per-batch deadline."""
import argparse
import random
from pathlib import Path
import struct
import subprocess
import time
from html_reference import cases


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--driver', required=True)
    ap.add_argument('--seconds', type=float, default=30)
    args = ap.parse_args()
    source = [c['input'].encode('utf-8', 'surrogatepass') for c in cases()]
    seed = 0x64f022
    rng = random.Random(seed)
    start = time.monotonic()
    batches = 0
    total = 0
    while time.monotonic() - start < args.seconds:
        wire = bytearray()
        for _ in range(256):
            data = bytearray(rng.choice(source))
            operation = rng.randrange(4)
            if operation == 0 and data:
                data[rng.randrange(len(data))] ^= 1 << rng.randrange(8)
            elif operation == 1:
                data = data[:rng.randrange(len(data) + 1)]
            elif operation == 2:
                other = rng.choice(source)
                data = data[:rng.randrange(len(data) + 1)] + other[rng.randrange(len(other) + 1):]
            elif data:
                at = rng.randrange(len(data))
                data = data[:at] + data[at:at + rng.randrange(1, 32)] * rng.randrange(1, 100) + data[at:]
            wire += struct.pack('<4I', 1, 0, len(data), 0) + data
        try:
            result = subprocess.run([args.driver, '--fuzz'], input=wire, stdout=subprocess.PIPE,
                                    check=True, timeout=20)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            path = Path(args.driver).parent / 'fuzz-inputs.bin'
            path.write_bytes(wire)
            print(f'Reproduce batch {batches}: {args.driver} --fuzz < {path}', flush=True)
            raise
        assert b'cases=256 ' in result.stdout, result.stdout
        total += 256
        batches += 1
    print(f'Fuzz: mutations={total} chunkings=3 seed={seed:#x} '
          f'budget={args.seconds:g}s elapsed={time.monotonic() - start:.2f}s batches={batches}')


if __name__ == '__main__':
    main()
