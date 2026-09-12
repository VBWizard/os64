#include "internal.h"

enum { E_UTF8, E_1252, E_UTF16LE, E_UTF16BE };
static const uint16_t windows_high[32] = {
    0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,
    0x2039, 0x152,  0x8d,   0x17d,  0x8f,   0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
    0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
static bool ascii_equal(const unsigned char *s, size_t n, const char *word)
{
    if (h_len(word) != n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (h_lower(s[i]) != (unsigned char)word[i])
            return false;
    return true;
}
static int label_encoding(const unsigned char *s, size_t n)
{
    while (n && h_space(*s)) {
        s++;
        n--;
    }
    while (n && h_space(s[n - 1]))
        n--;
    if (ascii_equal(s, n, "utf-8") || ascii_equal(s, n, "utf8") ||
        ascii_equal(s, n, "unicode-1-1-utf-8") || ascii_equal(s, n, "unicode11utf8") ||
        ascii_equal(s, n, "unicode20utf8") || ascii_equal(s, n, "x-unicode20utf8"))
        return E_UTF8;
    static const char *const aliases[] = {
        "ansi_x3.4-1968", "ascii",           "cp1252",     "cp819",     "csisolatin1",
        "ibm819",         "iso-8859-1",      "iso-ir-100", "iso8859-1", "iso88591",
        "iso_8859-1",     "iso_8859-1:1987", "l1",         "latin1",    "us-ascii",
        "windows-1252",   "x-cp1252"};
    for (size_t i = 0; i < H_ARRAY(aliases); i++)
        if (ascii_equal(s, n, aliases[i]))
            return E_1252;
    if (ascii_equal(s, n, "utf-16") || ascii_equal(s, n, "utf-16le"))
        return E_UTF16LE;
    if (ascii_equal(s, n, "utf-16be"))
        return E_UTF16BE;
    return -1;
}
static const unsigned char *content_charset(const unsigned char *s, size_t n, size_t *out_n)
{
    for (size_t i = 0; i + 7 <= n; i++) {
        if (!ascii_equal(s + i, 7, "charset"))
            continue;
        size_t at = i + 7;
        while (at < n && h_space(s[at]))
            at++;
        if (at == n || s[at++] != '=')
            continue;
        while (at < n && h_space(s[at]))
            at++;
        if (at == n)
            return NULL;
        unsigned char quote = s[at] == '\'' || s[at] == '"' ? s[at++] : 0;
        size_t begin = at;
        while (at < n && (quote ? s[at] != quote : !h_space(s[at]) && s[at] != ';'))
            at++;
        if (quote && at == n)
            return NULL;
        *out_n = at - begin;
        return s + begin;
    }
    return NULL;
}
static const unsigned char *prescan(const unsigned char *s, size_t n, size_t *out_n)
{
    size_t i = 0;
    while (i < n) {
        if (s[i++] != '<')
            continue;
        if (i + 2 < n && s[i] == '!' && s[i + 1] == '-' && s[i + 2] == '-') {
            i += 1;
            while (i + 2 < n && !(s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>'))
                i++;
            i = i + 2 < n ? i + 3 : n;
            continue;
        }
        bool meta =
            i + 4 < n && ascii_equal(s + i, 4, "meta") && (h_space(s[i + 4]) || s[i + 4] == '/');
        if (i + 1 < n && s[i] == '/' && h_alpha(s[i + 1]))
            i++;
        if (i < n && (s[i] == '!' || s[i] == '/' || s[i] == '?')) {
            while (i < n && s[i] != '>')
                i++;
            continue;
        }
        if (i == n || !h_alpha(s[i]))
            continue;
        while (i < n && !h_space(s[i]) && s[i] != '>' && s[i] != '/')
            i++;
        const unsigned char *charset = NULL;
        size_t charset_n = 0;
        bool pragma = false, need_pragma = false, got_charset = false, got_content = false,
             got_http = false;
        while (i < n && s[i] != '>') {
            while (i < n && (h_space(s[i]) || s[i] == '/'))
                i++;
            if (i == n || s[i] == '>')
                break;
            size_t a = i++;
            while (i < n && !h_space(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/')
                i++;
            size_t an = i - a;
            while (i < n && h_space(s[i]))
                i++;
            size_t v = i, vn = 0;
            if (i < n && s[i] == '=') {
                i++;
                while (i < n && h_space(s[i]))
                    i++;
                unsigned char quote = i < n && (s[i] == '\'' || s[i] == '"') ? s[i++] : 0;
                v = i;
                while (i < n && (quote ? s[i] != quote : !h_space(s[i]) && s[i] != '>'))
                    i++;
                vn = i - v;
                if (quote && i < n)
                    i++;
            }
            if (!meta)
                continue;
            if (ascii_equal(s + a, an, "charset") && !got_charset) {
                got_charset = true;
                charset = s + v;
                charset_n = vn;
                need_pragma = false;
            } else if (ascii_equal(s + a, an, "content") && !got_content) {
                got_content = true;
                if (!charset) {
                    charset = content_charset(s + v, vn, &charset_n);
                    need_pragma = charset != NULL;
                }
            } else if (ascii_equal(s + a, an, "http-equiv") && !got_http) {
                got_http = true;
                pragma = ascii_equal(s + v, vn, "content-type");
            }
        }
        /* A declaration cut by the sniff window is not a complete meta tag. */
        if (i == n)
            return NULL;
        if (charset && (!need_pragma || pragma)) {
            *out_n = charset_n;
            return charset;
        }
    }
    return NULL;
}
void h_codepoint(os64_html_parser_t *p, uint32_t cp)
{
    if (p->d->pub.refusal)
        return;
    if (cp == '\n' && p->previous_cr) {
        p->previous_cr = false;
        return;
    }
    p->previous_cr = cp == '\r';
    if (cp == '\r')
        cp = '\n';
    const char *input_error = NULL;
    if (cp != H_EOF) {
        if (cp >= 0xd800 && cp <= 0xdfff)
            input_error = "surrogate-in-input-stream";
        else if ((cp >= 0xfdd0 && cp <= 0xfdef) || (cp & 0xffffu) >= 0xfffe)
            input_error = "noncharacter-in-input-stream";
        else if ((cp >= 1 && cp <= 8) || cp == 11 || (cp >= 14 && cp <= 31) ||
                 (cp >= 127 && cp <= 159))
            input_error = "control-character-in-input-stream";
    }
    /* Declaration lookahead diagnoses its opening before consuming the
     * character that disproves the prefix. Preserve that diagnostic order. */
    bool deferred = input_error && p->state == T_DECLARATION && p->temporary.len != 0;
    if (input_error && !deferred)
        h_error(p, input_error);
    h_tokenize(p, cp);
    if (deferred)
        h_error(p, input_error);
}
void h_decode(os64_html_parser_t *p, unsigned char b, size_t offset)
{
    p->offset = offset;
    if (p->encoding == E_1252) {
        h_codepoint(p, b >= 0x80 && b <= 0x9f ? windows_high[b - 0x80] : b);
        return;
    }
    if (p->encoding == E_UTF16LE || p->encoding == E_UTF16BE) {
        if (p->utf16_byte < 0) {
            p->utf16_byte = b;
            p->decoder_offset = offset;
            return;
        }
        uint32_t c = p->encoding == E_UTF16LE ? (unsigned)p->utf16_byte | ((unsigned)b << 8)
                                              : ((unsigned)p->utf16_byte << 8) | b;
        p->utf16_byte = -1;
        p->offset = p->decoder_offset;
        if (p->high_surrogate) {
            uint32_t high = p->high_surrogate;
            p->high_surrogate = 0;
            p->offset = p->surrogate_offset;
            if (c >= 0xdc00 && c <= 0xdfff) {
                h_codepoint(p, 0x10000 + ((high - 0xd800) << 10) + c - 0xdc00);
                return;
            }
            h_error(p, "invalid-character-encoding");
            h_codepoint(p, 0xfffd);
            p->offset = p->decoder_offset;
        }
        if (c >= 0xd800 && c <= 0xdbff) {
            p->high_surrogate = c;
            p->surrogate_offset = p->offset;
        } else if (c >= 0xdc00 && c <= 0xdfff) {
            h_error(p, "invalid-character-encoding");
            h_codepoint(p, 0xfffd);
        } else
            h_codepoint(p, c);
        return;
    }
    if (p->decoder_need) {
        unsigned lo = p->decoder_seen ? 0x80 : p->decoder_first_min;
        unsigned hi = p->decoder_seen ? 0xbf : p->decoder_first_max;
        if (b >= lo && b <= hi) {
            p->decoder_cp = (p->decoder_cp << 6) | (b & 63);
            p->decoder_seen++;
            if (p->decoder_seen == p->decoder_need) {
                p->decoder_need = 0;
                p->offset = p->decoder_offset;
                h_codepoint(p, p->decoder_cp);
            }
            return;
        }
        p->decoder_need = 0;
        p->offset = p->decoder_offset;
        h_error(p, "invalid-character-encoding");
        h_codepoint(p, 0xfffd);
        p->offset = offset;
    }
    if (b < 0x80) {
        h_codepoint(p, b);
        return;
    }
    p->decoder_seen = 0;
    p->decoder_first_min = 0x80;
    p->decoder_first_max = 0xbf;
    p->decoder_offset = offset;
    if (b >= 0xc2 && b <= 0xdf) {
        p->decoder_need = 1;
        p->decoder_cp = b & 31;
    } else if (b >= 0xe0 && b <= 0xef) {
        p->decoder_need = 2;
        p->decoder_cp = b & 15;
        if (b == 0xe0)
            p->decoder_first_min = 0xa0;
        if (b == 0xed)
            p->decoder_first_max = 0x9f;
    } else if (b >= 0xf0 && b <= 0xf4) {
        p->decoder_need = 3;
        p->decoder_cp = b & 7;
        if (b == 0xf0)
            p->decoder_first_min = 0x90;
        if (b == 0xf4)
            p->decoder_first_max = 0x8f;
    } else {
        h_error(p, "invalid-character-encoding");
        h_codepoint(p, 0xfffd);
    }
}
void h_decode_finish(os64_html_parser_t *p)
{
    if (p->eof_sent || p->d->pub.refusal)
        return;
    p->offset = p->d->pub.input_bytes;
    if (p->decoder_need || p->high_surrogate || p->utf16_byte >= 0) {
        h_error(p, "invalid-character-encoding");
        h_codepoint(p, 0xfffd);
    }
    p->decoder_need = 0;
    p->high_surrogate = 0;
    p->utf16_byte = -1;
    h_codepoint(p, H_EOF);
    p->eof_sent = true;
}
void h_encoding_start(os64_html_parser_t *p)
{
    if (p->started || p->d->pub.refusal)
        return;
    p->started = true;
    unsigned char *s = p->prescan;
    size_t n = p->prescan_len, skip = 0;
    int encoding = -1;
    if (n >= 3 && s[0] == 0xef && s[1] == 0xbb && s[2] == 0xbf) {
        encoding = E_UTF8;
        skip = 3;
    } else if (n >= 2 && s[0] == 0xff && s[1] == 0xfe) {
        encoding = E_UTF16LE;
        skip = 2;
    } else if (n >= 2 && s[0] == 0xfe && s[1] == 0xff) {
        encoding = E_UTF16BE;
        skip = 2;
    } else {
        size_t len = 0;
        const unsigned char *label = NULL;
        bool transport = p->opt.charset != NULL;
        if (transport) {
            label = (const unsigned char *)p->opt.charset;
            len = h_len(p->opt.charset);
        } else
            label = prescan(s, n, &len);
        if (label) {
            encoding = label_encoding(label, len);
            if (!transport && (encoding == E_UTF16LE || encoding == E_UTF16BE))
                encoding = E_UTF8;
            if (encoding < 0 || encoding == E_UTF16LE || encoding == E_UTF16BE) {
                p->d->pub.charset_unsupported = h_copy(p, (const char *)label, len);
                encoding = E_1252;
            }
        }
    }
    if (encoding < 0)
        encoding = E_1252;
    p->encoding = (unsigned)encoding;
    p->d->pub.charset = encoding == E_UTF8      ? "utf-8"
                        : encoding == E_UTF16LE ? "utf-16le"
                        : encoding == E_UTF16BE ? "utf-16be"
                                                : "windows-1252";
    for (size_t i = skip; i < n && !p->d->pub.refusal; i++)
        h_decode(p, s[i], i);
}
void h_late_meta(os64_html_parser_t *p, HNode *n)
{
    if (p->offset < 1024 || p->d->pub.charset_late_meta)
        return;
    const HAttr *a = os64_html_attr(n, "charset");
    const char *label = a ? a->value : NULL;
    size_t len = h_len(label);
    if (!label) {
        const HAttr *pragma = os64_html_attr(n, "http-equiv"),
                    *content = os64_html_attr(n, "content");
        if (pragma && content &&
            ascii_equal((const unsigned char *)pragma->value, h_len(pragma->value), "content-type"))
            label = (const char *)content_charset((const unsigned char *)content->value,
                                                  h_len(content->value), &len);
    }
    if (label && label_encoding((const unsigned char *)label, len) != (int)p->encoding)
        p->d->pub.charset_late_meta = h_copy(p, label, len);
}
