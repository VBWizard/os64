# Frame Studio

Window-decoration design, 2026-09-21. Chris accepted the design direction in the
Frame Studio discussion, with access through Control Center alongside Appearance
Workshop. Typography, standard controls, the native editor and built-in
finishes, named self-contained Save/Load and startup selection are implemented on
`codex/frame-studio`, rebased onto `userland` at `be2d755` (vncd and ZRLE).
Saved composition deletion and opening the active saved composition are implemented. Independent
button-symbol colors are implemented; their contract is in
`FRAME_STUDIO_SYMBOL_COLORS.md`. Imported textures and rounded silhouettes
remain later work. The broader design below is not a claim that those stages
are implemented.

## Review and publication

The feature is split into a review stack: `codex/shared-text-profile` contains
shared decoding/data with no kernel implementation changes; `codex/ui-editing`
adds reusable widget editing with no kernel changes; `codex/frame-studio` adds
the decoration engine, its userland consumers and the editor together. See
[publication validation](docs/frame-studio/publication-checkpoint.md).
The subsequent review corrections and validation are recorded in the
[review checkpoint](docs/frame-studio/review-checkpoint.md).

Per-slice checkpoints below are historical evidence. Their pending-work and
hardware/publication statements describe the point when each slice was tested;
Current completion order and the publication checkpoint supersede those status
statements. Test success and user hardware acceptance do not claim review or merge.

## Current completion order

1. **Startup decoration (implemented):** Use at startup for a saved or
   unchanged loaded composition stores an independent, validated,
   self-contained startup snapshot. Replacement is atomic; selection changes
   the next startup, not the current session. The desktop loads it before configured applications,
   using generation-zero publication so a concurrent explicit Apply wins.
   Missing or corrupt assets retain usable compiled defaults and log the
   outcome. Restore default startup writes an explicit built-in choice.
2. **Delete saved compositions (implemented):** Delete selected in Saved is
   disabled with no selection. It confirms the selected entry by name; the editable Save name
   field does not determine the deletion target. Cancellation or a failed
   deletion keeps the entry and draft. Success refreshes the collection and
   preserves the live session, loaded draft, and independently stored startup
   snapshot. If the removed entry was the draft's last saved baseline, it marks
   the retained draft unsaved so closing cannot silently discard its remaining
   editable copy. Deletion is not part of draft Undo; the confirmation says
   it cannot be undone. Damaged saved files can also be removed by name.
3. **Open active composition (implemented):** Studio compares the live prepared
   fingerprint with its sorted saved collection and opens the first matching
   entry as a clean draft, with its name and list selection. No Apply occurs.
   Missing, damaged, or changed files do not match; a changing session or
   unavailable fingerprint retains the normal initial draft.
4. **Independent button-symbol colors (implemented):** each control has active
   and inactive ink that follows title text or uses its own opaque color.
   See `FRAME_STUDIO_SYMBOL_COLORS.md` and [verification](docs/frame-studio/symbol-checkpoint.md).

Startup, deletion, active matching and independent symbol colors are implemented.
Persistence acceptance includes cold-boot restoration without original font sources, refusal of a
failed startup replacement, confirmed/cancelled/failed deletion, preservation
of the live decoration and startup snapshot, and saved-baseline close handling.
Appearance Workshop deletion remains a separate task; this milestone applies
to Frame Studio's collection. See [deletion evidence](docs/frame-studio/deletion-checkpoint.md).

## Opening the active saved composition

The status text keeps its existing `generation = <decimal>\n` prefix and appends
`fingerprint = fnv1a64-v1:<16 lowercase hex digits>:<8 lowercase hex digits>\n`.
The final field is the prepared bundle's byte length. The kernel computes FNV-1a-64
on validated private staging outside the GUI lock and publishes the hash and
length with the successful generation change. Failed commits retain the previous
status. Built-in decoration reports generation, fingerprint and length zero.
Generation-only readers remain compatible; the new reader accepts legacy status
as having no fingerprint.

