# Control Center and Appearance Workshop

Design and implementation record, begun 2026-09-10.

## Current implementation

Control Center, the composable widget gallery, session-only Apply, a theme
collection, Save, and startup selection are implemented. **Apply to session**
updates participating open applications. **Save** stores a named composition
without activating it. **Use at startup** stores a separate startup snapshot.
Reboot clears session overrides and applications load that startup snapshot.
Palette and control treatment remain independently composable. Fonts, detailed
property editing, and window decorations remain separate slices.

## Review scope and checkpoint

PR #107 covers the theme foundation: Control Center, Workshop presets and
composition, live Apply, saved themes, and startup selection. Palette editing
and rounded controls belong to a follow-up PR. Window decorations belong in
a separate PR and design discussion. Their scope is not limited to frame colors
or a few border metrics; the desired visual treatments,
controls, geometry, and behavior must inform that design.

## Save and collection contracts (2026-09-15)

- The Workshop has a scrollable theme collection containing three compiled
  presets and up to 128 personal saved themes. Presets are marked `[preset]`;
  personal names are sorted by ASCII byte order. Selection highlights a row;
  **Load theme** imports its complete composition into the preview. Arrow,
  Home/End, and Page Up/Down keys navigate; the collection has a scrollbar.
  **Refresh** discovers saves from another Workshop without replacing the draft.
  An unreadable or over-capacity collection reports an error rather than silently
  claiming that a partial personal collection is complete.
- Palette selection has its own list. Flat/Raised remain independent choices.
  Loading a complete theme replaces both choices; choosing a palette preserves
  the treatment and geometry. Paper & Graphite's palette is unchanged.
- Personal snapshots live in `themes/<name>.theme` below the first directory
  of the shared config ladder (`/home/themes` with the default configuration).
  `os64_conf_target` uses the existing resolver's no-probe operation; this is
  not another search order or a new syscall. The Workshop creates this directory
  when listing or saving. Compiled presets are immutable; a personal theme may
  share a preset's display name without editing the compiled preset.
- Names contain 1–40 ASCII letters, digits, spaces, `-`, `_`, `&`, `(`, or `)`.
  Leading/trailing spaces and path separators are refused. Case behavior is
  the filesystem's: ext2 distinguishes case; FAT does not. Save reports a
  collision through the filesystem's no-replace operation, not a racy existence
  check. Malformed files stay visible but cannot replace the preview on Load.
- A saved file is a complete, independently owned copy of the current schema:
  colors, button relief, and validated geometry. It uses the shared config
  grammar and schema-driven encoding; partial, oversized, malformed, or
  range-invalid snapshots are refused. The sample text, caret, selection,
  and scrollbar state belong to the gallery, not to the theme file.
- Save uses a task/sequence-specific sibling temporary, checked writes and
  sync, then no-replace rename. A collision opens an explicit Replace/Keep
  editing prompt. Replacement uses `REQUIRE_ATOMIC_REPLACE`; a filesystem
  that cannot safely replace an existing destination (including FAT) refuses
  and keeps the existing file. No remove-first fallback is used. Concurrent
  explicit replacements are last-publication-wins; there is no claim of
  compare-and-swap against the version previously loaded. Temporary files
  are ignored by the collection and removed on reported publication failure.
- The composition name is separate from the gallery's sample text field.
  Unsaved status compares the draft and name with the loaded/saved baseline,
  independently of session Apply status. Loading another composition or closing
  a dirty Workshop prompts Discard changes/Keep editing. Cancel returns to the
  draft so it can be saved first. Save does not clear component edits pending
  Apply. A failed load/save preserves the draft and its gallery content.
- **Use at startup** operates on an unchanged loaded preset or saved draft;
  edited drafts must be saved first. It writes a complete snapshot into
  `theme.conf` through the shared config writer, preserving comments. This is
  a copy, not a live reference to the named file: replacing that file later
  does not change startup. The checked writer validates the complete merged
  output before publication and requires atomic replacement. It refuses
  unknown preserved keys/malformed lines that would make startup unreadable.
  Concurrent startup selections are last-publication-wins.
- Before changing `theme.conf`, userland preserves the current session palette
  and relief. If `/sys/appearance` is still generation zero, it publishes the
  old startup override with expected generation zero, preserving absent keys
  as application defaults. A racing valid Apply wins;
  an invalid session or I/O failure refuses startup selection. Existing sessions
  are not republished. Thus new applications continue to join the same live
  session after startup selection. Geometry is still initialized per application
  from startup configuration and is not live-reloaded; Workshop has no metric
  editor. The preservation publication can succeed even if the later file save
  fails, but it does not contain the proposed new startup colors.
