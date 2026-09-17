# F4 checkpoint 1 — widgets on measured runs, and the consumer adapter

2026-09-17, Opus. Worktree `.worktrees/font-widgets`, branch `opus/font-widgets`,
foundation `788de9900282e941a7a93aa110ffa69154daa066` (F2.5), this checkpoint
`d11608b`.

**This is not F4 complete, and it is not asking to be merged.** It is the first
of three checkpoints (Chris's cadence: widgets, then editor + Scribe, then the
bounded long-line window). It is submitted for review NOW because it is where
the API SHAPE got decided, and checkpoints 2 and 3 are built directly on top of
it. A contract change costs hours today and a rewrite later. `F4-REPORT.md`
follows at completion with the packet's full evidence.

Reviewed against: the F4 packet, [FONT_PROVIDER.md](../../FONT_PROVIDER.md),
[OPUS-F4-HANDOFF.md](OPUS-F4-HANDOFF.md).

## What is delivered

Labels, buttons, checkbox captions and list rows measure and paint through F2
runs instead of multiplying by an 8x16 cell. A window owns one text context and
one immutable role set, and its font can be replaced through `os64_font_adopt`.

`ui_text.c` is untouched — textfields, textviews and Scribe are checkpoint 2.

New: `userland/libos64/ui_font.c`, `tools/test_ui_text_host.{c,py}`,
`userland/tests/uifonttest/`.

## The API, and the initializer names the handoff asks for

All in `os64/ui.h`. The generic consumer descriptor F5 depends on comes from:

| Call | What it is |
|---|---|
| `os64_ui_font_consumer(ui, out)` | **The adapter initializer.** Fills an `os64_font_consumer_t` whose `user` is the `os64_ui_t`. Hand it to `os64_font_adopt`. No barrier — F3 owns the one barrier a batch may have. |
| `os64_ui_font_context(ui)` | The window's `os64_text_context_t`, built on demand and owned by libui. F5 prepares its candidate set on **this** context so one window adopts from one context. |
| `os64_ui_font_bind(ui, set)` | The startup door: install a set outright, taking a reference. There is no old layout to preserve, so it cannot half-succeed. |
| `os64_ui_font_planner(ui, plan, commit, discard, user)` | The application's layout, planned against the candidate. |
| `os64_ui_font_metrics` / `os64_ui_font_row_height` / `os64_ui_control_min_height` / `os64_ui_text_width` / `os64_ui_draw_text` | Measurement and painting. |
| `os64_ui_font_set` / `os64_ui_font_status` / `os64_ui_font_live_bytes` / `os64_ui_font_restamp` / `os64_ui_font_release` | The set, why there isn't one, the engine's live bytes, a manual restamp, teardown. |

The plan type behind the descriptor (`ui_font_plan_t`) is private to `ui_font.c`.
F5 never sees it, per font_adopt.h's rule.

**The planner contract.** `plan` returns `os64_font_status_t`: LIMIT when the
layout will not fit the content area, NO_MEMORY when staging could not be
allocated — different answers deserving different next moves. Anything but OK
fails the whole adoption with the old state intact. It measures through the
metrics calls, which report the **candidate** for the duration of prepare;
widget bounds still describe the installed face and only move at commit.
`commit` applies the staged layout and cannot fail; `discard` frees it.

## Four boundary questions

These are the decisions I would rather have argued now than inherited.

1. **Is the planner hook the right seam?** FONT_PROVIDER.md asked for "an
   F4-owned application planning hook or owned layout plan"; this is a
   registered triple on the UI rather than a parameter to the adapter. Its real
   workout is Scribe in checkpoint 2, so this round can validate its shape but
   not yet its sufficiency — if it is going to be wrong, it will be wrong about
   something Scribe needs and a fixture does not.

2. **The `metrics` callback on `os64_ui_class_t`.** It exists because
   `os64_ui_listbox_rows(list, theme)` is handed a THEME and needs a row pitch,
   and the theme's `font.w/h` are frozen at 8/16 by the handoff. Rather than
   break a public signature an application already calls, the listbox caches its
   own pitch and the class re-derives it. The cost: a fifth field in a public
   struct, so every class literal in the tree had to name it under
   `-Werror=missing-field-initializers` — including two in `appearance.c`.

3. **A one-row control's height now belongs to its class.** `label_metrics`,
   `button_metrics` and `checkbox_metrics` set `bounds.h`. This is a real
   behaviour change for an application that sized a label by hand: it will be
   overwritten when the widget joins a tree and again at each font change.
   I chose it over a separate "natural height" field after the separate field
   failed on the glass — `os64_ui_stack_vertical` writes `bounds.h` back, so
   "the app chose this" and "we computed it last time" were indistinguishable
   and labels stayed at their first face's height forever.

   The one instance in the tree today is `gclock`, which sets
   `gLblClockText.bounds.h = gUi.theme.font_h` by hand. Under the builtin face
   the class writes the same 16, so nothing moves; under a real face the class
   gives it the taller row it would have needed anyway. That is the change
   behaving well, not an argument that it always will.

4. **Per-window text context, or per-process?** I chose per-window: a window is
   the coherent adoption group, and the Appearance Workshop already draws a
   preview beside the live article. FONT_PROVIDER.md permits sharing a context
   across windows (only the SET must differ for a preview), and sharing would
   mean one glyph cache per program instead of one per window. If F5's preview
   story is happy with shared-context/different-set, per-process is cheaper and
   I would rather move before two more consumers exist.

## Cache and invalidation, as it stands

