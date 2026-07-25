#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdbool.h>

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

// console_read returned because the calling thread has SIGINT pending — the
// read didn't fail, the READER is being terminated. The read syscall sees
// this and enforces the default action (raise_sigint_and_die, syscall.c);
// the value never reaches ring 3.
#define CONSOLE_READ_INTERRUPTED (-2L)

// Block until at least one key is available, then copy up to `len` translated
// ascii bytes into `buf` and return the count (>0). Unix terminal semantics:
// a read returns as soon as there IS input, not only when `len` is filled.
// Sleeps (zero CPU) while the buffer is empty; woken by console_wake_if_ready.
// Returns CONSOLE_READ_INTERRUPTED if the caller has SIGINT pending.
long console_read(char *buf, size_t len);

// The line-discipline peek (the ISIG/VINTR seed): called by keyboard.c at the
// delivery choke for every key-down, BEFORE the byte enters the console ring.
// Returns true if the byte was consumed as the interrupt character (ETX/0x03
// -> SIGINT pending on the foreground task); false means "just data, deliver
// it". Policy lives HERE, not in keyboard.c — the device layer stays blind to
// tasks and signals. IRQ-safe on purpose: the raise is one word-OR; the
// actual kill happens later, at the victim's own syscall boundary.
bool console_intr_intercept(char ascii);

// Called from processSignals (scheduler context, every pass). If a reader is
// asleep in console_read AND the keyboard driver has input, wake the reader.
// Level-triggered on keyboard_has_event(), so a key arriving at any instant is
// caught on the next pass — the sleep/wake path is lost-wakeup-free.
void console_wake_if_ready(void);

#endif // CONSOLE_H