Studio scans the existing bounded, name-sorted collection, validating each file
through the normal loader and comparing its prepared bytes' hash and length.
A second status read must agree before accepting the candidate. The checked
generation is retained for the next Apply's compare-and-publish operation.
The first matching name wins if multiple saved entries have identical bundles.
The file container's name/font-source metadata is not part of the hash; embedded
font metrics, glyph masks, colors, controls and tiles are. Original font files
are not needed. Matching is byte-based, including bundle version and padding,
not visual equivalence. FNV is a local lookup identifier, not an authentication
mechanism; hash collisions are theoretically possible.

Opening a match establishes a clean saved baseline, an empty Undo history, and
its saved name/list selection. It neither publishes nor changes startup choice.
No match, legacy kernel, corrupt files, collection-limit failure or a session
change leaves the normal initial draft. This also handles a deleted active file
without resurrecting it. See [verification](docs/frame-studio/active-checkpoint.md).

## Editor, typography and controls checkpoint

The implementation uses the merged userland arena (`ARENA.md`) for bounded
font-preparation scratch. Each completed bundle is a separate heap allocation;
destroying the arena or font context cannot invalidate it. Publication copies
the bundle into kernel-owned staging and transfers ownership on commit.

- `shared/decoration.c` validates assets, computes four-sided insets/top-titlebar
  layout, and paints proportional grayscale title glyphs, button states and opaque frames.
  The preview consumer and kernel compile the same implementation. Text decoding
  shares the existing W1 rules; legacy byte titles retain a Latin-1 path. The
  startup fallback remains the compiled bitmap font until a bundle is applied.
- V6 accepts a 328-byte header, sorted 32-byte glyph records, sorted 8-byte pair
  adjustments, and tightly packed 8-bit masks. Caps: 8 MiB per bundle, 768
  glyphs, 65,536 pairs, 256x256 per mask, and 256 pixels of line height. There
  are no executable callbacks or texture/image decoders in the format. Border
  width is 1..16 pixels; horizontal/vertical padding is 0..32. V6 accepts top
  placement, prepared opaque finish tiles, and left/center/right title alignment. Its header
  adds up to eight ordered slots: close/minimize/maximize/pin or spacer,
  leading/trailing group, and square/round/bare housing. Duplicate actions are
  refused; unused slots must be zero. Button size is 16..48 pixels and gap
  0..16. Versions 1 through 3 are refused; saved container V1 accepts V4, V5 or V6.
  V4 requires a zero button-face field and retains its automatic housings.
  V5 assigns that word to the button fill: zero for automatic, FFrrggbb for
  custom opaque color, or 01rrggbb for transparent with retained RGB.
  V6 appends four active/inactive symbol-color pairs indexed by button action.
  Zero inherits title text; FFrrggbb supplies custom ink. V4/V5 retain their
  296-byte prefix and normalize to a V6 editable recipe with inherited symbols;
  a style edit emits V6 while copying the embedded font assets. Four 32x32
  XRGB tiles follow the glyph masks at an aligned offset: active/inactive
  title and active/inactive border. Userland prepares solid, gradient, grain,
  stripe and stipple finishes; the shared painter samples the finished tiles.
  Gradients stretch across the surface; patterns repeat in frame coordinates.
  Active/inactive titles and borders each own two colors. Gradients span the
  exact pair independently of Strength; patterns blend their second color
  toward the first according to Strength. Solid uses the first color.
- One layout places button groups and the remaining title interval. A minimum
  frame width reserves the selected groups and 24 pixels of drag space. Apply
  refuses existing or saved restore rectangles that cannot fit; creation
  refuses undersized frames, and resize clamps to the decoration minimum.
  Title height fits both the font and buttons. Round housings use rectangular
  hit extents equal to their slot; spacers have no hit target.
- The shared button capture tracks window identity and action. It activates
  on left release over the captured button, supports drag-out/re-entry, and
  drains additional consumed edges. Apply, geometry changes, destruction,
  minimization and VT handoff cancel the action. Close emits a normal request;
  mouse clicks do not alter Alt+F4's escalation timestamp. Pointer and keyboard
  actions share the WM helpers. Maximize/restore availability follows the
  application and decoration minima; unavailable controls are dimmed.
