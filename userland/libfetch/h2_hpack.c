// h2_hpack.c — see h2_hpack.h.
#include "h2_hpack.h"

#include "h2_hpack_tables.h"
#include "os64/str.h"

// RFC 7541 § 5.1 gives an integer no upper bound; this is ours. Nothing HTTP
// carries is this long, and a decoder that kept adding would wrap.
#define INT_MOST 0x7fffffffu

static bool same(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len)
{
    return a_len == b_len && (a_len == 0 || os64_memcmp(a, b, a_len) == 0);
}

// ── The dynamic table ───────────────────────────────────────────────────

static bool table_init(h2_hpack_table_t *t, void *storage, size_t storage_len, uint32_t room, uint32_t most)
{
    if (storage == NULL || storage_len < H2_HPACK_STORAGE(room) ||
        (uintptr_t)storage % _Alignof(h2_hpack_entry_t) != 0)
        return false;
    uint32_t slots = room / H2_HPACK_ENTRY_OVERHEAD + 1u;
    *t = (h2_hpack_table_t){.entries = storage, .slots = slots, .room = room, .most = most,
                            .bytes = (uint8_t *)storage + (size_t)slots * sizeof(h2_hpack_entry_t)};
    return true;
}

// Entry `i` of the table, 0 the newest.
static const h2_hpack_entry_t *table_at(const h2_hpack_table_t *t, uint32_t i)
{
    return &t->entries[(t->newest + i) % t->slots];
}

static void table_drop_oldest(h2_hpack_table_t *t)
{
    const h2_hpack_entry_t *oldest = table_at(t, t->count - 1);
    t->size -= oldest->name_len + oldest->value_len + H2_HPACK_ENTRY_OVERHEAD;
    t->count--;
    // Bytes are laid down in the order fields arrive, so the oldest field
    // left is where the live bytes begin.
    if (t->count != 0)
        t->from = table_at(t, t->count - 1)->at;
    else
        t->from = t->to = 0;
}

static void table_empty(h2_hpack_table_t *t)
{
    while (t->count != 0)
        table_drop_oldest(t);
}

static void table_resize(h2_hpack_table_t *t, uint32_t most)
{
    t->most = most;
    while (t->size > most)
        table_drop_oldest(t);
}

// RFC 7541 § 4.4: fields leave from the old end until the new one fits, and
// one larger than the whole table empties it and is not added. `name` and
// `value` may not point into this table: the bytes they point at can move.
static void table_add(h2_hpack_table_t *t, const uint8_t *name, uint32_t name_len, const uint8_t *value,
                      uint32_t value_len)
{
    uint64_t need = (uint64_t)name_len + value_len + H2_HPACK_ENTRY_OVERHEAD;
    if (need > t->most) {
        table_empty(t);
        return;
    }
    while (t->size + need > t->most)
        table_drop_oldest(t);
    uint32_t bytes = name_len + value_len;
    if (t->room - t->to < bytes) {
        // No room after the last field: slide the live bytes down to the
        // start. They are under `most` less this field's cost, so it fits.
        os64_memmove(t->bytes, t->bytes + t->from, t->to - t->from);
        for (uint32_t i = 0; i < t->count; i++)
            t->entries[(t->newest + i) % t->slots].at -= t->from;
        t->to -= t->from;
        t->from = 0;
    }
    t->newest = (t->newest + t->slots - 1) % t->slots;
    t->entries[t->newest] = (h2_hpack_entry_t){.at = t->to, .name_len = name_len, .value_len = value_len};
    if (name_len != 0)
        os64_memcpy(t->bytes + t->to, name, name_len);
    if (value_len != 0)
        os64_memcpy(t->bytes + t->to + name_len, value, value_len);
    t->to += bytes;
    t->count++;
    t->size += (uint32_t)need;
}

// The field at `index` of the one address space RFC 7541 § 2.3.3 makes of
// the two tables: the static table from 1, the dynamic one after it.
static bool lookup(const h2_hpack_table_t *t, uint32_t index, h2_field_t *field)
{
    if (index == 0)
        return false;
    if (index <= H2_HPACK_STATIC_COUNT) {
        const h2_static_entry_t *s = &kStatic[index - 1];
        *field = (h2_field_t){.name = (const uint8_t *)s->name, .value = (const uint8_t *)s->value,
                              .name_len = s->name_len, .value_len = s->value_len};
        return true;
    }
    index -= H2_HPACK_STATIC_COUNT + 1;
    if (index >= t->count)
        return false;
    const h2_hpack_entry_t *e = table_at(t, index);
    *field = (h2_field_t){.name = t->bytes + e->at, .value = t->bytes + e->at + e->name_len,
                          .name_len = e->name_len, .value_len = e->value_len};
    return true;
}

