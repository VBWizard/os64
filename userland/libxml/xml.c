#include "internal.h"

typedef union {
    max_align_t alignment;
    size_t size;
} allocation;
typedef struct {
    xml_parser_t *p;
    xml_slice_t s;
    size_t at;
} cursor;
static const char xml_uri[] = "http://www.w3.org/XML/1998/namespace";
static const char xmlns_uri[] = "http://www.w3.org/2000/xmlns/";

static void copy(void *to, const void *from, size_t n)
{
    unsigned char *d = to;
    const unsigned char *s = from;
    while (n--)
        *d++ = *s++;
}
static void zero(void *to, size_t n)
{
    unsigned char *d = to;
    while (n--)
        *d++ = 0;
}
static size_t length(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return n;
}
static xml_slice_t slice(const char *s, size_t n)
{
    return (xml_slice_t){s, n};
}
static xml_slice_t literal(const char *s)
{
    return slice(s, length(s));
}
static bool equal(xml_slice_t a, xml_slice_t b)
{
    if (a.size != b.size)
        return false;
    for (size_t i = 0; i < a.size; ++i)
        if (a.data[i] != b.data[i])
            return false;
    return true;
}
static bool is(xml_slice_t a, const char *s)
{
    return equal(a, literal(s));
}
static bool fail(xml_parser_t *p, xml_status_t status)
{
    if (p->status == XML_NEED_INPUT) {
        p->status = status;
        p->error_location = p->token_location;
    }
    return false;
}
static bool charge(xml_parser_t *p, size_t *value, size_t limit, size_t n)
{
    if (n > limit || *value > limit - n)
        return fail(p, XML_LIMIT);
    *value += n;
    return true;
}
static bool work(xml_parser_t *p, size_t n)
{
    if (p->status != XML_NEED_INPUT)
        return false;
    if (p->options.cancelled && p->options.cancelled(p->options.context))
        return fail(p, XML_CANCELLED);
    return charge(p, &p->stats.work, p->options.limits.work, n);
}
static bool same(xml_parser_t *p, xml_slice_t a, xml_slice_t b)
{
    return work(p, a.size < b.size ? a.size + 1 : b.size + 1) && equal(a, b);
}
static void *allocate(xml_parser_t *p, size_t n)
{
    if (n > SIZE_MAX - sizeof(allocation)) {
        fail(p, XML_LIMIT);
        return NULL;
    }
    size_t total = n + sizeof(allocation);
    if (!work(p, 1) || !charge(p, &p->stats.live_bytes, p->options.limits.live_bytes, total))
        return NULL;
    allocation *a = p->options.memory.allocate(p->options.memory.context, total);
    if (!a) {
        p->stats.live_bytes -= total;
        fail(p, XML_NO_MEMORY);
        return NULL;
    }
    a->size = total;
    if (p->stats.peak_bytes < p->stats.live_bytes)
        p->stats.peak_bytes = p->stats.live_bytes;
    return a + 1;
}
static void release(xml_parser_t *p, void *data)
{
    if (!data)
        return;
    allocation *a = (allocation *)data - 1;
    size_t n = a->size;
    p->stats.live_bytes -= n;
    p->options.memory.release(p->options.memory.context, a, n);
}
static bool append(xml_parser_t *p, buffer *b, const char *s, size_t n)
{
    if (!work(p, n))
        return false;
    if (!n)
        return true;
    if (n > p->options.limits.token_bytes || b->size > p->options.limits.token_bytes - n)
        return fail(p, XML_LIMIT);
    size_t wanted = b->size + n;
    if (wanted > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 64;
        while (cap < wanted) {
            if (cap > p->options.limits.token_bytes / 2) {
                cap = p->options.limits.token_bytes;
                break;
            }
            cap *= 2;
        }
        if (cap > p->options.limits.token_bytes)
            cap = p->options.limits.token_bytes;
        char *next = allocate(p, cap);
        if (!next)
            return false;
        if (!work(p, b->size)) {
            release(p, next);
            return false;
        }
        copy(next, b->data, b->size);
        release(p, b->data);
        b->data = next;
        b->capacity = cap;
    }
    copy(b->data + b->size, s, n);
    b->size = wanted;
    return true;
}
static bool utf8(xml_parser_t *p, buffer *b, uint32_t c)
{
    char out[4];
    size_t n;
    if (c < 0x80) {
        out[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        out[0] = (char)(0xc0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 63));
        n = 2;
    } else if (c < 0x10000) {
        out[0] = (char)(0xe0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 63));
        out[2] = (char)(0x80 | (c & 63));
        n = 3;
    } else {
        out[0] = (char)(0xf0 | (c >> 18));
        out[1] = (char)(0x80 | ((c >> 12) & 63));
        out[2] = (char)(0x80 | ((c >> 6) & 63));
        out[3] = (char)(0x80 | (c & 63));
        n = 4;
    }
    return append(p, b, out, n);
}
static bool character(uint32_t c)
{
    return c == 9 || c == 10 || c == 13 || (c >= 32 && c <= 0xd7ff) ||
           (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff);
}
/* Token bytes have already passed the incremental decoder. */
static uint32_t scalar_at(xml_slice_t s, size_t *i)
{
    unsigned c = (unsigned char)s.data[(*i)++];
    unsigned n = c < 128 ? 0 : c < 224 ? 1 : c < 240 ? 2 : 3;
    uint32_t r = n ? c & ((1u << (6 - n)) - 1) : c;
    while (n--)
        r = (r << 6) | ((unsigned char)s.data[(*i)++] & 63);
    return r;
}
static bool name_start(uint32_t c)
{
    return c == ':' || c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= 0xc0 && c <= 0xd6) || (c >= 0xd8 && c <= 0xf6) || (c >= 0xf8 && c <= 0x2ff) ||
           (c >= 0x370 && c <= 0x37d) || (c >= 0x37f && c <= 0x1fff) ||
           (c >= 0x200c && c <= 0x200d) || (c >= 0x2070 && c <= 0x218f) ||
           (c >= 0x2c00 && c <= 0x2fef) || (c >= 0x3001 && c <= 0xd7ff) ||
           (c >= 0xf900 && c <= 0xfdcf) || (c >= 0xfdf0 && c <= 0xfffd) ||
           (c >= 0x10000 && c <= 0xeffff);
}
static bool name_char(uint32_t c)
{
    return name_start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xb7 ||
           (c >= 0x300 && c <= 0x36f) || (c >= 0x203f && c <= 0x2040);
}
static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
static bool skip(cursor *c)
{
    size_t start = c->at;
    while (c->at < c->s.size && space(c->s.data[c->at])) {
        if (!work(c->p, 1))
            return false;
        ++c->at;
    }
    return work(c->p, c->at - start + 1);
}
static bool spaces(cursor *c)
{
    size_t before = c->at;
    return skip(c) && (c->at > before || fail(c->p, XML_MALFORMED));
}
static bool take(cursor *c, const char *s)
{
    size_t n = length(s);
    if (n > c->s.size - c->at || !equal(slice(c->s.data + c->at, n), slice(s, n)))
        return false;
    c->at += n;
    return work(c->p, n);
}
static bool need(cursor *c, const char *s)
{
    return take(c, s) || fail(c->p, XML_MALFORMED);
}
static bool name(cursor *c, xml_slice_t *out, bool qname)
{
    size_t start = c->at, colon = SIZE_MAX;
    bool first = true;
    while (c->at < c->s.size) {
        if (!work(c->p, 1))
            return false;
        size_t next = c->at;
        uint32_t cp = scalar_at(c->s, &next);
        if (!(first ? name_start(cp) : name_char(cp)))
            break;
        if (qname && cp == ':') {
            if (first || colon != SIZE_MAX)
                return fail(c->p, XML_MALFORMED);
            colon = c->at;
        } else if (qname && colon != SIZE_MAX && c->at == colon + 1 && !name_start(cp))
            return fail(c->p, XML_MALFORMED);
        c->at = next;
        first = false;
    }
    if (first || (qname && colon == c->at - 1))
        return fail(c->p, XML_MALFORMED);
    *out = slice(c->s.data + start, c->at - start);
    return out->size <= c->p->options.limits.name_bytes ? work(c->p, out->size)
                                                        : fail(c->p, XML_LIMIT);
}
static bool quoted(cursor *c, xml_slice_t *out)
{
    if (c->at == c->s.size || (c->s.data[c->at] != '\'' && c->s.data[c->at] != '"'))
        return fail(c->p, XML_MALFORMED);
    char q = c->s.data[c->at++];
    size_t start = c->at;
    while (c->at < c->s.size && c->s.data[c->at] != q) {
        if (!work(c->p, 1))
            return false;
        ++c->at;
    }
    if (c->at == c->s.size)
        return fail(c->p, XML_MALFORMED);
    *out = slice(c->s.data + start, c->at - start);
    ++c->at;
    return work(c->p, out->size + 2);
}
static bool emit(xml_parser_t *p, xml_event_t *e)
{
    if (!work(p, 1))
        return false;
    e->location = p->token_location;
    e->in_doctype = p->in_doctype;
    ++p->stats.events;
    return !p->options.event || p->options.event(p->options.context, e) ||
           fail(p, XML_CONSUMER_ABORT);
}
static bool has(xml_slice_t s, const char *needle)
{
    size_t n = length(needle);
    for (size_t i = 0; n <= s.size && i <= s.size - n; ++i)
        if (equal(slice(s.data + i, n), slice(needle, n)))
            return true;
    return false;
}
static bool suffix(buffer *b, const char *s)
{
    size_t n = length(s);
    return n <= b->size && equal(slice(b->data + b->size - n, n), slice(s, n));
}
static bool prefix(buffer *b, const char *s)
{
    size_t n = length(s);
    return n <= b->size && equal(slice(b->data, n), slice(s, n));
}
static bool predefined(xml_slice_t s, uint32_t *cp)
{
    if (is(s, "lt"))
        *cp = '<';
    else if (is(s, "gt"))
        *cp = '>';
    else if (is(s, "amp"))
        *cp = '&';
    else if (is(s, "apos"))
        *cp = '\'';
    else if (is(s, "quot"))
        *cp = '"';
    else
        return false;
    return true;
}
/* Numeric references are decoded once. Named predefined references in an
   entity declaration stay lexical until that replacement text is used. */
