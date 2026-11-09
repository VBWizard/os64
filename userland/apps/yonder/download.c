// download.c — what a download is called, and how fast it goes (download.h).

#include "download.h"

#include <stdint.h>

#include "os64/fmt.h"
#include "os64/str.h"

static bool space(char c)
{
    return c == ' ' || c == '\t';
}

// `a` and `b` agree on their first `n` bytes, ASCII case aside.
static bool same_nocase(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = (char)(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

static int hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// `in[0..len)` percent-decoded into `out`; the decoded length. A `%` not
// followed by two hex digits is kept as it is.
static size_t percent_decode(const char *in, size_t len, char *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        int hi, lo;
        if (in[i] == '%' && i + 2 < len && (hi = hex(in[i + 1])) >= 0 && (lo = hex(in[i + 2])) >= 0) {
            out[n++] = (char)(hi * 16 + lo);
            i += 2;
        } else {
            out[n++] = in[i];
        }
    }
    return n;
}

// One parameter of a Content-Disposition line: `name` (compared without
// case) and its value, unquoted (RFC 6266 § 4.1: a token or a quoted
// string), into `out`; its length, or -1 when the line does not have it.
static long disposition_param(const char *line, const char *name, char *out, size_t cap)
{
    size_t want = os64_strlen(name);
    const char *s = line;
    // The disposition type comes first and is no parameter.
    while (*s != '\0' && *s != ';')
        s++;
    while (*s == ';') {
        s++;
        while (space(*s))
            s++;
        const char *key = s;
        while (*s != '\0' && *s != '=' && *s != ';' && !space(*s))
            s++;
        size_t key_len = (size_t)(s - key);
        while (space(*s))
            s++;
        bool match = key_len == want && same_nocase(key, name, want);
        size_t n = 0;
        if (*s == '=') {
            s++;
            while (space(*s))
                s++;
            if (*s == '"') {
                s++;
                while (*s != '\0' && *s != '"') {
                    if (*s == '\\' && s[1] != '\0')
                        s++;
                    if (match && n + 1 < cap)
                        out[n++] = *s;
                    s++;
                }
                if (*s == '"')
                    s++;
            } else {
                const char *v = s;
                while (*s != '\0' && *s != ';')
                    s++;
                const char *end = s;
                while (end > v && space(end[-1]))
                    end--;
                for (; match && v < end && n + 1 < cap; v++)
                    out[n++] = *v;
            }
        }
        while (*s != '\0' && *s != ';')
            s++;
        if (match) {
            out[n] = '\0';
            return (long)n;
        }
    }
    return -1;
}

// The name a Content-Disposition line gives, raw, into `out`: `filename*`
// in UTF-8 (RFC 8187's charset'language'encoded), else `filename`. Its
// length, 0 when it gives none.
static size_t disposition_name(const char *line, char *out, size_t cap)
{
    char value[512];
    long n = disposition_param(line, "filename*", value, sizeof(value));
    if (n > 0) {
        const char *quote = os64_strchr(value, '\'');
        const char *second = quote != NULL ? os64_strchr(quote + 1, '\'') : NULL;
        if (second != NULL && (size_t)(quote - value) == 5 && same_nocase(value, "utf-8", 5)) {
            const char *encoded = second + 1;
            return percent_decode(encoded, os64_strlen(encoded), out, cap);
        }
    }
    n = disposition_param(line, "filename", out, cap);
    return n > 0 ? (size_t)n : 0;
}

// The last segment of `url`'s path, percent-decoded, into `out`: what is
// after the last `/` before any `?` or `#`. Its length, 0 when there is
// none (an address with no path, or one ending in `/`).
static size_t url_name(const char *url, char *out, size_t cap)
{
    const char *authority = url;
    while (*authority != '\0' && !(authority[0] == ':' && authority[1] == '/' && authority[2] == '/'))
        authority++;
    if (*authority == '\0')
        return 0;
    const char *path = os64_strchr(authority + 3, '/');
    if (path == NULL)
        return 0;
    const char *end = path;
    while (*end != '\0' && *end != '?' && *end != '#')
        end++;
    const char *last = end;
    while (last > path && last[-1] != '/')
        last--;
    return percent_decode(last, (size_t)(end - last), out, cap);
}

// `raw[0..len)` made a name: its last path part, refused bytes made `_`,
// leading dots and trailing dots and spaces gone, cut to `cap` with its
// extension kept. Its length.
static size_t tidy(const char *raw, size_t len, char *out, size_t cap)
{
    size_t from = 0;
    for (size_t i = 0; i < len; i++)
        if (raw[i] == '/' || raw[i] == '\\')
            from = i + 1;
    while (from < len && raw[from] == '.')
        from++;
    size_t end = len;
    while (end > from && (raw[end - 1] == '.' || raw[end - 1] == ' '))
        end--;
    size_t n = end - from;
    size_t room = cap - 1 < YONDER_DOWNLOAD_NAME_MAX - 1 ? cap - 1 : YONDER_DOWNLOAD_NAME_MAX - 1;
    // Too long: keep the extension (a short one, after the last dot) and
    // cut the rest, never inside a UTF-8 sequence.
    size_t ext = n;
    if (n > room) {
        for (size_t i = n; i > 1; i--)
            if (raw[from + i - 1] == '.') {
                ext = i - 1;
                break;
            }
        if (n - ext > 16 || n - ext >= room)
            ext = n;
    }
    size_t tail = n - ext;                      // the extension's bytes, dot and all
    size_t head = ext;
    if (head + tail > room) {
        head = room - tail;
        while (head > 0 && ((uint8_t)raw[from + head] & 0xC0) == 0x80)
            head--;
    }
    size_t k = 0;
    for (size_t i = 0; i < head + tail; i++) {
        char c = raw[from + (i < head ? i : ext + (i - head))];
        out[k++] = (uint8_t)c < 0x20 || c == 0x7F ? '_' : c;
    }
    out[k] = '\0';
    return k;
}

void yonder_download_name(const char *disposition, const char *url, char *out, size_t cap)
{
    if (cap == 0)
        return;
    char raw[512];
    size_t len = disposition != NULL && disposition[0] != '\0' ? disposition_name(disposition, raw, sizeof(raw)) : 0;
    if (len == 0 || tidy(raw, len, out, cap) == 0) {
        len = url != NULL ? url_name(url, raw, sizeof(raw)) : 0;
        if (len == 0 || tidy(raw, len, out, cap) == 0)
            os64_strcopy(out, cap, "download");
    }
}

bool yonder_download_free_name(const char *name, bool (*taken)(void *ctx, const char *name), void *ctx,
                               char *out, size_t cap)
{
    if (os64_strcopy(out, cap, name) >= cap)
        return false;
    if (!taken(ctx, out))
        return true;
    const char *dot = name[0] != '\0' ? os64_strchr(name + 1, '.') : NULL;
    size_t stem = dot != NULL ? (size_t)(dot - name) : os64_strlen(name);
    for (int n = 1; n < 100; n++) {
        int w = os64_snprintf(out, cap, "%.*s-%d%s", (int)stem, name, n, name + stem);
        if (w < 0 || (size_t)w >= cap)
            return false;
        if (!taken(ctx, out))
            return true;
    }
    return false;
}

void yonder_download_size(uint64_t bytes, char *out, size_t cap)
{
    const uint64_t mb = 1024 * 1024, gb = mb * 1024;
    if (bytes < mb) {
        os64_snprintf(out, cap, "%lu KB", (unsigned long)((bytes + 1023) / 1024));
    } else {
        uint64_t unit = bytes < gb ? mb : gb, tenths = bytes * 10 / unit;
        os64_snprintf(out, cap, "%lu.%lu %s", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10),
                      unit == mb ? "MB" : "GB");
    }
}

