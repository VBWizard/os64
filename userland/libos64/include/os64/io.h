#ifndef OS64_IO_H
#define OS64_IO_H

// libos64 raw I/O over kernel handles (LIBOS64.md layer). A handle is a small
// int index into the per-task handle table; 1/2 are the conventional console
// out/err until the table and real file handles land. Buffered FILE* I/O will
// layer OVER this — never compete with it.

#include <stddef.h>
#include <stdint.h>

// Write `len` bytes of `buf` to `handle`. Returns bytes written, or a
// negative value on error (the in-band status half of the ABI; no errno).
long os64_write(int handle, const void *buf, size_t len);

// Read up to `len` bytes from `handle` into `buf`. Blocks until at least one
// byte is available, then returns the count read (>= 1), or negative on error.
// handle 0 (stdin) reads the console keyboard.
long os64_read(int handle, void *buf, size_t len);

// Convenience: write a NUL-terminated string to the console (handle 1).
long os64_puts(const char *s);

// Terminate the task with `code`. Does not return.
void os64_exit(int code) __attribute__((noreturn));

// Diagnostic: print a string to the kernel serial log (prefixed "[user] ").
// Distinct from os64_puts (which goes to the console) — this is for test
// output an offscreen/headless run can capture.
void os64_debug_log(const char *s);

#endif // OS64_IO_H
