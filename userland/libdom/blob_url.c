/* blob_url.c — a page's blob URL store, and URL.createObjectURL and
 * URL.revokeObjectURL over it (BLOB_URLS.md; File API § 9).
 *
 * The store is the host's, one a page (os64_dom_options_t.blobs); this
 * file names and revokes its entries for the page's scripts, and the
 * host's doors take them (dom.h § BLOB URLS). A map from the URL's text to
 * an entry, open addressed with linear probing and backward-shift
 * deletion, so no slot is ever a tombstone. An entry is one allocation:
 * its header, its URL, its type and its bytes, copied out of the engine
 * when it is named and never written again, which is what lets a pool
 * thread read them without a lock. Its count is atomic; the map holds one
 * reference, each holder another, and the last release frees the block on
 * whichever thread makes it. The lock guards the map and the totals:
 * every map operation is the owner thread's today, and W1's workers will
 * name entries from theirs (WORKERS.md § 9). */

#include "internal.h"
#include "os64/lock.h"
#include "os64/mem.h"
#include "os64/str.h"

struct os64_dom_blob {
    uint32_t refs;
    uint64_t hash;
    const char *url;
    const char *type;
    const uint8_t *bytes;
    size_t length;
};

struct os64_dom_blob_store {
    os64_lock_t lock;
    os64_dom_blob_t **slots;
    size_t cap, count;          /* cap a power of two, or 0 before the first entry */
    size_t bytes, max, refused;
};

#define BLOB_FIRST_CAP 16

/* FNV-1a over the URL up to its fragment, which a take ignores and a
 * name never has. */
