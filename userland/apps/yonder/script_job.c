// script_job.c — a script's source fetched and decoded on a worker
// (script_job.h).

#include "script_job.h"
#include "html/html.h"
#include "os64/mem.h"
#include "os64/slurp.h"
#include "os64/str.h"

#define SCRIPT_ACCEPT "*/*"

// windows-1252's bytes 0x80..0x9F, which are not Latin-1's controls
// (Encoding Standard, index-windows-1252); zero leaves the byte's own code
// point, as the index does for the five it leaves undefined.
static const uint16_t kWindows1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};

// The length of a valid UTF-8 sequence at `p`, or 0 when it is not one.
static size_t utf8_sequence(const uint8_t *p, size_t left)
{
    uint8_t c = p[0];
    if (c < 0x80)
        return 1;
    size_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (n == 0 || n > left)
        return 0;
    for (size_t i = 1; i < n; i++)
        if ((p[i] & 0xC0) != 0x80)
            return 0;
    if (c == 0xE0 && p[1] < 0xA0)
        return 0;                       // overlong
    if (c == 0xED && p[1] >= 0xA0)
        return 0;                       // a surrogate
    if (c == 0xF0 && p[1] < 0x90)
        return 0;                       // overlong
    if (c == 0xF4 && p[1] >= 0x90)
        return 0;                       // past U+10FFFF
    return n;
}

static bool utf8_valid(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length;) {
        size_t n = utf8_sequence(bytes + i, length - i);
        if (n == 0)
            return false;
        i += n;
    }
    return true;
}

static bool names_utf8(const char *label)
{
    if (label == NULL || label[0] == '\0')
        return false;
    const char *name = os64_html_encoding_for_label(label, os64_strlen(label));
    return name != NULL && os64_streq(name, "utf-8");
}

static size_t put(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

char *yonder_script_decode(const uint8_t *bytes, size_t length, const char *label,
                           const char *fallback, size_t *out_length)
{
    bool utf8;
    if (length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        bytes += 3;
        length -= 3;
        utf8 = true;
    } else if (label != NULL && label[0] != '\0') {
        utf8 = names_utf8(label);
    } else if (fallback != NULL && fallback[0] != '\0') {
        utf8 = names_utf8(fallback);
    } else {
        utf8 = utf8_valid(bytes, length);
    }
    // Each input byte is at most three output bytes either way: a 1252 byte
    // becomes up to three, and an invalid UTF-8 byte one U+FFFD.
    if (length > (SIZE_MAX - 1) / 3)
        return NULL;
    char *out = os64_malloc(length * 3 + 1);
    if (out == NULL)
        return NULL;
    size_t at = 0;
    for (size_t i = 0; i < length;) {
        if (utf8) {
            size_t n = utf8_sequence(bytes + i, length - i);
            if (n == 0) {
                at += put(out + at, 0xFFFD);
                i++;
            } else {
                os64_memcpy(out + at, bytes + i, n);
                at += n;
                i += n;
            }
        } else {
            uint8_t c = bytes[i++];
            uint32_t cp = c >= 0x80 && c <= 0x9F && kWindows1252[c - 0x80] != 0 ? kWindows1252[c - 0x80] : c;
            at += put(out + at, cp);
        }
    }
    out[at] = '\0';
    *out_length = at;
    return out;
}

int64_t yonder_script_run(void *job, bool (*cancelled)(void *ctx), void *ctx, void **out)
{
    yonder_script_job_t *j = job;
    yonder_script_t *s = os64_calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;
    *out = s;
    os64_strcopy(s->url, sizeof(s->url), j->url);
    // A script beside a page read from disk is a file, with no label but
    // the fallback.
    if (os64_strlen(j->url) > 7 && os64_memcmp(j->url, "file://", 7) == 0) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (os64_slurp(j->url + 7, SCRIPT_SOURCE_MAX, &bytes, &len) != OS64_SLURP_OK)
            return 0;
        s->source = yonder_script_decode(bytes, len, NULL, j->fallback, &s->length);
        os64_free(bytes);
        s->ok = s->source != NULL;
        return s->ok ? 1 : 0;
    }
    os64_fetch_options_t opt = {0};
    opt.user_agent = j->agent;
    opt.accept = SCRIPT_ACCEPT;
    j->hooks.cancelled = cancelled;
    j->hooks.cancel_ctx = ctx;
    way_whole_t body;
    if (!way_fetch_whole(&j->hooks, j->url, &opt, SCRIPT_SOURCE_MAX, &body))
        return 0;
    // A script is any 2xx body: the old web serves scripts as text/html,
    // text/plain and worse, and browsers run them all.
    bool usable = body.status >= 200 && body.status < 300;
    if (!usable || cancelled(ctx)) {
        os64_free(body.bytes);
        return 0;
    }
    os64_strcopy(s->url, sizeof(s->url), body.url);
    s->source = yonder_script_decode(body.bytes, body.len, body.charset, j->fallback, &s->length);
    os64_free(body.bytes);
    s->ok = s->source != NULL;
    return s->ok ? 1 : 0;
}

void yonder_script_release(void *job, void *product)
{
    yonder_script_t *s = product;
    if (s != NULL) {
        os64_free(s->source);
        os64_free(s);
    }
    os64_free(job);
}
