# C6a — shared coverage blur and target checkpoint

Baseline: `0221d535`, the integration merge of pointer events PR #450.

## Implementation

`draw2d_blur_mask16` in `libdraw2d` owns the numeric operation extracted from
Yonder's shadow painter. Its uint16 scalar mask does not assume a particular
coverage normalization: Yonder preserves its 0–256 samples, while another
caller can use 0–65535. Three horizontal/vertical box passes retain the
existing integer rounding. The caller supplies disjoint scratch for the
longest row/column and admits a pixel budget; invalid storage and insufficient
scratch/budget refuse before modifying the mask. Padding is preserved, and
the final row needs no trailing padding. Radius zero needs no scratch.

The initial sum counts exterior samples analytically. Even `UINT32_MAX`
radius takes work proportional to the mask area; sums and divisors use
64-bit arithmetic. Yonder still chooses blur/2 as its box radius and owns
mask generation, allocation, cache tiles, clipping and paint emission.

This is preparation for Canvas shadows. It does not introduce shadow
properties, alter the frame/input interfaces or change Canvas draw semantics.

## Verification

- `tools/test_canvas_core.sh`: 170,761 assertions pass, including the new
  direct-convolution oracle for widths/heights 1–7, radii 1–9 and exterior
  values 0, 256 and 65535. Cases cover non-square masks, kernels larger than
  masks, padded rows and exact final-row capacity. Separate checks cover
  refusal without writes, aliasing/alignment, zero radius, empty masks and
  the maximum integer radius.
- `tools/test_yonder_host.sh`: 2,073 checks pass both before and after the
  extraction; all six corpus paints match. Existing checks cover blurred
  outer/inset shadows, cached/uncached rendering, split repaints and scratch
  allocation behavior.
- Native Yonder and its shared libraries build successfully.
- `tools/test_yonder_scripts_host.sh`: 16,539 complete browser checks pass,
  including image-worker allocation/cancellation and rebuild-refusal cases.

Sanitizer runs retain ASan, UBSan and LeakSanitizer. No new P5 acceptance is
claimed for this extraction; the existing CSS-shadow pixel checks are its
behavior-preservation gate.

## Signature diagnostic

The public `signature_wall.js` fetched on 2026-11-01 matches the saved
2026-10-29 inventory byte for byte: SHA-256
`2b6247abd711b1fd00bdf9eb813fa093bcc32aed6b98c1bf312da08b8e97a921`.
The source remains unmodified in a private local replay with the captured
homepage controls. External stylesheets, other scripts and markup images are
omitted; picture workers refuse images, and requests stay local to the host
fixture. These restrictions isolate editor behavior and are not substitutes
for full-site initialization, layout, fetching or rendering.

The actual script opens its editor, draws a 23-pixel stroke through native
pointer input, undoes to a blank bitmap, redoes to the previous ink count,
erases and clears. The diagnostic passes 71 checks and releases its tracked
allocations under LeakSanitizer. No signature is submitted to the public wall.

The wall's image loading and shadow painting remain unverified end to end.
The saved CD-ROM scratch module constructs Path2D from SVG path strings;
this dependency was absent from the initial named-context-member scan and
must be implemented before claiming that ticket works. Floppy/CRT and the
surrounding wallet/modal flow still need executed target acceptance, as does
the chosen cursor effect. P5 acceptance remains the final C6 gate.
