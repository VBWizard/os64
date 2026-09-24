# Frame Studio

Window-decoration design, 2026-09-21. Chris accepted the design direction in the
Frame Studio discussion, with access through Control Center alongside Appearance
Workshop. Implementation proposals below are not implemented or an ABI freeze;
the foundation stage must settle the detailed interfaces and limits. Source
inspection baseline: `userland` at `2e30eb0`.

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
refuse Apply with the affected window named. Content may remain partly offscreen;
do not shrink it to rescue an oversized decoration.

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

The desktop's userland startup path loads the chosen decoration before launching
configured applications. Generation-zero startup install must not overwrite an
Apply that already won. Missing/corrupt startup assets keep a compiled usable
decoration and produce a diagnostic. Saving or selecting next-boot appearance
does not install it into the current session.

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
4. **Persistence and richer surfaces:** saved self-contained compositions,
   startup selection, imported tiles, and rounded silhouettes with correct
   occlusion/damage. Side/bottom titlebars and shade remain later extensions.

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
feature. This draft records design work only; it does not claim either review.
