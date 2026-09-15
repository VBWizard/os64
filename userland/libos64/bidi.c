// bidi.c — which way a run of text runs. See os64/str.h for the rule.
//
// THE TABLE RECORDS THE EXCEPTIONS, because that is how Unicode writes it:
// DerivedBidiClass.txt's own default is `@missing: 0000..10FFFF;
// Left_To_Right`, so a code point the file does not list is L, and what is
// worth storing is everything that is not. It is a fraction of the ranges a
// complete map would take, and it keeps the file's default rule as this
// code's default rule instead of as a second thing to maintain. The ranges
// are sorted and disjoint, so a lookup is a binary search.

#include "os64/str.h"

static const struct {
    uint32_t first, last;
    uint8_t klass;
} kBidiRanges[] = {
#include "bidi_ranges.inc"
};

os64_bidi_strong_t os64_bidi_strong(uint32_t cp)
{
    size_t low = 0, high = sizeof(kBidiRanges) / sizeof(kBidiRanges[0]);
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (cp < kBidiRanges[mid].first)
            high = mid;
        else if (cp > kBidiRanges[mid].last)
            low = mid + 1;
        else
            return (os64_bidi_strong_t)kBidiRanges[mid].klass;
    }
    return OS64_BIDI_L;
}

os64_bidi_strong_t os64_bidi_first_strong(const char *utf8, size_t len)
{
    if (!utf8)
        return OS64_BIDI_NOT_STRONG;
    for (size_t at = 0; at < len;) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(utf8 + at, len - at, &cp);
        if (took == 0)
            break;
        at += took;
        os64_bidi_strong_t klass = os64_bidi_strong(cp);
        if (klass != OS64_BIDI_NOT_STRONG)
            return klass;
    }
    return OS64_BIDI_NOT_STRONG;
}
