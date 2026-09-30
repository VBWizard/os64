# htop: os64's interactive process monitor

`htop` is a native terminal application built on the shared
[`os64_monitor`](../../../userland/libos64/include/os64/monitor.h) API. It adds
navigation and presentation to the same process, CPU and memory measurements
used by `top`. It is an os64 implementation, not a port of the upstream htop
program. No kernel interfaces are added by this slice.

Run `htop` in a text terminal, or choose **Process monitor** from the desktop
Tools menu. The menu launches `/bin/gterm /bin/htop`.

## The screen

The dark, sixteen-color layout uses cyan structure, green busy CPU time and
amber scheduler time. The top shows machine CPU use and allocated memory,
followed by individual core meters. The activity trail keeps up to 48 samples;
its ASCII characters become denser as busy plus scheduler time rises. A `?`
in that trail represents an unavailable reading, not zero activity.

The process table has task/thread ID, state, last core, CPU percentage,
allocator-reported heap memory, accumulated CPU time, and command. The selected
row stays attached to its identity across refreshes and sorting. A thread
identity includes its owning task. If the selected row disappears, selection
moves to a remaining neighbor; an open action confirmation is cancelled when
its pinned target disappears or changes name in a fresh sample.

Machine and core meters describe their whole CPU capacity. Process CPU usage
uses one core as 100%, so a multithreaded process may exceed 100%. HEAP is the
existing process reader's allocator figure, not RSS or total process memory;
it excludes code and stacks. A dash in that column means zero or unreported.
A dash in CPU usage means there is no valid interval yet.

The monitor includes kernel tasks but hides the CPU idle tasks by default:
those are kernel tasks named `idle` followed by digits. The `i` key reveals
them. Zombies are hidden until requested. These choices affect the view,
not the sampling history. Filtering searches the existing bounded name and
command summaries; the details panel reads the complete command separately.

## Controls

| Key | Action |
|---|---|
| Up/Down, j/k | Select a row |
| Page Up/Down, Home/End | Navigate the process list |
| Left/Right | Scroll the command horizontally |
| / | Edit a case-insensitive name/command filter; Enter applies, Esc cancels |
| Esc in the list | Clear the applied filter |
| s / S | Cycle CPU, heap, time, ID, name sorting / reverse direction |
| v | Toggle the parent/child process tree |
| t | Expand multithreaded tasks into thread rows |
| i / z | Toggle CPU idle tasks / zombies |
| Enter | Open task details; arrows and page keys scroll the full command |
| Space | Pause/resume sampling; navigation and resize still work |
| + / - | Faster/slower refresh, bounded to 100–60000 ms |
| [ / ] | Previous/next bank of core meters |
| x / X | Ask to interrupt / kill the selected task |
| ? or h | Help |
| q | Quit; in a panel, return to the list |
| Ctrl+C / Ctrl+D | Leave the monitor |

F1, F3, F5, F6, F9 and F10 are also decoded as help, filter, tree, sort,
interrupt confirmation and quit when the terminal delivers their escape
sequences. Letter bindings remain available independently of function keys.

Command-line options: `-d/--delay MS`, `-t/--threads`, `-v/--tree`,
`-i/--idle`, and `-z/--zombies`. Refresh defaults to 1000 ms. Invalid delays
are rejected. The argument parser's usual help options also work.

## Actions and terminal ownership

`x` uses the existing `interrupt` ctl verb; `X` uses `kill`. Both require a
visible confirmation and an explicit lowercase `y`. A thread row targets its
owning task, including that task's threads. Kernel tasks, zombies and htop
itself are refused by the UI; the kernel also enforces its own restrictions.

The question stores the target ID and name, independent of the selected row.
Input queued before opening the question or during its first paint is drained
with a bound; reaching that bound cancels the question. The application re-reads the task immediately
before sending the ctl command, refuses changed/dead targets, and does not
retry partial writes as another command. The kernel's monotonic task-ID
contract is the same identity assumption used by the monitoring library.

The event loop owns the borrowed monitoring snapshot. It rebuilds row indices
before drawing after a new sample, and never keeps a pointer into a previous
sample. Full-command text is a separate owned allocation, released when the
details panel closes. Details show live task fields alongside the command
captured when the panel opened.

Input and repainting do not reset the sampling deadline. Pausing retains the
current snapshot; on resume the first interval spans the pause. Enabling
threads while paused takes effect on resume. A failed sample clears the view
and is retried instead of relabeling old values as live.

A bounded cell grid is composed in memory and changed rows are emitted with
cursor addressing and sixteen-color SGR. Process data is converted to printable
ASCII before output, preventing embedded control bytes from becoming terminal
instructions. Drawing leaves the physical last column unused to avoid wrap,
and terminal geometry is checked periodically and on SIGWINCH. The practical
minimum is 60 columns by 18 rows; smaller windows get a resize message. Drawing
is capped at 240 columns by 100 rows, leaving any extra area unused.

htop uses its controlling terminal for input and stdout for drawing. It must
run in the foreground. Raw input is restored on ordinary exit and handled
signals; the default terminal colors and a clean screen are restored as well.
The terminal does not provide an alternate-screen buffer, so the previous
screen is not restored. An uncatchable kill cannot run application cleanup.

## Boundaries and verification

The application retains up to 512 tasks, 512 thread rows and 128 core records.
A reached capacity or incomplete source is shown as a partial sample. Core
meters are paged; the process list scrolls. Tree traversal uses bounded storage
and a visited set to tolerate missing parents or cycles. Filtered-out parents
leave their matching children as roots.

The application leaves mouse interaction, priority/affinity changes, stop/resume
job control, RSS/PSS and disk/network panels for separate slices. The shared
monitoring layer remains independent of this UI and available to a future
graphical Task Manager.

`tools/test_htop_host.sh` runs ASan/UBSan checks against the real model, renderer,
action code, and event loop: stable identities, filtering, tree cycles, thread
IDs, stale confirmations, queued approvals, single ctl writes, terminal-size
bounds, input decoding, full-command ownership, and terminal cleanup.
`tools/test_monitor_host.sh` covers the shared measurement engine. Guest
validation passed text-VT filtering, details, tree/thread views, pause, and a
confirmed interrupt against a disposable sleep task. Normal quit returned 0;
Ctrl+C returned 130 and restored cooked input. In `gterm`, the display survived
shrinking below the minimum size and enlarging again while paused, then resumed
live sampling and thread expansion.
