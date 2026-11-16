// h2_frame.h — HTTP/2's frames as bytes (RFC 9113 §§ 4 and 6), and nothing
// above them. HTTP2.md § 4 H2.3 is the brief.
//
// PURE: no syscall, no allocation, no clock, no lock. A caller hands in
// bytes and gets facts back, or hands in facts and a buffer and gets bytes.
// What a frame MEANS to a connection or a stream (whether the stream is
// open, whose window it spends, what a setting changes) is the
// connection's, and is not here.
//
// A FRAME THAT BREAKS A RULE is answered with a fault: RFC 9113's error
// code, the rule, and whether the RFC makes it the stream's error or the
// whole connection's. A frame of a type this file does not know is not a
// fault (§ 4.1): it parses as H2_FRAME_UNKNOWN, to be ignored.
#ifndef FETCH_H2_FRAME_H
#define FETCH_H2_FRAME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define H2_FRAME_HEADER_LEN 9u
#define H2_FRAME_SIZE_LEAST 16384u       // SETTINGS_MAX_FRAME_SIZE's floor and its initial value
#define H2_FRAME_SIZE_MOST  16777215u    // and its ceiling, 2^24 - 1
#define H2_STREAM_MOST      0x7fffffffu  // a stream id, a window increment: 31 bits
#define H2_PREFACE_LEN      24u

// "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n": what a client sends before its first frame.
extern const uint8_t h2_preface[H2_PREFACE_LEN];

enum {
    H2_FRAME_DATA = 0,
    H2_FRAME_HEADERS = 1,
    H2_FRAME_PRIORITY = 2,
    H2_FRAME_RST_STREAM = 3,
    H2_FRAME_SETTINGS = 4,
    H2_FRAME_PUSH_PROMISE = 5,
    H2_FRAME_PING = 6,
    H2_FRAME_GOAWAY = 7,
    H2_FRAME_WINDOW_UPDATE = 8,
    H2_FRAME_CONTINUATION = 9,
    H2_FRAME_UNKNOWN = 0xff,    // a parsed frame's `kind` only, never a wire type we send
};

enum {
    H2_FLAG_END_STREAM = 0x01,  // DATA, HEADERS
    H2_FLAG_ACK = 0x01,         // SETTINGS, PING
    H2_FLAG_END_HEADERS = 0x04, // HEADERS, PUSH_PROMISE, CONTINUATION
    H2_FLAG_PADDED = 0x08,      // DATA, HEADERS, PUSH_PROMISE
    H2_FLAG_PRIORITY = 0x20,    // HEADERS
};

// RFC 9113 § 7.
enum {
    H2_ERR_NO_ERROR = 0x0,
    H2_ERR_PROTOCOL = 0x1,
    H2_ERR_INTERNAL = 0x2,
    H2_ERR_FLOW_CONTROL = 0x3,
    H2_ERR_SETTINGS_TIMEOUT = 0x4,
    H2_ERR_STREAM_CLOSED = 0x5,
    H2_ERR_FRAME_SIZE = 0x6,
    H2_ERR_REFUSED_STREAM = 0x7,
    H2_ERR_CANCEL = 0x8,
    H2_ERR_COMPRESSION = 0x9,
    H2_ERR_CONNECT = 0xa,
    H2_ERR_ENHANCE_YOUR_CALM = 0xb,
    H2_ERR_INADEQUATE_SECURITY = 0xc,
    H2_ERR_HTTP_1_1_REQUIRED = 0xd,
};

// RFC 9113 § 6.5.2.
enum {
    H2_SETTING_HEADER_TABLE_SIZE = 0x1,
    H2_SETTING_ENABLE_PUSH = 0x2,
    H2_SETTING_MAX_CONCURRENT_STREAMS = 0x3,
    H2_SETTING_INITIAL_WINDOW_SIZE = 0x4,
    H2_SETTING_MAX_FRAME_SIZE = 0x5,
    H2_SETTING_MAX_HEADER_LIST_SIZE = 0x6,
};