- Preservation payloads start with `inherit = startup` and contain the explicitly
  configured live keys from the old startup file. Missing, empty, or invalid
  startup configuration yields an empty override; transient read/allocation
  errors refuse preservation. Geometry keys are validated when reading the file
  but omitted from this payload. Ordinary Apply payloads still require the full
  set of colors and button relief. The kernel treats either form as opaque data.
- Active-context initialization uses `os64_ui_theme_current` with application
  defaults. It reads disk configuration and then the session store; a preserved
  override restores the caller's default colors before merging its present keys.
  Thus an app opened after startup selection does not adopt the next-boot colors.
  Existing contexts merge the same present keys without changing unspecified
  fields or geometry. The disk-only startup APIs remain available for displaying
  the next-boot selection. A later explicit Apply replaces the preserved override
  with a complete shared appearance.
- Control Center and the Workshop retain their Midnight defaults when no usable
  startup override exists, and honor a configured startup theme before applying
  the live session. Application-specific drawings and window chrome remain
  outside this widget contract.

## Agreed direction

- Build a settings entry point before Appearance Workshop. The Workshop is
  one of its tools; other settings tools can join it as they are developed.
- Appearance Workshop manages and edits themes, with an interactive widget
  gallery that gives us a place to develop the toolkit's appearance.
- Apply changes already-open participating applications. Restart-to-apply
  is not the completion criterion.
- Midnight Workshop is the first visual direction: ink blue, charcoal,
  warm white, and restrained amber accents. The native gallery supplies its
  working palette and control-state examples.
- Customizability is a product value worth supporting with additional
  infrastructure. Existing primitives are a starting point, not a limit.
- The default GUI should feel modern, with enough customization to adopt a
  classic appearance. Presets are compositions of independently selectable
  elements, not indivisible skins.
- Desktop tools should be composable and replaceable through configuration.
  The model is `grootmenu`: an ordinary application selected as the desktop's
  launcher in `gui.conf`. Control Center launches independent settings tools.
- Reusable widgets may be built ahead of their first production application.
  The earlier application-demand-only constraint no longer governs this work.
  Use the gallery to develop a coherent toolkit, with working interaction
  and theme support alongside appearance.
- Font choice is not restricted to one face. The existing bitmap renderer is
  a starting point; additional faces and rendering capabilities may be added.
- The primary commands are Apply and Save. Do not add a synonymous Accept
  action alongside them.
- Apply is session-only: use this appearance now; reboot clears that applied
  session state. Startup selection belongs to saved themes, not an option
  beside Apply. Defer saved-theme/window-decoration integration until after
  the widget pieces under development.
- Saved compositions keep independent copies of their source settings.
  Editing a source palette or treatment does not change saved compositions.
- The running appearance can layer per-component session overrides over a
  theme. Changing one component preserves the other current choices and does
  not modify the saved theme.
- Window Decorations is planned as a separate Control Center entry. It
  selects/configures the decoration as a unit; Appearance Workshop handles
  mixing appearance components. Whether Workshop Save captures that separate
  decoration selection is still open.
- Discuss window decorations after themes. Their controls, geometry, and
  rendering architecture remain a separate design decision.

## Proposed front door: Control Center

Use **Control Center** as the settings entry point's name. Show available
tools by name. Appearance Workshop is the first entry; do not present
unimplemented settings as functioning controls.

Control Center launches ordinary settings applications.
Appearance Workshop is independently launchable and owns its own window,
state, persistence, and errors. Closing Control Center does not close the
settings tool. A reaper thread collects children while Control Center waits
for GUI input; the kernel reparents surviving tools when their parent exits.

The first catalog is the named `settings` menu in `menu.conf`, read through
the shared configuration search. It uses the existing menu grammar, including
quoted labels, commands with arguments, separators, and nested categories.
Control Center pages through entries and provides category navigation.
The Workshop's executable is configured in the menu rather than hardcoded.
An existing user `menu.conf` replaces the system copy; add the `settings` menu
and root Control Center entry to that copy to expose the new tools.

Document the launch contract so another application can fill the same role.
Tools own their settings; Control Center owns discovery and launching. Shared
toolkit and configuration libraries keep ordinary controls and file handling
consistent across applications. A plugin loader or shared-process settings
host is not required for this structure.

Add Control Center to the root menu when it is functional. Future entries
may include window behavior, keyboard and pointer preferences, and desktop
settings; those examples do not commit their implementation in this work.

## Proposed Workshop interaction

The window has three areas:

1. A theme collection with named presets and saved personal variations.
2. A working preview with actual toolkit controls and editable sample text.
3. An inspector for the selected appearance property, with color swatches,
   a color picker, numeric values, and contextual explanations.

The preview should demonstrate focus, selection, pressed controls, text
fields, scrollbars, and menus. Additional controls belong in the reusable
toolkit and may be developed ahead of a particular application's needs.
Disabled and hover appearances
require matching toolkit behavior/state; a drawing alone does not provide
those capabilities.

