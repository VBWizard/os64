#ifndef OS64_PROC_H
#define OS64_PROC_H

// libos64 process control (LIBOS64.md layer): spawn in its shapes, wait and
// reap, exit, the cwd, the environment, and where a command by name lives.
// Spawn is the one way to start a program today; fork/exec are first-class
// in the design and arrive when something asks (LIBOS64.md/ABI.md).

#include <stddef.h>
#include <stdint.h>
#include "os64/env.h"
// For OS64_SPAWN_* — the spawn flag values are part of the kernel contract,
// so they are defined once in the ABI rather than mirrored here.
#include "os64/syscall_numbers.h"
#include "os64/signal.h"   // OS64_INTERRUPTED — what os64_wait and os64_sleep answer when a handled signal cut them short
// os64_ticks_t — the monotonic clock struct os64_ticks fills (and the
// stopwatch-vs-calendar doctrine, which lives with the struct).
#include "os64/ticks.h"

// Yield the CPU to the scheduler; returns when rescheduled.
void os64_yield(void);

// Terminate the task with `code`. Does not return. (A plain `return` from
// main() reaches the same place through launch — both paths are first-class;
// this one is for exiting from anywhere BUT main.)
void os64_exit(int32_t code) __attribute__((noreturn));

// The current working directory — process state, owned by the KERNEL (which
// is what makes it trustworthy: chdir validates the target exists at change
// time, stores it canonical, and every path-taking syscall resolves relative
// paths against it — open("notes.txt"), spawn("hello"), opendir("dir1") all
// mean "here"). A child inherits its parent's cwd at spawn; changing yours
// changes a copy nobody else sees — which is precisely why cd must be a
// shell BUILTIN and always has been, in every shell, ever.

// Copy the cwd (canonical, absolute, NUL-terminated) into buf. Returns its
// length, or negative if the buffer is too small. A path is bounded by the
// same number spawn's arguments are — OS64_SPAWN_ARG_MAX, terminator included
// — so a buffer of that size always suffices. (This said 128 while the kernel
// said 256, which is the reason the number is published rather than retyped.)
int64_t os64_getcwd(char *buf, size_t len);

// The ENVIRONMENT — more process state. The kernel maps every task's env
// block read-only into its address space and hands it to main() as the third
// argument; launch.S ALSO stashes that pointer in a library global before
// calling main, so a leaf function can ask for a variable without the
// program threading envp through itself. The block layout is ABI
// (abi/include/os64/env.h): packed key\0value\0 pairs — real string keys
// and values, no "KEY=VALUE" re-splitting, no os32-style fixed-width slots.

// Look up `key`. Returns a pointer to its value INSIDE the (read-only,
// process-lifetime) environment block, or NULL if unset. Never allocates.
//
//     const char *path = os64_getenv("PATH");   // e.g. "/bin"
const char *os64_getenv(const char *key);

int32_t os64_env_next(os64_envent_t *buffer);

// Set (or replace) `key` in THIS process's environment. Takes effect
// immediately — the env block the kernel mutates is the very page this
// process reads, so a following os64_getenv sees the new value — and flows
// to every child spawned AFTER the call. Children spawned BEFORE keep their
// snapshot: environments are copied downward at spawn, never shared
// sideways (the one-way valve that makes `export` a shell builtin).
// Returns 0, or negative (env block full, bad key).
//
// CAUTION for walkers: a set/unset rewrites the block in place, so pointers
// previously returned by os64_getenv and any in-flight os64_env_next walk
// are invalidated. Finish iterating before you mutate.
int64_t os64_setenv(const char *key, const char *value);

// Remove `key` from this process's environment. Unsetting a key that isn't
// there is success, not error — idempotent, the way `unset` has behaved in
// every shell since Bourne. Same snapshot semantics as os64_setenv.
int64_t os64_unsetenv(const char *key);


// Change directory. Relative and messy paths welcome ("../dir1//./x") — the
// kernel canonicalizes before storing. Returns 0, or negative if the target
// doesn't exist or isn't a directory (in which case the cwd is UNCHANGED).
int64_t os64_chdir(const char *path);

// Spawn `path` as a child, non-blocking. `argv` is a NULL-terminated array of
// string pointers (argv[0] conventionally the program name); pass NULL for no
// args. Returns the child's pid (> 0), or negative on error. The child
// inherits this process's environment.
int64_t os64_spawn(const char *path, char *const argv[]);

// Spawn with REDIRECTION: `in`/`out`/`err` are handles of THIS process to
// install as the child's 0/1/2, or -1 for this process's OWN 0/1/2 — the
// console if that is where you are, your redirect if you have one (so a
// program that spawns, run as `prog > log 2>&1`, sends its children's output
// to the log too; os64_spawn is the all-minus-ones case).
// This is how a shell builds a pipeline — the child is born reading from and
// writing to the right places and never knows it was redirected, which is why
// no program contains a single line of pipe-awareness:
//
//     int p[2]; os64_pipe(p);
//     os64_spawn_redirected("/bin/hello", a1, -1,   p[1], -1);  // stdout -> pipe
//     os64_spawn_redirected("/bin/upper", a2, p[0], -1,   -1);  // stdin  <- pipe
//     os64_close(p[0]); os64_close(p[1]);   // MANDATORY — see os64_pipe()
//
// The child gets its OWN reference on any pipe end passed here, so the parent
// closing its copy does not yank the pipe out from under it.
//
// `flags` is OS64_SPAWN_* (syscall_numbers.h); 0 is the everyday spawn.
// OS64_SPAWN_BACKGROUND marks a job launched with `&`: the kernel gives its
// reads of handle 0 an immediate EOF, so a background job can never quietly
// take keystrokes the shell was owed. It stays a parameter of THIS function
// rather than a fourth spawn entry point because a shell that redirects is
// the only thing that ever wants it.
int64_t os64_spawn_redirected(const char *path, char *const argv[],
                           int32_t in, int32_t out, int32_t err,
                           uint64_t flags);

