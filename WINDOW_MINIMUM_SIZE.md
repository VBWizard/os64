# Per-window minimum content size

## Scope and interface

This foundation is independent of the Appearance Workshop customizer. The
Workshop needs a 958x696 drawable area to keep its editor and live preview
visible. Its opt-in call belongs in the customizer follow-up; this change adds
reusable WM support and a guest fixture.

`os64_gui_window_set_min_size(handle, width, height)` (syscall 57) sets minimum
**content** dimensions, excluding decorations. The owning task can call it
repeatedly; zero or a value below the default normalizes to the existing 64x32
resize floor. Windows start with that floor, preserving existing behavior.

Both limits must fit the window's existing canvas capacity. Failure returns
`OS64_GUI_ERR_BAD_ARGS` without changing either limit or geometry. Handle and
ownership checks use the existing locked GUI lookup. Raw syscall dimension
registers above UINT32_MAX are rejected before narrowing. There are no user
pointers, new allocations, or canvas remappings in the setter.

Success returns zero. A window smaller than the minimum grows immediately at
its current origin through `wm_resize`, with the existing resize notification
and damage handling. Lowering a limit does not shrink the window. The caller
re-fetches its surface after success before drawing. Same-value calls do not
cancel gestures; changed limits cancel the active WM resize outline for that
window, so an outline computed under old constraints cannot later commit.
The WM retains ownership of consumed mouse presses through their releases,
including additional buttons pressed during the gesture. Cancelled geometry
has no retained window pointer, so destroying the target is safe. Mouse events
remain consumed on a text VT; client hover resumes after the grab drains.
Client pointer grabs are unaffected.

## Window operations

The rubber-band outline, drag commit, maximize, and restore share
`wm_clamp_frame`. Its content floor now comes from the window. North/west drag
edges retain the existing opposite-corner anchoring. Decoration toggles retain
content dimensions and therefore retain the minimum without scaling it.
Restoring a frame saved before a larger minimum was set clamps the saved frame
under the new limits.

Minimum content size takes precedence over fitting the screen, consistent with
existing support for windows larger than the screen. A request beyond the
canvas reservation is refused; an accepted minimum can still make a decorated
frame extend past screen edges. Applications must choose a minimum appropriate
to the supported desktop. Screen-size fallback layouts and font scaling are
separate concerns.

## Verification

Kernel regression coverage exercises decorated, undecorated and desktop clamps,
minimum and capacity bounds, and unchanged legacy floors. The `windowmintest`
guest fixture covers the syscall, immediate growth, preserved canvas address
and stride, rejection atomicity, reset behavior, and interactive resize,
maximize/restore and decoration changes.
Validation on 2026-09-16:

- `make -j8` passed with the repository's `-Wall -Wextra -Werror` build.
- `ASAN_OPTIONS=detect_leaks=0 tools/test_appearance_host.sh` passed.
- QEMU 1024x768, 8 CPUs: 31 pre-boot, 32 post-boot and 3 late built-in tests
  passed, including `window_minimum_clamp`.
- `/tests/windowmintest` and its `--hold` run both reported PASS, zero failures.
  The separate child was refused when it attempted to set its parent's limit.
- All four drag corners stopped at 958x696 content. Maximize reached 1022x747;
  restore returned to 958x696. A saved 658x480 restore frame was clamped to
  958x696 after the minimum was raised while maximized. Hiding the titlebar
  preserved content dimensions. Reset allowed shrinking back toward the
  default floor (64x46 observed in the drag).
- A delayed setter raised the minimum during an active outline; it cancelled
  the outline, grew the window, and the later mouse release did not commit
  the old geometry.
- `git diff --check`, `tools/stale_refs.sh`, and a read-only `e2fsck -fn` of
  the flushed private home filesystem passed.

Screenshots, serial output, build logs, fixture results and the QMP gesture
script are in `/tmp/window-minimum-size-20260916` on the development machine.
No P5 deployment was performed. Appearance Workshop adoption remains in its
separate customizer follow-up.

## Review follow-up: keep mouse ownership after cancelling the outline

The original cancellation cleared the outline's window pointer and also lost
its implicit WM pointer grab. Releasing the right button over a client then
produced a release without a matching client press. The compositor now tracks
WM-consumed buttons independently of the outline. It drains each button edge
through cancellation, target destruction and text-VT handoff. Additional
buttons are consumed while a resize is active and while its cancelled grab
is draining; hover stays suppressed until those releases finish.

`windowmintest --hold` checks client press/release pairing. Its `g` key changes
the minimum after three seconds, leaving time to start a resize; `d` replaces
the target window after the same delay. The original implementation failed
with zero client presses and one release. The corrected QEMU run reported
four intended client presses, four releases and zero failures after exercising
cancellation, extra buttons (including a combined release), target destruction,
a text-VT release, and ordinary resize/maximize/restore afterward.

The strict build, ASan/UBSan appearance suite, all 66 built-in tests,
whitespace/stale-reference checks and private-home filesystem check passed.
Evidence: `/tmp/window-minimum-review-108`, including `before.log`, `after.log`
and the QMP reproduction/verification scripts. No P5 deployment was performed.
