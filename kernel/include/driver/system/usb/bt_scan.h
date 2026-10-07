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
    BT_SCAN_LE_HOST, BT_SCAN_LE_MASK, BT_SCAN_LE_PARAMS, BT_SCAN_LE_ENABLE,
    BT_SCAN_LE_WAIT, BT_SCAN_LE_DISABLE, BT_SCAN_DONE,
    BT_SCAN_CLEANUP, BT_SCAN_FAILED
} bt_scan_phase_t;

// Owned by the serialized USB poll path. No allocation, waits or logging in
// the decoder: radio traffic can arrive while the scheduler lock is held.
typedef struct {
    bt_scan_phase_t phase;
    uint64_t deadline, total_deadline;
    uint16_t opcode, error_opcode;
    uint8_t credits, status, error_status, address[6], features[8];
    bool pending, command_done, bad_reply, hardware_error, inquiry_done, radio_active;
    uint8_t inquiry_status;
    const char *error;
    unsigned count, dropped, malformed_reports;
    bt_scan_device_t devices[BT_SCAN_DEVICES];
} bt_scan_t;

typedef void (*bt_scan_send_t)(void *context, uint16_t opcode,
                              const uint8_t *params, uint8_t length);
bool bt_scan_start(bt_scan_t *s, uint64_t now_ms);
bool bt_scan_request_valid(const char *data, size_t bytes);
void bt_scan_event(void *context, const uint8_t *event, size_t bytes);
void bt_scan_tick(bt_scan_t *s, uint64_t now_ms, bool usb_done, bool usb_failed,
                  bt_scan_send_t send, void *context);
size_t bt_scan_status(const bt_scan_t *s, char *out, size_t capacity);
size_t bt_scan_devices(const bt_scan_t *s, char *out, size_t capacity);
#endif
