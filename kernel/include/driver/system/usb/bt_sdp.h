#ifndef OS64_BT_SDP_H
#define OS64_BT_SDP_H

// bt_sdp.h — the Service Discovery Protocol on a Classic link, both ends
// (docs/design/pending/AUDIO_CLASSIC.md §§ S2, S3; Bluetooth Core Vol 3
// Part B). The client asks a headphone one question: what Audio Sink are
// you? The responder answers for os64's one record, an Audio Source.
//
// PURE: no kernel headers, no allocation, no waits, no logging, no clock.
// Time is the `now` (milliseconds) its owner hands in. Each side reads and
// writes whole SDP PDUs; carrying them on an L2CAP channel is the host's.
// Everything either side reads came over the air from a stranger: every
// length is checked against the bytes that remain before a byte is read,
// and nested data elements are walked with an explicit stack, never by
// recursion.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_SDP_BUFFER        1024   // the most attribute bytes the client gathers
#define BT_SDP_ROUNDS        16     // the most responses it takes for one answer
#define BT_SDP_LIMIT_MS      5000   // the longest it waits for the whole answer
#define BT_SDP_DEPTH         6      // the most sequences and alternatives the walker opens at once
#define BT_SDP_CONTINUATION  16     // the longest continuation state, by the specification

#define BT_SDP_UUID_AUDIO_SOURCE 0x110A
#define BT_SDP_UUID_AUDIO_SINK   0x110B

// ── The client: one query, "what Audio Sink are you?" ─────────────────────

typedef enum { BT_SDP_IDLE, BT_SDP_ASKING, BT_SDP_DONE, BT_SDP_FAILED } bt_sdp_state_t;

typedef struct {
    bool found;                 // a record with the wanted class was returned
    bool complete;              // false: the answer was cut off by a bound (bytes, rounds, time)
    uint16_t avdtp_version;     // from ProtocolDescriptorList: the AVDTP (0x0019) entry's version, e.g. 0x0103; 0 if absent
    uint16_t a2dp_version;      // from BluetoothProfileDescriptorList: Advanced Audio (0x110D), e.g. 0x0103; 0 if absent
    uint16_t features;          // SupportedFeatures (0x0311); 0 if absent
    bool features_present;
    uint16_t l2cap_psm;         // the PSM in the L2CAP (0x0100) protocol entry, expected 0x0019; 0 if absent
} bt_sdp_sink_t;

typedef struct bt_sdp_client {
    bt_sdp_state_t state;
    uint64_t started, now;
    uint16_t max_bytes;         // the MaximumAttributeByteCount we ask for
    uint16_t tid;               // the transaction ID of our latest request
    bool owed;                  // a request is to be written
    bool awaiting;              // a request was written and is not yet answered
    uint8_t rounds;             // responses taken for this answer
    uint8_t cont_len;
    uint8_t cont[BT_SDP_CONTINUATION];
    uint16_t held;
    uint8_t buf[BT_SDP_BUFFER];
    bt_sdp_sink_t result;
    // Why the query ended, for the status text, and the error code an
    // ErrorResponse carried.
    const char *why;
    uint16_t error;
    uint32_t malformed;
} bt_sdp_client_t;

// Begin the query. `mtu` is the smaller of the channel's two MTUs: the
// request always fits (it is at most 36 bytes), and the answer we ask for
// is bounded so that each response fits. Under an MTU of 31 it still asks
// for the least the specification allows, 7 bytes a response.
void bt_sdp_client_start(bt_sdp_client_t *c, uint64_t now, uint16_t mtu);
// Is there a request to send now? Writes the SDP PDU (ServiceSearchAttributeRequest,
// PDU 0x06: pattern = UUID16 0x110B, max bytes, attribute range 0x0000-0xFFFF,
// and the continuation state the last response gave) and returns its length;
// 0 = nothing to send, or `capacity` too small for it.
size_t bt_sdp_client_request(bt_sdp_client_t *c, uint8_t *out, size_t capacity);
// A PDU from the peer on the SDP channel.
void bt_sdp_client_receive(bt_sdp_client_t *c, const uint8_t *pdu, size_t bytes);
void bt_sdp_client_tick(bt_sdp_client_t *c, uint64_t now);     // the 5 s limit
bt_sdp_state_t bt_sdp_client_state(const bt_sdp_client_t *c);
const bt_sdp_sink_t *bt_sdp_client_result(const bt_sdp_client_t *c);
// One line for the status file, e.g.
// "sdp: audio sink, avdtp 1.3, a2dp 1.3, features 0x0003 (complete)".
size_t bt_sdp_client_status(const bt_sdp_client_t *c, char *out, size_t capacity);

// The walker the client reads an answer with: AttributeLists, a sequence of
// one attribute-list sequence per record. The first record whose
// ServiceClassIDList holds `wanted_class` fills `out` (out->found).
// Complete: the bytes must be exactly one well-formed sequence. Not
// complete: the bytes are a prefix cut by a bound, and what can be read
// before the cut is kept. In either mode a malformed answer leaves `out`
// cleared, and out->complete is the caller's to set. A sequence or
// alternative past BT_SDP_DEPTH is stepped over by its size, not entered.
typedef enum { BT_SDP_PARSED, BT_SDP_TRUNCATED, BT_SDP_MALFORMED } bt_sdp_parse_t;
bt_sdp_parse_t bt_sdp_parse_attribute_lists(const uint8_t *lists, size_t bytes, uint16_t wanted_class,
                                            bool complete, bt_sdp_sink_t *out);

// ── The responder: os64's one record, an Audio Source ─────────────────────
// Stateless: a request in, a response out (its length; 0 = send nothing,
// for a request too short to carry a transaction ID or no room for the
// answer).
// Handles ServiceSearchRequest (0x02), ServiceAttributeRequest (0x04) and
// ServiceSearchAttributeRequest (0x06), each within the requester's stated
// maximum and `capacity`, with continuation (the continuation state it
// issues is an offset it can validate when it comes back); ErrorResponse
// (0x01) with the right error code for a malformed request, an unknown
// PDU, a bad handle, or a continuation state it did not issue.
#define BT_SDP_RECORD_HANDLE 0x00010001u
size_t bt_sdp_respond(const uint8_t *request, size_t bytes, uint8_t *out, size_t capacity);

#endif
