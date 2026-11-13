# libsound: speexdsp's resampler, ported

`libsound.so` changes the sample rate of 16-bit PCM. `/dev/audio` takes one
rate (48 kHz, `os64/audio.h`); a great deal of sound is at another (a CD's
44.1 kHz, and much of what a web page sends), so a player resamples first.
The interface is `os64/resample.h`, four functions. `play` is its first
user; yonder's media element will be the second.

The work is speexdsp's resampler, **unmodified**. Signal-processing code in
this arc is ported and proven, not written here (BLUETOOTH_AUDIO.md
§ Players).

## What was taken

speexdsp 1.2.1 (tag `SpeexDSP-1.2.1`, commit `1b28a0f6`, released
2022-06-13), from the release archive `manifest.json` names and pins by
SHA-256. Five files, each pinned by its own hash:

| File | What it is |
|---|---|
| `libspeexdsp/resample.c` | the resampler: 1,239 lines, the one translation unit built |
| `libspeexdsp/arch.h` | its type and arithmetic macros |
| `include/speex/speex_resampler.h` | its interface |
| `COPYING`, `AUTHORS` | the notice |

**Nothing else in speexdsp is here**: not the echo canceller, the
preprocessor (noise suppression), the jitter buffer, the filter bank, or
any of its three FFTs. `resample.c` references none of them. That is
checked, not assumed: the library is linked `--no-undefined`, and
`tools/test_sound_audit.py` fails if it imports anything but an allocator
and `sin`, `floor`, `fabs`.

No file is patched. `tools/import_sound.py --check <archive>` proves
`upstream/` is the archive's bytes; the audit proves the tree matches the
manifest on every run.

## How it is built

`resample.c` is compiled with `OUTSIDE_SPEEX`, the switch upstream provides
for taking this one file into another project: it then needs only its own
two headers, `<stdlib.h>` for three allocator names, `<math.h>` and
`<limits.h>`. Under that switch `speex_resampler.h` defines speexdsp's four
integer names itself (`short`, `int` and their unsigned forms), so
`speexdsp_types.h` and the header `configure` would generate are not used
and are not vendored.

- `FLOATING_POINT`: upstream's reference arithmetic. Userland has floating
  point; the 16-bit entry points convert at the edges.
- `RANDOM_PREFIX=os64_speex`: the name prefix its header insists on outside
  speexdsp. None of those names leaves the library (`exports.map`).
- `port/compat/stdlib.h` gives `calloc`, `realloc` and `free` as libos64's.
- `port/resample.c` is `os64/resample.h`: about sixty lines over
  `speex_resampler_init`, `_process_interleaved_int`,
  `_get_input_latency` and `_destroy`, at quality 5 (upstream's "desktop").
- The plain C inner loops are used. Upstream's SSE and NEON variants
  (`resample_sse.h`, `resample_neon.h`) are not vendored.

### Warnings

The upstream file is compiled with the repository's `-Wall -Wextra -Werror`
and one exemption, `-Wno-sign-compare`: six comparisons between its
unsigned lengths and signed loop counters. They are upstream's to change,
and the file is kept byte for byte. os64's own file takes the full set.

## Proof

- `tools/test_resample_host.sh`: the two translation units, built on the
  host under ASan and UBSan, run through
  `userland/tests/resampletest/checks.h`. A 1 kHz sine goes in at one rate
  through writes of awkward sizes and must come out the same sine at
  another: fitted by least squares, under two counts in 12,000 left over
  (it measures about 0.4, which is the rounding to 16 bits) and the
  amplitude within a thousandth. 44.1 to 48 kHz, 48 to 44.1, mono from
  22.05, and 48 to 48; then the refusals.
- `/tests/resampletest`: the same header in ring 3, against `libsound.so`
  as the kernel loaded it.
- `/tests/playtest`: `/bin/play` on a 44.1 kHz mono WAV with the audio tap
  listening, and what the sink was given fitted the same way.
- `tools/test_sound_audit.py`: the manifest, the exports, the imports, the
  notice on both images.

## Upstream, and the monthly scan

Canonical repository: `https://gitlab.xiph.org/xiph/speexdsp` (mirrored at
`github.com/xiph/speexdsp`). Releases: `https://downloads.xiph.org/releases/speex/`.

**Watch the `main` branch.** There has been no release since 1.2.1, but
`main` is active (137 commits past the tag when this was written,
2026-11-13). `master` is an older line, fifteen commits past the tag.

What was checked on 2026-11-13, so the next scan starts from here:

- Between the tag and `main`, `resample.c` changes by 34 lines: two 2023
  commits that name local variables (`bd9162c`, `70f2cf7`), a comment's
  spelling, and an include and a field for RISC-V run-time dispatch. **No
  fix to the resampler's C code.** `arch.h` and the two headers are
  unchanged.
- `main`'s `resample.c` includes a new `cpu_support.h` and no longer builds
  alone under `OUTSIDE_SPEEX`; taking `main` means taking that file too.
- The branches whose names suggest fixes do not apply:
  `bugfix/resampler-uint` (2018) is inside 1.2.1; `fix-issue-12` touches
  `smallft.c`, an FFT that is not vendored; `bugfix/libspeexdsp.def` is a
  Windows export list. The `rvv-*`, `resample-sse-accumulator`,
  `check_asm` and `tighten-int16-word2int-gate` branches are speed work for
  the hand-written RISC-V, SSE and NEON loops, and tests for them.
- If resampling ever shows in a profile, the commit to look at is
  `3ad2acb` on `main` ("resample_sse: use 2 accumulators"), with
  `resample_sse.h`.

A bug here reaches wrong sound, and, since a page's audio will pass
through it, memory the resampler indexes with lengths it is given. Its
inputs from os64 are a channel count (1 to 8, checked in the wrapper), two
rates (non-zero, checked), and buffer lengths the caller owns.

**For `THIRD_PARTY.md`** (the registry lives on the `fable/modern-web`
branch and has not reached `userland`; this is its row when it does):

| Component | Pinned at | Where in the tree | What it is for | What a bug there reaches | Upstream, and where fixes are announced |
|---|---|---|---|---|---|
| **speexdsp** (resampler only) | 1.2.1 (tag SpeexDSP-1.2.1, 2022-06-13), unpatched | `userland/libsound/upstream/`, `manifest.json` | sample-rate conversion for `play`, and later a page's sound | wrong sound; an out-of-bounds read or write in the player's process if a length is mishandled | https://gitlab.xiph.org/xiph/speexdsp (`main` branch; no release since the pin), CVE search "speexdsp" |

## Licence

Revised BSD (three-clause), `upstream/COPYING`. The same bytes are
`license/speexdsp-LICENSE` in this repository and
`/etc/licenses/speexdsp.txt` on both disk images, which the audit checks.