Keep the editor controls usable while experimenting with an unreadable
preview. The preview owns a draft theme separately from the editor's active
theme. Changes to the draft repaint the preview immediately. Include reset
and undo facilities; decide the exact undo scope before implementation.

Primary command meanings:

- **Save** persists a named theme without activating it. Naming a new theme
  or saving a variation belongs in that save flow, not another primary button.
- **Apply** validates and publishes the draft as the active appearance,
  then requests participating applications to reload it.

Undo, resetting an individual property, and loading the active appearance
are editing operations whose placement remains to be designed. Keep their
meaning distinct from changing the active desktop.

Proposed state model: distinguish the draft being edited, the named theme
last saved, and the active appearance with its session overrides. Apply
changes the running appearance without silently overwriting a named theme.
Applying one component preserves the other current component choices;
activating a whole theme is an explicit operation. Save updates the named
theme without changing the running appearance. The UI should distinguish
unsaved edits from an unapplied draft; saving does not imply applying, or
vice versa.

Session overrides are temporary unless captured in a saved composition.
Startup selection stores an independent snapshot as described above. Applying
a draft does not change that selection or persist the session over reboot.

When changing the selected theme or closing the editor would discard unsaved
edits, offer a way to retain them before proceeding. Applying an unnamed
draft still leaves an active appearance that can be loaded and saved later.

Do not make a global change on every color-picker movement. Applying a
theme must not clear editor text, selection, scroll positions, or other
application content. Metric changes require layout as well as repainting.
Define what happens to an in-progress pointer drag before implementing
metric reloads.

## Theme model proposal

Keep theme files human-readable using the shared configuration grammar.
Preserve compatibility with existing widget color keys. The theme property
schema should drive loading, validation, serialization, and the inspector,
so those consumers agree on names, types, and permitted values.

Offer a small group of useful palette roles, such as background, surface,
text, accent, and selection, with detailed widget overrides available.
Proposed local behavior: widget properties follow their palette role unless
explicitly overridden. Show that relationship in the inspector and provide
a way to return an overridden property to its role. Changing the accent
then updates its followers without erasing deliberate widget overrides.
These are links inside one composition; whether saved compositions remain
linked to external source components is a separate decision below.

Treat color, spacing, and rendering style as distinct aspects of appearance.
Explore built-in paint styles through libui's behavior/paint split. Loading
arbitrary executable theme engines is not proposed for the initial version.

Presets compose those elements: choosing a palette must not implicitly reset
the selected control style or spacing. Provide an explicit whole-preset
action alongside per-element selection. Show when a composition has personal
overrides, and allow saving the resulting combination under its own name.
Future decoration and font elements follow this same composition principle
when their supporting features are designed.

Proposed first editable components are palette, widget treatment, and
spacing. A widget treatment describes its supported visual states as well
as its resting appearance. Hover, pressed, keyboard-focus, and disabled
treatments need matching toolkit state handling. Glow and animated transitions
are design possibilities; they do not commit the first editor to an animation
engine. Prefer styles that consume palette roles so one treatment can work
with several palettes.

Saved compositions keep independent copies: selecting a palette or treatment
imports its settings into the draft, and saving preserves that composition
without following later edits to its sources. Source names may be retained
as provenance without making them live dependencies. This does not prevent
role-following properties inside the saved composition; its own accent role
can still control several widgets.

Window Decorations changes its own part of the running appearance through
its separate settings tool. Returning to the Workshop and overriding a
palette must preserve that decoration selection. Open decision: whether Save
captures the currently selected decoration along with Workshop-owned parts.

Define compatibility between elements as part of the theme schema. A paint
style may need particular metrics or capabilities; incompatible combinations
need an understandable explanation rather than silent substitution. Begin
with compatible built-in elements while preserving this contract for growth.

Reserve room in the design for decoration and font choices, without
inventing their keys or exposing nonfunctional controls. Current drawing
uses an embedded bitmap face; changing `font.w` or `font.h` in the existing
theme table does not select or scale a font. Validate supported font metrics
until an actual font-rendering extension is designed.

The Save and collection contracts above define directories, naming, publication,
and startup selection. The active appearance uses the shared configuration
search; the personal collection uses its write destination.

## Initial source-tree evidence

Audited on `userland` at `7e1b451`:

- `userland/libos64/include/os64/ui.h` defines per-UI theme storage and
  separate widget paint/event functions. Its theme table has 23 colors and
  six metrics.
- `userland/libos64/ui.c` loads `/home/theme.conf` at initialization. The
  parser and property table are private, and dispatch has no theme-reload
  operation. Resize handling already provides an application layout callback
  and full repaint path to learn from.
