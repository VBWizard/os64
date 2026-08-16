#ifndef OS64_RUNTIME_H
#define OS64_RUNTIME_H

// os64/runtime.h — what libos64 does before main() runs.
//
// `launch` (userland/start/launch.S — the crt0-equivalent, named honestly)
// gets control from the kernel, publishes the environment block, and then
// calls os64_runtime_init(). Everything the LIBRARY needs standing up before
// a program's first line goes in there, in one place with one name, rather
// than each subsystem inventing a lazy "have I been initialized?" check at
// the top of its hot path.
//
// Today that is exactly one thing — the heap — and even the heap could have
// initialized itself lazily on its first malloc. It does not, deliberately:
// os64_heap_init registers the heap's report with the kernel, and a program
// whose /proc/<pid>/heap only appears after its first allocation would be a
// file that lies by omission about programs that never allocate.
//
// (When ELF constructors and at-exit teardown arrive — LIBOS64.md's plan —
// they hang here too.)

// Stand up the library. Called by launch before main; safe to call twice.
void os64_runtime_init(void);

#endif // OS64_RUNTIME_H
