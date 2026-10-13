# E2 — Shared icon vocabulary and first asset family

## Status and ownership

- **State:** Ready for review in `codex/explorer-e2`, 2026-10-13, based on
  E1C merge `faa321a5`. Host, asset, FAT/ext2 packaging and QEMU checks pass;
  [E2 evidence](evidence/e02/README.md) records their scope. Installed P5
  acceptance remains pending.
- **Design owner:** Quinn. Chris approved the Roam Classic visual direction and
  original image generation.
- **Prerequisite:** E1's optional icon-provider contract is available.
- **Fable review:** not required for this userland asset/provider design. A
  kernel change discovered during implementation must be scoped separately.
- **Requirements:** D002/D010/D011/D016 in [DECISIONS.md](DECISIONS.md).

## Outcome and scope

Deliver an initial coherent icon family and reusable named-icon support for
menus, toolbars, file views and later desktop icons. Chris states there is no
established icon set to supply this work; asset creation is a deliverable, not
an assumed dependency. Prove larger icon use in the gallery without implementing
desktop icon placement, launch policy, drag/drop or persistence.

Icons identify commands and objects; checkmarks/radio dots/cascade arrows remain
widget marks. File thumbnails are a different producer with different lifetime
and I/O requirements, owned by E14. An icon name does not inspect a file, choose
an application or claim a filesystem object is of a particular type. E4/E5
choose names from metadata and associations.

The logical packets are **E2A provider/cache/rendering** and **E2B visual family,
packaging and consumer integration**. They can share a PR when coherent; neither
is complete without its own evidence. No new shared library is needed solely
to avoid making a decision about the existing dependency graph.

## Dependency and ownership boundary

At the inspected baseline, libui is part of `libos64.so`. `libimage.so` already
depends on libos64, libpng and libjpeg. Therefore adding a libos64 call to
`os64_image_load` would create a circular dependency.

Place the small provider interface and pixel drawing helper in libos64, in
`os64/ui_icon.h`/`ui_icon.c`. Put the file-backed catalog/cache implementation
in `userland/libimage/icons.c`, exported through `image/icons.h`. Applications
that want file-backed icons link libimage and supply its provider. The widget
library calls function pointers and continues to work with NULL provider.
libimage can decode the tightly limited icon PNGs with its existing libpng
dependency; do not use the broad default image cap for tiny interface assets.

| Object | Ownership and lifetime |
| --- | --- |
| Semantic icon key | Copied with an action/model descriptor. No retained filename pointer. |
| Provider | App-owned, one per UI-thread icon catalog; outlives its widgets and leases. No process-global active-provider singleton. |
| Icon lease | Pins an immutable pixel view at the selected size/catalog generation. Explicit acquire/release; valid until released, including across unrelated cache misses. |
| Widget's slot | Owns its lease and releases it on replacement/teardown. Geometry is independent of cache-entry address. |
| Catalog cache | Owns decoded pixels and bounded lookup records; evicts unleased entries only. Destroy returns BUSY if leases remain. |

The provider API exposes `acquire(user, key, size, out_lease)` and
`release(user, lease)`. A lease contains immutable pixels, width/height/pitch,
catalog generation and an opaque release cookie; no caller frees its pixels.
Acquisition reports OK/MISSING/INVALID/LIMIT/NO_MEMORY/IO/MALFORMED. Failure
clears the output lease and preserves other leases. NULL provider behaves as
MISSING. A provider may be implemented with generated/in-memory pixels by a
host test or future application; widgets do not require a filesystem catalog.

Acquire during initialization/model preparation, not paint or hit testing.
The initial small catalog uses bounded synchronous file reads during explicit
preparation. Preload the actions needed by a menu before exposing it; show a
text-only menu on asset failure. Do not load arbitrary directory icons during
a row repaint. E4/E14 may add asynchronous preparation for many/custom sources
behind this interface without making a widget call into a worker thread.

Calls on one provider are serialized by the UI thread. A future worker needs
its own provider or explicit synchronization; this API does not imply a
thread-safe global cache. Cache entries retain no pointers into the user's
request buffer. Catalog replacement creates a new provider; bind new leases
transactionally and release the old provider after its consumers detach.

## Keys, sizes and lookup

Use semantic keys such as `folder`, `file`, `drive`, `app`, `go-back`, `go-up`,
`document-save` and `edit-copy`. Keys are case-sensitive ASCII, 1–47 bytes,
letters/digits/hyphen with an initial letter. Slashes, dots, whitespace and
control characters are invalid. Do not concatenate an unvalidated filename or
extension into an asset path.

