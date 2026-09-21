#ifndef CONSOLE_FONT_H
#define CONSOLE_FONT_H

// console_font.h — changing the face the virtual terminals draw with, while
// the machine runs. CONSOLE_FONTS.md is the design; /sys/console/font is the
// door (sysfs.c), and this is what stands behind it.
//
// TWO HALVES, IN TWO CONTEXTS, and the split is the design:
//
//   The door (begin/append/submit) runs wherever a sysfs write and close run
//   — a syscall with this core's interrupts off, or the burial of a task
//   that died with the file open. It may only do what is cheap and cannot
//   block: gather the bytes, VALIDATE them, and queue the result. A font the
//   loader refuses is refused HERE, synchronously, so the verdict exists by
//   the time the close returns — and /sys/console/font is where a program
//   reads it, because that text says WHY and a return value cannot.
//
//   The swap (console_font_sweep) runs in kworker: task context, interrupts
//   on, allowed to allocate. Re-shaping eight terminals' grids and repainting
//   a screen is work for there and nowhere else. It is the same arrangement
//   as the shell a keystroke summons on a dark terminal, for the same reason.
//
// So a submit that returns 0 means "accepted and queued", not "on the glass".
// The status text says which, and a program that needs to know reads it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct console_font_pending console_font_pending_t;

// Start gathering an image. NULL only if the machine has no terminals yet.
console_font_pending_t *console_font_begin(void);
// Add bytes. -1 once the image has outgrown the loader's fence; the pending
// is then poisoned and its submit will refuse, so a writer that ignores the
// -1 still cannot install half a font.
int console_font_append(console_font_pending_t *p, const void *bytes, size_t length);
// The writer is done. Validates, queues for the swap, frees `p` either way.
// The single word "boot" (whitespace around it ignored) asks for the face the
// machine booted with. 0 accepted, -1 refused.
int console_font_submit(console_font_pending_t *p);
// The open failed after begin: free `p`, change nothing.
void console_font_discard(console_font_pending_t *p);

// Is a face queued for the swap? Lock-free read, for the wake path.
bool console_font_pending(void);
// kworker context ONLY. Performs a queued swap; true if it did one.
bool console_font_sweep(void);

// /sys/console/font's text. Returns the bytes written (no terminator counted).
size_t console_font_status(char *out, size_t cap);

#endif // CONSOLE_FONT_H
