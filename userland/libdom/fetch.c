#include "internal.h"
#include "os64/fmt.h"

/* fetch() and its objects (the Fetch Standard, § 5): Headers, Request,
 * Response, the body they share and a minimal stream over it. The request
 * goes to the HOST (dom.h § A SCRIPT'S REQUESTS), which makes every judgement
 * of origins and CORS (libway's origin.c) and hands back a filtered
 * response; what this file judges is what a page may write into a request:
 * the header names and values and the guards, from the same origin.c, so a
 * forbidden header is one list in the tree.
 *
 * Every object's state is ENGINE memory, freed with it: a header list is a
 * C array, never a container a page can reach, read by index and looked up
 * again after anything that can run page code. Its sort-and-combine, which
 * iteration reads at every step, is kept until the list changes, so a walk
 * is n log n and not quadratic. A body is the bytes whole: the host reads a
 * response to its end before it answers, so a stream hands over the body
 * as one chunk. The requests out are the registry's fourth list. */

static JSClassID headers_slot, headers_iterator_slot, request_slot, response_slot, stream_slot, reader_slot;

enum { G_NONE, G_REQUEST, G_REQUEST_NO_CORS, G_RESPONSE, G_IMMUTABLE };

typedef struct {
    char *name, *value;         /* bytes, js_malloc'd, NUL-terminated */
    size_t name_length, value_length;
} DHeader;

typedef struct {
    DHeader *list;
    size_t count, cap;
    int guard;
    /* § 2.2.2 "sort and combine", kept until the list changes. */
    DHeader *sorted;
    size_t sorted_count;
    bool sorted_valid;
} DHeaders;

typedef struct {
    JSValue headers;
    size_t index;
    int kind;
} DHeadersIterator;

typedef struct {
    uint8_t *bytes;
    size_t length;
    bool has;                   /* a body, not null */
    bool used;                  /* disturbed: read, or read from */
    bool consumed;              /* read whole by a body method, whose reader keeps the stream locked */
    JSValue stream;             /* made at the first ask, [SameObject] */
    JSValue aborted;            /* the abort reason a fetch's signal gave it, or undefined */
} DBody;

typedef struct {
    DBody body;
    char *url;
    char method[WAY_METHOD_MAX];
    JSValue headers;
    way_mode_t mode;
    way_credentials_t credentials;
    way_redirect_t redirect;
    way_policy_t policy;
    bool policy_given;          /* referrerPolicy is not "" */
    int cache;
    char *referrer;             /* "about:client", "" for none, or an address */
    char *integrity;
    bool keepalive;
    int priority;
    JSValue signal;
} DReq;

typedef struct {
    DBody body;
    way_type_t type;
    int32_t status;
    char *status_text;
    char *url;                  /* "" for none */
    bool redirected;
    JSValue headers;
} DResp;

enum { ST_READABLE, ST_CLOSED, ST_ERRORED };

typedef struct {
    JSValue owner;              /* the Request or Response whose body this is, or undefined */
    uint8_t *bytes;             /* without an owner, its own */
    size_t length;
    int state;
    bool locked, delivered;
    JSValue reader;
    JSValue error;
} DStream;

typedef struct {
    JSValue stream;             /* undefined once released */
    JSValue closed, closed_resolve, closed_reject;
} DReader;

uint64_t d_request_steps;

/* ── The requests out ────────────────────────────────────────────────── */

void os64_dom_set_network(os64_dom_t *dom, const os64_dom_network_t *network)
{
    if (dom == NULL) return;
    if (network != NULL) dom->network = *network;
    else os64_memset(&dom->network, 0, sizeof(dom->network));
}

size_t os64_dom_fetches_pending(const os64_dom_t *dom)
{
    size_t n = 0;
    for (const DRequest *r = dom != NULL ? dom->requests : NULL; r != NULL; r = r->next) n++;
    return n;
}

bool os64_dom_fetch_waiting(const os64_dom_t *dom, uint32_t token)
{
    for (const DRequest *r = dom != NULL && !dom->closed ? dom->requests : NULL; r != NULL; r = r->next)
        if (r->token == token) return true;
    return false;
}

DRequest *d_request_find(os64_dom_t *dom, uint32_t token)
{
    for (DRequest *r = dom->requests; r != NULL; r = r->next)
        if (r->token == token) return r;
    return NULL;
}

DRequest *d_request_start(os64_dom_t *dom, JSContext *ctx, const os64_dom_request_t *request, DAnswer answer)
{
    if (dom->network.start == NULL) {
        JS_ThrowTypeError(ctx, "This page cannot fetch");
        return NULL;
    }
    DRequest *r = d_alloc(dom, sizeof(*r));
    if (r == NULL) {
        d_error(ctx, "QuotaExceededError", "DOM request quota exceeded");
        return NULL;
    }
    if (++dom->request_token == 0) dom->request_token = 1;
    r->token = dom->request_token;
    r->answer = answer;
    for (int i = 0; i < 3; i++) r->held[i] = JS_UNDEFINED;
    r->id = dom->network.start(dom->network.opaque, request, r->token);
    if (r->id == 0) {
        d_free(dom, r);
        JS_ThrowTypeError(ctx, "Failed to fetch");
        return NULL;
    }
    r->next = dom->requests;
    dom->requests = r;
    return r;
}

/* A value the request keeps until it ends (drain releases it). */
static void request_hold(os64_dom_t *dom, DRequest *r, int slot, JSValue value)
{
    if (!JS_IsUndefined(r->held[slot])) {
        JS_FreeValueRT(dom->engine, r->held[slot]);
        dom->requests_held--;
    }
    r->held[slot] = value;
    if (!JS_IsUndefined(value)) dom->requests_held++;
}

void d_request_end(os64_dom_t *dom, DRequest *request, bool cancel)
{
    for (DRequest **link = &dom->requests; *link != NULL; link = &(*link)->next)
        if (*link == request) {
            *link = request->next;
            break;
        }
    if (cancel && dom->network.cancel != NULL) dom->network.cancel(dom->network.opaque, request->id);
    for (int i = 0; i < 3; i++) request_hold(dom, request, i, JS_UNDEFINED);
    d_free(dom, request);
}

void d_fetch_drain(os64_dom_t *dom)
{
    while (dom->requests != NULL) d_request_end(dom, dom->requests, true);
}

/* After the engine is gone: drain has emptied the list, so this frees
 * records only, and touches no engine value. */
void d_fetch_free(os64_dom_t *dom)
{
    while (dom->requests != NULL) {
        DRequest *next = dom->requests->next;
        d_free(dom, dom->requests);
        dom->requests = next;
    }
}

/* ── Strings and bytes ───────────────────────────────────────────────── */

/* WebIDL's ByteString: a string whose every code unit is at most 0xFF, as
 * those bytes (engine memory). False with a TypeError for any other. */
bool d_byte_string(JSContext *ctx, JSValueConst value, char **out, size_t *length)
{
    size_t n;
    const char *text = JS_ToCStringLen(ctx, &n, value);
    if (text == NULL) return false;
    char *bytes = js_malloc(ctx, n + 1);
    if (bytes == NULL) {
        JS_FreeCString(ctx, text);
        return false;
    }
    size_t at = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            bytes[at++] = (char)c;
            i++;
        } else if ((c & 0xe0) == 0xc0 && i + 1 < n && c <= 0xc3) {
            bytes[at++] = (char)(((c & 0x03) << 6) | ((unsigned char)text[i + 1] & 0x3f));
            i += 2;
        } else {
            js_free(ctx, bytes);
            JS_FreeCString(ctx, text);
            JS_ThrowTypeError(ctx, "A header holds a character a byte cannot");
            return false;
        }
    }
    bytes[at] = '\0';
    JS_FreeCString(ctx, text);
    *out = bytes;
    *length = at;
    return true;
}

/* Bytes back as a string, each byte its own code point (isomorphic
 * decode). */
JSValue d_byte_text(JSContext *ctx, const char *bytes, size_t length)
{
    size_t extra = 0;
    for (size_t i = 0; i < length; i++) extra += (unsigned char)bytes[i] >= 0x80;
    if (extra == 0) return JS_NewStringLen(ctx, bytes, length);
    char *utf8 = js_malloc(ctx, length + extra + 1);
    if (utf8 == NULL) return JS_EXCEPTION;
    size_t at = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c < 0x80) utf8[at++] = (char)c;
        else {
            utf8[at++] = (char)(0xc0 | (c >> 6));
            utf8[at++] = (char)(0x80 | (c & 0x3f));
        }
    }
    JSValue text = JS_NewStringLen(ctx, utf8, at);
    js_free(ctx, utf8);
    return text;
}

static char *engine_dup(JSContext *ctx, const char *s, size_t n)
{
    char *copy = js_malloc(ctx, n + 1);
    if (copy == NULL) return NULL;
    os64_memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool d_same_nocase(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) return false;
    for (size_t i = 0; i < alen; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

static int order_nocase(const char *a, size_t alen, const char *b, size_t blen)
{
    for (size_t i = 0; i < alen && i < blen; i++) {
        unsigned char x = (unsigned char)lower(a[i]), y = (unsigned char)lower(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return alen == blen ? 0 : alen < blen ? -1 : 1;
}

/* ── Headers ─────────────────────────────────────────────────────────── */

static void headers_sorted_clear(JSRuntime *rt, DHeaders *h)
{
    for (size_t i = 0; i < h->sorted_count; i++) {
        js_free_rt(rt, h->sorted[i].name);
        js_free_rt(rt, h->sorted[i].value);
    }
    js_free_rt(rt, h->sorted);
    h->sorted = NULL;
    h->sorted_count = 0;
    h->sorted_valid = false;
}

static void headers_finalizer(JSRuntime *rt, JSValue value)
{
    DHeaders *h = JS_GetOpaque(value, os64_js_class_id(&headers_slot));
    if (h == NULL) return;
    for (size_t i = 0; i < h->count; i++) {
        js_free_rt(rt, h->list[i].name);
        js_free_rt(rt, h->list[i].value);
    }
    js_free_rt(rt, h->list);
    headers_sorted_clear(rt, h);
    js_free_rt(rt, h);
}

static void headers_iterator_finalizer(JSRuntime *rt, JSValue value)
{
    DHeadersIterator *it = JS_GetOpaque(value, os64_js_class_id(&headers_iterator_slot));
    if (it == NULL) return;
    JS_FreeValueRT(rt, it->headers);
    js_free_rt(rt, it);
}

static void headers_iterator_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DHeadersIterator *it = JS_GetOpaque(value, os64_js_class_id(&headers_iterator_slot));
    if (it != NULL) JS_MarkValue(rt, it->headers, mark);
}

static DHeaders *headers_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&headers_slot));
}

