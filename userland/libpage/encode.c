// encode.c — family E: the entry list as BYTES.
//
// Two questions, and they are separate. WHICH ENCODING the values go out in
// is the form's answer, asked of libhtml's own label table because a second
// copy of that table is what this library exists to prevent. HOW they are
// laid out is the enctype's, and there are three layouts, each the
// standard's.
//
// The classic mistake is the join between them: a code point the encoding
// cannot hold becomes `&#NNN;` — the standard's `html` error mode — and
// THOSE BYTES ARE THEN PERCENT-ENCODED LIKE ANY OTHER, so a Greek letter on
// a windows-1252 page goes out as the percent-encoding of `&#937;` and not
// as those five characters.
// Doing it the other way round sends a server an ampersand it will read as
// the start of the next field.

#include "internal.h"
#include "os64/fmt.h"

static bool ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// The Encoding Standard's "get an output encoding". A document may be UTF-16
// and a form may not: there is no way to spell a UTF-16 form submission that
// a server would read, so the standard sends UTF-8 instead.
static const char *output_encoding(const char *name)
{
    if (name == NULL)
        return "utf-8";
    if (os64_streq(name, "utf-16le") || os64_streq(name, "utf-16be"))
        return "utf-8";
    return name;
}

const char *p_encoding_for(const os64_page_t *page, int32_t form)
{
    const os64_page_form_t *f = os64_page_form(page, form);
    const char *labels = f != NULL ? f->accept_charset : NULL;
    if (labels != NULL) {
        // A space-separated list, and the first label that names an encoding
        // libhtml decodes is the one the values go out in.
        for (size_t at = 0; labels[at] != '\0';) {
            while (labels[at] != '\0' && ws(labels[at]))
                at++;
            size_t start = at;
            while (labels[at] != '\0' && !ws(labels[at]))
                at++;
            if (at == start)
                continue;
            const char *named = os64_html_encoding_for_label(labels + start, at - start);
            if (named != NULL)
                return output_encoding(named);
        }
    }
    return output_encoding(page->doc != NULL ? page->doc->charset : NULL);
}

// ── A buffer the body is built in ───────────────────────────────────────

typedef struct {
    char *bytes;
    size_t len, cap;
    size_t ceiling;   // the caller's max_body; past it the whole thing is a refusal
    bool too_long;
    bool no_memory;
} PBuf;

static bool buf_room(PBuf *buf, size_t more)
{
    if (buf->too_long || buf->no_memory)
        return false;
    if (buf->len + more > buf->ceiling) {
        buf->too_long = true;
        return false;
    }
    if (buf->len + more + 1 <= buf->cap)
        return true;
    size_t want = buf->cap != 0 ? buf->cap * 2 : 256;
    while (want < buf->len + more + 1)
        want *= 2;
    char *bigger = os64_realloc(buf->bytes, want);
    if (bigger == NULL) {
        buf->no_memory = true;
        return false;
    }
    buf->bytes = bigger;
    buf->cap = want;
    return true;
}

static bool buf_byte(PBuf *buf, char c)
{
    if (!buf_room(buf, 1))
        return false;
    buf->bytes[buf->len++] = c;
    return true;
}

static bool buf_put(PBuf *buf, const char *s, size_t n)
{
    if (!buf_room(buf, n))
        return false;
    os64_memcpy(buf->bytes + buf->len, s, n);
    buf->len += n;
    return true;
}

static bool buf_text(PBuf *buf, const char *s)
{
    return buf_put(buf, s, os64_strlen(s));
}

// ── The bytes a value becomes ───────────────────────────────────────────

// One code point in the selected encoding, or the standard's `html` error
// mode for one the encoding has no byte for. `emit` is what receives the
// bytes, so the same transcoding serves a query, a part and a line.
typedef bool (*PEmit)(PBuf *buf, const char *bytes, size_t n);

static bool emit_raw(PBuf *buf, const char *bytes, size_t n)
{
    return buf_put(buf, bytes, n);
}

// The urlencoded serializer's safe set: letters, digits, and `*-._`. Space
// is `+`, which is the one place this differs from percent-encoding a URL.
static bool urlencoded_safe(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '*' || c == '-' || c == '.' || c == '_';
}

