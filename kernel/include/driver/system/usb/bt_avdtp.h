#ifndef OS64_BT_AVDTP_H
#define OS64_BT_AVDTP_H

// bt_avdtp.h — AVDTP signalling on one Classic link, both roles
// (docs/design/pending/AUDIO_A2DP.md § Part V; the Audio/Video Distribution
// Transport Protocol 1.3 and A2DP 1.3). We are one audio SOURCE endpoint
// that sends SBC: as initiator we find the headphones' sink, configure it,
// open, start, suspend and close the stream; as acceptor we answer when
// they do the same to us, and both may happen at once.
//
// PURE: no kernel headers, no allocation, no waits, no logging, no clock.
// Time is the `now` (milliseconds) its owner hands in. It reads and writes
// whole signalling messages; carrying them on the L2CAP signalling channel
// (one payload slot a channel) and opening and closing the channels are the
// host's. Everything it reads came over the air from a stranger: every
// length is checked against the bytes that remain before a byte is read.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_AVDTP_SEID          1       // our one endpoint's identifier
#define BT_AVDTP_BITPOOL       51      // the bitpool we choose: high quality at 48 kHz joint stereo
#define BT_AVDTP_MESSAGE       256     // the longest message reassembled from fragments
#define BT_AVDTP_FRAGMENTS     4       // the most packets one message may arrive in
#define BT_AVDTP_ENDPOINTS     8       // the most endpoints read from a Discover answer
// The longest message we send: our Set Configuration with delay reporting
// (2 header + 2 SEIDs + 2 transport + 8 codec + 2 delay). Every message we
// build is built in a buffer this long, and it is under the least MTU a
// signalling channel may have, so nothing we send is ever fragmented.
#define BT_AVDTP_LONGEST       16
#define BT_AVDTP_LEAST_MTU     48

// Timers (BlueZ's figures for the commands).
#define BT_AVDTP_REPLY_MS      6000    // a command we sent waits this long for its answer
#define BT_AVDTP_SUSPEND_MS    10000   // a Suspend waits this long
#define BT_AVDTP_ABORT_MS      2000    // an Abort waits this long, and nothing waits on it after
#define BT_AVDTP_HOLD_MS       10000   // CONFIGURED for their Open; AWAITING_MEDIA for the channel
#define BT_AVDTP_RETRY_MS      1000    // our Set Configuration is sent again after this, once

typedef enum {
    BT_AVDTP_IDLE,            // no signalling channel
    BT_AVDTP_CONNECTED,       // sig up, no stream, nothing of ours outstanding
    BT_AVDTP_DISCOVERING,     // our Discover is out
    BT_AVDTP_ASKING,          // our Get (All) Capabilities is out
    BT_AVDTP_CONFIGURING,     // our Set Configuration is out, or waits to be sent again
    BT_AVDTP_CONFIGURED,      // we accepted THEIR Set Configuration; they are to open
    BT_AVDTP_OPENING,         // our Open is out
    BT_AVDTP_AWAITING_MEDIA,  // Open is agreed; the media channel is not up yet
    BT_AVDTP_OPEN,            // configured, both channels up, not streaming
    BT_AVDTP_STARTING,        // our Start is out
    BT_AVDTP_STREAMING,       // media may be sent
    BT_AVDTP_SUSPENDING,      // our Suspend is out
    BT_AVDTP_CLOSING,         // our Close is out
    BT_AVDTP_ENDING,          // a session went wrong: the host is closing both channels
    BT_AVDTP_FAILED,          // no session can be made on this connection; `why` says why
} bt_avdtp_state_t;

// The SBC settings agreed: the fields of sbc_settings_t (audio/sbc_frame.h),
// which the host converts to, field by field. The bitpool is the one the
// encoder is to use, the top of the range the configuration names.
typedef struct {
    uint32_t rate;            // 48000
    uint8_t channels;         // 2
    bool joint;               // joint stereo; false is plain stereo
    uint8_t blocks;           // 16
    uint8_t subbands;         // 8
    bool snr;                 // false: loudness
    uint8_t bitpool;
} bt_avdtp_sbc_t;