// The names as the RFCs spell them ("PROTOCOL_ERROR", "MAX_FRAME_SIZE"),
// for a reason sentence. A number with no name answers "UNKNOWN_ERROR" or
// "UNKNOWN_SETTING": a peer may send either, and neither is ours to refuse.
const char *h2_error_name(uint32_t code);
const char *h2_setting_name(uint32_t identifier);

// Which rule a frame broke.
typedef enum {
    H2_RULE_NONE = 0,
    H2_RULE_TOO_LONG,           // longer than the max frame size the receiver set
    H2_RULE_LENGTH,             // not the length its type has, or too short for its fixed fields
    H2_RULE_NEEDS_STREAM,       // a stream's frame on stream 0
    H2_RULE_NEEDS_CONNECTION,   // a connection's frame on a stream
    H2_RULE_PADDING,            // padding as long as what it pads, or longer
    H2_RULE_SETTINGS_ACK,       // a SETTINGS acknowledgement with a payload
    H2_RULE_SETTING_VALUE,      // a setting's value outside what the RFC allows it
    H2_RULE_WINDOW_ZERO,        // a WINDOW_UPDATE of nothing
    H2_RULE_BLOCK_INTERRUPTED,  // a field block open, and something other than its CONTINUATION
    H2_RULE_BLOCK_NOT_OPEN,     // a CONTINUATION continuing nothing
    H2_RULE_BLOCK_TOO_LARGE,    // a field block past the room given for one
} h2_rule_t;

typedef struct {
    uint32_t error;             // H2_ERR_*
    h2_rule_t rule;
    bool stream;                // the stream's error (reset it); otherwise the connection's
} h2_fault_t;

typedef struct {
    uint32_t length;            // of the payload, 24 bits
    uint8_t type, flags;
    uint32_t stream;            // the reserved bit already cleared
} h2_frame_header_t;

void h2_frame_header_read(const uint8_t bytes[H2_FRAME_HEADER_LEN], h2_frame_header_t *header);
void h2_frame_header_write(uint8_t bytes[H2_FRAME_HEADER_LEN], const h2_frame_header_t *header);

// Judge a header alone, before its payload is read: false and the fault when
// the payload is longer than `max_frame_size`, the size the receiver set.
bool h2_frame_header_check(const h2_frame_header_t *header, uint32_t max_frame_size, h2_fault_t *fault);

// One frame, its rules checked and its fields taken apart. `data` points
// into the payload the caller gave and lives as long as that does.
typedef struct {
    uint8_t kind;               // H2_FRAME_*: the type, or H2_FRAME_UNKNOWN
    uint8_t type, flags;        // as on the wire
    uint32_t stream;
    // DATA: the body bytes. HEADERS, PUSH_PROMISE, CONTINUATION: the field
    // block fragment. SETTINGS: its entries (h2_settings_at). PING: its
    // eight bytes. GOAWAY: the debug data. Otherwise empty.
    const uint8_t *data;
    uint32_t data_len;
    // DATA: the whole payload's length, padding and all, which is what flow
    // control counts (§ 6.9.1).
    uint32_t flow_len;
    // RST_STREAM and GOAWAY: the error code. WINDOW_UPDATE: the increment.
    // PUSH_PROMISE: the promised stream.
    uint32_t value;
    uint32_t last_stream;       // GOAWAY
    // PRIORITY, and a HEADERS with the PRIORITY flag.
    bool has_priority, exclusive;
    uint32_t depends_on;
    uint8_t weight;             // as on the wire: one less than the weight
} h2_frame_t;

// `header` and its `length` bytes of payload. False and the fault when the
// frame breaks one of the frame rules h2_rule_t names.
bool h2_frame_parse(const h2_frame_header_t *header, const uint8_t *payload, uint32_t max_frame_size,
                    h2_frame_t *frame, h2_fault_t *fault);

