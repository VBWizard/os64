#include "internal.h"
#include "os64/fmt.h"
#include "os64/url.h"

/* URL and URLSearchParams (the URL Standard, § 6). An address is parsed by
 * the rule the page's own links are resolved by (os64_page_resolve_in:
 * libos64's grammar, with the URL Standard's input cleaning and
 * percent-encoding and the special schemes' default ports), so a script's
 * `new URL(href, base)` and an `<a href>` agree on what an address is.
 * Where that grammar is not yet the URL Standard's parser (no userinfo, no
 * IPv6 host, no IDNA, no path that begins with a slash after a scheme the
 * standard does not know (`aaa:/b`), an opaque path's trailing spaces kept
 * as written, an address past OS64_URL_REF_MAX, a fragment left as
 * written), the difference is the grammar's to close, not this file's: an
 * address it refuses is a TypeError here, as an invalid URL is.
 *
 * A URL object holds its address serialized (href); every component is
 * read from that string, and every setter builds the address it describes
 * and parses it whole, keeping the old one when the new one does not
 * parse, as the standard's setters keep it. Its searchParams is made with
 * it and the two stay one: a change to either is the other's at once. A
 * query its searchParams grows is not parsed, so it may pass what a parse
 * takes; such an address is held and read back, and a setter, which would
 * parse it, keeps it as it is. */

static JSClassID url_slot, params_slot, iterator_slot;

typedef struct {
    char *name, *value;          /* UTF-8, js_malloc'd */
    size_t name_length, value_length;
} DPair;

typedef struct {
    DPair *pairs;
    size_t count, cap;
    JSValue url;                 /* the URL whose query this is, or undefined */
} DParams;

typedef struct {
    char *href;                  /* js_malloc'd, NUL-terminated */
    size_t length;
    JSValue params;
} DUrl;

typedef struct {
    JSValue params;
    uint32_t index;
    int kind;                    /* I_ENTRIES, I_KEYS, I_VALUES */
} DIterator;

enum { I_ENTRIES, I_KEYS, I_VALUES };

/* ── Addresses ───────────────────────────────────────────────────────── */

static uint16_t default_port(const char *scheme, size_t length)
{
    static const struct { const char *scheme; uint16_t port; } kSpecial[] = {
        {"http", 80}, {"https", 443}, {"ws", 80}, {"wss", 443}, {"ftp", 21},
    };
    for (size_t i = 0; i < sizeof(kSpecial) / sizeof(kSpecial[0]); i++)
        if (os64_strlen(kSpecial[i].scheme) == length && os64_memcmp(kSpecial[i].scheme, scheme, length) == 0)
            return kSpecial[i].port;
    return 0;
}

/* The URL Standard's special schemes: these five and file. */
static bool special(const char *scheme, size_t length)
{
    return default_port(scheme, length) != 0 || (length == 4 && os64_memcmp(scheme, "file", 4) == 0);
}

bool d_origin(const char *url, char *out, size_t cap)
{
    os64_url_t parsed;
    if (os64_url_parse(url, &parsed) != OS64_URL_OK ||
        default_port(parsed.scheme, os64_strlen(parsed.scheme)) == 0)
        return os64_strcopy(out, cap, "null") < cap;
    uint16_t standard = default_port(parsed.scheme, os64_strlen(parsed.scheme));
    int n = parsed.port != 0 && parsed.port != standard
        ? os64_snprintf(out, cap, "%s://%s:%u", parsed.scheme, parsed.host, (unsigned)parsed.port)
        : os64_snprintf(out, cap, "%s://%s", parsed.scheme, parsed.host);
    return n > 0 && (size_t)n < cap;
}

/* An address taken apart: each component's span in the href. The
 * authority's spans are empty for an address without one (an opaque path,
 * `mailto:`, `data:`). The query and fragment spans include their `?` and
 * `#`, and are empty when the address has none. */
typedef struct {
    size_t scheme_end;           /* the ':' */
    bool authority;
    size_t host, host_end, port, port_end, path, path_end, query, query_end, fragment, fragment_end;
} Parts;

static void parts_of(const char *href, size_t length, Parts *p)
{
    os64_memset(p, 0, sizeof(*p));
    size_t at = 0;
    while (at < length && href[at] != ':') at++;
    p->scheme_end = at;
    at = at < length ? at + 1 : at;
    p->authority = at + 1 < length && href[at] == '/' && href[at + 1] == '/';
    if (p->authority) {
        at += 2;
        p->host = at;
        while (at < length && href[at] != '/' && href[at] != '?' && href[at] != '#') at++;
        size_t end = at, colon = end;
        for (size_t i = end; i > p->host; i--)
            if (href[i - 1] == ':') { colon = i - 1; break; }
        p->host_end = colon;
        p->port = colon < end ? colon + 1 : end;
        p->port_end = end;
    } else {
        p->host = p->host_end = p->port = p->port_end = at;
    }
    p->path = at;
    while (at < length && href[at] != '?' && href[at] != '#') at++;
    p->path_end = p->query = at;
    if (at < length && href[at] == '?') while (at < length && href[at] != '#') at++;
    p->query_end = p->fragment = at;
    p->fragment_end = length;
}

/* `input` against `base` (NULL: none) by the page's rule; an address the
 * grammar refuses, or one with no scheme, is false. */
static bool resolve(const char *input, const char *base, char *out, size_t cap)
{
    if (!os64_page_resolve_in(NULL, base, input, out, cap)) return false;
    char scheme[OS64_URL_SCHEME_MAX];
    return os64_url_scheme_of(out, scheme, sizeof(scheme));
}

/* A string an address is made from, made ready for the grammar's door,
 * which takes C strings: with `trim`, the C0 controls and spaces at either
 * end go first (the URL Standard's first step, which must see a NUL there
 * before it is escaped), and every NUL left is %00, which is what the
 * standard's percent-encoding makes of one in a path, a query or a
 * fragment, and what makes a host that holds one fail, as the standard's
 * host parser fails it. False on no memory, with the exception pending. */
static bool address_text(os64_dom_t *dom, JSContext *ctx, DString *text, bool trim)
{
    size_t first = 0, end = text->length, nuls = 0;
    if (trim) {
        while (first < end && (unsigned char)text->data[first] <= 0x20) first++;
        while (end > first && (unsigned char)text->data[end - 1] <= 0x20) end--;
    }
    for (size_t at = first; at < end; at++) nuls += text->data[at] == '\0';
    if (first == 0 && end == text->length && nuls == 0) return true;
    char *out = d_alloc(dom, end - first + 2 * nuls + 1);
    if (out == NULL) {
        d_error(ctx, "QuotaExceededError", "URL quota exceeded");
        return false;
    }
    size_t used = 0;
    for (size_t at = first; at < end; at++) {
        if (text->data[at] != '\0') { out[used++] = text->data[at]; continue; }
        out[used++] = '%'; out[used++] = '0'; out[used++] = '0';
    }
    out[used] = '\0';
    d_string_free(dom, text);
    text->data = out;
    text->length = used;
    return true;
}

/* The URL Standard's "API URL parser": a base, when given, must be an
 * address itself. 1 parsed into `out` (OS64_DOM_URL_MAX bytes), 0
 * refused, -1 on no memory with the exception pending. */