// ── Huffman (RFC 7541 § 5.2 and Appendix B) ─────────────────────────────

// Decode `n` bytes, writing what fits in `room` and counting it all in
// `*len`. False for a code the RFC makes an error of: EOS inside the string,
// padding of more than seven bits, padding that is not all ones.
static bool huff_decode(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t room, uint32_t *len)
{
    uint32_t code = 0, count = 0;
    unsigned bits = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            code = code << 1 | ((in[i] >> bit) & 1u);
            bits++;
            uint32_t nth = code - kHuffFirst[bits];
            if (code >= kHuffFirst[bits] && nth < kHuffCount[bits]) {
                unsigned symbol = kHuffOrder[kHuffOffset[bits] + nth];
                if (symbol == H2_HUFF_EOS)
                    return false;
                if (count < room)
                    out[count] = (uint8_t)symbol;
                count++;
                code = 0;
                bits = 0;
            } else if (bits == H2_HUFF_LONGEST)
                return false;
        }
    }
    if (bits > 7 || code != (1u << bits) - 1u)
        return false;
    *len = count;
    return true;
}

// In 64 bits: a long string of long codes is more bytes coded than 32 hold.
static uint64_t huff_size(const uint8_t *in, uint32_t n)
{
    uint64_t bits = 0;
    for (uint32_t i = 0; i < n; i++)
        bits += kHuffBits[in[i]];
    return (bits + 7) / 8;
}

static uint8_t *huff_encode(uint8_t *out, const uint8_t *in, uint32_t n)
{
    uint64_t hold = 0;
    unsigned held = 0;
    for (uint32_t i = 0; i < n; i++) {
        hold = hold << kHuffBits[in[i]] | kHuffCode[in[i]];
        held += kHuffBits[in[i]];
        while (held >= 8) {
            held -= 8;
            *out++ = (uint8_t)(hold >> held);
        }
    }
    // The last byte is filled out with ones, the high bits of EOS.
    if (held != 0)
        *out++ = (uint8_t)(hold << (8 - held) | (0xffu >> held));
    return out;
}

// ── Integers and strings (RFC 7541 § 5) ─────────────────────────────────

static bool int_read(const uint8_t **p, const uint8_t *end, unsigned prefix, uint32_t *out)
{
    if (*p >= end)
        return false;
    uint32_t mask = (1u << prefix) - 1u;
    uint64_t value = *(*p)++ & mask;
    if (value == mask) {
        // Five more bytes carry 35 bits, more than INT_MOST needs; a sixth
        // is an integer no one meant, or zeros stacked up to waste time.
        for (unsigned shift = 0;; shift += 7) {
            if (*p >= end || shift > 28)
                return false;
            uint8_t byte = *(*p)++;
            value += (uint64_t)(byte & 0x7fu) << shift;
            if (value > INT_MOST)
                return false;
            if (!(byte & 0x80u))
                break;
        }
    }
    *out = (uint32_t)value;
    return true;
}

// `pattern` is the representation's bits above the prefix.
static uint8_t *int_write(uint8_t *out, unsigned prefix, uint8_t pattern, uint32_t value)
{
    uint32_t mask = (1u << prefix) - 1u;
    if (value < mask) {
        *out++ = (uint8_t)(pattern | value);
        return out;
    }
    *out++ = (uint8_t)(pattern | mask);
    for (value -= mask; value >= 0x80u; value >>= 7)
        *out++ = (uint8_t)(0x80u | (value & 0x7fu));
    *out++ = (uint8_t)value;
    return out;
}

typedef struct {
    const uint8_t *raw;
    uint32_t raw_len;
    bool huffman;
} string_t;

static bool string_read(const uint8_t **p, const uint8_t *end, string_t *s)
{
    if (*p >= end)
        return false;
    s->huffman = (**p & 0x80u) != 0;
    if (!int_read(p, end, 7, &s->raw_len) || s->raw_len > (size_t)(end - *p))
        return false;
    s->raw = *p;
    *p += s->raw_len;
    return true;
}

// The string's bytes into `out` when they fit `room`; `*len` is their count
// either way. False for Huffman coding that is an error.
static bool string_take(const string_t *s, uint8_t *out, uint32_t room, uint32_t *len)
{
    if (s->huffman)
        return huff_decode(s->raw, s->raw_len, out, room, len);
    *len = s->raw_len;
    if (s->raw_len != 0 && s->raw_len <= room)
        os64_memcpy(out, s->raw, s->raw_len);
    return true;
}

