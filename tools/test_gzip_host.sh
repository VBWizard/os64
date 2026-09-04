#!/bin/bash
# Cross-check both libgzip directions against Python's zlib across formats and
# chunk boundaries.

set -eu
cd "$(git rev-parse --show-toplevel)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cc -std=c11 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
   -I userland/libgzip/include -I userland/libos64/include -I abi/include \
   userland/libgzip/inflate.c userland/libgzip/deflate.c \
   userland/libgzip/gzip.c \
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
(root / "random_payload").write_bytes(rng.randbytes(100000))
for size in (1, 2, 3, 257, 258, 259, 32767, 32768, 32769,
             65535, 65536, 65537):
    (root / f"boundary-{size}").write_bytes(rng.randbytes(size))

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
(root / "trailing_long.gz").write_bytes(plain + b"not gzip" * 5)
(root / "trailing_zero.gz").write_bytes(plain + bytes(512))
(root / "next_header_truncated.gz").write_bytes(plain + b"\x1f\x8b\x08")
PY

run() {
    ASAN_OPTIONS=detect_leaks=0 "$work/test_gzip" "$@"
}

encode() {
    ASAN_OPTIONS=detect_leaks=0 "$work/test_gzip" encode "$@"
}

for chunks in "1 1" "2 3" "7 31" "4096 17" "65536 65536"; do
    set -- $chunks
    encode raw "$work/payload" "$work/encoded-raw-$1-$2" "$1" "$2" 0
    encode gzip "$work/payload" "$work/encoded-gzip-$1-$2" "$1" "$2" \
        1234567890
done
encode raw "$work/empty" "$work/encoded-raw-empty" 1 1 0
encode gzip "$work/empty" "$work/encoded-gzip-empty" 1 1 1234567890
encode raw "$work/far_payload" "$work/encoded-raw-far" 13 29 0
encode gzip "$work/random_payload" "$work/encoded-gzip-random" 19 23 0
for size in 1 2 3 257 258 259 32767 32768 32769 65535 65536 65537; do
    encode raw "$work/boundary-$size" "$work/encoded-raw-boundary-$size" \
        97 113 0
    encode gzip "$work/boundary-$size" \
        "$work/encoded-gzip-boundary-$size" 101 109 0
done

python3 - "$work" <<'PY'
import gzip
import pathlib
import struct
import sys
import zlib

root = pathlib.Path(sys.argv[1])
payload = (root / "payload").read_bytes()
raw_outputs = []
gzip_outputs = []
for path in root.glob("encoded-raw-[0-9]*"):
    encoded = path.read_bytes()
    assert zlib.decompress(encoded, -15) == payload, path
    raw_outputs.append(encoded)
for path in root.glob("encoded-gzip-[0-9]*"):
    encoded = path.read_bytes()
    assert gzip.decompress(encoded) == payload, path
    assert struct.unpack_from("<I", encoded, 4)[0] == 1234567890, path
    gzip_outputs.append(encoded)
assert len(set(raw_outputs)) == 1
assert len(set(gzip_outputs)) == 1
assert len(gzip_outputs[0]) < len(payload) // 4
assert zlib.decompress((root / "encoded-raw-empty").read_bytes(), -15) == b""
assert gzip.decompress((root / "encoded-gzip-empty").read_bytes()) == b""
assert zlib.decompress((root / "encoded-raw-far").read_bytes(), -15) == \
       (root / "far_payload").read_bytes()
assert gzip.decompress((root / "encoded-gzip-random").read_bytes()) == \
       (root / "random_payload").read_bytes()
for path in root.glob("boundary-*"):
    size = path.name.split("-")[1]
    expected = path.read_bytes()
    assert zlib.decompress(
        (root / f"encoded-raw-boundary-{size}").read_bytes(), -15) == expected
    assert gzip.decompress(
        (root / f"encoded-gzip-boundary-{size}").read_bytes()) == expected
PY
echo "raw/gzip encoders: chunks, boundaries, mtime, ratio, interop PASS"

run raw "$work/encoded-raw-1-1" "$work/payload" 3 5 \
    18446744073709551615 done 0
run gzip "$work/encoded-gzip-1-1" "$work/payload" 3 5 \
    18446744073709551615 done 1
echo "libgzip decoders accept streams produced by libgzip encoders PASS"

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
    18446744073709551615 "trailing data after final member" 1
run gzip "$work/trailing_long.gz" "$work/payload" 5 19 \
    18446744073709551615 "trailing data after final member" 1
run gzip "$work/trailing_zero.gz" "$work/payload" 5 19 \
    18446744073709551615 "trailing data after final member" 1
run gzip "$work/next_header_truncated.gz" "$work/payload" 5 19 \
    18446744073709551615 "truncated stream" 1
run gzip "$work/plain.gz" "$work/payload" 3 5 100 \
    "output limit exceeded" 0
run gzip "$work/plain.gz" "$work/payload" 7 13 140724 done 1
run gzip "$work/plain.gz" "$work/payload" 7 13 140723 \
    "output limit exceeded" 0
run gzip "$work/concat.gz" "$work/concat_expected" 7 13 141186 done 2
run gzip "$work/concat.gz" "$work/concat_expected" 7 13 141185 \
    "output limit exceeded" 0
echo "gzip empty/history, corruption, truncation, and limit cases  PASS"