static int url_parse(os64_dom_t *dom, JSContext *ctx, const DString *input, const DString *base, char *out)
{
    if (base == NULL) return resolve(input->data, NULL, out, OS64_DOM_URL_MAX) ? 1 : 0;
    char *whole = d_alloc(dom, OS64_DOM_URL_MAX);
    if (whole == NULL) {
        d_error(ctx, "QuotaExceededError", "URL quota exceeded");
        return -1;
    }
    int parsed = resolve(base->data, NULL, whole, OS64_DOM_URL_MAX) &&
                 resolve(input->data, whole, out, OS64_DOM_URL_MAX) ? 1 : 0;
    d_free(dom, whole);
    return parsed;
}

/* ── application/x-www-form-urlencoded ───────────────────────────────── */

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decoded, `+` as a space, then UTF-8 decoded with each invalid
 * sequence U+FFFD: the urlencoded parser's name and value. js_malloc'd;
 * NULL on no memory. */
static char *form_decode(JSContext *ctx, const char *text, size_t length, size_t *out_length)
{
    uint8_t *bytes = js_malloc(ctx, length + 1);
    if (bytes == NULL) return NULL;
    size_t used = 0;
    for (size_t at = 0; at < length; at++) {
        if (text[at] == '+') bytes[used++] = ' ';
        else if (text[at] == '%' && at + 2 < length && hex(text[at + 1]) >= 0 && hex(text[at + 2]) >= 0) {
            bytes[used++] = (uint8_t)(hex(text[at + 1]) << 4 | hex(text[at + 2]));
            at += 2;
        } else bytes[used++] = (uint8_t)text[at];
    }
    /* Each byte is at most three bytes of UTF-8 out (U+FFFD for a bad one). */
    char *out = js_malloc(ctx, used * 3 + 1);
    if (out == NULL) { js_free(ctx, bytes); return NULL; }
    size_t written = 0;
    for (size_t at = 0; at < used;) {
        uint8_t c = bytes[at];
        size_t need = c < 0x80 ? 0 : c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 4;
        bool good = need < 4 && at + need < used;
        if (good) {
            uint8_t lower = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80;
            uint8_t upper = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
            for (size_t i = 1; good && i <= need; i++) {
                uint8_t b = bytes[at + i];
                good = i == 1 ? b >= lower && b <= upper : b >= 0x80 && b <= 0xbf;
            }
        }
        if (!good) {
            /* The longest valid prefix is one error, and the next byte is
             * read anew: the Encoding Standard's UTF-8 decoder. */
            size_t skip = 1;
            if (need >= 1 && need < 4) {
                uint8_t lower = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80;
                uint8_t upper = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
                while (skip <= need && at + skip < used &&
                       bytes[at + skip] >= (skip == 1 ? lower : 0x80) && bytes[at + skip] <= (skip == 1 ? upper : 0xbf))
                    skip++;
            }
            out[written++] = (char)0xef; out[written++] = (char)0xbf; out[written++] = (char)0xbd;
            at += skip;
            continue;
        }
        os64_memcpy(out + written, bytes + at, need + 1);
        written += need + 1;
        at += need + 1;
    }
    js_free(ctx, bytes);
    out[written] = '\0';
    *out_length = written;
    return out;
}

/* The urlencoded byte serializer's rule: ASCII alphanumerics and `*-._`
 * as they are, a space as `+`, every other byte %XX. */
static size_t form_encode(const char *text, size_t length, char *out)
{
    static const char kHex[] = "0123456789ABCDEF";
    size_t used = 0;
    for (size_t at = 0; at < length; at++) {
        unsigned char c = (unsigned char)text[at];
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '*' || c == '-' || c == '.' || c == '_';
        if (keep) { if (out != NULL) out[used] = (char)c; used++; }
        else if (c == ' ') { if (out != NULL) out[used] = '+'; used++; }
        else {
            if (out != NULL) { out[used] = '%'; out[used + 1] = kHex[c >> 4]; out[used + 2] = kHex[c & 15]; }
            used += 3;
        }
    }
    return used;
}

static void pairs_free(JSContext *ctx, DParams *params)
{
    for (size_t i = 0; i < params->count; i++) {
        js_free(ctx, params->pairs[i].name);
        js_free(ctx, params->pairs[i].value);
    }
    js_free(ctx, params->pairs);
    params->pairs = NULL;
    params->count = params->cap = 0;
}

/* One more pair, at the end: copies of the name and value. -1 on no
 * memory, with the list as it was. */
static int pair_add(JSContext *ctx, DParams *params, const char *name, size_t name_length, const char *value,
                    size_t value_length, bool owned)
{
    if (params->count == params->cap) {
        size_t cap = params->cap != 0 ? params->cap * 2 : 4;
        DPair *pairs = js_realloc(ctx, params->pairs, cap * sizeof(*pairs));
        if (pairs == NULL) {
            if (owned) { js_free(ctx, (char *)name); js_free(ctx, (char *)value); }
            return -1;
        }
        params->pairs = pairs;
        params->cap = cap;
    }
    char *n = (char *)name, *v = (char *)value;
    if (!owned) {
        n = js_malloc(ctx, name_length + 1);
        v = n != NULL ? js_malloc(ctx, value_length + 1) : NULL;
        if (v == NULL) { js_free(ctx, n); return -1; }
        os64_memcpy(n, name, name_length);
        n[name_length] = '\0';
        os64_memcpy(v, value, value_length);
        v[value_length] = '\0';
    }
    params->pairs[params->count++] = (DPair){n, v, name_length, value_length};
    return 0;
}

/* The urlencoded parser over `text`, into a fresh list. -1 on no memory,
 * with `params` empty. */
static int form_parse(JSContext *ctx, DParams *params, const char *text, size_t length)
{
    pairs_free(ctx, params);
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && text[end] != '&') end++;
        if (end > at) {
            size_t equals = at;
            while (equals < end && text[equals] != '=') equals++;
            size_t name_length, value_length;
            char *name = form_decode(ctx, text + at, equals - at, &name_length);
            char *value = name != NULL ? form_decode(ctx, text + (equals < end ? equals + 1 : end),
                                                     equals < end ? end - equals - 1 : 0, &value_length)
                                       : NULL;
            if (value == NULL || pair_add(ctx, params, name, name_length, value, value_length, true) < 0) {
                if (value == NULL) js_free(ctx, name);
                pairs_free(ctx, params);
                return -1;
            }
        }
        at = end + 1;
    }
    return 0;
}

/* The urlencoded serializer: js_malloc'd, NUL-terminated. NULL on no
 * memory. */
static char *form_serialize(JSContext *ctx, const DParams *params, size_t *length)
{
    size_t need = 0;
    for (size_t i = 0; i < params->count; i++)
        need += (i != 0) + form_encode(params->pairs[i].name, params->pairs[i].name_length, NULL) + 1 +
                form_encode(params->pairs[i].value, params->pairs[i].value_length, NULL);
    char *out = js_malloc(ctx, need + 1);
    if (out == NULL) return NULL;
    size_t at = 0;
    for (size_t i = 0; i < params->count; i++) {
        if (i != 0) out[at++] = '&';
        at += form_encode(params->pairs[i].name, params->pairs[i].name_length, out + at);
        out[at++] = '=';
        at += form_encode(params->pairs[i].value, params->pairs[i].value_length, out + at);
    }
    out[at] = '\0';
    *length = at;
    return out;
}

/* ── The objects' state ──────────────────────────────────────────────── */

static void url_finalizer(JSRuntime *rt, JSValue value)
{
    DUrl *url = JS_GetOpaque(value, os64_js_class_id(&url_slot));
    if (url == NULL) return;
    js_free_rt(rt, url->href);
    JS_FreeValueRT(rt, url->params);
    js_free_rt(rt, url);
}

