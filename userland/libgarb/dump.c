// dump.c — a parse as JSON in css-parsing-tests' representation: what the
// harness diffs against the suite, and what a probe in the guest prints.

#include "internal.h"
#include "os64/fmt.h"
#include "os64/str.h"

typedef struct {
    char *out;
    size_t cap, len;
} Out;

static void put(Out *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++, o->len++)
        if (o->len + 1 < o->cap)
            o->out[o->len] = s[i];
    if (o->cap > 0)
        o->out[o->len < o->cap ? o->len : o->cap - 1] = '\0';
}

static void puts_(Out *o, const char *s)
{
    put(o, s, os64_strlen(s));
}

// A JSON string: quotes, backslashes and control bytes escaped, the rest
// (UTF-8 included) as it is.
static void str(Out *o, const char *s, size_t n)
{
    put(o, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(o, e, 2);
        } else if (c < 0x20 || c == 0x7F) {
            char e[8];
            os64_snprintf(e, sizeof(e), "\\u%04x", c);
            puts_(o, e);
        } else {
            put(o, (const char *)&c, 1);
        }
    }
    put(o, "\"", 1);
}

// A number JSON can hold: an integer as one, anything else to seventeen
// significant digits, which is a double's whole precision.
static void num(Out *o, double v)
{
    char b[48];
    if (v != v) {
        puts_(o, "NaN");
        return;
    }
    if (v > 1.7e308 || v < -1.7e308) {
        puts_(o, v > 0 ? "Infinity" : "-Infinity");
        return;
    }
    if (v == (double)(int64_t)v && v < 1e15 && v > -1e15) {
        os64_snprintf(b, sizeof(b), "%ld", (long)(int64_t)v);
        puts_(o, b);
        return;
    }
    size_t k = 0;
    if (v < 0) {
        b[k++] = '-';
        v = -v;
    }
    int32_t e = 0;
    while (v >= 10.0) {
        v /= 10.0;
        e++;
    }
    while (v < 1.0) {
        v *= 10.0;
        e--;
    }
    for (int i = 0; i < 17; i++) {
        int d = (int)v;
        if (d > 9)
            d = 9;
        b[k++] = (char)('0' + d);
        if (i == 0)
            b[k++] = '.';
        v = (v - d) * 10.0;
    }
    os64_snprintf(b + k, sizeof(b) - k, "e%d", (int)e);
    puts_(o, b);
}

static void values(Out *o, const garb_value_t *v, int32_t n);

static void kind_pair(Out *o, const char *kind, const char *text, size_t len)
{
    put(o, "[\"", 2);
    puts_(o, kind);
    put(o, "\", ", 3);
    str(o, text, len);
    put(o, "]", 1);
}

static void numeric(Out *o, const char *kind, const garb_value_t *v)
{
    put(o, "[\"", 2);
    puts_(o, kind);
    put(o, "\", ", 3);
    str(o, v->text, v->len);
    put(o, ", ", 2);
    num(o, v->number);
    puts_(o, v->integer ? ", \"integer\"" : ", \"number\"");
    if (v->kind == GARB_DIMENSION) {
        put(o, ", ", 2);
        str(o, v->unit, v->unit_len);
    }
    put(o, "]", 1);
}

