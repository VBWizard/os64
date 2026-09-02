#!/bin/bash
# Cross-check libgzip against Python's zlib across formats and chunk boundaries.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/libgzip/include -I userland/libos64/include -I abi/include \
   userland/libgzip/inflate.c userland/libgzip/gzip.c \
   userland/libos64/crc32.c tools/test_gzip_host.c \
   -o "$work/test_gzip"

python3 - "$work" <<'PY'
import gzip
import pathlib
import random
import struct
import sys
import zlib

root = pathlib.Path(sys.argv[1])
payload = (bytes(range(256)) * 257 +
           b"the quick brown fox jumps over the lazy dog\n" * 1703)
(root / "payload").write_bytes(payload)
(root / "empty").write_bytes(b"")
rng = random.Random(0x064D3F1A)
far_payload = rng.randbytes(32768)
far_payload += far_payload + far_payload[:4096]
(root / "far_payload").write_bytes(far_payload)

def raw(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    encoder = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    return encoder.compress(data) + encoder.flush()

(root / "stored.deflate").write_bytes(raw(payload, 0))
(root / "fixed.deflate").write_bytes(raw(payload, 6, zlib.Z_FIXED))
(root / "dynamic.deflate").write_bytes(raw(payload, 9))
(root / "empty.deflate").write_bytes(raw(b""))
(root / "far.deflate").write_bytes(raw(far_payload, 9))

plain = gzip.compress(payload, compresslevel=6, mtime=0)
(root / "plain.gz").write_bytes(plain)
(root / "empty.gz").write_bytes(gzip.compress(b"", mtime=0))
(root / "far.gz").write_bytes(gzip.compress(far_payload, compresslevel=9,
                                             mtime=0))

# Exercise every optional header field, including FHCRC. Header CRC16 is the
# low half of the ordinary gzip CRC32, not a separate polynomial.
header = bytearray(b"\x1f\x8b\x08\x1e" + b"\x00\x00\x00\x00\x00\xff")
extra = b"os64"
header += struct.pack("<H", len(extra)) + extra
header += b"fixture-name\0fixture-comment\0"
header += struct.pack("<H", zlib.crc32(header) & 0xffff)
body = raw(payload, 6)
trailer = struct.pack("<II", zlib.crc32(payload) & 0xffffffff,
                      len(payload) & 0xffffffff)
(root / "optional.gz").write_bytes(header + body + trailer)
bad_header_crc = bytearray(header + body + trailer)
bad_header_crc[len(header) - 1] ^= 0x01
(root / "bad_header_crc.gz").write_bytes(bad_header_crc)

second = b"second member\n" * 33
(root / "concat_expected").write_bytes(payload + second)
(root / "concat.gz").write_bytes(plain + gzip.compress(second, mtime=0))

bad_crc = bytearray(plain)
bad_crc[-8] ^= 0x80
(root / "bad_crc.gz").write_bytes(bad_crc)
bad_size = bytearray(plain)
bad_size[-1] ^= 0x01
(root / "bad_size.gz").write_bytes(bad_size)
reserved = bytearray(plain)
reserved[3] |= 0x20
(root / "reserved.gz").write_bytes(reserved)
method = bytearray(plain)
method[2] = 7
(root / "method.gz").write_bytes(method)
(root / "invalid_type.deflate").write_bytes(b"\x07")
(root / "bad_stored.deflate").write_bytes(b"\x01\x01\x00\x00\x00A")
(root / "truncated.gz").write_bytes(plain[:-3])
(root / "trailing.gz").write_bytes(plain + b"not gzip")
PY

run() {
    ASAN_OPTIONS=detect_leaks=0 "$work/test_gzip" "$@"
}

for stream in stored fixed dynamic; do
    for chunks in "1 1" "2 3" "7 31" "4096 17" "65536 65536"; do
        set -- $chunks
        run raw "$work/$stream.deflate" "$work/payload" "$1" "$2" \
            18446744073709551615 done 0
    done
    echo "raw $stream blocks across five chunk shapes                 PASS"
done

run raw "$work/empty.deflate" "$work/empty" 1 1 0 done 0
run raw "$work/far.deflate" "$work/far_payload" 1 7 \
    18446744073709551615 done 0
run raw "$work/invalid_type.deflate" "$work/empty" 1 1 \
    18446744073709551615 "malformed stream" 0
run raw "$work/bad_stored.deflate" "$work/empty" 1 1 \
    18446744073709551615 "malformed stream" 0
run raw "$work/dynamic.deflate" "$work/payload" 3 5 100 \
    "output limit exceeded" 0
echo "raw empty, 32K history, malformed, and expansion-limit cases PASS"

for chunks in "1 1" "2 3" "11 17" "4096 31" "65536 65536"; do
    set -- $chunks
    run gzip "$work/plain.gz" "$work/payload" "$1" "$2" \
        18446744073709551615 done 1
    run gzip "$work/optional.gz" "$work/payload" "$1" "$2" \
        18446744073709551615 done 1
    run gzip "$work/concat.gz" "$work/concat_expected" "$1" "$2" \
        18446744073709551615 done 2
done
echo "gzip plain, optional headers, and concatenated members       PASS"

run gzip "$work/empty.gz" "$work/empty" 1 1 0 done 1
run gzip "$work/far.gz" "$work/far_payload" 1 7 \
    18446744073709551615 done 1

run gzip "$work/bad_crc.gz" "$work/payload" 7 13 \
    18446744073709551615 "checksum or size mismatch" 0
run gzip "$work/bad_size.gz" "$work/payload" 7 13 \
    18446744073709551615 "checksum or size mismatch" 0
run gzip "$work/bad_header_crc.gz" "$work/empty" 1 1 \
    18446744073709551615 "checksum or size mismatch" 0
run gzip "$work/reserved.gz" "$work/empty" 2 2 \
    18446744073709551615 "malformed stream" 0
run gzip "$work/method.gz" "$work/empty" 2 2 \
    18446744073709551615 "unsupported compression method" 0
run gzip "$work/truncated.gz" "$work/payload" 5 19 \
    18446744073709551615 "truncated stream" 0
run gzip "$work/trailing.gz" "$work/payload" 5 19 \
    18446744073709551615 "truncated stream" 1
run gzip "$work/plain.gz" "$work/payload" 3 5 100 \
    "output limit exceeded" 0
echo "gzip empty/history, corruption, truncation, and limit cases  PASS"