static JSValue headers_make(JSContext *ctx, JSValueConst prototype, int guard)
{
    JSClassID id = os64_js_class_id(&headers_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DHeaders *h = js_mallocz(ctx, sizeof(*h));
    if (h == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    h->guard = guard;
    JS_SetOpaque(object, h);
    return object;
}

static void headers_changed(JSContext *ctx, DHeaders *h)
{
    if (h->sorted_valid || h->sorted != NULL) headers_sorted_clear(JS_GetRuntime(ctx), h);
}

/* A name and value put on the list as given (both taken), UNGUARDED: what
 * may call it is the guarded append (headers_append) and set, which have
 * judged the header, and the two places a list arrives already judged: a
 * network's filtered block (d_headers_from_block) and a copy of a list
 * (headers_copy). A header the binding writes itself goes through
 * headers_author. -1 on no memory. */
static int list_take(JSContext *ctx, DHeaders *h, char *name, size_t nlen, char *value, size_t vlen)
{
    if (h->count == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 8;
        DHeader *grown = js_realloc(ctx, h->list, cap * sizeof(*grown));
        if (grown == NULL) {
            js_free(ctx, name);
            js_free(ctx, value);
            return -1;
        }
        h->list = grown;
        h->cap = cap;
    }
    /* § 2.2.2 "append" gives a name the list holds already its first
     * spelling. Nothing a script reads shows a spelling (every reader
     * lowercases or folds case) and HTTP's names fold case, so the scan
     * that rule costs, a list's length at every append and so its square
     * for a list a page or a server hands over whole, is not made. */
    h->list[h->count++] = (DHeader){name, value, nlen, vlen};
    headers_changed(ctx, h);
    return 0;
}

static void list_delete(JSContext *ctx, DHeaders *h, const char *name, size_t nlen)
{
    size_t kept = 0;
    for (size_t i = 0; i < h->count; i++) {
        if (d_same_nocase(h->list[i].name, h->list[i].name_length, name, nlen)) {
            js_free(ctx, h->list[i].name);
            js_free(ctx, h->list[i].value);
            continue;
        }
        h->list[kept++] = h->list[i];
    }
    if (kept != h->count) headers_changed(ctx, h);
    h->count = kept;
}

static bool list_contains(const DHeaders *h, const char *name, size_t nlen)
{
    for (size_t i = 0; i < h->count; i++)
        if (d_same_nocase(h->list[i].name, h->list[i].name_length, name, nlen)) return true;
    return false;
}

/* § 2.2.2 "get": the values joined by ", ", or NULL for none (engine
 * memory). `*no_memory` says why NULL when it is that. */
static char *list_get(JSContext *ctx, const DHeaders *h, const char *name, size_t nlen, size_t *length, bool *no_memory)
{
    *no_memory = false;
    size_t total = 0, found = 0;
    for (size_t i = 0; i < h->count; i++)
        if (d_same_nocase(h->list[i].name, h->list[i].name_length, name, nlen)) {
            total += h->list[i].value_length + (found ? 2 : 0);
            found++;
        }
    if (found == 0) return NULL;
    char *out = js_malloc(ctx, total + 1);
    if (out == NULL) {
        *no_memory = true;
        return NULL;
    }
    size_t at = 0;
    for (size_t i = 0, k = 0; i < h->count; i++)
        if (d_same_nocase(h->list[i].name, h->list[i].name_length, name, nlen)) {
            if (k++ > 0) {
                out[at++] = ',';
                out[at++] = ' ';
            }
            os64_memcpy(out + at, h->list[i].value, h->list[i].value_length);
            at += h->list[i].value_length;
        }
    out[at] = '\0';
    *length = at;
    return out;
}

/* § 2.2.2 "sort and combine", cached: names lowercased and sorted (a merge
 * sort of the list's indices, n log n), each name's values joined, and each
 * set-cookie its own entry. -1 on no memory. */
static int headers_sort(JSContext *ctx, DHeaders *h)
{
    if (h->sorted_valid) return 0;
    headers_sorted_clear(JS_GetRuntime(ctx), h);
    size_t n = h->count;
    size_t *order = js_malloc(ctx, (2 * n + 1) * sizeof(*order));
    if (order == NULL) return -1;
    size_t *scratch = order + n;
    for (size_t i = 0; i < n; i++) order[i] = i;
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                const DHeader *a = &h->list[order[i]], *b = &h->list[order[j]];
                scratch[k++] = order_nocase(b->name, b->name_length, a->name, a->name_length) < 0 ? order[j++] : order[i++];
            }
            while (i < mid) scratch[k++] = order[i++];
            while (j < hi) scratch[k++] = order[j++];
        }
        os64_memcpy(order, scratch, n * sizeof(*order));
        d_request_steps += n;
    }
    DHeader *sorted = js_mallocz(ctx, (n + 1) * sizeof(*sorted));
    if (sorted == NULL) {
        js_free(ctx, order);
        return -1;
    }
    size_t count = 0;
    int result = 0;
    for (size_t i = 0; result == 0 && i < n;) {
        const DHeader *first = &h->list[order[i]];
        bool cookie = d_same_nocase(first->name, first->name_length, "set-cookie", 10);
        size_t j = i + 1, total = first->value_length;
        while (!cookie && j < n &&
               d_same_nocase(h->list[order[j]].name, h->list[order[j]].name_length, first->name, first->name_length)) {
            total += 2 + h->list[order[j]].value_length;
            j++;
        }
        if (cookie) j = i + 1;
        char *name = engine_dup(ctx, first->name, first->name_length);
        char *value = name != NULL ? js_malloc(ctx, total + 1) : NULL;
        if (value == NULL) {
            js_free(ctx, name);
            result = -1;
            break;
        }
        for (size_t k = 0; k < first->name_length; k++) name[k] = lower(name[k]);
        size_t at = 0;
        for (size_t k = i; k < j; k++) {
            if (k > i) {
                value[at++] = ',';
                value[at++] = ' ';
            }
            os64_memcpy(value + at, h->list[order[k]].value, h->list[order[k]].value_length);
            at += h->list[order[k]].value_length;
        }
        value[at] = '\0';
        sorted[count++] = (DHeader){name, value, first->name_length, at};
        d_request_steps += j - i;
        i = j;
    }
    js_free(ctx, order);
    h->sorted = sorted;
    h->sorted_count = count;
    if (result < 0) {
        headers_sorted_clear(JS_GetRuntime(ctx), h);
        return -1;
    }
    h->sorted_valid = true;
    return 0;
}

/* The name and value a Headers method was given, as bytes: the name a
 * token, the value normalized and a header value (§ 5.1 "validate"). */
static bool header_arguments(JSContext *ctx, JSValueConst name_value, JSValueConst value_value, bool has_value,
                             char **name, size_t *nlen, char **value, size_t *vlen)
{
    *name = *value = NULL;
    if (!d_byte_string(ctx, name_value, name, nlen)) return false;
    if (has_value && !d_byte_string(ctx, value_value, value, vlen)) {
        js_free(ctx, *name);
        *name = NULL;
        return false;
    }
    if (!way_header_name_ok(*name, *nlen)) {
        js_free(ctx, *name);
        js_free(ctx, *value);
        *name = *value = NULL;
        JS_ThrowTypeError(ctx, "That is not a header name");
        return false;
    }
    if (has_value) {
        const char *v = *value;
        size_t n = *vlen;
        way_header_value_normalize(&v, &n);
        os64_memmove(*value, v, n);
        (*value)[n] = '\0';
        *vlen = n;
        if (!way_header_value_ok(*value, *vlen)) {
            js_free(ctx, *name);
            js_free(ctx, *value);
            *name = *value = NULL;
            JS_ThrowTypeError(ctx, "That is not a header value");
            return false;
        }
    }
    return true;
}

/* § 5.1 "validate" a header for this object's guard: false (and an
 * exception) for an immutable one; false and no exception for a header the
 * guard silently refuses. */
static bool guard_allows(JSContext *ctx, const DHeaders *h, const char *name, size_t nlen, const char *value,
                         size_t vlen, bool *refused)
{
    *refused = false;
    if (h->guard == G_IMMUTABLE) {
        JS_ThrowTypeError(ctx, "These headers cannot be changed");
        *refused = true;
        return false;
    }
    if ((h->guard == G_REQUEST || h->guard == G_REQUEST_NO_CORS) &&
        way_header_forbidden_request(name, nlen, value, vlen))
        return false;
    if (h->guard == G_RESPONSE && way_header_forbidden_response(name, nlen)) return false;
    return true;
}

/* request-no-cors's "remove privileged no-CORS request-headers". */
static void remove_privileged(JSContext *ctx, DHeaders *h)
{
    if (h->guard == G_REQUEST_NO_CORS) list_delete(ctx, h, "range", 5);
}

/* § 5.1 "append" (both taken). 0 done or refused silently, -1 thrown. */
static int headers_append(JSContext *ctx, DHeaders *h, char *name, size_t nlen, char *value, size_t vlen)
{
    bool refused;
    if (!guard_allows(ctx, h, name, nlen, value, vlen, &refused)) {
        js_free(ctx, name);
        js_free(ctx, value);
        return refused ? -1 : 0;
    }
    if (h->guard == G_REQUEST_NO_CORS) {
        bool no_memory;
        size_t length = 0;
        char *joined = list_get(ctx, h, name, nlen, &length, &no_memory);
        if (no_memory) {
            js_free(ctx, name);
            js_free(ctx, value);
            return -1;
        }
        bool safe;
        if (joined == NULL) safe = way_header_no_cors_safelisted(name, nlen, value, vlen);
        else {
            char *temporary = js_malloc(ctx, length + 2 + vlen + 1);
            if (temporary == NULL) {
                js_free(ctx, joined);
                js_free(ctx, name);
                js_free(ctx, value);
                return -1;
            }
            os64_memcpy(temporary, joined, length);
            os64_memcpy(temporary + length, ", ", 2);
            os64_memcpy(temporary + length + 2, value, vlen);
            safe = way_header_no_cors_safelisted(name, nlen, temporary, length + 2 + vlen);
            js_free(ctx, temporary);
        }
        js_free(ctx, joined);
        if (!safe) {
            js_free(ctx, name);
            js_free(ctx, value);
            return 0;
        }
    }
    if (list_take(ctx, h, name, nlen, value, vlen) < 0) return -1;
    remove_privileged(ctx, h);
    return 0;
}

/* A header the binding writes itself (a body's Content-Type, a redirect's
 * Location): Fetch's constructors say "append" to the object's headers,
 * which is the guarded append, so a no-cors request's guard drops a type
 * that is not safelisted as it drops a page's own. -1 on no memory. */
static int headers_author(JSContext *ctx, DHeaders *h, const char *name, const char *value)
{
    size_t nlen = os64_strlen(name), vlen = os64_strlen(value);
    char *n = engine_dup(ctx, name, nlen);
    char *v = n != NULL ? engine_dup(ctx, value, vlen) : NULL;
    if (v == NULL) {
        js_free(ctx, n);
        return -1;
    }
    return headers_append(ctx, h, n, nlen, v, vlen);
}

/* § 5.1 "fill": a record, or a sequence of pairs, or another Headers. -1
 * thrown. */
static int headers_fill(os64_dom_t *dom, JSContext *ctx, JSValueConst self, JSValueConst init)
{
    if (JS_IsUndefined(init)) return 0;
    if (!JS_IsObject(init)) {
        JS_ThrowTypeError(ctx, "Headers' init must be an object");
        return -1;
    }
    JSAtom key = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    if (key == JS_ATOM_NULL) return -1;
    JSValue method = JS_GetProperty(ctx, init, key);
    JS_FreeAtom(ctx, key);
    if (JS_IsException(method)) return -1;
    bool sequence = !JS_IsUndefined(method) && !JS_IsNull(method);
    JS_FreeValue(ctx, method);
    if (sequence) {
        JSValue *pairs;
        size_t count;
        if (d_sequence(dom, ctx, init, D_SEQUENCE_MAX, &pairs, &count) < 0) return -1;
        /* Each pair is converted whole, as WebIDL converts the outer
         * sequence's elements, before any of them is appended. */
        JSValue (*converted)[2] = count ? js_mallocz(ctx, count * sizeof(*converted)) : NULL;
        int result = count && converted == NULL ? -1 : 0;
        for (size_t i = 0; result == 0 && i < count; i++) {
            JSValue *inner;
            size_t n;
            if (d_sequence(dom, ctx, pairs[i], 2, &inner, &n) < 0) { result = -1; break; }
            if (n != 2) {
                d_sequence_free(ctx, inner, n);
                JS_ThrowTypeError(ctx, "A header pair has exactly two items");
                result = -1;
                break;
            }
            converted[i][0] = inner[0];
            converted[i][1] = inner[1];
            js_free(ctx, inner);
        }
        for (size_t i = 0; result == 0 && i < count; i++) {
            char *name, *value;
            size_t nlen, vlen;
            DHeaders *h = headers_state(self);
            if (!header_arguments(ctx, converted[i][0], converted[i][1], true, &name, &nlen, &value, &vlen) ||
                headers_append(ctx, headers_state(self), name, nlen, value, vlen) < 0)
                result = -1;
            (void)h;
        }
        for (size_t i = 0; converted != NULL && i < count; i++) {
            JS_FreeValue(ctx, converted[i][0]);
            JS_FreeValue(ctx, converted[i][1]);
        }
        js_free(ctx, converted);
        d_sequence_free(ctx, pairs, count);
        return result;
    }
    /* record<ByteString, ByteString> (WebIDL § 3.2.18): each own string key
     * in order, its descriptor asked, and an enumerable one's key made a
     * ByteString BEFORE its value is read; the record is whole before fill
     * appends any of it. */
    JSPropertyEnum *props;
    uint32_t count;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, init, JS_GPN_STRING_MASK) < 0) return -1;
    JSValue *pairs = count ? js_mallocz(ctx, 2 * (size_t)count * sizeof(*pairs)) : NULL;
    int result = count && pairs == NULL ? -1 : 0;
    size_t taken = 0;
    for (uint32_t i = 0; result == 0 && i < count; i++) {
        JSPropertyDescriptor desc;
        int has = JS_GetOwnProperty(ctx, &desc, init, props[i].atom);
        if (has < 0) { result = -1; break; }
        if (has == 0) continue;
        bool enumerable = (desc.flags & JS_PROP_ENUMERABLE) != 0;
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        if (!enumerable) continue;
        JSValue key = JS_AtomToString(ctx, props[i].atom);
        char *bytes;
        size_t length;
        if (JS_IsException(key) || !d_byte_string(ctx, key, &bytes, &length)) {
            JS_FreeValue(ctx, key);
            result = -1;
            break;
        }
        js_free(ctx, bytes);
        JSValue value = JS_GetProperty(ctx, init, props[i].atom);
        JSValue text = JS_IsException(value) ? JS_EXCEPTION : JS_ToString(ctx, value);
        JS_FreeValue(ctx, value);
        if (!JS_IsException(text) && !d_byte_string(ctx, text, &bytes, &length)) {
            JS_FreeValue(ctx, text);
            text = JS_EXCEPTION;
        } else if (!JS_IsException(text)) js_free(ctx, bytes);
        if (JS_IsException(text)) {
            JS_FreeValue(ctx, key);
            result = -1;
            break;
        }
        pairs[2 * taken] = key;
        pairs[2 * taken + 1] = text;
        taken++;
    }
    for (size_t i = 0; result == 0 && i < taken; i++) {
        char *name, *value;
        size_t nlen, vlen;
        if (!header_arguments(ctx, pairs[2 * i], pairs[2 * i + 1], true, &name, &nlen, &value, &vlen) ||
            headers_append(ctx, headers_state(self), name, nlen, value, vlen) < 0)
            result = -1;
    }
    for (size_t i = 0; i < 2 * taken; i++) JS_FreeValue(ctx, pairs[i]);
    js_free(ctx, pairs);
    for (uint32_t i = 0; i < count; i++) JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props);
    return result;
}

