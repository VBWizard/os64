#include "internal.h"

/* BYTES A PAGE KEEPS OUTSIDE THE ENGINE (XHR_BYTES.md): a response's body,
 * and the bytes of a Blob the page makes. A body is the process's memory,
 * not the runtime's, so a page that downloads, or builds, more than its
 * scripts' heap holds still runs; what it costs is bounded instead by the
 * page's body bound (os64_dom_options_t.max_body_bytes), across every body
 * it holds, charged at what its bytes' allocations take (a piece with its
 * header). The record of a response's body is not charged, so an empty
 * response passes a bound of zero; the record of a Blob's is
 * (d_kept_new_whole), since a page makes Blobs as fast as it likes and
 * responses only as fast as it is answered.
 *
 * A body is refcounted: the XHR that received it, or the Blob the page
 * made, holds one reference, and a Blob made over it, an ArrayBuffer handed
 * it, or a request sending it holds another. While it arrives it is a
 * block, when the length was declared and fits, and then pieces as they
 * came; d_kept_join makes it one block, once. A body is
 * freed by its last reference, which an engine finalizer may drop: the
 * public teardown destroys the engine before the binding, so the binding
 * is there to be told. A finalizer the teardown never runs (an engine
 * reclaimed rather than collected, libjs/TEARDOWN.md) drops nothing, so
 * the binding keeps its bodies on a list and os64_dom_free frees what is
 * still on it (d_kept_free_all): no body waits on a finalizer. */

typedef struct DKeptPiece {
    struct DKeptPiece *next;
    size_t length;
    uint8_t bytes[];
} DKeptPiece;

struct DKept {
    os64_dom_t *dom;
    struct DKept *prev, *next;      /* the binding's list (dom->kept) */
    size_t refs;
    /* The block: `length` bytes of it in use when there are no pieces,
     * else the first `joined` bytes of the body, room for `cap`. */
    uint8_t *block;
    size_t cap, joined, length;
    DKeptPiece *pieces, *last;
    size_t charged;
};

/* `n` more bytes charged to the page's bound; false when they would pass it. */
static bool charge(os64_dom_t *dom, size_t n)
{
    if (n > dom->options.max_body_bytes || dom->body_bytes > dom->options.max_body_bytes - n) return false;
    dom->body_bytes += n;
    return true;
}

static void uncharge(os64_dom_t *dom, size_t n)
{
    dom->body_bytes -= n;
}

/* Memory for a body, charged: NULL when the bound or the process refuses. */
static void *body_alloc(DKept *b, size_t n)
{
    if (!charge(b->dom, n)) return NULL;
    void *p = os64_malloc(n);
    if (p == NULL) {
        uncharge(b->dom, n);
        return NULL;
    }
    b->charged += n;
    return p;
}

static void body_release(DKept *b, void *p, size_t n)
{
    if (p == NULL) return;
    os64_free(p);
    uncharge(b->dom, n);
    b->charged -= n;
}

DKept *d_kept_new(os64_dom_t *dom, size_t declared)
{
    DKept *b = os64_malloc(sizeof(*b));
    if (b == NULL) return NULL;
    os64_memset(b, 0, sizeof(*b));
    b->dom = dom;
    b->refs = 1;
    b->next = dom->kept;
    if (dom->kept != NULL) dom->kept->prev = b;
    dom->kept = b;
    /* A declared length within the bound is one block from the start; one
     * that is not leaves the body to its pieces, and the bound decides as
     * they come. */
    if (declared != 0) {
        b->block = body_alloc(b, declared);
        if (b->block != NULL) b->cap = declared;
    }
    return b;
}

DKept *d_kept_new_whole(os64_dom_t *dom, size_t length)
{
    /* A Blob's record is charged, a response's is not: a page makes as
     * many Blobs as its heap holds objects, so a million empty ones would
     * otherwise be process memory nothing counts. */
    if (!charge(dom, sizeof(DKept))) return NULL;
    DKept *b = d_kept_new(dom, length);
    if (b == NULL) {
        uncharge(dom, sizeof(DKept));
        return NULL;
    }
    b->charged += sizeof(DKept);
    if (length != 0 && b->block == NULL) {
        d_kept_unref(b);
        return NULL;
    }
    return b;
}

