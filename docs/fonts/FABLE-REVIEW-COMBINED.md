# Fable's review of the combined font feature

**Reviewed:** `codex/font-settings` at `10e142d`, range `3b82356..HEAD`, from
[FABLE-FONTS-REVIEW.md](FABLE-FONTS-REVIEW.md). Read by one reviewer, line by
line, in risk order: the kernel Restore change, the F5 library
(`font_config.c`, `font_discovery.c`, `font_install.c`, `ui_envelope.c`,
`ui_session.c`, `ui_theme.c`, `ui_saved.c`, `ui_font_settings.c`, `conf.c`),
F3 (`gterm.c`, `font_grid.c`), then Workshop, Control Center, gclock, Scribe.
F0-F2 were reviewed as they landed and F4 is my own work under Quinn's
review; neither was re-read here beyond the seams F5 touches.

**Verdict: ACCEPT. No P1, no P2.** Nothing below blocks a merge. Four P3s,
two of them in the kernel change, and a short text-only list.

## What was run

On this HEAD, ASan/UBSan, all exit 0, counts identical to the receipts:

| Runner | Result |
| --- | --- |
| `tools/test_font_config_host.py` | 28,012,099 checks, 1,141 denial cases, no live allocations |
| `tools/test_font_provider_host.py` | 1,368,124 checks, 672 denial points |
| `tools/test_gterm_fonts_host.py` | 2,406,573 checks, 4,482 denial cases, zero live bytes |
| `tools/test_appearance_host.sh` | customizer, Control Center responsive, appearance: passed |
| `tools/test_appearance_saved_host.sh` | passed |
| `tools/stale_refs.sh` | nothing retired survives in prose; no new superlatives |

No guest boot was made for this review. Findings below are from reading and
say so where a path was reasoned rather than reproduced.

## Findings

### P3-1. A refused Restore says nothing (kernel/src/gui/window.c, `wm_set_maximized`)

The refusal is Chris's ruling and the mechanism is right: the flag and
`restoreFrame` survive, and a later font reduction makes Restore work again.
But the refusing branch is a bare `return`. `chord_report` and the
double-click site both print only when the flag CHANGED, so Ctrl+Alt+M on a
window whose saved rectangle is below its minimum produces no pixel and no
log line. A `printd(DEBUG_GUI, ...)` naming the window, the saved content
size and the minimum makes "why won't it restore?" answerable from the log.

### P3-2. The titlebar toggle can make Restore refuse with no font involved (reasoned, not run)

`wm_set_decorated` adjusts `w->frame` so the CONTENT keeps its size, but it
leaves `restoreFrame` alone. `wm_clamp_frame` derives content from a frame
using the CURRENT chrome. So: a window sitting within a titlebar's height of its minimum content height
(Workshop opens within a titlebar of it: a 960x728 frame over a 958x696 floor) -> Ctrl+Alt+T off ->
Ctrl+Alt+M -> Ctrl+Alt+T on -> Ctrl+Alt+M. The saved frame was measured
without a titlebar; re-read with one, its content is a titlebar short of the
minimum, the clamp grows it, and the new rule refuses. Before this change
the same sequence restored a titlebar taller. Toggling the titlebar off again
releases it, and with P3-1 unfixed the user gets no hint of that. The cure
that matches the function's own stated invariant is for `wm_set_decorated`
to apply the same delta to `restoreFrame` while MAXIMIZED is set.

### P3-3. One font diagnostic was left on stdout, and it repeats (userland/libos64/ui_session.c:275)

The logging follow-up moved the libui and gterm refusals to
`os64_debug_log`. `os64_font_settings_current` still `os64_printf`s
"fonts.conf: line N: ..." for a malformed startup file, and it is called
from `refresh_fonts` and gterm's `refresh_font_settings` on EVERY appearance
event. One bad `/home/fonts.conf` therefore prints that line to the
launching console once per libui window per palette change - the same VT1
noise F5-FONT-LOGGING.md retired, through the sibling it missed.

### P3-4. An install that the preview then refuses leaves the list stale (userland/apps/appearance/font_page.c, `use_file`)

`os64_font_config_install` succeeds (the file is now in `fonts/`), then
`show_candidate` can still refuse (the specimen cannot fit it). That branch
skips `refresh()`, so the installed font is on disk but absent from the list
until Refresh is pressed, and the status line says only that the preview was
kept. Refresh on install success, whatever the preview says.

## Text-only (fix without a round)

- `kernel/include/gui/window.h`: the `GUI_WINDOW_MAXIMIZED` block and the
  `wm_set_maximized` prototype comment still describe an unconditional
  restore. The new rule is half of what that flag now means.
- `gterm.c` lost WHY comments whose code survived: the grabbed-drag clamp
  rationale (the clamp moved to `gterm_grid_cell_at` without it), why the
  terminal paints its own cursor, the two-hop WM -> gterm -> SIGWINCH resize
  story with its PTY.md pointer, and the 100x38 lineage. House rule: a
  comment leaves with its code, not before it.
- `os64_ui_theme_read_startup` no longer names the file it rejected (the
  path went away with `os64_conf_find_read`). On a ladder, WHICH theme.conf
  was unusable is the useful half of that message.
- `controlcenter.c` `fit_text` is now a copy with an ignored parameter; the
  name promises fitting that the widget clip does.
- New code in tab-indented files (`gterm.c`, `conf.c`) is space-indented.

## Read and found sound (not findings)

- **Save/read ordering.** Save pins the usable session before replacing the
  file; readers read disk before the store. Walked both interleavings: a
  reader sees old-disk/no-store or new-disk/pinned-store, never next-boot
  choices in this session.
- **`fonts.serial`.** Stamped `s_seen + 1`, which is the generation
  `publish_locked` is about to create; palette writers carry it verbatim
  through `ui_envelope_merge`. A reasonable envelope addition to R3.
- **Envelope merge.** Owned duplicates are dropped, unowned lines and
  comments survive, `inherit` is pinned to line one, the complete payload is
  bounded at 4096 before publication.
- **Discovery.** Alias array is exactly roles x 3 and cannot overflow; dedup
  compares retained bytes, not labels or hashes; the 64 MiB budget bounds
  both reads and retained buffers; sort happens after the byte table is
  freed and aliases hold paths, not indices.
- **Install.** Dot-prefixed per-task temp (discovery skips it), full
  candidate validated against the STAGED bytes, sync, no-replace rename,
  EXISTS distinguished from IO after a lost race.
- **gterm transaction.** Prepare builds all 448 runs before touching state,
  the PTY resize is the only barrier, commit cannot fail, the double-buffered
  snapshot means a short read never tears the picture, and selection is
  gated on a matching snapshot.
- **Planner commits** in Workshop and Control Center make one syscall that
  the plan has already proven can succeed; quitting on the impossible
  failure is the honest choice over continuing with mismatched geometry.

## Observations for later, not for this merge

- gterm now paints per CELL (fill + one-glyph draw) where it used to batch
  runs; a full 16,384-cell repaint at 30 Hz is the case to measure on the P5
  if `top` or a scrolling build ever feels heavy.
- The Workshop size slider re-reads every role's font file per step.
- A crashed install leaves its `.<task>.<seq>.install` temp behind; nothing
  sweeps `fonts/` for them.