JSValue d_headers_from_block(os64_dom_t *dom, JSContext *ctx, const char *block, size_t length, int guard)
{
    (void)dom;
    JSValue prototype = JS_GetClassProto(ctx, os64_js_class_id(&headers_slot));
    JSValue object = headers_make(ctx, prototype, G_NONE);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DHeaders *h = headers_state(object);
    size_t at = 0;
    way_header_t line;
    while (way_header_next(block, length, &at, &line)) {
        d_request_steps++;
        char *name = engine_dup(ctx, line.name, line.name_len);
        char *value = name != NULL ? engine_dup(ctx, line.value, line.value_len) : NULL;
        if (value == NULL || list_take(ctx, h, name, line.name_len, value, line.value_len) < 0) {
            if (value == NULL) js_free(ctx, name);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
    }
    h->guard = guard;
    return object;
}

/* A copy of a Headers' list into another's, as a Request or a clone takes
 * it. -1 on no memory. */
static int headers_copy(JSContext *ctx, DHeaders *to, const DHeaders *from)
{
    for (size_t i = 0; i < from->count; i++) {
        char *name = engine_dup(ctx, from->list[i].name, from->list[i].name_length);
        char *value = name != NULL ? engine_dup(ctx, from->list[i].value, from->list[i].value_length) : NULL;
        if (value == NULL) {
            js_free(ctx, name);
            return -1;
        }
        if (list_take(ctx, to, name, from->list[i].name_length, value, from->list[i].value_length) < 0) return -1;
    }
    return 0;
}

/* The list as a header block (way/origin.h's shape), engine memory. */
static char *headers_block(JSContext *ctx, const DHeaders *h, size_t *length)
{
    size_t total = 0;
    for (size_t i = 0; i < h->count; i++) total += h->list[i].name_length + 2 + h->list[i].value_length + 2;
    char *out = js_malloc(ctx, total + 1);
    if (out == NULL) return NULL;
    size_t at = 0;
    for (size_t i = 0; i < h->count; i++) {
        os64_memcpy(out + at, h->list[i].name, h->list[i].name_length);
        at += h->list[i].name_length;
        out[at++] = ':';
        out[at++] = ' ';
        os64_memcpy(out + at, h->list[i].value, h->list[i].value_length);
        at += h->list[i].value_length;
        out[at++] = '\r';
        out[at++] = '\n';
    }
    out[at] = '\0';
    *length = at;
    return out;
}

enum { H_APPEND, H_DELETE, H_GET, H_GET_SET_COOKIE, H_HAS, H_SET, H_ENTRIES, H_KEYS, H_VALUES, H_FOR_EACH, H_NEXT };

static JSValue result_object(JSContext *ctx, JSValue value, bool done)
{
    JSValue result = JS_NewObject(ctx);
    if (JS_IsException(result) || JS_IsException(value)) {
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, value);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(ctx, result, "value", value, JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueStr(ctx, result, "done", JS_NewBool(ctx, done), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, result);
        return JS_EXCEPTION;
    }
    return result;
}

static JSValue array_of_two(JSContext *ctx, JSValue a, JSValue b)
{
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array) || JS_IsException(a) || JS_IsException(b)) {
        JS_FreeValue(ctx, a);
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueUint32(ctx, array, 0, a, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, b);
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueUint32(ctx, array, 1, b, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, array);
        return JS_EXCEPTION;
    }
    return array;
}

static JSValue headers_iterator_new(JSContext *ctx, JSValueConst headers, int kind)
{
    JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&headers_iterator_slot));
    if (JS_IsException(object)) return object;
    DHeadersIterator *it = js_mallocz(ctx, sizeof(*it));
    if (it == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    it->headers = JS_DupValue(ctx, headers);
    it->kind = kind;
    JS_SetOpaque(object, it);
    return object;
}

static JSValue headers_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == H_NEXT) {
        DHeadersIterator *it = JS_GetOpaque(self, os64_js_class_id(&headers_iterator_slot));
        if (it == NULL) return JS_ThrowTypeError(ctx, "Expected a Headers iterator");
        /* WebIDL's pair iterator reads the sorted list at each step, by
         * index: the cache answers until the list changes. */
        DHeaders *h = headers_state(it->headers);
        if (h == NULL) return result_object(ctx, JS_UNDEFINED, true);
        if (headers_sort(ctx, h) < 0) return JS_EXCEPTION;
        if (it->index >= h->sorted_count) return result_object(ctx, JS_UNDEFINED, true);
        const DHeader *e = &h->sorted[it->index++];
        if (it->kind == H_KEYS) return result_object(ctx, d_byte_text(ctx, e->name, e->name_length), false);
        if (it->kind == H_VALUES) return result_object(ctx, d_byte_text(ctx, e->value, e->value_length), false);
        return result_object(ctx, array_of_two(ctx, d_byte_text(ctx, e->name, e->name_length),
                                               d_byte_text(ctx, e->value, e->value_length)), false);
    }
    DHeaders *h = headers_state(self);
    if (h == NULL) return JS_ThrowTypeError(ctx, "Expected a Headers");
    switch (magic) {
    case H_APPEND: case H_SET: {
        if (argc < 2) return JS_ThrowTypeError(ctx, "A header needs a name and a value");
        char *name, *value;
        size_t nlen, vlen;
        if (!header_arguments(ctx, argv[0], argv[1], true, &name, &nlen, &value, &vlen)) return JS_EXCEPTION;
        h = headers_state(self);    /* the conversions ran page code */
        if (magic == H_APPEND) return headers_append(ctx, h, name, nlen, value, vlen) < 0 ? JS_EXCEPTION : JS_UNDEFINED;
        bool refused;
        if (!guard_allows(ctx, h, name, nlen, value, vlen, &refused) ||
            (h->guard == G_REQUEST_NO_CORS && !way_header_no_cors_safelisted(name, nlen, value, vlen))) {
            js_free(ctx, name);
            js_free(ctx, value);
            return refused ? JS_EXCEPTION : JS_UNDEFINED;
        }
        /* § 2.2.2 "set": the first of the name replaced, the rest gone. */
        bool replaced = false;
        size_t kept = 0;
        for (size_t i = 0; i < h->count; i++) {
            if (d_same_nocase(h->list[i].name, h->list[i].name_length, name, nlen)) {
                if (!replaced) {
                    js_free(ctx, h->list[i].value);
                    h->list[i].value = value;
                    h->list[i].value_length = vlen;
                    h->list[kept++] = h->list[i];
                    replaced = true;
                } else {
                    js_free(ctx, h->list[i].name);
                    js_free(ctx, h->list[i].value);
                }
                continue;
            }
            h->list[kept++] = h->list[i];
        }
        h->count = kept;
        headers_changed(ctx, h);
        if (replaced) {
            js_free(ctx, name);
            remove_privileged(ctx, h);
            return JS_UNDEFINED;
        }
        if (list_take(ctx, h, name, nlen, value, vlen) < 0) return JS_EXCEPTION;
        remove_privileged(ctx, h);
        return JS_UNDEFINED;
    }
    case H_DELETE: case H_GET: case H_HAS: {
        if (argc < 1) return JS_ThrowTypeError(ctx, "A header name is needed");
        char *name, *unused;
        size_t nlen, ulen;
        if (!header_arguments(ctx, argv[0], JS_UNDEFINED, false, &name, &nlen, &unused, &ulen)) return JS_EXCEPTION;
        h = headers_state(self);
        JSValue result = JS_UNDEFINED;
        if (magic == H_HAS) result = JS_NewBool(ctx, list_contains(h, name, nlen));
        else if (magic == H_GET) {
            bool no_memory;
            size_t length = 0;
            char *joined = list_get(ctx, h, name, nlen, &length, &no_memory);
            result = no_memory ? JS_EXCEPTION : joined == NULL ? JS_NULL : d_byte_text(ctx, joined, length);
            js_free(ctx, joined);
        } else {
            bool refused;
            if (guard_allows(ctx, h, name, nlen, "", 0, &refused)) {
                if (!(h->guard == G_REQUEST_NO_CORS && !way_header_no_cors_safelisted_name(name, nlen) &&
                      !way_header_privileged_no_cors(name, nlen)))
                    list_delete(ctx, h, name, nlen);
                remove_privileged(ctx, h);
            } else if (refused) result = JS_EXCEPTION;
        }
        js_free(ctx, name);
        return result;
    }
    case H_GET_SET_COOKIE: {
        JSValue array = JS_NewArray(ctx);
        uint32_t at = 0;
        for (size_t i = 0; !JS_IsException(array) && i < h->count; i++)
            if (d_same_nocase(h->list[i].name, h->list[i].name_length, "set-cookie", 10) &&
                JS_DefinePropertyValueUint32(ctx, array, at++, d_byte_text(ctx, h->list[i].value, h->list[i].value_length),
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, array);
                array = JS_EXCEPTION;
            }
        return array;
    }
    case H_ENTRIES: return headers_iterator_new(ctx, self, H_ENTRIES);
    case H_KEYS: return headers_iterator_new(ctx, self, H_KEYS);
    case H_VALUES: return headers_iterator_new(ctx, self, H_VALUES);
    case H_FOR_EACH: {
        if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "forEach needs a function");
        for (size_t i = 0;; i++) {
            h = headers_state(self);
            if (h == NULL || headers_sort(ctx, h) < 0) return JS_EXCEPTION;
            if (i >= h->sorted_count) break;
            JSValue args[3] = {d_byte_text(ctx, h->sorted[i].value, h->sorted[i].value_length),
                               d_byte_text(ctx, h->sorted[i].name, h->sorted[i].name_length), JS_DupValue(ctx, self)};
            JSValue done = JS_IsException(args[0]) || JS_IsException(args[1]) ? JS_EXCEPTION
                         : JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, args);
            for (int k = 0; k < 3; k++) JS_FreeValue(ctx, args[k]);
            if (JS_IsException(done)) return done;
            JS_FreeValue(ctx, done);
        }
        return JS_UNDEFINED;
    }
    default: return JS_UNDEFINED;
    }
}

static JSValue headers_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = headers_make(ctx, prototype, G_NONE);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    if (argc > 0 && headers_fill(dom, ctx, object, argv[0]) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

/* ── Bodies ──────────────────────────────────────────────────────────── */

static void body_free(JSRuntime *rt, DBody *b)
{
    js_free_rt(rt, b->bytes);
    JS_FreeValueRT(rt, b->stream);
    JS_FreeValueRT(rt, b->aborted);
}

static void body_mark(JSRuntime *rt, const DBody *b, JS_MarkFunc *mark)
{
    JS_MarkValue(rt, b->stream, mark);
    JS_MarkValue(rt, b->aborted, mark);
}

static DReq *request_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&request_slot));
}

static DResp *response_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&response_slot));
}

static DBody *body_of(JSValueConst value)
{
    DReq *q = request_state(value);
    if (q != NULL) return &q->body;
    DResp *r = response_state(value);
    return r != NULL ? &r->body : NULL;
}

static DStream *stream_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&stream_slot));
}

/* § 5.3 "unusable": a body that is disturbed or locked. */
static bool body_unusable(const DBody *b)
{
    if (!b->has) return false;
    if (b->used) return true;
    DStream *s = JS_IsUndefined(b->stream) ? NULL : stream_state(b->stream);
    return s != NULL && s->locked;
}

int d_body_extract(os64_dom_t *dom, JSContext *ctx, JSValueConst value, DBytes *out, bool *has_type)
{
    os64_memset(out, 0, sizeof(*out));
    *has_type = true;
    const uint8_t *bytes;
    size_t length;
    const char *type;
    int kind = d_buffer_source(dom, ctx, value, &bytes, &length);
    if (kind < 0) return -1;
    if (kind > 0) {
        out->bytes = js_malloc(ctx, length + 1);
        if (out->bytes == NULL) return -1;
        if (length != 0) os64_memcpy(out->bytes, bytes, length);
        out->length = length;
        *has_type = false;
        return 1;
    }
    if (d_blob_get(value, &bytes, &length, &type)) {
        out->bytes = js_malloc(ctx, length + 1);
        out->type = out->bytes != NULL ? engine_dup(ctx, type, os64_strlen(type)) : NULL;
        if (out->type == NULL) {
            d_bytes_free(ctx, out);
            return -1;
        }
        if (length != 0) os64_memcpy(out->bytes, bytes, length);
        out->length = length;
        *has_type = type[0] != '\0';
        return 1;
    }
    if (d_form_data_is(value)) return d_form_data_encode(dom, ctx, value, out) < 0 ? -1 : 1;
    size_t n;
    char *text = d_params_text(ctx, value, &n);
    if (text == NULL && JS_HasException(ctx)) return -1;
    if (text != NULL) {
        out->bytes = (uint8_t *)text;
        out->length = n;
        out->type = engine_dup(ctx, "application/x-www-form-urlencoded;charset=UTF-8", 47);
        if (out->type == NULL) {
            d_bytes_free(ctx, out);
            return -1;
        }
        return 1;
    }
    if (JS_IsObject(value) && JS_GetOpaque(value, os64_js_class_id(&stream_slot)) != NULL) {
        JS_ThrowTypeError(ctx, "A stream as a request body is not supported");
        return -1;
    }
    /* Anything else is a USVString. */
    size_t length8;
    const char *text8 = JS_ToCStringLen(ctx, &length8, value);
    if (text8 == NULL) return -1;
    out->bytes = js_malloc(ctx, length8 + 1);
    if (out->bytes == NULL) {
        JS_FreeCString(ctx, text8);
        return -1;
    }
    os64_memcpy(out->bytes, text8, length8);
    JS_FreeCString(ctx, text8);
    for (size_t i = 0; i + 2 < length8; i++)
        if (out->bytes[i] == 0xed && out->bytes[i + 1] >= 0xa0) {
            out->bytes[i] = 0xef;
            out->bytes[i + 1] = 0xbf;
            out->bytes[i + 2] = 0xbd;
            i += 2;
        }
    out->length = length8;
    out->type = engine_dup(ctx, "text/plain;charset=UTF-8", 24);
    if (out->type == NULL) {
        d_bytes_free(ctx, out);
        return -1;
    }
    return 1;
}

/* The MIME type a body's headers give it, for blob() and formData()
 * (engine memory, "" for none): the last Content-Type line as written,
 * when it is a MIME type at all. A multipart body's boundary is one of its
 * parameters, and may hold a comma, so the line is not taken from the
 * combined value; blob() lowercases it, as a Blob's type is. */
static char *body_type(JSContext *ctx, JSValueConst headers)
{
    const DHeaders *h = headers_state(headers);
    const DHeader *line = NULL;
    for (size_t i = 0; h != NULL && i < h->count; i++)
        if (d_same_nocase(h->list[i].name, h->list[i].name_length, "content-type", 12)) line = &h->list[i];
    char essence[WAY_MIME_ESSENCE_MAX];
    bool type = line != NULL && way_mime_parse(line->value, line->value_length, essence, sizeof(essence), NULL, 0);
    return engine_dup(ctx, type ? line->value : "", type ? line->value_length : 0);
}

