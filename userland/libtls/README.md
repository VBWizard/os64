# BearSSL foundation

This directory contains a pinned BearSSL core, a private os64 compatibility
layer, and host/guest fixtures. It builds `userland/obj/libbearssl-foundation.a`
and `/tests/bearssltest`. It does not install a public crypto header or
`libtls.so`, enable native HTTPS, or change the kernel or libos64.
The client API, selected production object list, trust policy, and transport
integration are subsequent slices of [TLS.md](../../TLS.md).

## Provenance

- Official source: <https://www.bearssl.org/git/BearSSL>
- Commit: `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f` (2026-04-06).
- Git tree: `a8f933a27b14eaf3a348fe9b294bf4262dbb216c`.
- Release baseline reviewed: v0.6, `7d8e767e79bb1750345e571ec89cca1da13b52df`.
- `git archive --format=tar` SHA-256 at the selected commit:
  `a50dd8ae1eb20fc850c60cae6404f7426fd3f252e295f528d0ed59a6d9164124`.
- License: [upstream/LICENSE.txt](upstream/LICENSE.txt), MIT.
- Selection rationale and release delta: [UPSTREAM_REVIEW.md](UPSTREAM_REVIEW.md).

`upstream/` retains the complete 466-file source distribution, including
tests, T0 sources/compiler sources, build recipes, tools, and the upstream
compiler executable. The os64 build does not execute that executable or the
upstream build scripts. `sources.mk` explicitly lists the 294 core C files
used for the foundation archive. Reference tools and server/legacy algorithms
are not a production client allowlist; only archive members reached by the
fixture are linked into the guest program. This broad archive lets the link
audit detect dependencies across the core before the client object selection.

`upstream.sha256` records the pristine per-file hashes. Run:

```sh
python3 tools/check_bearssl_import.py
```

The checker reverses the recorded patches in a temporary copy, checks every
file and the source list, and verifies the extracted test header. Extra files,
missing files, and content changes fail. This makes local changes reviewable;
the hashes identify the fetched bytes, not independent authentication of the
upstream publisher. Builds and tests do not fetch from the network.

## Local patch and configuration

The sole upstream patch is [0001-scalar-intrinsics.patch](patches/0001-scalar-intrinsics.patch).
It gates the x86 intrinsic-header block on enabled AES-NI, SSE2, or RDRAND
implementations. Upstream's unconditional include from disabled acceleration
files pulls in hosted `stdlib.h` via the cross compiler's `mm_malloc.h`.
The patch changes no crypto arithmetic or generated state machine. Its SHA-256
is `8a15178d31db1eae130e63bb9dff4db7d1f14f9aba386d7f9b10de0cb041ce84`.

[port/config.h](port/config.h) disables automatic getentropy, urandom,
Windows RNG, RDRAND, and system time. It selects scalar code without AES-NI,
SSE2 intrinsics, POWER8, or 128-bit integer backends. Ordinary x86-64 SSE2
compiler output remains allowed by the existing os64 ABI. With this profile,
the default EC dispatcher uses m31 for P-256/Curve25519 and i31 for the other
supported prime curves; default RSA uses i31. AES uses ct64 and GHASH uses
ctmul64. Optional acceleration can be evaluated in a separate change.

The unaligned union-pointer shortcut is disabled in favor of upstream's
portable byte loads. The hosted reference also disables that shortcut so
alignment UBSan remains enabled; it otherwise retains upstream's hosted
configuration and original C sources. Both builds use ASan/UBSan.

The private `string.h` maps memory/string operations to existing os64
functions. [port/runtime.c](port/runtime.c) supplies an ordinary bytewise
memcmp with unsigned-byte ordering. Upstream constant-time comparisons are
unchanged. The host tests compile the real libos64 string implementation,
renaming its compiler aliases to avoid interposing on the sanitizer runtime.

`-O2`, PIC, hidden visibility, no red zone, and header dependencies apply to
the core. The full-core link audit permits exactly four os64 imports and
checks supported relocations, absence of runtime initialization/text
relocations, private symbols, and absence of hardware RNG/syscall opcodes.
Its temporary DSO is a link probe under `obj/`, not an installed library.

## Generated code and licensing

