# Pictures kept at the size they are drawn

Opus, 2026-10-31. Pays the DEBTS row "A picture is kept at its own size, not
the size it is drawn" (Browser layout). Userland only (yonder), no kernel.

## The problem

A raster picture is decoded once, at its own size, and kept that way for the
life of the page. A 3840x2160 wallpaper drawn as a 400x225 thumbnail keeps
33 MB of pixels to draw 360 KB of them, and every page's pictures share
`PICTURES_KEPT_MAX`. On a photo site's search page 13 of 42 pictures were
empty frames under the old 256 MiB cap; the cap was raised, the waste stayed.

## Two sizes, named apart

Today `picture_pixels()` returns the grid's dimensions and every caller reads
them as the picture's NATURAL size: libflow's `replaced_size`, `object-fit`
(`yonder_object_rect`), background tiles (`yonder_background_tile`), the
attribute tiler. Once the grid can be smaller than the picture, those are two
facts:

- **natural size**: the picture's own, in CSS pixels. Layout reads only this.
  `Picture` and the worker's product carry it as their own fields.
- **kept size**: the pixel grid yonder holds. The painters read only this;
  `yonder_draw_picture` and `yonder_tile_picture` already scale any grid into
  any rectangle, so a smaller grid needs no change there.

Shrinking a picture therefore never moves the page. Nothing that lays out
reads the grid.

## The needed size

After every layout, the walk that already visits every box for sheet pictures
(`css_pictures_under`) also asks each shown still raster how big it is drawn;
and once more after pictures arrive that move nothing (a background never
does), since no layout follows them (`pictures_judge_owed`, run by
`pictures_settle`, once however many arrived). Its need is the largest, over
all its uses, of

- an `img`: the rectangle `object-fit` places it in, in device pixels at the
  laid zoom (the whole rectangle, cropped or not: `cover` draws past its box);
- a sheet layer: its tile, from `yonder_background_tile`;
- a `background` attribute: its natural size at the zoom (its tile).

The needed size is the natural size scaled by one factor, the largest either
axis asks for, held to 1: a picture is never kept bigger than it is, and its
shape never changes. A picture with no use in the layout (every use
`display: none`) keeps what it has. A use drawn `image-rendering: pixelated`
(or `crisp-edges`) needs the picture whole: the painter keeps its pixels hard,
and averaging would already have mixed them.

## When it changes

- **Shrink** when the kept grid is at least twice the needed one in area.
  Hysteresis, so a window dragged a few pixels narrower does not re-decode
  every picture on the page.
- **Grow** as soon as the needed size passes the kept one on either axis
  (zoom in, a wider window, a page that enlarges a picture). Until the
  bigger copy lands the small one is drawn scaled up: soft for a moment,
  never blank.

## How: one job, a target size

There is no separate shrink. The picture job gets an optional target (a
device-pixel width and height). The worker decodes as today, and when a target
is set and the picture is bigger, it reduces the grid to the target by
averaging every source pixel under each output pixel (alpha-weighted, as
`smooth_at` weighs), then frees the full one before returning. The full grid
lives only inside the worker, under `PICTURE_RESERVE`, which already admits
libimage's largest decode. Averaging is also better looking than what the
painter does now: `smooth_at` mixes four neighbours, which at a tenth of the
size reads four pixels of every hundred and shimmers.

- **First request:** a box that layout has already fixed
  (`flow_replaced_fixed`, so it does not wait on the picture) with
  `object-fit` fill, contain or cover is asked for at the size that covers
  that box, which is at least what any of the three draws, unless it is
  drawn pixelated. Everything else is asked for whole, as today.
- **Later requests** (a resample) are the same job again, with the needed
  size as the target, which the worker takes exactly: it is a kept size
  already, and worked out a second time its rounding up can answer a size
  still twice the need, asked for again forever (7x3 drawn 2x1: need 3x1,
  worked out again 3x2). The bytes come from where they came the first time:
  yonder's cache for `http:`/`https:` (a miss fetches again), the file for
  `file:`, the job's own copy for `data:`.
- **Arrival** of a resample swaps the grid and corrects `kept_bytes` by the
  difference. A resample that fails leaves the picture as it was: a worse
  copy is never traded for none. One that fails, does not fit, or comes back
  no different from what is kept is not asked for again until the need
  changes. An `--exit-when-loaded` run waits for resamples too.

## A picture's life

| State | Grid held | Leaves when | To |
|---|---|---|---|
| QUEUED | none | fed to the pool, with a target when its box is fixed | WAITING |
| WAITING | none (the worker's) | arrives | SHOWN, FAILED, or NOT_KEPT |
| SHOWN | the kept size (whole, or a target's) | the walk (after a layout, or after arrivals that move nothing) needs it at least 2x smaller, or bigger | SHOWN + resample WAITING |
| SHOWN + resample WAITING | the old grid, still drawn | the resample lands (swapped) or fails (kept) | SHOWN |
| NOT_KEPT | none, but its natural size is kept | the walk needs it at a size the budget has room for | SHOWN + resample WAITING (drawn as its frame until then) |
| FAILED | none | never | |

A page left cancels every job of its own, resamples included (the existing
generation check). One resample per picture at a time: a need that changes
while one is out is looked at again when it lands. Resamples share the pool
with first requests under `PICTURES_AT_ONCE`, first requests first.

`NOT_KEPT` changes meaning slightly for the better: a picture refused at full
size because the page's pictures are past their memory is asked for again at
the size it is drawn, which is usually a hundredth of it. On the photo page
above, that is the 13 empty frames.

## Left out

- **Moving GIFs** keep their sequence: their frames are decoded as they play,
  and shrinking a sequence is libimage's business. Still GIFs are rasters
  and are covered.
- **SVG** is drawn from its document at whatever size is asked.
- **Decoding small in the first place.** JPEG can decode at 1/2, 1/4 or 1/8
  of its size for a fraction of the work (the DCT scaled); libjpeg here does
  not offer it. Booked as a DEBTS row: the slice's memory win does not need
  it, only its decode time.
- **Canvas** (`canvas-integration`): `drawImage` and `getImageData` read a
  picture's pixels. When Canvas merges, a Canvas source must ask for the
  natural size (a target of none), or read a separately decoded copy. Noted
  here so the merge finds it.

## The record

A line of its own beside the pictures line, so what reads that line reads it
unchanged, says how many are kept smaller than they are and what that
spared, written once there are some and from then on (saying none when a
zoom has grown them all back whole): `picture sizes: 4 kept at the size
they are drawn, 66485 KiB less than whole`. A resample that failed
is a plain fact line, not a FAILED picture: the picture is still shown.

## Tests

- **Host, `scale.c`:** the reduction against a reference average (odd sizes,
  1-pixel results, alpha edges, a 1xN strip), under ASan.
- **Host, the scripted window:** natural size survives a shrink (layout
  identical before and after); a fixed box asks with a target and an unfixed
  one without; shrink at 2x and not just under it; a grow on zoom; a resample in
  flight when the page is left; a failed resample keeps the old grid;
  NOT_KEPT recovered at its drawn size; every allocation in the resample
  path cut through the real release.
- **Mutants** on each rule above: `tools/test_yonder_picture_sizes_mutants.py`.
- **QEMU and the P5:** `/tests/pages/picture-sizes.html`, large photographs
  drawn as thumbnails and one wide, with the page's record; zoom in and out
  on it.
