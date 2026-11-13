#ifndef OS64_BT_L2CAP_H
#define OS64_BT_L2CAP_H

// bt_l2cap.h — Classic L2CAP on one ACL link, Basic mode only
// (docs/design/pending/AUDIO_CLASSIC.md § S1; Bluetooth Core Vol 3 Part A).
//
// PURE: no kernel headers, no allocation, no waits, no logging, no clock.
// Time is the `now` (milliseconds) its owner hands in. One bt_l2cap_t per
// Classic ACL link, embedded by the host, which hands it every ACL data
// packet for the link and a turn to send when the controller has a buffer.
// Everything it reads came over the air from a stranger: every length is
// checked against the bytes that remain before a byte is read.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The MTU we state: a whole PDU (4-byte header + 1,017) fits one 1,021-byte
// ACL packet, so what we SEND is never fragmented. It is also the
// signalling MTU we take, since the reassembly buffer holds that much.
#define BT_L2CAP_MTU       1017
#define BT_L2CAP_CHANNELS  4
#define BT_L2CAP_PSM_SDP   0x0001
#define BT_L2CAP_PSM_AVDTP 0x0019

// Signalling waiting for a turn: requests we make and answers we owe, each
// one whole C-frame. Our commands are small; an Echo Response carries at
// most BT_L2CAP_ECHO_BYTES of the request's data, which keeps it within the
// 48-byte signalling MTU every peer must take.
#define BT_L2CAP_SIGNALS      8
#define BT_L2CAP_SIGNAL_BYTES 64
#define BT_L2CAP_ECHO_BYTES   44
#define BT_L2CAP_ADMITTED     4

typedef enum {
    BT_L2CAP_FREE,
    BT_L2CAP_CONNECTING,        // our Connection Request is out
    BT_L2CAP_CONFIG,            // connected; configuring, or open when both directions are done
    BT_L2CAP_DISCONNECTING      // our Disconnection Request is out
} bt_l2cap_state_t;

typedef struct {
    uint8_t state;              // bt_l2cap_state_t
    bool outgoing;              // we sent the Connection Request
    uint16_t psm, cid, dcid;    // the PSM, our CID, the peer's CID
    uint16_t our_mtu;           // what our Configuration Request states
    uint16_t peer_mtu;          // the peer's, once its configuration is complete
    uint16_t peer_mtu_pending;  // gathered across continuation requests
    bool ours_done, peer_done;  // each direction's configuration succeeded
    bool peer_partial;          // the peer's request so far carried the continuation flag
    // Configuration Requests each side has sent (the peer's continuation
    // parts each count). Each side has four: the peer's fifth closes the
    // channel, and so does a refusal of our fourth. The peer's count starts
    // over when its configuration succeeds.
    uint8_t our_rounds, peer_rounds;
    // Our one outstanding request on this channel: its identifier (0 =
    // none) and its timer, which runs from when the request is handed to
    // the controller.
    uint8_t ident;
    bool timer;
    uint64_t deadline;
    // The one data payload waiting for a turn, as a whole PDU.
    bool queued;
    uint16_t queued_bytes;
    uint32_t queued_seq;
    uint8_t out[BT_L2CAP_MTU + 4];
} bt_l2cap_channel_t;

typedef struct {
    uint8_t bytes;
    uint8_t channel;            // the channel a request belongs to; 0xff for an answer
    uint8_t ident;
    bool request;
    uint8_t pdu[BT_L2CAP_SIGNAL_BYTES];
} bt_l2cap_signal_t;