// Edges the pump must see even if it only polls: each only goes up, from
// the link's coming up (bt_avdtp_init).
typedef struct {
    uint32_t their_starts;     // their Start, accepted
    uint32_t their_suspends;   // their Suspend, accepted
    uint32_t start_rejects;    // our Start, rejected
    uint32_t suspend_rejects;  // our Suspend, rejected
} bt_avdtp_edges_t;

// One message waiting for its turn on the channel.
typedef struct {
    uint8_t bytes;            // 0: the slot is empty
    uint8_t pdu[BT_AVDTP_LONGEST];
} bt_avdtp_msg_t;

// An endpoint from their Discover answer.
typedef struct {
    uint8_t seid;
    bool in_use;
    uint8_t media;            // media type: 0 audio
    bool sink;                // endpoint type: true sink, false source
} bt_avdtp_endpoint_t;

typedef struct bt_avdtp {
    bt_avdtp_state_t state;
    uint64_t now;
    uint16_t peer_version;    // AVDTP version from their SDP record; 0 when not read
    // The answers we owe, oldest first: the one their command earned, and
    // the reject (or Abort's accept) of one more that arrived before it
    // could leave. A third is dropped and counted.
    bt_avdtp_msg_t answer[2];
    // Our one command: built when queued, then handed out (`sent`), then
    // answered or timed out, its timer running from when it was queued.
    // Labels run 0 to 15 and round again, one a command. A response is
    // taken only with this label and this signal, and only once the command
    // has been handed out; anything else is a stray.
    bt_avdtp_msg_t command;
    bool command_live;        // a command is queued or outstanding
    bool command_sent;
    uint8_t command_signal, command_label;
    uint64_t command_deadline;
    uint8_t next_label;
    // State timers: CONFIGURED and AWAITING_MEDIA wait BT_AVDTP_HOLD_MS;
    // CONFIGURING waits BT_AVDTP_RETRY_MS to send its command again.
    uint64_t state_deadline;
    bool retry_waiting;       // CONFIGURING: our Set Configuration waits to be sent again
    bool retried;             // ...and has been, once
    bool rejected_theirs;     // CONFIGURING: we rejected their Set Configuration
    // What their Discover answer offered, and the walk through it.
    bt_avdtp_endpoint_t endpoints[BT_AVDTP_ENDPOINTS];
    uint8_t endpoint_count, endpoint_at;   // endpoint_at: the one being asked or configured
    bool no_get_all;          // they rejected Get All Capabilities: Get Capabilities from now on
    // The stream: their endpoint, the settings, the bitpool range the
    // configuration names, and whether it includes delay reporting.
    uint8_t remote_seid;
    bool configured;
    bt_avdtp_sbc_t settings;
    uint8_t range_lo, range_hi;
    bool delay_reporting;
    bool media_ours;          // AWAITING_MEDIA: we open the media channel (we sent Open)
    bool close_wanted;        // the host is to close both channels
    uint8_t wrongs;           // sessions gone wrong on this connection
    bool delay_known;
    uint16_t delay;           // their latest Delay Report, in 100 us units
    const char *why;          // why the session is FAILED, or why it last went wrong
    // Reassembly of one fragmented message: the start's header byte, the
    // packets still to come, and the signal and payload so far, which with
    // the header byte make at most BT_AVDTP_MESSAGE.
    bool rx_active;
    uint8_t rx_header, rx_left;
    size_t rx_have;
    uint8_t rx[BT_AVDTP_MESSAGE - 1];
    bt_avdtp_edges_t edges;
    // Counted for the status file.
    uint32_t strays;          // a response that answers nothing outstanding
    uint32_t malformed;       // a message, capability list or reject not well formed
    uint32_t fragments_dropped;
    uint32_t unsendable;      // a message longer than the channel's MTU, dropped
    uint32_t overrun;         // a third command of theirs with two answers owed, dropped
    uint32_t ignored;         // a message in a state that takes none (IDLE, ENDING)
    uint32_t media_refused;
    uint8_t last_reject_signal, last_reject_code;
} bt_avdtp_t;