- `/sys/decorations` reads a generation header followed by a versioned prepared-asset
  fingerprint and byte length, captured together as an immutable per-open snapshot.
  A write handle accepts BEGIN,
  ordered DATA (up to 1024 payload bytes per command), then COMMIT. The 24-byte
  command carries its expected generation, total length and exact offset.
  COMMIT validates the bundle and preflights geometry before publishing; its
  write result is the acknowledgement. Close discards uncommitted staging.
  Concurrent commands on one handle are serialized by a busy state without
  allocating under its spinlock. Competing publishers compare at commit.
- The GUI create ABI adds a content-size mode, an optional fit-to-screen mode,
  and an explicit W1 title flag. Existing frame-size calls keep their meaning.
  The WM re-derives geometry under the creation lock after canvas preparation,
  checking staged capacity if a decoration changed in between. gterm and gview
  use the new mode; fixed bare-frame menus keep their existing sizing helper.
- Ordinary Apply preserves content size/backing storage and normally its screen
  position. Frames move only to preserve a reachable titlebar. Maximized frames
  stay screen-sized; Apply refuses a violated minimum. Restore rectangles are
  converted through the old content rectangle. The bare-frame toggle uses the
  same geometry rules. Desktop and popup windows remain undecorated.
- The Alt+Tab strip adopts the active frame palette. Its text remains on the
  existing bitmap font; this checkpoint does not claim adjustable switcher text.

`/tests/decorationtest --hold` is an interactive integration fixture, not the
Frame Studio application. It prepares a 24px DejaVu title face, checks retained
content, exercises stale/malformed/abandoned and concurrent publication, and
leaves two sample windows for testing. `+`/`-` change size, `l` fixes the focused
window's minimum at its current content size, `r` releases that minimum, `v`
reports geometry and client mouse-edge counts, and `q` exits. Keys `1`..`5`
select standard, close-only, reversed leading group, no controls, and optional
pin. Close requests are counted and deliberately leave the fixture open;
`d` replaces its first window for capture-loss testing. The installed decoration remains after exit.
The application is `/bin/framestudio`, accessible through Control Center's
`settings` menu beside Appearance Workshop. Its pages are Frame, Buttons,
Finish and Saved; the stage uses the same painter as the WM at actual size.
Midnight Enamel starts with the pin on the left. Paper & Graphite and Workbench
keep the draft's chosen font while replacing the frame recipe. The editor's
interface follows UI settings, while its title font starts from a copy and
remains independent. A 32-entry/32-MiB Undo history retains prepared snapshots
and coalesces slider gestures;
failed or no-op edits do not consume history. Font preparation must succeed
before replacing the draft. Style edits copy the prepared glyph bundle and
regenerate only the finish tiles. Apply is session-only. Named Save/Load uses
self-contained files; Saved also offers Use at startup and Restore default
startup. Explicit Add/Remove, Earlier/Later, Group and
Shape controls provide keyboard-accessible arrangement; dragging slots is not
implemented. Rounded window silhouettes remain in the later surface stage.
The Finish page selects Color 1 or Color 2 for the chosen surface. Text has
one color; Solid disables Color 2 while preserving its value. Strength and
Scale are enabled when a title or border uses a pattern. Direction applies
to gradients and stripes. Matching the border finish retains the border's
own color pair.

Validation and remaining work are recorded in
[the typography checkpoint](docs/frame-studio/typography-checkpoint.md) and
[the controls checkpoint](docs/frame-studio/controls-checkpoint.md), and
[the native editor checkpoint](docs/frame-studio/editor-checkpoint.md).
The [two-color checkpoint](docs/frame-studio/two-colors-checkpoint.md) records
the finish color pairs and endpoint/Strength validation.
The [saving checkpoint](docs/frame-studio/saving-checkpoint.md) records named
self-contained Save/Load, replacement refusal and persistence checks.

## Product contract

Frame Studio is an independent Control Center application for composing window
decorations. Flexibility is a primary feature: presets demonstrate the system
and remain editable compositions. Appearance Workshop continues to own client
widget appearance and interface/document/terminal fonts.

Requirements settled in discussion:

- Start with standard top titlebars and normal window-management functionality.
  Preserve the ability to add left, right, and bottom titlebars in the design;
  those placements are not first-release controls.