- `userland/libos64/include/os64/conf.h` provides shared configuration
  resolution and writing. `OS64_CONF_WRITE_MAX` is 16, smaller than a full
  theme. Extend or generalize the shared writer as appropriate; do not
  publish a theme through several partial saves.
- `abi/include/os64/gui.h` contains keyboard, pointer, resize, close, focus,
  and visibility events, but no appearance-change event.
- `kernel/src/gui/gui_client.c` and `kernel/src/gui/window.c` already park
  GUI event waiters and wake them when events arrive. The event ring can
  drop events when full; an appearance notification needs a reliable way to
  recover the current state.
- `kernel/include/gui/window.h` specifies a 20-pixel titlebar and one-pixel
  border. The kernel paints these independently of libui's theme.
- `DEBTS.md` records live theme reload and migrating the theme loader to the
  shared configuration search as unfinished work. The existing GUI event
  and wake machinery is more developed than the earliest design discussion.

These observations describe the audited checkout, not a claim about every
unmerged branch. Recheck relevant branches before proposing missing OS APIs.

## Live application: required behavior

Desired sequence: validate a complete candidate, publish the active
configuration, notify subscribers, load and validate into temporary client
state, replace the client's theme, relayout if necessary, and repaint.
Readers retain their last usable appearance if loading fails.

A GUI appearance event backed by a readable session generation is the chosen
transport, specified below with Chris and Fable's kernel-side rulings.
The generation store, publisher, and delivery/wake integration implement this
transport; the contract and client behavior are described below.
Do not reuse terminal hangup or another signal with unrelated semantics.

The final protocol must address:

- A full event queue, delayed clients, and repeated Apply operations.
- An application starting while an appearance is being published.
- Failure after saving but before notification, including recovery on retry.
- Multiple Workshop instances attempting to apply changes.
- The boundary between publication success and confirmation that a client
  has repainted; do not promise an instantaneous screen-wide transaction.
- Capability and ownership rules for publishers and subscribers if a new
  OS interface is needed.

Toolkit windows should gain reload support through shared libui handling.
Custom loops that consume theme values, such as the root menu, need explicit
integration. Audit those consumers rather than assuming every GUI app uses
libui dispatch. Custom drawings and terminal palettes need defined mappings
before being included in the appearance contract.

Any required kernel change will be described and discussed with Chris before
implementation. Decoration painting remains outside the widget-reload slice.

## Implementation sequence

### Apply slice: transport and kernel-side rulings

Apply precedes Save. The first publisher exposes the gallery's palette and
button treatment; it preserves unrelated current settings. Save and startup
selection are separate slices. No persistent file is changed by Apply.

`/sys/appearance` follows the clipboard precedent: an opaque ring-3 payload
held in kernel memory, cleared at reboot, under the existing single-user
policy. Two dedicated instances are acceptable. A third consumer triggers
extraction of a generic publication-slot seam, rather than adding a fourth
bespoke store. This trigger is recorded in DEBTS.md.

The envelope is text. Its first line is `generation = N`, terminated by a
newline; the remaining bytes are the opaque payload, at most 4096 bytes of
userland-owned theme configuration. The payload emitted by libui uses the
shared conf grammar, making `cat /sys/appearance` readable. The kernel parses
the generation integer and checks envelope framing and size limits; it does
not interpret the payload's keys or values. The header has a 34-byte bound in addition to the payload limit. Its canonical
spelling is exactly `generation = `, 1–20 decimal digits without leading zeros
(except zero itself), and LF. Generation zero plus an empty payload is the
initial state. The kernel can publish arbitrary payload bytes; libui accepts
only complete, supported theme configurations.

An open reader receives an immutable snapshot with the current generation.
A writer sends its expected generation in the same first-line format. One
complete write compares that generation and atomically publishes the payload
and next generation, or refuses without changing either. Writes are commands,
not a stream accumulated at close; a caller must not retry a suffix as a new
publication. Generation arithmetic must refuse overflow. Theme parsing,
property validation, and serialization stay in libui.

Publication and notifications share the GUI lock. Existing windows receive a
coalesced appearance-generation event outside their ordinary input rings,
participating in event-wait readiness. It is delivered before ordinary input
to avoid starvation during continuous pointer traffic. The generation uses
two 32-bit words in the event union, preserving its existing alignment and
32-byte ABI layout; `os64_gui_appearance_generation` reconstructs it. New
clients read the current snapshot at initialization. Delayed clients reload the latest generation and may skip
intermediate appearances. There is no new syscall or theme renderer in the
kernel.

