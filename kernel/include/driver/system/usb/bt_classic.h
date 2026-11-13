#ifndef OS64_BT_CLASSIC_H
#define OS64_BT_CLASSIC_H

// bt_classic.h — one Classic Bluetooth (BR/EDR) link: paging or being
// called, Secure Simple Pairing as a device with no keys and no display,
// encryption, the bond it keeps, and page scan while it keeps one
// (docs/design/pending/AUDIO_CLASSIC.md §§ L3 to L5).
//
// PURE: no kernel headers, no allocation, no waits, no logging, no clock.
// Time is the `now` (milliseconds) its owner hands in. Owned by whoever
// holds USB's serializing flag (xhci.c), like the scan and the LE peers.
// The owner offers it every HCI event and gives it the command slot; it
// never answers from the event path: what the controller asks for is
// LATCHED and sent at the next turn it is given.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/system/usb/bt_scan.h"

typedef enum {
    BT_CLASSIC_IDLE, BT_CLASSIC_PAGING, BT_CLASSIC_ACCEPTING, BT_CLASSIC_LINKED,
    BT_CLASSIC_AUTHENTICATING, BT_CLASSIC_ENCRYPTING, BT_CLASSIC_CHECKING,
    BT_CLASSIC_READY, BT_CLASSIC_CLOSING, BT_CLASSIC_STOPPED, BT_CLASSIC_FAILED
} bt_classic_phase_t;

typedef struct {
    bool valid;
    uint8_t peer[6], local[6], key[16], key_type;
    char name[32];                       // what the inquiry heard, printable ASCII, NUL-terminated; may be empty
} bt_classic_bond_t;

// The bond's file: a fixed record of BT_CLASSIC_BOND_RECORD_BYTES.
//   0   magic "OS64BC01"
//   8   flags: 1 = valid, nothing else
//   9   key type
//   10  two zero bytes
//   12  peer address, 18 local address (least significant byte first)
//   24  link key
//   40  name: printable ASCII, NUL-terminated, zero after the NUL
//   72  four zero bytes
//   76  CRC-32 (IEEE 802.3, reflected, as bt_bond.c's) of bytes 0..75, little-endian
// A record that is not valid has every byte from 8 to 75 zero. decode
// refuses any other length, magic, check, flag or padding; a record whose
// `local` is not the adapter's is the CALLER's to refuse.
#define BT_CLASSIC_BOND_RECORD_BYTES 80
void bt_classic_bond_encode(uint8_t out[BT_CLASSIC_BOND_RECORD_BYTES], const bt_classic_bond_t *bond);
bool bt_classic_bond_decode(bt_classic_bond_t *bond, const uint8_t *record, size_t bytes);

// Why this session exists, which decides what pairing asks for and keeps.
typedef enum {
    BT_CLASSIC_SESSION_BOND,             // `bond`: pair, keep the key
    BT_CLASSIC_SESSION_CONNECT,          // `connect`: pair for this session, keep nothing
    BT_CLASSIC_SESSION_RECONNECT,        // `reconnect`: page the bonded address
    BT_CLASSIC_SESSION_INBOUND           // the bonded address called us
} bt_classic_session_t;

// Answers to the link's own peer, sent in this order (the first is what
// the controller waits on least patiently: about five seconds for a
// Connection Request, thirty for a pairing step).
enum {
    BT_CLASSIC_ANSWER_ACCEPT, BT_CLASSIC_ANSWER_LINK_KEY, BT_CLASSIC_ANSWER_IO_CAPABILITY,
    BT_CLASSIC_ANSWER_CONFIRM, BT_CLASSIC_ANSWER_PIN, BT_CLASSIC_ANSWERS
};
typedef struct {
    bool set;
    uint64_t since;
} bt_classic_answer_t;

// Refusals: a call the link will not take (a stranger's, reason 0x0f; the
// bonded peer's while the link is busy, reason 0x0d, unless it is the
// address being paged, whose call crosses ours and is accepted), pairing
// asks from anyone the link is not paging, accepting or linked to (while
// CLOSING, that is everyone), and a voice link from anyone.
// They never touch the link's state. A queue that is full drops the
// newest, and one that has waited 25 s is dropped; the controller's own
// timeout then refuses it.
#define BT_CLASSIC_REFUSALS 4
typedef struct {
    uint16_t opcode;
    uint8_t address[6], reason;
    uint64_t since;
} bt_classic_refusal_t;