- Adjustable title font and size belong in the first release. Chris's desktop
  target is 1920x1080; the fixed title font is difficult to read even with a
  readable interface font. Test at native scale, not just in enlarged mockups.
- Each composition owns its title font and size, initially copied from the
  effective interface selection. Later interface-font changes do not alter it.
- A composition selects which buttons exist, where they sit, and how they look.
  A close-only composition is valid. Removing a button removes its hit target.
- Titlebar and border finishes can use texture. Built-in patterns should make
  useful textures possible without an external image editor. Imported textures
  are part of the intended direction; their delivery stage is proposed below.
- Preview before Apply; distinguish active/inactive windows and button states.
  The mockup is a visual direction, not evidence of implemented functionality.

## Editor and visual direction

Access: **Control Center -> Frame Studio**, alongside Appearance Workshop.
Proposed executable: `/bin/framestudio`. Control Center reads the named
`settings` section of `menu.conf`, so the implementation adds Frame Studio to
that section. This does not require a separate desktop right-click menu item.
The studio follows Control Center's existing independent-tool model.

The editor uses the current interface font. Its own controls remain on the
active UI theme while the stage previews a draft decoration. The stage has two
overlapping sample windows, with focus switching and simulated button actions.
Closing a specimen resets/hides that specimen; it does not close Frame Studio.
An Actual size view is the reference for readability; any scaled overview must
be labeled and must not substitute for the actual-size check.

| Page | Controls |
| --- | --- |
| Frame | Title font and size, title alignment, title padding, border widths, corner treatment |
| Buttons | Included actions, leading/trailing groups, ordering and spacers, shape, symbol treatment, size and spacing, state samples |
| Finish | Active/inactive colors, titlebar/border finishes, match-titlebar option, texture scale/direction/strength, relief |

Use an Available buttons tray plus explicit Add/Remove and Move controls, with
dragging as an additional arrangement method. Keyboard users must be able to
make the same composition. Empty groups are valid. Repeated action buttons are
refused; spacers can repeat. Reintroducing an action restores a usable style.

Three initial compositions exercise different combinations: Midnight Enamel
(fine blue grain and amber focus edge), a monochrome striped composition, and
Workbench (warm gray, square edges, tactile bevels). The names and precise
palette are art direction, not file-format enums.

The studio follows the established distinction between Apply to session, Save,
and Use at startup. Save captures a named draft without activating it. Startup
selection does not alter the running session. Undo operates on draft edits.
Do not add a second Accept command with overlapping meaning.

## Title typography

Settled with Chris: a composition owns its title font and size. Initial choices
are copied from the effective interface selection; loading a saved composition
restores that composition's choices. Changing interface typography leaves saved
decorations and live title metrics unchanged. The proposed Copy interface font
operation explicitly copies the current interface selection into the draft;
Apply and Save retain their separate meanings.

Use the existing installed-font discovery and supported outline-font backend.
Do not add a separate font installation directory or pretend the existing
three-role provider already has a title role. Decoration typography remains
outside the opaque `fonts.*`/`/sys/appearance` component.

Title thickness derives from the prepared font's line/ink extents, padding,
and button extent. Increasing the font grows the titlebar; it must not clip
descenders or leave button symbols pressed against an edge. Font size and
button size are separate choices. Normal/focused/inactive states use identical
geometry. Font support and size limits must be displayed honestly.

Reserve button groups before placing text. Center alignment means centered in
the titlebar when it fits, constrained to the remaining text interval when it
does not. Truncate at decoded cluster boundaries with a fitted ellipsis; a
zero-width text interval is legal. Keep text out of buttons and pin indicators.

## Buttons and interaction

First functional actions: Close, Minimize, and Maximize/Restore. Optional Pin
can reuse the existing window state. The menu icon in the concept image is a
design placeholder: a window menu needs its own real behavior before appearing
as a selectable action. Shade is a later behavior, not a dummy first-release
button. Hiding a button does not disable its keyboard window-management action.

The visual housing (square, soft, round, or bare) is independent of its action
symbol. Bare means a symbol without a permanent housing. Hover, pressed,
inactive, and unavailable presentations must remain recognizable. Restore has
a distinct symbol when the window is maximized; pin indicates its toggled state.