void yonder_download_pace_note(yonder_download_pace_t *pace, uint64_t now_ms, uint64_t got)
{
    if (pace->count > 0 && now_ms - pace->at[pace->newest] < YONDER_DOWNLOAD_SAMPLE_MS)
        return;
    pace->newest = pace->count == 0 ? 0 : (pace->newest + 1) % YONDER_DOWNLOAD_SAMPLES;
    pace->at[pace->newest] = now_ms;
    pace->got[pace->newest] = got;
    if (pace->count < YONDER_DOWNLOAD_SAMPLES)
        pace->count++;
}

bool yonder_download_pace_rate(const yonder_download_pace_t *pace, uint64_t now_ms, uint64_t got,
                               uint64_t *rate)
{
    if (pace->count == 0)
        return false;
    // Oldest first, k = 0 the oldest kept: the first inside the window,
    // else the newest; then one further back if that spans under a second.
    uint32_t oldest = (pace->newest + YONDER_DOWNLOAD_SAMPLES - (pace->count - 1)) % YONDER_DOWNLOAD_SAMPLES;
    uint32_t k = 0;
    while (k + 1 < pace->count &&
           now_ms - pace->at[(oldest + k) % YONDER_DOWNLOAD_SAMPLES] > YONDER_DOWNLOAD_WINDOW_MS)
        k++;
    if (k > 0 && now_ms - pace->at[(oldest + k) % YONDER_DOWNLOAD_SAMPLES] < 1000)
        k--;
    uint32_t base = (oldest + k) % YONDER_DOWNLOAD_SAMPLES;
    if (now_ms < pace->at[base] || now_ms - pace->at[base] < 1000)
        return false;
    uint64_t moved = got > pace->got[base] ? got - pace->got[base] : 0;
    *rate = moved * 1000 / (now_ms - pace->at[base]);
    return true;
}

bool yonder_download_left(uint64_t left, uint64_t rate, char *out, size_t cap)
{
    if (rate == 0)
        return false;
    uint64_t s = left / rate + (left % rate != 0), m = (s + 59) / 60;
    if (s < 60)
        os64_snprintf(out, cap, "%lu s left", (unsigned long)s);
    else if (m < 60)
        os64_snprintf(out, cap, "%lu min left", (unsigned long)m);
    else if (m % 60 == 0)
        os64_snprintf(out, cap, "%lu h left", (unsigned long)(m / 60));
    else
        os64_snprintf(out, cap, "%lu h %lu min left", (unsigned long)(m / 60), (unsigned long)(m % 60));
    return true;
}
