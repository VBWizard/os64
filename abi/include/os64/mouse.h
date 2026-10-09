#ifndef OS64_MOUSE_H
#define OS64_MOUSE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define OS64_MOUSE_PATH "/sys/mouse"
#define OS64_MOUSE_VERSION 2u
#define OS64_MOUSE_DEVICES 16u
#define OS64_MOUSE_KEY 64u
#define OS64_MOUSE_NAME 64u
#define OS64_MOUSE_MIN_SPEED 25u
#define OS64_MOUSE_MAX_SPEED 400u
#define OS64_MOUSE_APPLY 0u
#define OS64_MOUSE_FORGET 1u

// Fixed-size binary snapshots and atomic compare-and-apply writes. Settings
// generation changes on Apply/Forget, not device arrival. Keys identify a bond
// (Bluetooth), controller/port/product (USB), or the PS/2 auxiliary port.
typedef struct {
    char key[OS64_MOUSE_KEY];
    uint32_t speed; // Percent of raw relative motion; 100 is unscaled.
    uint32_t right_primary;
} os64_mouse_setting_t;
typedef struct {
    os64_mouse_setting_t setting;
    char name[OS64_MOUSE_NAME];
    uint32_t connected, reserved;
} os64_mouse_device_t;
typedef struct {
    uint32_t version, count;
    uint64_t generation;
    os64_mouse_device_t devices[OS64_MOUSE_DEVICES];
} os64_mouse_snapshot_t;
typedef struct {
    uint32_t version, count;
    uint64_t expected_generation;
    // FORGET requires one valid setting and refuses a connected identity.
    uint32_t operation, reserved;
    os64_mouse_setting_t settings[OS64_MOUSE_DEVICES];
} os64_mouse_command_t;

static inline bool os64_mouse_key_valid(const char key[OS64_MOUSE_KEY])
{
    for(size_t i=0;i<OS64_MOUSE_KEY;i++) {
        char c=key[i];
        if(!c) return i!=0;
        if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-')) return false;
    }
    return false;
}
static inline bool os64_mouse_setting_valid(const os64_mouse_setting_t *s)
{
    return os64_mouse_key_valid(s->key) && s->speed>=OS64_MOUSE_MIN_SPEED &&
        s->speed<=OS64_MOUSE_MAX_SPEED && s->right_primary<=1;
}
#endif
