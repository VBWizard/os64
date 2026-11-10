// h2_hpack.h — HPACK, HTTP/2's field compression (RFC 7541), and the rules
// a decoded field list must keep (RFC 9113 § 8.2). HTTP2.md § 4 H2.3 is the
// brief.
//
// PURE, as h2_frame.h is: no syscall, no allocation, no clock, no lock. The
// caller gives each side its storage once.
//
// TWO TABLES, ONE PER DIRECTION, AND EACH MUST SEE EVERY BLOCK. A decoder
// that stops in the middle of a block, or skips one, no longer agrees with
// the encoder at the far end about what index 62 means, and every later
// head on the connection decodes to the wrong fields. So h2_hpack_decode
// reads a block to its end whatever its caller thinks of the fields, and
// the one failure it has (H2_HPACK_BAD) is the connection's
// COMPRESSION_ERROR, after which the decoder is not used again.
#ifndef FETCH_H2_HPACK_H
#define FETCH_H2_HPACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RFC 7541 § 4.1: what a field costs a table, over its name and value.
#define H2_HPACK_ENTRY_OVERHEAD 32u
// SETTINGS_HEADER_TABLE_SIZE's initial value: what both tables are until a
// SETTINGS frame says otherwise.
#define H2_HPACK_TABLE_DEFAULT 4096u

typedef struct {
    uint32_t at, name_len, value_len;   // where its bytes are, name then value
} h2_hpack_entry_t;

// A dynamic table: fields newest first, their bytes in one run. Private to
// h2_hpack.c; here so a caller can hold one.
typedef struct {
    h2_hpack_entry_t *entries;          // a ring: `newest` is index 0 of the table
    uint32_t slots, newest, count;
    uint8_t *bytes;                     // the live ones are [from, to)
    uint32_t room, from, to;
    uint32_t size, most;                // RFC 7541 § 4.1's size, and the size it may not pass
} h2_hpack_table_t;

// The storage a table of at most `most` needs, in bytes, and aligned as a
// uint32_t is: its fields' bytes and one slot for each field it can hold.
#define H2_HPACK_STORAGE(most) \
    ((size_t)(most) + ((size_t)(most) / H2_HPACK_ENTRY_OVERHEAD + 1u) * sizeof(h2_hpack_entry_t))

// A field. Names and values are bytes, not strings: neither ends in a NUL.
typedef struct {
    const uint8_t *name, *value;
    uint32_t name_len, value_len;
    bool sensitive;                     // never to be indexed, by us or by anyone who forwards it
} h2_field_t;

// ── Decoding ────────────────────────────────────────────────────────────

typedef struct {
    h2_hpack_table_t table;
    uint32_t advertised;                // the SETTINGS_HEADER_TABLE_SIZE we send
    uint32_t limit;                     // what a size update may ask for
    bool owes_update;                   // the next block must begin by shrinking the table
    uint8_t *scratch;                   // a literal field's bytes, while it is inserted and handed over
    uint32_t scratch_room;
} h2_hpack_decoder_t;

// `advertised` is the table size our SETTINGS frame offers. `storage` is
// H2_HPACK_STORAGE(the larger of that and H2_HPACK_TABLE_DEFAULT) bytes: the
// peer encodes against the default until it has read our frame. `scratch`
// holds one field's name and value, and must be at least as large as that
// same table size, so that any field the table can hold fits it. False when
// the storage is too small or misaligned.
bool h2_hpack_decoder_init(h2_hpack_decoder_t *decoder, void *storage, size_t storage_len, uint8_t *scratch,
                           size_t scratch_len, uint32_t advertised);

// The peer acknowledged our SETTINGS: from here the table may not pass
// `advertised`, and when it is larger than that now, the peer's next block
// must begin with the size update that shrinks it (RFC 7541 § 4.2).
void h2_hpack_decoder_settings_acked(h2_hpack_decoder_t *decoder);

// One field of a block, in order. The pointers are good until it returns.
// Answer false to be told no more of THIS block.
typedef bool (*h2_hpack_emit_t)(void *ctx, const h2_field_t *field);

