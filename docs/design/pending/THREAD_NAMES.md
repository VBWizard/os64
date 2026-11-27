# Thread names at creation

Status: design, 2026-10-26. Chris and Quinn. The library layering and single
syscall are agreed; the detailed contracts below are proposed for implementation.

Give threads optional descriptive names at creation, expose them in their
`/proc` status, and display them in `top` and `htop`. A Yonder ticker should
be distinguishable from a pool worker without identifying it by CPU usage
or remembering its numeric ID. Existing unnamed creation calls retain their
behavior and display fallback.

## Library and syscall contract

Keep the existing public function and add a named variant in libos64:

```c
int64_t os64_thread(int64_t (*fn)(void *), void *arg);
int64_t os64_thread_named(int64_t (*fn)(void *), void *arg,
                          const char *name);
```

`os64_thread(fn, arg)` delegates to `os64_thread_named(fn, arg, "")`.
The named function invokes the existing `SYSCALL_THREAD` with four arguments:
`entry`, `arg`, `exit_stub`, `name`. There is one thread-creation syscall and
one kernel creation path. The extra convenience function belongs in libos64;
it does not consume another syscall number.

Both functions return the same kind of join handle. Reading, closing,
joining, thread exit, affinity and scheduling retain their existing contracts.
Names are descriptive metadata, not identities or lookup keys; duplicates
are allowed. Task and thread IDs remain the monitor's identities.

Proposed name rules:

- `NULL` and the empty string mean unnamed.
- A shared ABI constant specifies 64 bytes of storage, including the final
  NUL: at most 63 name bytes.
- Initially accept printable ASCII, including spaces. Reject control bytes,
  non-ASCII bytes and overlong names rather than silently truncating them.
  This keeps status records and terminal labels literal and bounded. Unicode
  naming can widen this contract separately.
- The kernel copies the name during creation. The caller can reuse or release
  its buffer when the call returns.
- Names are fixed for the thread's lifetime in this feature. Runtime renaming
  and labels describing a worker's current job are outside this slice.

## Compatibility and rollout

The C API remains compatible: existing applications dynamically calling
`os64_thread()` use the updated libos64 wrapper without changing their calls.
The syscall ABI changes, because its fourth argument acquires meaning.

The old `os64_syscall3` stub does not initialize R10, the fourth argument
register. An old wrapper or direct caller can therefore supply an arbitrary
value there. The kernel cannot infer whether that value was an intentional
name pointer. Ship the updated kernel and libos64 together; do not claim
compatibility between the new kernel and old raw thread-creation callers.
An old kernel ignores the new argument, so it cannot provide named-thread
behavior even when paired with the new library.

The source audit for this design found the creation wrapper in
`userland/libos64/thread.c` and two direct calls in
`userland/tests/badptr/badptr.c`. The latter deliberately test invalid entry
and exit-stub addresses. Change those fixtures to the four-argument stub with
an explicit empty name and retain their address checks. Repeat the caller
audit at implementation time, including numeric syscall calls, assembly and
any statically embedded wrappers. Direct invocation is possible through the
ABI helpers even though ordinary applications use libos64.

The public `os64_thread_info_t`, monitor thread records and snapshot layouts
also form binary interfaces. Do not enlarge them to carry names. Add reader
and accessor functions while preserving the existing structures, following
the separate CPU-accounting metadata pattern.

## Kernel ownership and publication

Add fixed storage to `thread_t`, initialized empty for threads without a
supplied name, including task launch and kernel threads. Place it so existing
assembly offsets and alignment contracts remain valid; check the offset
definitions and static assertions during the build.

In `syscall_thread`, validate and copy a nonempty user name into bounded
kernel storage using the user-copy machinery. Complete validation before
creating or publishing the new thread. A bad pointer, missing terminator,
overlong name or disallowed byte must refuse the call without starting a
thread. Return the existing bad-user-data/invalid verdict appropriate to the
failure; no new error family is needed.

The optional pointer must allow NULL through syscall dispatch. Its handler
performs the guarded copy; do not put it in a dispatch pointer mask that
unconditionally rejects NULL. Re-read the dispatch and copy-helper comments
as part of that change.

Pass the validated kernel copy into `thread_join_create` and store it in the
new thread before linking it into the task's thread list or making it
runnable. A thread can execute immediately after publication. Setting its
name after the creation helper returns would race with execution and `/proc`
readers. Immutable storage in `thread_t` avoids a separate allocation and a
new rename/read synchronization protocol. Its lifetime follows the existing
thread lifetime.

## Proc reporting and monitor display

Add a `name` key to `/proc/<pid>/thread/<tid>/status`:

```text
thread  123
task    100
name    yonder-ticker
```

Use the existing tab-separated record format. Emit an empty value for an
unnamed thread. Leave `/proc/<pid>/status` and `/proc/<pid>/cmdline` describing
the task. Existing readers ignore the additional key; new readers treat an
absent key from an older kernel as unnamed.

Extend the internal thread-status parser to collect optional name metadata
alongside the existing record and CPU split in the same status read. Expose
an additive reader API with bounded caller-supplied name storage. Existing
reader entry points continue to work without requesting a name.

The opaque monitor owns name buffers alongside its sampled thread records.
Provide an additive name accessor keyed by task and thread ID, with borrowed
storage valid until the next sample or monitor destruction. Missing or
unnamed identities have no custom label. Clear metadata on failed reads,
disappearance and refresh so a reused sample slot cannot keep another
thread's name. No additional `/proc` read per thread is necessary.

