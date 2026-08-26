#ifndef OS64_FMT_H
#define OS64_FMT_H

// libos64 formatted output (LIBOS64.md layer). Pulled into existence by ls —
// the first app that needed columns and refused to count bytes by hand
// (consumer-driven, as designed: an os64_write with a hand-counted length of
// 70 was the demand signal).
//
// The conversion vocabulary is what systems programs actually use:
//   %s %c %%  |  %d %i %u %x %X %p  |  l / ll length modifiers
//   field width (fixed or `*` from the args), precision (`.N` / `.*`),
//   '0' and '-' flags
// The column cheat sheet (pulled into existence by ls):
//   %-40s      goto-column-41 for what follows (width = MINIMUM, pads)
//   %.39s      clip a string at 39 chars (precision = MAXIMUM for %s)
//   %-40.39s   both: the bulletproof column — survives any filename
//   %-*s       measured columns: width comes from the argument list, so
//              ls can find the longest name first and pass maxlen+1
// Deliberately absent until something demands them: floating point (nothing
// in an OS wants it), %n (nothing GOOD wants it), locale anything.
//
// THE TYPE→FORMAT TABLE (house convention 2026-07-19: stdint.h names in all
// new code — see nvme.c for the style done right since day one). os64 is
// LP64 and owns this printf, so the whole mapping is five lines, no PRIu64
// macro soup, ever:
//   uint8_t / uint16_t / uint32_t     %u   (%x hex)   [varargs promote to int]
//   int8_t  / int16_t  / int32_t      %d
//   uint64_t / uintptr_t / size_t     %lu  (%lx hex)
//   int64_t                           %ld
//   any pointer                       %p
//
// These are freestanding and reentrant: no shared buffers, no allocation —
// os64_printf's scratch lives on the caller's stack.
//
// THE COMPILER CHECKS EVERY CALL. Each function below carries
// __attribute__((format(printf, F, A))) — F is the format's parameter
// number, A the first vararg's (0 for the va_list variant) — so gcc reads
// the format string at the call site and matches each conversion against
// the argument's type, exactly as it does for the C library's printf, and
// -Werror turns a mismatch into a failed build. Two things it catches that
// no IDE did (2026-08-26, ping's "%ul" — read as unsigned int followed by a
// literal l, with a negative int64_t behind it): a length modifier on the
// wrong side of the letter, and a 64-bit value handed to a 32-bit
// conversion (the table above is the whole rule: %u for the small ones,
// %lu / %ld for the 64-bit ones). What it does NOT check is whether THIS
// printf implements the conversion — the checker knows C's vocabulary,
// and ours is the subset listed above; %zu or %hd would pass the compiler
// and print wrong. Stay inside the table.

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define OS64_PRINTF(fmt_index, first_arg) \
	__attribute__((format(printf, fmt_index, first_arg)))

// Format into buf (always NUL-terminated if size > 0). Returns the length
// the FULL result would have — if it's >= size, the output was truncated
// (the standard snprintf contract, which is one of C's genuinely good ones).
int32_t os64_snprintf(char *buf, size_t size, const char *fmt, ...) OS64_PRINTF(3, 4);
int32_t os64_vsnprintf(char *buf, size_t size, const char *fmt, va_list args) OS64_PRINTF(3, 0);

// Format and write to a handle. os64_printf goes to 1 (stdout by
// convention); os64_hprintf targets any handle — 2 for errors, a file, a
// pipe end; it neither knows nor cares (that's the point of handles).
// Returns bytes written, or negative on error. Output beyond 1024 formatted
// bytes per call is truncated — print in pieces if you mean more.
int32_t os64_printf(const char *fmt, ...) OS64_PRINTF(1, 2);
int32_t os64_hprintf(int32_t handle, const char *fmt, ...) OS64_PRINTF(2, 3);

#endif // OS64_FMT_H
