#ifndef OS64_BT_AVRCP_H
#define OS64_BT_AVRCP_H

// bt_avrcp.h — AVCTP and AVRCP on one Classic link, both roles
// (docs/design/pending/AUDIO_REMOTE.md § Part R; AVCTP 1.3, AVRCP 1.4 and
// the AV/C frame it carries). For the headphones' buttons we are the
// TARGET: their pass-through presses are answered and handed to the host,
// and we answer what a player of AVRCP 1.4 must (capabilities, the playback
// status and its notification, "no track", no metadata). For their volume
// we are the CONTROLLER: we ask whether they announce it, register to hear
// it change, and set it.
//
// PURE: no allocation, no waits, no logging, no clock. Time is the `now`
// (milliseconds) its owner hands in. The one header it takes beyond this
// one is strings/sprintf.h, for the status text. It reads and writes whole
// AVCTP messages, one L2CAP payload each (AVCTP's three-byte header is
// built and read here, and their fragmented messages reassembled);
// carrying them on the AVCTP channel and opening and closing it are the
// host's. Everything it reads came over the air from a stranger: every
// length is checked against the bytes that remain before a byte is read.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_AVRCP_PSM            0x0017  // AVCTP's control channel (bt_l2cap.h BT_L2CAP_PSM_AVCTP)
#define BT_AVRCP_PROFILE        0x110E  // the AVCTP profile identifier: AV Remote Control
#define BT_AVRCP_MESSAGE        512     // the longest AV/C frame reassembled from fragments
// The longest message we build, AVCTP's header included: an answer that
// echoes their frame (6 bytes of headers and at most BT_AVRCP_ECHO of
// their operands). Every message we build is built in a buffer this long,
// and it is under the least MTU an AVCTP channel may have, so nothing we
// send is ever fragmented.
#define BT_AVRCP_LONGEST        32
#define BT_AVRCP_ECHO           (BT_AVRCP_LONGEST - 6)
#define BT_AVRCP_LEAST_MTU      48
// Answers owed at once. Their commands may fill all but one while they
// hold a playback-status registration: that one is kept for its CHANGED,
// so a notification we owe always has room. A command past the bound is
// dropped unanswered and counted.
#define BT_AVRCP_ANSWERS        8
#define BT_AVRCP_PRESSES        16      // the ring of presses and releases not yet read by the host
#define BT_AVRCP_REPLY_MS       2000    // a command of ours waits this long after it is sent, twice (bt_avrcp_command_t)
#define BT_AVRCP_HOLD_MS        2000    // a press with no release is released by us this long after its last repeat
#define BT_AVRCP_RESTARTS       3       // FAILED begins again at playing at most this often a channel (bt_avrcp_playback)

// The pass-through operations taken (AV/C Panel Subunit; the low seven
// bits of the operation byte, the top bit being "released").
#define BT_AVRCP_OP_VOLUME_UP   0x41
#define BT_AVRCP_OP_VOLUME_DOWN 0x42
#define BT_AVRCP_OP_PLAY        0x44
#define BT_AVRCP_OP_STOP        0x45
#define BT_AVRCP_OP_PAUSE       0x46
#define BT_AVRCP_OP_FORWARD     0x4b
#define BT_AVRCP_OP_BACKWARD    0x4c

// The playback status, as AVRCP numbers it.
#define BT_AVRCP_STOPPED        0x00
#define BT_AVRCP_PLAYING        0x01
#define BT_AVRCP_PAUSED         0x02

// The controller's side: their absolute volume.
typedef enum {
    BT_AVRCP_IDLE,            // no AVCTP channel
    BT_AVRCP_ASKING,          // our Get Capabilities (events) is queued or out
    BT_AVRCP_REGISTERING,     // our Register Notification (volume changed) is queued or out
    BT_AVRCP_READY,           // their volume is known and a registration stands (or is being made again)
    BT_AVRCP_PLAIN,           // they have no absolute volume: not listed, rejected or not implemented
    BT_AVRCP_FAILED,          // our Get Capabilities or Register Notification went unanswered through both periods
                              // (a silent Set ends only itself); playing begins again: bt_avrcp_playback
} bt_avrcp_state_t;