/* WebIDL's JSON.parse, as the engine's own. */
static JSValue json_parse(JSContext *ctx, const uint8_t *bytes, size_t length)
{
    JSValue text = d_utf8_decode(ctx, bytes, length);
    if (JS_IsException(text)) return text;
    size_t n;
    const char *utf8 = JS_ToCStringLen(ctx, &n, text);
    JS_FreeValue(ctx, text);
    if (utf8 == NULL) return JS_EXCEPTION;
    JSValue value = JS_ParseJSON(ctx, utf8, n, "<body>");
    JS_FreeCString(ctx, utf8);
    return value;
}

enum { BODY_TEXT, BODY_JSON, BODY_ARRAY_BUFFER, BODY_BLOB, BODY_FORM_DATA, BODY_BYTES, BODY_USED, BODY_STREAM };

/* § 5.3 "consume body": the body taken whole and handed back as `kind`, in
 * a promise. */
static JSValue body_consume(os64_dom_t *dom, JSContext *ctx, JSValueConst self, DBody *b, JSValueConst headers, int kind)
{
    /* A body an abort errored answers its reason however often it is
     * asked. */
    if (!JS_IsUndefined(b->aborted)) {
        b->used = true;
        return d_promise_settled(ctx, JS_Throw(ctx, JS_DupValue(ctx, b->aborted)));
    }
    if (body_unusable(b)) return d_promise_settled(ctx, JS_ThrowTypeError(ctx, "The body has already been read"));
    b->used = b->has;
    /* "Fully read" takes a reader it never releases: the stream, made now
     * or later, is closed and locked. */
    b->consumed = b->has;
    DStream *s = JS_IsUndefined(b->stream) ? NULL : stream_state(b->stream);
    if (s != NULL) {
        s->locked = true;
        s->state = ST_CLOSED;
    }
    const uint8_t *bytes = b->bytes != NULL ? b->bytes : (const uint8_t *)"";
    size_t length = b->has ? b->length : 0;
    (void)self;
    switch (kind) {
    case BODY_TEXT: return d_promise_settled(ctx, d_utf8_decode(ctx, bytes, length));
    case BODY_JSON: return d_promise_settled(ctx, json_parse(ctx, bytes, length));
    case BODY_ARRAY_BUFFER: return d_promise_settled(ctx, JS_NewArrayBufferCopy(ctx, bytes, length));
    case BODY_BYTES: return d_promise_settled(ctx, d_uint8_array(ctx, bytes, length));
    case BODY_BLOB: {
        DBytes copy = {js_malloc(ctx, length + 1), length, body_type(ctx, headers)};
        if (copy.bytes == NULL || copy.type == NULL) {
            d_bytes_free(ctx, &copy);
            return d_promise_settled(ctx, JS_EXCEPTION);
        }
        if (length != 0) os64_memcpy(copy.bytes, bytes, length);
        for (char *t = copy.type; *t != '\0'; t++) *t = lower(*t);
        return d_promise_settled(ctx, d_blob_new(dom, ctx, &copy));
    }
    case BODY_FORM_DATA: {
        char *type = body_type(ctx, headers);
        if (type == NULL) return d_promise_settled(ctx, JS_EXCEPTION);
        JSValue form = d_form_data_parse(dom, ctx, bytes, length, type);
        js_free(ctx, type);
        return d_promise_settled(ctx, form);
    }
    default: return JS_UNDEFINED;
    }
}

static JSValue stream_make(os64_dom_t *dom, JSContext *ctx, JSValueConst owner, const uint8_t *bytes, size_t length);

/* body's getter: the stream, made once, or null for no body. */
static JSValue body_stream(os64_dom_t *dom, JSContext *ctx, JSValueConst self, DBody *b)
{
    if (!b->has) return JS_NULL;
    if (JS_IsUndefined(b->stream)) {
        JSValue stream = stream_make(dom, ctx, self, NULL, 0);
        if (JS_IsException(stream)) return stream;
        b = body_of(self);
        b->stream = stream;
        /* A body already read is a stream already closed, and one a body
         * method read is locked by the reader it took. */
        if (b->used) stream_state(stream)->state = ST_CLOSED;
        if (b->consumed) stream_state(stream)->locked = true;
    }
    return JS_DupValue(ctx, b->stream);
}

/* ── The stream (minimal: one chunk, then done) ──────────────────────── */

static void stream_finalizer(JSRuntime *rt, JSValue value)
{
    DStream *s = stream_state(value);
    if (s == NULL) return;
    JS_FreeValueRT(rt, s->owner);
    JS_FreeValueRT(rt, s->reader);
    JS_FreeValueRT(rt, s->error);
    js_free_rt(rt, s->bytes);
    js_free_rt(rt, s);
}

static void stream_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DStream *s = stream_state(value);
    if (s == NULL) return;
    JS_MarkValue(rt, s->owner, mark);
    JS_MarkValue(rt, s->reader, mark);
    JS_MarkValue(rt, s->error, mark);
}

static DReader *reader_state(JSValueConst value)
{
    return JS_GetOpaque(value, os64_js_class_id(&reader_slot));
}

static void reader_finalizer(JSRuntime *rt, JSValue value)
{
    DReader *r = reader_state(value);
    if (r == NULL) return;
    JS_FreeValueRT(rt, r->stream);
    JS_FreeValueRT(rt, r->closed);
    JS_FreeValueRT(rt, r->closed_resolve);
    JS_FreeValueRT(rt, r->closed_reject);
    js_free_rt(rt, r);
}

static void reader_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DReader *r = reader_state(value);
    if (r == NULL) return;
    JS_MarkValue(rt, r->stream, mark);
    JS_MarkValue(rt, r->closed, mark);
    JS_MarkValue(rt, r->closed_resolve, mark);
    JS_MarkValue(rt, r->closed_reject, mark);
}

static JSValue stream_make(os64_dom_t *dom, JSContext *ctx, JSValueConst owner, const uint8_t *bytes, size_t length)
{
    (void)dom;
    JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&stream_slot));
    if (JS_IsException(object)) return object;
    DStream *s = js_mallocz(ctx, sizeof(*s));
    uint8_t *copy = s != NULL && JS_IsUndefined(owner) ? js_malloc(ctx, length + 1) : NULL;
    if (s == NULL || (JS_IsUndefined(owner) && copy == NULL)) {
        js_free(ctx, s);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    if (copy != NULL && length != 0) os64_memcpy(copy, bytes, length);
    s->owner = JS_DupValue(ctx, owner);
    s->bytes = copy;
    s->length = length;
    s->reader = JS_UNDEFINED;
    s->error = JS_UNDEFINED;
    JS_SetOpaque(object, s);
    return object;
}

JSValue d_stream_new(os64_dom_t *dom, JSContext *ctx, const uint8_t *bytes, size_t length)
{
    return stream_make(dom, ctx, JS_UNDEFINED, bytes, length);
}

/* Settles a reader's `closed`: resolved (`error` undefined) or rejected. */
static void reader_closed(JSContext *ctx, DReader *r, JSValueConst error)
{
    JSValueConst fn = JS_IsUndefined(error) ? r->closed_resolve : r->closed_reject;
    if (!JS_IsUndefined(fn)) {
        JSValue done = JS_Call(ctx, fn, JS_UNDEFINED, 1, &error);
        JS_FreeValue(ctx, done);
        if (JS_IsException(done)) JS_FreeValue(ctx, JS_GetException(ctx));
    }
    JS_FreeValue(ctx, r->closed_resolve);
    JS_FreeValue(ctx, r->closed_reject);
    r->closed_resolve = r->closed_reject = JS_UNDEFINED;
}

enum { S_LOCKED, S_GET_READER, S_CANCEL, S_TEE, R_READ, R_RELEASE, R_CLOSED, R_CANCEL };

static JSValue stream_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    JSValueConst arg = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (magic >= R_READ) {
        DReader *r = reader_state(self);
        if (r == NULL) {
            JSValue error = JS_ThrowTypeError(ctx, "Expected a ReadableStreamDefaultReader");
            return magic == R_RELEASE ? error : d_promise_settled(ctx, error);
        }
        if (magic == R_CLOSED) return JS_DupValue(ctx, r->closed);
        DStream *s = JS_IsUndefined(r->stream) ? NULL : stream_state(r->stream);
        if (magic == R_RELEASE) {
            if (s == NULL) return JS_UNDEFINED;
            s->locked = false;
            JS_FreeValue(ctx, s->reader);
            s->reader = JS_UNDEFINED;
            JS_FreeValue(ctx, r->stream);
            r->stream = JS_UNDEFINED;
            JSValue error = JS_NewError(ctx);
            if (!JS_IsException(error)) reader_closed(ctx, r, error);
            JS_FreeValue(ctx, error);
            return JS_UNDEFINED;
        }
        if (s == NULL) return d_promise_settled(ctx, JS_ThrowTypeError(ctx, "The reader has been released"));
        if (magic == R_CANCEL) {
            if (s->state == ST_READABLE) {
                s->state = ST_CLOSED;
                DBody *b = body_of(s->owner);
                if (b != NULL) b->used = true;
                reader_closed(ctx, r, JS_UNDEFINED);
            }
            return d_promise_settled(ctx, JS_UNDEFINED);
        }
        /* read(): the whole body as one chunk, then done. */
        if (s->state == ST_ERRORED) return d_promise_settled(ctx, JS_Throw(ctx, JS_DupValue(ctx, s->error)));
        DBody *b = body_of(s->owner);
        if (b != NULL && !JS_IsUndefined(b->aborted) && s->state == ST_READABLE) {
            s->state = ST_ERRORED;
            JS_FreeValue(ctx, s->error);
            s->error = JS_DupValue(ctx, b->aborted);
            reader_closed(ctx, r, s->error);
            return d_promise_settled(ctx, JS_Throw(ctx, JS_DupValue(ctx, s->error)));
        }
        if (s->state == ST_CLOSED || s->delivered) {
            if (s->state != ST_CLOSED) {
                s->state = ST_CLOSED;
                reader_closed(ctx, r, JS_UNDEFINED);
            }
            return d_promise_settled(ctx, result_object(ctx, JS_UNDEFINED, true));
        }
        s->delivered = true;
        const uint8_t *bytes = b != NULL ? b->bytes : s->bytes;
        size_t length = b != NULL ? b->length : s->length;
        if (b != NULL) b->used = true;
        if (length == 0) {
            s->state = ST_CLOSED;
            reader_closed(ctx, r, JS_UNDEFINED);
            return d_promise_settled(ctx, result_object(ctx, JS_UNDEFINED, true));
        }
        return d_promise_settled(ctx, result_object(ctx, d_uint8_array(ctx, bytes != NULL ? bytes : (const uint8_t *)"", length), false));
    }
    DStream *s = stream_state(self);
    if (s == NULL) {
        JSValue error = JS_ThrowTypeError(ctx, "Expected a ReadableStream");
        return magic == S_CANCEL ? d_promise_settled(ctx, error) : error;
    }
    switch (magic) {
    case S_LOCKED: return JS_NewBool(ctx, s->locked);
    case S_TEE: return d_dom_exception(dom, ctx, "NotSupportedError", "tee() is not supported by this browser");
    case S_CANCEL:
        if (s->locked) return d_promise_settled(ctx, JS_ThrowTypeError(ctx, "The stream is locked"));
        if (s->state == ST_READABLE) {
            s->state = ST_CLOSED;
            DBody *b = body_of(s->owner);
            if (b != NULL) b->used = true;
        }
        return d_promise_settled(ctx, JS_UNDEFINED);
    case S_GET_READER: {
        if (JS_IsObject(arg)) {
            JSValue mode = JS_GetPropertyStr(ctx, arg, "mode");
            if (JS_IsException(mode)) return mode;
            bool byob = !JS_IsUndefined(mode);
            JS_FreeValue(ctx, mode);
            if (byob) return JS_ThrowRangeError(ctx, "Only the default reader is supported");
            s = stream_state(self);
        }
        if (s->locked) return JS_ThrowTypeError(ctx, "The stream is locked");
        JSValue object = JS_NewObjectClass(ctx, (int)os64_js_class_id(&reader_slot));
        DReader *r = JS_IsException(object) ? NULL : js_mallocz(ctx, sizeof(*r));
        if (r == NULL) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        r->stream = r->closed = r->closed_resolve = r->closed_reject = JS_UNDEFINED;
        JS_SetOpaque(object, r);
        JSValue resolving[2];
        JSValue closed = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(closed)) {
            JS_FreeValue(ctx, object);
            return closed;
        }
        r->closed = closed;
        r->closed_resolve = resolving[0];
        r->closed_reject = resolving[1];
        r->stream = JS_DupValue(ctx, self);
        s->locked = true;
        JS_FreeValue(ctx, s->reader);
        s->reader = JS_DupValue(ctx, object);
        if (s->state == ST_CLOSED) reader_closed(ctx, r, JS_UNDEFINED);
        else if (s->state == ST_ERRORED) reader_closed(ctx, r, s->error);
        return object;
    }
    default: return JS_UNDEFINED;
    }
}

/* ── Request ─────────────────────────────────────────────────────────── */

static void request_finalizer(JSRuntime *rt, JSValue value)
{
    DReq *q = request_state(value);
    if (q == NULL) return;
    body_free(rt, &q->body);
    js_free_rt(rt, q->url);
    js_free_rt(rt, q->referrer);
    js_free_rt(rt, q->integrity);
    JS_FreeValueRT(rt, q->headers);
    JS_FreeValueRT(rt, q->signal);
    js_free_rt(rt, q);
}

