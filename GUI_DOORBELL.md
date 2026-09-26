# Window doorbell

A worker thread can wake its application's GUI event loop with
`os64_gui_event_ring(window, mask)`. The caller must belong to the task
that owns the window. This is the first slice of
[the concurrency packet](docs/yonder/07-concurrency.md); the generic work
pool and browser fetching policies are separate work.

## Using the callback

The worker publishes a result, rings the window, and continues or exits.
The kernel records the notification and wakes the window's waiting UI
thread. When that thread resumes its event loop, libui calls the
application's `on_doorbell` function. The kernel does not call the C
callback directly, and ringing does not execute it on the worker.

Choose application-defined bits for the reasons you want to distinguish:

```c
#include "os64/ui.h"

enum {
    IMAGE_READY = 1u << 0,
    SCAN_READY  = 1u << 1,
};

static void background_ready(os64_ui_t *ui,
                             const os64_gui_event_t *event)
{
    if (event->doorbell.mask & IMAGE_READY) {
        // Drain ready images from the application's synchronized work
        // record, then update the corresponding widgets.
    }
    if (event->doorbell.mask & SCAN_READY) {
        // Collect the completed directory scan and update its list.
    }
    os64_ui_mark_dirty(ui, ui->root);
}
```

This is a callback skeleton: collecting results and updating widgets are
application code. In an application that already created its window,
draw context and widget tree, install the callback AFTER `os64_ui_init`
(initialization clears it), then run the usual loop:

```c
os64_ui_init(&ui, &ctx);
os64_ui_set_root(&ui, &root);
ui.on_doorbell = background_ready;

// Start the application's worker threads after setting up their state.
os64_ui_run(&ui, window, NULL);

// Request cancellation and join the workers before destroying the window
// or freeing any shared job state they still use.
```

On a worker, after publishing the result through the application's lock
or atomics:

```c
int64_t rc = os64_gui_event_ring(window, IMAGE_READY);
// Check rc: zero means accepted; a negative value is a GUI error.
// On failure, keep ownership/cleanup of the result explicit.
```

Keep the callback short: collect ready work, update widgets and mark
them dirty. `os64_ui_run` paints after dispatching the queued events, so
the callback need not paint immediately. Long downloads or calculations
belong on the workers. A custom event loop can use `os64_ui_dispatch` on
the UI thread and paint itself, or handle `OS64_GUI_EVENT_DOORBELL`
directly. With no callback, dispatch returns false; the packaged loop
has no application-specific handling to perform.

Several rings may arrive before the UI takes an event. Ringing
`IMAGE_READY`, `IMAGE_READY`, then `SCAN_READY` during that interval
produces one pending event with both bits set. The handler should drain
the work record rather than
assume one callback means one result. The bell itself does not replace
the lock or atomics used to share that record.

The event pointer is borrowed for the callback's duration. Copy its mask
if needed later; do not keep the pointer. Configure the callback on the
UI thread, and keep widget changes there as well. Ringing merely queues
work for that event loop; it does not interrupt an ongoing callback.

For an executable example, see
[`doorbelltest.c`](userland/tests/doorbelltest/doorbelltest.c): `notify`
rings from a worker, `on_doorbell` receives it, and `wait_case` exercises
the packaged libui loop and both forms of the raw wait API.

## Contract

- `SYSCALL_GUI_EVENT_RING` is syscall 58. Its arguments are a window
  handle and a nonzero 32-bit mask; there are no user pointers. Success
  returns zero. Invalid or foreign handles return the existing GUI
  errors. A zero mask or raw syscall mask wider than 32 bits is BAD_ARGS.
- `OS64_GUI_EVENT_DOORBELL` is event 14. `event.doorbell.mask` contains
  the OR of masks received since the previous doorbell delivery. The
  event remains 32 bytes, and `tick` is the most recent ring's tick.
- Each window has a coalesced slot outside its input ring. Input overflow
  and focus-event eviction cannot discard a pending doorbell. Appearance
  events retain priority; when doorbells and queued input are both
  pending, delivery alternates between them. Pointer snapshots retain
  their existing position after queued input.
- Ringing uses the existing GUI waiter wake path under `kGuiLock`.
  `os64_gui_event_wait(window, NULL)` wakes without consuming the event.
  The existing wait backstop also covers a ring racing with thread park.
- `os64_ui_dispatch` passes the unchanged borrowed event to
  `ui.on_doorbell(ui, event)` on the UI thread, even without a widget
  root. With no callback it returns false; widgets do not receive the
  event. Applications using `os64_ui_run` can set this callback, while
  applications with their own loop can handle the event directly.

The mask is a hint to inspect shared state, not a completion count or a
queue of result pointers. Publish and read that state using its own
atomics or lock. Repeated rings can collapse into one event. Stop and
join producers before destroying their target window: GUI handles can
be reused. The kernel does not extend the lifetime of an application's
job or result.

`os64_ui_t` gains a callback field. Rebuild and deploy libos64 and its
userland consumers together; the GUI event ABI itself keeps its layout.

## Validation

`tools/test_appearance_host.sh` includes host contracts for 1,000 rings
against a full input ring, OR coalescing, input/doorbell fairness, mask
reset, the high mask bit, and callback delivery with and without a root.
The suite runs with AddressSanitizer, UndefinedBehaviorSanitizer and
LeakSanitizer.

`/tests/testrun doorbelltest` checks the syscall in an isolated GUI guest:
foreign-task refusal, invalid arguments and destroyed handles, a full
input ring, 1,000 coalesced rings, the high bit, worker-thread wakeups,
NULL-output wait followed by poll, and the packaged libui loop. A
watchdog converts a lost wake into a test failure. It uses badge
`0x600B0000` for pass, `0x600B0001` for fail and `0x600B0002` for a
GUI-unavailable skip.

Validated on 2026-09-25:

- `make -j8`: kernel, userland, fixtures and ISO built with the default
  `-Werror` checks.
- `tools/test_appearance_host.sh`: passed with ASan, UBSan and LSan. The
  sanitizer run required execution outside the sandbox because its
  process tracing prevented LeakSanitizer from inspecting memory.
- Eight-core QEMU, GUI enabled, private disk copies:
  `testrun doorbelltest` reported **1 passed, 0 failed, 0 skipped**.
- `git diff --check` and `tools/stale_refs.sh`: clean.

The temporary QEMU driver used the shell-start marker for GUI boot
readiness (GUI boots do not enter `kernel_park`, which emits the helper's
usual `boot complete` marker), and Ctrl+Alt+F1 to select the text shell.
Repository VM helpers were not changed. Hardware validation and
independent code review remain outstanding.

Guest output is retained in
[the doorbell evidence](docs/gui-doorbell-evidence/guest.txt).
