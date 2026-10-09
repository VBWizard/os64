#ifndef OS64_BT_HOST_H
#define OS64_BT_HOST_H
#include "driver/system/usb/bt_manager.h"

#define BT_HOST_PEERS BT_BOND_SLOTS
// Controller commands and ACL credits are shared; pairing, encryption, ATT,
// bond policy and input state belong to a peer. USB polling serializes access.
typedef struct {
    bt_le_t le;
    bt_manager_t manager;
} bt_host_peer_t;
typedef struct {
    bt_scan_t scan;
    bt_host_peer_t peers[BT_HOST_PEERS];
    bool initialized, failed, command_done, tx_used;
    uint8_t owner, credits, next_peer, poll_peer;
    uint64_t init_retry_at;
    uint8_t wire[BT_LE_ACL_MAX+4];
    size_t wire_used, wire_need;
} bt_host_t;
typedef void (*bt_host_report_t)(void *, unsigned, const bt_le_input_t *);
void bt_host_init(bt_host_t *, uint64_t);
void bt_host_event(bt_host_t *, const uint8_t *, size_t);
void bt_host_receive(bt_host_t *, const uint8_t *, size_t, bt_host_report_t, void *);
void bt_host_tick(bt_host_t *, uint64_t, bool, bool, bool,
                  bt_scan_send_t, bt_le_acl_send_t, bt_host_report_t, void *);
void bt_host_policy(bt_host_t *, uint64_t);
bool bt_host_scan(bt_host_t *, uint64_t);
bool bt_host_command(bt_host_t *, const char *, size_t, uint64_t);
size_t bt_host_status(const bt_host_t *, char *, size_t);
#endif
