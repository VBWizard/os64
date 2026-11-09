// test_utf8_host.c — os64_utf8_decode/encode against the Unicode standard's
// Table 3-7, with the classic bad inputs: overlong forms, surrogates, values
// past U+10FFFF, truncation, stray continuation bytes. Every code point in
// the space round-trips; every malformed byte is one replacement, one byte.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "os64/str.h"

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int bad(const char *bytes, size_t n)
{
    // Every byte of a malformed sequence is one replacement, one byte.
    size_t at = 0;
    while (at < n) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(bytes + at, n - at, &cp);
        CHECK(took == 1 && cp == OS64_UTF8_REPLACEMENT);
        at += took;
    }
    return 0;
}

int main(void)
{
    // Round trip the whole space, skipping the surrogate gap.
    char buf[4];
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        size_t n = os64_utf8_encode(cp, buf);
        CHECK(n == (cp < 0x80 ? 1u : cp < 0x800 ? 2u : cp < 0x10000 ? 3u : 4u));
        uint32_t back = 0;
        CHECK(os64_utf8_decode(buf, n, &back) == n && back == cp);
        // Truncated by one byte: one replacement, one byte consumed.
        if (n > 1) {
            CHECK(os64_utf8_decode(buf, n - 1, &back) == 1 && back == OS64_UTF8_REPLACEMENT);
        }
    }
    // A surrogate or an out-of-range value encodes as the replacement.
    CHECK(os64_utf8_encode(0xD800, buf) == 3 && memcmp(buf, "\xEF\xBF\xBD", 3) == 0);
    CHECK(os64_utf8_encode(0x110000, buf) == 3 && memcmp(buf, "\xEF\xBF\xBD", 3) == 0);

    // The classics.
    CHECK(bad("\xC0\x80", 2) == 0);                 // overlong NUL
    CHECK(bad("\xC1\xBF", 2) == 0);                 // overlong
    CHECK(bad("\xE0\x80\x80", 3) == 0);             // overlong 3-byte
    CHECK(bad("\xED\xA0\x80", 3) == 0);             // surrogate D800
    CHECK(bad("\xF0\x80\x80\x80", 4) == 0);         // overlong 4-byte
    CHECK(bad("\xF4\x90\x80\x80", 4) == 0);         // U+110000
    CHECK(bad("\xF5\x80\x80\x80", 4) == 0);         // lead past F4
    CHECK(bad("\x80\xBF", 2) == 0);                 // stray continuations
    CHECK(bad("\xFF", 1) == 0);

    // A good sequence after a bad byte decodes normally: the walk resyncs.
    const char mixed[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "\xC3" "z";
    uint32_t want[] = { 'a', 0xE9, 0x20AC, 0x1F600, OS64_UTF8_REPLACEMENT, 'z' };
    size_t at = 0, i = 0;
    while (at < sizeof(mixed) - 1) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(mixed + at, sizeof(mixed) - 1 - at, &cp);
        CHECK(took >= 1 && i < sizeof(want) / sizeof(want[0]) && cp == want[i]);
        at += took;
        i++;
    }
    CHECK(i == sizeof(want) / sizeof(want[0]));
    uint32_t cp = 7;
    CHECK(os64_utf8_decode("x", 0, &cp) == 0 && cp == 7);

    printf("test_utf8_host: %d checks passed\n", checks);
    return 0;
}
