#ifndef GUI_GLASS_H
#define GUI_GLASS_H

// gui/glass.h — the kernel half of /dev/glass (os64/glass.h is the ABI,
// REMOTE.md section 3 the design). A glass VIEW is one reader's record of
// what changed on the screen since it last looked. The compositor feeds every
// open view the damage of every frame; a read takes one rectangle out and
// copies its pixels from the backbuffer.
//
// LOCKING: the views and their records are kGuiLock's, like the rest of the
// window system's state. A read takes its rectangle under the lock and copies
// the pixels after releasing it, so a slow copy never holds up a frame.
//
// LIFETIME: a view is reference counted. The handle holds one reference, each
// operation in flight holds one (handle.c's pin), and the last release frees
// it. Closing the handle marks the view closed and wakes a reader parked in
// it, which then answers GLASS_ERR_CLOSED instead of reading freed memory.

#include <stddef.h>
#include <stdint.h>
#include "gui/gui_types.h"

typedef struct glass_view glass_view_t;

// Read outcomes other than a byte count (glass_view_read).
#define GLASS_ERR_TIMEOUT      (-2)   // the deadline passed with nothing changed
#define GLASS_ERR_INTERRUPTED  (-3)   // a signal ended the wait
#define GLASS_ERR_CLOSED       (-4)   // the handle was closed under the reader
#define GLASS_ERR_SMALL        (-5)   // the buffer cannot hold the header and one row

// A new view whose first read is the whole screen, holding one reference for
// its handle; NULL on a boot with no desktop (no backbuffer) or no memory.
glass_view_t *glass_view_open(void);
void glass_view_ref(glass_view_t *v);
void glass_view_release(glass_view_t *v);
// The handle's close: marks the view closed, wakes a parked reader, and drops
// the handle's reference.
void glass_view_close(glass_view_t *v);

// Take one changed rectangle into `out` (kernel memory, at most `cap` bytes):
// an os64_glass_rect_t and its pixel rows. Blocks until something changed,
// until `deadline` (a tick; 0 = no deadline) or until a signal must be heard.
// Returns the bytes written or a GLASS_ERR_*. Task context only.
long glass_view_read(glass_view_t *v, void *out, size_t cap, uint64_t deadline);

// The compositor's feed: this frame's damage, already clipped to the screen,
// added to every open view. Caller holds kGuiLock.
void glass_damage_locked(const rect_t *rects, uint32_t count);

// Open views, for /sys/gui. Takes kGuiLock.
uint32_t glass_view_count(void);

#endif // GUI_GLASS_H
