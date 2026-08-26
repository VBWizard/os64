#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>   // uint64_t — console_read_deadline's tick deadline

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

// console_read returned because the calling thread has a signal pending
// that ends the wait (signal_park_must_end) — the read didn't fail. Either
// the READER is being terminated, and the read syscall enforces the default
// action (raise_terminating_signal_and_die, syscall.c), or a handler will
// catch it and the syscall answers OS64_INTERRUPTED so the handler can be
// armed on the way out. This value itself never reaches ring 3.
#define CONSOLE_READ_INTERRUPTED (-2L)

// console_read_deadline returned because the deadline passed with no byte to
// show. The read syscall translates this to OS64_ERR_TIMEOUT for ring 3;
// like INTERRUPTED, the sentinel itself never leaves the kernel.
#define CONSOLE_READ_TIMEOUT (-3L)

// Block until at least one key is available, then copy up to `len` translated
// ascii bytes into `buf` and return the count (>0). Unix terminal semantics:
// a read returns as soon as there IS input, not only when `len` is filled.
// Sleeps (zero CPU) while the buffer is empty; woken by console_wake_if_ready.
// Returns CONSOLE_READ_INTERRUPTED if the caller has a signal pending that
// ends the wait (signal_park_must_end: a terminate, or any signal a handler
// will catch — SIGWINCH included, since the resize slice).
long console_read(char *buf, size_t len);

// The same read with a patience limit: `deadline` is an ABSOLUTE kTicksSinceStart
// value after which an empty wait gives up and returns CONSOLE_READ_TIMEOUT
// (0 = no deadline — block forever; console_read is exactly that spelling).
// A deadline already in the past is the POLL gait: one drain of whatever the
// keyboard has translated, then the verdict, never a park. Pending EOF and
// buffered bytes outrank the deadline — a poll that finds something delivers
// it like any read. The ms→ticks conversion is the syscall boundary's job
// (same doctrine as sleep(): the ABI speaks time, this file speaks ticks).
long console_read_deadline(char *buf, size_t len, uint64_t deadline);

// Put a byte BACK at the head of the console's input, to be delivered before
// anything else. Born 2026-08-07 for exactly one caller: the read-patience
// test reads the console during boot, and any type-ahead a human queued
// before husk's first prompt was being eaten by the probe — weeks of "it
// keeps losing my first three characters" traced to a test's shrug. A probe
// that must consume to observe now un-consumes on the way out. Returns false
// when the pushback slot is full (bounded, tiny — probes hold at most one).
bool console_unread(char c);

// The line-discipline peek (the ISIG/VINTR seed): called by keyboard.c at the
// delivery choke for every key-down, BEFORE the byte enters the console ring.
// Returns true if the byte was consumed as the interrupt character (ETX/0x03
// -> SIGINT pending on the foreground task); false means "just data, deliver
// it". Policy lives HERE, not in keyboard.c — the device layer stays blind to
// tasks and signals. IRQ-safe on purpose: the raise is one word-OR; the
// actual kill happens later, at the victim's own syscall boundary.
bool console_intr_intercept(char ascii);
// The tty-scoped core (PTY.md): a pty master's write asks on behalf of its
// SLAVE — the keystroke "happened" on the terminal that window represents.
// The focused-terminal spelling above is now a wrapper over this.
struct tty;
bool console_intr_intercept_tty(struct tty *tty, char ascii);

// Called from processSignals (scheduler context, every pass). If a reader is
// asleep in console_read AND the keyboard driver has input, wake the reader.
// Level-triggered on keyboard_has_event(), so a key arriving at any instant is
// caught on the next pass — the sleep/wake path is lost-wakeup-free.
void console_wake_if_ready(void);

#endif // CONSOLE_H
