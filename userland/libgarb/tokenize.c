// tokenize.c — CSS Syntax Level 3 §3.3 and §4: text in, tokens out.
//
// Each function below is one of §4's algorithms, named after it, and
// follows its steps in order, so a reader can hold this file against the
// specification. Where the specification says "parse error" nothing is
// reported: CSS recovers from every error by rule, and the rule is what is
// written here.

#include "internal.h"
#include "os64/mem.h"
#include "os64/str.h"

#define REPLACEMENT 0xFFFDu
#define EOF_CP 0xFFFFFFFFu

// ── §3.3 Preprocessing ──────────────────────────────────────────────────

// One code point from UTF-8 at s[*i], invalid sequences decoding to U+FFFD
// one byte at a time, as the Encoding standard's decoder does.
static uint32_t utf8_next(const unsigned char *s, size_t n, size_t *i)
{
    unsigned char c = s[*i];
    if (c < 0x80) {
        (*i)++;
        return c;
    }
    int more = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
    if (more < 0 || c > 0xF4) {
        (*i)++;
        return REPLACEMENT;
    }
    uint32_t cp = c & (0x3F >> more);
    unsigned char lo = 0x80, hi = 0xBF;
    if (c == 0xE0) lo = 0xA0;
    if (c == 0xED) hi = 0x9F;
    if (c == 0xF0) lo = 0x90;
    if (c == 0xF4) hi = 0x8F;
    size_t j = *i + 1;
    for (int k = 0; k < more; k++, j++) {
        if (j >= n || s[j] < lo || s[j] > hi) {
            *i = j;
            return REPLACEMENT;
        }
        cp = (cp << 6) | (s[j] & 0x3F);
        lo = 0x80;
        hi = 0xBF;
    }
    *i = j;
    return cp;
}

bool tz_open(Tokenizer *tz, const char *text, size_t len, os64_arena_t *arena)
{
    os64_memset(tz, 0, sizeof(*tz));
    tz->arena = arena;
    tz->cp = os64_malloc((len + 1) * sizeof(uint32_t));
    if (tz->cp == NULL)
        return false;
    const unsigned char *s = (const unsigned char *)text;
    size_t i = 0;
    while (i < len) {
        uint32_t c = utf8_next(s, len, &i);
        if (c == '\r') {
            if (i < len && s[i] == '\n')
                i++;
            c = '\n';
        } else if (c == '\f') {
            c = '\n';
        } else if (c == 0) {
            c = REPLACEMENT;
        }
        tz->cp[tz->n++] = c;
    }
    return true;
}

void tz_close(Tokenizer *tz)
{
    os64_free(tz->cp);
    os64_free(tz->buf);
    tz->cp = NULL;
    tz->buf = NULL;
}

// ── Reading the input ───────────────────────────────────────────────────

static uint32_t peek(const Tokenizer *tz, size_t ahead)
{
    return tz->pos + ahead < tz->n ? tz->cp[tz->pos + ahead] : EOF_CP;
}

static uint32_t consume(Tokenizer *tz)
{
    uint32_t c = peek(tz, 0);
    if (tz->pos < tz->n)
        tz->pos++;
    return c;
}

// ── §4.2 Definitions ────────────────────────────────────────────────────

