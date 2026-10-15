// font_unwrap.c — a web font's container taken off (font_unwrap.h).

#include "font_unwrap.h"
#include "gzip/zlib.h"
#include "os64/mem.h"
#include "os64/str.h"

#define WOFF_HEADER 44u
#define WOFF_ENTRY 20u
#define SFNT_HEADER 12u
#define SFNT_ENTRY 16u

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

yonder_font_kind_t yonder_font_sniff(const uint8_t *bytes, size_t len)
{
    if (bytes == NULL || len < 4)
        return YONDER_FONT_UNKNOWN;
    uint32_t tag = be32(bytes);
    if (tag == 0x00010000u || tag == 0x4F54544Fu /* OTTO */ || tag == 0x74727565u /* true */)
        return YONDER_FONT_SFNT;
    if (tag == 0x774F4646u)     // wOFF
        return YONDER_FONT_WOFF;
    if (tag == 0x774F4632u)     // wOF2
        return YONDER_FONT_WOFF2;
    return YONDER_FONT_UNKNOWN;
}

// One table of the WOFF directory.
typedef struct {
    uint32_t tag, offset, comp, orig, checksum;
} Table;

// Insertion sort by offset: a directory holds tens of tables, and this
// keeps the unwrapper free of qsort's callback.
static void by_offset(const Table *t, uint16_t *order, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++)
        order[i] = i;
    for (uint16_t i = 1; i < n; i++) {
        uint16_t k = order[i], j = i;
        for (; j > 0 && t[order[j - 1]].offset > t[k].offset; j--)
            order[j] = order[j - 1];
        order[j] = k;
    }
}

// A table's bytes into `out` (exactly `t->orig` of them): stored as they
// are when its compressed length is its length, else a zlib stream that
// makes exactly that many and ends where the table does.
static yonder_font_status_t unpack(os64_zlib_t *z, const uint8_t *woff, const Table *t, uint8_t *out)
{
    if (t->comp == t->orig) {
        os64_memcpy(out, woff + t->offset, t->orig);
        return YONDER_FONT_OK;
    }
    os64_zlib_reset(z, t->orig);
    const uint8_t *in = woff + t->offset;
    size_t in_left = t->comp, out_left = t->orig;
    os64_zlib_status_t st = os64_zlib_process(z, &in, &in_left, &out, &out_left, true);
    return st == OS64_ZLIB_DONE && in_left == 0 && out_left == 0 ? YONDER_FONT_OK
                                                                 : YONDER_FONT_MALFORMED;
}