static void url_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DUrl *url = JS_GetOpaque(value, os64_js_class_id(&url_slot));
    if (url != NULL) JS_MarkValue(rt, url->params, mark);
}

static void params_finalizer(JSRuntime *rt, JSValue value)
{
    DParams *params = JS_GetOpaque(value, os64_js_class_id(&params_slot));
    if (params == NULL) return;
    for (size_t i = 0; i < params->count; i++) {
        js_free_rt(rt, params->pairs[i].name);
        js_free_rt(rt, params->pairs[i].value);
    }
    js_free_rt(rt, params->pairs);
    JS_FreeValueRT(rt, params->url);
    js_free_rt(rt, params);
}

static void params_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DParams *params = JS_GetOpaque(value, os64_js_class_id(&params_slot));
    if (params != NULL) JS_MarkValue(rt, params->url, mark);
}

static void iterator_finalizer(JSRuntime *rt, JSValue value)
{
    DIterator *it = JS_GetOpaque(value, os64_js_class_id(&iterator_slot));
    if (it == NULL) return;
    JS_FreeValueRT(rt, it->params);
    js_free_rt(rt, it);
}

static void iterator_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DIterator *it = JS_GetOpaque(value, os64_js_class_id(&iterator_slot));
    if (it != NULL) JS_MarkValue(rt, it->params, mark);
}

static DUrl *url_of(JSContext *ctx, JSValueConst value)
{
    DUrl *url = JS_GetOpaque(value, os64_js_class_id(&url_slot));
    if (url == NULL) JS_ThrowTypeError(ctx, "Expected a URL");
    return url;
}

static DParams *params_of(JSContext *ctx, JSValueConst value)
{
    DParams *params = JS_GetOpaque(value, os64_js_class_id(&params_slot));
    if (params == NULL) JS_ThrowTypeError(ctx, "Expected a URLSearchParams");
    return params;
}

/* A URL's address replaced by `href` (already parsed), and its
 * searchParams made its new query's. -1 on no memory, with neither
 * changed. */
static int url_set(JSContext *ctx, DUrl *url, const char *href)
{
    size_t length = os64_strlen(href);
    char *copy = js_malloc(ctx, length + 1);
    if (copy == NULL) return -1;
    os64_memcpy(copy, href, length + 1);
    DParams *params = JS_IsUndefined(url->params) ? NULL : JS_GetOpaque(url->params, os64_js_class_id(&params_slot));
    if (params != NULL) {
        Parts p;
        parts_of(copy, length, &p);
        size_t from = p.query < p.query_end ? p.query + 1 : p.query_end;
        DParams fresh = {0};
        if (form_parse(ctx, &fresh, copy + from, p.query_end - from) < 0) {
            js_free(ctx, copy);
            return -1;
        }
        pairs_free(ctx, params);
        params->pairs = fresh.pairs;
        params->count = fresh.count;
        params->cap = fresh.cap;
    }
    js_free(ctx, url->href);
    url->href = copy;
    url->length = length;
    return 0;
}

/* A URLSearchParams's change, made its URL's query: the serialization, or
 * no query at all for an empty one (the URL Standard's "update"). */
static int params_update(JSContext *ctx, DParams *params)
{
    if (JS_IsUndefined(params->url)) return 0;
    DUrl *url = JS_GetOpaque(params->url, os64_js_class_id(&url_slot));
    if (url == NULL) return 0;
    size_t query_length;
    char *query = form_serialize(ctx, params, &query_length);
    if (query == NULL) return -1;
    Parts p;
    parts_of(url->href, url->length, &p);
    size_t keep = p.query, tail = p.fragment_end - p.fragment;
    size_t length = keep + (query_length != 0 ? 1 + query_length : 0) + tail;
    char *href = js_malloc(ctx, length + 1);
    if (href == NULL) { js_free(ctx, query); return -1; }
    os64_memcpy(href, url->href, keep);
    size_t at = keep;
    if (query_length != 0) {
        href[at++] = '?';
        os64_memcpy(href + at, query, query_length);
        at += query_length;
    }
    os64_memcpy(href + at, url->href + p.fragment, tail);
    href[length] = '\0';
    js_free(ctx, query);
    js_free(ctx, url->href);
    url->href = href;
    url->length = length;
    return 0;
}

/* A new URLSearchParams with `prototype`, belonging to `owner` (a URL, or
 * undefined). */
static JSValue params_new(JSContext *ctx, JSValueConst prototype, JSValueConst owner)
{
    JSClassID id = os64_js_class_id(&params_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DParams *params = js_mallocz(ctx, sizeof(*params));
    if (params == NULL) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    params->url = JS_DupValue(ctx, owner);
    JS_SetOpaque(object, params);
    return object;
}

/* ── URL ─────────────────────────────────────────────────────────────── */

/* new URL(url, base): the address, and its searchParams over its query. */
static JSValue url_make(JSContext *ctx, JSValueConst prototype, const char *href)
{
    JSClassID id = os64_js_class_id(&url_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DUrl *url = js_mallocz(ctx, sizeof(*url));
    if (url == NULL) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    url->params = JS_UNDEFINED;
    JS_SetOpaque(object, url);
    JSValue params = params_new(ctx, JS_UNDEFINED, object);
    if (JS_IsException(params)) { JS_FreeValue(ctx, object); return params; }
    url->params = params;
    if (url_set(ctx, url, href) < 0) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
    return object;
}

/* The constructor's and the statics' arguments: url, and base unless it
 * is undefined. 1 parsed into `*out` (d_alloc'd, the caller frees it), 0
 * refused, -1 thrown. */
static int url_arguments(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv, char **out)
{
    *out = NULL;
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "URL needs an address");
        return -1;
    }
    DString input = {0}, base = {0};
    if (!d_string_text(dom, ctx, argv[0], &input, true)) return -1;
    bool has_base = argc > 1 && !JS_IsUndefined(argv[1]);
    int parsed = -1;
    if ((!has_base || d_string_text(dom, ctx, argv[1], &base, true)) && address_text(dom, ctx, &input, true) &&
        (!has_base || address_text(dom, ctx, &base, true))) {
        *out = d_alloc(dom, OS64_DOM_URL_MAX);
        if (*out == NULL) d_error(ctx, "QuotaExceededError", "URL quota exceeded");
        else parsed = url_parse(dom, ctx, &input, has_base ? &base : NULL, *out);
    }
    d_string_free(dom, &input);
    d_string_free(dom, &base);
    if (parsed < 0) {
        d_free(dom, *out);
        *out = NULL;
    }
    return parsed;
}

static JSValue url_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    char *href;
    int parsed = url_arguments(dom, ctx, argc, argv, &href);
    if (parsed <= 0) {
        d_free(dom, href);
        return parsed < 0 ? JS_EXCEPTION : JS_ThrowTypeError(ctx, "Invalid URL");
    }
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue made = JS_IsException(prototype) ? prototype : url_make(ctx, prototype, href);
    JS_FreeValue(ctx, prototype);
    d_free(dom, href);
    return made;
}

enum {
    U_HREF, U_ORIGIN, U_PROTOCOL, U_USERNAME, U_PASSWORD, U_HOST, U_HOSTNAME, U_PORT, U_PATHNAME,
    U_SEARCH, U_SEARCH_PARAMS, U_HASH, U_TO_STRING, U_CAN_PARSE, U_PARSE
};

/* An address whose spans [from, to) of `href` are `middle`, NUL-terminated
 * into `out`. False when it does not fit. */