Proposed pointer contract:

- Press captures the button by window identity, action, and layout generation.
  Activation occurs on release over that same button. Leaving it removes the
  pressed appearance; releasing elsewhere cancels. Re-entering can re-arm it.
- A button press is not a titlebar drag or the first half of a titlebar
  double-click. Focus/raise can still accompany the press.
- Destruction, VT handoff, or geometry replacement cancels the action. Drain
  the matching release even after cancellation; do not send an orphan release
  into a client. Additional buttons must not leak edges either.
- Close sends the existing close-request event, allowing applications to
  prompt about unsaved work. Repeated mouse clicks do not terminate a process.
  Keep the existing deliberate Alt+F4 escalation separate; a mouse close must
  not arm its escalation timestamp.
- Minimize uses existing minimize/Alt+Tab restoration. Maximize/Restore uses
  the existing minimum-size checks. Desktop and popup exceptions stay explicit.

One computed layout supplies painting, button hit testing, titlebar dragging,
and border resize regions. Resize affordances must not cover client pixels or
neighboring windows invisibly. Start with visible edge/corner regions inside
the frame and preserve Ctrl+Alt resize/move as the independent escape hatch.
Thin-border usability needs a deliberate measured solution, not an invisible
input area extending arbitrarily into the client.

For narrow windows, text gives way first. A decoration-derived minimum width
then preserves its button groups and a small drag region. Preflight existing
windows against that minimum; do not silently remove selected buttons or
enlarge application contents to make an Apply succeed.

## Finishes

Begin with solid, gradient, fine grain, stripes, and stipple, plus independent
edge highlights/shadows for relief. Pattern generation is deterministic for a
saved seed. Pattern coordinates are anchored to the window, not to the screen
or current damage rectangle, so dragging and partial repaint do not make the
material crawl. Titlebar and border can share those coordinates for a continuous
finish, or use independent finishes. Active/inactive states share geometry.

Imported tiles are prepared in userland through libimage. A recipe states the
tile scale, tint/strength, and repeat behavior. Opposite edges should join for
a seamless repeat; the preview must make seams visible before Apply. An initial
import can require opaque pixels, with an explicit background-flattening step
for alpha, instead of accidentally introducing transparent window contents.

The first texture lesson should build a quiet enamel finish: base blue, weak
grain, a thin edge highlight, then compare it with the same finish inactive.
Highlights follow the frame geometry; they are not baked into a repeating tile.
Sample recipes should be reproducible in the editor without a graphics package.

Rounded outer corners require compositor work: background painting, occlusion,
damage, and pointer containment must agree about the exposed corner pixels.
The current rectangular coverage optimization cannot be retained unchanged.
Treat rounded silhouettes as an explicit implementation slice. Shadows outside
the frame and general window translucency are separate scope, not implied by
rounded buttons or textured borders.

## Current implementation and consequences

Inspection found these concrete integration points:

| Source | Current behavior and design consequence |
| --- | --- |
| `kernel/include/gui/window.h` | Fixed 20px titlebar and 1px border; geometry helpers take flags and assume a top titlebar. Replace assumptions with explicit four-sided insets and layout. |
| `kernel/src/gui/window.c` | `composite_one` draws fixed 8x16 title text; content placement, resizing, maximize/restore, and occlusion use frame geometry. |
| `kernel/src/gui/compositor.c` | Existing move, resize, minimize, maximize, Alt+Tab and close behavior; add button capture and share action helpers rather than reproduce state transitions. The switcher also uses chrome colors and fixed text. |
| `abi/include/os64/gui.h` and `kernel/src/gui/gui_client.c` | Creation accepts outer frame dimensions; inline content-to-frame arithmetic uses compiled constants. Dynamic decorations need a race-free content-size creation contract. |
| `userland/libos64/font_config.c`, `font_discovery.c`, and `include/os64/font_backend.h` | Userland already loads fonts and exposes grayscale glyph masks/metrics. Font decoding does not need to move into ring 0. |
| `kernel/src/appearance.c` | Generation-checked opaque widget/font publication. Decorations must not turn this into a kernel theme parser. |
| `kernel/src/driver/filesystem/sys/sysfs.c` | Appearance writes return publication success directly; a decoration operation needs an equally observable commit result. |

