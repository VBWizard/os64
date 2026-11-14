# kernel/ports/sbc: Android's SBC encoder, ported into the kernel

The A2DP sink (docs/design/pending/AUDIO_A2DP.md § Part P) turns the
PCM `/dev/audio` is given into SBC frames for the headphones. The encoder
is **the first third-party code in ring 0**, admitted on the terms
BLUETOOTH_AUDIO.md § Where the encoder lives sets: integer-only, allocates
nothing, keeps no pointer but its own state, parses nothing a stranger
sent, and proven on the host against a reference before the kernel links
it. This file is the record of what was taken, what was read in it, and
what the proof found. AUDIO_A2DP.md § Part E is the design; its "E, as
built" says where this differs from it.

The kernel may call two functions, `sbc_port.h`'s. Nothing else in the
kernel names the encoder; `tools/test_sbc_audit.py` fails if anything does.

## What was taken

From the Android Bluetooth module
(`https://android.googlesource.com/platform/packages/modules/Bluetooth`),
`system/embdrv/sbc/encoder/`, at commit **`6b3bed84`**
(`6b3bed846fb38036e23e70b18c07be2fa1c0aaaf`, 2025-01-09, "Modify
apex_available to Bluetooth apex"), on `main`. **No file is modified.**

**Why that commit.** It is the newest on `main` that touches the directory.
`main`'s head when this was fetched (2026-11-13) was `745ee92b`, dated
2025-03-17, and the directory is byte for byte the same there. The commit
itself changed only `Android.bp`, which is not taken; the files taken last
changed in the merge `d02482c2` (2024-11-11, two cpplint clean-ups). No
release tag was a clearer choice than the newest `main`. Gitiles shows no
history to an anonymous reader, so these were found by halving along
`main`'s first parents and comparing the directory's tree ids at each step.

| File | What it is |
|---|---|
| `upstream/srce/sbc_encoder.c` | `SBC_Encode` (scale factors, the joint-stereo decision) and `SBC_Encoder_Init` |
| `upstream/srce/sbc_analysis.c` | the analysis filter bank: the windowing, as macros, and its sample history |
| `upstream/srce/sbc_dct.c` | the fast DCT the filter bank ends in |
| `upstream/srce/sbc_enc_bit_alloc_ste.c`, `_mono.c` | bit allocation, two channels and one |
| `upstream/srce/sbc_packing.c` | quantising and packing a frame, and its CRC |
| `upstream/srce/sbc_enc_coeffs.c`, `sbc_dct_coeffs.c` | the window and DCT tables |
| `upstream/include/sbc_encoder.h` | `SBC_ENC_PARAMS`, the switches, the two entry points |
| `upstream/include/sbc_enc_func_declare.h`, `sbc_dct.h` | internal declarations and the DCT's multiply macros |
| `upstream/include/sbc_if.h` | a track-level interface (`SBC_init`, `SBC_write`) that nothing in the encoder defines; taken because it is in the directory, included by nothing |

`manifest.json` pins every file by SHA-256 and by git blob id, and each
directory by the tree id the commit names, so the audit ties these bytes to
that commit without a network. Gitiles stamps a tarball with the time it
was asked for, so the archive itself has no stable digest; the URL is in
the manifest all the same.

**Not taken**: `Android.bp`; the decoder beside it
(`system/embdrv/sbc/decoder`); anything else in the module. The licence is
the module's `NOTICE` (it has no `LICENSE` file), carried whole in
`license/android-sbc-LICENSE`.

## How it is built

`sources.mk` names the eight translation units; `sbc.mk` builds them and
`kernel/GNUmakefile` includes it and links the objects. The kernel finds
its own sources with a `find src`, so nothing under `ports/` is compiled by
the generic rule.

| Flags, for `upstream/` | Why |
|---|---|
| the kernel's code generation, taken from `CFLAGS` with the optimisation, warnings, include paths and dependency flags filtered out: `-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -march=x86-64 -m64 -ffreestanding -fno-stack-protector -fno-stack-check -fno-PIC -fno-lto -ffunction-sections -fdata-sections -masm=intel -std=gnu11 -g -pipe` | it is kernel code: no SIMD, no red zone, the kernel's memory model. Filtering rather than restating means a flag the kernel adds reaches this code too |
| `-O2` | the kernel is `-O0`; upstream at `-O0` costs about three times as much a frame (8.7 µs against 3 on the build machine, measured once) |
| `-fno-strict-aliasing` | `sbc_analysis.c` keeps its history in `int32_t s32X[]` and reads and writes it through `int16_t *s16X` |
| `-Wall -Wextra`, no `-Werror` | upstream's warnings are shown; they are upstream's to fix. **There are none**: GCC 14.2 (the kernel's) and GCC 13.3 (the host's) are both silent |
| `-include port/sbc_switches.h` | the switches, below |
| `-I port/compat` | a `string.h` that declares the kernel's `memset` and nothing else |

`port/sbc_port.c` is ours and builds with the kernel's `CFLAGS` whole,
`-O0 -Werror` included.

### The switches (`port/sbc_switches.h`)

Every switch `sbc_encoder.h` defaults is set there, to upstream's own
default, and used by every build (the kernel's, `sbc_port.c`'s, the host
proof's). The audit fails if upstream's headers default a switch that file
does not set.

| Switch | Value | What it chooses |
|---|---|---|
| `SBC_ARM_ASM_OPT` | FALSE | ARM inline assembly for the multiplies |
| `SBC_DSP_OPT` | FALSE | an external 32x16 multiply function |
| `SBC_IPAQ_OPT` | TRUE | multiplies done in `int64_t`, which x86-64 has natively |
| `SBC_GHS_COMPILER` | FALSE | Green Hills assembly syntax (read only with ARM assembly) |
| `SBC_ARM_COMPILER` | TRUE | ARM compiler assembly syntax (read only with ARM assembly) |
| `SBC_USE_ARM_PRAGMA` | FALSE | `#pragma arm section` around the filter history |
| `SBC_IS_64_MULT_IN_WINDOW_ACCU` | FALSE | 32-bit window coefficients; FALSE uses the 16-bit table |
| `SBC_IS_64_MULT_IN_IDCT` | FALSE | 64-bit multiplies in the DCT |
| `SBC_IS_64_MULT_IN_QUANTIZER` | TRUE | a 64-bit product in the quantiser (FALSE whistles at 5.5 and 11 kHz, upstream says) |
| `SBC_FAST_DCT` | TRUE | the fast DCT; FALSE uses the table and matrix |
| `SBC_JOINT_STE_INCLUDED` | TRUE | joint stereo (also changes `SBC_ENC_PARAMS`' layout) |
| `SBC_MAX_NUM_FRAME` | 1 | frames per call |
| `ENC_VX_BUFFER_SIZE` | `MINIMUM_ENC_VX_BUFFER_SIZE + 64` | the analysis history, 224 samples |
| `TRUE`, `FALSE` | `(!FALSE)`, `0` | as `sbc_encoder.h` spells them |

There is no floating point in the code at any setting of these, not even a
literal in a table compiled out: the DCT's cosines appear as decimals only
in comments, beside the hex fixed-point values the code uses. The audit
reads the objects all the same: the kernel is `-mno-sse` but the x87 is not disabled, so a `double`
would compile.

## What was read upstream, and the answers

**The knob is a bit rate.** `SBC_Encoder_Init` turns `u16BitRate` (kbit/s)
into a bitpool: for stereo and joint stereo, `rate × subbands × 1000 / fs`
less `(32 + 4 × subbands × channels + join × subbands) / blocks`; it then
computes that bitpool's frame length and bit rate and takes one off the
bitpool if that bit rate exceeds the one asked; it clamps to 255 (8
subbands) and to 0, and writes the result over `s16BitPool`.
`sbc_port_begin` does not restate any of that: it asks `Init` itself, by
halving between 1 and 4,096 kbit/s, for the least rate whose bitpool is at
least the one asked, and then **refuses to begin unless the encoder holds
exactly the bitpool asked**. Every accepted setting is reached (at 48 kHz
joint stereo, bitpool b at 6b + 39 kbit/s: 345 for 51); the proof sweeps
them all, and a test build that caps the search shows the refusal.

**Writing `s16BitPool` after `Init` would also be sound.** `Init` uses
`u16BitRate` for nothing but the bitpool: the frame length and bit rate it
computes are locals that only correct it; the header byte comes from the
frequency, blocks, mode, allocation and subbands; `EncMaxShiftCounter` from
the subbands and channels; `SbcAnalysisInit` clears the history.
`SBC_Encode` reads `s16BitPool` (bit allocation, packing) and never
`u16BitRate`. The port asks `Init` anyway, so that the encoder's own
arithmetic decides, and the check stands either way.

**The PCM is only read.** `SBC_Encode(params, int16_t *input, output)`
hands `input` to `SbcAnalysisFilter8` (or `4`), which copies each sample
into its history `s16X`; nothing stores through `input`. `sbc_port_encode`
passes the caller's `const` buffer with a cast and a comment saying so;
the proof checks the buffer is unchanged after every frame.

**State, and pointers.** One encoder in the machine: `sbc_encoder.c` has
three globals (`EncMaxShiftCounter`, `s32LRSum[16]`, `s32LRDiff[16]`) and
`sbc_analysis.c` four statics (`s32X`, the history; `s16X`, a pointer into
it; `s32DCTY`; `ShiftCounter`). The parameter block is the boundary's own
static. Nothing keeps a pointer to the caller's memory past a call: the
input and the output are used inside `SBC_Encode` and forgotten.

**Allocation.** None. One C library call, `memset` in `SbcAnalysisInit`.
`sbc_packing.c`'s `__builtin_mul_overflow` (an `int32_t` by a `uint16_t`
into an `int64_t`, which cannot overflow) compiles to an `imul`, not a
call. The audit finds `memset` the only name the port's objects leave
undefined.

**The frame's length.** `EncPacking` returns the distance its write pointer
moved, so it reports what it wrote. It also reads back the bytes of the
frame it has written to compute the CRC, all inside the frame at every
accepted setting.

**The output has no capacity.** Hence the boundary's own buffer:
`SBC_FRAME_BYTES_MAX` (513 bytes, joint stereo at bitpool 250), then 16
guard bytes. A frame of the wrong length, a disturbed guard byte, or a
capacity under a frame fails the encode, and every call after it, until
`begin`.

## UBSan

One finding, and that one check is off for `upstream/` alone
(`-fno-sanitize=shift-base` in `tools/test_sbc_host.sh`):

| Where | What | Why it is benign |
|---|---|---|
| `srce/sbc_dct.c:108` `x6 <<= 1;` | left shift of a negative value | GCC defines a signed `<<` as the two's-complement multiply by two ("GCC does not use the latitude given in C99 and C11 only to treat certain aspects of signed '<<' as undefined", the GCC manual's Integers chapter); UBSan reports it all the same. The magnitudes never reach the sign bit: the DCT's inputs are window sums of under 2^28, halved, and doubled at most twice, so they stay under 2^30. UBSan's signed-overflow check, which stays on, never fired across the sweep and the random frames. The kernel's object compiles these to `add reg, reg`, or, where GCC folded the halving before them, to `and reg, 0xfffffffe` |
| `srce/sbc_dct.c:123` `x7 <<= 1;` | the same | the same |
| `srce/sbc_dct.c:124` `x5 = (x5 << 1) - x7;` | the same | the same |
| `srce/sbc_dct.c:125` `x3 = (x3 << 1) - x5;` | the same | the same |
| `srce/sbc_dct.c:136` `x7 <<= 1;` | the same | the same |

Nothing else: not in the analysis filter's 32-bit accumulations, not in the
quantiser, not in packing.

## The proof

`tools/test_sbc_host.sh` (the parts are AUDIO_A2DP.md § Part E § Proof's):

1. **Every accepted setting** (2 rates × joint or stereo × loudness or SNR
   × bitpools 2 to 250: 1,992), three frames each: `begin` settles on the
   bitpool, the frame is `sbc_frame_bytes` long and read back by a parser
   written from the specification (header, join bits, scale factors, CRC),
   the header says the settings, nothing past the frame is written, the
   PCM is unchanged, the frame copied out is the one made. Twenty
   refused settings refused by every function.
2. **FFmpeg 6.1** decodes our streams and FFmpeg's own at 24 settings
   (bitpools 8 to 250, both modes, both rates, three SNR settings) for a
   sweep, white noise, full-scale square waves, music-like notes and
   silence. Frames out must equal frames in. The delay is 73 samples for
   both encoders, found by cross-correlation and asserted the same at every
   setting. **Ours is never more than 1.06 dB worse than FFmpeg's encoder**
   (full-scale squares at bitpool 250, where both are above 67 dB), and
   usually within 0.3 dB either way; silence decodes to 0. The SNR settings
   are held against FFmpeg's loudness stream (FFmpeg cannot allocate by
   SNR) and beat it by up to 7 dB.
3. **Side information** for identical, opposite and one-silent channels.
4. **Random**: 4,800 frames at 400 random settings, under ASan and UBSan.
5. **Speed**: about 3 µs a frame (48 kHz joint stereo, bitpool 51, the
   port's flags, no sanitizers), 0.11% of a core in real time on the
   build machine. Bound: 200 µs.
6. **Tripwires**: a short capacity, a frame of the wrong length, a frame
   over the guard (a build whose room is 100 bytes), and a bitpool the
   encoder did not settle on (a build whose search is capped) each fail
   the boundary.

FFmpeg 6.1's `-xerror` ends a decode at the first bad frame but exits 0,
and without it a bad frame is dropped in silence. So the suite checks the
decode's frame count and FFmpeg's error output, and proves that check can
fail: one flipped scale-factor bit in a good stream must be caught.

`tools/test_sbc_audit.py`: the import is the pinned commit; every object
is integer-only (and the scan is shown to see x87 and SSE code in a probe);
`memset` is the only name left undefined; the boundary's object exports
the two functions and nothing else; the licence carries the NOTICE and
every file's notice. `tools/test_sbc_frame_mutants.py` breaks our two files
one rule at a time (`mutants_parallel.py ... --with kernel`).

## Upstream, and the monthly scan

Canonical: `https://android.googlesource.com/platform/packages/modules/Bluetooth`,
`main`, path `system/embdrv/sbc/encoder`. Gitiles serves a directory's
listing (`+/<commit>/<path>?format=JSON`) and a file (`?format=TEXT`, base64)
without signing in, but not the log; `main~N` resolves, which is how the
pinned commit was found. **What to look at**: whether the directory's tree
ids (`manifest.json` `trees`) have changed at `main`'s head. On 2026-11-13
`main`'s head was dated 2025-03-17.

A bug here reaches wrong sound, a frame the headphones reject, or, if the
encoder wrote past its frame by more than the guard, kernel memory beside
the boundary's buffer. Its inputs are our PCM and settings
`sbc_settings_valid` admitted; it parses nothing from outside.

**For `THIRD_PARTY.md`** (not in this tree; this is its row when it is):

| Component | Pinned at | Where in the tree | What it is for | What a bug there reaches | Upstream, and where fixes are announced |
|---|---|---|---|---|---|
| **Android SBC encoder** (`system/embdrv/sbc/encoder`) | commit `6b3bed84` (2025-01-09, `main`), unpatched | `kernel/ports/sbc/upstream/`, `manifest.json` | SBC frames for the A2DP sink, in the kernel | wrong sound or a rejected frame; a write past the frame beyond the 16 guard bytes would reach kernel memory | https://android.googlesource.com/platform/packages/modules/Bluetooth (`main`); Android Security Bulletins, CVE search "system/embdrv/sbc" |

## Licence

Apache 2.0, which os64 (MIT) may carry. Section 4 asks a redistribution to
give the licence, keep the files' notices, and carry the NOTICE:

- **In source**: `license/android-sbc-LICENSE` holds the module's `NOTICE`
  as fetched (pinned in `manifest.json`) and the notice at the head of every
  vendored file; the files themselves are untouched.
- **In a binary**: the kernel links the objects, but nothing calls the
  encoder yet, so `--gc-sections` leaves all of it out of
  `bin/os64_kernel`. When the A2DP sink calls it, the notice must go on both
  images as speexdsp's does (`/etc/licenses/`): a DEBTS row says so.
