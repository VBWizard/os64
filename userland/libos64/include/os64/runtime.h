#ifndef OS64_RUNTIME_H
#define OS64_RUNTIME_H

// os64/runtime.h — what libos64 does before main() runs.
//
// `launch` (userland/start/launch.S — the crt0-equivalent, named honestly)
// gets control from the kernel and calls os64_runtime_init(), handing it the
// environment block the kernel passed in RDX. Everything the LIBRARY needs
// standing up before a program's first line goes in there, in one place with
// one name, rather than each subsystem inventing a lazy "have I been
// initialized?" check at the top of its hot path.
//
// Today that is exactly one thing — the heap — and even the heap could have
// initialized itself lazily on its first malloc. It does not, deliberately:
// os64_heap_init registers the heap's report with the kernel, and a program
// whose /proc/<pid>/heap only appears after its first allocation would be a
// file that lies by omission about programs that never allocate.
//
// (When ELF constructors and at-exit teardown arrive — LIBOS64.md's plan —
// they hang here too.)

#include "os64/env.h"

// Stand up the library. Called by launch before main; safe to call twice.
// `env` is the environment block the kernel mapped for this task (NULL is
// legal — a bare fixture may have none).
void os64_runtime_init(const os64_env_block_t *env);

// Publish the environment block so os64_getenv can answer from anywhere.
// Called by os64_runtime_init; separate because the storage lives in env.c
// with the code that reads it, and because a program that stands the runtime
// up by hand should not have to know that. (A FUNCTION rather than a shared
// global on purpose — see env.c for the copy-relocation story.)
void os64_env_publish(const os64_env_block_t *env);

#endif // OS64_RUNTIME_H