static bool emit_urlencoded(PBuf *buf, const char *bytes, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (urlencoded_safe(c)) {
            if (!buf_byte(buf, (char)c))
                return false;
        } else if (c == ' ') {
            if (!buf_byte(buf, '+'))
                return false;
        } else if (!buf_byte(buf, '%') || !buf_byte(buf, hex[c >> 4]) ||
                   !buf_byte(buf, hex[c & 0x0F])) {
            return false;
        }
    }
    return true;
}

// A name inside a multipart part header: the three bytes that would break
// the quoted string it sits in, and nothing else.
static bool emit_multipart_name(PBuf *buf, const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char *escape = bytes[i] == '"'    ? "%22"
                             : bytes[i] == '\n' ? "%0A"
                             : bytes[i] == '\r' ? "%0D"
                                                : NULL;
        if (escape != NULL ? !buf_text(buf, escape) : !buf_byte(buf, bytes[i]))
            return false;
    }
    return true;
}

static bool transcode(PBuf *buf, const char *utf8, size_t len, bool to_1252, PEmit emit)
{
    if (!to_1252)
        return emit(buf, utf8, len);
    for (size_t at = 0; at < len;) {
        uint32_t cp = 0;
        size_t took = os64_utf8_decode(utf8 + at, len - at, &cp);
        if (took == 0)
            break;
        at += took;
        uint8_t byte = 0;
        if (os64_html_encode_windows_1252(cp, &byte)) {
            if (!emit(buf, (const char *)&byte, 1))
                return false;
            continue;
        }
        // The `html` error mode: a code point the encoding cannot hold goes
        // out as the markup that names it, and whatever is wrapping this
        // then escapes those bytes like any others.
        char reference[16];
        int32_t n = os64_snprintf(reference, sizeof(reference), "&#%u;", (unsigned)cp);
        if (n <= 0 || !emit(buf, reference, (size_t)n))
            return false;
    }
    return true;
}

// ── The three layouts ───────────────────────────────────────────────────

static bool serialise_urlencoded(PBuf *buf, const PEntries *entries, bool to_1252)
{
    for (int32_t i = 0; i < entries->count; i++) {
        if (i != 0 && !buf_byte(buf, '&'))
            return false;
        if (!transcode(buf, entries->items[i].name, entries->items[i].name_len, to_1252,
                       emit_urlencoded) ||
            !buf_byte(buf, '=') ||
            !transcode(buf, entries->items[i].value, entries->items[i].value_len, to_1252,
                       emit_urlencoded))
            return false;
    }
    return true;
}

// THE BOUNDARY IS DERIVED FROM THE CONTENT, not drawn from a hat. A library
// that does no I/O has no randomness to draw on, and it does not need any:
// what a boundary must be is ABSENT from the content, so it is hashed from
// the content and then lengthened until it does not appear. That also makes
// a submission reproducible, which is what lets a test check its bytes.
static bool boundary_for(const PEntries *entries, char *out, size_t cap)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (int32_t i = 0; i < entries->count; i++) {
        for (size_t at = 0; at < entries->items[i].name_len; at++) {
            hash ^= (unsigned char)entries->items[i].name[at];
            hash *= 0x100000001b3ull;
        }
        for (size_t at = 0; at < entries->items[i].value_len; at++) {
            hash ^= (unsigned char)entries->items[i].value[at];
            hash *= 0x100000001b3ull;
        }
    }
    size_t len = (size_t)os64_snprintf(out, cap, "----os64page%016lx", (unsigned long)hash);
    if (len >= cap)
        return false;
    for (;;) {
        bool clashes = false;
        for (int32_t i = 0; i < entries->count && !clashes; i++) {
            const PEntry *entry = &entries->items[i];
            for (size_t at = 0; at + len <= entry->value_len && !clashes; at++)
                clashes = os64_memcmp(entry->value + at, out, len) == 0;
            for (size_t at = 0; at + len <= entry->name_len && !clashes; at++)
                clashes = os64_memcmp(entry->name + at, out, len) == 0;
        }
        if (!clashes)
            return true;
        // A longer string cannot appear where a shorter one did not, so this
        // ends — at worst when the boundary outgrows the content.
        if (len + 2 >= cap || len >= 70)
            return false;   // RFC 7578's ceiling on how long one may be
        out[len++] = '-';
        out[len] = '\0';
    }
}

