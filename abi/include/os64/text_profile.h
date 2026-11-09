#ifndef OS64_TEXT_PROFILE_H
#define OS64_TEXT_PROFILE_H
#include <stdbool.h>
#include "os64/utf8_scalar.h"
#include "os64/text_w1_data.h"

#define OS64_W1_MARKER UINT32_MAX
#define OS64_W1_TAB (UINT32_MAX - 1u)
typedef struct { size_t end; uint32_t scalar; bool extra_marker; } os64_w1_cluster_t;

/* The bounded Western profile is defined in FONT_CONTRACTS.md. Call decode
 * with at < length. LATIN1 preserves legacy byte-title character mapping. */
static inline bool os64_w1_supported(uint32_t c)
{
    return (c>=0x20 && c<=0x7e) || (c>=0xa0 && c<=0x17f && c!=0xad) ||
        (c>=0x2010 && c<=0x2015) || (c>=0x2018 && c<=0x2022) || c==0x2026 ||
        c==0x2030 || (c>=0x2032 && c<=0x2033) || (c>=0x2039 && c<=0x203a) ||
        c==0x20ac || c==0x2122 || (c>=0x2190 && c<=0x2193) || c==0x2212 ||
        c==0x221a || c==0x221e || c==0x2260 || c==0x2264 || c==0x2265 ||
        (c>=0x2500 && c<=0x259f);
}
static inline bool os64_w1_latin(uint32_t c)
{
    if ((c>='A' && c<='Z') || (c>='a' && c<='z')) return true;
    for (size_t i=0;i<sizeof(text_latin_letters)/sizeof(*text_latin_letters);i++)
        if (text_latin_letters[i]==c) return true;
    return false;
}
static inline os64_w1_cluster_t os64_w1_decode(const uint8_t *bytes, size_t length, size_t at,
    bool latin1)
{
    uint32_t scalar=OS64_W1_MARKER;
    size_t n = latin1 ? 1 : os64_utf8_scalar((const char *)bytes + at, length - at, &scalar);
    if (latin1) scalar = bytes[at];
    os64_w1_cluster_t out = {at + n, scalar, false};
    if (scalar == 9) { out.scalar = OS64_W1_TAB; return out; }
    if (latin1) {
        if (scalar < 32 || (scalar >= 127 && scalar < 160)) out.scalar = OS64_W1_MARKER;
        return out;
    }
    bool base=os64_w1_latin(scalar), mark=scalar>=0x300 && scalar<=0x36f;
    if (base || mark) {
        size_t count=mark?1:0;
        uint32_t first=mark?scalar:0;
        while (out.end<length) {
            uint32_t c=OS64_W1_MARKER;size_t step=os64_utf8_scalar((const char *)bytes+out.end,length-out.end,&c);
            if (c<0x300 || c>0x36f) break;
            if (!count) first=c;
            count++;out.end+=step;
        }
        if (mark) {out.scalar=OS64_W1_MARKER;return out;}
        if (count==1) {
            for (size_t i=0;i<sizeof(text_compositions)/sizeof(*text_compositions);i++)
                if (text_compositions[i].base==scalar && text_compositions[i].mark==first) {
                    out.scalar=text_compositions[i].composed;return out;
                }
        }
        out.extra_marker=count!=0;
    }
    if (!os64_w1_supported(scalar)) out.scalar=OS64_W1_MARKER;
    return out;
}

#endif
