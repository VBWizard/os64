#ifndef OS64_APPEARANCE_H
#define OS64_APPEARANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// /sys/appearance: boot-lifetime opaque payload, immutable per-open reads.
// One write is a compare-and-publish command: expected generation in the
// header, then up to 4096 payload bytes. Success returns the complete length;
// refusal is negative and leaves the store unchanged. Never retry a suffix.
// Generation zero with an empty payload denotes the initial, unpublished state.
#define OS64_APPEARANCE_PATH "/sys/appearance"
#define OS64_APPEARANCE_PAYLOAD_MAX 4096u
#define OS64_APPEARANCE_HEADER_MAX 34u
#define OS64_APPEARANCE_MAX (OS64_APPEARANCE_HEADER_MAX + OS64_APPEARANCE_PAYLOAD_MAX)

// Canonical framing: "generation = " + 1..20 decimal digits + LF.
// No sign, leading zeroes, or extra whitespace. Payload bytes remain opaque.
static inline bool os64_appearance_header_read(const char *s, size_t n,
                                               uint64_t *generation, size_t *body)
{
    const char prefix[] = "generation = ";
    size_t i = sizeof(prefix) - 1, start = i;
    if (!s || n <= i) return false;
    for (size_t j = 0; j < i; ++j)
        if (s[j] != prefix[j]) return false;
    uint64_t value = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        unsigned d = (unsigned)(s[i] - '0');
        if (i - start >= 20 || value > (UINT64_MAX - d) / 10) return false;
        value = value * 10 + d;
        ++i;
    }
    if (i == start || i >= n || s[i] != '\n' ||
        (i > start + 1 && s[start] == '0')) return false;
    *generation = value;
    *body = i + 1;
    return true;
}

// Caller provides OS64_APPEARANCE_HEADER_MAX bytes; result excludes any NUL.
static inline size_t os64_appearance_header_write(char *s, uint64_t generation)
{
    const char prefix[] = "generation = ";
    char digits[20];
    size_t n = 0, used = sizeof(prefix) - 1;
    do {
        digits[n++] = (char)('0' + generation % 10);
        generation /= 10;
    } while (generation);
    for (size_t i = 0; i < used; ++i) s[i] = prefix[i];
    while (n) s[used++] = digits[--n];
    s[used++] = '\n';
    return used;
}
#endif