## Proposed rendering boundary

Recommend retaining window ownership, geometry, hit testing, and composition in
the existing WM, with a shared bounded decoration layout/painter usable by both
Frame Studio's preview and the kernel. Userland prepares a versioned immutable
bundle: validated style values, glyph coverage masks and metrics, and normalized
texture tiles. The kernel validates and owns the accepted bytes; it does not
retain a mutable pointer into Frame Studio's memory. Closing Frame Studio must
leave the appearance installed and fully functional.

This follows the useful console-font precedent of userland rasterization, but
PSF2 itself is not the proposed title format: proportional advances, bearings,
grayscale coverage, and title text fitting need a richer bounded representation.
FreeType and image decoders remain in userland. Kernel painting uses integer
arithmetic and no font-file I/O, rasterization, or allocation under the GUI lock.

For a first font bundle, propose the finite Western W1 repertoire already
defined in `FONT_CONTRACTS.md`, with prepared fallback/marker glyphs and bounded
pair adjustments. Extract the reusable decoder/cluster rules from the existing
text implementation; do not develop a second subtly different UTF-8 policy.
Include deterministic fallback for unsupported scalars and malformed input.
The foundation must explicitly specify title encoding at the GUI boundary:
legacy byte-title rendering and new W1 titles must not be silently conflated.
Exact glyph/pair/texture/byte caps and shared-module boundaries must be written
down and tested in the foundation slice before accepting external bundles.

Alternatives considered: a live userland decorator service would avoid prepared
repertoire limits, but adds per-window requests, stale-response handling and
service-loss recovery. Moving FreeType into the kernel expands the parser and
runtime boundary unnecessarily. The prepared-bundle recommendation fits the
existing WM, at the cost of an explicitly bounded text profile and asset format.

## Geometry and live Apply

Represent decoration placement as an edge plus leading/trailing button groups,
with explicit left/top/right/bottom content insets. V1 accepts the top edge;
other values receive an unsupported-version/feature result. Do not encode
future side titlebars by proliferating hardcoded top-height arithmetic.

Proposed Apply transaction:

1. In userland, validate the recipe and prepare fonts/tiles. Kernel-side staging
   checks lengths, arithmetic, offsets, counts, metrics, and supported actions.
2. Under the GUI lock, check the expected generation and preflight the live
   window set, minima, canvas capacities, saved restore geometries and screen.
3. If the whole candidate fits, cancel affected WM captures, swap the immutable
   style, update frame/content geometry and damage old/new extents. Notify
   clients whose drawable size changes through the existing resize contract.
4. Release retired resources outside the GUI lock. Failure before commit leaves
   the old style and window geometries intact. Stale drafts receive a conflict.

For ordinary windows, preserve the content dimensions and content screen
position, growing/shrinking the frame around them. Translate only when necessary
to expose the enabled button groups and a usable drag region on screen. If no
such placement exists, or the frame is narrower than its required controls,
refuse Apply and name the affected window and rule in the DEBUG_GUI log.
Content may remain partly offscreen; do not shrink it to rescue an oversized
decoration.

Maximized windows keep their screen-filling outer frame; new insets therefore
change their drawable size. Refuse the whole Apply if that would violate a
window's minimum. Convert saved restore frames via their old content rectangle
so Restore retains its intended content size. Include minimized windows and
their saved geometry in preflight. Desktop and popup windows do not acquire
titlebars. Ordinary no-titlebar windows retain their defined bare-frame policy;
do not silently reinterpret that flag as full decoration.

Propose an additive content-size creation mode on the existing GUI create
boundary, with a named userland helper. The WM derives outer dimensions using
the active style in the same locked operation as creation. A metrics query
followed by legacy creation is insufficient because Apply can race between them.
Keep legacy frame-size creation's meaning, migrate in-tree callers that intend
content sizes, and document compiled geometry constants as legacy defaults.
This is an ABI change proposal, not a request for a new syscall number.

Atomic publication means WM style/geometry commit together. It does not mean
resized applications have already painted their new contents when Apply returns.

## Publication, saving, and startup

