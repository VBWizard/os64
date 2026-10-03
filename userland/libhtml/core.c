#include "internal.h"
#include "os64/io.h"
#include "os64/proc.h"

/* Blocks are owned by the document, including parser scratch until finish.
 * Charging capacity and headers makes the arena limit a retained-memory bound,
 * rather than a count of requested payload bytes. Growth charges both blocks
 * while copying; the returned document reports its peak as well as live cost.
 * `stamp` is a version of the document. On a live block, which sits on the
 * document's `blocks` list, it is the version the block was made at; on a
 * retired one, which sits on `retired`, the version of the change that
 * replaced it. The node tag identifies individually reclaimable fragment
 * bodies; padding keeps payloads 16-byte aligned. */
struct HBlock {
    HBlock *prev, *next;
    size_t size;
    uint64_t stamp;
    uint64_t node, padding;
};
static const char *const tag_names[] = {
#include "tags.inc"
};

size_t h_len(const char *s)
{
    size_t n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}
bool h_eq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}
bool h_in(const char *name, const char *set)
{
    if (!name)
        return false;
    /* Sets are fixed grammar words. Bound comparisons by those words, not by
     * the possibly enormous unknown element name supplied by the document. */
    while (*set) {
        const char *start = set;
        while (*set && *set != ' ')
            set++;
        size_t len = (size_t)(set - start), i = 0;
        while (i < len && name[i] && name[i] == start[i])
            i++;
        if (i == len && name[i] == 0)
            return true;
        if (*set)
            set++;
    }
    return false;
}
bool h_space(uint32_t c)
{
    return c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}
