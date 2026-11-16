// os64/mediakeys.h — a media-key session (/dev/mediakeys), shared by the
// kernel and ring 3 (docs/design/pending/MEDIA_KEYS.md).
//
// A program opens /dev/mediakeys in mode "u"; the handle is its session.
// Writing one word ("playing", "paused", "stopped") says the program's
// state; reading gives the next press meant for it, one record a read.

#ifndef OS64_MEDIAKEYS_H
#define OS64_MEDIAKEYS_H

#include <stdint.h>

typedef struct {
	uint32_t key;        // OS64_MEDIA_KEY_PLAY .. _STOP
	uint32_t serial;     // this session's presses, counted from 1
	uint64_t at_us;      // micros() when the press was routed
} os64_media_key_t;

#define OS64_MEDIA_KEY_PLAY      1u
#define OS64_MEDIA_KEY_PAUSE     2u
#define OS64_MEDIA_KEY_NEXT      3u
#define OS64_MEDIA_KEY_PREVIOUS  4u
#define OS64_MEDIA_KEY_STOP      5u

// Negative answers of this family. -1 (refused, which a write of a word
// that is no state answers), -2 (unreadable memory), OS64_INTERRUPTED (-4)
// and OS64_ERR_TIMEOUT (-10) keep their system-wide meanings here.
#define OS64_MEDIA_KEYS_FULL     (-30)   // open: every session the machine keeps is taken
#define OS64_MEDIA_KEYS_BUSY     (-31)   // read: another thread is already waiting on this session

#endif