Upstream-generated C is retained as published. Corresponding `.t0` inputs and
the C# generator are included. Upstream `mk/Rules.mk` describes regeneration:
build the generator with `mk/mkT0.sh`, then use `make T0` with Mono. No C#
toolchain was available during this port, so regeneration was not verified.
Any future generated-code edit must change/review the T0 source and record
the regenerated diff as well.

`python3 tools/bearssl_vectors.py` extracts the GCM and RFC 6979 ECDSA vectors
from the pinned `test/test_crypto.c`, plus `test/x509/ee-p256.crt`, into
`test/vectors.h`. It retains attribution and embeds the full upstream license
in the guest fixture. `/tests/bearssltest --license` prints that notice; the
ELF audit checks its presence in the binary, including when distributed alone.

## Validation commands

From the repository root:

```sh
python3 tools/check_bearssl_import.py
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_bearssl_host.py --output /tmp/bearssl-proof
make -C userland
python3 tools/audit_bearssl.py
make
```

The host output directory must be new; omit `--output` to use disposable
storage. `detect_leaks=0` is needed in the current ptrace environment;
LeakSanitizer can be left enabled on hosts that support it. Address and
undefined-behavior sanitizers remain active, with no recovery after errors.

The host runner tests 19 explicitly checked upstream crypto groups, including
SHA-2, HMAC/HKDF, DRBGs, TLS PRF, AES-ct64, GCM, ChaCha20/Poly1305, RSA-i31,
and EC/ECDSA. It also runs the upstream X.509 chains/name extraction suite.
Identical fixture transcripts compare pristine hosted code with the adapted
core across deterministic inputs and fragmented GCM processing. These are
bounded differential tests, not a fuzz campaign or timing-security proof.

Run `/tests/testrun bearssltest` in the guest using the isolated-disk procedure
in [VERIFICATION.md](../../VERIFICATION.md). The fixture covers upstream GCM
vectors with bytewise input and damaged tags; ECDSA SHA-256/384/512 vectors
for P-256/P-384/P-521; terminal decoder errors; malformed CBC lengths; explicit
time; and refusal to start an unseeded client. A fixture-only seed proves that
explicit injection can produce a ClientHello. No network handshake is claimed.
Compare its `DIGEST` line with the hosted reference output.

The fixture owns its large contexts/buffers on the heap. Compiler `.su` files
record individual core stack frames; those sizes are not whole-call-chain
bounds or a production connection-cap measurement.

## Recorded evidence (2026-09-07)

- Import verification: 466 pristine file hashes, the recorded local patch,
  all 294 core source entries, and extracted vector data checked.
- Host GCC 13.3.0: all 19 selected crypto groups passed in both builds under
  ASan/UBSan. Both X.509 suites passed (52 chain cases plus name extraction);
  their complete output matched. LeakSanitizer was disabled for ptrace.
- Cross GCC 14.2.0: strict userland and full-image builds passed. The image
  build emitted existing RWX-segment warnings for legacy kernel fixtures;
  the BearSSL build/audit emitted no warnings.
- Whole-core audit: four runtime imports (`os64_memcpy`, `os64_memmove`,
  `os64_memset`, `os64_strlen`), no exported BearSSL/private libc symbols,
  and no hosted runtime, hardware RNG, or unsupported dynamic relocations.
- QEMU 8.2.2 TCG, q35, default qemu64 CPU, 8 vCPUs, 8 GiB RAM: isolated
  ext2 root/home disks reached boot complete; `/tests/testrun bearssltest`
  reported **1 passed, 0 failed, 0 skipped**. The root UUID was
  `1ec5f5ab-71b7-45cd-a7a4-05646e878e57`.
- Reference, adapted, and guest fixture transcript digest:
  `978e00828d5f0432e655182e175b37459c02e0896651aea2c5f9ed0ecb03def0`.
- The largest reported individual core frame was 2432 bytes (i15 RSA private
  operation, outside the intended production client profile). This does not
  establish a whole-call-chain stack limit.

Local evidence artifacts are under `/tmp/os64-bearssl-host-3` and
`/tmp/os64-bearssl-qemu/scalar`; full-image and audit logs are
`/tmp/os64-bearssl-image-build.log` and `/tmp/os64-bearssl-audit.log`.
Physical-hardware runs, TLS interoperability, certificate-policy coverage,
fuzzing, and production entropy integration are not established by this slice.
