// media_bytes.c — a sound's bytes off the network, the arithmetic
// (media_bytes.h). No library calls: the host drives it as it is.

#include "media_bytes.h"

yonder_media_seek_t yonder_media_seek_plan(const yonder_media_window_t *w, uint64_t offset,
                                           const yonder_media_seek_state_t *s)
{
    uint64_t end = w->base + w->have;
    if (offset >= w->base && offset <= end)
        return YONDER_MEDIA_SEEK_HERE;
    // A reader opening the media probes (the MP3 reader looks at the end
    // for tags) and carries on without what it asked: never a request for it.
    if (s->opening)
        return YONDER_MEDIA_SEEK_REFUSE;
    if (offset < w->base)
        return s->ranges ? YONDER_MEDIA_SEEK_JUMP : YONDER_MEDIA_SEEK_REFUSE;
    if (s->ended)
        return YONDER_MEDIA_SEEK_REFUSE;
    // A little past it, the bytes come anyway (the fetch, or the held one
    // resumed from the window's end), and the window is kept.
    bool coming = s->fetching || s->held;
    if (s->ranges && (!coming || offset - end > YONDER_MEDIA_JUMP_GAP))
        return YONDER_MEDIA_SEEK_JUMP;
    return coming ? YONDER_MEDIA_SEEK_WAIT : YONDER_MEDIA_SEEK_REFUSE;
}

uint64_t yonder_media_drop_for(const yonder_media_window_t *w, uint64_t n, uint64_t at, uint64_t max,
                               uint64_t behind)
{
    if (n <= max && w->have <= max - n)
        return 0;
    // have + n - max, without the sum wrapping.
    uint64_t need = w->have <= max ? n - (max - w->have)
                                   : (n > UINT64_MAX - (w->have - max) ? UINT64_MAX : w->have - max + n);
    uint64_t allowed = at > w->base && at - w->base > behind ? at - w->base - behind : 0;
    if (allowed > w->have)
        allowed = w->have;
    // All that may go, not only what is needed: the kept bytes move down
    // once for many arrivals instead of once for each.
    return need <= allowed ? allowed : UINT64_MAX;
}

bool yonder_media_take_more(const yonder_media_window_t *w, uint64_t at, uint64_t ahead)
{
    uint64_t end = w->base + w->have;
    return at >= end || end - at < ahead;
}

// ── The head ───────────────────────────────────────────────────────────

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool same_nocase(const char *a, size_t n, const char *word)
{
    size_t i = 0;
    for (; i < n; i++)
        if (word[i] == '\0' || lower(a[i]) != word[i])
            return false;
    return word[i] == '\0';
}

typedef struct {
    const char *at, *end;
} Lines;

// The next header's name and value (the value without the blanks around
// it or the line's end). False at the block's end.
static bool next_header(Lines *l, const char **name, size_t *nlen, const char **value, size_t *vlen)
{
    while (l->at < l->end) {
        const char *line = l->at, *p = line;
        while (p < l->end && *p != '\n')
            p++;
        l->at = p < l->end ? p + 1 : p;
        const char *stop = p > line && p[-1] == '\r' ? p - 1 : p;
        const char *colon = line;
        while (colon < stop && *colon != ':')
            colon++;
        if (colon == stop)
            continue;
        const char *v = colon + 1;
        while (v < stop && (*v == ' ' || *v == '\t'))
            v++;
        while (stop > v && (stop[-1] == ' ' || stop[-1] == '\t'))
            stop--;
        *name = line;
        *nlen = (size_t)(colon - line);
        *value = v;
        *vlen = (size_t)(stop - v);
        return true;
    }
    return false;
}

// Decimal digits at `*p`, which moves past them. False for none, or a
// number past INT64_MAX.
static bool digits(const char **p, const char *end, uint64_t *out)
{
    uint64_t n = 0;
    const char *q = *p;
    while (q < end && *q >= '0' && *q <= '9') {
        uint64_t d = (uint64_t)(*q - '0');
        if (n > ((uint64_t)INT64_MAX - d) / 10)
            return false;
        n = n * 10 + d;
        q++;
    }
    if (q == *p)
        return false;
    *p = q;
    *out = n;
    return true;
}

int64_t yonder_media_content_length(const char *headers, size_t length)
{
    Lines l = {headers, headers + length};
    const char *name, *value;
    size_t nlen, vlen;
    int64_t size = -1;
    while (next_header(&l, &name, &nlen, &value, &vlen)) {
        if (same_nocase(name, nlen, "content-encoding"))
            return -1;
        if (same_nocase(name, nlen, "content-length")) {
            const char *p = value;
            uint64_t n;
            size = digits(&p, value + vlen, &n) && p == value + vlen ? (int64_t)n : -1;
        }
    }
    return size;
}

bool yonder_media_content_range(const char *headers, size_t length, uint64_t *first, int64_t *total)
{
    Lines l = {headers, headers + length};
    const char *name, *value;
    size_t nlen, vlen;
    while (next_header(&l, &name, &nlen, &value, &vlen)) {
        if (!same_nocase(name, nlen, "content-range"))
            continue;
        const char *p = value, *end = value + vlen;
        if (vlen < 6 || !same_nocase(p, 5, "bytes") || p[5] != ' ')
            return false;
        p += 6;
        uint64_t a, b;
        if (!digits(&p, end, &a) || p >= end || *p++ != '-' || !digits(&p, end, &b) || b < a || p >= end ||
            *p++ != '/')
            return false;
        if (end - p == 1 && *p == '*') {
            *total = -1;
        } else {
            uint64_t t;
            if (!digits(&p, end, &t) || p != end || t <= b)
                return false;
            *total = (int64_t)t;
        }
        *first = a;
        return true;
    }
    return false;
}

