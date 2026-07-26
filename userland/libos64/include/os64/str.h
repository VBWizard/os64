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

// Copy `n` bytes. No overlap handling (that is memmove's job, and nothing has
// asked for one yet).
void *os64_memcpy(void *dst, const void *src, size_t n);

// Fill `n` bytes with `c`.
void *os64_memset(void *dst, int c, size_t n);

#endif // OS64_STR_H
