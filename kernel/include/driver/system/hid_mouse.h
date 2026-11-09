#ifndef HID_MOUSE_H
#define HID_MOUSE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_MOUSE_REPORT_BYTES 64
#define HID_MOUSE_DESCRIPTOR_BYTES 1024

typedef struct {
    uint16_t bit;
    uint8_t size;
} hid_mouse_field_t;

typedef struct {
    uint8_t report_id;                 // zero means no ID prefix
    uint16_t bytes;                    // includes the ID prefix, if present
    uint16_t max_bytes;                // largest input report on this interface
    hid_mouse_field_t buttons[3], x, y, wheel;
} hid_mouse_layout_t;

typedef struct {
    uint8_t buttons;
    int16_t x, y, wheel;               // HID wheel: positive away from user
} hid_mouse_sample_t;

// Accept one complete relative mouse report with a wheel. Unsupported or
// ambiguous descriptors fail without modifying out; the caller can use boot
// protocol. Other report IDs are ignored by the decoder, not read as motion.
bool hid_mouse_parse(const uint8_t *desc, size_t length, hid_mouse_layout_t *out);
bool hid_mouse_decode(const hid_mouse_layout_t *layout, const uint8_t *report,
                      size_t length, hid_mouse_sample_t *out);
#endif
