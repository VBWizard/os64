#ifndef OS64_BT_INTEL_H
#define OS64_BT_INTEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_INTEL_READ_VERSION 0xfc05
#define BT_HCI_EVENT_BYTES 257

typedef struct {
    uint8_t address, packet_bytes, interval;
} bt_usb_endpoint_t;

typedef struct {
    uint8_t configuration;
    bt_usb_endpoint_t interrupt_in, bulk_in, bulk_out;
} bt_intel_usb_t;

// Select the full-speed AX210's HCI interface (interface 0, alternate 0).
// Reject malformed descriptor chains and ambiguous endpoint assignments.
bool bt_intel_find_usb(const uint8_t *cfg, size_t bytes, bt_intel_usb_t *out);

enum {
    BT_INTEL_IMAGE = 1u << 0,
    BT_INTEL_CNVI = 1u << 1,
    BT_INTEL_CNVR = 1u << 2,
    BT_INTEL_TIMESTAMP = 1u << 3,
    BT_INTEL_BUILD = 1u << 4,
    BT_INTEL_SHA1 = 1u << 5,
};

typedef enum {
    BT_INTEL_WAITING,
    BT_INTEL_VERSION,
    BT_INTEL_COMMAND_ERROR,
    BT_INTEL_BAD_REPLY,
} bt_intel_result_t;

typedef struct {
    bt_intel_result_t result;
    uint8_t status, image;
    uint32_t fields, cnvi, cnvr, build, sha1;
    uint16_t timestamp;
} bt_intel_reply_t;

// A USB packet boundary is not an HCI event boundary. Each receive endpoint
// owns a separate accumulator; completed events update the shared query reply.
typedef struct {
    uint8_t bytes[BT_HCI_EVENT_BYTES];
    uint16_t used;
} bt_hci_stream_t;

void bt_intel_feed(bt_hci_stream_t *stream, bt_intel_reply_t *reply,
                   const uint8_t *data, size_t bytes);

#endif
