#ifndef OS64_PROC_H
#define OS64_PROC_H

// libos64 process control (LIBOS64.md layer). SCAFFOLDING: only yield exists
// so far; spawn/fork/exec*/waitpid arrive when the shell work pulls them in
// (both spawn AND fork/exec are first-class — see LIBOS64.md/ABI.md).

// Yield the CPU to the scheduler; returns when rescheduled.
void os64_yield(void);

#endif // OS64_PROC_H
