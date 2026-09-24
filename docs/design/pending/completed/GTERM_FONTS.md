# gterm font integration (F3)

F3 consumes the shared [font provider and adoption contract](FONT_PROVIDER.md).
The source boundary is `userland/apps/gterm/font_grid.h`; completion evidence is
in [F3-REPORT.md](docs/fonts/F3-REPORT.md). It changes userland terminal rendering,
not PTY byte interpretation or the kernel ABI.

## Geometry and painting

The provider's TERMINAL role validates the primary face and supplies integer
cell width, row height and baseline. gterm uses these metrics for initial window
content, grid dimensions, foreground/background cells, cursor, pointer mapping
and selection. Initial chrome comes from `os64_gui_frame_for_content`, rather
than a local decoration estimate. The default requests 100 columns by 38 rows.
Bounds are 2–512 columns, 2–256 rows and at most 16,384 cells. Surface division
rounds down; incomplete right/bottom cells remain background. An invalid proposed
grid is refused rather than silently changing the child's reported geometry.

Each installed font plan retains 448 F2 one-cell runs: bytes 32–255 in Latin-1
and CP437. Interpreted low control/blank bytes paint as spaces. This finite layout
collection is prepared in advance so future PTY output cannot require allocation
at paint time. It is not a glyph cache: font fallback, glyph lookup, rasterization,
mask sharing and eviction belong to F2. The layout table retains shared masks.
This trades preparation work and retained run storage for allocation-free drawing
and a genuinely final PTY barrier. There is no application-owned bitmap atlas.

ANSI colors and attributes are resolved before drawing each cell; selection swaps
those resolved colors. Background fill and the F2 foreground mask use the same
cell rectangle. The caller clips to that rectangle as well as F2's grid clip.
The cursor uses the same rectangle. Missing box/block glyphs use F2's procedural
fallback at the selected cell dimensions. Unknown charset values retain the
legacy Latin-1 default. Clipboard copying preserves high bytes; it does not
convert the terminal into a Unicode byte stream.

## Adoption and ownership

`gterm_grid_consumer(&grid)` returns the common `os64_font_consumer_t` descriptor.
Initialize the grid with allocator callbacks, current surface width/height, a PTY
resize callback and an infallible invalidation callback. The UI thread serializes
adoption, window resizing, input and drawing. Use active-only geometry/draw helpers
after the first successful adoption and before `gterm_grid_destroy`.

1. Prepare reads the borrowed TERMINAL view, validates the proposed grid, retains
   the whole candidate set, and constructs the complete run collection. Failure
   destroys the partial plan without changing active state.
2. The barrier calls the existing PTY resize callback if dimensions differ. A
   failed callback must leave the external grid unchanged. There is no PTY call
   for a font change that preserves row/column counts. Startup has no PTY yet;
   the callback succeeds locally and PTY creation follows the installed geometry.
3. Commit swaps the plan and dimensions, invalidates snapshot/selection/drag, then
   releases the old runs and set. It neither allocates nor fails. Abort releases
   the candidate plan. Candidate constructors and the coordinator own their
   respective references; the installed plan retains its own reference.

F2 accounts the fonts, runs and masks against its context budget. The app allocates
one fixed-size plan outside that budget (a role view, 448 pointers, set reference,
and two dimensions); active and proposed plans coexist. Its allocator is injectable
for failure tests. Snapshot storage is two fixed arrays of 16,384 PTY cells
(256 KiB total). There is no allocation proportional to untrusted window dimensions.

A failed font change preserves active identity, dimensions, displayed snapshot,
selection and drag. A successful change clears selection/drag even when the grid
counts match: the picture and pointer metrics must agree after the switch.
A window resize records the actual surface size, proposes a new grid and changes
local grid dimensions only after PTY success. Refusal keeps the accepted grid
clipped/letterboxed in the new surface. Successful geometry changes invalidate
selection and drag; a surface resize within the same cell counts can retain them.

## Snapshot publication

Header-only polling uses a separate header. Full reads go into the other fixed
cell buffer. gterm accepts a read only when header dimensions equal the installed
grid and the return count equals the complete cell count. It then swaps header
and cells together and paints. Short, failed or mismatched reads never overwrite
the displayed snapshot. A failed full read is retried on a later frame, including
after a committed font/PTY change; it does not pretend to roll that change back.
Pointer selection is disabled while no matching snapshot is installed. Existing
pixels may remain until the first successful post-change read, but are not used
as the new grid's selection source. Header errors/session hangup end the session.

## F5 seam and test injection

Production starts with a shared builtin set. F5 supplies a resolved candidate
through `replace_fonts` (or uses `gterm_grid_consumer` in the process's adoption
batch). Refresh surface metrics before preparing against a newly changed window.
Only this consumer contributes the PTY barrier; the coordinator still permits
at most one external barrier in a batch. F5 owns configuration, publication,
status messages and generation bookkeeping. No font file option, environment
variable, discovery path or persistent selector is added to gterm.

`/tests/gtermfonttest` compiles the production gterm implementation and consumer.
Its event-poll wrapper intercepts test keys; painting, pointer events, copying,
window resize, snapshot handling and child lifetime still run through gterm.
A bounded fixture reader supplies resolved font bytes. A child installs SIGWINCH,
reads `/proc/self/tty`, and paints text/ANSI/CP437 specimens. See the report for
fixture keys and the generated font whose box/block mappings are absent.