// One message waiting for its turn on the channel. `id` names this message
// and no other (a new one, or one rebuilt or sent again, gets a new id): it
// is the serial bt_avrcp_next gives for it.
typedef struct {
    uint8_t bytes;            // 0: the slot is empty
    uint8_t pdu[BT_AVRCP_LONGEST];
    uint32_t id;
} bt_avrcp_msg_t;

// A command of ours. It is QUEUED until the carrier says it is sent
// (bt_avrcp_sent), and while queued may be replaced or retired; once sent
// it waits BT_AVRCP_REPLY_MS for its answer, then a second period. For
// the second period a question or registration is queued again with the
// same label and bytes, so an answer to either transmission is its
// answer, even one to the first that arrives while the second copy still
// waits to be sent (the copy is then withdrawn and never sent); silent
// through both, the controller has FAILED. A Set is not sent twice, and
// silent through both it ends only itself (bt_avrcp.c, ar_timer, says
// why). A registration answered INTERIM STANDS: it keeps its label, and
// no timer, until its CHANGED.
typedef struct {
    bool live, sent;
    bool waiting;             // sent, and its timer is running (a registration that stands is not)
    uint8_t pdu_id;           // the AVRCP PDU it carries
    uint8_t label;
    uint8_t tries;            // silences so far: the second ends it
    uint64_t deadline;
    bt_avrcp_msg_t msg;
} bt_avrcp_command_t;

// A press or release of theirs, for the host.
typedef struct { uint8_t operation; bool pressed; } bt_avrcp_press_t;

typedef struct bt_avrcp {
    bt_avrcp_state_t state;
    uint64_t now;
    uint32_t next_id;         // the last message id given
    uint8_t next_label;       // where the search for a free label of ours starts
    // The answers we owe, oldest first.
    bt_avrcp_msg_t answer[BT_AVRCP_ANSWERS];
    uint8_t answers;
    // Our two commands: Get Capabilities or Register Notification (one at a
    // time, as the state table walks), and Set Absolute Volume, with the
    // newest level asked while one is out.
    bt_avrcp_command_t reg, set;
    bool set_pending;
    uint8_t set_wanted;
    // Their volume, 0..127, and how many times they told us a level. It is
    // known exactly in READY, which only a level they told us enters.
    uint8_t volume;
    uint32_t volume_serial;
    // The target's side. Their playback-status registration stands with its
    // label (and the subunit it named) until our CHANGED spends it.
    uint8_t playback;
    bool play_registered;
    uint8_t play_label, play_subunit;
    bool track_registered;    // answered INTERIM, never CHANGED: we have no tracks
    // The press held (0 when none): its operation, when it lapses, and
    // whether its press reached the ring (only then does its release).
    uint8_t held;
    uint64_t held_until;
    bool held_reported;
    bt_avrcp_press_t ring[BT_AVRCP_PRESSES];
    uint8_t ring_head, ring_count;
    // Reassembly of one fragmented message: the start's header byte and
    // profile, the packets still to come, the frame so far.
    bool rx_active;
    uint8_t rx_header, rx_left;
    uint16_t rx_profile;
    size_t rx_have;
    uint8_t rx[BT_AVRCP_MESSAGE];
    // Counted for the status file.
    uint32_t presses[7];      // presses taken, by operation (volume up, down, play, stop, pause, forward, backward)
    uint32_t auto_released, presses_lost, unmatched_releases, not_taken;
    uint32_t sets, sets_accepted, sets_rejected, sets_interim, registrations, silences;
    uint32_t sets_silent;     // a Set unanswered through both periods, ended alone
    uint8_t restarts;         // FAILED begun again by playing, on this channel
    uint32_t answered, rejected, not_implemented;
    // A response of theirs that answers nothing of ours: no live command
    // sent under its label, or that label with another PDU or a
    // non-vendor opcode, or a response under another profile without IPID.
    uint32_t strays;
    // Their messages we cannot read: an empty payload, a single packet
    // without its profile, a command without an AV/C header or carrying
    // IPID, and their response to a command of ours that is not well formed.
    uint32_t malformed;
    uint32_t fragments_dropped, too_long;
    uint32_t unsendable;      // a message longer than the channel's MTU, dropped
    uint32_t stale_sends;     // bt_avrcp_sent for a serial no message has any more
    uint32_t overrun;         // a command of theirs with no room for its answer, dropped
    uint32_t ignored;         // a message received while IDLE, dropped unread and unanswered
    uint32_t invalid_profile; // their messages for another profile, and their answers saying ours was
} bt_avrcp_t;

