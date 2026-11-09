# Terminal and application settings contracts

A ring-3 program can request roughly 41 MB of retained terminal cells at
512 columns and 10,000 history rows. The kernel must be able to refuse that
request without panicking or losing the existing terminal. This foundation
supplies three contracts; application settings and presentation consume them.

## Fallible optional allocation

`kmalloc_try` returns zeroed, HHDM-accessible memory or NULL. Zero length,
size overflow, physical exhaustion, metadata exhaustion, and insufficient
paging-table pool capacity refuse. Ordinary `kmalloc` keeps its existing
panic-on-exhaustion contract. Successful allocations are freed with `kfree`.

Under the allocator lock, `allocate_memory_try` selects an extent and prepares
its HHDM intermediate tables **before** carving ownership. A failed preparation
leaves ownership intact and maps no leaf data pages. Empty intermediate tables
already published remain reusable. The paging pool reserves pages with CAS;
its fallible entry returns zero at exhaustion, while the ordinary entry panics.
Concurrent table creation can consume a losing candidate page, as in the
existing table-publication policy; pool pages are not reclaimed here.

`tools/test_memory_try_host.py` executes production allocator and paging code.
The allocator harness substitutes host irq locking and a controllable preparation
boundary; it checks no carve/no zeroing on refusal, successful zeroing, and
metadata exhaustion. The paging harness checks eight-thread unique pool draws,
exhaustion, partial preparation/retry, absent leaf mappings, and overflow.
These host tests do not execute hardware TLB invalidation. QEMU exercises the
real allocator, HHDM mappings, and SMP boot paths.

## PTY history and coherent viewports

`pty_history` (60) sets 0..10,000 **additional physical rows** on a GRID PTY.
Capacity is independent of screen height. Zero disables history; growing keeps
live cells; shrinking retains the newest rows. It does not deliver SIGWINCH.
`pty_resize` (51) retains that capacity, preserves left edges without reflow,
and sends SIGWINCH only after a successful geometry change.

A global 128 MiB quota charges retained rows, including both old and replacement
history until the old ring is freed. Non-growing replacements bypass quota
refusal: their temporary overlap may take total history above 128 MiB, bounded
by twice that quota while replacements are in flight. Each PTY reserves its
old charge for one replacement at a time; growth refuses while usage is over
the quota. Live cells and viewport scratch are outside
that quota. A fresh GRID requests legacy three-screen history but falls back to
a live-only ring if that allocation is refused, records zero as its configured
limit so later resizes stay live-only, and logs the fallback under `DEBUG_TASK`. Physical or metadata exhaustion
can still refuse the terminal itself. At 512 columns the maximum history is
40,960,000 bytes; adding a 256-row live grid makes a 42,008,576-byte allocation.

Replacement is allocated outside the terminal lock, then geometry and capacity
are revalidated under the lock before copying and publication. Four attempts
blocked by another replacement or lost to revalidation return BUSY. Output arriving during allocation is copied from current state.
Shrinking does not refuse on quota grounds, including nonzero reductions;
physical/metadata/page-table exhaustion can still refuse its new allocation. A refused call publishes no change, though
other callers may concurrently change the terminal.

| Result | Meaning |
| --- | --- |
| 0 | History/resize succeeded, including a no-op |
| -1 | Invalid handle, mode, or range/geometry |
| -2 | Bad user buffer in viewport |
| `OS64_PTY_ERR_HISTORY_BUDGET` (-3) | Retained-row quota refused the replacement |
| `OS64_PTY_ERR_NO_MEMORY` (-5) | Physical, metadata, or page-table resources refused allocation |
| `OS64_PTY_ERR_BUSY` (-6) | Reconfiguration repeatedly raced another caller |

-4 stays reserved for the system-wide `OS64_INTERRUPTED` convention.

`pty_viewport` (61) returns a coherent 72-byte header and at most `max_cells`
cells. Zero capacity polls metadata. Logical line IDs anchor a reader across
output; an evicted anchor clamps to the oldest retained row. Geometry changes
advance the epoch; an old epoch returns the live screen. Capacity changes keep
the epoch. Both calls pin the master and reject STREAM mode. The legacy live
snapshot remains compatible.

`kernel/test/test_pty.c` tests ring retention, eviction, epochs, refusal
preservation, quota exhaustion, live-only birth, and quota reclamation.
`/tests/ptyhistorytest` exercises the syscall ABI, typed quota refusal, bounded
snapshots during concurrent reconfiguration, and legacy snapshot compatibility.

## Application settings and owned-window focus

`OS64_GUI_WINDOW_HAS_SETTINGS` opts a window into Settings actions.
`OS64_GUI_EVENT_SETTINGS` (15) is delivered by the titlebar control or Ctrl+Alt+S.
The application creates and manages its settings UI. Unsupported windows omit
the control and automatic gap in geometry, painting, and hit testing.

Decoration V7 adds the drawn slider symbol and configurable ink/housing/layout.
The shared painter and WM interpret the same filtered composition; V4/V5/V6
upgrade paths preserve older controls. `tools/test_decoration_host.sh` covers
legacy compatibility, capability filtering, geometry, hits, and icon colors.
The kernel window-minimum test covers Settings-capable frames.

`gui_window_focus` (62) raises and focuses an owned ordinary window within its
stacking band, restoring it if minimized. This lets repeated Settings requests
reveal one existing draft. Desktop and popup windows return BAD_ARGS; foreign
windows return NOT_OWNER. Invalid handles and an inactive GUI use the existing
`OS64_GUI_ERR_*` errors. `/tests/windowfocustest` checks ownership, focus events,
idempotence, special-window refusal, and stale handles in a GUI guest. Its
`--restore` fixture waits for Ctrl+Alt+N and verifies syscall restoration.

The syscall inventory is in [ABI.md](ABI.md); the product consumer is a separate
stacked change. This foundation builds and runs with the existing gterm.

## Validation

The standalone foundation built with strict `make -j4` and the existing gterm.
ASan/UBSan host runs passed 4,107 allocator and 4,636 paging checks, plus the
shared decoration suite. `ASAN_OPTIONS=detect_leaks=0` disables unsupported
ptrace leak discovery; the harnesses free their allocated test storage.

An eight-core QEMU GUI guest passed [27 PTY checks](docs/terminal-contracts/pty.txt),
[11 focus checks](docs/terminal-contracts/focus.txt), and [4 minimized-window
restore checks](docs/terminal-contracts/restore.txt). The [kernel harness](docs/terminal-contracts/kernel-tests.txt)
reported 31 preboot, 34 postboot, and 3 late passes with zero failures; its
NIC-dependent and unmounted-secondary-filesystem checks skipped.
`git diff --check` passed. `tools/stale_refs.sh` found references to
`PAGING_ARENA.md`, which remains present in `docs/design/completed/`.

### Fable round 2

`tools/test_pty_budget_host.py` reproduces the original fallback-resize and
shrink refusals against the production TTY source, then checks the corrected
paths, fallback diagnostics, exclusive replacement credit, growth while over
quota, and rollback after allocation failure. Its 22 checks pass under ASan/UBSan.
The guest test now fills remaining quota dynamically and verifies viewport
capacity zero before exercising resize, rather than merely creating another PTY.

Round 2 passed the strict full build, 27 guest PTY checks, and the eight-core
kernel harness (31/34/3 reported passes, zero failures; device skips retained).
[Before/after budget receipt](docs/terminal-contracts/budget-host.txt).