The event is per window; snapshot loading and validation are per process.
Libui maintains a synchronized process-wide cache and generation so three
windows receiving the same generation do not perform three reloads. Each
participating UI context separately tracks which generation it has installed
and adopts the cached appearance in the loop that paints it. Deduplicating
the read must not suppress sibling windows' updates. Context initialization
checks the current store generation, since a process may have had no windows
when an update was published. Session properties merge into each context
without copying another context's geometry. Independent draft previews opt
out of active theme adoption. Invalid data does not advance the usable cache
generation. A separate observed generation deduplicates rejection; allocation
or I/O failures remain retryable. These APIs run in ordinary thread context,
not signal handlers. A yielding process-local lock serializes reloads and
publications without sharing mutable UI contexts between threads.

Userland loads startup `theme.conf` through the shared configuration ladder,
then resolves the active colors through a validated session snapshot. A pinned
startup override uses application defaults for keys absent from that override,
even when the disk file has since been replaced with the next-boot selection. A bad or unsupported snapshot
does not replace a running client's usable theme. Applying a component merges
it into the latest active appearance; a generation conflict is reported and
can be retried without silently discarding another publisher's changes.
The file write boundary reports generic refusal; libui rereads after failure
and reports a conflict if the generation advanced. It never retries a suffix
or automatically overwrites the racing publisher. A complete composition can
repair invalid session bytes; a component-only update refuses them because it
cannot preserve fields it could not decode.

The first Apply changes colors and button relief, so it requires repainting
without geometry changes. Spacing/font changes remain outside this publisher.
Toolkit clients reload in dispatch; custom theme consumers such as grootmenu
need explicit integration. Workshop keeps draft and active themes separate,
and applying must preserve editor text, selection, scroll position, and
application data. Publication success means the update is available, not that
every client has already presented a frame.

Chrome stays out of the appearance payload. Future window decorations get
their own small file with explicitly defined keys that the kernel parses.
The compositor must not extract titlebar colors or other decoration settings
from this opaque blob. A future saved theme may refer to both components,
but that does not combine their runtime interfaces or parsing ownership.

Implementation: `kernel/src/appearance.c` owns the bounded opaque store;
sysfs exposes immutable reads and complete-write publication. `ui_theme.c`
owns schema/validation/serialization, while `ui_session.c` owns the process
cache and publisher. Libui dispatch and grootmenu consume appearance events.
Control Center and the Workshop editor overlay active session settings after
their house defaults; the Workshop preview opts out of unsolicited updates.

The Workshop's initial Apply publishes its whole preview. After a successful
Apply, palette and treatment edits track their components separately, merge
against the latest session, and show the merged result after publication.
Applying an unchanged preview explicitly reapplies the whole composition.
Failures preserve the draft and report an actionable status. The editor's
text, selections, scroll position, and sample-control state are untouched.
Window chrome, terminal palettes, and application-specific drawings are outside
this widget appearance contract.

1. Completed: Control Center, launchable widget gallery, and reusable interaction
   foundation with composable palette and button treatment.
2. Completed: session publication, live reload, and explicit Apply.
3. Completed: theme collection, complete saved compositions, and startup selection.
4. Next: inspector, color picker, and further editing controls.
5. Refine Midnight Workshop and the other visual directions in the same tool.
6. Discuss window decorations after themes, retaining their separate interface.

Keep these slices independently reviewable. Settle each slice's contracts
before its implementation. Transport and save/publication decisions are
required for the slices that use them; they do not block a local widget
gallery or Control Center's launch behavior.

### First milestone proposal

The first runnable path is: desktop menu -> Control Center -> Appearance
Workshop gallery. Control Center uses the `settings` menu described above.
Launch errors appear in its status line with full diagnostics on the console.

Begin the gallery with existing panels, labels, buttons, text fields, text
views, and scrollbars. Present a Midnight Workshop palette and a classic
control-style experiment as local previews. These are design experiments;
the Apply implementation below makes the selected appearance available to
participating applications.

Proposed toolkit additions, in small groups:

- Interaction foundation: keyboard focus traversal and activation, disabled
  state, and pointer-hover state. Define how hiding or disabling a focused
  or grabbed control ends its interaction. Preserve the behavior/paint split.
- Choice and value controls: checkboxes, radio groups, sliders, and numeric
  inputs. These support settings tools as well as the appearance inspector.
- Collections and navigation: a selectable list, dropdown, and tabs, with
  keyboard behavior and scrolling defined where applicable.
- Appearance editing: reusable color swatches and a color picker, with numeric
  entry so a chosen color can be reproduced exactly.

This is a proposed development sequence, not a requirement to finish the
entire catalog before showing a working application. Add container layout
support where the gallery and inspector establish concrete resize needs.
The gallery should expose normal, focused, hovered, pressed, selected, and
disabled states where they have meaning for the control.