Export sizes: **16, 24, 32, 48 and 64 pixels square**.
Menu slot size uses the smallest bucket at least as large as the Interface
line height, capped at 32 px; font heights beyond that enlarge the row without
forcing a giant menu icon. Toolbars use an explicitly chosen bucket; the
gallery demonstrates 48/64 px labels-plus-icons for future desktop use.

Exact-size artwork is preferred. If that variant is absent, try smaller
variants in descending order and center the returned raster in the requested
slot without upscaling. Do not stretch a 16 px image into a blurry desktop
icon. If no usable variant exists, return MISSING; generic fallback is handled
as described below. The initial shipped family supplies every listed size.
Runtime raster scaling is not used; the build tool resamples the source masters.

The shipped set is installed at `/etc/icons/default/`. Each set is an extracted
folder containing `theme.conf` (`format = 1`, `name = ...`, `author = ...`) and
fixed `<key>-<size>.png` filenames. The resolver finds `icons.conf` on the
standard config ladder; `set = /home/icons/roam-classic` selects an absolute
folder path. It rejects a selected folder with a missing or invalid manifest
and uses the shipped default. A selected set that lacks a requested icon uses
the default set for that icon. Other selected-set errors retain their typed
status, including malformed PNG and I/O failure. No manifest path mapping,
GUI chooser or live switch is provided. A new application instance reads the
current selection; an existing instance keeps its catalog and leases until
teardown. Archives are extracted before selection.

The existing open/stat ABI cannot distinguish every failed lookup from absence.
After an open failure, successful stat produces retryable IO; failed stat maps
to MISSING and may use default artwork. This limitation is confined to lookup
failures; decode errors and failures after a successful open retain typed status.

## Resource limits and fallback behavior

Each catalog has a 2 MiB retained pixel budget, 256 positive or negative lookup
records, and a 64 KiB catalog metadata bound. The selected/default resolver may
hold two catalogs: at most 4 MiB retained pixels and 128 KiB catalog metadata.
The resolver's object and live lease-routing records have a separate 64 KiB
bound, charged using actual heap block capacity and allocator headers. Live
route count is limited by that byte budget and a compile-time maximum below
1024. Both leased and unleased decoded pixels count toward the pixel budget.
A fully leased cache returns LIMIT rather than invalidating a visible icon.
Checked dimensions, strides, byte counts and allocator overhead are included
in accounting; these limits are ceilings, not preallocations.

For each file: encoded size at most 64 KiB; PNG only; dimensions must match the
selected source-size bucket; decoded pixel cap at most 64 × 64. Verify the
header dimensions before decoding and pass the explicit pixel cap to
`os64_png_decode`. Validate dimensions again after decode. Inspection of
`libpng/png.c` shows one output plane, two filtered row buffers and one streaming
inflater; compressed IDAT chunks are fed from the encoded file without a second
concatenated copy. At 64 × 64 and the largest supported 16-bit RGBA rows, the
requested heap payload is bounded by 65,536 encoded bytes + 16,384 output bytes
+ 1,026 row bytes + the inflater. A compile-only host size probe at this baseline
measured `sizeof(struct os64_inflate)` as 35,016 bytes: 117,962 bytes total,
before allocator headers/alignment and caller bookkeeping. Assert the same
budget with allocation instrumentation in the implementation harness; do not
claim the 128 KiB temporary-load target covers unmeasured allocator overhead.
Allow one load at a time per provider. Charge the decoded plane to retained
storage before installing it; it is not double-counted as a second pixel copy.

Evict unleased least-recently-used entries to fit a load. If load fails, keep
existing leased entries and return a typed error. Negative-cache missing/bad
assets within the same bounded record budget to avoid rereading them on every
menu open. Transient IO/NO_MEMORY/LIMIT failures are not permanent negative cache
entries. A new provider generation or explicit cache refresh permits retry
after asset installation. Rate-limit diagnostics by key/generation.

Consumers choose fallback appropriate to their presentation:

- Menus/toolbars with labels: retain the empty reserved slot and label.
- Files/folders/applications: request the matching generic key; if the asset
  family itself is unavailable, use a simple built-in geometric placeholder
  plus the text label. Do not make the recovery path require loading another
  PNG. The placeholder is not the final artistic family.
- Unknown key: report missing; do not accidentally open a similarly named file.

The draw helper accepts a lease and slot rectangle, centers it, clips to that
rectangle and the destination surface, and uses source-over straight-alpha
composition. Reuse `os64_draw_blend` for the normal path. It allocates nothing.
Selected state changes the widget background/label treatment, not the identity
of the icon. Disabled state reduces alpha to half with defined integer rounding;
use a bounded row/span implementation or a tested compositing extension, never
mutate cached shared pixels. Test the same lease drawn normally and disabled
in adjacent controls. Bad or missing artwork must not erase the command label.

