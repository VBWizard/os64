#ifndef OS64_UTF8_SCALAR_H
#define OS64_UTF8_SCALAR_H
#include <stddef.h>
#include <stdint.h>

/* Strict scalar decoding usable by text layout and window decorations.
 * An invalid sequence consumes one byte and yields U+FFFD. */
static inline size_t os64_utf8_scalar(const char *s, size_t n, uint32_t *cp)
{
    if (n == 0)
        return 0;
    const unsigned char *b = (const unsigned char *)s;
    unsigned char lead = b[0];
    if (lead < 0x80) {
        *cp = lead;
        return 1;
    }
    // The lead byte says how many continuation bytes follow and what the
    // FIRST of them may be — the narrowed first-byte ranges are what rule
    // out overlong forms (E0 80..9F), surrogates (ED A0..BF) and values
    // past U+10FFFF (F4 90..BF) without a second pass. Unicode Table 3-7.
    size_t need;
    unsigned char lo = 0x80, hi = 0xBF;
    uint32_t v;
    if (lead >= 0xC2 && lead <= 0xDF) { need = 1; v = lead & 0x1F; }
    else if (lead >= 0xE0 && lead <= 0xEF) {
        need = 2; v = lead & 0x0F;
        if (lead == 0xE0) lo = 0xA0;
        if (lead == 0xED) hi = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        need = 3; v = lead & 0x07;
        if (lead == 0xF0) lo = 0x90;
        if (lead == 0xF4) hi = 0x8F;
    } else {
        *cp = 0xfffdu;   // C0, C1, F5..FF, or a stray continuation byte
        return 1;
    }
    for (size_t i = 1; i <= need; i++) {
        if (i >= n || b[i] < lo || b[i] > hi) {
            *cp = 0xfffdu;   // truncated, or not a continuation byte
            return 1;
        }
        v = (v << 6) | (b[i] & 0x3F);
        lo = 0x80;
        hi = 0xBF;
    }
    *cp = v;
    return need + 1;
}

#endif