bool h_alpha(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
uint32_t h_lower(uint32_t c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

void h_refuse(os64_html_parser_t *p, int64_t status)
{
    if (!p->d->pub.refusal)
        p->d->pub.refusal = status;
}
bool h_work(os64_html_parser_t *p, uint64_t n)
{
    if (p->d->pub.refusal)
        return false;
    if (n > p->opt.max_work - p->d->pub.work) {
        h_refuse(p, OS64_HTML_WORK_EXHAUSTED);
        return false;
    }
    p->d->pub.work += n;
    return true;
}
void h_error(os64_html_parser_t *p, const char *name)
{
    size_t n = p->d->pub.parse_errors;
    if (n < H_ARRAY(p->d->pub.first_errors))
        p->d->pub.first_errors[n] = (os64_html_parse_error_t){p->offset, name};
    if (n != SIZE_MAX)
        p->d->pub.parse_errors++;
}
void *d_alloc(HDoc *d, size_t size, int64_t *why)
{
    if (size > SIZE_MAX - sizeof(HBlock) ||
        size + sizeof(HBlock) > d->budget - d->pub.arena_bytes) {
        *why = OS64_HTML_ARENA_EXHAUSTED;
        return NULL;
    }
    HBlock *b = os64_malloc(sizeof(*b) + size);
    if (!b) {
        *why = OS64_HTML_NO_MEMORY;
        return NULL;
    }
    b->size = sizeof(*b) + size;
    b->stamp = d->version;
    b->node = b->padding = 0;
    b->prev = NULL;
    b->next = d->blocks;
    if (b->next)
        b->next->prev = b;
    d->blocks = b;
    d->pub.arena_bytes += b->size;
    if (d->pub.arena_bytes > d->pub.peak_arena_bytes)
        d->pub.peak_arena_bytes = d->pub.arena_bytes;
    unsigned char *out = (unsigned char *)(b + 1);
    for (size_t i = 0; i < size; i++)
        out[i] = 0;
    return out;
}
void *d_node_alloc(HDoc *d, size_t size, int64_t *why)
{
    void *out = d_alloc(d, size, why);
    if (out)
        ((HBlock *)out - 1)->node = 1;
    return out;
}
/* Publish the complete staged ledger without allocating. */
void d_transfer_blocks(HDoc *to, HDoc *from)
{
    HBlock *tail = from->blocks;
    if (!tail)
        return;
    for (HBlock *b = tail; b; b = b->next) {
        b->stamp = to->version;
        tail = b;
    }
    tail->next = to->blocks;
    if (to->blocks)
        to->blocks->prev = tail;
    to->blocks = from->blocks;
    from->blocks = NULL;
    to->pub.arena_bytes += from->pub.arena_bytes - sizeof(*from);
    to->pub.node_count += from->pub.node_count;
    to->records += from->records;
    from->pub.arena_bytes = sizeof(*from);
}
void *h_alloc(os64_html_parser_t *p, size_t size)
{
    if (p->d->pub.refusal)
        return NULL;
    int64_t why = OS64_HTML_OK;
    void *out = d_alloc(p->d, size, &why);
    if (!out)
        h_refuse(p, why);
    return out;
}
static void block_unlink(HBlock **list, HBlock *b)
{
    if (b->prev)
        b->prev->next = b->next;
    else
        *list = b->next;
    if (b->next)
        b->next->prev = b->prev;
}
void h_free(HDoc *d, void *ptr)
{
    if (!ptr)
        return;
    HBlock *b = (HBlock *)ptr - 1;
    block_unlink(&d->blocks, b);
    d->pub.arena_bytes -= b->size;
    os64_free(b);
}

/* ── Pins and retirement (DOM.md § The mutation core) ────────────────────
 *
 * A snapshot built at version P borrowed what the tree held at P. A block
 * replaced by the change that made version R was in the tree at every
 * version below R, so a pin below R may still point at it and a pin at R or
 * later cannot. */
static bool pinned_below(const HDoc *d, uint64_t version)
{
    for (size_t i = 0; i < H_PINS; i++)
        if (d->pins[i] && d->pins[i] < version)
            return true;
    return false;
}
static void retired_free(HDoc *d, HBlock *b)
{
    block_unlink(&d->retired, b);
    d->retired_bytes -= b->size;
    d->pub.arena_bytes -= b->size;
    os64_free(b);
}
void d_retire(HDoc *d, void *ptr)
{
    if (!ptr)
        return;
    if (!pinned_below(d, d->version)) {
        h_free(d, ptr);
        return;
    }
    /* Still charged to the arena: a snapshot that never lets go must not
     * make replaced bytes free. Its links are the block's own, so retiring
     * allocates nothing and cannot fail. */
    HBlock *b = (HBlock *)ptr - 1;
    block_unlink(&d->blocks, b);
    b->stamp = d->version;
    b->prev = NULL;
    b->next = d->retired;
    if (b->next)
        b->next->prev = b;
    d->retired = b;
    d->retired_bytes += b->size;
}
static void reclaim(HDoc *d)
{
    for (HBlock *b = d->retired, *next; b; b = next) {
        next = b->next;
        if (!pinned_below(d, b->stamp))
            retired_free(d, b);
    }
}
/* Whether a snapshot may be pointing at a live block made at version `born`:
 * a pin taken before the block existed cannot be. */
static bool pinned_since(const HDoc *d, uint64_t born)
{
    for (size_t i = 0; i < H_PINS; i++)
        if (d->pins[i] >= born && d->pins[i])
            return true;
    return false;
}
/* A broken promise about memory ends the program, as a stomped heap canary
 * does: whoever still holds the pin is about to read what is being freed. */
static void fatal(const char *sentence)
{
    os64_write(2, sentence, h_len(sentence));
    os64_exit((int32_t)OS64_HTML_FATAL_EXIT);
}
uint64_t os64_html_version(const os64_html_document_t *doc)
{
    return doc ? ((const HDoc *)doc)->version : 0;
}
bool os64_html_owns_node(const os64_html_document_t *doc, const os64_html_node_t *node)
{
    return doc && node && node->document_id == ((const HDoc *)doc)->id;
}
/* A handle is its slot and a serial number no other pin in the program has
 * had, so one that was let go already, or that belongs to another document,
 * matches nothing. A handle that was only the slot would come round again,
 * and its stale twin would then release a pin some other snapshot holds.
 * Documents on different threads draw from the one counter, so it is
 * advanced atomically. */
static uint64_t pin_serial;
os64_html_pin_t os64_html_pin(const os64_html_document_t *doc)
{
    /* The pin table is the document's own bookkeeping, not its content. */
    HDoc *d = (HDoc *)doc;
    if (!d)
        return 0;
    for (size_t i = 0; i < H_PINS; i++)
        if (!d->pins[i]) {
            d->pins[i] = d->version;
            d->pinned++;
            d->pin_handles[i] = __atomic_add_fetch(&pin_serial, 1, __ATOMIC_RELAXED) * H_PINS + i;
            return d->pin_handles[i];
        }
    return 0;
}
void os64_html_unpin(const os64_html_document_t *doc, os64_html_pin_t pin)
{
    HDoc *d = (HDoc *)doc;
    if (!d || !pin)
        return;
    size_t slot = pin % H_PINS;
    if (!d->pins[slot] || d->pin_handles[slot] != pin)
        fatal("libhtml: unpin of a pin that is not held\n");
    d->pins[slot] = 0;
    d->pinned--;
    reclaim(d);
}
size_t os64_html_retired_bytes(const os64_html_document_t *doc)
{
    return doc ? ((const HDoc *)doc)->retired_bytes : 0;
}

/* Stable nodes and strings share geometrically grown arena chunks. Scratch
 * buffers use individually releasable ledger blocks so replacing a buffer does
 * not retain every old capacity until document_free. Both charge one budget. */
void *d_permanent(HDoc *d, size_t size, int64_t *why)
{
    if (size > SIZE_MAX - 15) {
        *why = OS64_HTML_ARENA_EXHAUSTED;
        return NULL;
    }
    size = (size + 15) & ~(size_t)15;
    if (size > d->permanent_left) {
        size_t cap = d->permanent_chunk ? d->permanent_chunk * 2 : 4096;
        if (cap > 256 * 1024)
            cap = 256 * 1024;
        if (cap < size)
            cap = size;
        size_t available = d->budget - d->pub.arena_bytes;
        if (available > sizeof(HBlock) && cap > available - sizeof(HBlock))
            cap = available - sizeof(HBlock);
        if (cap < size) {
            *why = OS64_HTML_ARENA_EXHAUSTED;
            return NULL;
        }
        unsigned char *chunk = d_alloc(d, cap, why);
        if (!chunk)
            return NULL;
        d->permanent = chunk;
        d->permanent_left = cap;
        d->permanent_chunk = cap;
    }
    void *out = d->permanent;
    d->permanent += size;
    d->permanent_left -= size;
    return out;
}
void *h_permanent(os64_html_parser_t *p, size_t size)
{
    if (p->d->pub.refusal)
        return NULL;
    int64_t why = OS64_HTML_OK;
    void *out = d_permanent(p->d, size, &why);
    if (!out)
        h_refuse(p, why);
    return out;
}
char *h_copy(os64_html_parser_t *p, const char *s, size_t n)
{
    if (n == SIZE_MAX) {
        h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
        return NULL;
    }
    char *out = h_permanent(p, n + 1);
    if (out)
        for (size_t i = 0; i < n; i++)
            out[i] = s[i];
    return out;
}
bool h_buf_bytes(os64_html_parser_t *p, HBuf *b, const char *s, size_t n)
{
    if (p->d->pub.refusal)
        return false;
    if (n >= SIZE_MAX - b->len) {
        h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
        return false;
    }
    size_t need = b->len + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 32;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        char *out = h_alloc(p, cap);
        if (!out)
            return false;
        for (size_t i = 0; i < b->len; i++)
            out[i] = b->s[i];
        for (size_t i = 0; i < n; i++)
            out[b->len + i] = s[i];
        h_free(p->d, b->s);
        b->s = out;
        b->cap = cap;
    } else {
        for (size_t i = 0; i < n; i++)
            b->s[b->len + i] = s[i];
    }
    b->len += n;
    b->s[b->len] = 0;
    return true;
}
bool h_buf_put(os64_html_parser_t *p, HBuf *b, uint32_t c)
{
    char s[4];
    size_t n;
    if (c < 0x80) {
        s[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        s[0] = (char)(0xc0 | (c >> 6));
        s[1] = (char)(0x80 | (c & 63));
        n = 2;
    } else if (c < 0x10000) {
        s[0] = (char)(0xe0 | (c >> 12));
        s[1] = (char)(0x80 | ((c >> 6) & 63));
        s[2] = (char)(0x80 | (c & 63));
        n = 3;
    } else {
        s[0] = (char)(0xf0 | (c >> 18));
        s[1] = (char)(0x80 | ((c >> 12) & 63));
        s[2] = (char)(0x80 | ((c >> 6) & 63));
        s[3] = (char)(0x80 | (c & 63));
        n = 4;
    }
    return h_buf_bytes(p, b, s, n);
}
/* DOM.md's "grow in place". A text node's bytes are a block of the node's
 * own once its private word holds a capacity, and the tree can be read
 * between two calls of the parser. A snapshot built then pinned a version no
 * older than the block and may be pointing into it, so the block is not
 * written to and not freed: the text moves to a block of its own first, and
 * the old one is retired. A pin older than the block never saw it, and the
 * block the move makes is newer than every pin, so a text node is moved
 * once for each snapshot taken, not once for each character. */
bool h_text_put(os64_html_parser_t *p, HNode *n, uint32_t c)
{
    HDoc *d = p->d;
    size_t *cap = h_word(n);
    if (*cap && d->pinned && pinned_since(d, ((HBlock *)n->text - 1)->stamp)) {
        char *own = h_alloc(p, *cap);
        if (!own)
            return false;
        for (size_t i = 0; i < n->text_len; i++)
            own[i] = n->text[i];
        d_retire(d, (char *)n->text);
        n->text = own;
    }
    HBuf b = {*cap ? (char *)n->text : NULL, n->text_len, *cap};
    if (!h_buf_put(p, &b, c))
        return false;
    n->text = b.s;
    n->text_len = b.len;
    *cap = b.cap;
    return true;
}
void h_buf_reset(HBuf *b)
{
    b->len = 0;
    if (b->s)
        b->s[0] = 0;
}
bool h_nodes_push(os64_html_parser_t *p, HNodes *list, HNode *n)
{
    if (list->n == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 16;
        if (cap < list->cap || cap > SIZE_MAX / sizeof(HNode *)) {
            h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
            return false;
        }
        HNode **v = h_alloc(p, cap * sizeof(*v));
        if (!v)
            return false;
        for (size_t i = 0; i < list->n; i++)
            v[i] = list->v[i];
        h_free(p->d, list->v);
        list->v = v;
        list->cap = cap;
    }
    list->v[list->n++] = n;
    return true;
}
HNode *h_current(os64_html_parser_t *p)
{
    return p->stack.n ? p->stack.v[p->stack.n - 1] : &p->d->root;
}
HNode *h_node(os64_html_parser_t *p, os64_html_node_kind_t kind)
{
    /* Fragment staging adds a temporary allocation ordinal for deterministic
     * form-owner remapping. Ordinary document nodes keep one private word. */
    HNode *n = h_permanent(p, sizeof(*n) + sizeof(size_t) * (p->fragment_context ? 2 : 1));
    if (n) {
        n->kind = kind;
        n->document_id = p->d->id;
        if (p->fragment_context)
            *(h_word(n) + 1) = p->d->pub.node_count;
        p->d->pub.node_count++;
    }
    return n;
}
void h_detach(HNode *n)
{
    if (!n->parent)
        return;
    if (n->prev)
        n->prev->next = n->next;
    else
        n->parent->first_child = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        n->parent->last_child = n->prev;
    n->parent = n->prev = n->next = NULL;
}
void h_attach(HNode *parent, HNode *before, HNode *n)
{
    h_detach(n);
    n->parent = parent;
    n->next = before;
    n->prev = before ? before->prev : parent->last_child;
    if (n->prev)
        n->prev->next = n;
    else
        parent->first_child = n;
    if (before)
        before->prev = n;
    else
        parent->last_child = n;
}
os64_html_tag_t os64_html_tag_from_name(const char *name)
{
    for (size_t i = 1; i < H_ARRAY(tag_names); i++)
        if (h_eq(name, tag_names[i]))
            return (os64_html_tag_t)i;
    return OS64_HTML_TAG_UNKNOWN;
}
const char *os64_html_tag_name(os64_html_tag_t tag)
{
    return (unsigned)tag < H_ARRAY(tag_names) && tag != OS64_HTML_TAG_UNKNOWN ? tag_names[tag]
                                                                              : NULL;
}
const HAttr *os64_html_attr(const HNode *e, const char *name)
{
    if (e && e->kind == OS64_HTML_ELEMENT)
        for (const HAttr *a = e->attrs; a; a = a->next)
            if (h_eq(a->name, name))
                return a;
    return NULL;
}
const char *os64_html_status_name(int64_t s)
{
    switch (s) {
    case OS64_HTML_OK:
        return "OS64_HTML_OK";
    case OS64_HTML_SCRIPT:
        return "OS64_HTML_SCRIPT";
    case OS64_HTML_TOO_LARGE:
        return "OS64_HTML_TOO_LARGE";
    case OS64_HTML_ARENA_EXHAUSTED:
        return "OS64_HTML_ARENA_EXHAUSTED";
    case OS64_HTML_NO_MEMORY:
        return "OS64_HTML_NO_MEMORY";
    case OS64_HTML_TOO_DEEP:
        return "OS64_HTML_TOO_DEEP";
    case OS64_HTML_WORK_EXHAUSTED:
        return "OS64_HTML_WORK_EXHAUSTED";
    case OS64_HTML_HIERARCHY:
        return "OS64_HTML_HIERARCHY";
    case OS64_HTML_NOT_FOUND:
        return "OS64_HTML_NOT_FOUND";
    case OS64_HTML_BAD_TEXT:
        return "OS64_HTML_BAD_TEXT";
    case OS64_HTML_ROOT_REQUIRED:
        return "OS64_HTML_ROOT_REQUIRED";
    case OS64_HTML_BAD_ARGUMENT:
        return "OS64_HTML_BAD_ARGUMENT";
    default:
        return "OS64_HTML_UNKNOWN_STATUS";
    }
}
void os64_html_document_free(os64_html_document_t *doc)
{
    if (!doc)
        return;
    HDoc *d = (HDoc *)doc;
    for (size_t i = 0; i < H_PINS; i++)
        if (d->pins[i])
            fatal("libhtml: a document was freed while a snapshot still had it pinned\n");
    while (d->retired)
        retired_free(d, d->retired);
    while (d->blocks)
        h_free(d, d->blocks + 1);
    os64_free(d);
}
void os64_html_parser_destroy(os64_html_parser_t *p)
{
    if (!p)
        return;
    os64_free(p->hold);     /* the heap's, so the ledger below does not free it */
    os64_html_document_free(&p->d->pub);
}

os64_html_options_t os64_html_options_default(void)
{
    /* The saved Wikipedia page peaks near 4.3 MB of charged bytes and 1.2M
     * work units. 64 MiB and 100M leave about 15x/82x headroom for real pages;
     * crafted growth still refuses by name (tools/test_html_driver.c). */
    return (os64_html_options_t){NULL, 8u * 1024u * 1024u, 64u * 1024u * 1024u, 512, 100000000,
                                 false};
}
/* Where HDoc.id comes from: a count of the documents the program has begun.
 * It does not come round. A mark given out twice could sit on two live
 * documents at once, each taking the other's nodes for its own, so when the
 * count is spent the answer is zero and no further document is made.
 * Documents are made on more than one thread, so it is advanced atomically. */
uint32_t h_document_serial;
static uint32_t document_mark(void)
{
    uint32_t seen = __atomic_load_n(&h_document_serial, __ATOMIC_RELAXED);
    do {
        if (seen == UINT32_MAX)
            return 0;
    } while (!__atomic_compare_exchange_n(&h_document_serial, &seen, seen + 1, true, __ATOMIC_RELAXED,
                                          __ATOMIC_RELAXED));
    return seen + 1;
}
os64_html_parser_t *os64_html_parser_new(const os64_html_options_t *options)
{
    os64_html_options_t opt = options ? *options : os64_html_options_default();
    if (opt.max_arena_bytes < sizeof(HDoc))
        return NULL;
    HDoc *d = os64_calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->budget = opt.max_arena_bytes;
    d->version = 1;
    d->max_depth = opt.max_depth;
    d->pub.arena_bytes = d->pub.peak_arena_bytes = sizeof(*d);
    os64_html_parser_t bootstrap = {0};
    bootstrap.d = d;
    os64_html_parser_t *p = h_alloc(&bootstrap, sizeof(*p));
    d->id = p ? document_mark() : 0;
    if (!d->id) {
        os64_html_document_free(&d->pub);
        return NULL;
    }
    p->d = d;
    p->opt = opt;
    p->frameset_ok = true;
    p->utf16_byte = -1;
    d->root.kind = OS64_HTML_DOCUMENT;
    d->html.kind = OS64_HTML_ELEMENT;
    d->html.name = "html";
    d->html.tag = OS64_HTML_TAG_HTML;
    d->root.document_id = d->html.document_id = d->id;
    d->pub.document = &d->root;
    d->pub.html = &d->html;
    d->pub.node_count = 2;
    if (opt.charset) {
        if (!h_buf_bytes(p, &p->charset_label, opt.charset, h_len(opt.charset))) {
            os64_html_document_free(&d->pub);
            return NULL;
        }
        p->opt.charset = p->charset_label.s;
    }
    return p;
}
/* ── The parse, call by call ─────────────────────────────────────────────
 *
 * Bytes reach the decoder in the order they were fed, from three places: the
 * sniff window (`prescan`) once the encoding is chosen, then the hold, then
 * the caller's own buffer, which is read where it lies when nothing is
 * waiting ahead of it. A parse stopped at a script takes no byte further:
 * what is fed meanwhile joins the hold. */
/* The document's version moves once in each call of the parser that parses
 * anything, and before the first thing it parses: whatever was built or
 * pinned between two calls is then older than every block this call makes
 * and every block it retires (DOM.md, the mutation core). It is said by
 * whoever is about to hand the decoder a byte, and not by the decoder, which
 * would say it a byte at a time. */
void h_moving(os64_html_parser_t *p)
{
    if (!p->moved) {
        p->moved = true;
        p->d->version++;
    }
}
static void hold_free(os64_html_parser_t *p)
{
    os64_free(p->hold);
    p->hold = NULL;
    p->hold_len = p->hold_cap = p->hold_at = 0;
}
void h_pump(os64_html_parser_t *p)
{
    HDoc *d = p->d;
    if (!p->started)
        return;
    if (!d->pub.refusal && !p->script &&
        (p->parsed < p->prescan_len || p->hold_at < p->hold_len))
        h_moving(p);
    while (!d->pub.refusal && !p->script) {
        if (p->parsed < p->prescan_len) {
            size_t at = p->parsed++;
            h_decode(p, p->prescan[at], at);
        } else if (p->hold_at < p->hold_len)
            h_decode(p, p->hold[p->hold_at++], p->parsed++);
        else
            break;
    }
    if (p->hold_at == p->hold_len)
        hold_free(p);
}
/* Keep `n` bytes for later. False, and the parse refused, when the heap has
 * no room. */
static bool hold_bytes(os64_html_parser_t *p, const unsigned char *s, size_t n)
{
    if (n > p->hold_cap - p->hold_len) {
        size_t cap = p->hold_cap ? p->hold_cap : 4096;
        while (cap - p->hold_len < n)
            cap = cap > SIZE_MAX / 2 ? SIZE_MAX : cap * 2;
        unsigned char *grown = os64_malloc(cap);
        if (!grown) {
            h_refuse(p, OS64_HTML_NO_MEMORY);
            return false;
        }
        for (size_t i = 0; i < p->hold_len; i++)
            grown[i] = p->hold[i];
        os64_free(p->hold);
        p->hold = grown;
        p->hold_cap = cap;
    }
    for (size_t i = 0; i < n; i++)
        p->hold[p->hold_len + i] = s[i];
    p->hold_len += n;
    return true;
}
/* What a call answers. The end of the input, said by the host or met at
 * `max_bytes`, is acted on here, once nothing is waiting to be parsed: a
 * parse that is not stopped and not refused has parsed all it was given. */
static int64_t settle(os64_html_parser_t *p)
{
    HDoc *d = p->d;
    if ((p->cut || p->ended) && !d->pub.refusal) {
        /* A document shorter than the sniff window is parsed here, and may
         * stop like any other. */
        if (!p->started)
            h_encoding_start(p);
        if (!p->script) {
            h_decode_finish(p);
            if (p->cut) {
                /* The end is parsed before the byte refusal is recorded, so a
                 * tag or character reference cut by the limit has prefix-EOF
                 * semantics. */
                d->pub.truncated = true;
                h_refuse(p, OS64_HTML_TOO_LARGE);
            }
        }
    }
    return d->pub.refusal ? d->pub.refusal : p->script ? OS64_HTML_SCRIPT : OS64_HTML_OK;
}
int64_t os64_html_parser_feed(os64_html_parser_t *p, const void *bytes, size_t len)
{
    if (!p)
        return OS64_HTML_NO_MEMORY;
    HDoc *d = p->d;
    if (d->pub.refusal)
        return d->pub.refusal;
    if (p->ended)
        return OS64_HTML_BAD_ARGUMENT;
    p->moved = false;
    const unsigned char *s = bytes;
    size_t left = p->opt.max_bytes - d->pub.input_bytes;
    size_t take = len < left ? len : left;
    /* Past the sniff window and not stopped, the bytes are parsed as they
     * come. */
    if (take && p->started && !p->script)
        h_moving(p);
    for (size_t i = 0; i < take && !d->pub.refusal;) {
        if (p->script) {
            if (hold_bytes(p, s + i, take - i))
                d->pub.input_bytes += take - i;
            break;
        }
        d->pub.input_bytes++;
        if (!p->started) {
            p->prescan[p->prescan_len++] = s[i++];
            if (p->prescan_len == sizeof(p->prescan))
                h_encoding_start(p);
        } else
            h_decode(p, s[i++], p->parsed++);
    }
    if (len > left)
        p->cut = true;
    return settle(p);
}
os64_html_node_t *os64_html_parser_script(const os64_html_parser_t *p)
{
    return p ? p->script : NULL;
}
int64_t os64_html_parser_resume(os64_html_parser_t *p)
{
    if (!p)
        return OS64_HTML_NO_MEMORY;
    if (p->d->pub.refusal)
        return p->d->pub.refusal;
    if (!p->script)
        return OS64_HTML_BAD_ARGUMENT;
    p->moved = false;
    p->script = NULL;
    h_pump(p);
    return settle(p);
}
int64_t os64_html_parser_end(os64_html_parser_t *p)
{
    if (!p)
        return OS64_HTML_NO_MEMORY;
    if (p->d->pub.refusal)
        return p->d->pub.refusal;
    p->moved = false;
    p->ended = true;
    return settle(p);
}
os64_html_document_t *os64_html_parser_document(os64_html_parser_t *p)
{
    return p ? &p->d->pub : NULL;
}
/* The parse is over and the document is the caller's. Scratch has the same
 * ownership ledger as nodes; freeing these buffers at transfer leaves node
 * strings and attributes alive with the document. */
static os64_html_document_t *release(os64_html_parser_t *p)
{
    HDoc *d = p->d;
    d_seat_html(d);
    HBuf *buffers[] = {&p->token.name, &p->token.data,   &p->token.public_id, &p->token.system_id,
                       &p->attr_name,  &p->attr_value,   &p->temporary,       &p->last_start,
                       &p->table_text, &p->charset_label};
    for (size_t i = 0; i < H_ARRAY(buffers); i++)
        h_free(d, buffers[i]->s);
    hold_free(p);
    h_free(d, p->stack.v);
    h_free(d, p->formatting.v);
    h_free(d, p->templates);
    h_free(d, p);
    return &d->pub;
}
os64_html_document_t *os64_html_parser_finish(os64_html_parser_t *p)
{
    if (!p)
        return NULL;
    /* The input has ended and finish does not stop: what is held is parsed
     * straight through, a script in it included. */
    p->moved = false;
    p->ended = p->straight = true;
    p->script = NULL;
    h_pump(p);
    settle(p);
    return release(p);
}
os64_html_document_t *os64_html_parser_abandon(os64_html_parser_t *p)
{
    return p ? release(p) : NULL;
}