static uint8_t *string_write(uint8_t *out, const uint8_t *bytes, uint32_t len, bool huffman)
{
    uint64_t coded = huffman ? huff_size(bytes, len) : len;
    if (coded < len)
        return huff_encode(int_write(out, 7, 0x80, (uint32_t)coded), bytes, len);
    out = int_write(out, 7, 0x00, len);
    if (len != 0)
        os64_memcpy(out, bytes, len);
    return out + len;
}

// ── Decoding ────────────────────────────────────────────────────────────

static uint32_t larger(uint32_t a, uint32_t b) { return a > b ? a : b; }

bool h2_hpack_decoder_init(h2_hpack_decoder_t *decoder, void *storage, size_t storage_len, uint8_t *scratch,
                           size_t scratch_len, uint32_t advertised)
{
    uint32_t room = larger(advertised, H2_HPACK_TABLE_DEFAULT);
    if (advertised > INT_MOST || scratch == NULL || scratch_len < room || scratch_len > INT_MOST)
        return false;
    *decoder = (h2_hpack_decoder_t){.advertised = advertised, .limit = room, .scratch = scratch,
                                    .scratch_room = (uint32_t)scratch_len};
    return table_init(&decoder->table, storage, storage_len, room, H2_HPACK_TABLE_DEFAULT);
}

void h2_hpack_decoder_settings_acked(h2_hpack_decoder_t *decoder)
{
    decoder->limit = decoder->advertised;
    if (decoder->table.most > decoder->advertised)
        decoder->owes_update = true;
}

h2_hpack_result_t h2_hpack_decode(h2_hpack_decoder_t *decoder, const uint8_t *block, size_t len,
                                  h2_hpack_emit_t emit, void *ctx)
{
    h2_hpack_table_t *table = &decoder->table;
    const uint8_t *p = block, *end = block + len;
    h2_hpack_result_t result = H2_HPACK_OK;
    bool telling = true, fields_begun = false;

    while (p < end) {
        uint8_t first = *p;
        h2_field_t field;
        uint32_t index;
        if ((first & 0xe0u) == 0x20u) {
            // A table size update (§ 6.3): at a block's start and nowhere
            // else, and never past the limit, which h2_hpack.h gives for
            // before and after our SETTINGS are acknowledged.
            uint32_t most;
            if (fields_begun || !int_read(&p, end, 5, &most) || most > decoder->limit)
                return H2_HPACK_BAD;
            table_resize(table, most);
            decoder->owes_update = false;
            continue;
        }
        if (decoder->owes_update)
            return H2_HPACK_BAD;
        fields_begun = true;
        if (first & 0x80u) {
            // An indexed field (§ 6.1): handed over where it lies.
            if (!int_read(&p, end, 7, &index) || !lookup(table, index, &field))
                return H2_HPACK_BAD;
            if (telling && !emit(ctx, &field)) {
                telling = false;
                result = H2_HPACK_REFUSED;
            }
            continue;
        }
        // A literal (§ 6.2): added to the table, or not, or never to be.
        bool adds = (first & 0xc0u) == 0x40u;
        bool sensitive = !adds && (first & 0x10u) != 0;
        if (!int_read(&p, end, adds ? 6 : 4, &index))
            return H2_HPACK_BAD;
        // The name and then the value into the scratch, side by side. A
        // name the table holds is copied out of it first: adding the field
        // can move or drop the very entry the name came from.
        uint32_t name_len, value_len;
        string_t s;
        if (index != 0) {
            if (!lookup(table, index, &field))
                return H2_HPACK_BAD;
            name_len = field.name_len;
            if (name_len != 0 && name_len <= decoder->scratch_room)
                os64_memcpy(decoder->scratch, field.name, name_len);
        } else if (!string_read(&p, end, &s) || !string_take(&s, decoder->scratch, decoder->scratch_room, &name_len))
            return H2_HPACK_BAD;
        bool fits = name_len <= decoder->scratch_room;
        uint32_t left = fits ? decoder->scratch_room - name_len : 0;
        if (!string_read(&p, end, &s) ||
            !string_take(&s, fits ? decoder->scratch + name_len : decoder->scratch, left, &value_len))
            return H2_HPACK_BAD;
        fits = fits && value_len <= left;
        if (!fits) {
            // Larger than the scratch, so larger than the table can be: to
            // add it is to empty the table (§ 4.4), and that is all of it
            // the table needs. The head it belonged to has a hole in it.
            if (adds)
                table_empty(table);
            telling = false;
            result = H2_HPACK_REFUSED;
            continue;
        }
        field = (h2_field_t){.name = decoder->scratch, .value = decoder->scratch + name_len, .name_len = name_len,
                             .value_len = value_len, .sensitive = sensitive};
        if (adds)
            table_add(table, field.name, name_len, field.value, value_len);
        if (telling && !emit(ctx, &field)) {
            telling = false;
            result = H2_HPACK_REFUSED;
        }
    }
    // An update still owed is one this block did not begin with: a block
    // with nothing in it is a block (§ 4.2).
    if (decoder->owes_update)
        return H2_HPACK_BAD;
    return result;
}

