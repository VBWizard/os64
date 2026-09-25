# GIF validation — 2026-09-25

Branch: `codex/gif`, based on `6d6937f1`. Author host/QEMU tests and Chris's
P5 observation are distinguished below. Independent review is not claimed.

## Host and build

- `python3 tools/test_gif_host.py --output /tmp/os64-gif-host`: **122 fixtures
  passed** under ASan/UBSan. The 93 successful fixtures exercised **125,759
  truncated prefixes**, **37,200 deterministic mutations**, and failure of each
  output/scratch allocation. Refusal fixtures checked malformed data,
  unsupported inherited palettes, and limits before allocation.
  [Full output](host.txt).
- `ASAN_OPTIONS=detect_leaks=0 python3 tools/test_jpeg_host.py`: JPEG reference
  cases passed; adjacent image/drawing tests passed **174 checks, 0 failures**.
  [Full output](jpeg-host.txt). LeakSanitizer is disabled for the host sandbox;
  the GIF harness independently tracks live allocations.
- `make -j8`: strict kernel/userland/ISO build passed. `readelf -Ws` confirms
  `image_decode_gif` is local and the public image exports remain unchanged.
- `git diff --check` and `tools/stale_refs.sh`: checked after the final edits.

## Guest

QEMU q35, 8 virtual CPUs, e1000 user networking, GUI with a writable ext2 root
and private copies of the root/home disk images. A private ISO changed only
the first boot entry to add `GUI BACKSTOP=10`; no boot configuration changes
are part of this feature. Named-pipe QMP helpers came from the main checkout.

`/tests/giftest` returned **0** and printed [this result](guest.txt), covering
exact pixels, interlace, transparency, local palettes, offsets, KwKwK,
truncation, file loading and heap cleanup. `/tests/jpegtest` returned its
success marker **0x90650000** through the updated shared library.

The first guest attempt selected the existing FAT-root GUI entry. Pixel
checks passed, but giftest returned 3 when opening its temporary file on that
root. The writable-ext2 run above passed the complete test. The stock VM
helper's text-boot readiness marker also did not cover this GUI boot; guest
command results and the screenshot are the runtime evidence.

The guest ran:

```
os64get http://textfiles.com/images/textfile.gif /home/textfile.gif
gview /home/textfile.gif
```

The [download log](download.txt) reports 3,711 bytes and return code 0. The
391×68 image displayed in [gview](gview.png). Guest and independently downloaded
host files compared byte-for-byte, SHA-256:

```
636fe37309c83a580a966d771e832b24dbd561646de979abff786e7d13a881e5
```

The real file also passed the host Pillow reference comparison, every
truncated prefix, allocation failures and mutations. It is additional to the
122 generated fixtures above. Source: [textfiles.com logo](http://textfiles.com/images/textfile.gif).

After guest `sync`/shutdown and confirming QEMU was stopped, both extracted
ext2 partitions passed read-only `e2fsck -fn`:
[root](fsck-root.txt), [home](fsck-home.txt). No shared disk image was used by
the guest.

Transparency is established by pixel tests. This screenshot uses the current
opaque-copy drawing path and does not prove source-over compositing.

## P5 and the Hayabusa2 animation

Chris reported that gview displayed his downloaded GIF on the P5, with no
animation. That is the first-frame behavior this slice implements.
Read-only inspection found `/tmp/1/Animation1.gif`; its 7,262,063 bytes matched
an independent download from the supplied
[Wikimedia URL](https://upload.wikimedia.org/wikipedia/commons/9/96/.Animation_of_Hayabusa2_orbit.gif).
SHA-256:

```
90fe10ffec287da0abcd14b2ad7947f8d0a125f5eb0d2f4ca5193bc01bd348f6
```

Pillow identified a 560×420 image with 416 frames, indefinite looping,
50/1000 ms frame delays, 22.7 seconds total duration, and disposal modes 1
and 3. The production decoder's first frame matched Pillow exactly under
ASan/UBSan. Forced allocation failures, 400 mutations, and 444 sampled
truncated prefixes passed using a temporary copy of the host driver whose
prefix loop advances by 16,381 bytes. This large-file check sampled prefixes;
it did not exhaust all 7.26 million truncation positions.

The P5 observation establishes display behavior reported by Chris. The
reference-pixel and sanitizer checks ran on the host, and the byte comparison
establishes file identity, not deployed-library identity.
