#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>

// The console: the READ side of stdin for text-mode userland (v1).
//
// Layering (each layer TTYs will multiply, not rewrite):
//   keyboard.c   — the device driver: PS/2 scancodes -> a ring of TRANSLATED
//                  keys (ascii + modifiers). keyboard_has_event()/pop_event().
//   console.c    — THIS: the blocking read abstraction on top. One console,
//                  one sleeping reader for now; becomes tty_t[N] later, where
//                  the single waiter/buffer become per-TTY fields.
//   read syscall — the ring-3 bridge (SYSCALL_READ, handle 0 -> console_read).
//
// stdout/stderr already work via the write syscall; this adds stdin.

// Block until at least one key is available, then copy up to `len` translated
// ascii bytes into `buf` and return the count (>0). Unix terminal semantics:
// a read returns as soon as there IS input, not only when `len` is filled.
// Sleeps (zero CPU) while the buffer is empty; woken by console_wake_if_ready.
long console_read(char *buf, size_t len);

// Called from processSignals (scheduler context, every pass). If a reader is
// asleep in console_read AND the keyboard driver has input, wake the reader.
// Level-triggered on keyboard_has_event(), so a key arriving at any instant is
// caught on the next pass — the sleep/wake path is lost-wakeup-free.
void console_wake_if_ready(void);

#endif // CONSOLE_H