// ── Encoding ────────────────────────────────────────────────────────────

bool h2_hpack_encoder_init(h2_hpack_encoder_t *encoder, void *storage, size_t storage_len, uint32_t ceiling)
{
    if (ceiling > INT_MOST)
        return false;
    uint32_t most = ceiling < H2_HPACK_TABLE_DEFAULT ? ceiling : H2_HPACK_TABLE_DEFAULT;
    // A table smaller than the default is one the peer's decoder does not
    // know of until it is told: the first block says so.
    *encoder = (h2_hpack_encoder_t){.ceiling = ceiling, .target = most, .least = most,
                                    .announce = most != H2_HPACK_TABLE_DEFAULT, .huffman = true};
    return table_init(&encoder->table, storage, storage_len, ceiling, most);
}

void h2_hpack_encoder_peer_table_size(h2_hpack_encoder_t *encoder, uint32_t size)
{
    uint32_t most = size < encoder->ceiling ? size : encoder->ceiling;
    if (most == encoder->table.most)
        return;
    table_resize(&encoder->table, most);
    if (!encoder->announce || most < encoder->least)
        encoder->least = most;
    encoder->target = most;
    encoder->announce = true;
}

size_t h2_hpack_encode_bound(const h2_field_t *fields, size_t count)
{
    // Two size updates of six bytes each at most; then a field at its
    // longest is a literal name and value, each behind a six-byte length,
    // behind one byte of representation. Huffman is used only when shorter.
    size_t bound = 12;
    for (size_t i = 0; i < count; i++)
        bound += 13 + (size_t)fields[i].name_len + fields[i].value_len;
    return bound;
}

// The index of a field with this name, and `*whole` when one of them has
// the value too (that one's index is answered). 0 when no table has the name.
static uint32_t find(const h2_hpack_table_t *t, const h2_field_t *field, bool *whole)
{
    uint32_t by_name = 0;
    *whole = false;
    for (uint32_t i = 0; i < H2_HPACK_STATIC_COUNT; i++) {
        const h2_static_entry_t *s = &kStatic[i];
        if (!same((const uint8_t *)s->name, s->name_len, field->name, field->name_len))
            continue;
        if (same((const uint8_t *)s->value, s->value_len, field->value, field->value_len)) {
            *whole = true;
            return i + 1;
        }
        if (by_name == 0)
            by_name = i + 1;
    }
    for (uint32_t i = 0; i < t->count; i++) {
        const h2_hpack_entry_t *e = table_at(t, i);
        const uint8_t *name = t->bytes + e->at;
        if (!same(name, e->name_len, field->name, field->name_len))
            continue;
        if (same(name + e->name_len, e->value_len, field->value, field->value_len)) {
            *whole = true;
            return H2_HPACK_STATIC_COUNT + 1 + i;
        }
        if (by_name == 0)
            by_name = H2_HPACK_STATIC_COUNT + 1 + i;
    }
    return by_name;
}

bool h2_hpack_encode(h2_hpack_encoder_t *encoder, const h2_field_t *fields, size_t count, uint8_t *out,
                     size_t room, size_t *len)
{
    static const uint8_t kAuthorization[] = "authorization";
    h2_hpack_table_t *table = &encoder->table;
    *len = 0;
    if (room < h2_hpack_encode_bound(fields, count))
        return false;
    for (size_t i = 0; i < count; i++)
        if (fields[i].name_len > INT_MOST || fields[i].value_len > INT_MOST)
            return false;
    uint8_t *p = out;
    if (encoder->announce) {
        if (encoder->least < encoder->target)
            p = int_write(p, 5, 0x20, encoder->least);
        p = int_write(p, 5, 0x20, encoder->target);
        encoder->announce = false;
    }
    for (size_t i = 0; i < count; i++) {
        const h2_field_t *f = &fields[i];
        bool whole;
        uint32_t index = find(table, f, &whole);
        if (whole) {
            p = int_write(p, 7, 0x80, index);
            continue;
        }
        bool never = f->sensitive || same(f->name, f->name_len, kAuthorization, sizeof(kAuthorization) - 1);
        bool adds = !never && (uint64_t)f->name_len + f->value_len + H2_HPACK_ENTRY_OVERHEAD <= table->most;
        p = adds ? int_write(p, 6, 0x40, index) : int_write(p, 4, never ? 0x10 : 0x00, index);
        if (index == 0)
            p = string_write(p, f->name, f->name_len, encoder->huffman);
        p = string_write(p, f->value, f->value_len, encoder->huffman);
        if (adds)
            table_add(table, f->name, f->name_len, f->value, f->value_len);
    }
    *len = (size_t)(p - out);
    return true;
}