void yonder_media_validator(const char *headers, size_t length, char *out, size_t cap)
{
    Lines l = {headers, headers + length};
    const char *name, *value, *etag = NULL, *modified = NULL;
    size_t nlen, vlen, elen = 0, mlen = 0;
    while (next_header(&l, &name, &nlen, &value, &vlen)) {
        if (same_nocase(name, nlen, "etag")) {
            etag = value;
            elen = vlen;
        } else if (same_nocase(name, nlen, "last-modified")) {
            modified = value;
            mlen = vlen;
        }
    }
    const char *prefix = etag != NULL ? "etag " : "modified ";
    const char *v = etag != NULL ? etag : modified;
    size_t vl = etag != NULL ? elen : mlen, pl = etag != NULL ? 5 : 9;
    if (cap == 0)
        return;
    out[0] = '\0';
    if (v == NULL || pl + vl + 1 > cap)
        return;
    for (size_t i = 0; i < pl; i++)
        out[i] = prefix[i];
    for (size_t i = 0; i < vl; i++)
        out[pl + i] = v[i];
    out[pl + vl] = '\0';
}

bool yonder_media_accepts_ranges(const char *headers, size_t length)
{
    Lines l = {headers, headers + length};
    const char *name, *value;
    size_t nlen, vlen;
    while (next_header(&l, &name, &nlen, &value, &vlen)) {
        if (!same_nocase(name, nlen, "accept-ranges"))
            continue;
        const char *p = value, *end = value + vlen;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
                p++;
            const char *token = p;
            while (p < end && *p != ',' && *p != ' ' && *p != '\t')
                p++;
            if (same_nocase(token, (size_t)(p - token), "bytes"))
                return true;
        }
    }
    return false;
}

void yonder_media_content_type(const char *headers, size_t length, char *out, size_t cap)
{
    Lines l = {headers, headers + length};
    const char *name, *value;
    size_t nlen, vlen;
    if (cap == 0)
        return;
    out[0] = '\0';
    while (next_header(&l, &name, &nlen, &value, &vlen)) {
        if (!same_nocase(name, nlen, "content-type"))
            continue;
        size_t n = 0;
        while (n < vlen && value[n] != ';')
            n++;
        while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t'))
            n--;
        if (n + 1 > cap)
            return;
        for (size_t i = 0; i < n; i++)
            out[i] = value[i];
        out[n] = '\0';
    }
}

// ── The words a failure is recorded in ─────────────────────────────────

typedef struct {
    char *out;
    size_t cap, at;
} Text;

static void put(Text *t, const char *s)
{
    for (; *s != '\0' && t->at + 1 < t->cap; s++)
        t->out[t->at++] = *s;
    t->out[t->at] = '\0';
}

static void put_number(Text *t, uint64_t n)
{
    char digits[24];
    int i = (int)sizeof(digits) - 1;
    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    put(t, digits + i);
}

static bool same_text(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
        a++, b++;
    return *a == *b;
}

void yonder_media_failure_text(const yonder_media_failure_t *f, char *out, size_t cap)
{
    static const char *const kClass[] = {"error", "aborted", "network", "decode", "not supported"};
    Text t = {out, cap, 0};
    if (cap == 0)
        return;
    out[0] = '\0';
    put(&t, f->element);
    put(&t, ", ");
    if (f->error >= 1 && f->error <= 4) {
        put(&t, kClass[f->error]);
    } else {
        put(&t, "error ");
        put_number(&t, (uint64_t)(f->error < 0 ? 0 : f->error));
    }
    put(&t, ": ");
    put(&t, f->reason[0] != '\0' ? f->reason : "no reason given");
    put(&t, " (");
    if (f->fetched) {
        if (f->status != 0) {
            put(&t, "HTTP ");
            put_number(&t, (uint64_t)f->status);
        } else {
            put(&t, "no answer");
        }
        if (f->type[0] != '\0') {
            put(&t, ", ");
            put(&t, f->type);
        }
        put(&t, ", ");
        put_number(&t, f->received);
        put(&t, f->received == 1 ? " byte received, " : " bytes received, ");
        if (f->range_from == 0) {
            put(&t, "the first request");
        } else {
            put(&t, "a Range request from byte ");
            put_number(&t, f->range_from);
        }
    } else {
        put(&t, "a file");
    }
    bool said = f->said[0] != '\0' && !same_text(f->said, f->reason);
    bool named = f->format[0] != '\0' || f->sound[0] != '\0' || f->picture[0] != '\0';
    if (said || named) {
        put(&t, "; libmedia");
        if (said) {
            put(&t, ": ");
            put(&t, f->said);
        }
        if (f->format[0] != '\0') {
            put(&t, said ? ", read as " : " read it as ");
            put(&t, f->format);
        }
        const char *codec[2] = {f->sound, f->picture};
        const char *kind[2] = {"sound ", "picture "};
        bool unplayable[2] = {f->sound_unplayable, f->picture_unplayable};
        for (int i = 0; i < 2; i++) {
            if (codec[i][0] == '\0')
                continue;
            put(&t, ", ");
            put(&t, kind[i]);
            put(&t, codec[i]);
            if (unplayable[i])
                put(&t, " (not decoded here)");
        }
    }
    put(&t, ")");
}