Use a separate decoration interface, provisionally `/sys/decorations`, with a
versioned prepared bundle and expected generation. Persistent recipe settings
remain separate from `theme.conf`/`fonts.conf`; neither interface-font Apply nor
widget-color Apply should overwrite a decoration. The earlier separate-chrome
contract remains the boundary; the prepared asset format is a new proposal.

Freeze whether publication is one bounded write or staged writes plus explicit
commit before implementation. Large assets favor staging. In either case,
success/failure must be returned by an observable operation, not inferred from
close: `DEBTS.md` records that close currently discards filesystem failure.
Concurrent writes to a shared staging handle require serialization; closing or
abandoning staging without commit must not change the active decoration.

Prefer self-contained saved compositions containing the recipe and prepared
assets, so replacing a source texture/font file does not silently change a saved
look. Source paths can remain editing metadata; missing originals may prevent
rerasterizing a different size but must not prevent loading the saved appearance.
Specify the container, budgets and atomic replace procedure before Save work.
No automatic coupling to Appearance Workshop's saved-theme format in this slice.

Named-save contract: the config target `frames` contains `<name>.frame` files
(normally `/home/frames`). Names use 1..40 letters, digits, interior spaces,
hyphens, underscores, ampersands or parentheses. The little-endian V1 container
has an 800-byte header: eight uint32 fields (magic `0x314d5246`, version, total
bytes, bundle offset, bundle bytes, font size, checksum, reserved), followed
by three 256-byte NUL-terminated font paths. The prepared V4, V5 or V6 decoration bundle
follows at offset 800. The checksum is FNV-1a over the complete file with its
checksum field treated as zero; it detects corruption, not authenticity.
Maximum file size is 800 bytes plus the 8 MiB bundle cap. Source paths are
editing metadata; Load and same-font restyling use embedded assets.

Save validates the recipe/bundle and container before I/O, creates an
exclusive per-task staging file in the collection, handles short writes,
requires successful sync, then publishes with NOREPLACE. Explicit replacement
uses REQUIRE_ATOMIC_REPLACE; a filesystem unable to preserve the old name
refuses. Failure removes the staging file and keeps the old composition.
The browser admits 128 named entries within a 512-entry directory scan.
Load validates the complete file before replacing the draft and clears Undo
after a discard confirmation if needed. Undo owns prepared snapshots so font
changes can be reversed without source files; its budget is 32 entries and
32 MiB. Closing an applied but unsaved draft asks before discarding it.
Named Save/Load is independent of startup selection and imported surfaces.

The desktop's userland startup path loads the chosen decoration before launching
configured applications. Generation-zero startup install must not overwrite an
Apply that already won. Missing/corrupt startup assets keep a compiled usable
decoration and produce a diagnostic. Saving or selecting next-boot appearance
does not install it into the current session. Startup selection stores its own
self-contained snapshot; replacing or deleting a named collection entry does
not change that snapshot. `decoration.startup` resolves through the config
ladder (normally `/home/decoration.startup`). Its 16-byte V1 header carries
magic, version, payload length and an FNV-1a checksum over the header and
payload with the checksum field zeroed. A nonempty payload is a validated
prepared V4/V5/V6 bundle, capped at 8 MiB; a zero-length payload explicitly
selects the compiled default and masks lower config layers. Neither path
requires the original font or collection file. Exclusive same-directory
staging, sync and REQUIRE_ATOMIC_REPLACE preserve the previous startup choice
on write or replacement failure. This is an explicit desktop startup call,
not a shared-library constructor. See
[the startup checkpoint](docs/frame-studio/startup-checkpoint.md) for evidence.
Collection deletion follows the confirmation and
draft-preservation contract in Current completion order above.

This is a kernel-interpreted rendering configuration, unlike the opaque widget
payload. Re-check the publication-store extraction trigger in `DEBTS.md` when
choosing its transport; do not copy another bespoke opaque store by reflex.

## Implementation stages and acceptance

1. **Foundation and readable titles:** freeze bundle limits, font preparation,
   shared geometry and creation compatibility. Implement adjustable title
   typography and a small Apply exercise. Prove size changes on real windows
   before building the full editor. Use an isolated worktree for implementation.
