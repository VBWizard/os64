#ifndef OS64_PROC_H
#define OS64_PROC_H

// libos64 process control (LIBOS64.md layer). SCAFFOLDING: only yield exists
// so far; spawn/fork/exec*/waitpid arrive when the shell work pulls them in
// (both spawn AND fork/exec are first-class — see LIBOS64.md/ABI.md).

#include <stddef.h>
#include <stdint.h>
#include "os64/env.h"
// For OS64_SPAWN_* — the spawn flag values are part of the kernel contract,
// so they are defined once in the ABI rather than mirrored here.
#include "os64/syscall_numbers.h"
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
// length, or negative if the buffer is too small. TASK_MAX_PATH_LEN is 128
// kernel-side, so a 128-byte buffer always suffices.
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
// install as the child's 0/1/2, or -1 to leave that stream on the console.
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

// Wait for a child to exit and reap it. pid > 0 waits for that specific child;
// pid == 0 waits for the first of any child to end. Returns the pid that
// ended (> 0), or negative if there's no such child; writes the child's exit
// code to *exit_code if non-NULL. Returns immediately if the child already
// ended.
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

// Who am I? The calling task's ID — one syscall, one register, cannot fail.
// V1 Unix answered this in 1971 and nobody has improved on the answer. The
// namespace spelling of the same fact is /proc/self (open-time identity:
// whoever OPENS it is the self); this call is expansion-time identity —
// what husk's $$ freezes into a command line before any child exists.
uint64_t os64_getpid(void);

// Sleep for AT LEAST `ms` milliseconds — the thread genuinely parks, zero
// CPU. The floor is the kernel's scheduler tick (1000/per_second ms — ask
// os64_ticks); requests round UP to it, and the rounding tracks the ACTIVE
// rate, so a faster-ticking kernel makes every existing binary's short
// sleeps better with no recompile. os64_sleep(0) is the documented free
// yield. Returns 0. (One call, milliseconds — Unix needed four generations
// of this function because the units kept being wrong.)
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
