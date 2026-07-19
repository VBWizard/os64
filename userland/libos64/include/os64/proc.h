#ifndef OS64_PROC_H
#define OS64_PROC_H

// libos64 process control (LIBOS64.md layer). SCAFFOLDING: only yield exists
// so far; spawn/fork/exec*/waitpid arrive when the shell work pulls them in
// (both spawn AND fork/exec are first-class — see LIBOS64.md/ABI.md).

#include <stddef.h>

// Yield the CPU to the scheduler; returns when rescheduled.
void os64_yield(void);

// Terminate the task with `code`. Does not return. (A plain `return` from
// main() reaches the same place through launch — both paths are first-class;
// this one is for exiting from anywhere BUT main.)
void os64_exit(int code) __attribute__((noreturn));

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
long os64_getcwd(char *buf, size_t len);

// Change directory. Relative and messy paths welcome ("../dir1//./x") — the
// kernel canonicalizes before storing. Returns 0, or negative if the target
// doesn't exist or isn't a directory (in which case the cwd is UNCHANGED).
long os64_chdir(const char *path);

// Spawn `path` as a child, non-blocking. `argv` is a NULL-terminated array of
// string pointers (argv[0] conventionally the program name); pass NULL for no
// args. Returns the child's pid (> 0), or negative on error. The child
// inherits this process's environment.
long os64_spawn(const char *path, char *const argv[]);

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
long os64_spawn_redirected(const char *path, char *const argv[],
                           int in, int out, int err);

// Wait for a child to exit and reap it. pid > 0 waits for that specific child;
// pid == 0 waits for the first of any child to end. Returns the pid that
// ended (> 0), or negative if there's no such child; writes the child's exit
// code to *exit_code if non-NULL. Returns immediately if the child already
// ended.
long os64_wait(long pid, int *exit_code);

#endif // OS64_PROC_H