2. **Standard controls:** minimize/maximize/close, optional pin, shared action
   helpers, layouts and button state/cancellation tests. Deliver close-only and
   reversed-group compositions as ordinary data.
3. **Frame Studio and built-in finishes:** native editor, actual-size stage,
   draft Undo, font selection, accessible arrangement controls, procedural
   finishes and the three sample compositions. Apply is live and session-only.
4. **Persistence:** saved self-contained compositions, startup selection and
   deleting saved compositions. These are implemented, along with active
   saved-composition lookup and independent button-symbol colors; acceptance
   boundaries are in Current completion order above.
5. **Later extensions:** imported tiles and
   rounded silhouettes with correct occlusion/damage. Side/bottom titlebars
   and shade remain later extensions as well.

Each stage needs focused host tests and a strict build; integrated runtime
changes also need QEMU interaction and pixel/geometry evidence. Primary checks:

- Font growth at 1920x1080, including ascenders/descenders, long and non-ASCII
  titles, high-contrast inactive titles, and preview/real-window agreement.
- Two distinct layouts, close-only and empty groups, narrow windows, button
  drag-out/re-entry, double-click isolation, rapid clicks, and capture loss on
  destroy, Apply, minimize, and VT handoff. Verify no orphan client edges.
- Ordinary, maximized, minimized, pinned, no-titlebar, popup and desktop cases;
  preserve unsaved application contents, minima and restore behavior.
- Apply raced with window creation/destruction, a minimum-size change, another
  publisher, malformed data, partial staging, allocation failure, and abandonment.
- Deterministic textures across split damage/dragging; rounded corner exposure
  and occlusion; compositor cost at native resolution with multiple windows.
- Save/load independence from source assets, replacement failure, startup race,
  and reboot persistence. Separate QEMU evidence from Chris's hardware acceptance.

Implementation source changes and their comments must be reviewed together;
scan sibling geometry/color uses, including the Alt+Tab strip. Do not describe
the strip's fixed font as adjustable until that explicit companion work lands.
Architecture review should precede implementation review for this cross-boundary
feature. The implementation checkpoints record build/runtime evidence; neither
architecture review nor implementation review is claimed here.

## Button fills and selectable color codes

On **Buttons**, select a control and choose **Edit button colors...**. The
Finish page also lists Close, Minimize, Maximize and Pin below the frame colors
(navigate with Up/Down, Home/End or Page Up/Down). Opening Finish or using
Edit button colors focuses the list for immediate keyboard navigation. Each
included control owns its fill independently.
The **Fill** button cycles Automatic, Color and Transparent. Picking a color
or setting a six-digit code selects Color. A Bare control becomes Round when
a fill is chosen so the new color is visible; Square/Round remain independent
shape choices on Buttons. Missing controls have disabled color editors.

Custom fills use the chosen RGB for active and inactive windows. Hover lightens
the fill, pressing darkens it, and unavailable controls blend it toward the title
color. Symbols follow active/inactive title text by default; each action can override
its active and inactive ink independently. Transparent suppresses
the housing in all interaction states, exposing the title finish; the symbol,
press offset and rectangular click target remain. This does not make the window
or its titlebar translucent. Fill settings participate in Undo, Apply and Save.

The hex field supports mouse-drag selection, Shift+Left/Right/Home/End,
Ctrl+A, Ctrl+C, Ctrl+X and Ctrl+V. These are textfield behaviors, so the saved
composition name field also benefits. Copy publishes the selected bytes to the
system clipboard. Paste uses the first line, replacing a selection only after
successfully reading text that fits at a cluster boundary. Failed cut/paste
preserves the original selection and text. Font changes retain selection byte
boundaries and repaint using the adopted font's geometry.

The shared color picker preserves HSV editing coordinates when an application
echoes its current RGB value. Frame Studio skips that redundant setter call;
shade drags retain the chosen hue, including near gray/black where RGB rounding
cannot recover it. A different external RGB (hex edit, Undo or another color
role) still updates the picker.

List keyboard navigation lives in libui, with PS/2 and USB HID event coverage.
Navigation also reveals an unchanged selection if a companion scrollbar moved
it out of view. A built-in list scrollbar remains a separate slice.