For thread rows, both `top` and `htop` display the custom name when present,
otherwise their existing owning-task name. Process rows retain their current
labels and command-line behavior. CPU accounting, sorting identities and
task-directed actions remain unchanged. If htop search is extended to thread
names, retain the owning task as context and preserve the existing process
search behavior; that is an explicit UI choice, not an incidental change to
how rows are selected.

## Work pools and Yonder

Most Yonder worker threads are created by `os64_work_pool_create`, rather
than directly by Yonder. Add `os64_work_pool_create_named` with the existing
arguments followed by a name prefix. The existing pool constructor delegates
with an empty prefix. Named pools create numbered labels such as
`yonder-worker-01`, `yonder-worker-02`, and so on through the configured worker
count. Validate space for the prefix, separator, largest index and terminator
before starting workers; preserve the existing cleanup path for a later
creation failure.

Use `yonder-worker` for Yonder's pool, `yonder-ticker` for its ticker and
`yonder-settings` for the settings relay. Keep the launch thread unnamed so
its row continues to read `yonder`. Pool workers handle different job types,
so a permanent label such as `image-loader` would misdescribe their role.
Labeling the current job would require a separate runtime-renaming design.

## Implementation and acceptance

The expected touch points are the thread ABI/header and libos64 wrapper;
kernel syscall handler, thread structure and join creation; procfs formatter
and typed reader; monitor storage/accessor; `top` and `htop` display; the
work-pool API; Yonder's pool, ticker and settings call sites; and thread/
monitor fixtures. This is a contained feature with a coordinated ABI rollout.

Acceptance checks:

1. Existing unnamed calls still create threads with working join/close/exit
   behavior. A program built against the old public thread API works with
   the updated libos64 and kernel.
2. Named creation preserves empty, short and maximum-length names. Mutating
   the caller's buffer after return does not change the reported name.
3. Valid names crossing a page boundary succeed. Invalid pointers, names
   crossing into an unmapped page, missing terminators, overlong names and
   disallowed bytes are refused without a new live thread. Preserve the
   existing bad-entry and bad-exit-stub tests.
4. Proc-reader and monitor host fixtures cover missing/empty/custom names,
   failed reads, disappearing threads and slot reuse. Existing public record
   layouts remain unchanged. Run relevant sanitizer checks with leak
   detection enabled.
5. Monitor display tests cover named rows and fallback while retaining task
   identity and action targets. Pool tests cover numbered names, unnamed
   creation, invalid prefixes and partial-creation cleanup.
6. Cross-build kernel and affected userland. In QEMU, create named and unnamed
   threads, read their status and exercise join/exit. On the P5, expand Yonder
   in `top` and `htop`, confirm distinct worker/ticker labels and unchanged
   process rows, and check the settings relay while it exists.

This document records the feature for a later implementation slice; it does
not change the SVG schedule or implement thread naming.

## Review and additions (Fable, 2026-11-26)

The design stands as written: one syscall, the name copied and fixed at
creation, stored in `thread_t`, read from the thread's status. Chris ruled
it built after the thread stacks slice, which works in the same creation
path. Three additions.

**The kernel's own threads are named too.** `knet`, `kusb`, `kaudio`,
`kworker`, the compositor and the late tests are made inside the kernel,
so the name reaches them through the kernel's creation path, with no user
copy. A daemon held off its CPU is the question `/sys` reports are read
for, and a monitor row that names it is the first place to look.

**The media threads are the first customers.** A player's threads were
told apart on the P5 by the order they were created in, and only
tentatively. `os64_media_player` and `os64_media_output` name theirs
(decoding, the conversion to the window's pixels, the show thread, the
audio output); yonder names its media opener and loader, its media keys
reader and its ticker; libfetch names a connection's reader; gplay its
key reader. A library's thread is named for its job, with no program
prefix: the row already sits under its task.

**The caller audit is the slice's first step, not its last.** A direct
caller of the thread syscall that passes three arguments hands the kernel
whatever R10 held, and the kernel refuses the creation when that is not a
readable name. Every caller is found before the kernel half lands, numeric
syscall calls and assembly included.

## As built, part A (the kernel and the ABI)

- The rule is `abi/include/os64/thread_name.h` (`OS64_THREAD_NAME_MAX`,
  `os64_thread_name_valid`), shared by the kernel and libos64, with a host
  test and mutants (`tools/test_thread_name_host.sh`,
  `tools/test_thread_name_mutants.py`).
- `syscall_thread` copies and checks the name right after the entry and the
  exit stub, before the reap backlog's park, the handle and the thread, so
  a refusal leaves nothing behind: -1
  for a name with no end within 64 bytes or a byte outside the range, -2 for
  one that cannot be read.
- Both creation doors name the thread through one helper,
  `thread_name_init`, before the thread is published: `thread_join_create`
  for a thread a program asks for, `task_create` for a kernel task's thread
  (named for its task, on the caller's asking for a kernel task, never on
  the path). A program spawned from ring 3 has its first thread unnamed. The
  name stays out of the dispatch pointer mask (which, as it happens, passes
  NULL through): the handler's guarded string reads vet every page, and
  badptr's name group reaches their own bound that way. createThread takes no
  name: a kernel task's thread is made before its task's name is worked out.
- `/proc/<id>/thread/<tid>/status` prints `name` after `task`.
- Fixtures: `/tests/threadnames`, and `badptr thread`'s name group.
