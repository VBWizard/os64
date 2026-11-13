#ifndef OS64_BT_SCAN_H
#define OS64_BT_SCAN_H
#include "driver/system/usb/bt_intel.h"

#define BT_SCAN_DEVICES 64
#define BT_SCAN_NAME 64
#define BT_SCAN_TEXT_BYTES 12288

typedef struct {
    uint8_t address[6], address_type;
    bool le, complete_name;
    int8_t rssi;
    uint8_t device_class[3];
    char name[BT_SCAN_NAME];
} bt_scan_device_t;

typedef enum {
    BT_SCAN_IDLE, BT_SCAN_RESET, BT_SCAN_FEATURES, BT_SCAN_ADDRESS,
    BT_SCAN_MASK, BT_SCAN_INQUIRY_MODE, BT_SCAN_INQUIRY, BT_SCAN_CLASSIC_WAIT,
    BT_SCAN_LE_HOST, BT_SCAN_LE_MASK,
    // The controller's ACL buffers, asked once: a scan on a controller
    // already in service starts at LE_PARAMS, past them.
    BT_SCAN_BUFFER, BT_SCAN_LE_BUFFER,
    BT_SCAN_LE_PARAMS, BT_SCAN_LE_ENABLE,
    BT_SCAN_LE_WAIT, BT_SCAN_LE_DISABLE, BT_SCAN_DONE,
    BT_SCAN_CLEANUP, BT_SCAN_STOPPED, BT_SCAN_FAILED
} bt_scan_phase_t;

// Owned by whoever holds USB's serializing flag (xhci.c). No allocation,
// waits or logging in the decoder: it runs inside kusb's pass, against that
// pass's deadline.
typedef struct {
    bt_scan_phase_t phase;
    uint64_t deadline, total_deadline;
    uint16_t opcode, error_opcode;
    uint8_t credits, status, error_status, address[6], features[8];
    bool pending, command_done, bad_reply, hardware_error, inquiry_done, radio_active;
    bool le_only, shared_controller;
    // When the recorded error is a command timeout: had the USB transfer
    // that carries the command completed, and had the controller's reply
    // arrived? They fail differently (a command that never left, a reply
    // that never came or was not recognised), and the error line says
    // which.
    bool timed_out, timeout_usb_done, timeout_reply;
    uint8_t inquiry_status;
    // Read Buffer Size's ACL packet length and count, and LE Read Buffer
    // Size's, as the start-up round heard them (bt_acl.h). A later scan
    // clears them with the rest: whoever wants them takes them at DONE.
    bool buffers_known;
    uint16_t acl_size, acl_total, le_acl_size;
    uint8_t le_acl_total;
    const char *error;
    unsigned count, dropped, malformed_reports;
    bt_scan_device_t devices[BT_SCAN_DEVICES];
} bt_scan_t;

typedef void (*bt_scan_send_t)(void *context, uint16_t opcode,
                              const uint8_t *params, uint8_t length);
bool bt_scan_start(bt_scan_t *s, uint64_t now_ms);
bool bt_scan_start_le(bt_scan_t *s, uint64_t now_ms);
// STOPPED retains the error after confirmed scan cleanup and USB completion.
bool bt_scan_quiescent(const bt_scan_t *s);
bool bt_scan_request_valid(const char *data, size_t bytes);
void bt_scan_event(void *context, const uint8_t *event, size_t bytes);
void bt_scan_tick(bt_scan_t *s, uint64_t now_ms, bool usb_done, bool usb_failed,
                  bt_scan_send_t send, void *context);
size_t bt_scan_status(const bt_scan_t *s, char *out, size_t capacity);
// "" when the recorded error is not a timeout; else which half was missing,
// as a clause to follow the error. A string literal either way.
const char *bt_scan_timeout_text(const bt_scan_t *s);
size_t bt_scan_devices(const bt_scan_t *s, char *out, size_t capacity);
#endif