typedef struct bt_classic {
    bt_classic_phase_t phase;
    bt_classic_session_t session;
    uint64_t now;                        // the last tick's or request's time; events have none of their own
    // The wait of the current phase (for a Connection Complete, an
    // Authentication Complete, an Encryption Change, a Disconnection
    // Complete), armed only while `waiting`.
    uint64_t deadline;
    bool waiting;
    // The command in flight: `pending` until its Command Status or
    // Complete has arrived AND its USB transfer is done (`control`).
    bool pending, command_done;
    uint16_t opcode;
    uint8_t status;                      // the reply's status
    uint64_t command_deadline;
    // A link command belongs to one attempt; its reply is ignored once a
    // later attempt has begun. Refusals and Write Scan Enable belong to none.
    bool command_link;
    uint32_t attempt, command_attempt;
    uint8_t command_address[6], command_value;
    // The link's own next command, sent when no answer or refusal is
    // waiting; 0 = none.
    uint16_t next_op;
    uint16_t handle;                     // meaningful while `connected`
    bool connected, encrypted;
    // A Connection Complete is still owed for `peer`: Create Connection or
    // Accept Connection Request was sent and nothing has ended it.
    bool link_may_active;
    // Two calls crossed: `peer` called us while we paged it, and its call
    // is accepted (latched or sent). The controller completes one of the
    // two and fails the other, so while this is set a failing Connection
    // Complete, or a refused page or accept, is one of the two ending and
    // not the attempt.
    bool crossing;
    // Disconnect has been sent for the live handle; Link Key Request Reply
    // gave the bond's key in this attempt; the peer that called us has
    // begun authenticating, so we wait for it rather than start our own.
    bool disconnect_sent, key_given, peer_authenticating;
    uint8_t close_reason;                // the Disconnect's reason
    uint16_t command_handle;
    uint8_t local[6], peer[6];
    char peer_name[32];
    bt_classic_answer_t answer[BT_CLASSIC_ANSWERS];
    bt_classic_refusal_t refusal[BT_CLASSIC_REFUSALS];
    unsigned refusal_count;
    uint32_t refusals_dropped;
    // The key a `bond` session's pairing produced, kept until READY.
    bool have_key;
    uint8_t new_key[16], new_key_type;
    // This session's pairing produced a debug key (type 0x03, Simple
    // Pairing debug mode, which anyone listening can compute): the link
    // used it and the bond did not keep it.
    bool debug_key;
    bt_classic_bond_t bond;
    bool bond_dirty, bond_loaded;
    // Write Scan Enable: wanted is 0x02 (page scan only) exactly while a
    // bond is held; set is what the controller last confirmed, tried what
    // was last sent. 0xff = unknown. status is the last write's, 0xff
    // when it went unanswered.
    uint8_t scan_enable_wanted, scan_enable_set, scan_enable_tried, scan_enable_status;
    // For the status text.
    bool role_known, mode_known, peer_io_known, ssp_done;
    uint8_t role, mode, key_size, refresh_status, peer_io[3];
    uint16_t mode_interval;
    // The first error, as bt_scan.c records it; `timed_out` and its two
    // halves say what a command timeout was missing.
    const char *error;
    uint16_t error_opcode;
    uint8_t error_status, disconnect_reason;
    bool retryable, timed_out, timeout_usb_done, timeout_reply;
    uint32_t malformed;
} bt_classic_t;

void bt_classic_init(bt_classic_t *c, const uint8_t local[6]);
// The host loaded a bond from disk (or found none: bond->valid false).
void bt_classic_set_bond(bt_classic_t *c, const bt_classic_bond_t *bond);
// A user command, already stripped of its "slot 2 " prefix:
//   "bond aa:bb:cc:dd:ee:ff"     page, pair, and keep the bond (bond_dirty goes true when there is one to save)
//   "connect aa:bb:cc:dd:ee:ff"  page and pair for this session; nothing is kept
//   "reconnect"                  page the bonded address; refused without a bond
//   "forget"                     drop the bond (bond_dirty true, bond.valid false), close any link
//   "disconnect"                 close the link, keep the bond
// `hint` is the scan's record of the address paged, NULL when it has none;
// one the scan heard as LE is refused. Returns false with *refusal (a
// string literal) set for anything refused.
bool bt_classic_request(bt_classic_t *c, const char *text, size_t bytes,
                        const bt_scan_device_t *hint, uint64_t now, const char **refusal);
// Every HCI event, whole. True when it was the Classic link's: Command
// Complete/Status for its pending opcode, the BR/EDR-only events it reads
// (whoever they are for: a stranger's are refused here), and Disconnection
// Complete, Encryption Change and Encryption Key Refresh Complete for its
// live handle. Number Of Completed Packets is never its.
bool bt_classic_event(bt_classic_t *c, const uint8_t *event, size_t bytes);
// One step. `control`: the command slot is this link's and the previous
// command's USB transfer is done; only then is `send` called, at most once,
// and never in the tick that gives up waiting for a reply (the controller
// keeps that command's credit until it answers).
// `failed`: the transport is gone; FAILED, and nothing is sent again.
void bt_classic_tick(bt_classic_t *c, uint64_t now, bool control, bool failed,
                     bt_scan_send_t send, void *context);
// An answer is latched that the controller is waiting for.
bool bt_classic_urgent(const bt_classic_t *c);
// IDLE or STOPPED, nothing in flight, latched or left to send.
bool bt_classic_quiescent(const bt_classic_t *c);
size_t bt_classic_status(const bt_classic_t *c, char *out, size_t capacity);
#endif
