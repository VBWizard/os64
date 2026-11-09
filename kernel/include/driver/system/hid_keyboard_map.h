#ifndef HID_KEYBOARD_MAP_H
#define HID_KEYBOARD_MAP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define HID_KEYBOARD_MAP_BYTES 1024
#define HID_KEYBOARD_INPUT_BYTES 64

typedef struct {
    // A zero entry is absent; otherwise this is the bit offset plus one.
    uint16_t usage_bits[256];
    uint16_t bytes, array_bit;
    uint8_t report_id, array_size, array_count, logical_min, logical_max, usage_min;
} hid_keyboard_layout_t;

// Parse the specified report ID's keyboard application collection. Supports
// absolute key bitmaps and one contiguous usage array, with report padding.
// Unsupported descriptors leave out untouched. No allocation or waiting.
bool hid_keyboard_parse_map(const uint8_t *, size_t, uint8_t, hid_keyboard_layout_t *);
// GATT Report values omit the Report ID byte; the Report Reference selects it.
// Convert to the shared eight-byte keyboard format, using HID rollover when
// more than six non-modifier keys are held. A false return leaves out untouched.
bool hid_keyboard_decode_map(const hid_keyboard_layout_t *, const uint8_t *, size_t, uint8_t out[8]);
#endif
