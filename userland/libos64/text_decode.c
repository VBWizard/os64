#include "text_internal.h"
#include "text_w1_data.h"
#include "os64/str.h"
#include "os64/charset.h"

bool text_w1_supported(uint32_t c)
{
    return (c>=0x20 && c<=0x7e) || (c>=0xa0 && c<=0x17f && c!=0xad) ||
        (c>=0x2010 && c<=0x2015) || (c>=0x2018 && c<=0x2022) || c==0x2026 ||
        c==0x2030 || (c>=0x2032 && c<=0x2033) || (c>=0x2039 && c<=0x203a) ||
        c==0x20ac || c==0x2122 || (c>=0x2190 && c<=0x2193) || c==0x2212 ||
        c==0x221a || c==0x221e || c==0x2260 || c==0x2264 || c==0x2265 ||
        (c>=0x2500 && c<=0x259f);
}
static bool latin(uint32_t c)
{
    if ((c>='A' && c<='Z') || (c>='a' && c<='z')) return true;
    for (size_t i=0;i<sizeof(text_latin_letters)/sizeof(*text_latin_letters);i++)
        if (text_latin_letters[i]==c) return true;
    return false;
}
text_cluster text_decode(const uint8_t *bytes, size_t length, size_t at,
    os64_text_encoding_t encoding, bool grid)
{
    uint32_t scalar;
    size_t n=encoding==OS64_TEXT_UTF8_WESTERN_V1
        ? os64_utf8_decode((const char *)bytes+at,length-at,&scalar) : 1;
    if (encoding!=OS64_TEXT_UTF8_WESTERN_V1)
        scalar=encoding==OS64_TEXT_CP437 ? os64_cp437_codepoint(bytes[at]) : bytes[at];
    text_cluster out={at+n,scalar,false};
    if (grid) return out;
    if (scalar==9) {out.scalar=TEXT_TAB;return out;}
    if (encoding!=OS64_TEXT_UTF8_WESTERN_V1) {
        if (scalar<32 || (scalar>=127 && scalar<160)) out.scalar=TEXT_MARKER;
        return out;
    }
    bool base=latin(scalar), mark=scalar>=0x300 && scalar<=0x36f;
    if (base || mark) {
        size_t count=mark?1:0;
        uint32_t first=mark?scalar:0;
        while (out.end<length) {
            uint32_t c;size_t step=os64_utf8_decode((const char *)bytes+out.end,length-out.end,&c);
            if (c<0x300 || c>0x36f) break;
            if (!count) first=c;
            count++;out.end+=step;
        }
        if (mark) {out.scalar=TEXT_MARKER;return out;}
        if (count==1) {
            for (size_t i=0;i<sizeof(text_compositions)/sizeof(*text_compositions);i++)
                if (text_compositions[i].base==scalar && text_compositions[i].mark==first) {
                    out.scalar=text_compositions[i].composed;return out;
                }
        }
        out.extra_marker=count!=0;
    }
    if (!text_w1_supported(scalar)) out.scalar=TEXT_MARKER;
    return out;
}