static bool reference(cursor *c, xml_slice_t *ref, uint32_t *cp, bool *numeric)
{
    if (!need(c, "&"))
        return false;
    size_t start = c->at;
    *numeric = take(c, "#");
    if (*numeric) {
        unsigned base = take(c, "x") ? 16 : 10;
        uint32_t value = 0;
        size_t digits = 0;
        while (c->at < c->s.size && c->s.data[c->at] != ';') {
            if (!work(c->p, 1))
                return false;
            unsigned char ch = (unsigned char)c->s.data[c->at++];
            unsigned digit;
            if (ch >= '0' && ch <= '9')
                digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                digit = ch - 'A' + 10;
            else
                return fail(c->p, XML_MALFORMED);
            if (digit >= base || value > (0x10ffff - digit) / base)
                return fail(c->p, XML_MALFORMED);
            value = value * base + digit;
            ++digits;
        }
        if (!digits || !character(value))
            return fail(c->p, XML_MALFORMED);
        *cp = value;
        *ref = slice(c->s.data + start, c->at - start);
    } else if (!name(c, ref, false) || has(*ref, ":"))
        return fail(c->p, XML_MALFORMED);
    return need(c, ";") && work(c->p, c->at - start);
}
static bool expansion_bytes(xml_parser_t *p, size_t n)
{
    return charge(p, &p->stats.expanded_bytes, p->options.limits.expanded_bytes, n);
}
static bool expand(xml_parser_t *p, xml_slice_t s, buffer *out, bool attribute, bool replacement)
{
    cursor c = {p, s, 0};
    if (!work(p, s.size))
        return false;
    if (!attribute && has(s, "]]>"))
        return fail(p, XML_MALFORMED);
    while (c.at < s.size && p->status == XML_NEED_INPUT) {
        if (s.data[c.at] == '<')
            return fail(p, replacement ? XML_UNSUPPORTED : XML_MALFORMED);
        if (s.data[c.at] == '&') {
            xml_slice_t ref;
            uint32_t cp = 0;
            bool numeric;
            if (!reference(&c, &ref, &cp, &numeric))
                return false;
            if (numeric || predefined(ref, &cp)) {
                if (!expansion_bytes(p, cp < 128     ? 1
                                        : cp < 2048  ? 2
                                        : cp < 65536 ? 3
                                                     : 4) ||
                    !utf8(p, out, cp))
                    return false;
            } else {
                if (replacement)
                    return fail(p, XML_UNSUPPORTED);
                entity *e = p->entities;
                while (e && !same(p, e->name, ref) && p->status == XML_NEED_INPUT)
                    e = e->next;
                if (p->status != XML_NEED_INPUT)
                    return false;
                if (!e)
                    return fail(p, p->external_subset && !p->standalone ? XML_UNSUPPORTED
                                                                        : XML_MALFORMED);
                if (!expand(p, e->value, out, attribute, true))
                    return false;
            }
        } else {
            size_t start = c.at;
            uint32_t cp = scalar_at(s, &c.at);
            if (attribute && (cp == 9 || cp == 10 || cp == 13)) {
                if (!expansion_bytes(p, 1) || !append(p, out, " ", 1))
                    return false;
            } else if (!expansion_bytes(p, c.at - start) ||
                       !append(p, out, s.data + start, c.at - start))
                return false;
        }
    }
    return p->status == XML_NEED_INPUT;
}
static bool expanded(xml_parser_t *p, xml_slice_t s, buffer *out, bool attr)
{
    return expand(p, s, out, attr, false);
}
static xml_name_t split(xml_slice_t q)
{
    xml_name_t n = {0};
    n.local = q;
    for (size_t i = 0; i < q.size; ++i)
        if (q.data[i] == ':') {
            n.prefix = slice(q.data, i);
            n.local = slice(q.data + i + 1, q.size - i - 1);
            break;
        }
    return n;
}
static bool resolve(xml_parser_t *p, xml_slice_t q, bool attr, xml_name_t *out)
{
    *out = split(q);
    if (is(out->prefix, "xmlns"))
        return fail(p, XML_MALFORMED);
    if (is(out->prefix, "xml")) {
        out->uri = literal(xml_uri);
        return true;
    }
    if (attr && !out->prefix.size)
        return true;
    for (binding *b = p->bindings; b && p->status == XML_NEED_INPUT; b = b->next) {
        if (same(p, b->prefix, out->prefix)) {
            out->uri = b->uri;
            return true;
        }
    }
    return p->status == XML_NEED_INPUT && (!out->prefix.size || fail(p, XML_MALFORMED));
}
static void pop(xml_parser_t *p)
{
    frame *f = p->top;
    while (p->bindings != f->scope) {
        binding *b = p->bindings;
        p->bindings = b->next;
        release(p, b);
        --p->binding_count;
    }
    p->top = f->parent;
    release(p, f);
    --p->stats.depth;
    if (!p->top)
        p->root_closed = true;
}
static bool bind(xml_parser_t *p, xml_slice_t pre, xml_slice_t uri)
{
    if (is(pre, "xmlns") || is(uri, xmlns_uri) || (is(pre, "xml") != is(uri, xml_uri)) ||
        (pre.size && !uri.size))
        return fail(p, XML_MALFORMED);
    if (p->binding_count == p->options.limits.namespaces)
        return fail(p, XML_LIMIT);
    if (pre.size > SIZE_MAX - sizeof(binding) - uri.size)
        return fail(p, XML_LIMIT);
    binding *b = allocate(p, sizeof(*b) + pre.size + uri.size);
    if (!b)
        return false;
    char *data = (char *)(b + 1);
    copy(data, pre.data, pre.size);
    copy(data + pre.size, uri.data, uri.size);
    b->prefix = slice(data, pre.size);
    b->uri = slice(data + pre.size, uri.size);
    b->next = p->bindings;
    p->bindings = b;
    ++p->binding_count;
    return true;
}
typedef struct {
    xml_slice_t qname;
    buffer value;
    bool ns;
} raw_attribute;
static bool start_tag(xml_parser_t *p, cursor *c)
{
    xml_slice_t q;
    raw_attribute *raw = NULL;
    xml_attribute_t *attrs = NULL;
    xml_namespace_t *namespaces = NULL;
    size_t count = 0, capacity = 0, ac = 0, nc = 0;
    bool empty = false, ok = false;
    if (p->root_closed || !name(c, &q, true))
        return fail(p, XML_MALFORMED);
    if (p->stats.depth == p->options.limits.depth)
        return fail(p, XML_LIMIT);
    frame *f = allocate(p, sizeof(*f) + q.size);
    if (!f)
        return false;
    zero(f, sizeof(*f));
    copy(f + 1, q.data, q.size);
    f->qname = slice((char *)(f + 1), q.size);
    f->parent = p->top;
    f->scope = p->bindings;
    p->top = f;
    ++p->stats.depth;
    if (p->stats.peak_depth < p->stats.depth)
        p->stats.peak_depth = p->stats.depth;
    while (c->at < c->s.size) {
        size_t before = c->at;
        if (!skip(c))
            goto done;
        if (take(c, "/>")) {
            empty = true;
            break;
        }
        if (take(c, ">"))
            break;
        if (c->at == before) {
            fail(p, XML_MALFORMED);
            goto done;
        }
        if (count == p->options.limits.attributes) {
            fail(p, XML_LIMIT);
            goto done;
        }
        if (count == capacity) {
            size_t cap = capacity ? capacity * 2 : 8;
            if (cap < capacity || cap > SIZE_MAX / sizeof(*raw)) {
                fail(p, XML_LIMIT);
                goto done;
            }
            raw_attribute *next = allocate(p, cap * sizeof(*raw));
            if (!next)
                goto done;
            zero(next, cap * sizeof(*raw));
            copy(next, raw, count * sizeof(*raw));
            release(p, raw);
            raw = next;
            capacity = cap;
        }
        raw_attribute *r = &raw[count++];
        xml_slice_t value;
        if (!name(c, &r->qname, true) || !skip(c) || !need(c, "=") || !skip(c) ||
            !quoted(c, &value) || !expanded(p, value, &r->value, true))
            goto done;
        for (size_t i = 0; i + 1 < count; ++i)
            if (same(p, raw[i].qname, r->qname)) {
                fail(p, XML_MALFORMED);
                goto done;
            }
        if (p->status != XML_NEED_INPUT)
            goto done;
        xml_name_t n = split(r->qname);
        r->ns = is(r->qname, "xmlns") || is(n.prefix, "xmlns");
        if (r->ns) {
            xml_slice_t pre = is(r->qname, "xmlns") ? slice(NULL, 0) : n.local;
            if (!bind(p, pre, slice(r->value.data, r->value.size)))
                goto done;
            ++nc;
        } else
            ++ac;
    }
    if (c->at != c->s.size || !resolve(p, f->qname, false, &f->name))
        goto done;
    if (ac) {
        attrs = allocate(p, ac * sizeof(*attrs));
        if (!attrs)
            goto done;
    }
    if (nc) {
        namespaces = allocate(p, nc * sizeof(*namespaces));
        if (!namespaces)
            goto done;
    }
    ac = nc = 0;
    for (size_t i = 0; i < count; ++i) {
        raw_attribute *r = &raw[i];
        xml_slice_t value = slice(r->value.data, r->value.size);
        if (r->ns) {
            xml_name_t n = split(r->qname);
            namespaces[nc++] =
                (xml_namespace_t){is(r->qname, "xmlns") ? slice(NULL, 0) : n.local, value};
        } else {
            xml_attribute_t *a = &attrs[ac];
            a->value = value;
            if (!resolve(p, r->qname, true, &a->name))
                goto done;
            for (size_t j = 0; j < ac; ++j)
                if (same(p, attrs[j].name.local, a->name.local) &&
                    same(p, attrs[j].name.uri, a->name.uri)) {
                    fail(p, XML_MALFORMED);
                    goto done;
                }
            ++ac;
        }
    }
    p->root_seen = true;
    xml_event_t e = {.kind = XML_START_ELEMENT,
                     .name = f->name,
                     .attributes = attrs,
                     .attribute_count = ac,
                     .namespaces = namespaces,
                     .namespace_count = nc};
    if (!emit(p, &e))
        goto done;
    if (empty) {
        e = (xml_event_t){.kind = XML_END_ELEMENT, .name = f->name};
        if (!emit(p, &e))
            goto done;
        pop(p);
    }
    ok = true;
done:
    for (size_t i = 0; i < count; ++i)
        release(p, raw[i].value.data);
    release(p, raw);
    release(p, attrs);
    release(p, namespaces);
    return ok;
}
static bool end_tag(xml_parser_t *p, cursor *c)
{
    xml_slice_t q;
    if (!name(c, &q, true) || !skip(c) || !need(c, ">") || c->at != c->s.size || !p->top ||
        !same(p, p->top->qname, q))
        return fail(p, XML_MALFORMED);
    xml_event_t e = {.kind = XML_END_ELEMENT, .name = p->top->name};
    if (!emit(p, &e))
        return false;
    pop(p);
    return true;
}
static bool insensitive(xml_slice_t s, const char *word)
{
    if (s.size != length(word))
        return false;
    for (size_t i = 0; i < s.size; ++i) {
        char c = s.data[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != word[i])
            return false;
    }
    return true;
}
static bool pseudo(cursor *c, const char *key, xml_slice_t *value)
{
    return need(c, key) && skip(c) && need(c, "=") && skip(c) && quoted(c, value);
}
static bool declaration(xml_parser_t *p, cursor *c)
{
    xml_event_t e = {.kind = XML_DECLARATION, .standalone = -1};
    if (!p->declaration_allowed || !spaces(c) || !pseudo(c, "version", &e.version))
        return fail(p, XML_MALFORMED);
    if (!is(e.version, "1.0")) {
        if (e.version.size < 3 || e.version.data[0] != '1' || e.version.data[1] != '.')
            return fail(p, XML_MALFORMED);
        for (size_t i = 2; i < e.version.size; ++i)
            if (e.version.data[i] < '0' || e.version.data[i] > '9')
                return fail(p, XML_MALFORMED);
        return fail(p, XML_UNSUPPORTED);
    }
    size_t before = c->at;
    if (!skip(c))
        return false;
    if (c->at < c->s.size && c->s.data[c->at] == 'e') {
        if (before == c->at || !pseudo(c, "encoding", &e.encoding))
            return fail(p, XML_MALFORMED);
        for (size_t i = 0; i < e.encoding.size; ++i) {
            char ch = e.encoding.data[i];
            bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            if (!letter &&
                (!i || !((ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')))
                return fail(p, XML_MALFORMED);
        }
        if (!e.encoding.size)
            return fail(p, XML_MALFORMED);
        if (!append(p, &p->label, e.encoding.data, e.encoding.size))
            return false;
        bool u8 = insensitive(e.encoding, "utf-8"), u16 = insensitive(e.encoding, "utf-16");
        bool le = insensitive(e.encoding, "utf-16le"), be = insensitive(e.encoding, "utf-16be");
        if (u8 || u16 || le || be) {
            if ((u8 && p->encoding != 1) || (u16 && (p->encoding == 1 || !p->bom)) ||
                (le && p->encoding != 2) || (be && p->encoding != 3))
                return fail(p, XML_MALFORMED);
        } else
            return fail(p, p->bom || p->encoding != 1 ? XML_MALFORMED : XML_UNSUPPORTED_ENCODING);
        before = c->at;
        if (!skip(c))
            return false;
    }
    if (c->at < c->s.size && c->s.data[c->at] == 's') {
        xml_slice_t s;
        if (before == c->at || !pseudo(c, "standalone", &s))
            return fail(p, XML_MALFORMED);
        if (is(s, "yes"))
            e.standalone = 1;
        else if (is(s, "no"))
            e.standalone = 0;
        else
            return fail(p, XML_MALFORMED);
        if (!skip(c))
            return false;
    }
    if (!p->bom && p->encoding != 1 && !e.encoding.size)
        return fail(p, XML_MALFORMED);
    p->standalone = e.standalone == 1;
    return need(c, "?>") && c->at == c->s.size && emit(p, &e);
}
static bool comment(xml_parser_t *p, xml_slice_t text)
{
    if (has(text, "--") || (text.size && text.data[text.size - 1] == '-'))
        return fail(p, XML_MALFORMED);
    xml_event_t e = {.kind = XML_COMMENT, .text = text};
    return work(p, text.size) && emit(p, &e);
}
static bool pi(xml_parser_t *p, cursor *c, bool dtd)
{
    xml_slice_t target;
    if (!name(c, &target, false) || has(target, ":"))
        return fail(p, XML_MALFORMED);
    if (insensitive(target, "xml")) {
        if (!is(target, "xml") || dtd)
            return fail(p, XML_MALFORMED);
        return declaration(p, c);
    }
    xml_event_t e = {.kind = XML_PI, .target = target};
    if (take(c, "?>"))
        return emit(p, &e);
    if (!spaces(c))
        return false;
    size_t start = c->at;
    while (c->at + 1 < c->s.size && !(c->s.data[c->at] == '?' && c->s.data[c->at + 1] == '>'))
        ++c->at;
    e.text = slice(c->s.data + start, c->at - start);
    return work(p, e.text.size) && need(c, "?>") && emit(p, &e);
}
static bool entity_declaration(xml_parser_t *p, cursor *c)
{
    xml_slice_t n, value;
    buffer replacement = {0};
    bool ok = false;
    if (p->entity_count == p->options.limits.entities)
        return fail(p, XML_LIMIT);
    ++p->entity_count;
    if (!spaces(c))
        return false;
    if (take(c, "%"))
        return fail(p, XML_UNSUPPORTED);
    if (!name(c, &n, false) || has(n, ":") || !spaces(c))
        return fail(p, XML_MALFORMED);
    if (c->at < c->s.size && (c->s.data[c->at] == 'S' || c->s.data[c->at] == 'P'))
        return fail(p, XML_UNSUPPORTED);
    if (!quoted(c, &value) || !skip(c) || !need(c, ">"))
        return false;
    cursor v = {p, value, 0};
    while (v.at < value.size) {
        size_t start = v.at;
        char ch = value.data[v.at];
        if (ch == '%' || ch == '<') {
            fail(p, XML_UNSUPPORTED);
            goto done;
        }
        if (ch == '&') {
            xml_slice_t ref;
            uint32_t cp = 0;
            bool numeric;
            if (!reference(&v, &ref, &cp, &numeric))
                goto done;
            if (numeric) {
                if (!utf8(p, &replacement, cp))
                    goto done;
            } else {
                if (!predefined(ref, &cp)) {
                    fail(p, XML_UNSUPPORTED);
                    goto done;
                }
                if (!append(p, &replacement, value.data + start, v.at - start))
                    goto done;
            }
        } else {
            ++v.at;
            if (!append(p, &replacement, &ch, 1))
                goto done;
        }
    }
    uint32_t predefined_cp;
    if (predefined(n, &predefined_cp)) {
        /* Reserved entities may only be declared with their specified
           replacement. lt and amp require a reference, not a literal. */
        bool ref_required = predefined_cp == '<' || predefined_cp == '&';
        bool valid = false;
        if (replacement.size == 1 && !ref_required)
            valid = (unsigned char)replacement.data[0] == predefined_cp;
        else {
            cursor r = {p, slice(replacement.data, replacement.size), 0};
            xml_slice_t ref;
            uint32_t cp = 0;
            bool numeric = false;
            valid = reference(&r, &ref, &cp, &numeric) && numeric && cp == predefined_cp &&
                    r.at == r.s.size;
        }
        if (!valid) {
            fail(p, XML_MALFORMED);
            goto done;
        }
        ok = true;
        goto done;
    }
    if (has(slice(replacement.data, replacement.size), "<")) {
        fail(p, XML_UNSUPPORTED);
        goto done;
    }
    for (entity *e = p->entities; e && p->status == XML_NEED_INPUT; e = e->next)
        if (same(p, e->name, n)) {
            ok = true;
            goto done;
        }
    if (p->status != XML_NEED_INPUT)
        goto done;
    if (n.size > SIZE_MAX - sizeof(entity) - replacement.size) {
        fail(p, XML_LIMIT);
        goto done;
    }
    entity *e = allocate(p, sizeof(*e) + n.size + replacement.size);
    if (!e)
        goto done;
    char *data = (char *)(e + 1);
    copy(data, n.data, n.size);
    copy(data + n.size, replacement.data, replacement.size);
    e->name = slice(data, n.size);
    e->value = slice(data + n.size, replacement.size);
    e->next = p->entities;
    p->entities = e;
    ok = true;
done:
    release(p, replacement.data);
    return ok;
}
static bool doctype(xml_parser_t *p, cursor *c)
{
    if (p->root_seen || p->doctype_seen)
        return fail(p, XML_MALFORMED);
    p->doctype_seen = true;
    xml_event_t e = {.kind = XML_DOCTYPE};
    if (!spaces(c) || !name(c, &e.target, true))
        return false;
    size_t before = c->at;
    if (!skip(c))
        return false;
    bool separated = c->at > before;
    bool system = take(c, "SYSTEM"), public = false;
    if (!system)
    public = take(c, "PUBLIC");
    if (system || public) {
        p->external_subset = true;
        if (!separated || !spaces(c))
            return fail(p, XML_MALFORMED);
        if (public) {
            if (!quoted(c, &e.public_id) || !spaces(c))
                return false;
            for (size_t i = 0; i < e.public_id.size; ++i) {
                char ch = e.public_id.data[i];
                if (ch != ' ' && ch != '\r' && ch != '\n' && !(ch >= 'a' && ch <= 'z') &&
                    !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') &&
                    !has(literal("-'()+,./:=?;!*#@$_%"), (char[]){ch, 0}))
                    return fail(p, XML_MALFORMED);
            }
        }
        if (!quoted(c, &e.system_id) || has(e.system_id, "#") || !skip(c))
            return fail(p, XML_MALFORMED);
    }
    if (!emit(p, &e))
        return false;
    if (take(c, "[")) {
        p->in_doctype = true;
        while (p->status == XML_NEED_INPUT) {
            if (!skip(c))
                return false;
            if (take(c, "]"))
                break;
            if (take(c, "<!ENTITY")) {
                if (!entity_declaration(p, c))
                    return false;
            } else if (take(c, "<!--")) {
                size_t start = c->at;
                while (c->at + 2 < c->s.size && !equal(slice(c->s.data + c->at, 3), literal("-->")))
                    ++c->at;
                if (!comment(p, slice(c->s.data + start, c->at - start)) || !need(c, "-->"))
                    return false;
            } else if (take(c, "<?")) {
                if (!pi(p, c, true))
                    return false;
            } else
                return fail(p, XML_UNSUPPORTED);
        }
        if (!skip(c))
            return false;
    }
    p->in_doctype = false;
    return need(c, ">") && c->at == c->s.size;
}
static bool token(xml_parser_t *p)
{
    xml_slice_t s = slice(p->token.data, p->token.size);
    cursor c = {p, s, 0};
    bool ok;
    if (!work(p, s.size))
        return false;
    /* A signature selects a decoder, not permission to omit the declaration.
       Without a BOM the first UTF-16 token must declare its encoding. */
    if (!p->bom && p->encoding != 1 && p->declaration_allowed &&
        !(p->markup && prefix(&p->token, "<?xml") && s.size > 5 && space(s.data[5])))
        return fail(p, XML_MALFORMED);
    if (!p->markup) {
        if (!p->top) {
            for (size_t i = 0; i < s.size; ++i)
                if (!space(s.data[i]))
                    return fail(p, XML_MALFORMED);
            ok = true;
        } else {
            buffer value = {0};
            ok = expanded(p, s, &value, false);
            if (ok && value.size) {
                xml_event_t e = {.kind = XML_TEXT, .text = slice(value.data, value.size)};
                ok = emit(p, &e);
            }
            release(p, value.data);
        }
    } else if (take(&c, "<!--"))
        ok = s.size >= 7 && comment(p, slice(s.data + 4, s.size - 7));
    else if (take(&c, "<![CDATA[")) {
        if (!p->top || s.size < 12)
            return fail(p, XML_MALFORMED);
        xml_event_t e = {.kind = XML_CDATA, .text = slice(s.data + 9, s.size - 12)};
        ok = emit(p, &e);
    } else if (take(&c, "<?"))
        ok = pi(p, &c, false) && c.at == s.size;
    else if (take(&c, "<!DOCTYPE"))
        ok = doctype(p, &c);
    else if (take(&c, "</"))
        ok = end_tag(p, &c);
    else if (take(&c, "<!"))
        return fail(p, XML_MALFORMED);
    else
        ok = need(&c, "<") && start_tag(p, &c);
    p->declaration_allowed = false;
    if (!ok)
        return fail(p, XML_MALFORMED);
    p->token.size = 0;
    return true;
}
/* Each decoded scalar visits the terminator recognizer once. Token parsing
   happens after closure, avoiding rescans of an unfinished one-byte feed. */
static bool decoded(xml_parser_t *p, uint32_t cp, size_t byte)
{
    if (!character(cp)) {
        p->token_location = (xml_location_t){byte, p->location.line, p->location.column};
        return fail(p, XML_MALFORMED);
    }
    if (!charge(p, &p->stats.decoded_bytes, p->options.limits.decoded_bytes,
                cp < 128     ? 1
                : cp < 2048  ? 2
                : cp < 65536 ? 3
                             : 4))
        return false;
    if (p->cr && cp == 10) {
        p->cr = false;
        return true;
    }
    p->cr = cp == 13;
    if (p->cr)
        cp = 10;
    xml_location_t here = {byte, p->location.line, p->location.column};
    if (!p->markup && cp == '<') {
        if (p->token.size && !token(p))
            return false;
        p->markup = true;
        p->quote = p->subset = p->dtd_mode = 0;
    }
    if (!p->token.size)
        p->token_location = here;
    if (!utf8(p, &p->token, cp))
        return false;
    if (cp == 10) {
        ++p->location.line;
        p->location.column = 1;
    } else
        ++p->location.column;
    p->location.byte = byte;
    if (!p->markup)
        return true;
    buffer *b = &p->token;
    bool done = false;
    if (prefix(b, "<!--"))
        done = b->size >= 7 && suffix(b, "-->");
    else if (prefix(b, "<![CDATA["))
        done = suffix(b, "]]>");
    else if (prefix(b, "<?"))
        done = suffix(b, "?>");
    else if (prefix(b, "<!DOCTYPE")) {
        if (p->dtd_mode == 1) {
            if (b->size - p->dtd_comment_end >= 3 && suffix(b, "-->"))
                p->dtd_mode = 0;
        } else if (p->dtd_mode == 2) {
            if (suffix(b, "?>"))
                p->dtd_mode = 0;
        } else if (p->quote) {
            if (cp == p->quote)
                p->quote = 0;
        } else if (suffix(b, "<!--")) {
            p->dtd_mode = 1;
            p->dtd_comment_end = b->size;
        } else if (suffix(b, "<?"))
            p->dtd_mode = 2;
        else if (cp == '\'' || cp == '"')
            p->quote = cp;
        else if (cp == '[') {
            if (p->subset)
                return fail(p, XML_UNSUPPORTED);
            p->subset = 1;
        } else if (cp == ']') {
            if (!p->subset)
                return fail(p, XML_MALFORMED);
            p->subset = 0;
        } else if (cp == '>')
            done = !p->subset;
    } else {
        if (p->quote) {
            if (cp == p->quote)
                p->quote = 0;
        } else if (cp == '\'' || cp == '"')
            p->quote = cp;
        else if (cp == '>')
            done = true;
    }
    if (done) {
        if (!token(p))
            return false;
        p->markup = false;
    }
    return true;
}
static bool byte_input(xml_parser_t *p, unsigned char b, size_t offset)
{
    if (p->encoding == 1) {
        if (!p->needed) {
            p->scalar_byte = offset;
            if (b < 128)
                return decoded(p, b, offset);
            if (b >= 0xc2 && b <= 0xdf) {
                p->scalar = b & 31;
                p->needed = 1;
                p->minimum = 0x80;
            } else if (b >= 0xe0 && b <= 0xef) {
                p->scalar = b & 15;
                p->needed = 2;
                p->minimum = 0x800;
            } else if (b >= 0xf0 && b <= 0xf4) {
                p->scalar = b & 7;
                p->needed = 3;
                p->minimum = 0x10000;
            } else
                goto malformed;
        } else {
            if ((b & 0xc0) != 0x80)
                goto malformed;
            p->scalar = (p->scalar << 6) | (b & 63);
            if (!--p->needed) {
                if (p->scalar < p->minimum)
                    goto malformed;
                return decoded(p, p->scalar, p->scalar_byte);
            }
        }
    } else {
        if (!p->pending) {
            p->scalar = b;
            p->pending = 1;
            if (!p->high_surrogate)
                p->scalar_byte = offset;
        } else {
            uint32_t u = p->encoding == 2 ? p->scalar | ((uint32_t)b << 8) : (p->scalar << 8) | b;
            p->pending = 0;
            if (p->high_surrogate) {
                if (u < 0xdc00 || u > 0xdfff)
                    goto malformed;
                uint32_t cp = 0x10000 + ((p->high_surrogate - 0xd800) << 10) + (u - 0xdc00);
                p->high_surrogate = 0;
                return decoded(p, cp, p->scalar_byte);
            }
            if (u >= 0xd800 && u <= 0xdbff)
                p->high_surrogate = u;
            else if (u >= 0xdc00 && u <= 0xdfff)
                goto malformed;
            else
                return decoded(p, u, p->scalar_byte);
        }
    }
    return true;
malformed:
    p->token_location = (xml_location_t){p->scalar_byte, p->location.line, p->location.column};
    return fail(p, XML_MALFORMED);
}
static bool detect(xml_parser_t *p)
{
    size_t start = 0;
    unsigned char *b = p->probe;
    unsigned n = p->probe_count;
    p->detected = true;
    p->encoding = 1;
    if (n >= 4 &&
        ((!b[0] && !b[1] && !b[2] && b[3] == '<') || (b[0] == '<' && !b[1] && !b[2] && !b[3]) ||
         (!b[0] && !b[1] && b[2] == 0xfe && b[3] == 0xff) ||
         (b[0] == 0xff && b[1] == 0xfe && !b[2] && !b[3]) ||
         (b[0] == 0x4c && b[1] == 0x6f && b[2] == 0xa7 && b[3] == 0x94)))
        return fail(p, XML_UNSUPPORTED_ENCODING);
    if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
        start = 3;
        p->bom = true;
    } else if (n >= 2 && b[0] == 0xff && b[1] == 0xfe) {
        p->encoding = 2;
        start = 2;
        p->bom = true;
    } else if (n >= 2 && b[0] == 0xfe && b[1] == 0xff) {
        p->encoding = 3;
        start = 2;
        p->bom = true;
    } else if (n >= 4 && b[0] == '<' && !b[1] && b[2] == '?' && !b[3])
        p->encoding = 2;
    else if (n >= 4 && !b[0] && b[1] == '<' && !b[2] && b[3] == '?')
        p->encoding = 3;
    xml_event_t e = {.kind = XML_START_DOCUMENT,
                     .encoding = literal(p->encoding == 1   ? "UTF-8"
                                         : p->encoding == 2 ? "UTF-16LE"
                                                            : "UTF-16BE")};
    if (!emit(p, &e))
        return false;
    p->started = true;
    for (size_t i = start; i < n; ++i)
        if (!byte_input(p, b[i], i))
            return false;
    return true;
}
xml_limits_t xml_default_limits(void)
{
    return (xml_limits_t){.encoded_bytes = 16 * 1024 * 1024,
                          .decoded_bytes = 32 * 1024 * 1024,
                          .expanded_bytes = 32 * 1024 * 1024,
                          .live_bytes = 8 * 1024 * 1024,
                          .work = 128 * 1024 * 1024,
                          .depth = 256,
                          .token_bytes = 1024 * 1024,
                          .name_bytes = 4096,
                          .attributes = 256,
                          .namespaces = 1024,
                          .entities = 256};
}
xml_status_t xml_create(const xml_options_t *options, xml_parser_t **out)
{
    if (!out)
        return XML_BAD_ARGUMENT;
    *out = NULL;
    if (!options || !options->memory.allocate || !options->memory.release)
        return XML_BAD_ARGUMENT;
    xml_options_t o = *options;
    xml_limits_t d = xml_default_limits();
#define DEFAULT(field)                                                                             \
    if (!o.limits.field)                                                                           \
    o.limits.field = d.field
    DEFAULT(encoded_bytes);
    DEFAULT(decoded_bytes);
    DEFAULT(expanded_bytes);
    DEFAULT(live_bytes);
    DEFAULT(work);
    DEFAULT(depth);
    DEFAULT(token_bytes);
    DEFAULT(name_bytes);
    DEFAULT(attributes);
    DEFAULT(namespaces);
    DEFAULT(entities);
#undef DEFAULT
    if (o.limits.live_bytes < sizeof(xml_parser_t))
        return XML_LIMIT;
    xml_parser_t *p = o.memory.allocate(o.memory.context, sizeof(*p));
    if (!p)
        return XML_NO_MEMORY;
    zero(p, sizeof(*p));
    p->options = o;
    p->stats.live_bytes = p->stats.peak_bytes = sizeof(*p);
    p->location = p->token_location = (xml_location_t){0, 1, 1};
    p->declaration_allowed = true;
    *out = p;
    return XML_NEED_INPUT;
}
xml_status_t xml_feed(xml_parser_t *p, const void *input, size_t size, size_t *consumed)
{
    if (consumed)
        *consumed = 0;
    if (!p || !consumed || (!input && size) || p->busy)
        return XML_BAD_ARGUMENT;
    if (p->status != XML_NEED_INPUT)
        return p->status == XML_DONE && size ? XML_BAD_ARGUMENT : p->status;
    p->busy = true;
    const unsigned char *bytes = input;
    while (*consumed < size && p->status == XML_NEED_INPUT) {
        if (!work(p, 1) || !charge(p, &p->stats.encoded_bytes, p->options.limits.encoded_bytes, 1))
            break;
        unsigned char b = bytes[(*consumed)++];
        size_t offset = p->stats.encoded_bytes - 1;
        if (!p->detected) {
            p->probe[p->probe_count++] = b;
            if (p->probe_count == 4)
                detect(p);
        } else
            byte_input(p, b, offset);
    }
    p->busy = false;
    return p->status;
}
xml_status_t xml_finish(xml_parser_t *p)
{
    if (!p || p->busy)
        return XML_BAD_ARGUMENT;
    if (p->status != XML_NEED_INPUT)
        return p->status;
    p->busy = true;
    if ((p->detected || detect(p)) && p->status == XML_NEED_INPUT) {
        if (p->needed || p->pending || p->high_surrogate || p->markup)
            fail(p, XML_MALFORMED);
        else if ((!p->token.size || token(p)) && p->status == XML_NEED_INPUT) {
            if (!p->root_seen || p->top)
                fail(p, XML_MALFORMED);
            else {
                p->location.byte = p->stats.encoded_bytes;
                p->token_location = p->location;
                xml_event_t e = {.kind = XML_END_DOCUMENT};
                if (emit(p, &e))
                    p->status = XML_DONE;
            }
        }
    }
    p->busy = false;
    return p->status;
}
void xml_destroy(xml_parser_t *p)
{
    if (!p || p->busy)
        return;
    while (p->top)
        pop(p);
    while (p->entities) {
        entity *e = p->entities;
        p->entities = e->next;
        release(p, e);
    }
    release(p, p->token.data);
    release(p, p->label.data);
    xml_allocator_t memory = p->options.memory;
    memory.release(memory.context, p, sizeof(*p));
}
xml_status_t xml_status(const xml_parser_t *p)
{
    return p ? p->status : XML_BAD_ARGUMENT;
}
xml_stats_t xml_stats(const xml_parser_t *p)
{
    return p ? p->stats : (xml_stats_t){0};
}
xml_location_t xml_error_location(const xml_parser_t *p)
{
    return p ? p->error_location : (xml_location_t){0};
}
xml_slice_t xml_encoding(const xml_parser_t *p)
{
    return p ? slice(p->label.data, p->label.size) : slice(NULL, 0);
}
const char *xml_status_name(xml_status_t s)
{
    static const char *const names[] = {
        "NEED_INPUT", "DONE",      "MALFORMED", "UNSUPPORTED",    "UNSUPPORTED_ENCODING",
        "LIMIT",      "NO_MEMORY", "CANCELLED", "CONSUMER_ABORT", "BAD_ARGUMENT"};
    return (unsigned)s < sizeof(names) / sizeof(*names) ? names[s] : "UNKNOWN";
}
