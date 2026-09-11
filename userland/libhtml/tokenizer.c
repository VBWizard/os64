#include "internal.h"

typedef struct {
    const char *name;
    uint32_t a, b;
} HEntity;
static const HEntity entities[] = {
#include "entities.inc"
};
static const uint16_t numeric_high[32] = {
    0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,
    0x2039, 0x152,  0x8d,   0x17d,  0x8f,   0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
static bool alnum(uint32_t c)
{
    return h_alpha(c) || (c >= '0' && c <= '9');
}
static void character(os64_html_parser_t *p, uint32_t c)
{
    if (p->d->pub.refusal)
        return;
    HToken t = {0};
    t.type = c == H_EOF ? H_END_INPUT : H_CHAR;
    t.ch = c;
    if (p->token_sink)
        p->token_sink(p, &t, p->sink_context);
    else
        h_tree(p, &t);
}
static void literal(os64_html_parser_t *p, const char *s)
{
    while (*s && !p->d->pub.refusal)
        character(p, (unsigned char)*s++);
}
static void begin_token(os64_html_parser_t *p, HType type)
{
    HToken *t = &p->token;
    t->type = type;
    h_buf_reset(&t->name);
    h_buf_reset(&t->data);
    h_buf_reset(&t->public_id);
    h_buf_reset(&t->system_id);
    t->attrs = t->last_attr = NULL;
    t->has_public = t->has_system = false;
    t->self_closing = t->acknowledged = t->force_quirks = false;
    p->attr_active = false;
    p->duplicate_attr = false;
}
static void attribute_begin(os64_html_parser_t *p)
{
    p->attr_active = true;
    p->duplicate_attr = false;
    h_buf_reset(&p->attr_name);
    h_buf_reset(&p->attr_value);
}
static void attribute_name_done(os64_html_parser_t *p)
{
    for (HAttr *a = p->token.attrs; a; a = a->next) {
        if (!h_work(p, 1 + p->attr_name.len))
            return;
        if (h_eq(a->name, p->attr_name.s)) {
            p->duplicate_attr = true;
            h_error(p, "duplicate-attribute");
            return;
        }
    }
}
static void attribute_finish(os64_html_parser_t *p)
{
    if (!p->attr_active)
        return;
    p->attr_active = false;
    if (p->duplicate_attr)
        return;
    HAttr *a = h_permanent(p, sizeof(*a));
    if (!a)
        return;
    a->name = h_copy(p, p->attr_name.s, p->attr_name.len);
    a->value = p->attr_value.len ? h_copy(p, p->attr_value.s, p->attr_value.len) : "";
    if (p->d->pub.refusal)
        return;
    if (p->token.last_attr)
        p->token.last_attr->next = a;
    else
        p->token.attrs = a;
    p->token.last_attr = a;
}
void h_emit(os64_html_parser_t *p)
{
    attribute_finish(p);
    HToken *t = &p->token;
    if (p->d->pub.refusal)
        return;
    if (t->type == H_START) {
        h_buf_reset(&p->last_start);
        h_buf_bytes(p, &p->last_start, t->name.s, t->name.len);
    } else if (t->type == H_END) {
        if (t->attrs)
            h_error(p, "end-tag-with-attributes");
        if (t->self_closing)
            h_error(p, "end-tag-with-trailing-solidus");
    }
    if (p->d->pub.refusal)
        return;
    if (p->token_sink)
        p->token_sink(p, t, p->sink_context);
    else {
        h_tree(p, t);
        if (t->type == H_START && t->self_closing && !t->acknowledged && !p->d->pub.refusal)
            h_error(p, "non-void-html-element-start-tag-with-trailing-solidus");
    }
}
static bool reference_in_attribute(os64_html_parser_t *p)
{
    return p->return_state == T_VALUE_DQ || p->return_state == T_VALUE_SQ ||
           p->return_state == T_VALUE_UQ;
}
static void reference_char(os64_html_parser_t *p, uint32_t c)
{
    if (reference_in_attribute(p))
        h_buf_put(p, &p->attr_value, c);
    else
        character(p, c);
}
static void reference_literal(os64_html_parser_t *p)
{
    for (size_t i = 0; i < p->temporary.len; i++)
        reference_char(p, (unsigned char)p->temporary.s[i]);
}
/* Return the first table entry with this prefix. The generated table is
 * sorted by ASCII spelling; a reference buffer is at most the longest name
 * plus its delimiter, independent of the length of an ambiguous ampersand. */
static size_t entity_prefix(const char *s, size_t n)
{
    size_t lo = 0, hi = H_ARRAY(entities);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2, i = 0;
        const char *name = entities[mid].name;
        while (i < n && name[i] && name[i] == s[i])
            i++;
        int cmp = i == n ? 0 : (int)(unsigned char)name[i] - (int)(unsigned char)s[i];
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == H_ARRAY(entities))
        return lo;
    for (size_t i = 0; i < n; i++)
        if (!entities[lo].name[i] || entities[lo].name[i] != s[i])
            return H_ARRAY(entities);
    return lo;
}
static void finish_named(os64_html_parser_t *p, uint32_t current, bool consumed)
{
    HState ret = p->return_state;
    if (p->reference_match) {
        size_t used = p->reference_match;
        bool semicolon = p->temporary.s[used - 1] == ';';
        uint32_t next = used < p->temporary.len ? (unsigned char)p->temporary.s[used] : current;
        if (reference_in_attribute(p) && !semicolon && (alnum(next) || next == '=')) {
            reference_literal(p);
            p->state = ret;
            if (!consumed)
                h_tokenize(p, current);
            return;
        }
        if (!semicolon)
            h_error(p, "missing-semicolon-after-character-reference");
        HEntity e = entities[p->reference_index];
        reference_char(p, e.a);
        if (e.b)
            reference_char(p, e.b);
        char tail[64];
        size_t n = p->temporary.len - used;
        for (size_t i = 0; i < n; i++)
            tail[i] = p->temporary.s[used + i];
        p->state = ret;
        for (size_t i = 0; i < n; i++)
            h_tokenize(p, (unsigned char)tail[i]);
        if (!consumed)
            h_tokenize(p, current);
    } else {
        if (consumed && current == ';')
            h_error(p, "unknown-named-character-reference");
        reference_literal(p);
        p->state = T_AMBIGUOUS;
        if (!consumed)
            h_tokenize(p, current);
    }
}
static void numeric_finish(os64_html_parser_t *p)
{
    uint32_t c = p->number;
    if (!c) {
        h_error(p, "null-character-reference");
        c = 0xfffd;
    } else if (c > 0x10ffff) {
        h_error(p, "character-reference-outside-unicode-range");
        c = 0xfffd;
    } else if (c >= 0xd800 && c <= 0xdfff) {
        h_error(p, "surrogate-character-reference");
        c = 0xfffd;
    } else if ((c >= 0xfdd0 && c <= 0xfdef) || (c & 0xffffu) >= 0xfffe)
        h_error(p, "noncharacter-character-reference");
    else if (c == 13 || (c >= 1 && c <= 8) || c == 11 || (c >= 14 && c <= 31) ||
             (c >= 127 && c <= 159)) {
        h_error(p, "control-character-reference");
        if (c >= 0x80 && c <= 0x9f)
            c = numeric_high[c - 0x80];
    }
    reference_char(p, c);
    p->state = p->return_state;
}
static bool appropriate(os64_html_parser_t *p)
{
    return h_eq(p->token.name.s, p->last_start.s);
}
static void raw_fallback(os64_html_parser_t *p, HState state, uint32_t c)
{
    literal(p, "</");
    literal(p, p->temporary.s ? p->temporary.s : "");
    p->state = state;
    h_tokenize(p, c);
}
static void data_null(os64_html_parser_t *p, HBuf *b, uint32_t c)
{
    if (c == 0) {
        h_error(p, "unexpected-null-character");
        c = 0xfffd;
    }
    h_buf_put(p, b, c);
}
void h_tokenize(os64_html_parser_t *p, uint32_t c)
{
    /* Reconsumption is a state transition, not another input byte. Charging it
     * prevents malformed input from hiding repeated work behind one byte. */
    bool again = true;
    while (again && !p->d->pub.refusal) {
        if (!h_work(p, 1))
            return;
        again = false;
        HToken *t = &p->token;
        switch (p->state) {
        case T_DATA:
            if (c == '&') {
                p->return_state = T_DATA;
                p->state = T_REFERENCE;
                h_buf_reset(&p->temporary);
                h_buf_put(p, &p->temporary, '&');
            } else if (c == '<')
                p->state = T_TAG_OPEN;
            else {
                if (c == 0)
                    h_error(p, "unexpected-null-character");
                character(p, c);
            }
            break;
        case T_RCDATA:
        case T_RAWTEXT:
        case T_SCRIPT:
        case T_PLAIN:
            if (c == '&' && p->state == T_RCDATA) {
                p->return_state = T_RCDATA;
                p->state = T_REFERENCE;
                h_buf_reset(&p->temporary);
                h_buf_put(p, &p->temporary, '&');
            } else if (c == '<' && p->state != T_PLAIN)
                p->state = p->state == T_RCDATA    ? T_RC_LT
                           : p->state == T_RAWTEXT ? T_RAW_LT
                                                   : T_SCRIPT_LT;
            else {
                if (c == 0) {
                    h_error(p, "unexpected-null-character");
                    c = 0xfffd;
                }
                character(p, c);
            }
            break;
        case T_TAG_OPEN:
            if (c == '!') {
                p->state = T_DECLARATION;
                h_buf_reset(&p->temporary);
            } else if (c == '/')
                p->state = T_END_OPEN;
            else if (h_alpha(c)) {
                begin_token(p, H_START);
                p->state = T_TAG_NAME;
                again = true;
            } else if (c == '?') {
                h_error(p, "unexpected-question-mark-instead-of-tag-name");
                begin_token(p, H_COMMENT);
                p->state = T_BOGUS_COMMENT;
                again = true;
            } else if (c == H_EOF) {
                h_error(p, "eof-before-tag-name");
                character(p, '<');
                character(p, H_EOF);
            } else {
                h_error(p, "invalid-first-character-of-tag-name");
                character(p, '<');
                p->state = T_DATA;
                again = true;
            }
            break;
        case T_END_OPEN:
            if (h_alpha(c)) {
                begin_token(p, H_END);
                p->state = T_TAG_NAME;
                again = true;
            } else if (c == '>') {
                h_error(p, "missing-end-tag-name");
                p->state = T_DATA;
            } else if (c == H_EOF) {
                h_error(p, "eof-before-tag-name");
                literal(p, "</");
                character(p, H_EOF);
            } else {
                h_error(p, "invalid-first-character-of-tag-name");
                begin_token(p, H_COMMENT);
                p->state = T_BOGUS_COMMENT;
                again = true;
            }
            break;
        case T_TAG_NAME:
            if (h_space(c))
                p->state = T_BEFORE_ATTR;
            else if (c == '/')
                p->state = T_SELF_CLOSE;
            else if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-tag");
                character(p, H_EOF);
            } else
                data_null(p, &t->name, h_lower(c));
            break;
        case T_BEFORE_ATTR:
            if (h_space(c))
                break;
            if (c == '/' || c == '>' || c == H_EOF) {
                p->state = T_AFTER_ATTR;
                again = true;
                break;
            }
            attribute_begin(p);
            if (c == '=') {
                h_error(p, "unexpected-equals-sign-before-attribute-name");
                h_buf_put(p, &p->attr_name, c);
                p->state = T_ATTR_NAME;
            } else {
                p->state = T_ATTR_NAME;
                again = true;
            }
            break;
        case T_ATTR_NAME:
            if (h_space(c) || c == '/' || c == '>' || c == H_EOF) {
                attribute_name_done(p);
                p->state = T_AFTER_ATTR;
                again = true;
            } else if (c == '=') {
                attribute_name_done(p);
                p->state = T_BEFORE_VALUE;
            } else {
                if (c == '"' || c == '\'' || c == '<')
                    h_error(p, "unexpected-character-in-attribute-name");
                data_null(p, &p->attr_name, h_lower(c));
            }
            break;
        case T_AFTER_ATTR:
            if (h_space(c))
                break;
            if (c == '/') {
                attribute_finish(p);
                p->state = T_SELF_CLOSE;
            } else if (c == '=')
                p->state = T_BEFORE_VALUE;
            else if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-tag");
                character(p, H_EOF);
            } else {
                attribute_finish(p);
                attribute_begin(p);
                p->state = T_ATTR_NAME;
                again = true;
            }
            break;
        case T_BEFORE_VALUE:
            if (h_space(c))
                break;
            if (c == '"')
                p->state = T_VALUE_DQ;
            else if (c == '\'')
                p->state = T_VALUE_SQ;
            else if (c == '>') {
                h_error(p, "missing-attribute-value");
                p->state = T_DATA;
                h_emit(p);
            } else {
                p->state = T_VALUE_UQ;
                again = true;
            }
            break;
        case T_VALUE_DQ:
        case T_VALUE_SQ:
        case T_VALUE_UQ:
            if ((p->state == T_VALUE_DQ && c == '"') || (p->state == T_VALUE_SQ && c == '\'')) {
                attribute_finish(p);
                p->state = T_AFTER_VALUE;
            } else if (p->state == T_VALUE_UQ && h_space(c)) {
                attribute_finish(p);
                p->state = T_BEFORE_ATTR;
            } else if (p->state == T_VALUE_UQ && c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == '&') {
                p->return_state = p->state;
                p->state = T_REFERENCE;
                h_buf_reset(&p->temporary);
                h_buf_put(p, &p->temporary, '&');
            } else if (c == H_EOF) {
                h_error(p, "eof-in-tag");
                character(p, H_EOF);
            } else {
                if (p->state == T_VALUE_UQ &&
                    (c == '"' || c == '\'' || c == '<' || c == '=' || c == '`'))
                    h_error(p, "unexpected-character-in-unquoted-attribute-value");
                data_null(p, &p->attr_value, c);
            }
            break;
        case T_AFTER_VALUE:
            if (h_space(c))
                p->state = T_BEFORE_ATTR;
            else if (c == '/')
                p->state = T_SELF_CLOSE;
            else if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-tag");
                character(p, H_EOF);
            } else {
                h_error(p, "missing-whitespace-between-attributes");
                p->state = T_BEFORE_ATTR;
                again = true;
            }
            break;
        case T_SELF_CLOSE:
            if (c == '>') {
                t->self_closing = true;
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-tag");
                character(p, H_EOF);
            } else {
                h_error(p, "unexpected-solidus-in-tag");
                p->state = T_BEFORE_ATTR;
                again = true;
            }
            break;
        case T_RC_LT:
        case T_RAW_LT:
        case T_SCRIPT_LT:
            if (c == '/') {
                h_buf_reset(&p->temporary);
                p->state = p->state == T_RC_LT    ? T_RC_END_OPEN
                           : p->state == T_RAW_LT ? T_RAW_END_OPEN
                                                  : T_SCRIPT_END_OPEN;
            } else if (p->state == T_SCRIPT_LT && c == '!') {
                literal(p, "<!");
                p->state = T_ESCAPE_START;
            } else {
                character(p, '<');
                p->state = p->state == T_RC_LT    ? T_RCDATA
                           : p->state == T_RAW_LT ? T_RAWTEXT
                                                  : T_SCRIPT;
                again = true;
            }
            break;
        case T_RC_END_OPEN:
        case T_RAW_END_OPEN:
        case T_SCRIPT_END_OPEN:
        case T_ESCAPED_END_OPEN:
            if (h_alpha(c)) {
                begin_token(p, H_END);
                p->state = p->state == T_RC_END_OPEN       ? T_RC_END_NAME
                           : p->state == T_RAW_END_OPEN    ? T_RAW_END_NAME
                           : p->state == T_SCRIPT_END_OPEN ? T_SCRIPT_END_NAME
                                                           : T_ESCAPED_END_NAME;
                again = true;
            } else {
                literal(p, "</");
                p->state = p->state == T_RC_END_OPEN       ? T_RCDATA
                           : p->state == T_RAW_END_OPEN    ? T_RAWTEXT
                           : p->state == T_SCRIPT_END_OPEN ? T_SCRIPT
                                                           : T_ESCAPED;
                again = true;
            }
            break;
        case T_RC_END_NAME:
        case T_RAW_END_NAME:
        case T_SCRIPT_END_NAME:
        case T_ESCAPED_END_NAME:
            if (h_alpha(c)) {
                h_buf_put(p, &t->name, h_lower(c));
                h_buf_put(p, &p->temporary, c);
            } else if (appropriate(p) && h_space(c))
                p->state = T_BEFORE_ATTR;
            else if (appropriate(p) && c == '/')
                p->state = T_SELF_CLOSE;
            else if (appropriate(p) && c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else
                raw_fallback(p,
                             p->state == T_RC_END_NAME       ? T_RCDATA
                             : p->state == T_RAW_END_NAME    ? T_RAWTEXT
                             : p->state == T_SCRIPT_END_NAME ? T_SCRIPT
                                                             : T_ESCAPED,
                             c);
            break;
        case T_ESCAPE_START:
            if (c == '-') {
                character(p, c);
                p->state = T_ESCAPE_START_DASH;
            } else {
                p->state = T_SCRIPT;
                again = true;
            }
            break;
        case T_ESCAPE_START_DASH:
            if (c == '-') {
                character(p, c);
                p->state = T_ESCAPED_DASH_DASH;
            } else {
                p->state = T_SCRIPT;
                again = true;
            }
            break;
        case T_ESCAPED:
        case T_ESCAPED_DASH:
        case T_ESCAPED_DASH_DASH:
            if (c == '-') {
                character(p, c);
                p->state = p->state == T_ESCAPED ? T_ESCAPED_DASH : T_ESCAPED_DASH_DASH;
            } else if (c == '<')
                p->state = T_ESCAPED_LT;
            else if (c == '>' && p->state == T_ESCAPED_DASH_DASH) {
                character(p, c);
                p->state = T_SCRIPT;
            } else if (c == H_EOF) {
                h_error(p, "eof-in-script-html-comment-like-text");
                character(p, c);
            } else {
                if (!c) {
                    h_error(p, "unexpected-null-character");
                    c = 0xfffd;
                }
                character(p, c);
                p->state = T_ESCAPED;
            }
            break;
        case T_ESCAPED_LT:
            if (c == '/') {
                h_buf_reset(&p->temporary);
                p->state = T_ESCAPED_END_OPEN;
            } else if (h_alpha(c)) {
                h_buf_reset(&p->temporary);
                character(p, '<');
                p->state = T_DOUBLE_START;
                again = true;
            } else {
                character(p, '<');
                p->state = T_ESCAPED;
                again = true;
            }
            break;
        case T_DOUBLE_START:
        case T_DOUBLE_END:
            if (h_space(c) || c == '/' || c == '>') {
                bool script = h_eq(p->temporary.s, "script");
                p->state = p->state == T_DOUBLE_START ? (script ? T_DOUBLE : T_ESCAPED)
                                                      : (script ? T_ESCAPED : T_DOUBLE);
                character(p, c);
            } else if (h_alpha(c)) {
                h_buf_put(p, &p->temporary, h_lower(c));
                character(p, c);
            } else {
                p->state = p->state == T_DOUBLE_START ? T_ESCAPED : T_DOUBLE;
                again = true;
            }
            break;
        case T_DOUBLE:
        case T_DOUBLE_DASH:
        case T_DOUBLE_DASH_DASH:
            if (c == '-') {
                character(p, c);
                p->state = p->state == T_DOUBLE ? T_DOUBLE_DASH : T_DOUBLE_DASH_DASH;
            } else if (c == '<') {
                character(p, c);
                p->state = T_DOUBLE_LT;
            } else if (c == '>' && p->state == T_DOUBLE_DASH_DASH) {
                character(p, c);
                p->state = T_SCRIPT;
            } else if (c == H_EOF) {
                h_error(p, "eof-in-script-html-comment-like-text");
                character(p, c);
            } else {
                if (!c) {
                    h_error(p, "unexpected-null-character");
                    c = 0xfffd;
                }
                character(p, c);
                p->state = T_DOUBLE;
            }
            break;
        case T_DOUBLE_LT:
            if (c == '/') {
                h_buf_reset(&p->temporary);
                character(p, c);
                p->state = T_DOUBLE_END;
            } else {
                p->state = T_DOUBLE;
                again = true;
            }
            break;
        case T_REFERENCE:
            if (alnum(c)) {
                p->reference_match = 0;
                p->state = T_NAMED_REFERENCE;
                again = true;
            } else if (c == '#') {
                h_buf_put(p, &p->temporary, c);
                p->state = T_NUMERIC;
                p->number = 0;
            } else {
                reference_literal(p);
                p->state = p->return_state;
                again = true;
            }
            break;
        case T_NAMED_REFERENCE:
            if (c < 128 && (alnum(c) || c == ';')) {
                h_buf_put(p, &p->temporary, c);
                size_t i = entity_prefix(p->temporary.s + 1, p->temporary.len - 1);
                if (i == H_ARRAY(entities)) {
                    finish_named(p, c, true);
                    break;
                }
                if (h_len(entities[i].name) == p->temporary.len - 1) {
                    p->reference_match = p->temporary.len;
                    p->reference_index = i;
                }
                if (c == ';')
                    finish_named(p, c, true);
            } else
                finish_named(p, c, false);
            break;
        case T_AMBIGUOUS:
            if (alnum(c))
                reference_char(p, c);
            else {
                if (c == ';')
                    h_error(p, "unknown-named-character-reference");
                p->state = p->return_state;
                again = true;
            }
            break;
        case T_NUMERIC:
            if (c == 'x' || c == 'X') {
                h_buf_put(p, &p->temporary, c);
                p->state = T_HEX_START;
            } else {
                p->state = T_DEC_START;
                again = true;
            }
            break;
        case T_HEX_START:
        case T_DEC_START: {
            bool hex = p->state == T_HEX_START;
            if ((c >= '0' && c <= '9') || (hex && h_lower(c) >= 'a' && h_lower(c) <= 'f')) {
                p->state = hex ? T_HEX : T_DEC;
                again = true;
            } else {
                h_error(p, "absence-of-digits-in-numeric-character-reference");
                reference_literal(p);
                p->state = p->return_state;
                again = true;
            }
            break;
        }
        case T_HEX:
        case T_DEC: {
            unsigned base = p->state == T_HEX ? 16 : 10;
            unsigned digit = c >= '0' && c <= '9' ? c - '0'
                             : base == 16 && h_lower(c) >= 'a' && h_lower(c) <= 'f'
                                 ? h_lower(c) - 'a' + 10
                                 : 99;
            if (digit < base) {
                if (p->number <= 0x110000)
                    p->number = p->number * base + digit;
            } else {
                if (c != ';')
                    h_error(p, "missing-semicolon-after-character-reference");
                numeric_finish(p);
                if (c != ';')
                    again = true;
            }
            break;
        }
        case T_DECLARATION: {
            size_t previous_length = p->temporary.len;
            if (c != H_EOF)
                h_buf_put(p, &p->temporary, c);
            const char *s = p->temporary.s ? p->temporary.s : "";
            size_t n = p->temporary.len;
            bool dash = n <= 2, doctype = n <= 7, cdata = n <= 7;
            const char *d = "DOCTYPE", *cd = "[CDATA[";
            for (size_t i = 0; i < n; i++) {
                if (i >= 2 || s[i] != '-')
                    dash = false;
                if (i >= 7 || h_lower((unsigned char)s[i]) != h_lower((unsigned char)d[i]))
                    doctype = false;
                if (i >= 7 || s[i] != cd[i])
                    cdata = false;
            }
            if (dash && n == 2) {
                begin_token(p, H_COMMENT);
                p->state = T_COMMENT_START;
            } else if (doctype && n == 7) {
                begin_token(p, H_DOCTYPE);
                p->state = T_DOCTYPE;
            } else if (cdata && n == 7) {
                if (!p->token_sink && h_current(p)->ns != OS64_HTML_NS_HTML)
                    p->state = T_CDATA;
                else {
                    h_error(p, "cdata-in-html-content");
                    begin_token(p, H_COMMENT);
                    h_buf_bytes(p, &t->data, s, n);
                    p->state = T_BOGUS_COMMENT;
                }
            } else if ((!dash && !doctype && !cdata) || c == H_EOF) {
                h_error(p, "incorrectly-opened-comment");
                begin_token(p, H_COMMENT);
                h_buf_bytes(p, &t->data, s, previous_length);
                p->state = T_BOGUS_COMMENT;
                again = true;
            }
            break;
        }
        case T_BOGUS_COMMENT:
            if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_emit(p);
                character(p, H_EOF);
            } else
                data_null(p, &t->data, c);
            break;
        case T_COMMENT_START:
            if (c == '-')
                p->state = T_COMMENT_START_DASH;
            else if (c == '>') {
                h_error(p, "abrupt-closing-of-empty-comment");
                p->state = T_DATA;
                h_emit(p);
            } else {
                p->state = T_COMMENT;
                again = true;
            }
            break;
        case T_COMMENT_START_DASH:
            if (c == '-')
                p->state = T_COMMENT_END;
            else if (c == '>') {
                h_error(p, "abrupt-closing-of-empty-comment");
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-comment");
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_buf_put(p, &t->data, '-');
                p->state = T_COMMENT;
                again = true;
            }
            break;
        case T_COMMENT:
            if (c == '<') {
                h_buf_put(p, &t->data, c);
                p->state = T_COMMENT_LT;
            } else if (c == '-')
                p->state = T_COMMENT_END_DASH;
            else if (c == H_EOF) {
                h_error(p, "eof-in-comment");
                h_emit(p);
                character(p, H_EOF);
            } else
                data_null(p, &t->data, c);
            break;
        case T_COMMENT_LT:
            if (c == '!') {
                h_buf_put(p, &t->data, c);
                p->state = T_COMMENT_LT_BANG;
            } else if (c == '<')
                h_buf_put(p, &t->data, c);
            else {
                p->state = T_COMMENT;
                again = true;
            }
            break;
        case T_COMMENT_LT_BANG:
            if (c == '-')
                p->state = T_COMMENT_LT_BANG_DASH;
            else {
                p->state = T_COMMENT;
                again = true;
            }
            break;
        case T_COMMENT_LT_BANG_DASH:
            if (c == '-')
                p->state = T_COMMENT_LT_BANG_DASH_DASH;
            else {
                p->state = T_COMMENT_END_DASH;
                again = true;
            }
            break;
        case T_COMMENT_LT_BANG_DASH_DASH:
            if (c != '>' && c != H_EOF)
                h_error(p, "nested-comment");
            p->state = T_COMMENT_END;
            again = true;
            break;
        case T_COMMENT_END_DASH:
            if (c == '-')
                p->state = T_COMMENT_END;
            else if (c == H_EOF) {
                h_error(p, "eof-in-comment");
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_buf_put(p, &t->data, '-');
                p->state = T_COMMENT;
                again = true;
            }
            break;
        case T_COMMENT_END:
            if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == '!')
                p->state = T_COMMENT_END_BANG;
            else if (c == '-')
                h_buf_put(p, &t->data, '-');
            else if (c == H_EOF) {
                h_error(p, "eof-in-comment");
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_buf_bytes(p, &t->data, "--", 2);
                p->state = T_COMMENT;
                again = true;
            }
            break;
        case T_COMMENT_END_BANG:
            if (c == '-') {
                h_buf_bytes(p, &t->data, "--!", 3);
                p->state = T_COMMENT_END_DASH;
            } else if (c == '>') {
                h_error(p, "incorrectly-closed-comment");
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-comment");
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_buf_bytes(p, &t->data, "--!", 3);
                p->state = T_COMMENT;
                again = true;
            }
            break;
        case T_DOCTYPE:
            if (h_space(c))
                p->state = T_BEFORE_DOCTYPE_NAME;
            else if (c == '>') {
                p->state = T_BEFORE_DOCTYPE_NAME;
                again = true;
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_error(p, "missing-whitespace-before-doctype-name");
                p->state = T_BEFORE_DOCTYPE_NAME;
                again = true;
            }
            break;
        case T_BEFORE_DOCTYPE_NAME:
            if (h_space(c))
                break;
            if (c == '>') {
                h_error(p, "missing-doctype-name");
                t->force_quirks = true;
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else {
                data_null(p, &t->name, h_lower(c));
                p->state = T_DOCTYPE_NAME;
            }
            break;
        case T_DOCTYPE_NAME:
            if (h_space(c))
                p->state = T_AFTER_DOCTYPE_NAME;
            else if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else
                data_null(p, &t->name, h_lower(c));
            break;
        case T_AFTER_DOCTYPE_NAME:
            if (h_space(c))
                break;
            if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_buf_reset(&p->temporary);
                p->state = T_DOCTYPE_KEYWORD;
                again = true;
            }
            break;
        case T_DOCTYPE_KEYWORD: {
            if (c != H_EOF)
                h_buf_put(p, &p->temporary, h_lower(c));
            size_t n = p->temporary.len;
            bool pub = n <= 6, sys = n <= 6;
            for (size_t i = 0; i < n; i++) {
                if (i >= 6 || p->temporary.s[i] != "public"[i])
                    pub = false;
                if (i >= 6 || p->temporary.s[i] != "system"[i])
                    sys = false;
            }
            if (n == 6 && (pub || sys))
                p->state = pub ? T_AFTER_PUBLIC : T_AFTER_SYSTEM;
            else if ((!pub && !sys) || c == H_EOF) {
                h_error(p, "invalid-character-sequence-after-doctype-name");
                t->force_quirks = true;
                p->state = T_BOGUS_DOCTYPE;
                /* The failed lookahead includes its delimiter. */
                if (c == '>' || c == H_EOF || c == 0)
                    again = true;
            }
            break;
        }
        case T_AFTER_PUBLIC:
        case T_BEFORE_PUBLIC:
        case T_AFTER_SYSTEM:
        case T_BEFORE_SYSTEM: {
            bool pub = p->state == T_AFTER_PUBLIC || p->state == T_BEFORE_PUBLIC;
            bool after = p->state == T_AFTER_PUBLIC || p->state == T_AFTER_SYSTEM;
            if (h_space(c)) {
                p->state = pub ? T_BEFORE_PUBLIC : T_BEFORE_SYSTEM;
                break;
            }
            if (c == '"' || c == '\'') {
                if (after)
                    h_error(p, pub ? "missing-whitespace-after-doctype-public-keyword"
                                   : "missing-whitespace-after-doctype-system-keyword");
                if (pub)
                    t->has_public = true;
                else
                    t->has_system = true;
                p->state = pub ? (c == '"' ? T_PUBLIC_DQ : T_PUBLIC_SQ)
                               : (c == '"' ? T_SYSTEM_DQ : T_SYSTEM_SQ);
            } else if (c == '>') {
                h_error(p, pub ? "missing-doctype-public-identifier"
                               : "missing-doctype-system-identifier");
                t->force_quirks = true;
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_error(p, pub ? "missing-quote-before-doctype-public-identifier"
                               : "missing-quote-before-doctype-system-identifier");
                t->force_quirks = true;
                p->state = T_BOGUS_DOCTYPE;
                again = true;
            }
            break;
        }
        case T_PUBLIC_DQ:
        case T_PUBLIC_SQ:
        case T_SYSTEM_DQ:
        case T_SYSTEM_SQ: {
            bool pub = p->state == T_PUBLIC_DQ || p->state == T_PUBLIC_SQ;
            uint32_t quote = p->state == T_PUBLIC_DQ || p->state == T_SYSTEM_DQ ? '"' : '\'';
            if (c == quote)
                p->state = pub ? T_AFTER_PUBLIC_ID : T_AFTER_SYSTEM_ID;
            else if (c == '>') {
                h_error(p, pub ? "abrupt-doctype-public-identifier"
                               : "abrupt-doctype-system-identifier");
                t->force_quirks = true;
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else
                data_null(p, pub ? &t->public_id : &t->system_id, c);
            break;
        }
        case T_AFTER_PUBLIC_ID:
        case T_BETWEEN_IDS:
            if (h_space(c))
                p->state = T_BETWEEN_IDS;
            else if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == '"' || c == '\'') {
                if (p->state == T_AFTER_PUBLIC_ID)
                    h_error(p, "missing-whitespace-between-doctype-public-and-system-identifiers");
                t->has_system = true;
                p->state = c == '"' ? T_SYSTEM_DQ : T_SYSTEM_SQ;
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_error(p, "missing-quote-before-doctype-system-identifier");
                t->force_quirks = true;
                p->state = T_BOGUS_DOCTYPE;
                again = true;
            }
            break;
        case T_AFTER_SYSTEM_ID:
            if (h_space(c))
                break;
            if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_error(p, "eof-in-doctype");
                t->force_quirks = true;
                h_emit(p);
                character(p, H_EOF);
            } else {
                h_error(p, "unexpected-character-after-doctype-system-identifier");
                p->state = T_BOGUS_DOCTYPE;
                again = true;
            }
            break;
        case T_BOGUS_DOCTYPE:
            if (c == '>') {
                p->state = T_DATA;
                h_emit(p);
            } else if (c == H_EOF) {
                h_emit(p);
                character(p, H_EOF);
            } else if (c == 0)
                h_error(p, "unexpected-null-character");
            break;
        case T_CDATA:
            if (c == ']')
                p->state = T_CDATA_BRACKET;
            else if (c == H_EOF) {
                h_error(p, "eof-in-cdata");
                character(p, H_EOF);
            } else
                character(p, c);
            break;
        case T_CDATA_BRACKET:
            if (c == ']')
                p->state = T_CDATA_END;
            else {
                character(p, ']');
                p->state = T_CDATA;
                again = true;
            }
            break;
        case T_CDATA_END:
            if (c == ']')
                character(p, ']');
            else if (c == '>')
                p->state = T_DATA;
            else {
                literal(p, "]]");
                p->state = T_CDATA;
                again = true;
            }
            break;
        }
    }
}