typedef enum {
    H2_HPACK_OK,
    // The block was decoded whole and the table is in step, but its fields
    // did not all reach `emit`: it said enough, or one was larger than the
    // scratch. The stream's problem (HTTP2.md § 2.7), not the connection's.
    H2_HPACK_REFUSED,
    H2_HPACK_BAD,                       // COMPRESSION_ERROR: the connection's, and this decoder's end
} h2_hpack_result_t;

// A WHOLE field block (h2_block_t's, or one frame's with END_HEADERS).
h2_hpack_result_t h2_hpack_decode(h2_hpack_decoder_t *decoder, const uint8_t *block, size_t len,
                                  h2_hpack_emit_t emit, void *ctx);

// ── Encoding ────────────────────────────────────────────────────────────

typedef struct {
    h2_hpack_table_t table;
    uint32_t ceiling;                   // the most this encoder will use, whatever a peer allows
    uint32_t target;                    // the size the next block will announce, when `announce`
    uint32_t least;                     // the smallest size asked for since the last block
    bool announce;
    bool huffman;                       // code a string when that is shorter (on unless a caller clears it)
} h2_hpack_encoder_t;

// `storage` is H2_HPACK_STORAGE(ceiling) bytes. The table starts at the
// default size or `ceiling`, whichever is smaller.
bool h2_hpack_encoder_init(h2_hpack_encoder_t *encoder, void *storage, size_t storage_len, uint32_t ceiling);

// The peer's SETTINGS_HEADER_TABLE_SIZE, when its SETTINGS frame is applied.
// The table follows it up to `ceiling`, and the next block says so first:
// the smallest size it passed through and then the size it is at, as RFC
// 7541 § 4.2 asks of several changes between two blocks.
void h2_hpack_encoder_peer_table_size(h2_hpack_encoder_t *encoder, uint32_t size);

// The most bytes `count` fields can encode to, any table state.
size_t h2_hpack_encode_bound(const h2_field_t *fields, size_t count);

// The fields as one block, in order. A field found whole in a table is sent
// as its index; any other is sent literally and added to the table, its name
// as an index when a table has the name. `authorization`, and a field its
// caller marked sensitive, is sent as a never-indexed literal, a table
// holding it whole or not, and is not added; one too large for the table to
// hold is not added either. False, with nothing written and
// the table as it was, when `room` is under h2_hpack_encode_bound, or a name
// or value is longer than 2^31 - 1 bytes.
bool h2_hpack_encode(h2_hpack_encoder_t *encoder, const h2_field_t *fields, size_t count, uint8_t *out,
                     size_t room, size_t *len);

// ── The rules on a field list (RFC 9113 § 8.2 and § 8.3) ────────────────

typedef enum {
    H2_HEAD_REQUEST,
    H2_HEAD_RESPONSE,
    H2_HEAD_TRAILERS,                   // either direction: no pseudo-header fields at all
} h2_head_kind_t;

typedef enum {
    H2_HEAD_OK = 0,
    H2_HEAD_NAME_EMPTY,
    H2_HEAD_NAME_BYTES,                 // uppercase, a control, a space, DEL or above, a colon past the first byte
    H2_HEAD_VALUE_BYTES,                // NUL, CR or LF, or a space or tab at either end
    H2_HEAD_PSEUDO_UNKNOWN,             // one this kind of head does not have
    H2_HEAD_PSEUDO_LATE,                // after a regular field
    H2_HEAD_PSEUDO_TWICE,
    H2_HEAD_PSEUDO_MISSING,             // a request without its method, scheme or path; a response without a status
    H2_HEAD_STATUS,                     // a status that is not three digits
    H2_HEAD_CONNECTION_FIELD,           // connection, keep-alive, proxy-connection, transfer-encoding, upgrade
    H2_HEAD_TE,                         // te with anything but "trailers", in whatever case
    H2_HEAD_CONTENT_LENGTH,             // not a number, or past 64 bits
} h2_head_rule_t;

// The first rule the list breaks, a stream error (PROTOCOL_ERROR) when it is
// not H2_HEAD_OK; `*at` (when not NULL) is the field that broke it, or
// `count` when the fault is something missing.
h2_head_rule_t h2_head_check(const h2_field_t *fields, size_t count, h2_head_kind_t kind, size_t *at);

#endif
