#ifndef OS64_PROC_H
#define OS64_PROC_H

// libos64 process control (LIBOS64.md layer). SCAFFOLDING: only yield exists
// so far; spawn/fork/exec*/waitpid arrive when the shell work pulls them in
// (both spawn AND fork/exec are first-class — see LIBOS64.md/ABI.md).

// Yield the CPU to the scheduler; returns when rescheduled.
void os64_yield(void);

// Spawn `path` as a child, non-blocking. `argv` is a NULL-terminated array of
// string pointers (argv[0] conventionally the program name); pass NULL for no
// args. Returns the child's pid (> 0), or negative on error. The child
// inherits this process's environment.
long os64_spawn(const char *path, char *const argv[]);

// Wait for a child to exit and reap it. pid > 0 waits for that specific child;
// pid == 0 waits for the first of any child to end. Returns the pid that
// ended (> 0), or negative if there's no such child; writes the child's exit
// code to *exit_code if non-NULL. Returns immediately if the child already
// ended.
long os64_wait(long pid, int *exit_code);

#endif // OS64_PROC_H