// Seat the child on a pty slave (PTY.md): `master` is a handle from
// os64_pty_create, and the child becomes that slave's shell — controlling
// terminal, foreground, its console handles routed there — with zero
// pty-awareness in the child. The terminal's whole job becomes: write
// keystrokes to the master, snapshot the screen out.
int64_t os64_spawn_seated(const char *path, char *const argv[], int64_t master);

// Where is the program called `command`? The search a shell runs at every
// command line, as a library verb for anything that spawns what a user typed:
//   - a name containing '/' names a PLACE — used exactly as typed, no search
//   - a bare name tries the cwd first (V6 shells did: `cd /bin` + `ls`
//     worked before PATH existed), then each colon-separated directory
//     of $PATH as this process sees it right now
// Existence is probed with os64_stat, and a directory never wins — typing
// `bin` at / must not spawn a directory. Returns `command` itself for a
// place or a cwd hit, `resolved` filled with the PATH hit, or `command`
// unresolved when nothing matched, so the spawn that follows delivers the
// "no" and the caller reports the name the user typed. A candidate that
// would not fit `resolved` is skipped, not truncated. The buffer must stay
// alive until the spawn; OS64_SPAWN_ARG_MAX bytes is exactly what a spawnable
// path can be, so nothing that fits there is lost for want of room here.
const char *os64_resolve_command(const char *command, char *resolved,
                                 size_t capacity);

// Wait for a child to exit and reap it. pid > 0 waits for that specific child;
// pid == 0 waits for the first of any child to end. Returns the pid that
// ended (> 0), or negative if there's no such child; writes the child's exit
// code to *exit_code if non-NULL. Returns immediately if the child already
// ended.
//
// A blocking wait can be CUT SHORT: if this program has a handler installed
// and that signal arrives (SIGWINCH, when your terminal window is dragged),
// the call answers OS64_INTERRUPTED with nothing collected — the child is
// still yours to wait for, so loop (SIGNALS.md §8, no restart). The handler
// has usually run by then, but INTERRUPTED promises only that the wait ended
// early (a sibling uninstalling the handler in between leaves nothing to
// run). A program with no handlers never sees it.
int64_t os64_wait(int64_t pid, int32_t *exit_code);

// Collect ONE already-finished child WITHOUT ever blocking. Returns that
// child's pid (> 0), or 0 when nothing has finished — 0 is the ordinary
// answer, not an error, so a shell can call this at every prompt without
// treating the common case as a failure. Exit code via *exit_code if non-NULL.
//
// A separate call rather than a flag on os64_wait, because a "wait" that does
// not wait is a name that lies. This is what makes `&` clean: reporting a
// finished background job and reaping it are the same act, so nothing
// accumulates and no kernel sweeper is needed —
//
//     int32_t code;  int64_t pid;
//     while ((pid = os64_reap(&code)) > 0)
//         /* one finished job */;
int64_t os64_reap(int32_t *exit_code);

// Who am I? The calling TASK's ID — one syscall, one register, cannot fail.
// V1 Unix answered this in 1971 and nobody has improved on the answer; os64
// keeps the answer and changes the noun, because it runs tasks and this
// returns task->taskID. (It was os64_getpid until 2026-08-24. The rename is
// not cosmetics: "pid" reads as per-process, and libos64's own config writer
// believed it, built a temporary file name out of it, and had every thread of
// a program collide on that name. A name that lies costs more than a name
// that is merely unfamiliar — see SYSCALL_TASKID for the whole story.)
//
// IT IS PER-TASK, NOT PER-THREAD. Every thread of a program gets the same
// number back. There is no thread-id call today; if you need one, that is a
// consumer talking and it can be built — do not reach for this instead.
//
// The namespace spelling of the same fact is /proc/self (open-time identity:
// whoever OPENS it is the self); this call is expansion-time identity —
// what husk's $$ freezes into a command line before any child exists.
uint64_t os64_taskid(void);

// Sleep for AT LEAST `ms` milliseconds — the thread genuinely parks, zero
// CPU. The floor is the kernel's scheduler tick (1000/per_second ms — ask
// os64_ticks); requests round UP to it, and the rounding tracks the ACTIVE
// rate, so a faster-ticking kernel makes every existing binary's short
// sleeps better with no recompile. os64_sleep(0) is the documented free
// yield. (One call, milliseconds — Unix needed four generations of this
// function because the units kept being wrong.)
//
// Returns 0 when the nap completed, or OS64_INTERRUPTED when a signal
// you HANDLE arrived and cut it short (2026-08-23). The remaining time is not
// slept and is not resumed: os64 has no SA_RESTART and no EINTR — an
// interrupted call says so, and a caller that wanted the whole nap loops,
// which it can read. (A signal nothing handles still ends the program, as it
// always did; this return can only happen to a program that asked for it.)
// The value is OS64_INTERRUPTED (os64/signal.h) — one spelling for every
// blocking call, since they all mean the same thing by it.
int64_t os64_sleep(uint64_t ms);

// Read the monotonic clock: ticks since boot + the active tick rate, in one
// call (the two numbers are only useful together). This is the STOPWATCH —
// safe for intervals, CPU%, timeouts, uptime — never the calendar; see
// os64/ticks.h for the doctrine and the everyday arithmetic. Returns 0, or
// negative if `out` is bad.
//
//     os64_ticks_t t;
//     os64_ticks(&t);
//     /* uptime seconds = t.ticks / t.per_second */
int64_t os64_ticks(os64_ticks_t *out);

#endif // OS64_PROC_H