The initial native gallery has three independently selectable palettes and
flat/raised button treatment. Its text field is editable, its sample text
view supports selection and scrolling, and its sample buttons count presses
or reset sample state. It can apply the preview to the session; saving themes is a separate slice.
Small windows show an enlargement prompt instead of overlapping controls.

## Widget interaction slice

This slice adds keyboard traversal, hover, disabled state, checkboxes,
and horizontal sliders to libui, then exercises them in the gallery.
Buttons and checkboxes activate once on release of Space/Enter; key repeat
does not generate repeated activations. Pointer activation uses the left
button, preserves the drag-off cancellation contract, and is cancelled when
the control is disabled, hidden, or loses the relevant interaction ownership.

Tab and Shift+Tab traverse enabled, visible, focusable controls in tree order.
An editable text view retains literal Tab input; Ctrl+Tab and Ctrl+Shift+Tab
provide traversal from that view. A read-only text view can use plain Tab
for traversal. New focus and hover styling use theme fields rather than
application-specific paint constants. Disabled containers disable their
descendants for input. Runtime setters clear invalid focus/grabs and repaint.

Checkboxes own a checked state and notify their caller when interaction
changes it. Sliders use a bounded integer range and step, support pointer
dragging and keyboard arrows/Home/End, and notify on value changes. Setters
clamp values without simulating a user action. Controls with tiny bounds
must keep their paint inside those bounds.

The gallery includes an enable checkbox, a sample checkbox, and a 0–100 slider
with a live value label. Reset restores their initial state. Keyboard traversal
crosses the editor and preview trees. Window focus loss cancels held gestures
and hides the focus indicator while preserving its logical target for return.

### Pointer boundaries and queue pressure

Chris approved the kernel addition through the existing GUI event interface.
`OS64_GUI_EVENT_POINTER_STATE` carries content-local coordinates and an
`inside` flag. The compositor reconciles the exposed content target each
frame, accounting for stationary-pointer geometry/z-order changes, client
grabs, window-manager gestures, VT ownership, and window teardown. Additional
button presses during a client grab stay with its owner.

Pointer state is a coalesced snapshot outside the window's input ring. It is
delivered after queued input, so older motion cannot overwrite the final
leave state. Intermediate crossings can collapse; the most recent snapshot
survives a full input ring. The existing ring policy remains drop-newest for
ordinary input and oldest-event eviction for a focus change. The new snapshot
participates in event-wait readiness and wake delivery. It updates hover and
does not masquerade as a drag motion. No syscall was added.

## Later investigation: display ownership and fonts

Requested for later, not part of the first milestone: trace what Limine
provides and what os64 owns after boot, then establish how font rendering and
resolution selection could evolve. Distinguish boot-time mode selection from
runtime resolution changes, and software text rendering from display-device
programming. Check the actual framebuffer code and relevant hardware/boot
protocol documentation before proposing drivers or APIs. Include QEMU and
physical-hardware implications. No investigation or implementation of that
work is claimed by this draft.

## Validation plan

Use focused host tests for malformed/range-invalid themes, complete save/load
round trips, publication failures, and any new widget behavior. Exercise
notification overflow and startup/reload races for the chosen transport.

In QEMU, keep Scribe and other participating applications open while applying
several themes. Verify repaint, changed metrics, preserved application state,
focus and drag behavior, and a new application's initial appearance. Exercise
the custom-loop consumers as well as toolkit windows. Check save failures
and persistence after restart on the relevant filesystems; the current config
writer documents different replacement guarantees on ext2 and FAT.

Run the strict build and repository checks for implementation slices. A visual
mockup is design evidence, not proof that guest rendering or reload works.

### First milestone verification

- Strict userland and full image builds passed.
- `ASAN_OPTIONS=detect_leaks=0 tools/test_appearance_host.sh` passed palette
  composition, bounded bevel painting, and multiple-context render checks.
  LeakSanitizer cannot run under this environment's tracing; ASan and UBSan
  remained enabled.
- Headless QEMU q35 with eight CPUs and isolated disks passed the native
  menu -> Control Center -> Workshop launch path; independent palette and
  button treatment; sample text initialization, editing and Enter submission;
  button count/reset; selection; scrolling; and compact/restore resizing.
- A scratch catalog exercised nested categories, page navigation, and a
  visible missing-executable error. Closing Control Center left the Workshop
  usable. The scratch entries are not included in the shipped catalog.
- The final guest reported 30 pre-boot, 29 post-boot, and 3 late tests passed,
  with zero failures. This is QEMU coverage, not a physical-hardware claim.
- `git diff --check` passed. `tools/stale_refs.sh` reported the existing
  conceptual shorthand `draw_text` in docs/examples after the font paragraph
  was rewritten; those hits were reviewed. No drawing function was removed.

