# GIF animation validation — 2026-09-25

Branch: `codex/gif-animation`, based on first-picture GIF commit `f25c1789`.
Author host/QEMU results and Chris's P5 observations are distinguished below.
Independent review of the animation slice has not taken place.

## Host and build

- `python3 tools/test_gif_sequence_host.py --output /tmp/os64-gif-sequence --real /tmp/os64-hayabusa2.gif`:
  **30 cases passed** under ASan/UBSan: 23 opened sequences (22 GIF and one
  PPM), plus seven refusals. Exact reference canvases cover disposal 0–3,
  local palettes, interlace, transparency, loop reset, finite/infinite loops,
  delays, rewind and singleton behavior. The later-corrupt-raster case checks
  that an advance preserves the previous picture and metadata on failure.
  The harness also checks copied-input ownership, each allocation failure,
  no allocations during playback/rewind, 2,988 truncated prefixes and 2,816
  deterministic mutations. [Output](host.txt).
- The original first-picture corpus passed **122 fixtures** after the shared
  parser refactor. [First-picture output](first-picture-host.txt).
  JPEG reference cases and adjacent image/drawing regressions
  passed **174 checks, zero failures**. [JPEG/image output](jpeg-host.txt).
- `make -j8` passed the strict kernel/userland/ISO build.
  [Built artifact hashes](build-sha256.txt). The library extracted from the
  tested guest root matches the final `libimage.so` byte-for-byte.
- `git diff --check` passed. `tools/stale_refs.sh` reported `BI_BITFIELDS` and
  `BI_RGB` because an edited DEBTS.md table row also contains those BMP names.
  Both remain valid format names in code, tests and documentation; neither
  was retired by this change.

LeakSanitizer is disabled for the host environment. ASan/UBSan remain enabled;
the GIF harness separately tracks live allocations and forced failures.

The supplied [Hayabusa2 animation](https://upload.wikimedia.org/wikipedia/commons/9/96/.Animation_of_Hayabusa2_orbit.gif)
contains 416 frames. Every composed frame matched the Pillow reference over
two passes, with **9,786,191 bytes** of owned decoder storage, including the
compressed input. Encoded delays are 50/1000 ms, totaling 22.7 seconds per
pass. Slow decoding or presentation can lengthen actual playback. Source
file SHA-256:

```
90fe10ffec287da0abcd14b2ad7947f8d0a125f5eb0d2f4ca5193bc01bd348f6
```

## QEMU

Q35, eight virtual CPUs, e1000 user networking, GUI on a writable ext2 root,
and private copies of the root/home disks. A private ISO adds `GUI BACKSTOP=10`
to the first boot entry. No tracked boot configuration changes are included.

- `/tests/giftest` returned **0** through the shared library, checking exact
  composition, disposal, loops, rewind, file loading and heap cleanup.
  [Guest output](guest.txt).
- Hayabusa2 visibly advances: [first capture](flight-1.png) and
  [capture two seconds later](flight-2.png).
- Space pauses: [paused capture](pause-1.png) and
  [two seconds later](pause-2.png) are byte-identical. R restarts playback.
- A three-frame, two-pass fixture holds its [final frame](finite-1.png);
  R returns to the [first frame](finite-restart.png).
- A frame with a 650-second encoded delay remained responsive to Q: the
  window was absent in the capture taken about 0.21 seconds after input.
  This is an observation window, not an exact input-latency measurement.
- Transparent canvas areas show the [neutral gray mat](transparent.png).
- Switching to VT1 preserves the frame across 2.3 seconds away:
  [before](hide-check-before.png), [after](hide-check-after.png).
  A separate return-to-GUI check showed [playback advancing again](hide-resumed.png).
- Still [PNG](still-png.png) and [JPEG](still-jpeg.png) images continue to open.
- Resize while playing preserves the centered image and gray mat in an
  [enlarged 440x280 canvas](resize-large.png), and clips the animation in a
  [smaller 120x70 canvas](resize-small.png).

The initial automated drag did not produce a resize. Explicit QMP
`input-send-event` modifier presses/releases around Ctrl+Alt+right-drag
completed the check; timed `sendkey` attempts were inconclusive. A repeat
boot also exposed interleaved serial output that defeated the temporary
helper's readiness match; correcting the helper allowed the guest checks to
continue. Neither issue required product code changes.

After `sync`, shutdown and confirmation that QEMU stopped, both extracted
ext2 partitions passed read-only `e2fsck -fn`: [root](fsck-root.txt) and
[home](fsck-home.txt). These checks used the private test disks.

Screenshots establish the observed viewer behavior. Exact all-frame pixel
comparisons and memory accounting above were host tests, not P5 measurements.

## P5

Chris installed the animation build and reported successful playback of
around ten GIFs, including a roughly ten-second Minions animation and Homer
backing into the bushes. These are user-reported real-hardware playback
observations. The files were not collected as a reference corpus, and these
reports do not establish exact frame timing or memory measurements on the P5.
