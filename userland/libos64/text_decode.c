#include "text_internal.h"
#include "os64/text_profile.h"
#include "os64/charset.h"

bool text_w1_supported(uint32_t scalar) { return os64_w1_supported(scalar); }
text_cluster text_decode(const uint8_t *bytes, size_t length, size_t at,
    os64_text_encoding_t encoding, bool grid)
{
    if (encoding == OS64_TEXT_UTF8_WESTERN_V1) {
        os64_w1_cluster_t d = os64_w1_decode(bytes, length, at, false);
        return (text_cluster){d.end, d.scalar, d.extra_marker};
    }
    uint32_t scalar = encoding == OS64_TEXT_CP437 ? os64_cp437_codepoint(bytes[at]) : bytes[at];
    if (!grid) {
        if (scalar == 9) scalar = TEXT_TAB;
        else if (scalar < 32 || (scalar >= 127 && scalar < 160)) scalar = TEXT_MARKER;
    }
    return (text_cluster){at + 1, scalar, false};
}
