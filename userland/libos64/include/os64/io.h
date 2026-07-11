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

// Convenience: write a NUL-terminated string to the console (handle 1).
long os64_puts(const char *s);

// Terminate the task with `code`. Does not return.
void os64_exit(int code) __attribute__((noreturn));

#endif // OS64_IO_H