static uint64_t url_hash(const char *url, size_t length)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; i++) {
        h ^= (uint8_t)url[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static size_t without_fragment(const char *url)
{
    size_t n = 0;
    while (url[n] != '\0' && url[n] != '#')
        n++;
    return n;
}

static bool url_is(const os64_dom_blob_t *b, uint64_t hash, const char *url, size_t length)
{
    return b->hash == hash && os64_strlen(b->url) == length && os64_memcmp(b->url, url, length) == 0;
}

/* The slot holding `url`, or the empty one where it would go. */
static size_t slot_of(const os64_dom_blob_store_t *s, uint64_t hash, const char *url, size_t length)
{
    size_t mask = s->cap - 1, at = (size_t)hash & mask;
    while (s->slots[at] != NULL && !url_is(s->slots[at], hash, url, length))
        at = (at + 1) & mask;
    return at;
}

static bool grow(os64_dom_blob_store_t *s)
{
    size_t cap = s->cap == 0 ? BLOB_FIRST_CAP : s->cap * 2;
    os64_dom_blob_t **slots = cap > s->cap ? os64_calloc(cap, sizeof(*slots)) : NULL;
    if (slots == NULL)
        return false;
    for (size_t i = 0; i < s->cap; i++) {
        os64_dom_blob_t *b = s->slots[i];
        if (b == NULL)
            continue;
        size_t at = (size_t)b->hash & (cap - 1);
        while (slots[at] != NULL)
            at = (at + 1) & (cap - 1);
        slots[at] = b;
    }
    os64_free(s->slots);
    s->slots = slots;
    s->cap = cap;
    return true;
}

/* Empties slot `at`, moving back each entry after it whose probe passed
 * through the hole (backward-shift deletion). */
static void remove_at(os64_dom_blob_store_t *s, size_t at)
{
    size_t mask = s->cap - 1, hole = at, next = (at + 1) & mask;
    s->slots[hole] = NULL;
    while (s->slots[next] != NULL) {
        size_t home = (size_t)s->slots[next]->hash & mask;
        /* It may fill the hole when its home is not in (hole, next]. */
        bool stays = hole <= next ? (home > hole && home <= next) : (home > hole || home <= next);
        if (!stays) {
            s->slots[hole] = s->slots[next];
            s->slots[next] = NULL;
            hole = next;
        }
        next = (next + 1) & mask;
    }
    s->count--;
}

os64_dom_blob_store_t *os64_dom_blob_store_new(size_t max)
{
    os64_dom_blob_store_t *s = os64_calloc(1, sizeof(*s));
    if (s != NULL)
        s->max = max != 0 ? max : OS64_DOM_BLOB_STORE_MAX;
    return s;
}

void os64_dom_blob_store_free(os64_dom_blob_store_t *store)
{
    if (store == NULL)
        return;
    for (size_t i = 0; i < store->cap; i++)
        if (store->slots[i] != NULL)
            os64_dom_blob_release(store->slots[i]);
    os64_free(store->slots);
    os64_free(store);
}

os64_dom_blob_t *os64_dom_blob_url_take(os64_dom_blob_store_t *store, const char *url)
{
    if (store == NULL || url == NULL)
        return NULL;
    size_t length = without_fragment(url);
    if (length < 5 || os64_memcmp(url, "blob:", 5) != 0)
        return NULL;
    uint64_t hash = url_hash(url, length);
    os64_dom_blob_t *found = NULL;
    os64_lock_acquire(&store->lock);
    if (store->cap > 0) {
        found = store->slots[slot_of(store, hash, url, length)];
        if (found != NULL)
            __atomic_fetch_add(&found->refs, 1, __ATOMIC_RELAXED);
    }
    os64_lock_release(&store->lock);
    return found;
}

os64_dom_blob_t *os64_dom_blob_hold(os64_dom_blob_t *blob)
{
    if (blob != NULL)
        __atomic_fetch_add(&blob->refs, 1, __ATOMIC_RELAXED);
    return blob;
}

void os64_dom_blob_release(os64_dom_blob_t *blob)
{
    if (blob != NULL && __atomic_sub_fetch(&blob->refs, 1, __ATOMIC_ACQ_REL) == 0)
        os64_free(blob);
}

const uint8_t *os64_dom_blob_bytes(const os64_dom_blob_t *blob) { return blob->bytes; }
size_t os64_dom_blob_length(const os64_dom_blob_t *blob) { return blob->length; }
const char *os64_dom_blob_type(const os64_dom_blob_t *blob) { return blob->type; }

os64_dom_blob_usage_t os64_dom_blob_store_usage(os64_dom_blob_store_t *store)
{
    os64_dom_blob_usage_t u = {0, 0, 0};
    if (store == NULL)
        return u;
    os64_lock_acquire(&store->lock);
    u = (os64_dom_blob_usage_t){store->bytes, store->count, store->refused};
    os64_lock_release(&store->lock);
    return u;
}

os64_dom_blob_t *d_blob_url_take(os64_dom_t *dom, const char *url)
{
    return os64_dom_blob_url_take(dom->options.blobs, url);
}

/* Names `bytes` in the store under "blob:<origin>/<uuid>": 0, the entry
 * made and its URL in *made (the entry's own text); -1 without memory; -2
 * past the store's bound. */
static int name_entry(os64_dom_blob_store_t *s, const char *origin, const char *uuid, const uint8_t *bytes,
                      size_t length, const char *type, const char **made)
{
    size_t origin_length = os64_strlen(origin), type_length = os64_strlen(type);
    size_t url_length = 5 + origin_length + 1 + 36;
    size_t head = sizeof(os64_dom_blob_t);
    if (length > SIZE_MAX - head - url_length - type_length - 2)
        return -2;
    os64_lock_acquire(&s->lock);
    bool room = length <= s->max - s->bytes;
    if (!room)
        s->refused++;
    os64_lock_release(&s->lock);
    if (!room)
        return -2;
    os64_dom_blob_t *b = os64_malloc(head + url_length + 1 + type_length + 1 + length);
    if (b == NULL)
        return -1;
    char *text = (char *)(b + 1);
    os64_memcpy(text, "blob:", 5);
    os64_memcpy(text + 5, origin, origin_length);
    text[5 + origin_length] = '/';
    os64_memcpy(text + 6 + origin_length, uuid, 36);
    text[url_length] = '\0';
    char *kind = text + url_length + 1;
    os64_memcpy(kind, type, type_length + 1);
    uint8_t *copy = (uint8_t *)(kind + type_length + 1);
    if (length > 0)
        os64_memcpy(copy, bytes, length);
    *b = (os64_dom_blob_t){1, url_hash(text, url_length), text, kind, copy, length};
    os64_lock_acquire(&s->lock);
    /* Checked again under the lock that adds it: the bound is the store's,
     * whoever else named something meanwhile. */
    int status = length > s->max - s->bytes ? -2 : (2 * (s->count + 1) > s->cap && !grow(s)) ? -1 : 0;
    if (status == 0) {
        s->slots[slot_of(s, b->hash, text, url_length)] = b;
        s->count++;
        s->bytes += length;
    } else if (status == -2) {
        s->refused++;
    }
    os64_lock_release(&s->lock);
    if (status != 0) {
        os64_free(b);
        return status;
    }
    *made = text;
    return 0;
}

static JSValue create_url(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    const uint8_t *bytes;
    size_t length;
    const char *type;
    if (argc < 1 || !d_blob_get(argv[0], &bytes, &length, &type))
        return JS_ThrowTypeError(ctx, "Failed to execute 'createObjectURL' on 'URL': parameter 1 is not a Blob");
    if (dom->options.blobs == NULL)
        return JS_ThrowTypeError(ctx, "This page cannot name a Blob");
    char origin[WAY_ORIGIN_TEXT_MAX], uuid[37];
    if (!d_origin(dom->url != NULL ? dom->url : "about:blank", origin, sizeof(origin)))
        os64_strcopy(origin, sizeof(origin), "null");
    if (!d_uuid_text(dom, ctx, uuid))
        return JS_EXCEPTION;
    const char *made = NULL;
    int status = name_entry(dom->options.blobs, origin, uuid, bytes, length, type != NULL ? type : "", &made);
    if (status == -2)
        return d_error(ctx, "QuotaExceededError", "The page's Blob URLs would hold more than its bound");
    if (status != 0)
        return JS_ThrowOutOfMemory(ctx);
    return JS_NewString(ctx, made);
}

static JSValue revoke_url(os64_dom_t *dom, JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "Failed to execute 'revokeObjectURL' on 'URL': 1 argument required");
    size_t length;
    const char *url = JS_ToCStringLen(ctx, &length, argv[0]);
    if (url == NULL)
        return JS_EXCEPTION;
    os64_dom_blob_store_t *s = dom->options.blobs;
    os64_dom_blob_t *gone = NULL;
    /* The text as named: a URL with a fragment, or of another page, names
     * nothing here, and is ignored without a word, as the standard says. */
    if (s != NULL && length >= 5 && os64_memcmp(url, "blob:", 5) == 0 && os64_strlen(url) == length) {
        uint64_t hash = url_hash(url, length);
        os64_lock_acquire(&s->lock);
        if (s->cap > 0) {
            size_t at = slot_of(s, hash, url, length);
            gone = s->slots[at];
            if (gone != NULL) {
                s->bytes -= gone->length;
                remove_at(s, at);
            }
        }
        os64_lock_release(&s->lock);
    }
    JS_FreeCString(ctx, url);
    os64_dom_blob_release(gone);
    return JS_UNDEFINED;
}

JSValue d_blob_url_member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic,
                          JSValue *data)
{
    (void)self;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (dom == NULL)
        return JS_EXCEPTION;
    return magic == D_BLOB_URL_CREATE ? create_url(dom, ctx, argc, argv) : revoke_url(dom, ctx, argc, argv);
}
