# Graphical terminal scrollback and application settings

Status: implemented. User guide: [GTERM.md](../../../GTERM.md).
Validation: [gterm validation](../../gterm/VALIDATION.md).

## Ownership and scope

The graphical window titled `husk` is `gterm` displaying a GRID PTY.
Husk reads keys and writes terminal output; the kernel interprets that output
and owns the cell ring. Unconfigured rings request one live screen plus three
screens of history, with a live-only fallback if that allocation is refused. Gterm configures an independent row limit.
`os64_pty_snapshot()` exposes the live screen alone.

Implement the feature in gterm and its GRID PTY interface. Programs launched
with `gterm /bin/program ...` receive the same terminal features. Text VTs
and STREAM PTYs keep their existing behavior. Use the existing GUI wheel
events and libui scrollbar.

Capturing successive live-screen snapshots cannot provide reliable history:
output may scroll through multiple screens between frames, including while
the terminal is covered. Retain output history at the existing TTY writer.

## User experience

- Reserve a vertical scrollbar at the terminal's right edge. Its thumb
  represents the visible rows within retained history plus the live screen.
- Support thumb dragging, track clicks, mouse wheel, Shift+PageUp/PageDown,
  and Shift+Home/End. Consume these terminal gestures before forwarding
  their bytes to the child; ordinary navigation keys still reach the child.
- Follow new output while at the bottom. When browsing history, preserve the
  top visible line as new output arrives. If that line is evicted, clamp to
  the oldest retained line. Typing or starting a paste returns to live output.
- Paint the cursor in the live view. Preserve colored and attributed cells
  in history. Selection and copy operate on the displayed historical cells;
  changing views or invalidating their content clears the old selection.
- Resize uses the existing PTY policy: retain left columns, clamp the cursor,
  and carry displaced top rows into history. Width changes do not introduce
  paragraph reflow in this slice. Preserve the configured history capacity
  independently of the number of visible rows.

## Shared titlebar Settings action

Use an application-opt-in Settings titlebar control. Gterm is its first
consumer. The content area holds the terminal and its scrollbar. Right-click
remains paste.

The application advertises settings support for the window. The window
manager handles the control's placement, painting, hover, press capture and
activation, then delivers a Settings request to the owning application.
The app owns its fields, validation, runtime changes and persistence.
No application code or settings-file parsing runs inside the compositor.

Add Settings to Frame Studio's action palette alongside Close, Minimize,
Maximize and Pin. It has the same leading/trailing placement, ordering,
spacing, square/round/bare housing, fill and active/inactive symbol colors.
Symbol: three horizontal adjustment tracks with small offset slider
marks. The mark and housing are independent; a round button can contain the
three-line symbol. Test legibility at the supported 16-pixel minimum.

A composition's Settings slot appears for windows advertising support.
Unsupported windows omit that slot and its spacing/hit target; explicit
spacers remain intentional. Painting, hit testing, title placement and
minimum width must use the same capability-filtered layout. Frame Studio's
preview must exercise supported and unsupported windows through that layout.

Provide Settings in the built-in decoration and updated included presets.
Keep existing personal compositions readable and preserve their choices;
their users can add Settings in Frame Studio. Supply a keyboard route to
the same action so omitted controls and hidden titlebars do not make an
application's settings inaccessible. The chord is Ctrl+Alt+S. Version any expanded decoration data explicitly,
preserving old bundle/header readers and the fixed event-structure size.

## The first application settings dialog

Libui supplies a reusable, modeless settings-window helper: normal themed
window, content container, standard action row, validation/status area and
keyboard/focus handling. An app provides widgets and apply/save callbacks.
Keep the helper small and app-driven; gterm supplies the first real form.

Clicking the titlebar control opens `Terminal Settings`. Repeated requests
raise/focus the existing settings window. The settings window does not
advertise its own settings action. Closing the terminal also closes its
settings window. Dialog labels, fields and buttons use the shared Interface
font; terminal cells retain the Terminal font.

The initial field is **Scrollback lines**: additional terminal rows retained
above the live screen, not a screen multiplier or command-history length.
Default: 2,000 lines. Range: 0 through 10,000; 0 disables
history. Show the estimated cell-storage cost at the terminal's current
column count. A wrapped command may occupy multiple stored rows.

- **Apply** validates and changes this terminal's capacity immediately.
- **Save as default** applies the value and writes `ScrollbackLines = N` to
  `gterm.conf` through the existing configuration search/write helpers.
- **Close** discards unsubmitted field edits. Previously applied changes
  remain in effect; the dialog does not imply that discarded history can be
  restored by cancelling.

An increase retains current history but cannot recover evicted lines. A
decrease retains the newest lines; state that policy beside the field.
Other open terminal windows retain their own limits. New windows read the
saved default. Preserve configuration comments and unrelated keys, report
save failure separately from successful application, and leave invalid
input available for correction. A settings window must keep its terminal
session servicing output, paste, appearance changes, and hangup.

This establishes an application-owned settings pattern: typed local state,
shared widgets, and the existing conf reader/writer. It does not require a
new global registry of application preferences.

## Kernel dependency

The [kernel foundation](../../../TERMINAL_CONTRACTS.md) owns fallible allocation,
PTY history/viewports, refusal codes, Settings capabilities, and window focus.
The application consumes those contracts. Existing PTY calls and the 32-byte
live snapshot remain compatible; gterm uses the 72-byte viewport response to
keep geometry, history metadata and displayed cells coherent.

The consumer keeps a logical first-row anchor and geometry epoch. New output
preserves that anchor until eviction; a geometry change returns to live output.
The dialog distinguishes invalid input, history quota exhaustion, memory
refusal, and retryable contention. The 128 MiB quota charges retained rows,
including temporary replacement history; live grids are exempt. A refused
initial history request reads the actual capacity, including the live-only
fallback. Kernel implementation details and tests live with the foundation.

## Verification requirements

1. Cover the kernel interface and fallible-allocation contracts with focused
   ring/viewport tests.
2. Implement capacity and snapshot operations with lifetime, bounds,
   concurrent mutation and ABI compatibility coverage.
3. Implement the opt-in window capability, Settings request event, decoration
   action, Frame Studio support and keyboard route. Test capture cancellation,
   absent capabilities, old compositions, minimum width and shared previews.
4. Add the libui settings-window helper and convert gterm to a libui tree
   containing its custom terminal view and scrollbar. Keep terminal font
   adoption and PTY resizing transactional with the existing font-grid
   implementation.
5. Implement the dialog and config persistence, including invalid numbers,
   zero history, shrink/grow, allocation refusal, and save failures.
6. Run strict builds, focused host tests, kernel/guest PTY tests, and QEMU
   interaction tests. Exercise output bursts larger than a screen between
   frames, ring rollover, browsing during output, resize/font changes,
   selection/paste, settings edits while output is active, restart with a
   saved default, and multiple terminal windows with different limits.
7. Verify old snapshot consumers, STREAM PTYs and text VTs retain their
   contracts. Update affected code comments and PTY documentation together
   with the implementation, and run `tools/stale_refs.sh`.