static void request_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DReq *q = request_state(value);
    if (q == NULL) return;
    body_mark(rt, &q->body, mark);
    JS_MarkValue(rt, q->headers, mark);
    JS_MarkValue(rt, q->signal, mark);
}

static void response_finalizer(JSRuntime *rt, JSValue value)
{
    DResp *r = response_state(value);
    if (r == NULL) return;
    body_free(rt, &r->body);
    js_free_rt(rt, r->status_text);
    js_free_rt(rt, r->url);
    JS_FreeValueRT(rt, r->headers);
    js_free_rt(rt, r);
}

static void response_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    DResp *r = response_state(value);
    if (r == NULL) return;
    body_mark(rt, &r->body, mark);
    JS_MarkValue(rt, r->headers, mark);
}

static const char *const kModes[] = {"cors", "same-origin", "no-cors"};          /* way_mode_t */
static const char *const kCredentials[] = {"same-origin", "omit", "include"};    /* way_credentials_t */
static const char *const kRedirects[] = {"follow", "error", "manual"};           /* way_redirect_t */
static const char *const kPolicies[] = {
    "strict-origin-when-cross-origin", "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin",
    "strict-origin", "origin-when-cross-origin", "unsafe-url",
};                                                                                /* way_policy_t */
static const char *const kCaches[] = {"default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached"};
static const char *const kPriorities[] = {"auto", "high", "low"};

/* A WebIDL enum value: its index, or -1 with a TypeError. */
static int enum_value(JSContext *ctx, JSValueConst value, const char *const *names, int count, const char *what)
{
    const char *text = JS_ToCString(ctx, value);
    if (text == NULL) return -1;
    int found = -1;
    for (int i = 0; i < count && found < 0; i++)
        if (os64_streq(text, names[i])) found = i;
    JS_FreeCString(ctx, text);
    if (found < 0) JS_ThrowTypeError(ctx, "That is not a %s", what);
    return found;
}

static JSValue request_alloc(JSContext *ctx, JSValueConst prototype)
{
    JSClassID id = os64_js_class_id(&request_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DReq *q = js_mallocz(ctx, sizeof(*q));
    if (q == NULL) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    q->body.stream = q->body.aborted = q->headers = q->signal = JS_UNDEFINED;
    JS_SetOpaque(object, q);
    return object;
}

/* What RequestInit says, read in WebIDL's dictionary order (the members
 * sorted by name) before any of it is used. */
typedef struct {
    JSValue body, headers, signal, referrer;
    bool has_body, has_headers, has_signal, has_referrer;
    int cache, credentials, mode, redirect, policy, priority;   /* -1: not given */
    char *integrity;
    bool has_integrity, keepalive, has_keepalive, has_method;
    char method[WAY_METHOD_MAX];
} Init;

static void init_free(JSContext *ctx, Init *init)
{
    JS_FreeValue(ctx, init->body);
    JS_FreeValue(ctx, init->headers);
    JS_FreeValue(ctx, init->signal);
    JS_FreeValue(ctx, init->referrer);
    js_free(ctx, init->integrity);
}

static int init_member(JSContext *ctx, JSValueConst init, const char *name, JSValue *out)
{
    *out = JS_GetPropertyStr(ctx, init, name);
    if (JS_IsException(*out)) return -1;
    return JS_IsUndefined(*out) ? 0 : 1;
}

static int init_read(JSContext *ctx, JSValueConst value, Init *init)
{
    os64_memset(init, 0, sizeof(*init));
    init->body = init->headers = init->signal = init->referrer = JS_UNDEFINED;
    init->cache = init->credentials = init->mode = init->redirect = init->policy = init->priority = -1;
    if (JS_IsUndefined(value) || JS_IsNull(value)) return 0;
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "RequestInit must be a dictionary");
        return -1;
    }
    JSValue v;
    int has;
    if ((has = init_member(ctx, value, "body", &init->body)) < 0) return -1;
    init->has_body = has > 0;
    if ((has = init_member(ctx, value, "cache", &v)) < 0) return -1;
    if (has) { init->cache = enum_value(ctx, v, kCaches, 6, "cache mode"); JS_FreeValue(ctx, v); if (init->cache < 0) return -1; }
    if ((has = init_member(ctx, value, "credentials", &v)) < 0) return -1;
    if (has) { init->credentials = enum_value(ctx, v, kCredentials, 3, "credentials mode"); JS_FreeValue(ctx, v); if (init->credentials < 0) return -1; }
    if ((has = init_member(ctx, value, "duplex", &v)) < 0) return -1;
    if (has) {
        static const char *const kDuplex[] = {"half"};
        int d = enum_value(ctx, v, kDuplex, 1, "duplex");
        JS_FreeValue(ctx, v);
        if (d < 0) return -1;
    }
    if ((has = init_member(ctx, value, "headers", &init->headers)) < 0) return -1;
    init->has_headers = has > 0;
    if ((has = init_member(ctx, value, "integrity", &v)) < 0) return -1;
    if (has) {
        size_t n;
        const char *text = JS_ToCStringLen(ctx, &n, v);
        JS_FreeValue(ctx, v);
        if (text == NULL) return -1;
        init->integrity = engine_dup(ctx, text, n);
        JS_FreeCString(ctx, text);
        if (init->integrity == NULL) return -1;
        init->has_integrity = true;
    }
    if ((has = init_member(ctx, value, "keepalive", &v)) < 0) return -1;
    if (has) { init->keepalive = JS_ToBool(ctx, v) > 0; init->has_keepalive = true; JS_FreeValue(ctx, v); }
    if ((has = init_member(ctx, value, "method", &v)) < 0) return -1;
    if (has) {
        char *bytes;
        size_t n;
        bool got = d_byte_string(ctx, v, &bytes, &n);
        JS_FreeValue(ctx, v);
        if (!got) return -1;
        bool ok = way_method_ok(bytes, n);
        bool forbidden = ok && way_method_forbidden(bytes, n);
        if (ok && !forbidden) os64_memcpy(init->method, bytes, n + 1);
        js_free(ctx, bytes);
        if (!ok) { JS_ThrowTypeError(ctx, "That is not a method"); return -1; }
        if (forbidden) { JS_ThrowTypeError(ctx, "That method is forbidden"); return -1; }
        way_method_normalize(init->method);
        init->has_method = true;
    }
    if ((has = init_member(ctx, value, "mode", &v)) < 0) return -1;
    if (has) {
        const char *text = JS_ToCString(ctx, v);
        bool navigate = text != NULL && os64_streq(text, "navigate");
        JS_FreeCString(ctx, text);
        init->mode = navigate ? -2 : enum_value(ctx, v, kModes, 3, "request mode");
        JS_FreeValue(ctx, v);
        if (init->mode == -1) return -1;
    }
    if ((has = init_member(ctx, value, "priority", &v)) < 0) return -1;
    if (has) { init->priority = enum_value(ctx, v, kPriorities, 3, "priority"); JS_FreeValue(ctx, v); if (init->priority < 0) return -1; }
    if ((has = init_member(ctx, value, "redirect", &v)) < 0) return -1;
    if (has) { init->redirect = enum_value(ctx, v, kRedirects, 3, "redirect mode"); JS_FreeValue(ctx, v); if (init->redirect < 0) return -1; }
    if ((has = init_member(ctx, value, "referrer", &init->referrer)) < 0) return -1;
    init->has_referrer = has > 0;
    if ((has = init_member(ctx, value, "referrerPolicy", &v)) < 0) return -1;
    if (has) {
        const char *text = JS_ToCString(ctx, v);
        bool empty = text != NULL && text[0] == '\0';
        JS_FreeCString(ctx, text);
        init->policy = empty ? -2 : enum_value(ctx, v, kPolicies, 8, "referrer policy");
        JS_FreeValue(ctx, v);
        if (init->policy == -1) return -1;
    }
    if ((has = init_member(ctx, value, "signal", &init->signal)) < 0) return -1;
    init->has_signal = has > 0;
    if ((has = init_member(ctx, value, "window", &v)) < 0) return -1;
    bool window_ok = !has || JS_IsNull(v);
    JS_FreeValue(ctx, v);
    if (!window_ok) {
        JS_ThrowTypeError(ctx, "A request's window can only be null");
        return -1;
    }
    return 0;
}

/* data: is taken as written, whatever its length (the host caps it); every
 * other address is parsed against the document's base. 1, 0 for one that
 * does not parse, -1 thrown. `*url` is engine memory. */
static int request_url(os64_dom_t *dom, JSContext *ctx, JSValueConst input, char **url)
{
    *url = NULL;
    size_t n;
    const char *text = JS_ToCStringLen(ctx, &n, input);
    if (text == NULL) return -1;
    size_t first = 0;
    while (first < n && (unsigned char)text[first] <= 0x20) first++;
    bool data = n - first >= 5 && (text[first] | 0x20) == 'd' && (text[first + 1] | 0x20) == 'a' &&
                (text[first + 2] | 0x20) == 't' && (text[first + 3] | 0x20) == 'a' && text[first + 4] == ':';
    if (data) {
        size_t end = n;
        while (end > first && (unsigned char)text[end - 1] <= 0x20) end--;
        *url = js_malloc(ctx, end - first + 1);
        if (*url != NULL) {
            os64_memcpy(*url, "data:", 5);
            os64_memcpy(*url + 5, text + first + 5, end - first - 5);
            (*url)[end - first] = '\0';
        }
        JS_FreeCString(ctx, text);
        return *url != NULL ? 1 : -1;
    }
    JS_FreeCString(ctx, text);
    char *parsed;
    int got = d_url_parse(dom, ctx, input, &parsed);
    if (got <= 0) return got;
    *url = engine_dup(ctx, parsed, os64_strlen(parsed));
    d_free(dom, parsed);
    return *url != NULL ? 1 : -1;
}