To try the milestone, build this worktree, boot a GUI entry, then choose
Control Center from the desktop's right-click menu. A personal `menu.conf`
needs the new root entry and `settings` menu copied into it, since it replaces
the system menu. `/bin/controlcenter` and `/bin/appearance` can also be
launched directly. Theme saving, the remaining widget catalog, fonts, and
window-decoration editing remain the later slices described above.

### Widget interaction verification

- Strict userland and full image builds passed. The focused host suite ran
  with ASan/UBSan; LeakSanitizer remained disabled for the tracing limitation
  described above. It covers keyboard/pointer activation and cancellation,
  focus traversal, editable versus read-only Tab handling, ancestor disabled
  state, checkbox callbacks, full signed-integer slider endpoints, stationary
  thumb clicks, tiny-control caption/checkmark clipping, and pointer snapshots
  across ring overflow and wrap-around.
- The final eight-CPU QEMU boot passed 30 pre-boot, 29 post-boot, and 3 late
  tests. The native gallery passed hover entry/leave/titlebar checks, disabled
  input refusal, slider drag/arrows/Home/End, held-key activation, focus loss
  and return, and traversal across the editor/preview trees.
- Stationary-pointer checks passed for maximize/restore, VT leave and return
  after moving the pointer on a text VT, window teardown exposing Control
  Center, and keyboard-driven creation of a new Workshop under the pointer.
  Palette/treatment changes, sample reset, text selection, and compact/restore
  geometry also passed. Queue-pressure coverage is in the host suite.
- A client drag retained ownership while a second mouse button was pressed
  over another window; returning and releasing activated its button once.
- `git diff --check` passed. The stale-reference scan reports conceptual
  shorthand `BUTTON_DOWN` and `draw_text` in prose; these hits were reviewed,
  and the corresponding event and drawing APIs remain present.

This slice provides the reusable controls and pointer notifications. Apply
is implemented by the session transport above; saving and startup selection
are described in the collection contracts. The theme/window-decoration relationship
remains a separate design discussion.

The disabled slider track retains a `disabled.fg` outline, matching the
other disabled control outlines instead of disappearing into the panel.
The outline follow-up passed the host suite and full image build; QEMU pixel
checks verified the disabled outline in all three palettes and the unchanged
enabled track.
Chris reported elevated Appearance/compositor CPU usage during sustained,
very rapid slider dragging. This observation has not been profiled; the
gallery currently renders both complete trees and publishes the whole window
when a sample changes. Performance work is deferred at his request.

### Userland refresh verification (2026-09-15)

The worktree base advanced from `7e1b451` to `d7f25fe` with the uncommitted
Workshop changes preserved. The source-list conflict retained upstream's
`walk.c` and standalone image-library layout alongside the Workshop modules.

- The appearance host suite passed before and after the update with ASan/UBSan
  enabled and LeakSanitizer disabled as above. The full strict image build passed.
- An isolated eight-CPU QEMU GUI boot passed 30 pre-boot, 32 post-boot, and
  3 late tests with zero failures. The menu -> Control Center -> Workshop path,
  three palettes, raised/flat buttons, sample activation, slider pointer and
  Home/End input, disabled styling, and reset were exercised and inspected.
- `git diff --check` passed. The stale-reference scan's `BUTTON_DOWN`,
  `draw_text`, and `docs/conf_path.md` hits still name live APIs or documentation.

This was a compatibility smoke test; the broader interaction checks above
were not repeated in full. This refresh preceded the Apply implementation;
Save was not part of this refresh; its subsequent implementation is described
in the collection contracts above.


### Apply verification (2026-09-15)

- The ASan/UBSan host suite covers framing and size refusals, maximum opaque
  payloads, immutable snapshots, stale and concurrent writers, component
  preservation, I/O failures, short-write refusal without suffix retry, invalid
  payload recovery, and retry after allocation failure. Concurrent sibling
  contexts read/validate once and each install the generation; draft contexts
  opt out. Appearance events survive full input rings and coalesce independently
  of pointer snapshots. LeakSanitizer remains disabled for the tracing limitation.
- The full strict image build passed. The isolated eight-CPU QEMU GUI boot
  passed 30 pre-boot, 32 post-boot, and 3 late tests with zero failures.
- `/tests/appearancetest` passed through the real sysfs/GUI interfaces: stat and
  directory listing, immutable per-open reads, mode/framing/stale-write refusal,
  a blocked event waiter in a sibling thread, independent context adoption,
  geometry preservation, invalid payload retention, and repair. This explicit
  fixture mutates the session and restores its initial usable theme; it is not
  included in unattended boot tests.
- GUI interaction verified Apply in open Control Center and Scribe windows,
  raised/flat treatment, all three palette compositions across the host/guest
  checks, and a newly opened themed menu. Scribe retained its 32-line unsaved
  document, the selection of lines 29–30, and its scrolled viewport.
