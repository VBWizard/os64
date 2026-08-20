#ifndef OS64_STR_H
#define OS64_STR_H

// libos64 string primitives (LIBOS64.md layer).
//
// Freestanding: there is no libc under this, and there is not going to be one
// — os64 owns the whole world here. These exist because apps kept re-growing
// them privately (io.c had a static strlen; husk has str_eq; echo has str_is),
// which is the consumer-driven signal that they belong in the library.
//
// NOTE what is deliberately ABSENT: strcpy. It is the single most notorious
// buffer overflow in the language's history — it cannot be called safely
// without already knowing the answer it refuses to tell you. strncpy is worse
// in a subtler way: it does not NUL-terminate when the source fills the
// buffer, so it turns an overflow into a string with no end (the kernel
// already carries a `strncpy won't NUL-terminate an over-long src` comment
// paid for the hard way). os64 supplies neither, and supplies the function
// they should have been instead.

#include <stddef.h>
#include <stdint.h>    // os64_atoi/atou return fixed-width ints
#include <stdbool.h>   // os64_streq returns one; a header must include what it uses

// Length of a NUL-terminated string, not counting the NUL.
size_t os64_strlen(const char *s);

// Copy `src` into `dst`, never writing more than `cap` bytes INCLUDING the
// terminator, and ALWAYS terminating (even on truncation, even for cap == 1).
//
// Returns the length src WANTED to be. That is the useful answer rather than
// the flattering one:
//
//     if (os64_strcopy(dst, sizeof(dst), src) >= sizeof(dst))
//         /* it did not fit -- and you know it, at the call site */;
//
// Returning the bytes actually written would make "fit exactly" and
// "truncated" indistinguishable, which is precisely the ambiguity that makes
// every other bounded-copy function in C a trap. (The semantics are OpenBSD
// strlcpy's, adopted on merit and renamed: the `l` in strlcpy tells a reader
// nothing, and os64 does not inherit jargon.)
//
// Argument order is (destination, its size, source) — matching os64_snprintf,
// so the buffer and the number that bounds it always sit next to each other.
size_t os64_strcopy(char *dst, size_t cap, const char *src);

// Compare two NUL-terminated strings for exact equality. A bool, not a
// three-way int: every caller in this tree so far (husk's str_eq, echo's
// str_is) wanted "are these the same word", and `strcmp(a,b) == 0` reading as
// "equal" has confused people for fifty years. Ordering comparison arrives if
// something ever needs to SORT — consumer-driven, like everything else here.
bool os64_streq(const char *a, const char *b);

// Parse a decimal integer from the front of `s`: optional +/- sign, then
// digits, stopping at the first non-digit (the classic contract). Returns 0
// for no-digits — indistinguishable from a real zero, which is atoi's
// fifty-year-old wart; when a caller needs to tell them apart, a
// full-diagnosis parser can join it (consumer-driven, as ever). The name
// survives on merit: `atoi` is one of the handful of Unix names (fork, exec)
// that earned its keep — every C programmer alive reads it instantly.
// (Graduated from top's `temporaryAtoi` — "the library guy" was off playing
// with the network, per the heckling in topmain.c, and has now returned.)
int64_t os64_atoi(const char *s);

// The unsigned sibling: digits only, no sign, same stop-at-first-non-digit
// contract. This is the one /proc parsing wants — every value in a status
// file is an unsigned decimal, and a stray '-' should end the number, not
// negate a tick count.
uint64_t os64_atou(const char *s);

// The HEX sibling, and the one /proc's ADDRESSES want: every %p the kernel
// prints is zero-padded hex with no 0x prefix (sprintf.c), so os64_atou would
// read "0000000070000000" as seventy million decimal and be quietly, ruinously
// wrong — it does not fail, it succeeds at another number. Accepts an optional
// 0x/0X, consumes [0-9a-fA-F], stops at the first character that is not one.
//
// It takes the ENDPTR that os64_atoi's comment above promised to a future
// caller ("a full-diagnosis parser can join it, consumer-driven"): if `end` is
// non-NULL it is set to where parsing stopped. That is strtol's one genuinely
// good idea, and it is what makes TWO numbers in one string a non-problem —
// you never split, you parse in place and step over the delimiter:
//
//     const char *p;
//     uint64_t lo = os64_xtou(text, &p);      // p lands on the '-'
//     uint64_t hi = os64_xtou(p + 1, NULL);
//
// No digits consumed is reported by *end coming back EQUAL to `s` — the
// distinction atoi has never been able to make. Like its decimal siblings it
// wraps rather than complaining if you feed it more than 16 digits; a value
// that long is a corrupt file, not an arithmetic question.
uint64_t os64_xtou(const char *s, const char **end);

// Parse the "<hex>-<hex>" range that /proc prints for any span of address
// space — `heap` in status (heapStart-heapEnd) and every line of `maps`
// (procfs.c). Returns true only if both halves carried at least one digit and
// a '-' separated them; on false, *lo and *hi are untouched, because a parser
// that half-fills its outputs on failure is how a caller ends up subtracting
// a stale number from a fresh one and believing the result.
//
// Either output may be NULL if you only want the other. The SIZE of a range
// is hi - lo — the first number is the low end, in both files.
bool os64_parse_range(const char *s, uint64_t *lo, uint64_t *hi);

// Copy `n` bytes. No overlap handling (that is memmove's job, below).
void *os64_memcpy(void *dst, const void *src, size_t n);

// Copy `n` bytes between REGIONS THAT MAY OVERLAP — the editor's verb
// (scribe was the consumer that finally asked, 2026-08-20: inserting a
// character into a line is a rightward shuffle of everything after it, and
// deleting one is the same shuffle leftward). Direction is picked from the
// pointers, the classic way.
void *os64_memmove(void *dst, const void *src, size_t n);

// Fill `n` bytes with `c`.
void *os64_memset(void *dst, int c, size_t n);

#endif // OS64_STR_H
