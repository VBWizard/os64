# Frame Studio: independent button-symbol colors

Status: implemented after persistence and active-composition lookup. See
[verification](docs/frame-studio/symbol-checkpoint.md) for host and guest evidence.

## Problem and intended behavior

The minimize, maximize/restore, close and pin symbols default to the decoration's
active/inactive title-text colors. Bright button fills can require dark symbols
while a dark titlebar requires light title text. Independent ink enables that
combination.

Each button has a symbol-color choice alongside its fill:

- Follow title text, preserving the existing behavior by default.
- Custom active and inactive symbol colors, independent of the title and the
  other buttons. This permits black symbols on bright traffic-light controls
  while a pin on a dark or transparent fill retains a light symbol.

These symbols are painted shapes in `shared/decoration.c`, not title-font
glyphs. The shared painter supplies both the editor preview and live frames.
This slice changes symbol color, not symbol geometry or typography.

## Implementation boundaries

- Extend the prepared recipe/bundle with explicit inheritance and color values;
  preserve loading existing V4/V5 saved compositions with their current look.
  Freeze the versioned representation and migration before editing the format.
- Expose the choices through the existing color picker and selectable hex field.
  Keep title-text colors clearly named and distinguish symbol color from fill.
- Carry the settings through presets, preparation/restyling, Undo, Apply,
  Save/Load and the startup snapshot. Editing one symbol must not recolor the
  title, a sibling button, or an automatic housing as an unintended side effect.
- Define readable hover, pressed and unavailable states for opaque, automatic
  and transparent fills. Preserve button hit regions and window-management
  actions.

## Acceptance

Show white title text on a dark-blue titlebar with black minimize/maximize/close
symbols on yellow/green/red fills, plus an independently light pin symbol.
Verify active/inactive states, preview/live parity, Undo, save/load and startup
round trips, old-format compatibility, and failed preparation/publication
preserving the previous decoration. Use focused host tests, strict builds and
QEMU before describing the slice as implemented.

## V6 representation and migration

V6 extends the 296-byte V4/V5 header to 328 bytes by appending four pairs
of uint32 active/inactive symbol colors, indexed by action minus one
(close, minimize, maximize, pin). Zero inherits that state's title text;
FFrrggbb is a custom opaque color. Other alpha values are refused. Choices
belong to the action, so reordering or temporarily removing a button retains
its settings; spacers do not consume symbol colors. Presets reset inheritance.

The loader accepts V4, V5 and V6 with their respective header lengths.
A normalized editable recipe zero-fills the extension for older bundles.
Unedited saved/loaded assets keep their original bytes, including their active
fingerprint. Restyling emits V6, shifts asset offsets by the header-size delta,
and copies embedded glyphs, pairs and masks without reopening font sources.

Housing colors continue to use title text for their existing automatic hover
calculation. Symbol overrides are selected afterward. Hover keeps the chosen
ink; pressed also retains the one-pixel offset. Custom unavailable symbols
blend halfway with the painted pixel beneath each mark, including transparent
and bare controls over textures. Inherited symbols retain the existing disabled
appearance so older compositions render identically.

Finish lists distinct title-text, button-fill and active/inactive symbol roles.
Selecting a color makes that state custom; the Ink toggle returns it to title
text. The adjacent shortcut returns to that button's fill. Existing picker,
hex selection, keyboard list navigation and Undo serve these roles.