// One component value, and — for a string or url cut off by the end of the
// text — the suite's error note after it, as its own entry.
static void value(Out *o, const garb_value_t *v)
{
    switch (v->kind) {
    case GARB_IDENT: kind_pair(o, "ident", v->text, v->len); break;
    case GARB_AT_KEYWORD: kind_pair(o, "at-keyword", v->text, v->len); break;
    case GARB_HASH:
        put(o, "[\"hash\", ", 9);
        str(o, v->text, v->len);
        puts_(o, v->id ? ", \"id\"]" : ", \"unrestricted\"]");
        break;
    case GARB_STRING:
        kind_pair(o, "string", v->text, v->len);
        if (v->eof)
            puts_(o, ", [\"error\", \"eof-in-string\"]");
        break;
    case GARB_BAD_STRING: puts_(o, "[\"error\", \"bad-string\"]"); break;
    case GARB_URL:
        kind_pair(o, "url", v->text, v->len);
        if (v->eof)
            puts_(o, ", [\"error\", \"eof-in-url\"]");
        break;
    case GARB_BAD_URL: puts_(o, "[\"error\", \"bad-url\"]"); break;
    case GARB_DELIM: str(o, v->text, v->len); break;
    case GARB_NUMBER: numeric(o, "number", v); break;
    case GARB_PERCENTAGE: numeric(o, "percentage", v); break;
    case GARB_DIMENSION: numeric(o, "dimension", v); break;
    case GARB_WHITESPACE: puts_(o, "\" \""); break;
    case GARB_CDO: puts_(o, "\"<!--\""); break;
    case GARB_CDC: puts_(o, "\"-->\""); break;
    case GARB_COLON: puts_(o, "\":\""); break;
    case GARB_SEMICOLON: puts_(o, "\";\""); break;
    case GARB_COMMA: puts_(o, "\",\""); break;
    case GARB_FUNCTION:
        put(o, "[\"function\", ", 13);
        str(o, v->text, v->len);
        if (v->nchildren > 0)
            put(o, ", ", 2);
        values(o, v->children, v->nchildren);
        put(o, "]", 1);
        break;
    case GARB_BLOCK: {
        char head[8];
        os64_snprintf(head, sizeof(head), "[\"%c%c\"", v->open, v->open == '{' ? '}' : v->open == '[' ? ']' : ')');
        puts_(o, head);
        if (v->nchildren > 0)
            put(o, ", ", 2);
        values(o, v->children, v->nchildren);
        put(o, "]", 1);
        break;
    }
    case GARB_UNMATCHED: {
        char e[24];
        os64_snprintf(e, sizeof(e), "[\"error\", \"%c\"]", v->open);
        puts_(o, e);
        break;
    }
    }
}

// Component values separated by commas, without the brackets around them.
static void values(Out *o, const garb_value_t *v, int32_t n)
{
    for (int32_t i = 0; i < n; i++) {
        if (i > 0)
            put(o, ", ", 2);
        value(o, &v[i]);
    }
}

static void values_list(Out *o, const garb_value_t *v, int32_t n)
{
    put(o, "[", 1);
    values(o, v, n);
    put(o, "]", 1);
}

static void rule(Out *o, const garb_rule_t *r)
{
    if (r->at) {
        put(o, "[\"at-rule\", ", 12);
        str(o, r->name, r->len);
        put(o, ", ", 2);
        values_list(o, r->prelude, r->nprelude);
        put(o, ", ", 2);
        if (r->has_block)
            values_list(o, r->block, r->nblock);
        else
            puts_(o, "null");
        put(o, "]", 1);
        return;
    }
    put(o, "[\"qualified rule\", ", 19);
    values_list(o, r->prelude, r->nprelude);
    put(o, ", ", 2);
    values_list(o, r->block, r->nblock);
    put(o, "]", 1);
}

static void decl(Out *o, const garb_decl_t *d)
{
    put(o, "[\"declaration\", ", 16);
    str(o, d->name, d->len);
    put(o, ", ", 2);
    values_list(o, d->value, d->nvalue);
    puts_(o, d->important ? ", true]" : ", false]");
}

size_t garb_dump_values(const garb_value_t *v, int32_t n, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    values_list(&o, v, n);
    return o.len;
}

size_t garb_dump_items(const garb_item_t *items, int32_t n, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    put(&o, "[", 1);
    for (int32_t i = 0; i < n; i++) {
        if (i > 0)
            put(&o, ", ", 2);
        if (items[i].kind == GARB_ITEM_DECL)
            decl(&o, &items[i].decl);
        else if (items[i].kind == GARB_ITEM_RULE)
            rule(&o, items[i].rule);
        else
            puts_(&o, "[\"error\", \"invalid\"]");
    }
    put(&o, "]", 1);
    return o.len;
}

size_t garb_dump_rule(const garb_rule_t *r, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    rule(&o, r);
    return o.len;
}

size_t garb_dump_decl(const garb_decl_t *d, char *out, size_t cap)
{
    Out o = {out, cap, 0};
    decl(&o, d);
    return o.len;
}