// A SETTINGS frame's entries, in wire order.
typedef struct {
    uint16_t identifier;
    uint32_t value;
} h2_setting_t;
uint32_t h2_settings_count(const h2_frame_t *frame);
h2_setting_t h2_settings_at(const h2_frame_t *frame, uint32_t i);
// What § 6.5.2 allows a setting's value: ENABLE_PUSH 0 or 1, an initial
// window of at most 2^31 - 1 (FLOW_CONTROL_ERROR past it), a max frame size
// within its two bounds. An identifier with no rule here passes.
bool h2_setting_check(h2_setting_t setting, h2_fault_t *fault);

// ── A field block across frames (§ 4.3) ─────────────────────────────────
//
// A HEADERS or PUSH_PROMISE without END_HEADERS opens a block, and until a
// CONTINUATION with END_HEADERS ends it, the next frame on the connection
// must be a CONTINUATION on the same stream: anything else, an unknown type
// included, is the connection's PROTOCOL_ERROR. The fragments are gathered
// into the caller's buffer; a block past its room is the connection's
// ENHANCE_YOUR_CALM, since the bytes that would keep HPACK in step are gone.
typedef struct {
    uint8_t *buffer;
    size_t room, len;
    bool open;
    h2_frame_t first;           // the frame that opened it, its `data` not kept
} h2_block_t;

typedef enum {
    H2_BLOCK_NOT_MINE,          // no block open and the frame opens none: the caller's as it is
    H2_BLOCK_MORE,              // gathered; more is owed
    H2_BLOCK_DONE,              // `buffer[0..len)` is the whole block; `first` says whose
    H2_BLOCK_FAULT,
} h2_block_step_t;

void h2_block_init(h2_block_t *block, uint8_t *buffer, size_t room);
h2_block_step_t h2_block_feed(h2_block_t *block, const h2_frame_t *frame, h2_fault_t *fault);

// ── Writers ─────────────────────────────────────────────────────────────
//
// Each writes whole frames at `out` and answers how many bytes, or 0 when
// they do not fit in `room` or an argument is one the frame cannot carry (a
// stream past 31 bits, an increment of nothing). SETTINGS and GOAWAY are
// written as one frame within the smallest max frame size a peer may have,
// and refuse more than that holds. Nothing is written on 0.

// The settings IN THE ORDER GIVEN: a greeting's order is part of what a
// server sees of a client (HTTP2.md § 2.9).
size_t h2_write_settings(uint8_t *out, size_t room, const h2_setting_t *settings, uint32_t count);
size_t h2_write_settings_ack(uint8_t *out, size_t room);
size_t h2_write_window_update(uint8_t *out, size_t room, uint32_t stream, uint32_t increment);
size_t h2_write_ping(uint8_t *out, size_t room, bool ack, const uint8_t opaque[8]);
size_t h2_write_rst_stream(uint8_t *out, size_t room, uint32_t stream, uint32_t error);
size_t h2_write_goaway(uint8_t *out, size_t room, uint32_t last_stream, uint32_t error,
                       const uint8_t *debug, uint32_t debug_len);
size_t h2_write_priority(uint8_t *out, size_t room, uint32_t stream, uint32_t depends_on, bool exclusive,
                         uint8_t weight);

typedef struct {
    uint32_t depends_on;
    bool exclusive;
    uint8_t weight;
} h2_priority_t;

// A field block as one HEADERS and as many CONTINUATIONs as `max_frame_size`
// (the PEER's) makes of it, END_HEADERS on the last. `priority` NULL sends
// none. h2_headers_size is what it will write.
size_t h2_headers_size(size_t block_len, uint32_t max_frame_size, bool with_priority);
size_t h2_write_headers(uint8_t *out, size_t room, uint32_t stream, const uint8_t *block, size_t block_len,
                        bool end_stream, const h2_priority_t *priority, uint32_t max_frame_size);

// One DATA frame carrying the first `most` bytes of `data` at most, with
// END_STREAM when `end_stream` and it carried them all. `*taken` is how many
// it carried. An empty frame is written for no data, which is how a stream
// is ended after its last byte has gone.
size_t h2_write_data(uint8_t *out, size_t room, uint32_t stream, const uint8_t *data, size_t len, uint32_t most,
                     bool end_stream, size_t *taken);

#endif