// ── The rules on a field list ───────────────────────────────────────────

static bool named(const h2_field_t *f, const char *name)
{
    return same(f->name, f->name_len, (const uint8_t *)name, (uint32_t)os64_strlen(name));
}

h2_head_rule_t h2_head_check(const h2_field_t *fields, size_t count, h2_head_kind_t kind, size_t *at)
{
    static const char *const kRequest[] = {":method", ":scheme", ":path", ":authority"};
    static const char *const kResponse[] = {":status"};
    static const char *const kConnection[] = {"connection", "keep-alive", "proxy-connection", "transfer-encoding",
                                              "upgrade"};
    const char *const *pseudo = kind == H2_HEAD_REQUEST ? kRequest : kResponse;
    unsigned pseudos = kind == H2_HEAD_REQUEST ? 4 : kind == H2_HEAD_RESPONSE ? 1 : 0;
    unsigned seen = 0;
    bool regular = false;
    size_t i = 0, unused;
    if (at == NULL)
        at = &unused;
    for (; i < count; i++) {
        const h2_field_t *f = &fields[i];
        *at = i;
        if (f->name_len == 0)
            return H2_HEAD_NAME_EMPTY;
        for (uint32_t k = 0; k < f->name_len; k++) {
            uint8_t c = f->name[k];
            if (c <= 0x20 || c >= 0x7f || (c >= 'A' && c <= 'Z') || (c == ':' && k != 0))
                return H2_HEAD_NAME_BYTES;
        }
        for (uint32_t k = 0; k < f->value_len; k++) {
            uint8_t c = f->value[k];
            bool edge = k == 0 || k == f->value_len - 1;
            if (c == 0 || c == '\r' || c == '\n' || (edge && (c == ' ' || c == '\t')))
                return H2_HEAD_VALUE_BYTES;
        }
        if (f->name[0] == ':') {
            unsigned which = 0;
            while (which < pseudos && !named(f, pseudo[which]))
                which++;
            if (which == pseudos)
                return H2_HEAD_PSEUDO_UNKNOWN;
            if (regular)
                return H2_HEAD_PSEUDO_LATE;
            if (seen & (1u << which))
                return H2_HEAD_PSEUDO_TWICE;
            seen |= 1u << which;
            if (kind == H2_HEAD_RESPONSE) {
                if (f->value_len != 3)
                    return H2_HEAD_STATUS;
                for (uint32_t k = 0; k < 3; k++)
                    if (f->value[k] < '0' || f->value[k] > '9')
                        return H2_HEAD_STATUS;
            }
            continue;
        }
        regular = true;
        for (size_t k = 0; k < sizeof(kConnection) / sizeof(kConnection[0]); k++)
            if (named(f, kConnection[k]))
                return H2_HEAD_CONNECTION_FIELD;
        if (named(f, "te") && !same(f->value, f->value_len, (const uint8_t *)"trailers", 8))
            return H2_HEAD_TE;
        if (named(f, "content-length")) {
            uint64_t n = 0;
            if (f->value_len == 0)
                return H2_HEAD_CONTENT_LENGTH;
            for (uint32_t k = 0; k < f->value_len; k++) {
                uint8_t c = f->value[k];
                if (c < '0' || c > '9' || n > (UINT64_MAX - (uint64_t)(c - '0')) / 10)
                    return H2_HEAD_CONTENT_LENGTH;
                n = n * 10 + (uint64_t)(c - '0');
            }
        }
    }
    *at = count;
    // A request's authority may be absent (§ 8.3.1); its other three may not.
    unsigned needed = kind == H2_HEAD_REQUEST ? 0x7u : kind == H2_HEAD_RESPONSE ? 0x1u : 0u;
    if ((seen & needed) != needed)
        return H2_HEAD_PSEUDO_MISSING;
    return H2_HEAD_OK;
}