static bool splice(const char *href, size_t length, size_t from, size_t to, const char *middle,
                   size_t middle_length, char *out, size_t cap)
{
    size_t need = from + middle_length + (length - to);
    if (need >= cap) return false;
    os64_memcpy(out, href, from);
    os64_memcpy(out + from, middle, middle_length);
    os64_memcpy(out + from + middle_length, href + to, length - to);
    out[need] = '\0';
    return true;
}

/* A setter's value with `#`, and in a path `?` too, escaped: within a
 * component they are its characters, not the next component's start.
 * Into `out`; false when it does not fit. */
static bool escape_delimiters(const DString *value, size_t from, bool path, char *out, size_t cap)
{
    size_t used = 0;
    for (size_t at = from; at < value->length; at++) {
        char c = value->data[at];
        bool escape = c == '#' || (path && c == '?');
        if (used + (escape ? 3 : 1) >= cap) return false;
        if (escape) {
            out[used++] = '%';
            out[used++] = c == '#' ? '2' : '3';
            out[used++] = c == '#' ? '3' : 'F';
        } else out[used++] = c;
    }
    out[used] = '\0';
    return true;
}

/* The first index in `text` holding any of `stops`, or its length. */
static size_t span_until(const DString *text, size_t from, const char *stops)
{
    size_t at = from;
    while (at < text->length && os64_strchr(stops, text->data[at]) == NULL) at++;
    return at;
}

/* A setter (URL § 6.1): the address with one component replaced, parsed
 * whole; kept when it parses, the old one kept when it does not. Only
 * href's refusal throws. -1 thrown. */
static int url_assign(os64_dom_t *dom, JSContext *ctx, DUrl *url, int which, JSValueConst value)
{
    DString text = {0};
    if (!d_string_text(dom, ctx, value, &text, true)) return -1;
    if (!address_text(dom, ctx, &text, which == U_HREF)) {
        d_string_free(dom, &text);
        return -1;
    }
    char *candidate = d_alloc(dom, OS64_DOM_URL_MAX * 3);
    if (candidate == NULL) {
        d_string_free(dom, &text);
        d_error(ctx, "QuotaExceededError", "URL quota exceeded");
        return -1;
    }
    char *parsed = candidate + OS64_DOM_URL_MAX, *piece = parsed + OS64_DOM_URL_MAX;
    const char *href = url->href;
    size_t length = url->length;
    Parts p;
    parts_of(href, length, &p);
    bool scheme_special = special(href, p.scheme_end);
    bool file = p.scheme_end == 4 && os64_memcmp(href, "file", 4) == 0;
    bool built = false;
    switch (which) {
    case U_HREF:
        built = os64_strcopy(candidate, OS64_DOM_URL_MAX, text.data) < OS64_DOM_URL_MAX;
        break;
    case U_PROTOCOL: {
        /* The scheme up to the first `:` (the scheme state's override): a
         * special scheme stays special and another stays not, an address
         * with a port cannot become file, and a file address with no host
         * keeps its scheme. */
        size_t end = span_until(&text, 0, ":");
        bool valid = end > 0 && end < OS64_URL_SCHEME_MAX &&
                     ((text.data[0] >= 'a' && text.data[0] <= 'z') || (text.data[0] >= 'A' && text.data[0] <= 'Z'));
        char scheme[OS64_URL_SCHEME_MAX];
        for (size_t i = 0; valid && i < end; i++) {
            char c = text.data[i];
            valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '+' || c == '-' || c == '.';
            scheme[i] = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
        }
        if (!valid) break;
        scheme[end] = '\0';
        if (special(scheme, end) != scheme_special) break;
        if (end == 4 && os64_memcmp(scheme, "file", 4) == 0 && p.port < p.port_end) break;
        if (file && p.host == p.host_end) break;
        built = splice(href, length, 0, p.scheme_end, scheme, end, candidate, OS64_DOM_URL_MAX);
        break;
    }
    case U_USERNAME: case U_PASSWORD:
        /* The grammar holds no userinfo (url.h): an address with one is not
         * one it parses, so no setter can make one. */
        break;
    case U_HOST: case U_HOSTNAME: {
        /* The host state's override: up to the path, the query or the
         * fragment. host takes a port with it, and keeps the address's
         * when it names none; hostname is refused one. */
        if (!p.authority) break;
        size_t end = span_until(&text, 0, scheme_special ? "/?#\\" : "/?#");
        size_t colon = span_until(&text, 0, ":");
        if (which == U_HOSTNAME && colon < end) break;
        if (colon == 0 || (end == 0 && scheme_special)) break;
        size_t to = which == U_HOST && colon < end ? p.port_end : p.host_end;
        built = splice(href, length, p.host, to, text.data, end, candidate, OS64_DOM_URL_MAX);
        break;
    }
    case U_PORT: {
        /* The port state's override: the digits it starts with, or none to
         * take the port away; a value that starts with anything else, or a
         * port past 65535, changes nothing. */
        if (!p.authority || p.host == p.host_end || file) break;
        size_t digits = 0;
        while (digits < text.length && text.data[digits] >= '0' && text.data[digits] <= '9') digits++;
        if (digits == 0 && text.length != 0) break;
        size_t zeros = 0;
        while (digits > 1 && zeros < digits - 1 && text.data[zeros] == '0') zeros++;
        if (digits - zeros > 5) break;
        char port[8] = ":";
        os64_memcpy(port + 1, text.data + zeros, digits - zeros);
        port[1 + digits - zeros] = '\0';
        built = splice(href, length, p.host_end, p.port_end, port, digits == 0 ? 0 : 1 + digits - zeros,
                       candidate, OS64_DOM_URL_MAX);
        break;
    }
    case U_PATHNAME: {
        /* An opaque path is not set; any other is the value, from a `/`. */
        if (!p.authority) break;
        piece[0] = '/';
        bool slash = text.length != 0 && (text.data[0] == '/' || (scheme_special && text.data[0] == '\\'));
        if (!escape_delimiters(&text, 0, true, piece + (slash ? 0 : 1), OS64_DOM_URL_MAX - 1)) break;
        built = splice(href, length, p.path, p.path_end, piece, os64_strlen(piece), candidate, OS64_DOM_URL_MAX);
        break;
    }
    case U_SEARCH: case U_HASH: {
        /* "" takes the query (the fragment) away; anything else, its
         * leading `?` (`#`) dropped, is the new one, empty included. */
        char mark = which == U_SEARCH ? '?' : '#';
        size_t from = text.length != 0 && text.data[0] == mark ? 1 : 0;
        size_t at = which == U_SEARCH ? p.query : p.fragment;
        size_t to = which == U_SEARCH ? p.query_end : p.fragment_end;
        if (text.length == 0) {
            built = splice(href, length, at, to, "", 0, candidate, OS64_DOM_URL_MAX);
            break;
        }
        piece[0] = mark;
        if (!escape_delimiters(&text, from, false, piece + 1, OS64_DOM_URL_MAX - 1)) break;
        built = splice(href, length, at, to, piece, os64_strlen(piece), candidate, OS64_DOM_URL_MAX);
        break;
    }
    default: break;
    }
    int result = 0;
    bool took = built && resolve(candidate, NULL, parsed, OS64_DOM_URL_MAX);
    if (which == U_HREF && !took) {
        JS_ThrowTypeError(ctx, "Invalid URL");
        result = -1;
    } else if (took && url_set(ctx, url, parsed) < 0) result = -1;
    d_free(dom, candidate);
    d_string_free(dom, &text);
    return result;
}