- Two Workshops retained independent previews. After one published Electric,
  the other's treatment-only Apply retained Electric; the first's later
  palette-only Apply retained the new flat treatment. Each successful Apply
  showed the merged composition. The rebuilt image reports an external change
  without labeling an older preview as the current applied session.
- A fresh boot read `generation = 0` from `/sys/appearance`, after the preceding
  guest had published several generations. Persistent theme files are not used
  by the publisher. No P5 installation or independent P5 validation is claimed.
- `git diff --check` passed. The stale-reference scan's conceptual event/drawing
  names and `docs/conf_path.md` references were checked; those APIs and the
  document remain present.

### Save and collection verification (2026-09-15)

- Strict full `make -j8` passed without compiler warnings/errors. Both
  `tools/test_appearance_host.sh` and `tools/test_appearance_saved_host.sh`
  passed with ASan/UBSan (`ASAN_OPTIONS=detect_leaks=0`). Coverage includes
  list selection/cancellation/navigation/bounded painting, complete saved-theme
  round trips, invalid names/partial/oversized files, allocation/read/write/sync
  failures, atomic replacement refusal, competing no-replace saves, startup
  comment preservation and merged-output validation. Session tests cover the
  first startup preservation publication, a racing Apply, and publication failure.
- Eight-CPU QEMU boot checks passed 30 pre-boot, 32 post-boot, and 3 late tests.
  `/tests/appearancetest` passed with zero failures; `/tests/conftest` passed.
- Native UI checks passed: naming a variation, Save without Apply, replacement
  confirmation/cancel/accept, saved-theme reload, collection Home/End scrolling,
  unsaved Load and Close prompts, and cancellation retaining the draft.
- Saved files and startup snapshots survived guest restarts. Replacing a saved
  theme did not alter its prior startup copy. With a generation-zero session
  after a Paper boot, choosing Electric for startup kept the running Workshop
  and a newly opened Control Center on Paper. The following boot loaded Electric;
  Workshop and Scribe both showed it. The rebuilt guest also loaded the saved
  Electric composition and exercised the replacement prompt.
- `git diff --check` passed. `tools/stale_refs.sh` findings were reviewed:
  surviving shorthand for button events/drawing APIs and the live config-path
  document; no new superlative claims were reported.
- Evidence is under `/tmp/appearance-save-20260915/`, including
  `final-workshop.png`, `final-scribe.png`, host/build logs, and guest serial logs.
  Tests used private disk copies. The P5 was not modified by this work.
- Following installation, Chris reported that startup reload works correctly
  on the P5. This is user-reported hardware validation, separate from the QEMU
  checks above.


### PR #107 startup-preservation correction

- A regression reproduced the default-install failure: the first startup
  selection changed a Midnight context's surface from `ff202a36` to generic
  `ffc0c0c0`. Preservation now publishes a startup override with field presence,
  keeping absent keys at each application's defaults.
- Host ASan/UBSan coverage includes missing, empty, malformed, partial, and
  complete startup files; independently themed contexts; new-context and cold
  cache initialization after the disk file changes; racing Apply; read/allocation
  and publication failures; failed startup saving; repeated startup selection;
  and replacement by explicit Apply. Ordinary incomplete Apply payloads and
  preservation payloads containing geometry or invalid keys remain rejected.
- Both appearance host suites, the default strict build, `git diff --check`,
  and the stale-reference scan passed. Leak detection was disabled while
  ASan/UBSan remained enabled.
- In an isolated QEMU guest with a fresh home disk, selecting Paper for startup
  without Apply left the running Midnight Workshop and generic Scribe unchanged.
  A new Control Center retained Midnight, and a new Scribe retained its defaults.
  After reboot the Workshop used Paper. The guest appearance/configuration tests
  passed, alongside 30 pre-boot, 32 post-boot, and 3 late tests with zero failures.
- The correction changes userland only. The pending palette editor and rounded
  controls remain outside this PR.


### PR #107 resize-focus correction

Resize cancels held presses, drags, and hover through
`os64_ui_cancel_gestures`, retaining a valid keyboard focus target. The shared
libui resize path and Workshop's three-tree dispatcher use this operation.
Full interaction cancellation still clears focus for modal/tree transitions;
Workshop also clears it when its compact layout hides the editable controls.

The host regression failed before the fix and passes afterward: text fields
and editable text views continue receiving input after resize. Coverage also
checks held mouse/key activation cancellation, blurred focus, full cancellation,
and a layout callback hiding the focused control. The ASan/UBSan appearance
suite, strict build, diff check, and stale-reference scan passed. QEMU verified
continued typing through maximize/restore in Scribe, the composition-name field,
and the sample field; all 65 built-in guest tests passed.
