// decode.c — CSS Syntax Level 3 §3.2: a stylesheet's bytes into text.
//
// The encodings are the ones libhtml reads — UTF-8, UTF-16 either way round,
// and windows-1252 under every label the Encoding standard gives it — and a
// label naming any other is treated as naming none, so the next step of
// §3.2 decides, down to UTF-8 at the last. A sheet in another encoding
// keeps its ASCII, which is nearly all of CSS, whichever of those it is
// read in.

#include "internal.h"
#include "html/html.h"
#include "os64/mem.h"
#include "os64/str.h"

// windows-1252's 0x80..0x9F; the rest of the byte range is Latin-1's.
static const uint16_t kW1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static size_t put_utf8(char *out, uint32_t c)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

// The bytes in `encoding`, as UTF-8 in a new buffer. NULL on no memory.
static char *to_utf8(const uint8_t *b, size_t n, const char *encoding, size_t *len)
{
    // Three bytes of UTF-8 at most for each byte of windows-1252 or for each
    // pair of UTF-16 bytes (a surrogate pair is four bytes in, four out).
    char *out = os64_malloc(n * 3 + 1);
    if (out == NULL)
        return NULL;
    size_t o = 0;
    if (os64_streq(encoding, "utf-8")) {
        os64_memcpy(out, b, n);
        o = n;
    } else if (os64_streq(encoding, "windows-1252")) {
        for (size_t i = 0; i < n; i++)
            o += put_utf8(out + o, b[i] >= 0x80 && b[i] <= 0x9F ? kW1252[b[i] - 0x80] : b[i]);
    } else {
        bool be = os64_streq(encoding, "utf-16be");
        size_t i = 0;
        for (; i + 1 < n; i += 2) {
            uint32_t u = be ? (uint32_t)(b[i] << 8 | b[i + 1]) : (uint32_t)(b[i + 1] << 8 | b[i]);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
                uint32_t lo = be ? (uint32_t)(b[i + 2] << 8 | b[i + 3])
                                 : (uint32_t)(b[i + 3] << 8 | b[i + 2]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    o += put_utf8(out + o, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                    i += 2;
                    continue;
                }
            }
            o += put_utf8(out + o, u >= 0xD800 && u <= 0xDFFF ? 0xFFFD : u);
        }
        if (i < n)
            o += put_utf8(out + o, 0xFFFD);     // a lone last byte
    }
    out[o] = '\0';
    *len = o;
    return out;
}

// An encoding for a label, or NULL for one this library does not read.
static const char *named(const char *label, size_t len)
{
    return label != NULL ? os64_html_encoding_for_label(label, len) : NULL;
}

// §3.2 "determine the fallback encoding".
static const char *fallback(const uint8_t *b, size_t n, const char *protocol,
                            const char *environment)
{
    const char *e = protocol != NULL ? named(protocol, os64_strlen(protocol)) : NULL;
    if (e != NULL)
        return e;
    static const char kCharset[] = "@charset \"";
    size_t k = sizeof(kCharset) - 1;
    size_t limit = n < 1024 ? n : 1024;
    if (limit > k && os64_memcmp(b, kCharset, k) == 0) {
        size_t end = k;
        while (end < limit && b[end] != '"')
            end++;
        if (end + 1 < limit && b[end + 1] == ';') {
            e = named((const char *)b + k, end - k);
            if (e != NULL)
                return os64_streq(e, "utf-16be") || os64_streq(e, "utf-16le") ? "utf-8" : e;
        }
    }
    e = environment != NULL ? named(environment, os64_strlen(environment)) : NULL;
    return e != NULL ? e : "utf-8";
}

garb_status_t garb_parse_sheet(const uint8_t *bytes, size_t len, const char *protocol,
                               const char *environment, garb_parsed_t *out)
{
    if (len > GARB_SHEET_MAX) {
        os64_memset(out, 0, sizeof(*out));
        return GARB_TOO_BIG;
    }
    // The Encoding standard's decode: a byte order mark outranks everything.
    const char *encoding;
    size_t skip = 0;
    if (len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        encoding = "utf-8";
        skip = 3;
    } else if (len >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        encoding = "utf-16be";
        skip = 2;
    } else if (len >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        encoding = "utf-16le";
        skip = 2;
    } else {
        encoding = fallback(bytes, len, protocol, environment);
    }
    size_t tlen = 0;
    char *text = to_utf8(bytes + skip, len - skip, encoding, &tlen);
    if (text == NULL) {
        os64_memset(out, 0, sizeof(*out));
        return GARB_NO_MEMORY;
    }
    garb_status_t st = garb_parse_sheet_text(text, tlen, out);
    os64_free(text);
    out->encoding = encoding;
    return st;
}
