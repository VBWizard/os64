#include "internal.h"

/* Blocks are owned by the document, including parser scratch until finish.
 * Charging capacity and headers makes the arena limit a retained-memory bound,
 * rather than a count of requested payload bytes. Growth charges both blocks
 * while copying; the returned document reports its peak as well as live cost. */
struct HBlock {
    HBlock *prev, *next;
    size_t size;
    uint64_t alignment;
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
void *h_alloc(os64_html_parser_t *p, size_t size)
{
    HDoc *d = p->d;
    if (d->pub.refusal)
        return NULL;
    if (size > SIZE_MAX - sizeof(HBlock) ||
        size + sizeof(HBlock) > d->budget - d->pub.arena_bytes) {
        h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
        return NULL;
    }
    HBlock *b = os64_malloc(sizeof(*b) + size);
    if (!b) {
        h_refuse(p, OS64_HTML_NO_MEMORY);
        return NULL;
    }
    b->size = sizeof(*b) + size;
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
void h_free(HDoc *d, void *ptr)
{
    if (!ptr)
        return;
    HBlock *b = (HBlock *)ptr - 1;
    if (b->prev)
        b->prev->next = b->next;
    else
        d->blocks = b->next;
    if (b->next)
        b->next->prev = b->prev;
    d->pub.arena_bytes -= b->size;
    os64_free(b);
}
/* Stable nodes and strings share geometrically grown arena chunks. Scratch
 * buffers use individually releasable ledger blocks so replacing a buffer does
 * not retain every old capacity until document_free. Both charge one budget. */
void *h_permanent(os64_html_parser_t *p, size_t size)
{
    HDoc *d = p->d;
    if (d->pub.refusal)
        return NULL;
    if (size > SIZE_MAX - 15) {
        h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
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
            h_refuse(p, OS64_HTML_ARENA_EXHAUSTED);
            return NULL;
        }
        unsigned char *chunk = h_alloc(p, cap);
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
    /* Text nodes carry a private capacity word after the public node. */
    HNode *n = h_permanent(p, sizeof(*n) + sizeof(size_t));
    if (n) {
        n->kind = kind;
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
    default:
        return "OS64_HTML_UNKNOWN_STATUS";
    }
}
void os64_html_document_free(os64_html_document_t *doc)
{
    if (!doc)
        return;
    HDoc *d = (HDoc *)doc;
    while (d->blocks)
        h_free(d, d->blocks + 1);
    os64_free(d);
}
void os64_html_parser_destroy(os64_html_parser_t *p)
{
    if (p)
        os64_html_document_free(&p->d->pub);
}

os64_html_options_t os64_html_options_default(void)
{
    /* The saved Wikipedia page peaks at 4,255,104 charged bytes and 1,210,369
     * work units. 64 MiB and 100M leave about 15x/82x headroom for real pages;
     * crafted growth still refuses by name (tools/test_html_driver.c). */
    return (os64_html_options_t){NULL, 8u * 1024u * 1024u, 64u * 1024u * 1024u, 512, 100000000};
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
    d->pub.arena_bytes = d->pub.peak_arena_bytes = sizeof(*d);
    os64_html_parser_t bootstrap = {0};
    bootstrap.d = d;
    os64_html_parser_t *p = h_alloc(&bootstrap, sizeof(*p));
    if (!p) {
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
int64_t os64_html_parser_feed(os64_html_parser_t *p, const void *bytes, size_t len)
{
    if (!p)
        return OS64_HTML_NO_MEMORY;
    if (p->d->pub.refusal)
        return p->d->pub.refusal;
    const unsigned char *s = bytes;
    size_t left = p->opt.max_bytes - p->d->pub.input_bytes;
    size_t take = len < left ? len : left;
    for (size_t i = 0; i < take && !p->d->pub.refusal; i++) {
        size_t offset = p->d->pub.input_bytes++;
        if (!p->started) {
            p->prescan[p->prescan_len++] = s[i];
            if (p->prescan_len == sizeof(p->prescan))
                h_encoding_start(p);
        } else
            h_decode(p, s[i], offset);
    }
    if (len > left && !p->d->pub.refusal) {
        /* EOF must be processed before recording the byte refusal, so a tag
         * or character reference cut by the limit has prefix-EOF semantics. */
        if (!p->started)
            h_encoding_start(p);
        h_decode_finish(p);
        p->d->pub.truncated = true;
        h_refuse(p, OS64_HTML_TOO_LARGE);
    }
    return p->d->pub.refusal;
}
os64_html_document_t *os64_html_parser_finish(os64_html_parser_t *p)
{
    if (!p)
        return NULL;
    HDoc *d = p->d;
    if (!d->pub.refusal) {
        if (!p->started)
            h_encoding_start(p);
        h_decode_finish(p);
    }
    if (!d->html.parent)
        h_attach(&d->root, NULL, &d->html);
    /* Scratch has the same ownership ledger as nodes. Freeing these buffers
     * at transfer leaves node strings and attributes alive with the document. */
    HBuf *buffers[] = {&p->token.name, &p->token.data,   &p->token.public_id, &p->token.system_id,
                       &p->attr_name,  &p->attr_value,   &p->temporary,       &p->last_start,
                       &p->table_text, &p->charset_label};
    for (size_t i = 0; i < H_ARRAY(buffers); i++)
        h_free(d, buffers[i]->s);
    h_free(d, p->stack.v);
    h_free(d, p->formatting.v);
    h_free(d, p->templates);
    h_free(d, p);
    return &d->pub;
}
