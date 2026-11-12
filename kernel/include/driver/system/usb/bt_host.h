#ifndef OS64_BT_HOST_H
#define OS64_BT_HOST_H
#include "driver/system/usb/bt_manager.h"

#define BT_HOST_PEERS BT_BOND_SLOTS
// Controller commands and ACL credits are shared; pairing, encryption, ATT,
// bond policy and input state belong to a peer. USB's serializing flag
// (xhci.c) orders access.
typedef struct {
    bt_le_t le;
    bt_manager_t manager;
} bt_host_peer_t;
typedef struct {
    bt_scan_t scan;
    bt_host_peer_t peers[BT_HOST_PEERS];
    bool initialized, failed, command_done, tx_used;
    uint8_t owner, credits, next_peer, poll_peer;
    uint8_t duplicate_slot; // Last refused peer's owning slot plus one; zero means none.
    uint64_t init_retry_at;
    // Start-up rounds that ended FAILED (the controller's reset and the
    // reset that cleans up after it both unanswered). The round is run
    // again, after the same pause a cleanly stopped one gets, until
    // BT_HOST_INIT_ROUNDS have failed: one bad start is not the boot.
    uint8_t init_failed_rounds;
    uint8_t wire[BT_LE_ACL_MAX+4];
    size_t wire_used, wire_need;
    // ACL data being discarded until its transfer ends, and how often the
    // assembler was found out of step and emptied (bt_host_receive).
    bool wire_skip;
    uint32_t wire_resyncs;
} bt_host_t;
#define BT_HOST_INIT_ROUNDS 3
typedef void (*bt_host_report_t)(void *, unsigned, const bt_le_input_t *);
void bt_host_init(bt_host_t *, uint64_t);
void bt_host_event(bt_host_t *, const uint8_t *, size_t);
// `max_packet`: the bulk endpoint's maximum packet size when the bytes are
// ONE USB packet, 0 for an arbitrary slice of the stream. As bt_hci_feed:
// a packet shorter than the maximum ends a transfer and an ACL packet does
// not continue past the end of one, so the packet form can tell when its
// assembler is out of step, and empties it. A header whose length cannot
// be real is then a transfer to skip, where the stream form can only call
// the host failed.
void bt_host_receive(bt_host_t *, const uint8_t *, size_t, size_t max_packet,
                     bt_host_report_t, void *);
void bt_host_tick(bt_host_t *, uint64_t, bool, bool, bool,
                  bt_scan_send_t, bt_le_acl_send_t, bt_host_report_t, void *);
void bt_host_policy(bt_host_t *, uint64_t);
bool bt_host_scan(bt_host_t *, uint64_t);
bool bt_host_command(bt_host_t *, const char *, size_t, uint64_t);
size_t bt_host_status(const bt_host_t *, char *, size_t);
#endif
