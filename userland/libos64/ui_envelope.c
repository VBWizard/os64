#include "ui_envelope.h"
#include "os64/conf.h"
#include "os64/appearance.h"
#include "os64/str.h"
#include "os64/fmt.h"
#include "os64/mem.h"

static bool dotted_key(const char *s)
{
    bool dot = false, component = false;
    for (; *s; ++s) {
        if (*s == '.') {
            if (!component) return false;
            dot = true; component = false;
        } else {
            if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                  (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) return false;
            component = true;
        }
    }
    return dot && component;
}

static bool font_key(const char *key)
{
    const char *const roles[] = {"ui", "terminal", "document"};
    const char *const fields[] = {"face", "size", "fallback.1", "fallback.2"};
    for (size_t r = 0; r < 3; ++r) for (size_t f = 0; f < 4; ++f) {
        char name[40];
        os64_snprintf(name, sizeof(name), "fonts.%s.%s", roles[r], fields[f]);
        if (os64_streq_nocase(name, key)) return true;
    }
    return false;
}

typedef struct {
    unsigned owner;
    bool delivered, invalid;
    char *font_text;
    size_t font_used;
    uint64_t serial;
    bool font_present;
} line_t;

static bool key_line(const char *key, const char *value, void *user)
{
    line_t *p = user;
    p->delivered = true;
    if (!key) { p->invalid = true; return false; }
    p->owner = ui_theme_key_owner(key);
    if (os64_streq(key, "inherit")) {
        p->owner = UI_ENVELOPE_INHERIT;
        if (!os64_streq(value, "startup")) p->invalid = true;
    } else if (os64_streq_nocase(key, "fonts.serial")) {
        p->owner = UI_ENVELOPE_FONTS;
        p->font_present = true;
        uint64_t serial = 0;
        if (!*value) p->invalid = true;
        for (size_t i = 0; value[i]; ++i) {
            if (value[i] < '0' || value[i] > '9' ||
                serial > (UINT64_MAX - (unsigned)(value[i] - '0')) / 10) {
                p->invalid = true; break;
            }
            serial = serial * 10 + (unsigned)(value[i] - '0');
        }
        if (!serial) p->invalid = true;
        p->serial = serial;
    } else if (font_key(key)) {
        p->owner = UI_ENVELOPE_FONTS;
        p->font_present = true;
        size_t n = os64_strlen(key);
        bool size_key = n >= 5 && os64_streq_nocase(key + n - 5, ".size");
        if (!size_key && value[0] != '/' && !os64_streq(value, "builtin"))
            p->invalid = true;
        if (p->font_text) {
            int added = os64_snprintf(p->font_text + p->font_used,
                OS64_CONF_MAX - p->font_used, "%s = %s\n", key + 6, value);
            if (added < 0 || (size_t)added > OS64_CONF_MAX - 1 - p->font_used)
                p->invalid = true;
            else p->font_used += (size_t)added;
        }
    } else if (!p->owner && !dotted_key(key)) p->invalid = true;
    return false;
}

static int64_t walk(const char *text, size_t len, unsigned owners, bool unowned,
                     char *out, size_t cap, line_t *line)
{
    if (!text || !out || !cap || len >= OS64_CONF_MAX)
        return OS64_CONF_BAD_SETTING;
    size_t used = 0, start = 0;
    while (start < len) {
        size_t end = start;
        while (end < len && text[end] != '\n') ++end;
        line->owner = 0; line->delivered = false; line->invalid = false;
        int64_t result = os64_conf_parse(text + start, end - start, key_line, line);
        if (result < 0) return result;
        size_t first = start;
        while (first < end && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) ++first;
        if (line->invalid || (!line->delivered && first < end && text[first] != '#') ||
            (line->owner == UI_ENVELOPE_INHERIT && start != 0)) return OS64_CONF_BAD_SETTING;
        size_t next = end + (end < len);
        if ((line->owner & owners) || (!line->owner && unowned)) {
            if (next - start >= cap - used) return OS64_CONF_TRUNCATED;
            os64_memcpy(out + used, text + start, next - start);
            used += next - start;
        }
        start = next;
    }
    out[used] = 0;
    return (int64_t)used;
}

int64_t ui_envelope_select(const char *text, size_t len, unsigned owners, bool unowned,
                            char *out, size_t cap)
{
    line_t line = {0};
    return walk(text, len, owners, unowned, out, cap, &line);
}

int64_t ui_envelope_fonts(const char *text, size_t len, os64_font_config_t *out, bool *present, uint64_t *serial)
{
    if (!out || !present) return OS64_CONF_BAD_SETTING;
    char *storage = os64_malloc(2 * OS64_CONF_MAX);
    if (!storage) return OS64_CONF_NO_MEMORY;
    line_t line = {.font_text = storage};
    storage[0] = 0;
    int64_t result = walk(text, len, 0, false, storage + OS64_CONF_MAX,
                          OS64_CONF_MAX, &line);
    if (result >= 0) {
        os64_font_config_status_t status = os64_font_config_decode(storage, line.font_used,
                                                                  "/fonts.conf", out, NULL);
        result = status == OS64_FONT_CONFIG_NO_MEMORY ? OS64_CONF_NO_MEMORY :
                 status ? OS64_CONF_BAD_SETTING : 0;
        if (!result) { *present = line.font_present; if (serial) *serial = line.serial; }
    }
    os64_free(storage);
    return result;
}

int64_t ui_envelope_merge(const char *base, size_t len, const char *replacement,
    size_t replacement_len, unsigned owners, char *out, size_t cap)
{
    int64_t n = ui_envelope_select(base, len, ~owners, true, out, cap);
    if (n < 0) return n;
    size_t used = (size_t)n;
    if (used && out[used - 1] != '\n') {
        if (used + 1 >= cap) return OS64_CONF_TRUNCATED;
        out[used++] = '\n';
    }
    n = ui_envelope_select(replacement, replacement_len, owners, false, out + used, cap - used);
    if (n < 0) return n;
    if (used + (size_t)n > OS64_APPEARANCE_PAYLOAD_MAX) return OS64_CONF_TRUNCATED;
    return (int64_t)used + n;
}