bool d_kept_append(DKept *b, const uint8_t *bytes, size_t length)
{
    if (length == 0) return true;
    if (b->pieces == NULL && b->block != NULL && length <= b->cap - b->length) {
        os64_memcpy(b->block + b->length, bytes, length);
        b->length += length;
        b->joined = b->length;
        return true;
    }
    if (length > SIZE_MAX - sizeof(DKeptPiece)) return false;
    DKeptPiece *piece = body_alloc(b, sizeof(*piece) + length);
    if (piece == NULL) return false;
    piece->next = NULL;
    piece->length = length;
    os64_memcpy(piece->bytes, bytes, length);
    if (b->last != NULL) b->last->next = piece;
    else b->pieces = piece;
    b->last = piece;
    if (b->pieces == piece) b->joined = b->length;
    b->length += length;
    return true;
}

bool d_kept_join(DKept *b)
{
    if (b->pieces == NULL) return true;
    /* The peak, block and pieces at once, is charged like any other byte. */
    uint8_t *whole = body_alloc(b, b->length);
    if (whole == NULL) return false;
    if (b->joined != 0) os64_memcpy(whole, b->block, b->joined);
    size_t at = b->joined;
    for (DKeptPiece *p = b->pieces, *next; p != NULL; p = next) {
        next = p->next;
        os64_memcpy(whole + at, p->bytes, p->length);
        at += p->length;
        body_release(b, p, sizeof(*p) + p->length);
    }
    body_release(b, b->block, b->cap);
    b->pieces = b->last = NULL;
    b->block = whole;
    b->cap = b->joined = b->length;
    return true;
}

const uint8_t *d_kept_bytes(const DKept *b)
{
    return b->pieces == NULL && b->block != NULL ? b->block : (const uint8_t *)"";
}

size_t d_kept_length(const DKept *b)
{
    return b->length;
}

DKept *d_kept_ref(DKept *b)
{
    b->refs++;
    return b;
}

static void body_free(DKept *b)
{
    for (DKeptPiece *p = b->pieces, *next; p != NULL; p = next) {
        next = p->next;
        body_release(b, p, sizeof(*p) + p->length);
    }
    body_release(b, b->block, b->cap);
    uncharge(b->dom, b->charged);       /* the record, for a Blob's */
    if (b->prev != NULL) b->prev->next = b->next;
    else b->dom->kept = b->next;
    if (b->next != NULL) b->next->prev = b->prev;
    os64_free(b);
}

void d_kept_unref(DKept *b)
{
    if (b == NULL || --b->refs != 0) return;
    body_free(b);
}

void d_kept_free_all(os64_dom_t *dom)
{
    while (dom->kept != NULL) body_free(dom->kept);
}

/* QuickJS calls an ArrayBuffer's free function once with its bytes when
 * they go (finalized, detached, transferred to a buffer of another length)
 * and again with NULL when a buffer whose bytes went is finalized; a
 * transfer at the same length hands the bytes, the function and the body
 * to the new buffer and leaves the old one NULL. So the reference goes
 * with the bytes, exactly once. */
static void array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
    (void)rt;
    if (ptr != NULL) d_kept_unref(opaque);
}

JSValue d_kept_array_buffer(JSContext *ctx, DKept *b)
{
    if (b->pieces != NULL) return JS_ThrowInternalError(ctx, "A response body was read before it was joined");
    if (b->length == 0) return JS_NewArrayBufferCopy(ctx, d_kept_bytes(b), 0);
    JSValue buffer = JS_NewArrayBuffer(ctx, b->block, b->length, array_buffer_free, b, false);
    if (!JS_IsException(buffer)) d_kept_ref(b);
    return buffer;
}

void os64_dom_body_stats(const os64_dom_t *dom, size_t *held, uint64_t *refused)
{
    if (held != NULL) *held = dom != NULL ? dom->body_bytes : 0;
    if (refused != NULL) *refused = dom != NULL ? dom->bodies_refused : 0;
}