/* A component of the address, as its getter reads it. */
static JSValue url_component(JSContext *ctx, const DUrl *url, int which)
{
    const char *href = url->href;
    Parts p;
    parts_of(href, url->length, &p);
    switch (which) {
    case U_HREF: case U_TO_STRING: return JS_NewStringLen(ctx, href, url->length);
    case U_PROTOCOL: return JS_NewStringLen(ctx, href, p.scheme_end + 1);
    case U_USERNAME: case U_PASSWORD: return JS_NewString(ctx, "");
    case U_HOST: return JS_NewStringLen(ctx, href + p.host, p.port_end - p.host);
    case U_HOSTNAME: return JS_NewStringLen(ctx, href + p.host, p.host_end - p.host);
    case U_PORT: return JS_NewStringLen(ctx, href + p.port, p.port_end - p.port);
    case U_PATHNAME: return JS_NewStringLen(ctx, href + p.path, p.path_end - p.path);
    case U_SEARCH:
        return p.query_end - p.query > 1 ? JS_NewStringLen(ctx, href + p.query, p.query_end - p.query)
                                         : JS_NewString(ctx, "");
    case U_HASH:
        return p.fragment_end - p.fragment > 1 ? JS_NewStringLen(ctx, href + p.fragment, p.fragment_end - p.fragment)
                                               : JS_NewString(ctx, "");
    case U_ORIGIN: {
        /* A tuple origin for the special schemes but file; a blob: address
         * has its inner address's, when that is http or https. */
        char origin[OS64_URL_SCHEME_MAX + OS64_URL_HOST_MAX + 16];
        if (p.scheme_end == 4 && os64_memcmp(href, "blob", 4) == 0) {
            const char *inner = href + p.path;
            size_t inner_length = url->length - p.path;
            if (((inner_length >= 5 && os64_memcmp(inner, "http:", 5) == 0) ||
                 (inner_length >= 6 && os64_memcmp(inner, "https:", 6) == 0)) &&
                d_origin(inner, origin, sizeof(origin)))
                return JS_NewString(ctx, origin);
            return JS_NewString(ctx, "null");
        }
        return JS_NewString(ctx, d_origin(href, origin, sizeof(origin)) ? origin : "null");
    }
    default: return JS_UNDEFINED;
    }
}

static JSValue url_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                          JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    bool set = (magic & D_SET) != 0;
    magic &= ~D_SET;
    if (magic == U_CAN_PARSE || magic == U_PARSE) {
        char *href;
        int parsed = url_arguments(dom, ctx, argc, argv, &href);
        JSValue made = parsed < 0 ? JS_EXCEPTION : magic == U_CAN_PARSE ? JS_NewBool(ctx, parsed == 1) : JS_NULL;
        if (parsed == 1 && magic == U_PARSE) {
            /* URL.parse makes a URL as `new URL` does, whatever `this` is. */
            JSValue prototype = JS_GetClassProto(ctx, os64_js_class_id(&url_slot));
            made = url_make(ctx, prototype, href);
            JS_FreeValue(ctx, prototype);
        }
        d_free(dom, href);
        return made;
    }
    DUrl *url = url_of(ctx, self);
    if (url == NULL) return JS_EXCEPTION;
    if (set) {
        if (url_assign(dom, ctx, url, magic, argc > 0 ? argv[0] : JS_UNDEFINED) < 0) return JS_EXCEPTION;
        return JS_UNDEFINED;
    }
    if (magic == U_SEARCH_PARAMS) return JS_DupValue(ctx, url->params);
    return url_component(ctx, url, magic);
}

/* ── URLSearchParams ─────────────────────────────────────────────────── */

/* Each value an iterable yields, by ECMAScript's iteration (what WebIDL's
 * sequence conversion runs) through `method`, its @@iterator, handed to
 * `each`, which owns it and answers -1 to stop with an exception pending.
 * -1 thrown. */
static int each_value(JSContext *ctx, JSValueConst iterable, JSValueConst method,
                      int (*each)(void *opaque, JSValue value), void *opaque)
{
    JSValue iterator = JS_Call(ctx, method, iterable, 0, NULL);
    if (JS_IsException(iterator)) return -1;
    JSValue next = JS_GetPropertyStr(ctx, iterator, "next");
    int result = JS_IsException(next) ? -1 : 0;
    while (result == 0) {
        JSValue step = JS_Call(ctx, next, iterator, 0, NULL);
        if (JS_IsException(step)) { result = -1; break; }
        if (!JS_IsObject(step)) {
            JS_FreeValue(ctx, step);
            JS_ThrowTypeError(ctx, "An iterator's result must be an object");
            result = -1;
            break;
        }
        JSValue done = JS_GetPropertyStr(ctx, step, "done");
        int finished = JS_IsException(done) ? -1 : JS_ToBool(ctx, done);
        JS_FreeValue(ctx, done);
        if (finished != 0) { JS_FreeValue(ctx, step); result = finished < 0 ? -1 : 1; break; }
        JSValue value = JS_GetPropertyStr(ctx, step, "value");
        JS_FreeValue(ctx, step);
        if (JS_IsException(value) || each(opaque, value) < 0) result = -1;
    }
    JS_FreeValue(ctx, next);
    JS_FreeValue(ctx, iterator);
    return result < 0 ? -1 : 0;
}

/* An object's @@iterator method, read with the Symbol.iterator the
 * binding took before any page ran: undefined when it has none. */
static JSValue iterator_method(os64_dom_t *dom, JSContext *ctx, JSValueConst object)
{
    JSAtom atom = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue method = JS_GetProperty(ctx, object, atom);
    JS_FreeAtom(ctx, atom);
    if (!JS_IsException(method) && !JS_IsUndefined(method) && !JS_IsNull(method) && !JS_IsFunction(ctx, method)) {
        JS_FreeValue(ctx, method);
        return JS_ThrowTypeError(ctx, "The object's iterator is not a function");
    }
    return method;
}

/* One inner sequence: its first two items, and how many it had. */
typedef struct {
    JSContext *ctx;
    JSValue items[2];
    uint32_t count;
} Pair;

static int pair_item(void *opaque, JSValue value)
{
    Pair *pair = opaque;
    if (pair->count < 2) pair->items[pair->count] = value;
    else JS_FreeValue(pair->ctx, value);
    if (pair->count < UINT32_MAX) pair->count++;
    return 0;
}

typedef struct {
    os64_dom_t *dom;
    JSContext *ctx;
    DParams *params;
} Pairs;

/* sequence<sequence<USVString>>'s outer item: an inner sequence of exactly
 * two strings, which becomes a pair. */
static int pairs_item(void *opaque, JSValue value)
{
    Pairs *pairs = opaque;
    JSContext *ctx = pairs->ctx;
    os64_dom_t *dom = pairs->dom;
    Pair pair = {ctx, {JS_UNDEFINED, JS_UNDEFINED}, 0};
    JSValue method = JS_IsObject(value) ? iterator_method(dom, ctx, value) : JS_UNDEFINED;
    int result = -1;
    if (!JS_IsException(method) && (JS_IsUndefined(method) || JS_IsNull(method)))
        JS_ThrowTypeError(ctx, "Each pair must be a sequence");
    else if (!JS_IsException(method) && each_value(ctx, value, method, pair_item, &pair) == 0) {
        if (pair.count != 2) JS_ThrowTypeError(ctx, "Each pair must have exactly two items");
        else {
            DString text[2] = {{0}, {0}};
            if (d_string_text(dom, ctx, pair.items[0], &text[0], true) &&
                d_string_text(dom, ctx, pair.items[1], &text[1], true))
                result = pair_add(ctx, pairs->params, text[0].data, text[0].length, text[1].data, text[1].length,
                                  false);
            d_string_free(dom, &text[0]);
            d_string_free(dom, &text[1]);
        }
    }
    JS_FreeValue(ctx, pair.items[0]);
    JS_FreeValue(ctx, pair.items[1]);
    JS_FreeValue(ctx, method);
    JS_FreeValue(ctx, value);
    return result;
}