## Initial asset inventory and visual review

Chris selected Roam Classic, an original generated raster family. The 26-key
inventory below is being exported at five sizes, targeting 130 transparent PNG
files. The 16/24/32/48/64 px files are derived from generated 256 px raster masters;
they are not independently hand-drawn small variants. The contact sheet must
show the shipped pixels at native size on light, dark and selected backgrounds.

| Family | Initial semantic keys |
| --- | --- |
| Objects and places | `folder`, `file`, `drive`, `app`, `home` |
| Navigation | `go-back`, `go-forward`, `go-up`, `view-refresh` |
| Documents | `document-open`, `document-save`, `document-save-as`, `folder-new` |
| Editing | `edit-cut`, `edit-copy`, `edit-paste`, `edit-delete` |
| Information | `search`, `help`, `settings` |
| Views | `view-details`, `view-icons`, `view-preview`, `view-split`, `tab-new`, `window-close` |

Specialized application branding and richer file-type families can be
added with real consumers. A generic app icon is preferable to inventing a
misleading logo for each existing program. No icon means that an unavailable
operation has suddenly become supported.

Store installed PNGs and `theme.conf` in `assets/icons/default/`, 256 px masters
and `generation.json` in `assets/icons/sources/default/`, and export records in
`assets/icons/default/provenance.json`. The masters were made with OpenAI's
built-in image generation, without external artwork. The shipped files carry
the repository's MIT license to the extent rights allow; this is no promise of
exclusive rights to generated material. Export transformations and validation
are recorded in provenance. See [icon set installation](../../../../assets/icons/README.md).

## Consumer and packaging integration

E1 menu models already carry optional names and reserve a separate icon column.
Wire the provider to the menu gallery, grootmenu and Scribe. For grootmenu,
derive known application icons from the parsed executable argv, never from a
substring of raw command text; otherwise use `app`. Keep grammar/config behavior
unchanged. Future application associations can own a richer mapping in E5.

Add the implementation to `userland/libimage/shared.mk` and its public exports
to `userland/libimage/exports.map`. Add libimage to the actual consumer link
lists in the build. Keep libos64 independent of image decoding. Update both
FAT and ext2 image assembly and their prerequisites so a changed PNG rebuilds
the image; verify the installed files rather than just the host directory.

The gallery's larger-icon page uses the same provider with captions and
selection backgrounds; it demonstrates the desktop-facing contract. It is not
a desktop implementation or an excuse to modify the wallpaper application.

## Acceptance and evidence

E1B tested the in-memory provider boundary and drawing helper with synthetic
pixels. E2 host tests, asset checks, FAT/ext2 packaging and QEMU checks pass;
P5 acceptance is pending. Results and limitations are in
[evidence/e02/README.md](evidence/e02/README.md). Required E2 evidence:

| Environment | Check |
| --- | --- |
| Host provider | Valid PNGs, missing exact size with smaller fallback, fully missing family, path rejection, malformed/truncated files, wrong dimensions, encoded/decoded limits, allocation failure, stale generation, BUSY destroy and lease lifetime across eviction. |
| Host renderer | Straight-alpha edges on light/dark backgrounds, clipped/negative bounds, checked+icon menu row, disabled rendering without cache mutation, no out-of-slot writes, no paint-time allocation/I/O. |
| Host resource accounting | Cold load vs cache hit, retained and temporary peaks, record cap with many unknown keys, all entries leased, transient retry and teardown to baseline under ASan/UBSan/LSan. |
| Artifact validation | Manifest covers each key/size, PNG signature/dimensions/alpha, provenance records, contact sheet uses shipped pixels at native sizes. |
| QEMU | Packaged assets in FAT/ext2 images, menus and Scribe with/without asset directory, gallery at different Interface fonts and 48/64 px sizes; repeated open/close without retained-growth. |
| P5 / Chris | Chosen family is readable at menu sizes and pleasant at desktop sizes; selection/disabled states and light/dark appearance accepted at the installed revision. |

Verify decoder scratch/allocator accounting before finalizing E2A. Do not claim
a total memory bound from a cache limit alone. Asset-file caps and decoded pixel
caps are separate acceptance assertions.

## Handoff

The provider and resolver are implemented in libimage over the E1 interface in
libos64. E2A and E2B share this PR, with separate host/provider and artwork/native
evidence. Next: ordinary review and Chris's P5 walkthrough. The installable-set
archive can be rebuilt with `tools/build_icon_assets.py --archive PATH`; see
[installation instructions](../../../../assets/icons/README.md). Native archive
extraction, folder selection and partial-set fallback are verified.