/* § 5.4 the Request constructor's steps, into `object` (made, empty). */
static int request_init(os64_dom_t *dom, JSContext *ctx, JSValueConst object, JSValueConst input, JSValueConst init_value)
{
    DReq *in = request_state(input);
    char *url = NULL;
    if (in == NULL) {
        int parsed = request_url(dom, ctx, input, &url);
        if (parsed < 0) return -1;
        if (parsed == 0) {
            JS_ThrowTypeError(ctx, "Failed to parse the URL");
            return -1;
        }
    } else {
        url = engine_dup(ctx, in->url, os64_strlen(in->url));
        if (url == NULL) return -1;
    }
    Init init;
    if (init_read(ctx, init_value, &init) < 0) {
        init_free(ctx, &init);
        js_free(ctx, url);
        return -1;
    }
    in = request_state(input);          /* reading the init ran page code */
    DReq *q = request_state(object);
    q->url = url;
    int result = 0;
    /* The request's facts: the input's, or the defaults for a new one. */
    os64_strcopy(q->method, sizeof(q->method), in != NULL ? in->method : "GET");
    q->mode = in != NULL ? in->mode : WAY_MODE_CORS;
    q->credentials = in != NULL ? in->credentials : WAY_CREDENTIALS_SAME_ORIGIN;
    q->redirect = in != NULL ? in->redirect : WAY_REDIRECT_FOLLOW;
    q->policy = in != NULL ? in->policy : WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN;
    q->policy_given = in != NULL && in->policy_given;
    q->cache = in != NULL ? in->cache : 0;
    q->keepalive = in != NULL && in->keepalive;
    q->priority = in != NULL ? in->priority : 0;
    q->referrer = engine_dup(ctx, in != NULL ? in->referrer : "about:client", os64_strlen(in != NULL ? in->referrer : "about:client"));
    q->integrity = engine_dup(ctx, in != NULL ? in->integrity : "", in != NULL ? os64_strlen(in->integrity) : 0);
    if (q->referrer == NULL || q->integrity == NULL) result = -1;
    if (result == 0 && init.has_referrer) {
        size_t n;
        const char *text = JS_ToCStringLen(ctx, &n, init.referrer);
        if (text == NULL) result = -1;
        else if (n == 0) {
            js_free(ctx, q->referrer);
            q->referrer = engine_dup(ctx, "", 0);
            JS_FreeCString(ctx, text);
            if (q->referrer == NULL) result = -1;
        } else {
            JS_FreeCString(ctx, text);
            char *parsed;
            int got = d_url_parse(dom, ctx, init.referrer, &parsed);
            if (got < 0) result = -1;
            else if (got == 0) {
                JS_ThrowTypeError(ctx, "The referrer is not an address");
                result = -1;
            } else {
                /* about:client, or an address not of this origin, is the
                 * client: the page itself. */
                char mine[WAY_ORIGIN_TEXT_MAX], theirs[WAY_ORIGIN_TEXT_MAX];
                bool same = d_origin(parsed, theirs, sizeof(theirs)) &&
                            d_origin(dom->url != NULL ? dom->url : "about:blank", mine, sizeof(mine)) &&
                            os64_streq(mine, theirs) && !os64_streq(mine, "null");
                const char *chosen = os64_streq(parsed, "about:client") || !same ? "about:client" : parsed;
                js_free(ctx, q->referrer);
                q->referrer = engine_dup(ctx, chosen, os64_strlen(chosen));
                d_free(dom, parsed);
                if (q->referrer == NULL) result = -1;
            }
        }
    }
    if (result == 0 && init.policy != -1) {
        q->policy_given = init.policy != -2;
        q->policy = init.policy >= 0 ? (way_policy_t)init.policy : WAY_POLICY_STRICT_ORIGIN_WHEN_CROSS_ORIGIN;
    }
    if (result == 0 && init.mode == -2) {
        JS_ThrowTypeError(ctx, "A request's mode cannot be navigate");
        result = -1;
    }
    if (result == 0 && init.mode >= 0) q->mode = (way_mode_t)init.mode;
    if (result == 0 && init.credentials >= 0) q->credentials = (way_credentials_t)init.credentials;
    if (result == 0 && init.cache >= 0) q->cache = init.cache;
    if (result == 0 && q->cache == 5 && q->mode != WAY_MODE_SAME_ORIGIN) {
        JS_ThrowTypeError(ctx, "only-if-cached needs mode same-origin");
        result = -1;
    }
    if (result == 0 && init.redirect >= 0) q->redirect = (way_redirect_t)init.redirect;
    if (result == 0 && init.has_integrity) {
        js_free(ctx, q->integrity);
        q->integrity = init.integrity;
        init.integrity = NULL;
    }
    if (result == 0 && init.has_keepalive) q->keepalive = init.keepalive;
    if (result == 0 && init.priority >= 0) q->priority = init.priority;
    if (result == 0 && init.has_method) os64_strcopy(q->method, sizeof(q->method), init.method);
    /* The signal: a new one, following the init's or the input's. */
    JSValueConst source = init.has_signal ? (JSValueConst)init.signal : in != NULL ? in->signal : JS_UNDEFINED;
    if (result == 0 && !JS_IsUndefined(source) && !JS_IsNull(source) && !d_signal_is(source)) {
        JS_ThrowTypeError(ctx, "A request's signal must be an AbortSignal");
        result = -1;
    }
    if (result == 0) {
        q->signal = d_signal_new(dom, ctx, JS_UNDEFINED);
        if (JS_IsException(q->signal)) {
            q->signal = JS_UNDEFINED;
            result = -1;
        } else if (!JS_IsUndefined(source) && !JS_IsNull(source) && d_signal_follow(dom, ctx, q->signal, source) < 0)
            result = -1;
    }
    /* The headers: the init's, else a copy of the input's. */
    if (result == 0) {
        JSValue prototype = JS_GetClassProto(ctx, os64_js_class_id(&headers_slot));
        q->headers = headers_make(ctx, prototype, G_REQUEST);
        JS_FreeValue(ctx, prototype);
        if (JS_IsException(q->headers)) {
            q->headers = JS_UNDEFINED;
            result = -1;
        }
    }
    if (result == 0) {
        if (q->mode == WAY_MODE_NO_CORS) {
            if (!way_method_safelisted(q->method)) {
                JS_ThrowTypeError(ctx, "A no-cors request's method must be GET, HEAD or POST");
                result = -1;
            } else headers_state(q->headers)->guard = G_REQUEST_NO_CORS;
        }
    }
    if (result == 0) {
        if (init.has_headers) result = headers_fill(dom, ctx, q->headers, init.headers);
        else if (in != NULL && !JS_IsUndefined(in->headers)) {
            /* A copy, through the guard as the standard's "fill" does. */
            DHeaders *from = headers_state(in->headers);
            for (size_t i = 0; result == 0 && from != NULL && i < from->count; i++) {
                char *name = engine_dup(ctx, from->list[i].name, from->list[i].name_length);
                char *value = name != NULL ? engine_dup(ctx, from->list[i].value, from->list[i].value_length) : NULL;
                if (value == NULL) {
                    js_free(ctx, name);
                    result = -1;
                } else result = headers_append(ctx, headers_state(q->headers), name, from->list[i].name_length,
                                               value, from->list[i].value_length);
                from = headers_state(in->headers);
            }
        }
    }
    /* The body: the init's, else the input's (which this takes, leaving
     * the input's disturbed). */
    in = request_state(input);
    bool body_given = init.has_body && !JS_IsNull(init.body);
    bool input_body = in != NULL && in->body.has;
    if (result == 0 && (body_given || input_body) &&
        (os64_streq(q->method, "GET") || os64_streq(q->method, "HEAD"))) {
        JS_ThrowTypeError(ctx, "A GET or HEAD request cannot have a body");
        result = -1;
    }
    if (result == 0 && body_given) {
        DBytes extracted;
        bool has_type;
        if (d_body_extract(dom, ctx, init.body, &extracted, &has_type) < 0) result = -1;
        else {
            q = request_state(object);
            q->body.bytes = extracted.bytes;
            q->body.length = extracted.length;
            q->body.has = true;
            DHeaders *h = headers_state(q->headers);
            if (has_type && extracted.type != NULL && !list_contains(h, "content-type", 12) &&
                headers_author(ctx, h, "Content-Type", extracted.type) < 0)
                result = -1;
            js_free(ctx, extracted.type);
        }
    } else if (result == 0 && input_body) {
        if (body_unusable(&in->body)) {
            JS_ThrowTypeError(ctx, "The input request's body has already been read");
            result = -1;
        } else {
            q->body.bytes = js_malloc(ctx, in->body.length + 1);
            if (q->body.bytes == NULL) result = -1;
            else {
                if (in->body.length != 0) os64_memcpy(q->body.bytes, in->body.bytes, in->body.length);
                q->body.length = in->body.length;
                q->body.has = true;
                in->body.used = true;
            }
        }
    }
    init_free(ctx, &init);
    return result;
}