static bool serialise_multipart(PBuf *buf, const PEntries *entries, bool to_1252,
                                const char *boundary)
{
    for (int32_t i = 0; i < entries->count; i++) {
        const PEntry *entry = &entries->items[i];
        if (!buf_text(buf, "--") || !buf_text(buf, boundary) || !buf_text(buf, "\r\n") ||
            !buf_text(buf, "Content-Disposition: form-data; name=\"") ||
            !transcode(buf, entry->name, entry->name_len, to_1252, emit_multipart_name) ||
            !buf_byte(buf, '"'))
            return false;
        if (entry->is_file) {
            // A file part carries the NAME and then the bytes; a field with
            // no file chosen carries a name of nothing and no bytes, which
            // is what is written here because nothing can choose one yet
            // (LIBPAGE.md books the file dialog). The content belongs
            // between the blank line and the CRLF that ends the part.
            if (!buf_text(buf, "; filename=\"") ||
                !transcode(buf, entry->value, entry->value_len, to_1252, emit_multipart_name) ||
                !buf_text(buf, "\"\r\nContent-Type: application/octet-stream\r\n\r\n\r\n"))
                return false;
            continue;
        }
        if (!buf_text(buf, "\r\n\r\n") ||
            !transcode(buf, entry->value, entry->value_len, to_1252, emit_raw) ||
            !buf_text(buf, "\r\n"))
            return false;
    }
    return buf_text(buf, "--") && buf_text(buf, boundary) && buf_text(buf, "--\r\n");
}

// The standard's own note about this one is that it is ambiguous: nothing is
// escaped, so a name holding an `=` cannot be told from a value. That is the
// format, and inventing an escape would make os64 the only thing that reads
// what it writes.
static bool serialise_text_plain(PBuf *buf, const PEntries *entries, bool to_1252)
{
    for (int32_t i = 0; i < entries->count; i++) {
        if (!transcode(buf, entries->items[i].name, entries->items[i].name_len, to_1252,
                       emit_raw) ||
            !buf_byte(buf, '=') ||
            !transcode(buf, entries->items[i].value, entries->items[i].value_len, to_1252,
                       emit_raw) ||
            !buf_text(buf, "\r\n"))
            return false;
    }
    return true;
}

bool p_serialise(const PEntries *entries, os64_page_enctype_t enctype, const char *encoding,
                 size_t max_body, char **bytes, size_t *len, char **type,
                 os64_page_reason_t *refused)
{
    *bytes = NULL;
    *len = 0;
    *type = NULL;
    bool to_1252 = os64_streq(encoding, "windows-1252");
    PBuf buf = {NULL, 0, 0, max_body, false, false};
    char boundary[96], content_type[128];
    content_type[0] = '\0';
    bool ok = false;
    switch (enctype) {
    case OS64_PAGE_ENCTYPE_MULTIPART:
        if (!boundary_for(entries, boundary, sizeof(boundary))) {
            *refused = OS64_PAGE_REASON_BODY_TOO_LONG;
            return false;
        }
        os64_snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s",
                      boundary);
        ok = serialise_multipart(&buf, entries, to_1252, boundary);
        break;
    case OS64_PAGE_ENCTYPE_TEXT_PLAIN:
        os64_strcopy(content_type, sizeof(content_type), "text/plain");
        ok = serialise_text_plain(&buf, entries, to_1252);
        break;
    default:
        os64_strcopy(content_type, sizeof(content_type), "application/x-www-form-urlencoded");
        ok = serialise_urlencoded(&buf, entries, to_1252);
        break;
    }
    if (!ok || buf.too_long || buf.no_memory) {
        os64_free(buf.bytes);
        *refused = buf.too_long ? OS64_PAGE_REASON_BODY_TOO_LONG : OS64_PAGE_REASON_NO_MEMORY;
        return false;
    }
    // An empty entry list is an empty body, and a body of nothing still has
    // to be a pointer a caller can free.
    if (!buf_room(&buf, 1)) {
        os64_free(buf.bytes);
        *refused = OS64_PAGE_REASON_NO_MEMORY;
        return false;
    }
    buf.bytes[buf.len] = '\0';
    size_t type_len = os64_strlen(content_type);
    char *type_copy = os64_malloc(type_len + 1);
    if (type_copy == NULL) {
        os64_free(buf.bytes);
        *refused = OS64_PAGE_REASON_NO_MEMORY;
        return false;
    }
    os64_memcpy(type_copy, content_type, type_len + 1);
    *bytes = buf.bytes;
    *len = buf.len;
    *type = type_copy;
    return true;
}