/* WebIDL's record<USVString, USVString>: the object's own enumerable
 * string keys, in its own order, each with its value. -1 thrown. */
static int params_from_record(os64_dom_t *dom, JSContext *ctx, DParams *params, JSValueConst record)
{
    JSPropertyEnum *keys;
    uint32_t count;
    if (JS_GetOwnPropertyNames(ctx, &keys, &count, record, JS_GPN_STRING_MASK) < 0) return -1;
    int result = 0;
    for (uint32_t i = 0; result == 0 && i < count; i++) {
        JSPropertyDescriptor desc;
        int own = JS_GetOwnProperty(ctx, &desc, record, keys[i].atom);
        if (own < 0) { result = -1; break; }
        if (own == 0) continue;
        bool enumerable = (desc.flags & JS_PROP_ENUMERABLE) != 0;
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        if (!enumerable) continue;
        JSValue key = JS_AtomToValue(ctx, keys[i].atom);
        JSValue value = JS_IsException(key) ? JS_EXCEPTION : JS_GetProperty(ctx, record, keys[i].atom);
        DString text[2] = {{0}, {0}};
        bool ok = !JS_IsException(value) && d_string_text(dom, ctx, key, &text[0], true) &&
                  d_string_text(dom, ctx, value, &text[1], true);
        JS_FreeValue(ctx, key);
        JS_FreeValue(ctx, value);
        /* Two keys may be one string once each lone surrogate is U+FFFD:
         * a record keeps the first key's place and the last value. Only a
         * key with U+FFFD in it can meet another so. */
        bool replaced = false;
        for (size_t at = 0; ok && at + 2 < text[0].length && !replaced; at++)
            replaced = (unsigned char)text[0].data[at] == 0xef && (unsigned char)text[0].data[at + 1] == 0xbf &&
                       (unsigned char)text[0].data[at + 2] == 0xbd;
        DPair *same_key = NULL;
        for (size_t at = 0; ok && replaced && at < params->count && same_key == NULL; at++)
            if (params->pairs[at].name_length == text[0].length &&
                os64_memcmp(params->pairs[at].name, text[0].data, text[0].length) == 0)
                same_key = &params->pairs[at];
        if (ok && same_key != NULL) {
            char *copy = js_malloc(ctx, text[1].length + 1);
            if (copy == NULL) ok = false;
            else {
                os64_memcpy(copy, text[1].data, text[1].length);
                copy[text[1].length] = '\0';
                js_free(ctx, same_key->value);
                same_key->value = copy;
                same_key->value_length = text[1].length;
            }
        } else if (ok && pair_add(ctx, params, text[0].data, text[0].length, text[1].data, text[1].length, false) < 0)
            ok = false;
        d_string_free(dom, &text[0]);
        d_string_free(dom, &text[1]);
        if (!ok) result = -1;
    }
    JS_FreePropertyEnum(ctx, keys, count);
    return result;
}