static JSValue request_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "Request needs an input");
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(prototype)) return prototype;
    JSValue object = request_alloc(ctx, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    if (request_init(dom, ctx, object, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

enum {
    Q_METHOD, Q_URL, Q_HEADERS, Q_DESTINATION, Q_REFERRER, Q_REFERRER_POLICY, Q_MODE, Q_CREDENTIALS, Q_CACHE,
    Q_REDIRECT, Q_INTEGRITY, Q_KEEPALIVE, Q_SIGNAL, Q_DUPLEX, Q_CLONE, Q_HISTORY_NAVIGATION, Q_RELOAD_NAVIGATION,
    Q_BODY_BASE = 100,
};

/* A copy of a body for a clone, or -1. */
static int body_clone(JSContext *ctx, const DBody *from, DBody *to)
{
    to->stream = to->aborted = JS_UNDEFINED;
    to->has = from->has;
    if (!from->has) return 0;
    to->bytes = js_malloc(ctx, from->length + 1);
    if (to->bytes == NULL) return -1;
    if (from->length != 0) os64_memcpy(to->bytes, from->bytes, from->length);
    to->length = from->length;
    to->aborted = JS_DupValue(ctx, from->aborted);
    return 0;
}

static JSValue request_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)argc; (void)argv;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    DReq *q = request_state(self);
    if (q == NULL) {
        JSValue error = JS_ThrowTypeError(ctx, "Expected a Request");
        return magic >= Q_BODY_BASE && magic - Q_BODY_BASE <= BODY_BYTES ? d_promise_settled(ctx, error) : error;
    }
    if (magic >= Q_BODY_BASE) {
        int kind = magic - Q_BODY_BASE;
        if (kind == BODY_USED) return JS_NewBool(ctx, q->body.used);
        if (kind == BODY_STREAM) return body_stream(dom, ctx, self, &q->body);
        return body_consume(dom, ctx, self, &q->body, q->headers, kind);
    }
    switch (magic) {
    case Q_METHOD: return JS_NewString(ctx, q->method);
    case Q_URL: return JS_NewString(ctx, q->url);
    case Q_HEADERS: return JS_DupValue(ctx, q->headers);
    case Q_DESTINATION: return JS_NewString(ctx, "");
    case Q_REFERRER:
        if (q->referrer[0] == '\0') return JS_NewString(ctx, "");
        return JS_NewString(ctx, q->referrer);
    case Q_REFERRER_POLICY: return JS_NewString(ctx, q->policy_given ? kPolicies[q->policy] : "");
    case Q_MODE: return JS_NewString(ctx, kModes[q->mode]);
    case Q_CREDENTIALS: return JS_NewString(ctx, kCredentials[q->credentials]);
    case Q_CACHE: return JS_NewString(ctx, kCaches[q->cache]);
    case Q_REDIRECT: return JS_NewString(ctx, kRedirects[q->redirect]);
    case Q_INTEGRITY: return JS_NewString(ctx, q->integrity);
    case Q_KEEPALIVE: return JS_NewBool(ctx, q->keepalive);
    case Q_SIGNAL: return JS_DupValue(ctx, q->signal);
    case Q_DUPLEX: return JS_NewString(ctx, "half");
    case Q_HISTORY_NAVIGATION: case Q_RELOAD_NAVIGATION: return JS_FALSE;
    case Q_CLONE: {
        if (body_unusable(&q->body)) return JS_ThrowTypeError(ctx, "The request's body has already been read");
        JSValue prototype = JS_GetPrototype(ctx, self);
        if (JS_IsException(prototype)) return prototype;
        JSValue object = request_alloc(ctx, prototype);
        JS_FreeValue(ctx, prototype);
        if (JS_IsException(object)) return object;
        DReq *c = request_state(object);
        q = request_state(self);
        *c = *q;
        c->body = (DBody){0};
        c->url = c->referrer = c->integrity = NULL;
        c->headers = c->signal = JS_UNDEFINED;
        int result = body_clone(ctx, &q->body, &c->body);
        c->url = result == 0 ? engine_dup(ctx, q->url, os64_strlen(q->url)) : NULL;
        c->referrer = c->url != NULL ? engine_dup(ctx, q->referrer, os64_strlen(q->referrer)) : NULL;
        c->integrity = c->referrer != NULL ? engine_dup(ctx, q->integrity, os64_strlen(q->integrity)) : NULL;
        if (c->integrity == NULL) result = -1;
        if (result == 0) {
            JSValue hp = JS_GetClassProto(ctx, os64_js_class_id(&headers_slot));
            c->headers = headers_make(ctx, hp, headers_state(q->headers)->guard);
            JS_FreeValue(ctx, hp);
            if (JS_IsException(c->headers)) {
                c->headers = JS_UNDEFINED;
                result = -1;
            } else result = headers_copy(ctx, headers_state(c->headers), headers_state(q->headers));
        }
        if (result == 0) {
            c->signal = d_signal_new(dom, ctx, JS_UNDEFINED);
            if (JS_IsException(c->signal)) {
                c->signal = JS_UNDEFINED;
                result = -1;
            } else result = d_signal_follow(dom, ctx, c->signal, q->signal);
        }
        if (result < 0) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        return object;
    }
    default: return JS_UNDEFINED;
    }
}

/* ── Response ────────────────────────────────────────────────────────── */

static JSValue response_alloc(JSContext *ctx, JSValueConst prototype)
{
    JSClassID id = os64_js_class_id(&response_slot);
    JSValue object = JS_IsObject(prototype) ? JS_NewObjectProtoClass(ctx, prototype, id)
                                            : JS_NewObjectClass(ctx, (int)id);
    if (JS_IsException(object)) return object;
    DResp *r = js_mallocz(ctx, sizeof(*r));
    char *text = r != NULL ? engine_dup(ctx, "", 0) : NULL;
    char *url = text != NULL ? engine_dup(ctx, "", 0) : NULL;
    if (url == NULL) {
        js_free(ctx, text);
        js_free(ctx, r);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    r->body.stream = r->body.aborted = r->headers = JS_UNDEFINED;
    r->status_text = text;
    r->url = url;
    r->type = WAY_TYPE_DEFAULT;
    r->status = 200;
    JS_SetOpaque(object, r);
    JSValue hp = JS_GetClassProto(ctx, os64_js_class_id(&headers_slot));
    r->headers = headers_make(ctx, hp, G_RESPONSE);
    JS_FreeValue(ctx, hp);
    if (JS_IsException(r->headers)) {
        r->headers = JS_UNDEFINED;
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static bool null_body_status(int32_t status)
{
    return status == 101 || status == 103 || status == 204 || status == 205 || status == 304;
}

/* § 5.5 "initialize a response": status, statusText, headers, then the
 * body with its type. */
static int response_initialize(os64_dom_t *dom, JSContext *ctx, JSValueConst object, JSValueConst init, DBytes *body,
                               bool has_body, bool has_type)
{
    int32_t status = 200;
    char *text = NULL;
    size_t text_length = 0;
    JSValue headers = JS_UNDEFINED;
    if (!JS_IsUndefined(init) && !JS_IsNull(init) && !JS_IsObject(init)) {
        JS_ThrowTypeError(ctx, "ResponseInit must be a dictionary");
        return -1;
    }
    if (JS_IsObject(init)) {
        if (init_member(ctx, init, "headers", &headers) < 0) return -1;
        JSValue v;
        int has = init_member(ctx, init, "status", &v);
        if (has < 0) { JS_FreeValue(ctx, headers); return -1; }
        if (has) {
            /* [EnforceRange] unsigned short. */
            double number;
            int got = JS_ToFloat64(ctx, &number, v);
            JS_FreeValue(ctx, v);
            if (got < 0) { JS_FreeValue(ctx, headers); return -1; }
            if (number != number || number < 0 || number > 65535) {
                JS_FreeValue(ctx, headers);
                JS_ThrowTypeError(ctx, "A status is a number from 0 to 65535");
                return -1;
            }
            status = (int32_t)number;
        }
        has = init_member(ctx, init, "statusText", &v);
        if (has < 0) { JS_FreeValue(ctx, headers); return -1; }
        if (has) {
            bool got = d_byte_string(ctx, v, &text, &text_length);
            JS_FreeValue(ctx, v);
            if (!got) { JS_FreeValue(ctx, headers); return -1; }
        }
    }
    int result = 0;
    if (status < 200 || status > 599) {
        JS_ThrowRangeError(ctx, "A response's status is from 200 to 599");
        result = -1;
    }
    /* reason-phrase: HTAB, SP, VCHAR and obs-text. */
    for (size_t i = 0; result == 0 && text != NULL && i < text_length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!(c == '\t' || c == ' ' || (c >= 0x21 && c <= 0x7e) || c >= 0x80)) {
            JS_ThrowTypeError(ctx, "That is not a reason phrase");
            result = -1;
        }
    }
    DResp *r = response_state(object);
    if (result == 0) {
        r->status = status;
        if (text != NULL) {
            js_free(ctx, r->status_text);
            r->status_text = text;
            text = NULL;
        }
        if (!JS_IsUndefined(headers)) result = headers_fill(dom, ctx, r->headers, headers);
    }
    r = response_state(object);
    if (result == 0 && has_body) {
        if (null_body_status(r->status)) {
            JS_ThrowTypeError(ctx, "A response with a null body status cannot have a body");
            result = -1;
        } else {
            r->body.bytes = body->bytes;
            r->body.length = body->length;
            r->body.has = true;
            body->bytes = NULL;
            DHeaders *h = headers_state(r->headers);
            if (has_type && body->type != NULL && !list_contains(h, "content-type", 12) &&
                headers_author(ctx, h, "Content-Type", body->type) < 0)
                result = -1;
        }
    }
    js_free(ctx, text);
    JS_FreeValue(ctx, headers);
    return result;
}

static JSValue response_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    os64_dom_t *dom = d_binding(ctx);
    if (dom == NULL) return JS_EXCEPTION;
    JSValueConst body = argc > 0 ? argv[0] : JS_UNDEFINED;
    DBytes extracted = {0};
    bool has_type = false, has_body = !JS_IsUndefined(body) && !JS_IsNull(body);
    if (has_body && d_body_extract(dom, ctx, body, &extracted, &has_type) < 0) return JS_EXCEPTION;
    JSValue prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue object = JS_IsException(prototype) ? JS_EXCEPTION : response_alloc(ctx, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object) ||
        response_initialize(dom, ctx, object, argc > 1 ? argv[1] : JS_UNDEFINED, &extracted, has_body, has_type) < 0) {
        d_bytes_free(ctx, &extracted);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    d_bytes_free(ctx, &extracted);
    return object;
}

enum {
    R_TYPE, R_URL, R_REDIRECTED, R_STATUS, R_OK, R_STATUS_TEXT, R_HEADERS, R_CLONE, R_ERROR_STATIC,
    R_REDIRECT_STATIC, R_JSON_STATIC, R_BODY_BASE = 100,
};

static JSValue response_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    if (magic == R_ERROR_STATIC || magic == R_REDIRECT_STATIC || magic == R_JSON_STATIC) {
        JSValue prototype = JS_GetClassProto(ctx, os64_js_class_id(&response_slot));
        JSValue object = response_alloc(ctx, prototype);
        JS_FreeValue(ctx, prototype);
        if (JS_IsException(object)) return object;
        DResp *r = response_state(object);
        if (magic == R_ERROR_STATIC) {
            r->type = WAY_TYPE_ERROR;
            r->status = 0;
            headers_state(r->headers)->guard = G_IMMUTABLE;
            return object;
        }
        if (magic == R_REDIRECT_STATIC) {
            char *url = NULL;
            int parsed = argc > 0 ? d_url_parse(dom, ctx, argv[0], &url) : 0;
            int32_t status = 302;
            if (parsed > 0 && argc > 1 && !JS_IsUndefined(argv[1])) {
                double number;
                if (JS_ToFloat64(ctx, &number, argv[1]) < 0) parsed = -1;
                else status = number == number && number >= 0 && number <= 65535 ? (int32_t)number : -1;
            }
            if (parsed <= 0 || !way_redirect_status(status)) {
                d_free(dom, url);
                JS_FreeValue(ctx, object);
                if (parsed < 0) return JS_EXCEPTION;
                if (parsed == 0) return JS_ThrowTypeError(ctx, "Failed to parse the URL");
                return JS_ThrowRangeError(ctx, "A redirect's status is 301, 302, 303, 307 or 308");
            }
            r = response_state(object);
            r->status = status;
            DHeaders *h = headers_state(r->headers);
            int added = headers_author(ctx, h, "Location", url);
            d_free(dom, url);
            if (added < 0) {
                JS_FreeValue(ctx, object);
                return JS_EXCEPTION;
            }
            h->guard = G_IMMUTABLE;
            return object;
        }
        /* Response.json(data, init): data serialized by JSON.stringify. */
        JSValue text = JS_JSONStringify(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(text)) {
            JS_FreeValue(ctx, object);
            return text;
        }
        if (JS_IsUndefined(text)) {
            JS_FreeValue(ctx, object);
            return JS_ThrowTypeError(ctx, "The data cannot be written as JSON");
        }
        DBytes body;
        bool has_type;
        int extracted = d_body_extract(dom, ctx, text, &body, &has_type);
        JS_FreeValue(ctx, text);
        if (extracted < 0) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        js_free(ctx, body.type);
        body.type = engine_dup(ctx, "application/json", 16);
        if (body.type == NULL ||
            response_initialize(dom, ctx, object, argc > 1 ? argv[1] : JS_UNDEFINED, &body, true, true) < 0) {
            d_bytes_free(ctx, &body);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        d_bytes_free(ctx, &body);
        return object;
    }
    DResp *r = response_state(self);
    if (r == NULL) {
        JSValue error = JS_ThrowTypeError(ctx, "Expected a Response");
        return magic >= R_BODY_BASE && magic - R_BODY_BASE <= BODY_BYTES ? d_promise_settled(ctx, error) : error;
    }
    if (magic >= R_BODY_BASE) {
        int kind = magic - R_BODY_BASE;
        if (kind == BODY_USED) return JS_NewBool(ctx, r->body.used);
        if (kind == BODY_STREAM) return body_stream(dom, ctx, self, &r->body);
        return body_consume(dom, ctx, self, &r->body, r->headers, kind);
    }
    switch (magic) {
    case R_TYPE: return JS_NewString(ctx, way_type_name(r->type));
    case R_URL: return JS_NewString(ctx, r->url);
    case R_REDIRECTED: return JS_NewBool(ctx, r->redirected);
    case R_STATUS: return JS_NewInt32(ctx, r->status);
    case R_OK: return JS_NewBool(ctx, r->status >= 200 && r->status <= 299);
    case R_STATUS_TEXT: return d_byte_text(ctx, r->status_text, os64_strlen(r->status_text));
    case R_HEADERS: return JS_DupValue(ctx, r->headers);
    case R_CLONE: {
        if (body_unusable(&r->body)) return JS_ThrowTypeError(ctx, "The response's body has already been read");
        JSValue prototype = JS_GetPrototype(ctx, self);
        if (JS_IsException(prototype)) return prototype;
        JSValue object = response_alloc(ctx, prototype);
        JS_FreeValue(ctx, prototype);
        if (JS_IsException(object)) return object;
        DResp *c = response_state(object);
        r = response_state(self);
        c->type = r->type;
        c->status = r->status;
        c->redirected = r->redirected;
        int result = body_clone(ctx, &r->body, &c->body);
        char *text = result == 0 ? engine_dup(ctx, r->status_text, os64_strlen(r->status_text)) : NULL;
        char *url = text != NULL ? engine_dup(ctx, r->url, os64_strlen(r->url)) : NULL;
        if (url == NULL) {
            js_free(ctx, text);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        js_free(ctx, c->status_text);
        js_free(ctx, c->url);
        c->status_text = text;
        c->url = url;
        DHeaders *from = headers_state(r->headers), *to = headers_state(c->headers);
        if (headers_copy(ctx, to, from) < 0) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        to->guard = from->guard;
        return object;
    }
    default: return JS_UNDEFINED;
    }
}

/* ── fetch() ─────────────────────────────────────────────────────────── */

/* held[0] resolve, held[1] reject, held[2] the request object. */
enum { FETCH_RESOLVE, FETCH_REJECT, FETCH_REQUEST };

/* A network answer as the page's Response (engine memory: a body the
 * runtime has no room for fails here, as any allocation does). */
static JSValue response_from_network(os64_dom_t *dom, JSContext *ctx, const os64_dom_response_t *in)
{
    JSValue prototype = JS_GetClassProto(ctx, os64_js_class_id(&response_slot));
    JSValue object = response_alloc(ctx, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(object)) return object;
    DResp *r = response_state(object);
    r->type = in->type;
    r->status = in->status;
    r->redirected = in->redirected;
    char *text = engine_dup(ctx, in->status_text != NULL ? in->status_text : "",
                            in->status_text != NULL ? os64_strlen(in->status_text) : 0);
    char *url = text != NULL ? engine_dup(ctx, in->url != NULL ? in->url : "", in->url != NULL ? os64_strlen(in->url) : 0) : NULL;
    if (url == NULL) {
        js_free(ctx, text);
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    js_free(ctx, r->status_text);
    js_free(ctx, r->url);
    r->status_text = text;
    r->url = url;
    JSValue headers = d_headers_from_block(dom, ctx, in->headers != NULL ? in->headers : "", in->headers_length, G_IMMUTABLE);
    if (JS_IsException(headers)) {
        JS_FreeValue(ctx, object);
        return headers;
    }
    r = response_state(object);
    JS_FreeValue(ctx, r->headers);
    r->headers = headers;
    if (in->has_body) {
        r->body.bytes = js_malloc(ctx, in->body_length + 1);
        if (r->body.bytes == NULL) {
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        if (in->body_length != 0) os64_memcpy(r->body.bytes, in->body, in->body_length);
        r->body.length = in->body_length;
        r->body.has = true;
    }
    return object;
}

/* Settles a fetch's promise inside the host's task, through the wrapper's
 * own call so its budget and guard apply. */
static void settle(os64_dom_t *dom, JSValueConst function, JSValue value, os64_js_outcome_t *outcome)
{
    os64_js_outcome_t call;
    os64_js_call(dom->runtime, OS64_JS_ABI_ID, function, JS_UNDEFINED, 1, &value, NULL, &call);
    d_outcome_fold(outcome, &call);
}

/* The fetch is ended before its promise settles: what settling runs (an
 * abort among it) then finds an answered fetch, never one still out, and
 * nothing it does can free what the call is using. */
static void fetch_answered(os64_dom_t *dom, JSContext *ctx, DRequest *request, const os64_dom_response_t *response,
                           os64_js_outcome_t *outcome)
{
    uint32_t token = request->token;
    JSValue resolve = JS_DupValue(ctx, request->held[FETCH_RESOLVE]);
    JSValue reject = JS_DupValue(ctx, request->held[FETCH_REJECT]);
    JSValue object = JS_DupValue(ctx, request->held[FETCH_REQUEST]);
    d_request_end(dom, request, false);
    DReq *q = request_state(object);
    JSValue value;
    bool ok = response->ok;
    if (!ok) {
        JS_ThrowTypeError(ctx, "Failed to fetch%s%s", response->why != NULL ? ": " : "",
                          response->why != NULL ? response->why : "");
        value = JS_GetException(ctx);
    } else {
        value = response_from_network(dom, ctx, response);
        if (JS_IsException(value)) {
            ok = false;
            value = JS_GetException(ctx);
        } else if (q != NULL) d_signal_fetch_answered(ctx, q->signal, token, value);
    }
    settle(dom, ok ? resolve : reject, value, outcome);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, reject);
    JS_FreeValue(ctx, object);
}

void d_fetch_abort(os64_dom_t *dom, JSContext *ctx, uint32_t token, JSValueConst response, JSValueConst reason)
{
    DRequest *request = d_request_find(dom, token);
    if (request != NULL) {
        /* Still out: the promise rejects with the reason, and the host
         * stops. */
        JSValue done = JS_Call(ctx, request->held[FETCH_REJECT], JS_UNDEFINED, 1, &reason);
        JS_FreeValue(ctx, done);
        if (JS_IsException(done)) JS_FreeValue(ctx, JS_GetException(ctx));
        d_request_end(dom, request, true);
        return;
    }
    /* Answered: a body not yet read is read as the reason; an empty one
     * closed before it could error, and reads as empty. */
    DResp *r = response_state(response);
    if (r != NULL && JS_IsUndefined(r->body.aborted) && r->body.has && r->body.length != 0 && !r->body.used) {
        r->body.aborted = JS_DupValue(ctx, reason);
        DStream *s = JS_IsUndefined(r->body.stream) ? NULL : stream_state(r->body.stream);
        if (s != NULL && s->state == ST_READABLE && !s->delivered) {
            s->state = ST_ERRORED;
            JS_FreeValue(ctx, s->error);
            s->error = JS_DupValue(ctx, reason);
            DReader *rd = JS_IsUndefined(s->reader) ? NULL : reader_state(s->reader);
            if (rd != NULL) reader_closed(ctx, rd, reason);
        }
    }
}

bool d_response_body_settled(JSValueConst response)
{
    DResp *r = response_state(response);
    return r == NULL || !r->body.has || r->body.used || !JS_IsUndefined(r->body.aborted);
}

/* The request as the host takes it. */
static void host_request(const DReq *q, const char *headers, size_t headers_length, const char *referrer,
                         os64_dom_request_t *out)
{
    *out = (os64_dom_request_t){
        .url = q->url, .method = q->method, .headers = headers, .headers_length = headers_length,
        .body = q->body.has ? (const void *)(q->body.bytes != NULL ? q->body.bytes : (const uint8_t *)"") : NULL,
        .body_length = q->body.has ? q->body.length : 0,
        .mode = q->mode, .credentials = q->credentials, .redirect = q->redirect, .policy = q->policy,
        .preflight = false, .referrer = referrer,
    };
}

static JSValue fetch_function(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data)
{
    (void)self; (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL) return JS_EXCEPTION;
    /* § 5.6: a Request made from the arguments, as `new Request` makes
     * one; whatever that throws rejects. */
    JSValue prototype = JS_GetClassProto(ctx, os64_js_class_id(&request_slot));
    JSValue request = request_alloc(ctx, prototype);
    JS_FreeValue(ctx, prototype);
    if (JS_IsException(request)) return d_promise_settled(ctx, request);
    if (argc < 1) {
        JS_FreeValue(ctx, request);
        return d_promise_settled(ctx, JS_ThrowTypeError(ctx, "fetch needs an input"));
    }
    if (request_init(dom, ctx, request, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED) < 0) {
        JS_FreeValue(ctx, request);
        return d_promise_settled(ctx, JS_EXCEPTION);
    }
    DReq *q = request_state(request);
    JSValue reason;
    if (d_signal_aborted(q->signal, &reason, ctx)) {
        JS_FreeValue(ctx, request);
        return d_promise_settled(ctx, JS_Throw(ctx, reason));
    }
    /* Subresource Integrity needs a digest this library does not carry:
     * a request that asks for it fails rather than go unchecked. */
    if (q->integrity[0] != '\0') {
        JS_FreeValue(ctx, request);
        return d_promise_settled(ctx, JS_ThrowTypeError(ctx, "Failed to fetch: integrity metadata is not supported"));
    }
    if (q->keepalive && q->body.has && q->body.length > 64 * 1024) {
        JS_FreeValue(ctx, request);
        return d_promise_settled(ctx, JS_ThrowTypeError(ctx, "Failed to fetch: a keepalive body is past 64 KiB"));
    }
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) {
        JS_FreeValue(ctx, request);
        return promise;
    }
    size_t headers_length;
    char *headers = headers_block(ctx, headers_state(q->headers), &headers_length);
    if (headers == NULL) {
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, promise);
        JS_FreeValue(ctx, request);
        return d_promise_settled(ctx, JS_EXCEPTION);
    }
    /* The referrer: the page's address for "about:client", none for "". */
    const char *referrer = os64_streq(q->referrer, "about:client") ? (dom->url != NULL ? dom->url : "") : q->referrer;
    os64_dom_request_t host;
    host_request(q, headers, headers_length, referrer, &host);
    DRequest *out = d_request_start(dom, ctx, &host, fetch_answered);
    js_free(ctx, headers);
    if (out == NULL) {
        JSValue error = JS_GetException(ctx);
        JSValue done = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &error);
        JS_FreeValue(ctx, done);
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, request);
        return promise;
    }
    request_hold(dom, out, FETCH_RESOLVE, resolving[0]);
    request_hold(dom, out, FETCH_REJECT, resolving[1]);
    request_hold(dom, out, FETCH_REQUEST, JS_DupValue(ctx, request));
    if (d_signal_add_fetch(ctx, q->signal, out->token) < 0) {
        d_request_end(dom, out, true);
        JS_FreeValue(ctx, request);
        JS_FreeValue(ctx, promise);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, request);
    return promise;
}

