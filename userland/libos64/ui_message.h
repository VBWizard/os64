#ifndef OS64_UI_MESSAGE_H
#define OS64_UI_MESSAGE_H
#include "os64/ui_result.h"
#include "os64/text.h"
#include "os64/text_profile.h"
/* Shared Western UI policy, including its missing-glyph marker semantics.
 * Malformed/control/unsupported clusters are refused for action/status text. */
static inline os64_ui_status_code_t ui_message_length(const char *s, size_t cap, size_t *out)
{
    if (!s) return OS64_UI_INVALID;
    size_t n = 0;
    while (n < cap && s[n]) ++n;
    if (n == cap) return OS64_UI_LIMIT;
    for (size_t i = 0; i < n;) {
        os64_w1_cluster_t c = os64_w1_decode((const uint8_t *)s, n, i, false);
        if (c.scalar == OS64_W1_MARKER || c.extra_marker || c.scalar == OS64_W1_TAB)
            return OS64_UI_INVALID;
        i = c.end;
    }
    *out = n;
    return OS64_UI_OK;
}

static inline os64_ui_status_code_t ui_message_font_result(os64_font_status_t rc)
{
    switch (rc) {
    case OS64_FONT_OK: return OS64_UI_OK;
    case OS64_FONT_NO_MEMORY: return OS64_UI_NO_MEMORY;
    case OS64_FONT_LIMIT: return OS64_UI_LIMIT;
    case OS64_FONT_BUSY: return OS64_UI_BUSY;
    default: return OS64_UI_INVALID;
    }
}
#endif