void bt_avrcp_init(bt_avrcp_t *a);                   // the link came up: IDLE, everything cleared
// The link dropped: IDLE and everything cleared, except the presses the
// host has not read, and a release for a press it has, so every press the
// host sees is followed by its release.
void bt_avrcp_reset(bt_avrcp_t *a);
// The AVCTP channel is open: we begin asking. The host calls it before it
// hands over the channel's first payload: until then the state is IDLE,
// and a message received in IDLE is dropped unread and unanswered
// (counted as `ignored`), their commands included.
void bt_avrcp_channel_open(bt_avrcp_t *a, uint64_t now);
// It closed, the link staying up or not: IDLE. Everything the channel
// carried goes (answers owed, our commands, their registrations, their
// volume, the restarts counted, reassembly); a press held is released.
void bt_avrcp_channel_closed(bt_avrcp_t *a, uint64_t now);
// One whole L2CAP payload from the AVCTP channel: a single packet or one
// fragment of a message. In IDLE (no bt_avrcp_channel_open yet, or since
// the close) it is dropped and counted.
void bt_avrcp_receive(bt_avrcp_t *a, const uint8_t *pdu, size_t bytes, uint64_t now);
void bt_avrcp_tick(bt_avrcp_t *a, uint64_t now);     // the timers: our commands', a press held
// The message to send NOW: the oldest answer owed first, then our commands
// not yet sent (the registration's, then the Set). It is copied to `out`
// and its serial to *serial; nothing is consumed, so asking again before
// bt_avrcp_sent gives the same message and serial, unless something was
// queued ahead of it meanwhile (an answer owed goes ahead of our commands).
// Returns its length; 0 (and serial 0) when there is nothing, or
// `capacity` is too small (the message waits). A message longer than
// `mtu` is dropped here and counted: a command so dropped is treated as
// sent and left to its timer.
size_t bt_avrcp_next(bt_avrcp_t *a, uint8_t *out, size_t capacity, uint16_t mtu, uint64_t now, uint32_t *serial);
// The message with this serial was handed to the controller. The serial
// is looked up among everything next can have handed out (the answers
// owed, our commands not yet sent), not only the head, because a message
// the carrier parked may have been overtaken since: an answer leaves the
// owed list, the rest keeping their order; a command is sent and its
// timer starts. False, and nothing changes, when no message has this
// serial any more (it was retired, replaced or already committed): the
// carrier should not have sent it.
bool bt_avrcp_sent(bt_avrcp_t *a, uint32_t serial, uint64_t now);

// The controller's side: their volume.
bt_avrcp_state_t bt_avrcp_state(const bt_avrcp_t *a);
bool bt_avrcp_volume(const bt_avrcp_t *a, uint8_t *raw);     // their level 0..127, in READY
// Goes up each time THEY told us a level (an INTERIM, a CHANGED, a Set's
// ACCEPTED), so a host that polls sees each, even one equal to the last.
uint32_t bt_avrcp_volume_serial(const bt_avrcp_t *a);
// READY only: set their level (the top bit is cleared). One Set is out at
// a time; a newer level replaces a Set not yet sent, and while one is out
// it is held and sent when that one is answered, the latest winning.
bool bt_avrcp_set_volume(bt_avrcp_t *a, uint8_t raw, uint64_t now);

// The target's side: their buttons, and what we tell them.
bool bt_avrcp_press(bt_avrcp_t *a, bt_avrcp_press_t *out);   // the oldest press or release not yet read; false when none
// The playback status as the host finds it (BT_AVRCP_STOPPED, PLAYING or
// PAUSED; anything else is ignored), each tick. A change is announced to a
// standing registration with CHANGED, which spends it. A change TO playing
// while the controller is FAILED begins it again at ASKING, at most
// BT_AVRCP_RESTARTS times until the channel closes: headphones may hold
// back their volume registration's answer until something plays.
void bt_avrcp_playback(bt_avrcp_t *a, uint8_t status, uint64_t now);

const char *bt_avrcp_state_name(bt_avrcp_state_t s);
size_t bt_avrcp_status(const bt_avrcp_t *a, char *out, size_t capacity);

#endif