os64_js_status_t os64_dom_fetched(os64_dom_t *dom, uint32_t token, const os64_dom_response_t *response,
                                  os64_js_outcome_t *outcome)
{
    if (outcome == NULL) return OS64_JS_BAD_ARGUMENT;
    os64_memset(outcome, 0, sizeof(*outcome));
    if (dom == NULL || response == NULL) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return outcome->status;
    }
    if (dom->closed) {
        outcome->status = OS64_JS_FAILED_RUNTIME;
        return outcome->status;
    }
    DRequest *request = d_request_find(dom, token);
    if (request == NULL) return OS64_JS_OK;
    os64_js_outcome_t begin;
    if (os64_js_task_begin(dom->runtime, OS64_JS_ABI_ID, "fetch", &begin) != OS64_JS_OK) {
        *outcome = begin;
        return outcome->status;
    }
    os64_js_outcome_t access;
    JSContext *ctx = os64_js_context(dom->runtime, OS64_JS_ABI_ID, &access);
    if (ctx != NULL) {
        request->answer(dom, ctx, request, response, outcome);
        /* Answered is ended; an answer may have ended it itself, so it is
         * looked up again, by its token. */
        request = d_request_find(dom, token);
        if (request != NULL) d_request_end(dom, request, false);
        if (JS_HasException(ctx)) {
            JSValue error = JS_GetException(ctx);
            d_report(dom, ctx, error);
            JS_FreeValue(ctx, error);
        }
    }
    return d_task_finish(dom, outcome);
}

/* ── Installation ────────────────────────────────────────────────────── */

typedef struct { const char *name; int magic, argc; bool accessor; } Member;

int d_iterator_next(os64_dom_t *dom, JSContext *ctx, JSValueConst prototype, JSCFunctionData *fn, int magic)
{
    JSValue function = JS_NewCFunctionData(ctx, fn, 0, magic, 1, &dom->anchor->value);
    if (JS_IsException(function)) return -1;
    return JS_DefinePropertyValueStr(ctx, prototype, "next", function, JS_PROP_C_W_E);
}

static int methods(os64_dom_t *dom, JSContext *ctx, JSValueConst target, JSCFunctionData *fn, const Member *list,
                   size_t count)
{
    for (size_t i = 0; i < count; i++)
        if ((list[i].accessor ? d_accessor(dom, ctx, target, list[i].name, fn, list[i].magic, false)
                              : d_method(dom, ctx, target, list[i].name, fn, list[i].argc, list[i].magic)) < 0)
            return -1;
    return 0;
}

static int class_of(os64_dom_t *dom, JSClassID id, const JSClassDef *definition)
{
    return JS_IsRegisteredClass(dom->engine, id) || JS_NewClass(dom->engine, id, definition) >= 0 ? 0 : -1;
}

int d_fetch_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    static const JSClassDef headers_class = {.class_name = "Headers", .finalizer = headers_finalizer};
    static const JSClassDef iterator_class = {.class_name = "Headers Iterator", .finalizer = headers_iterator_finalizer,
                                              .gc_mark = headers_iterator_mark};
    static const JSClassDef request_class = {.class_name = "Request", .finalizer = request_finalizer, .gc_mark = request_mark};
    static const JSClassDef response_class = {.class_name = "Response", .finalizer = response_finalizer,
                                              .gc_mark = response_mark};
    static const JSClassDef stream_class = {.class_name = "ReadableStream", .finalizer = stream_finalizer,
                                            .gc_mark = stream_mark};
    static const JSClassDef reader_class = {.class_name = "ReadableStreamDefaultReader", .finalizer = reader_finalizer,
                                            .gc_mark = reader_mark};
    JSClassID ids[6] = {os64_js_class_id(&headers_slot), os64_js_class_id(&headers_iterator_slot),
                        os64_js_class_id(&request_slot), os64_js_class_id(&response_slot),
                        os64_js_class_id(&stream_slot), os64_js_class_id(&reader_slot)};
    const JSClassDef *defs[6] = {&headers_class, &iterator_class, &request_class, &response_class, &stream_class, &reader_class};
    DValue *protos[6];
    JSValue global_iterator = JS_GetPropertyStr(ctx, global, "Iterator");
    JSValue iterator_parent = JS_IsException(global_iterator) ? JS_EXCEPTION
                            : JS_GetPropertyStr(ctx, global_iterator, "prototype");
    JS_FreeValue(ctx, global_iterator);
    if (JS_IsException(iterator_parent)) return -1;
    for (int i = 0; i < 6; i++) {
        if (class_of(dom, ids[i], defs[i]) < 0) {
            JS_FreeValue(ctx, iterator_parent);
            return -1;
        }
        protos[i] = d_retain(dom, ctx, i == 1 ? JS_NewObjectProto(ctx, iterator_parent) : JS_NewObject(ctx));
        if (protos[i] == NULL) {
            JS_FreeValue(ctx, iterator_parent);
            return -1;
        }
        JS_SetClassProto(ctx, ids[i], JS_DupValue(ctx, protos[i]->value));
    }
    JS_FreeValue(ctx, iterator_parent);
    static const Member kHeaders[] = {
        {"append", H_APPEND, 2, false}, {"delete", H_DELETE, 1, false}, {"get", H_GET, 1, false},
        {"getSetCookie", H_GET_SET_COOKIE, 0, false}, {"has", H_HAS, 1, false}, {"set", H_SET, 2, false},
        {"entries", H_ENTRIES, 0, false}, {"keys", H_KEYS, 0, false}, {"values", H_VALUES, 0, false},
        {"forEach", H_FOR_EACH, 1, false},
    };
    static const Member kBody[] = {
        {"text", BODY_TEXT, 0, false}, {"json", BODY_JSON, 0, false}, {"arrayBuffer", BODY_ARRAY_BUFFER, 0, false},
        {"blob", BODY_BLOB, 0, false}, {"formData", BODY_FORM_DATA, 0, false}, {"bytes", BODY_BYTES, 0, false},
        {"bodyUsed", BODY_USED, 0, true}, {"body", BODY_STREAM, 0, true},
    };
    static const Member kRequest[] = {
        {"method", Q_METHOD, 0, true}, {"url", Q_URL, 0, true}, {"headers", Q_HEADERS, 0, true},
        {"destination", Q_DESTINATION, 0, true}, {"referrer", Q_REFERRER, 0, true},
        {"referrerPolicy", Q_REFERRER_POLICY, 0, true}, {"mode", Q_MODE, 0, true},
        {"credentials", Q_CREDENTIALS, 0, true}, {"cache", Q_CACHE, 0, true}, {"redirect", Q_REDIRECT, 0, true},
        {"integrity", Q_INTEGRITY, 0, true}, {"keepalive", Q_KEEPALIVE, 0, true}, {"signal", Q_SIGNAL, 0, true},
        {"duplex", Q_DUPLEX, 0, true}, {"isHistoryNavigation", Q_HISTORY_NAVIGATION, 0, true},
        {"isReloadNavigation", Q_RELOAD_NAVIGATION, 0, true}, {"clone", Q_CLONE, 0, false},
    };
    static const Member kResponse[] = {
        {"type", R_TYPE, 0, true}, {"url", R_URL, 0, true}, {"redirected", R_REDIRECTED, 0, true},
        {"status", R_STATUS, 0, true}, {"ok", R_OK, 0, true}, {"statusText", R_STATUS_TEXT, 0, true},
        {"headers", R_HEADERS, 0, true}, {"clone", R_CLONE, 0, false},
    };
    static const Member kStream[] = {
        {"locked", S_LOCKED, 0, true}, {"getReader", S_GET_READER, 0, false}, {"cancel", S_CANCEL, 0, false},
        {"tee", S_TEE, 0, false},
    };
    static const Member kReader[] = {
        {"read", R_READ, 0, false}, {"releaseLock", R_RELEASE, 0, false}, {"closed", R_CLOSED, 0, true},
        {"cancel", R_CANCEL, 0, false},
    };
    Member body_for_request[8], body_for_response[8];
    for (int i = 0; i < 8; i++) {
        body_for_request[i] = kBody[i];
        body_for_request[i].magic += Q_BODY_BASE;
        body_for_response[i] = kBody[i];
        body_for_response[i].magic += R_BODY_BASE;
    }
    if (methods(dom, ctx, protos[0]->value, headers_member, kHeaders, 10) < 0 ||
        d_iterator_next(dom, ctx, protos[1]->value, headers_member, H_NEXT) < 0 ||
        methods(dom, ctx, protos[2]->value, request_member, kRequest, 17) < 0 ||
        methods(dom, ctx, protos[2]->value, request_member, body_for_request, 8) < 0 ||
        methods(dom, ctx, protos[3]->value, response_member, kResponse, 8) < 0 ||
        methods(dom, ctx, protos[3]->value, response_member, body_for_response, 8) < 0 ||
        methods(dom, ctx, protos[4]->value, stream_member, kStream, 4) < 0 ||
        methods(dom, ctx, protos[5]->value, stream_member, kReader, 4) < 0)
        return -1;
    /* Headers' [Symbol.iterator] is entries; the iterator and the stream's
     * objects say what they are. */
    JSAtom iterator_atom = JS_ValueToAtom(ctx, dom->iterator_symbol->value);
    JSValue entries = iterator_atom == JS_ATOM_NULL ? JS_EXCEPTION : JS_GetPropertyStr(ctx, protos[0]->value, "entries");
    int linked = JS_IsException(entries) ? -1
        : JS_DefinePropertyValue(ctx, protos[0]->value, iterator_atom, entries, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    if (iterator_atom != JS_ATOM_NULL) JS_FreeAtom(ctx, iterator_atom);
    if (linked < 0 || d_string_tag(ctx, protos[1]->value, "Headers Iterator") < 0 ||
        d_string_tag(ctx, protos[4]->value, "ReadableStream") < 0 ||
        d_string_tag(ctx, protos[5]->value, "ReadableStreamDefaultReader") < 0)
        return -1;
    /* ReadableStream is no global: a stream here is only a body's, and a
     * `new ReadableStream(source)` this browser cannot honour would be a
     * name a page detects and then finds broken. */
    static const struct { const char *name; int length; JSCFunction *constructor; int proto; } kInterfaces[] = {
        {"Headers", 0, headers_constructor, 0}, {"Request", 1, request_constructor, 2},
        {"Response", 0, response_constructor, 3},
    };
    JSValue response_interface = JS_UNDEFINED;
    for (size_t i = 0; i < sizeof(kInterfaces) / sizeof(kInterfaces[0]); i++) {
        JSValue interface = d_interface_new(ctx, kInterfaces[i].name, kInterfaces[i].length, kInterfaces[i].constructor,
                                            protos[kInterfaces[i].proto]->value, JS_UNDEFINED);
        if (JS_IsException(interface)) return -1;
        if (kInterfaces[i].proto == 3) response_interface = JS_DupValue(ctx, interface);
        if (JS_DefinePropertyValueStr(ctx, global, kInterfaces[i].name, interface, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
            JS_FreeValue(ctx, response_interface);
            return -1;
        }
    }
    bool statics = d_method(dom, ctx, response_interface, "error", response_member, 0, R_ERROR_STATIC) >= 0 &&
                   d_method(dom, ctx, response_interface, "redirect", response_member, 1, R_REDIRECT_STATIC) >= 0 &&
                   d_method(dom, ctx, response_interface, "json", response_member, 1, R_JSON_STATIC) >= 0;
    JS_FreeValue(ctx, response_interface);
    if (!statics) return -1;
    return d_method(dom, ctx, global, "fetch", fetch_function, 1, 0);
}