static JSValue params_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = params_new(ctx, prototype, JS_UNDEFINED);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DParams *params = JS_GetOpaque(object, os64_js_class_id(&params_slot));
    JSValueConst init = argc > 0 ? argv[0] : JS_UNDEFINED;
    int result = 0;
    if (JS_IsObject(init)) {
        /* (sequence<sequence<USVString>> or record<USVString, USVString>):
         * an object with an iterator is the sequence. */
        JSValue method = iterator_method(dom, ctx, init);
        Pairs pairs = {dom, ctx, params};
        if (JS_IsException(method)) result = -1;
        else if (JS_IsUndefined(method) || JS_IsNull(method)) result = params_from_record(dom, ctx, params, init);
        else result = each_value(ctx, init, method, pairs_item, &pairs);
        JS_FreeValue(ctx, method);
    } else if (!JS_IsUndefined(init)) {
        DString text = {0};
        if (!d_string_text(dom, ctx, init, &text, true)) result = -1;
        else {
            size_t from = text.length != 0 && text.data[0] == '?' ? 1 : 0;
            result = form_parse(ctx, params, text.data + from, text.length - from);
            d_string_free(dom, &text);
        }
    }
    if (result < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static bool same(const DPair *pair, bool value, const DString *text)
{
    const char *bytes = value ? pair->value : pair->name;
    size_t length = value ? pair->value_length : pair->name_length;
    return length == text->length && os64_memcmp(bytes, text->data, length) == 0;
}

/* The code units of UTF-8 text one at a time, as UTF-16 has them: what
 * sort() compares by. */
static uint32_t unit_at(const char *text, size_t length, size_t *at, uint32_t *low)
{
    if (*low != 0) { uint32_t unit = *low; *low = 0; return unit; }
    unsigned char c = (unsigned char)text[*at];
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (*at + n > length) n = length - *at;
    uint32_t point = n == 1 ? c : n == 2 ? c & 0x1fu : n == 3 ? c & 0x0fu : c & 0x07u;
    for (size_t i = 1; i < n; i++) point = point << 6 | ((unsigned char)text[*at + i] & 0x3fu);
    *at += n;
    if (point < 0x10000) return point;
    point -= 0x10000;
    *low = 0xdc00 + (point & 0x3ff);
    return 0xd800 + (point >> 10);
}

static int name_order(const DPair *a, const DPair *b)
{
    size_t i = 0, j = 0;
    uint32_t low_a = 0, low_b = 0;
    while ((i < a->name_length || low_a != 0) && (j < b->name_length || low_b != 0)) {
        uint32_t x = unit_at(a->name, a->name_length, &i, &low_a);
        uint32_t y = unit_at(b->name, b->name_length, &j, &low_b);
        if (x != y) return x < y ? -1 : 1;
    }
    bool more_a = i < a->name_length || low_a != 0, more_b = j < b->name_length || low_b != 0;
    return more_a == more_b ? 0 : more_a ? 1 : -1;
}

enum {
    Q_APPEND, Q_DELETE, Q_GET, Q_GET_ALL, Q_HAS, Q_SET, Q_SORT, Q_TO_STRING, Q_SIZE, Q_FOR_EACH,
    Q_ENTRIES, Q_KEYS, Q_VALUES, Q_NEXT
};

static JSValue pair_value(JSContext *ctx, const DPair *pair, int kind)
{
    if (kind == I_KEYS) return JS_NewStringLen(ctx, pair->name, pair->name_length);
    if (kind == I_VALUES) return JS_NewStringLen(ctx, pair->value, pair->value_length);
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    if (JS_SetPropertyUint32(ctx, array, 0, JS_NewStringLen(ctx, pair->name, pair->name_length)) < 0 ||
        JS_SetPropertyUint32(ctx, array, 1, JS_NewStringLen(ctx, pair->value, pair->value_length)) < 0) {
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

static JSValue iterator_result(JSContext *ctx, JSValue value, bool done)
{
    if (JS_IsException(value)) return value;
    JSValue result = JS_NewObject(ctx);
    if (JS_IsException(result) ||
        JS_DefinePropertyValueStr(ctx, result, "value", value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, result, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        if (JS_IsException(result)) JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

static JSValue params_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                             JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == Q_NEXT) {
        DIterator *it = JS_GetOpaque(self, os64_js_class_id(&iterator_slot));
        if (it == NULL) return JS_ThrowTypeError(ctx, "Expected a URLSearchParams Iterator");
        DParams *params = JS_GetOpaque(it->params, os64_js_class_id(&params_slot));
        if (params == NULL || it->index >= params->count) return iterator_result(ctx, JS_UNDEFINED, true);
        return iterator_result(ctx, pair_value(ctx, &params->pairs[it->index++], it->kind), false);
    }
    DParams *params = params_of(ctx, self);
    if (params == NULL) return JS_EXCEPTION;
    static const int kNeeds[] = {2, 1, 1, 1, 1, 2, 0, 0, 0, 1, 0, 0, 0};
    if (argc < kNeeds[magic])
        return JS_ThrowTypeError(ctx, "URLSearchParams's method needs %d argument%s", kNeeds[magic],
                                 kNeeds[magic] == 1 ? "" : "s");
    switch (magic) {
    case Q_SIZE: return JS_NewInt64(ctx, (int64_t)params->count);
    case Q_TO_STRING: {
        size_t length;
        char *text = form_serialize(ctx, params, &length);
        if (text == NULL) return JS_EXCEPTION;
        JSValue result = JS_NewStringLen(ctx, text, length);
        js_free(ctx, text);
        return result;
    }
    case Q_SORT: {
        /* A stable sort by name, in UTF-16 code units: a merge sort, since
         * a page may append as many pairs as it likes. */
        if (params->count > 1) {
            DPair *scratch = js_malloc(ctx, params->count * sizeof(*scratch));
            if (scratch == NULL) return JS_EXCEPTION;
            DPair *from = params->pairs, *to = scratch;
            for (size_t width = 1; width < params->count; width *= 2) {
                for (size_t lo = 0; lo < params->count; lo += 2 * width) {
                    size_t mid = lo + width < params->count ? lo + width : params->count;
                    size_t hi = mid + width < params->count ? mid + width : params->count;
                    size_t a = lo, b = mid, at = lo;
                    while (a < mid && b < hi) to[at++] = name_order(&from[b], &from[a]) < 0 ? from[b++] : from[a++];
                    while (a < mid) to[at++] = from[a++];
                    while (b < hi) to[at++] = from[b++];
                }
                DPair *swap = from;
                from = to;
                to = swap;
            }
            if (from != params->pairs) os64_memcpy(params->pairs, from, params->count * sizeof(*from));
            js_free(ctx, scratch);
        }
        return params_update(ctx, params) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    case Q_FOR_EACH: {
        if (!JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "forEach needs a function");
        JSValueConst that = argc > 1 ? argv[1] : JS_UNDEFINED;
        for (size_t i = 0; i < params->count; i++) {
            JSValue args[3] = {JS_NewStringLen(ctx, params->pairs[i].value, params->pairs[i].value_length),
                               JS_NewStringLen(ctx, params->pairs[i].name, params->pairs[i].name_length),
                               JS_DupValue(ctx, self)};
            JSValue done = JS_IsException(args[0]) || JS_IsException(args[1]) ? JS_EXCEPTION
                         : JS_Call(ctx, argv[0], that, 3, (JSValueConst *)args);
            for (int k = 0; k < 3; k++) JS_FreeValue(ctx, args[k]);
            if (JS_IsException(done)) return done;
            JS_FreeValue(ctx, done);
            /* The callback may have changed the list under the loop. */
            params = JS_GetOpaque(self, os64_js_class_id(&params_slot));
            if (params == NULL) break;
        }
        return JS_UNDEFINED;
    }
    case Q_ENTRIES: case Q_KEYS: case Q_VALUES: {
        JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&iterator_slot));
        if (JS_IsException(object)) return object;
        DIterator *it = js_mallocz(ctx, sizeof(*it));
        if (it == NULL) { JS_FreeValue(ctx, object); return JS_EXCEPTION; }
        *it = (DIterator){JS_DupValue(ctx, self), 0, magic == Q_ENTRIES ? I_ENTRIES : magic == Q_KEYS ? I_KEYS : I_VALUES};
        JS_SetOpaque(object, it);
        return object;
    }
    default: break;
    }
    DString name = {0}, value = {0};
    if (!d_string_text(dom, ctx, argv[0], &name, true)) return JS_EXCEPTION;
    /* delete's and has's value is optional, and undefined is not given. */
    bool valued = argc > 1 && (magic == Q_APPEND || magic == Q_SET || !JS_IsUndefined(argv[1])) &&
                  magic != Q_GET && magic != Q_GET_ALL;
    if (valued && !d_string_text(dom, ctx, argv[1], &value, true)) {
        d_string_free(dom, &name);
        return JS_EXCEPTION;
    }
    JSValue result = JS_UNDEFINED;
    switch (magic) {
    case Q_APPEND:
        if (pair_add(ctx, params, name.data, name.length, value.data, value.length, false) < 0 ||
            params_update(ctx, params) < 0)
            result = JS_EXCEPTION;
        break;
    case Q_DELETE: {
        size_t kept = 0;
        for (size_t i = 0; i < params->count; i++) {
            DPair *pair = &params->pairs[i];
            if (same(pair, false, &name) && (!valued || same(pair, true, &value))) {
                js_free(ctx, pair->name);
                js_free(ctx, pair->value);
            } else params->pairs[kept++] = *pair;
        }
        params->count = kept;
        if (params_update(ctx, params) < 0) result = JS_EXCEPTION;
        break;
    }
    case Q_GET: case Q_HAS: {
        result = magic == Q_GET ? JS_NULL : JS_FALSE;
        for (size_t i = 0; i < params->count; i++)
            if (same(&params->pairs[i], false, &name) && (magic == Q_GET || !valued || same(&params->pairs[i], true, &value))) {
                result = magic == Q_GET ? JS_NewStringLen(ctx, params->pairs[i].value, params->pairs[i].value_length)
                                        : JS_TRUE;
                break;
            }
        break;
    }
    case Q_GET_ALL: {
        result = JS_NewArray(ctx);
        uint32_t used = 0;
        for (size_t i = 0; !JS_IsException(result) && i < params->count; i++)
            if (same(&params->pairs[i], false, &name) &&
                JS_SetPropertyUint32(ctx, result, used++,
                                     JS_NewStringLen(ctx, params->pairs[i].value, params->pairs[i].value_length)) < 0) {
                JS_FreeValue(ctx, result);
                result = JS_EXCEPTION;
            }
        break;
    }
    case Q_SET: {
        /* The first pair by that name takes the value and keeps its place;
         * the rest go; none, and it is appended. */
        char *copy = js_malloc(ctx, value.length + 1);
        if (copy == NULL) { result = JS_EXCEPTION; break; }
        os64_memcpy(copy, value.data, value.length);
        copy[value.length] = '\0';
        bool placed = false;
        size_t kept = 0;
        for (size_t i = 0; i < params->count; i++) {
            DPair *pair = &params->pairs[i];
            if (!same(pair, false, &name)) { params->pairs[kept++] = *pair; continue; }
            if (placed) { js_free(ctx, pair->name); js_free(ctx, pair->value); continue; }
            js_free(ctx, pair->value);
            pair->value = copy;
            pair->value_length = value.length;
            params->pairs[kept++] = *pair;
            placed = true;
        }
        params->count = kept;
        if (!placed && pair_add(ctx, params, name.data, name.length, copy, value.length, false) < 0)
            result = JS_EXCEPTION;
        if (!placed) js_free(ctx, copy);
        if (!JS_IsException(result) && params_update(ctx, params) < 0) result = JS_EXCEPTION;
        break;
    }
    default: break;
    }
    d_string_free(dom, &name);
    d_string_free(dom, &value);
    return result;
}

/* ── The row ─────────────────────────────────────────────────────────── */

static DValue *prototype_made(os64_dom_t *dom, JSContext *ctx, JSClassID id, const JSClassDef *definition,
                              JSValueConst parent)
{
    if (!JS_IsRegisteredClass(dom->engine, id) && JS_NewClass(dom->engine, id, definition) < 0) return NULL;
    JSValue prototype = JS_IsObject(parent) ? JS_NewObjectProto(ctx, parent) : JS_NewObject(ctx);
    DValue *held = d_retain(dom, ctx, prototype);
    if (held != NULL) JS_SetClassProto(ctx, id, JS_DupValue(ctx, prototype));
    return held;
}

int d_url_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef url_class = {.class_name = "URL", .finalizer = url_finalizer, .gc_mark = url_mark};
    static const JSClassDef params_class = {.class_name = "URLSearchParams", .finalizer = params_finalizer,
                                            .gc_mark = params_mark};
    static const JSClassDef iterator_class = {.class_name = "URLSearchParams Iterator",
                                              .finalizer = iterator_finalizer, .gc_mark = iterator_mark};
    /* %IteratorPrototype%, which every iterator WebIDL makes inherits:
     * Iterator.prototype, read before any page can replace Iterator. Read,
     * not reached by calling an Array's iterator: a call at creation can
     * run the engine's interrupt check, which makes an allocation refused
     * earlier in the creation the runtime's sticky failure. */
    JSValue iterator_constructor = JS_GetPropertyStr(ctx, global, "Iterator");
    JSValue iterator_prototype = JS_IsException(iterator_constructor) ? JS_EXCEPTION
                               : JS_GetPropertyStr(ctx, iterator_constructor, "prototype");
    JS_FreeValue(ctx, iterator_constructor);
    if (JS_IsException(iterator_prototype)) return -1;
    if (!JS_IsObject(iterator_prototype)) {
        JS_FreeValue(ctx, iterator_prototype);
        JS_ThrowTypeError(ctx, "The engine has no Iterator.prototype");
        return -1;
    }
    DValue *url = prototype_made(dom, ctx, os64_js_class_id(&url_slot), &url_class, JS_UNDEFINED);
    DValue *params = url != NULL ? prototype_made(dom, ctx, os64_js_class_id(&params_slot), &params_class, JS_UNDEFINED) : NULL;
    DValue *iterator = params != NULL ? prototype_made(dom, ctx, os64_js_class_id(&iterator_slot), &iterator_class,
                                                       iterator_prototype) : NULL;
    JS_FreeValue(ctx, iterator_prototype);
    if (iterator == NULL) return -1;
    static const struct { const char *name; int magic; bool writable; } kUrl[] = {
        {"href", U_HREF, true}, {"origin", U_ORIGIN, false}, {"protocol", U_PROTOCOL, true},
        {"username", U_USERNAME, true}, {"password", U_PASSWORD, true}, {"host", U_HOST, true},
        {"hostname", U_HOSTNAME, true}, {"port", U_PORT, true}, {"pathname", U_PATHNAME, true},
        {"search", U_SEARCH, true}, {"searchParams", U_SEARCH_PARAMS, false}, {"hash", U_HASH, true},
    };
    for (size_t i = 0; i < sizeof(kUrl) / sizeof(kUrl[0]); i++)
        if (d_accessor(dom, ctx, url->value, kUrl[i].name, url_member, kUrl[i].magic, kUrl[i].writable) < 0)
            return -1;
    if (d_method(dom, ctx, url->value, "toString", url_member, 0, U_TO_STRING) < 0 ||
        d_method(dom, ctx, url->value, "toJSON", url_member, 0, U_TO_STRING) < 0)
        return -1;
    static const struct { const char *name; int magic, argc; } kParams[] = {
        {"append", Q_APPEND, 2}, {"delete", Q_DELETE, 1}, {"get", Q_GET, 1}, {"getAll", Q_GET_ALL, 1},
        {"has", Q_HAS, 1}, {"set", Q_SET, 2}, {"sort", Q_SORT, 0}, {"toString", Q_TO_STRING, 0},
        {"forEach", Q_FOR_EACH, 1}, {"entries", Q_ENTRIES, 0}, {"keys", Q_KEYS, 0}, {"values", Q_VALUES, 0},
    };
    for (size_t i = 0; i < sizeof(kParams) / sizeof(kParams[0]); i++)
        if (d_method(dom, ctx, params->value, kParams[i].name, params_member, kParams[i].argc, kParams[i].magic) < 0)
            return -1;
    if (d_accessor(dom, ctx, params->value, "size", params_member, Q_SIZE, false) < 0 ||
        d_method(dom, ctx, iterator->value, "next", params_member, 0, Q_NEXT) < 0)
        return -1;
    /* [Symbol.iterator] is entries, the same function (WebIDL § 3.7.9.1);
     * the iterator's prototype says what it is. The symbol is kept, for
     * the constructor's sequence. */
    JSValue global_symbol = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue iterator_key = JS_IsException(global_symbol) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, global_symbol, "iterator");
    JSValue tag_key = JS_IsException(iterator_key) ? JS_EXCEPTION : JS_GetPropertyStr(ctx, global_symbol, "toStringTag");
    JS_FreeValue(ctx, global_symbol);
    if (JS_IsException(tag_key) || !JS_IsSymbol(iterator_key) || !JS_IsSymbol(tag_key)) {
        JS_FreeValue(ctx, iterator_key);
        JS_FreeValue(ctx, tag_key);
        return -1;
    }
    dom->iterator_symbol = d_retain(dom, ctx, JS_DupValue(ctx, iterator_key));
    JSAtom iterator_atom = dom->iterator_symbol == NULL ? JS_ATOM_NULL : JS_ValueToAtom(ctx, iterator_key);
    JSAtom tag_atom = iterator_atom == JS_ATOM_NULL ? JS_ATOM_NULL : JS_ValueToAtom(ctx, tag_key);
    JS_FreeValue(ctx, iterator_key);
    JS_FreeValue(ctx, tag_key);
    JSValue entries = tag_atom == JS_ATOM_NULL ? JS_EXCEPTION : JS_GetPropertyStr(ctx, params->value, "entries");
    int linked = JS_IsException(entries) ? -1
        : JS_DefinePropertyValue(ctx, params->value, iterator_atom, entries, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    if (linked >= 0)
        linked = JS_DefinePropertyValue(ctx, iterator->value, tag_atom, JS_NewString(ctx, "URLSearchParams Iterator"),
                                        JS_PROP_CONFIGURABLE);
    if (iterator_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, iterator_atom);
    if (tag_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, tag_atom);
    if (linked < 0) return -1;
    JSValue url_interface = d_interface_new(ctx, "URL", 1, url_constructor, url->value, JS_UNDEFINED);
    if (JS_IsException(url_interface)) return -1;
    if (d_method(dom, ctx, url_interface, "canParse", url_member, 1, U_CAN_PARSE) < 0 ||
        d_method(dom, ctx, url_interface, "parse", url_member, 1, U_PARSE) < 0) {
        JS_FreeValue(ctx, url_interface);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, global, "URL", url_interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    JSValue params_interface = d_interface_new(ctx, "URLSearchParams", 0, params_constructor, params->value,
                                               JS_UNDEFINED);
    if (JS_IsException(params_interface) ||
        JS_DefinePropertyValueStr(ctx, global, "URLSearchParams", params_interface,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return 0;
}