static bool digit(uint32_t c) { return c >= '0' && c <= '9'; }
static bool hex(uint32_t c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool letter(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool non_ascii(uint32_t c) { return c >= 0x80 && c != EOF_CP; }
static bool ident_start(uint32_t c) { return letter(c) || non_ascii(c) || c == '_'; }
static bool ident_cp(uint32_t c) { return ident_start(c) || digit(c) || c == '-'; }
static bool newline(uint32_t c) { return c == '\n'; }
static bool whitespace(uint32_t c) { return c == '\n' || c == '\t' || c == ' '; }
static bool non_printable(uint32_t c)
{
    return c <= 0x08 || c == 0x0B || (c >= 0x0E && c <= 0x1F) || c == 0x7F;
}

static uint32_t hex_value(uint32_t c)
{
    return digit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
}

// §4.3.8
static bool valid_escape(uint32_t a, uint32_t b)
{
    return a == '\\' && !newline(b);
}

// §4.3.9
static bool starts_ident(uint32_t a, uint32_t b, uint32_t c)
{
    if (a == '-')
        return ident_start(b) || b == '-' || valid_escape(b, c);
    if (ident_start(a))
        return true;
    return a == '\\' && valid_escape(a, b);
}

// §4.3.10
static bool starts_number(uint32_t a, uint32_t b, uint32_t c)
{
    if (a == '+' || a == '-')
        return digit(b) || (b == '.' && digit(c));
    if (a == '.')
        return digit(b);
    return digit(a);
}

// ── Building text ───────────────────────────────────────────────────────

static void buf_reset(Tokenizer *tz)
{
    tz->buflen = 0;
}

static void buf_byte(Tokenizer *tz, char b)
{
    if (tz->buflen == tz->bufcap) {
        size_t cap = tz->bufcap ? tz->bufcap * 2 : 64;
        char *grown = os64_realloc(tz->buf, cap);
        if (grown == NULL) {
            tz->short_of_memory = true;
            return;
        }
        tz->buf = grown;
        tz->bufcap = cap;
    }
    tz->buf[tz->buflen++] = b;
}

static void buf_cp(Tokenizer *tz, uint32_t c)
{
    if (c < 0x80) {
        buf_byte(tz, (char)c);
    } else if (c < 0x800) {
        buf_byte(tz, (char)(0xC0 | (c >> 6)));
        buf_byte(tz, (char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        buf_byte(tz, (char)(0xE0 | (c >> 12)));
        buf_byte(tz, (char)(0x80 | ((c >> 6) & 0x3F)));
        buf_byte(tz, (char)(0x80 | (c & 0x3F)));
    } else {
        buf_byte(tz, (char)(0xF0 | (c >> 18)));
        buf_byte(tz, (char)(0x80 | ((c >> 12) & 0x3F)));
        buf_byte(tz, (char)(0x80 | ((c >> 6) & 0x3F)));
        buf_byte(tz, (char)(0x80 | (c & 0x3F)));
    }
}

// The buffer, copied into the arena with a NUL after it.
static const char *buf_keep(Tokenizer *tz, size_t *len)
{
    char *out = os64_arena_alloc(tz->arena, tz->buflen + 1);
    if (out == NULL) {
        tz->short_of_memory = true;
        *len = 0;
        return "";
    }
    if (tz->buflen > 0)
        os64_memcpy(out, tz->buf, tz->buflen);
    out[tz->buflen] = '\0';
    *len = tz->buflen;
    return out;
}

// ── §4.3 The algorithms ─────────────────────────────────────────────────

// §4.3.7, the backslash already consumed.
static uint32_t consume_escape(Tokenizer *tz)
{
    uint32_t c = consume(tz);
    if (c == EOF_CP)
        return REPLACEMENT;
    if (!hex(c))
        return c;
    uint32_t v = hex_value(c);
    for (int k = 0; k < 5 && hex(peek(tz, 0)); k++)
        v = v * 16 + hex_value(consume(tz));
    if (whitespace(peek(tz, 0)))
        consume(tz);
    if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF)
        return REPLACEMENT;
    return v;
}

// §4.3.11, into the buffer.
static void consume_ident_sequence(Tokenizer *tz)
{
    for (;;) {
        uint32_t c = peek(tz, 0);
        if (ident_cp(c)) {
            buf_cp(tz, consume(tz));
        } else if (valid_escape(c, peek(tz, 1))) {
            consume(tz);
            buf_cp(tz, consume_escape(tz));
        } else {
            return;
        }
    }
}

// 10 to the power `e`, by squaring: the tokenizer's only arithmetic past
// what integers hold.
static double pow10(int32_t e)
{
    double result = 1.0, base = 10.0;
    bool neg = e < 0;
    uint32_t n = neg ? (uint32_t)-(int64_t)e : (uint32_t)e;
    while (n > 0) {
        if (n & 1)
            result *= base;
        base *= base;
        n >>= 1;
    }
    return neg ? 1.0 / result : result;
}

// §4.3.12 and §4.3.13: the number's representation into the buffer, its
// value and its type out.
static double consume_number(Tokenizer *tz, bool *integer)
{
    *integer = true;
    int sign = 1, esign = 1;
    uint64_t mantissa = 0;
    int32_t scale = 0, exponent = 0;
    uint32_t c = peek(tz, 0);
    if (c == '+' || c == '-') {
        sign = c == '-' ? -1 : 1;
        buf_cp(tz, consume(tz));
    }
    // Digits past what 19 decimal places hold only move the exponent: the
    // value is kept to a double's precision either way.
    while (digit(peek(tz, 0))) {
        uint32_t d = consume(tz);
        buf_cp(tz, d);
        if (mantissa < 1000000000000000000ull)
            mantissa = mantissa * 10 + (d - '0');
        else
            scale++;
    }
    if (peek(tz, 0) == '.' && digit(peek(tz, 1))) {
        buf_cp(tz, consume(tz));
        *integer = false;
        while (digit(peek(tz, 0))) {
            uint32_t d = consume(tz);
            buf_cp(tz, d);
            if (mantissa < 1000000000000000000ull) {
                mantissa = mantissa * 10 + (d - '0');
                scale--;
            }
        }
    }
    c = peek(tz, 0);
    uint32_t c1 = peek(tz, 1), c2 = peek(tz, 2);
    if ((c == 'e' || c == 'E') && (digit(c1) || ((c1 == '+' || c1 == '-') && digit(c2)))) {
        *integer = false;
        buf_cp(tz, consume(tz));
        if (c1 == '+' || c1 == '-') {
            esign = c1 == '-' ? -1 : 1;
            buf_cp(tz, consume(tz));
        }
        while (digit(peek(tz, 0))) {
            uint32_t d = consume(tz);
            buf_cp(tz, d);
            if (exponent < 100000)
                exponent = exponent * 10 + (int32_t)(d - '0');
        }
    }
    int32_t e = esign * exponent + scale;
    double v = (double)mantissa;
    if (mantissa != 0) {
        // Past a double's range the value is its infinity or its zero.
        if (e > 400)
            v = 1e308 * 10.0;
        else if (e < -400)
            v = 0.0;
        else if (e < -300)
            v = v * pow10(e + 300) * 1e-300;
        else if (e < 0 && e >= -22)
            v = v / pow10(-e);          // 10^22 and below are exact: one rounding
        else
            v = v * pow10(e);
    }
    return sign * v;
}

// §4.3.3
static Tok consume_numeric(Tokenizer *tz)
{
    Tok t = {T_VALUE, {0}};
    buf_reset(tz);
    bool integer;
    double v = consume_number(tz, &integer);
    t.v.number = v;
    t.v.integer = integer;
    t.v.text = buf_keep(tz, &t.v.len);
    if (starts_ident(peek(tz, 0), peek(tz, 1), peek(tz, 2))) {
        t.v.kind = GARB_DIMENSION;
        buf_reset(tz);
        consume_ident_sequence(tz);
        t.v.unit = buf_keep(tz, &t.v.unit_len);
    } else if (peek(tz, 0) == '%') {
        consume(tz);
        t.v.kind = GARB_PERCENTAGE;
    } else {
        t.v.kind = GARB_NUMBER;
    }
    return t;
}

// §4.3.14
static void consume_bad_url_remnants(Tokenizer *tz)
{
    for (;;) {
        uint32_t c = consume(tz);
        if (c == ')' || c == EOF_CP)
            return;
        if (valid_escape(c, peek(tz, 0)))
            (void)consume_escape(tz);
    }
}

// §4.3.6, `url(` already consumed.
static Tok consume_url(Tokenizer *tz)
{
    Tok t = {T_VALUE, {0}};
    t.v.kind = GARB_URL;
    buf_reset(tz);
    while (whitespace(peek(tz, 0)))
        consume(tz);
    for (;;) {
        uint32_t c = consume(tz);
        if (c == ')')
            break;
        if (c == EOF_CP) {
            t.v.eof = true;
            break;
        }
        if (whitespace(c)) {
            while (whitespace(peek(tz, 0)))
                consume(tz);
            if (peek(tz, 0) == ')') {
                consume(tz);
                break;
            }
            if (peek(tz, 0) == EOF_CP) {
                t.v.eof = true;
                break;
            }
            consume_bad_url_remnants(tz);
            t.v.kind = GARB_BAD_URL;
            break;
        }
        if (c == '"' || c == '\'' || c == '(' || non_printable(c)) {
            consume_bad_url_remnants(tz);
            t.v.kind = GARB_BAD_URL;
            break;
        }
        if (c == '\\') {
            if (valid_escape(c, peek(tz, 0))) {
                buf_cp(tz, consume_escape(tz));
                continue;
            }
            consume_bad_url_remnants(tz);
            t.v.kind = GARB_BAD_URL;
            break;
        }
        buf_cp(tz, c);
    }
    if (t.v.kind == GARB_URL)
        t.v.text = buf_keep(tz, &t.v.len);
    return t;
}

// §4.3.4
static Tok consume_ident_like(Tokenizer *tz)
{
    Tok t = {T_VALUE, {0}};
    buf_reset(tz);
    consume_ident_sequence(tz);
    bool is_url = tz->buflen == 3 && (tz->buf[0] | 0x20) == 'u' && (tz->buf[1] | 0x20) == 'r' &&
                  (tz->buf[2] | 0x20) == 'l';
    if (is_url && peek(tz, 0) == '(') {
        consume(tz);
        while (whitespace(peek(tz, 0)) && whitespace(peek(tz, 1)))
            consume(tz);
        uint32_t a = peek(tz, 0), b = peek(tz, 1);
        if (a == '"' || a == '\'' || (whitespace(a) && (b == '"' || b == '\''))) {
            t.kind = T_FUNCTION;
            t.v.kind = GARB_FUNCTION;
            t.v.text = buf_keep(tz, &t.v.len);
            return t;
        }
        return consume_url(tz);
    }
    t.v.text = buf_keep(tz, &t.v.len);
    if (peek(tz, 0) == '(') {
        consume(tz);
        t.kind = T_FUNCTION;
        t.v.kind = GARB_FUNCTION;
    } else {
        t.v.kind = GARB_IDENT;
    }
    return t;
}

// §4.3.5
static Tok consume_string(Tokenizer *tz, uint32_t ending)
{
    Tok t = {T_VALUE, {0}};
    t.v.kind = GARB_STRING;
    buf_reset(tz);
    for (;;) {
        uint32_t c = consume(tz);
        if (c == ending)
            break;
        if (c == EOF_CP) {
            t.v.eof = true;
            break;
        }
        if (newline(c)) {
            tz->pos--;                  // reconsume
            t.v.kind = GARB_BAD_STRING;
            return t;
        }
        if (c == '\\') {
            uint32_t next = peek(tz, 0);
            if (next == EOF_CP)
                continue;
            if (newline(next)) {
                consume(tz);
                continue;
            }
            buf_cp(tz, consume_escape(tz));
            continue;
        }
        buf_cp(tz, c);
    }
    t.v.text = buf_keep(tz, &t.v.len);
    return t;
}

static Tok simple(garb_kind_t kind)
{
    Tok t = {T_VALUE, {0}};
    t.v.kind = kind;
    return t;
}

static Tok delim(Tokenizer *tz, uint32_t c)
{
    Tok t = simple(GARB_DELIM);
    buf_reset(tz);
    buf_cp(tz, c);
    t.v.text = buf_keep(tz, &t.v.len);
    return t;
}

static Tok bracket(tok_kind_t kind, char which)
{
    Tok t = {kind, {0}};
    t.v.open = which;
    return t;
}

// §4.3.2 "consume comments"
static void consume_comments(Tokenizer *tz)
{
    while (peek(tz, 0) == '/' && peek(tz, 1) == '*') {
        tz->pos += 2;
        while (tz->pos < tz->n && !(peek(tz, 0) == '*' && peek(tz, 1) == '/'))
            tz->pos++;
        tz->pos = tz->pos + 2 <= tz->n ? tz->pos + 2 : tz->n;
    }
}

// §4.3.1
Tok tz_next(Tokenizer *tz)
{
    consume_comments(tz);
    uint32_t c = consume(tz);
    if (c == EOF_CP)
        return (Tok){T_EOF, {0}};
    if (whitespace(c)) {
        while (whitespace(peek(tz, 0)))
            consume(tz);
        return simple(GARB_WHITESPACE);
    }
    switch (c) {
    case '"':
    case '\'':
        return consume_string(tz, c);
    case '#':
        if (ident_cp(peek(tz, 0)) || valid_escape(peek(tz, 0), peek(tz, 1))) {
            Tok t = simple(GARB_HASH);
            t.v.id = starts_ident(peek(tz, 0), peek(tz, 1), peek(tz, 2));
            buf_reset(tz);
            consume_ident_sequence(tz);
            t.v.text = buf_keep(tz, &t.v.len);
            return t;
        }
        return delim(tz, c);
    case '(':
    case '[':
    case '{':
        return bracket(T_OPEN, (char)c);
    case ')':
    case ']':
    case '}':
        return bracket(T_CLOSE, (char)c);
    case '+':
    case '.':
        if (starts_number(c, peek(tz, 0), peek(tz, 1))) {
            tz->pos--;
            return consume_numeric(tz);
        }
        return delim(tz, c);
    case ',':
        return simple(GARB_COMMA);
    case '-':
        if (starts_number(c, peek(tz, 0), peek(tz, 1))) {
            tz->pos--;
            return consume_numeric(tz);
        }
        if (peek(tz, 0) == '-' && peek(tz, 1) == '>') {
            tz->pos += 2;
            return simple(GARB_CDC);
        }
        if (starts_ident(c, peek(tz, 0), peek(tz, 1))) {
            tz->pos--;
            return consume_ident_like(tz);
        }
        return delim(tz, c);
    case ':':
        return simple(GARB_COLON);
    case ';':
        return simple(GARB_SEMICOLON);
    case '<':
        if (peek(tz, 0) == '!' && peek(tz, 1) == '-' && peek(tz, 2) == '-') {
            tz->pos += 3;
            return simple(GARB_CDO);
        }
        return delim(tz, c);
    case '@':
        if (starts_ident(peek(tz, 0), peek(tz, 1), peek(tz, 2))) {
            Tok t = simple(GARB_AT_KEYWORD);
            buf_reset(tz);
            consume_ident_sequence(tz);
            t.v.text = buf_keep(tz, &t.v.len);
            return t;
        }
        return delim(tz, c);
    case '\\':
        if (valid_escape(c, peek(tz, 0))) {
            tz->pos--;
            return consume_ident_like(tz);
        }
        return delim(tz, c);
    default:
        break;
    }
    if (digit(c)) {
        tz->pos--;
        return consume_numeric(tz);
    }
    if (ident_start(c)) {
        tz->pos--;
        return consume_ident_like(tz);
    }
    return delim(tz, c);
}
