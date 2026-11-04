#include "internal.h"

/* A RESPONSE'S BYTES OUTSIDE THE ENGINE (XHR_BYTES.md). A body is the
 * process's memory, not the runtime's, so a page that downloads more than
 * its scripts' heap holds still runs; what it costs is bounded instead by
 * the page's body bound (os64_dom_options_t.max_body_bytes), across every
 * body it holds, charged at what its bytes' allocations take (a piece with
 * its header); the record of a body is not charged, so an empty body
 * passes a bound of zero.
 *
 * A body is refcounted: the XHR that received it holds one reference, and
 * a Blob made over it or an ArrayBuffer handed it holds another. While it
 * arrives it is a block, when the length was declared and fits, and then
 * pieces as they came; d_received_join makes it one block, once. A body is
 * freed by its last reference, which an engine finalizer may drop: the
 * public teardown destroys the engine before the binding, so the binding
 * is there to be told. A finalizer the teardown never runs (an engine
 * reclaimed rather than collected, libjs/TEARDOWN.md) drops nothing, so
 * the binding keeps its bodies on a list and os64_dom_free frees what is
 * still on it (d_received_free_all): no body waits on a finalizer. */

typedef struct DReceivedPiece {
    struct DReceivedPiece *next;
    size_t length;
    uint8_t bytes[];
} DReceivedPiece;

struct DReceived {
    os64_dom_t *dom;
    struct DReceived *prev, *next;      /* the binding's list (dom->received) */
    size_t refs;
    /* The block: `length` bytes of it in use when there are no pieces,
     * else the first `joined` bytes of the body, room for `cap`. */
    uint8_t *block;
    size_t cap, joined, length;
    DReceivedPiece *pieces, *last;
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
static void *body_alloc(DReceived *b, size_t n)
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

static void body_release(DReceived *b, void *p, size_t n)
{
    if (p == NULL) return;
    os64_free(p);
    uncharge(b->dom, n);
    b->charged -= n;
}

DReceived *d_received_new(os64_dom_t *dom, size_t declared)
{
    DReceived *b = os64_malloc(sizeof(*b));
    if (b == NULL) return NULL;
    os64_memset(b, 0, sizeof(*b));
    b->dom = dom;
    b->refs = 1;
    b->next = dom->received;
    if (dom->received != NULL) dom->received->prev = b;
    dom->received = b;
    /* A declared length within the bound is one block from the start; one
     * that is not leaves the body to its pieces, and the bound decides as
     * they come. */
    if (declared != 0) {
        b->block = body_alloc(b, declared);
        if (b->block != NULL) b->cap = declared;
    }
    return b;
}

bool d_received_append(DReceived *b, const uint8_t *bytes, size_t length)
{
    if (length == 0) return true;
    if (b->pieces == NULL && b->block != NULL && length <= b->cap - b->length) {
        os64_memcpy(b->block + b->length, bytes, length);
        b->length += length;
        b->joined = b->length;
        return true;
    }
    if (length > SIZE_MAX - sizeof(DReceivedPiece)) return false;
    DReceivedPiece *piece = body_alloc(b, sizeof(*piece) + length);
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

bool d_received_join(DReceived *b)
{
    if (b->pieces == NULL) return true;
    /* The peak, block and pieces at once, is charged like any other byte. */
    uint8_t *whole = body_alloc(b, b->length);
    if (whole == NULL) return false;
    if (b->joined != 0) os64_memcpy(whole, b->block, b->joined);
    size_t at = b->joined;
    for (DReceivedPiece *p = b->pieces, *next; p != NULL; p = next) {
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

const uint8_t *d_received_bytes(const DReceived *b)
{
    return b->pieces == NULL && b->block != NULL ? b->block : (const uint8_t *)"";
}

size_t d_received_length(const DReceived *b)
{
    return b->length;
}

DReceived *d_received_ref(DReceived *b)
{
    b->refs++;
    return b;
}

static void body_free(DReceived *b)
{
    for (DReceivedPiece *p = b->pieces, *next; p != NULL; p = next) {
        next = p->next;
        body_release(b, p, sizeof(*p) + p->length);
    }
    body_release(b, b->block, b->cap);
    if (b->prev != NULL) b->prev->next = b->next;
    else b->dom->received = b->next;
    if (b->next != NULL) b->next->prev = b->prev;
    os64_free(b);
}

void d_received_unref(DReceived *b)
{
    if (b == NULL || --b->refs != 0) return;
    body_free(b);
}

void d_received_free_all(os64_dom_t *dom)
{
    while (dom->received != NULL) body_free(dom->received);
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
    if (ptr != NULL) d_received_unref(opaque);
}

JSValue d_received_array_buffer(JSContext *ctx, DReceived *b)
{
    if (b->pieces != NULL) return JS_ThrowInternalError(ctx, "A response body was read before it was joined");
    if (b->length == 0) return JS_NewArrayBufferCopy(ctx, d_received_bytes(b), 0);
    JSValue buffer = JS_NewArrayBuffer(ctx, b->block, b->length, array_buffer_free, b, false);
    if (!JS_IsException(buffer)) d_received_ref(b);
    return buffer;
}

void os64_dom_body_stats(const os64_dom_t *dom, size_t *held, uint64_t *refused)
{
    if (held != NULL) *held = dom != NULL ? dom->body_bytes : 0;
    if (refused != NULL) *refused = dom != NULL ? dom->bodies_refused : 0;
}
