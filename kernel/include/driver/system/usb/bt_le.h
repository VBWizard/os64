#ifndef OS64_BT_LE_H
#define OS64_BT_LE_H
#include "driver/system/usb/bt_scan.h"
#include "driver/system/hid_keyboard_map.h"

// One explicitly selected LE peer. Fixed storage and asynchronous callbacks
// keep USB polling usable from the scheduler's serialized input path.
#define BT_LE_QUEUE 8
#define BT_LE_ACL_MAX 1024
#define BT_LE_REPORTS 8

typedef enum {
    BT_LE_IDLE, BT_LE_RESET, BT_LE_MASK, BT_LE_HOST, BT_LE_EVENTS,
    BT_LE_ADDRESS, BT_LE_BUFFER, BT_LE_SHARED_BUFFER, BT_LE_CREATE, BT_LE_CONNECTING,
    BT_LE_PASSKEY, BT_LE_RANDOM_LOW, BT_LE_RANDOM_HIGH, BT_LE_PAIR,
    BT_LE_PAIR_WAIT, BT_LE_CONFIRM_LOW, BT_LE_CONFIRM_HIGH, BT_LE_CONFIRM_WAIT,
    BT_LE_RANDOM_WAIT, BT_LE_VERIFY_LOW, BT_LE_VERIFY_HIGH, BT_LE_STK,
    BT_LE_ENCRYPT, BT_LE_ENCRYPT_WAIT, BT_LE_SERVICES, BT_LE_CHARACTERISTICS,
    BT_LE_DESCRIPTORS, BT_LE_REPORT_MAP, BT_LE_REPORT_DESCRIPTORS, BT_LE_REPORT_REFERENCE,
    BT_LE_PROTOCOL, BT_LE_SUBSCRIBE, BT_LE_READY,
    BT_LE_CLEANUP, BT_LE_FAILED
} bt_le_phase_t;

typedef struct {
    uint16_t value, end, ccc, reference;
    uint8_t id, type;
} bt_le_report_char_t;

typedef struct {
    bt_le_phase_t phase;
    uint64_t now, deadline, total_deadline, command_deadline, acl_deadline;
    uint16_t opcode, handle, acl_size, outstanding;
    uint8_t credits, status, reply[16], reply_bytes;
    bool pending, command_done, connected, encrypted, stop_requested, release_pending, link_may_active;
    const char *error;
    uint16_t error_opcode;
    uint8_t error_status, address_type, peer[6], local[6];
    unsigned reports, malformed;
    uint32_t passkey;
    bool passkey_visible, just_works;
    uint8_t request[7], response[7], random[16], peer_random[16], peer_confirm[16];
    uint8_t tk[16], crypto[16];
    // USB fragmentation and HCI ACL fragmentation are separate boundaries.
    uint8_t wire[BT_LE_ACL_MAX+4], l2cap[68];
    size_t wire_used, wire_need, l2cap_used, l2cap_need;
    uint8_t queue[BT_LE_QUEUE][31], queue_bytes[BT_LE_QUEUE];
    unsigned queue_head, queue_count;
    uint16_t cursor, service_start, service_end, boot_value, boot_end, protocol, ccc;
    uint8_t boot_properties, protocol_properties, att_opcode;
    bool att_pending, report_protocol;
    uint16_t input_value, report_map_handle, report_map_bytes;
    uint8_t report_count, report_index;
    bt_le_report_char_t report_chars[BT_LE_REPORTS];
    uint8_t report_map[HID_KEYBOARD_MAP_BYTES];
    hid_keyboard_layout_t report_layout;
} bt_le_t;

typedef void (*bt_le_acl_send_t)(void *, const uint8_t *, size_t);
typedef void (*bt_le_report_t)(void *, const uint8_t [8]);
bool bt_le_request(bt_le_t *, const char *, size_t, uint64_t);
void bt_le_event(void *, const uint8_t *, size_t);
void bt_le_receive(bt_le_t *, const uint8_t *, size_t, bt_le_report_t, void *);
void bt_le_tick(bt_le_t *, uint64_t, bool, bool, bool,
                bt_scan_send_t, bt_le_acl_send_t, bt_le_report_t, void *);
size_t bt_le_status(const bt_le_t *, char *, size_t);
#endif