void bt_avdtp_init(bt_avdtp_t *a);                       // the link came up: IDLE, everything cleared
void bt_avdtp_reset(bt_avdtp_t *a);                      // the link dropped: likewise, silently

// What the host tells it. `peer_version` is the AVDTP version from their
// SDP record (0x0103), or 0 when the record was not read: Get All
// Capabilities is asked only of 0x0103 or later.
void bt_avdtp_signalling_open(bt_avdtp_t *a, uint64_t now, uint16_t peer_version);
// Sig closed with the link up: everything cleared, IDLE, and a session gone
// wrong (the second on this connection is FAILED). In ENDING it is what was
// asked for, and nothing more is counted.
void bt_avdtp_signalling_closed(bt_avdtp_t *a, uint64_t now);
void bt_avdtp_media_open(bt_avdtp_t *a, uint64_t now);
// `refused`: our attempt to open it was refused.
void bt_avdtp_media_closed(bt_avdtp_t *a, uint64_t now, bool refused);
// One whole L2CAP payload from the signalling channel: a single packet or
// one fragment of a message.
void bt_avdtp_receive(bt_avdtp_t *a, const uint8_t *pdu, size_t bytes, uint64_t now);
void bt_avdtp_tick(bt_avdtp_t *a, uint64_t now);         // the timers
// The next message to send, when the channel's payload slot is free: the
// oldest answer owed first, then our command. Returns its length; 0 when
// there is nothing, or `capacity` is too small (the message waits). A
// message longer than `mtu` (the peer's MTU for the channel) is dropped and
// counted: a command so dropped is left to its timer.
size_t bt_avdtp_next(bt_avdtp_t *a, uint8_t *out, size_t capacity, uint16_t mtu, uint64_t now);

// What the pump asks for. False: not in a state that allows it, or a
// command of ours (an Abort) is still outstanding.
bool bt_avdtp_begin(bt_avdtp_t *a, uint64_t now);        // CONNECTED: start discovering
bool bt_avdtp_start(bt_avdtp_t *a, uint64_t now);        // OPEN
bool bt_avdtp_suspend(bt_avdtp_t *a, uint64_t now);      // STREAMING
bool bt_avdtp_close(bt_avdtp_t *a, uint64_t now);        // OPEN or STREAMING

// What the host reads.
bt_avdtp_state_t bt_avdtp_state(const bt_avdtp_t *a);
bool bt_avdtp_wants_media(const bt_avdtp_t *a);          // AWAITING_MEDIA, and WE are to open the channel
// A media channel belongs to the session from AWAITING_MEDIA through
// CLOSING; the host closes one it holds when this is false.
bool bt_avdtp_media_wanted(const bt_avdtp_t *a);
// A session gone wrong: the host is to close both channels. True once the
// Abort that goes first, if any, has been handed out.
bool bt_avdtp_wants_closed(const bt_avdtp_t *a);
const bt_avdtp_sbc_t *bt_avdtp_settings(const bt_avdtp_t *a);   // what was agreed; NULL before CONFIGURED/OPENING
bool bt_avdtp_delay(const bt_avdtp_t *a, uint32_t *delay_100us); // their latest Delay Report
const bt_avdtp_edges_t *bt_avdtp_edges(const bt_avdtp_t *a);
const char *bt_avdtp_state_name(bt_avdtp_state_t s);
size_t bt_avdtp_status(const bt_avdtp_t *a, char *out, size_t capacity);

#endif