yonder_font_status_t yonder_font_unwrap_woff(const uint8_t *woff, size_t len, uint8_t **sfnt,
                                             size_t *sfnt_len)
{
    *sfnt = NULL;
    *sfnt_len = 0;
    if (woff == NULL || len < WOFF_HEADER || yonder_font_sniff(woff, len) != YONDER_FONT_WOFF)
        return YONDER_FONT_MALFORMED;
    uint32_t flavor = be32(woff + 4), length = be32(woff + 8);
    uint16_t ntables = be16(woff + 12), reserved = be16(woff + 14);
    uint32_t total = be32(woff + 16);
    uint32_t meta_offset = be32(woff + 24), meta_length = be32(woff + 28);
    uint32_t priv_offset = be32(woff + 36), priv_length = be32(woff + 40);
    if (length != len || ntables == 0 || reserved != 0 ||
        WOFF_HEADER + (uint64_t)WOFF_ENTRY * ntables > len ||
        (meta_length != 0 && (uint64_t)meta_offset + meta_length > len) ||
        (priv_length != 0 && (uint64_t)priv_offset + priv_length > len))
        return YONDER_FONT_MALFORMED;
    if (total > YONDER_FONT_SFNT_MAX || (uint64_t)total > (uint64_t)YONDER_FONT_EXPANSION_MAX * len)
        return YONDER_FONT_LIMIT;

    Table *t = os64_malloc((size_t)ntables * sizeof(*t));
    uint16_t *order = os64_malloc((size_t)ntables * sizeof(*order));
    if (t == NULL || order == NULL) {
        os64_free(t);
        os64_free(order);
        return YONDER_FONT_NO_MEMORY;
    }
    // The directory: tags ascending, each table in the file past it,
    // 4-byte aligned, and no longer packed than unpacked; the SFNT it makes
    // as long as the header says.
    yonder_font_status_t st = YONDER_FONT_OK;
    uint64_t need = SFNT_HEADER + (uint64_t)SFNT_ENTRY * ntables;
    uint32_t first = WOFF_HEADER + WOFF_ENTRY * (uint32_t)ntables;
    for (uint16_t i = 0; i < ntables && st == YONDER_FONT_OK; i++) {
        const uint8_t *e = woff + WOFF_HEADER + (size_t)WOFF_ENTRY * i;
        t[i] = (Table){be32(e), be32(e + 4), be32(e + 8), be32(e + 12), be32(e + 16)};
        if ((i > 0 && t[i].tag <= t[i - 1].tag) || t[i].offset % 4 != 0 || t[i].offset < first ||
            (uint64_t)t[i].offset + t[i].comp > len || t[i].comp > t[i].orig)
            st = YONDER_FONT_MALFORMED;
        need += ((uint64_t)t[i].orig + 3) & ~(uint64_t)3;
    }
    if (st == YONDER_FONT_OK && need != total)
        st = YONDER_FONT_MALFORMED;
    // No two tables overlap, read in the order they lie in.
    if (st == YONDER_FONT_OK) {
        by_offset(t, order, ntables);
        for (uint16_t i = 1; i < ntables && st == YONDER_FONT_OK; i++)
            if ((uint64_t)t[order[i - 1]].offset + t[order[i - 1]].comp > t[order[i]].offset)
                st = YONDER_FONT_MALFORMED;
    }
    uint8_t *out = st == YONDER_FONT_OK ? os64_malloc(total) : NULL;
    os64_zlib_t *z = st == YONDER_FONT_OK ? os64_zlib_create(0) : NULL;
    if (st == YONDER_FONT_OK && (out == NULL || z == NULL))
        st = YONDER_FONT_NO_MEMORY;
    if (st == YONDER_FONT_OK) {
        os64_memset(out, 0, total);
        uint16_t power = 1, log = 0;
        while ((uint32_t)power * 2 <= ntables)
            power = (uint16_t)(power * 2), log++;
        put32(out, flavor);
        put16(out + 4, ntables);
        put16(out + 6, (uint16_t)(power * SFNT_ENTRY));
        put16(out + 8, log);
        put16(out + 10, (uint16_t)(ntables * SFNT_ENTRY - power * SFNT_ENTRY));
        // The data in the order it lay in the container; the directory in
        // the order of its tags.
        uint32_t at = SFNT_HEADER + SFNT_ENTRY * (uint32_t)ntables;
        for (uint16_t i = 0; i < ntables && st == YONDER_FONT_OK; i++) {
            const Table *x = &t[order[i]];
            uint8_t *e = out + SFNT_HEADER + (size_t)SFNT_ENTRY * order[i];
            put32(e, x->tag);
            put32(e + 4, x->checksum);
            put32(e + 8, at);
            put32(e + 12, x->orig);
            st = unpack(z, woff, x, out + at);
            at += (x->orig + 3) & ~3u;
        }
    }
    os64_zlib_destroy(z);
    os64_free(t);
    os64_free(order);
    if (st != YONDER_FONT_OK) {
        os64_free(out);
        return st;
    }
    *sfnt = out;
    *sfnt_len = total;
    return YONDER_FONT_OK;
}

const char *yonder_font_status_name(yonder_font_status_t status)
{
    switch (status) {
        case YONDER_FONT_OK:        return "ok";
        case YONDER_FONT_MALFORMED: return "malformed container";
        case YONDER_FONT_LIMIT:     return "past the size limits";
        case YONDER_FONT_NO_MEMORY: return "no memory";
    }
    return "unknown";
}