- **Runs are laid out per draw and released.** Widget captions are app-owned
  `const char *` that can change under libui at any moment, and F2's context
  already caches the expensive half — the rasterised glyph images — so a
  re-layout is re-placement, not re-rasterisation. A retained per-widget run
  cache would need an invalidation key over (bytes, length, set identity) and
  has no measured problem to solve yet. The DOCUMENT side is different and
  checkpoint 2 will need per-line runs keyed by document revision; that design
  goes in the F4 report.
- **The set is the invalidation unit.** Commit swaps the set, restamps every
  widget's cached geometry, then marks the whole window dirty: every run in it
  was laid out against the retired set.
- **Order is load-bearing.** Widget geometry is restamped BEFORE the
  application's commit callback. A layout arranges widgets by the heights they
  report; restamping afterwards arranges the window against the face it just
  stopped using.

## Deliberate behaviour changes

- **Tabs and control bytes.** The bitmap painter drew the PSF1 glyph at the byte
  index and advanced one cell. A run obeys F0: a tab advances to the next stop
  and draws nothing, any other control byte is one missing-glyph marker.
  Asserted in the host suite rather than hidden.
- **Widget text is UTF-8 W1**, per FONTS.md ("Terminal byte/charset
  compatibility remains distinct from Scribe's UTF-8 policy"). A raw Latin-1
  high byte in a caption is now malformed input and draws a marker. No shipped
  application has one — checked across `userland/apps` and `userland/libos64`.
- **Control minima come from the face.** `theme.button_h` (28) is the floor, not
  the answer; it was written for an 8x16 cell and clipped captions at 24px.
- **Metrics never build a font engine.** A widget re-derives geometry when it
  joins a tree, long before anything is painted. Metrics answer from the 8x16
  fallback, which is the same face the builtin set holds; the host suite asserts
  that equality rather than trusting it.

## Evidence

**Host** — `tools/test_ui_text_host.py`, which compiles the real `ui_font.c` and
provider against stubs for the heap and the window system:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real          # 85 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py --real -O 0     # 85 checks
ASAN_OPTIONS=detect_leaks=0 python3 tools/test_ui_text_host.py                 # 61 checks, fake backend
```

Covers: surface-exact identity against the bitmap painter over printable
samples and at six clip widths from 0 to 35; the contracted tab and control-byte
differences; builtin metrics and the fallback equality; proportional widths
differing per character under real DejaVu and Source Sans; row height growing
with nominal size and never equalling it; the adoption transaction — a refusing
planner leaves the set, the row and the widget geometry untouched, a committing
one installs the candidate and the staged layout; geometry restamped before the
application's commit; zero live engine bytes after release.

The restamp-ordering check was confirmed to FAIL against the original ordering
before the fix was kept.

**Neighbours, unchanged:** `tools/test_text_host.py` 7,161 checks (F2), and
`tools/test_appearance_host.sh` green including its LeakSanitizer pass — which
is what caught the engine being built at widget-attach time.

**Build:** full default-strict `make` and `make -C userland` clean, `git diff
--check` clean, `tools/stale_refs.sh` reports no retired names and no new
superlatives, `make fsck-ext2` green on root and /home.

**QEMU** (`/QEMU GUI Boot`, ext2 root, 1024x768, headless with screendumps):
`/tests/uifonttest` builds a panel, three labels, a checkbox, a listbox and two
buttons, and replaces the window's font through the production adoption path
with fixture bytes from `/tests/fonts`. Verified by eye and by Chris:

| Evidence | Shows |
|---|---|
| `f4-evidence/cp1-builtin.png` | builtin 8x16, row 16 baseline 12 — unchanged appearance |
| `f4-evidence/cp1-dejavu24.png` | DejaVu Sans 24px, row 29 baseline 23; proportional widths, controls sized to the face, list given the remaining height |
| `f4-evidence/cp1-sourcecodepro-24px.png` | Source Code Pro (OpenType/CFF) 24px |
| `f4-evidence/cp1-refused.png` | an armed refusal: face, layout and status retained, nothing half-applied |

**Not run:** P5 / real hardware. No kernel change is involved and nothing here
touches a syscall, but that is an absence, not a result.

## Shared hunks (Quinn owns final build/image integration)

- `userland/GNUmakefile` — one filename, `libos64/ui_font.c`.
- `userland/apps/appearance/appearance.c` — two `NULL`s, forced by the fifth
  field in `os64_ui_class_t` under `-Werror=missing-field-initializers`.
- `tools/test_appearance_host.sh` — the font sources plus a link-time stub
  returning the fake backend, since the widgets now measure through the
  provider and that harness has no FreeType.
- `tools/test_appearance_customizer_host.c` — releases the two bindings its
  windows acquire, so the suite's leak check stays meaningful.

## Known, and deliberately not done here

- `ui_text.c` is on the monospace grid. Textfields, textviews, Scribe, the
  document run cache and source-byte positions are checkpoint 2; the bounded
  >1 MiB line window is checkpoint 3.
- `theme.checkbox_size` is a fixed 16px square, so the indicator looks
  undersized beside 24px text. It is a theme metric, not a font metric, and
  raising it is a product decision rather than a silent redefinition.
- Window chrome stays bitmap: the kernel compositor paints titlebars from its
  own constants, which ui.h's scope note already records.
- The desktop will look MIXED until the remaining consumers migrate. Anything
  built from libui widgets came along for free — `gclock` and `uiprobe` are
  already measuring. What still paints its own text through `os64_draw_text`,
  and so is still 8x16: `grootmenu`, `glogo`, `fpu_orbit`, `appearance`'s two
  sample classes (its widgets are migrated; the gallery samples paint by hand),
  and the fixtures `gkeys`, `fpu_demo`, `texttest`, `windowmintest`. `gterm` is
  on that list too and is F3's, not mine.