typedef struct bt_l2cap {
    bool up, encrypted;
    uint16_t admitted[BT_L2CAP_ADMITTED];   // 0 = an empty row (PSM 0 is never valid)
    uint64_t now;
    uint8_t next_ident;
    uint16_t next_cid;
    uint32_t next_seq;
    bt_l2cap_channel_t ch[BT_L2CAP_CHANNELS];
    bt_l2cap_signal_t sig[BT_L2CAP_SIGNALS];
    unsigned sig_head, sig_count;
    // Reassembly of one PDU: the bytes of it seen so far, its whole length
    // once the 4-byte header is in (0 before), and whether it is too long
    // for the buffer, in which case only its first 6 bytes are kept.
    uint8_t rx[BT_L2CAP_MTU + 4];
    size_t rx_have, rx_total;
    bool rx_active, rx_oversize;
    // Why the last channel we asked for was not opened: the Connection
    // Response's result, or 0xffff for no answer, 0xfffe for a Command
    // Reject, 0xfffd for a success naming a CID outside the dynamic range;
    // and its PSM.
    uint16_t refused, refused_psm;
    // Counted for the status file.
    uint32_t malformed;         // an ACL packet or signalling command that is not well formed
    uint32_t oversized;         // a PDU longer than the buffer
    uint32_t orphans;           // a continuation with no PDU begun
    uint32_t unfinished;        // a PDU abandoned by a new start
    uint32_t closed_cid;        // data on a CID that is not an open channel, or over the MTU we stated for it
    uint32_t fixed_cid;         // connectionless or reserved CIDs
    uint32_t strays;            // a response that answers nothing outstanding
    uint32_t queue_full;        // signalling dropped for want of a queue row
} bt_l2cap_t;

void bt_l2cap_init(bt_l2cap_t *l);                       // the link came up: everything cleared, admitted PSMs too
void bt_l2cap_reset(bt_l2cap_t *l);                      // the link went down: every channel closed, nothing queued
void bt_l2cap_set_encrypted(bt_l2cap_t *l, bool on);
// Which PSMs an incoming Connection Request may open, a list of
// BT_L2CAP_ADMITTED. A PSM that is not valid (Vol 3 Part A § 4.2), or one
// more than the list holds, is not added.
void bt_l2cap_admit(bt_l2cap_t *l, uint16_t psm, bool admit);

// One HCI ACL data packet for this link, WHOLE, header included (handle
// and flags u16, length u16, payload). The packet-boundary flag (bits 12-13
// of the first u16) says start (0b10 or 0b00) or continuation (0b01).
// Reassembles one L2CAP PDU at a time; handles signalling (CID 0x0001)
// itself; hands a complete PDU on an OPEN dynamic channel to `deliver`. A
// PDU longer than the buffer, a continuation with nothing begun, a start
// with one unfinished (the unfinished one goes): dropped and counted.
typedef void (*bt_l2cap_deliver_t)(void *context, unsigned channel, uint16_t psm,
                                   const uint8_t *data, size_t bytes);
void bt_l2cap_receive(bt_l2cap_t *l, const uint8_t *acl, size_t bytes, uint64_t now,
                      bt_l2cap_deliver_t deliver, void *context);

// Open a channel to the peer's PSM (we are the initiator). Returns the
// channel index, or -1 when none is free, the link is down or the PSM is
// not valid.
int bt_l2cap_connect(bt_l2cap_t *l, uint16_t psm, uint64_t now);
// A connected channel is sent a Disconnection Request and freed when it is
// answered or its timer runs out; one still connecting is freed at once.
void bt_l2cap_close(bt_l2cap_t *l, unsigned channel, uint64_t now);
bool bt_l2cap_is_open(const bt_l2cap_t *l, unsigned channel);          // both directions configured
bool bt_l2cap_is_free(const bt_l2cap_t *l, unsigned channel);
uint16_t bt_l2cap_peer_mtu(const bt_l2cap_t *l, unsigned channel);     // the largest payload the peer takes; 0 when not open
// Queue ONE payload on an open channel (copied; at most the peer's MTU and
// BT_L2CAP_MTU). False when the channel is not open, its queue slot is
// taken, or it is too long.
bool bt_l2cap_send(bt_l2cap_t *l, unsigned channel, const uint8_t *data, size_t bytes);

// One step: runs the timers, then, when `may_send`, hands AT MOST ONE whole
// L2CAP PDU (4-byte header + payload, never more than BT_L2CAP_MTU+4) to
// `send`: signalling before data, oldest first.
typedef void (*bt_l2cap_send_t)(void *context, const uint8_t *pdu, size_t bytes);
void bt_l2cap_tick(bt_l2cap_t *l, uint64_t now, bool may_send, bt_l2cap_send_t send, void *context);
bool bt_l2cap_wants_send(const bt_l2cap_t *l);
size_t bt_l2cap_status(const bt_l2cap_t *l, char *out, size_t capacity);

#endif
