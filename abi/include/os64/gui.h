// os64/gui.h — the GUI boundary, as ring 3 sees it (L0 of LIBDRAW.md).
//
// Everything that crosses syscalls 16-22 is defined HERE for userland: the
// three structs (rect, surface, input event), the window flags, the error
// values, the color constants, and thin inline wrappers over the raw
// syscalls. The kernel keeps its own headers (gui/gui_types.h, gui/input.h
// — written dependency-free precisely so this day could come), and
// gui_client.c carries _Static_asserts pinning BOTH sides to the same
// sizes and offsets: if either definition drifts, the KERNEL BUILD breaks,
// not a pixel. (The ext2 superblock plays the same trick with its on-disk
// offsets — a layout that two parties must agree on gets a tripwire, not
// trust.)
//
// Coordinates are CONTENT-local, origin top-left. Color is XRGB8888 (the X
// byte reserved for future alpha). Drawing never crosses the ring — an app
// draws into its mapped canvas and only publish() takes a syscall; that
// cost model is GRAPHICS.md's founding rule ("syscalls carry handles,
// rects, and signals — never pixels").

#ifndef OS64_GUI_H
#define OS64_GUI_H

#include <stdint.h>
#include <stdbool.h>
#include "os64/syscall.h"
#include "os64/syscall_numbers.h"

// ── Errors (gui_client.h's values, verbatim) ────────────────────────────────
#define OS64_GUI_ERR_INVALID_HANDLE  (-1)
#define OS64_GUI_ERR_NO_RESOURCES    (-2)
#define OS64_GUI_ERR_BAD_ARGS        (-3)
#define OS64_GUI_ERR_NOT_RUNNING     (-4)   // boot without the GUI flag — treat as SKIP
#define OS64_GUI_ERR_NOT_OWNER       (-5)   // exists, and is none of your business
#define OS64_GUI_ERR_INTERRUPTED     (-6)   // a blocking wait cut short by termination

// ── Window flags ────────────────────────────────────────────────────────────
#define OS64_GUI_WINDOW_NO_DECORATIONS  (1u << 0)   // reserved; not yet honored
#define OS64_GUI_WINDOW_START_UNFOCUSED (1u << 1)   // born on top, declines focus

// ── The shared palette (gui_types.h's values, verbatim) ─────────────────────
#define OS64_GUI_COLOR_BLACK      0xff000000u
#define OS64_GUI_COLOR_WHITE      0xffffffffu
#define OS64_GUI_COLOR_GRAY       0xff808080u
#define OS64_GUI_COLOR_LIGHT_GRAY 0xffc0c0c0u
#define OS64_GUI_COLOR_DARK_GRAY  0xff404040u
#define OS64_GUI_COLOR_RED        0xffcc3333u
#define OS64_GUI_COLOR_GREEN      0xff33aa55u
#define OS64_GUI_COLOR_BLUE       0xff3355ccu
#define OS64_GUI_COLOR_YELLOW     0xffe0c040u
#define OS64_GUI_COLOR_DESKTOP    0xff2a5566u

// ── The three boundary structs ──────────────────────────────────────────────
// Layout-locked against the kernel's gui_types.h / input.h by the asserts
// in gui_client.c. Do not reorder fields on either side alone.

typedef struct os64_gui_rect
{
    int32_t x, y;
    int32_t w, h;
} os64_gui_rect_t;

typedef struct os64_gui_surface
{
    uint32_t *pixels;    // your mapped canvas — draw here, then publish
    uint32_t width;      // in pixels
    uint32_t height;
    uint32_t pitch_px;   // pixels per row (== width for canvases)
} os64_gui_surface_t;

// Input event types (input.h's values, verbatim).
#define OS64_GUI_EVENT_KEY_DOWN          1
#define OS64_GUI_EVENT_KEY_UP            2
#define OS64_GUI_EVENT_MOUSE_MOVE        3
#define OS64_GUI_EVENT_MOUSE_BUTTON_DOWN 4
#define OS64_GUI_EVENT_MOUSE_BUTTON_UP   5

typedef struct os64_gui_event
{
    uint8_t type;
    union {
        struct {
            char    ascii;      // 0 when the key has no glyph
            uint8_t scancode;   // PS/2 set-1 make code
            uint8_t modifiers;
        } key;
        struct {
            int32_t x, y;       // CONTENT-local cursor position
            int16_t dx, dy;     // raw motion delta
            uint8_t buttons;    // current button state bitmask
            uint8_t button;     // which button changed (DOWN/UP)
        } mouse;
    };
    uint64_t tick;              // kTicksSinceStart at enqueue
} os64_gui_event_t;

// ── L0: the wrappers (thin by doctrine — all logic is kernel-side) ──────────

// Returns a window handle > 0, or a negative OS64_GUI_ERR_*. NOT_RUNNING
// means this boot has no GUI — a well-mannered app treats that as SKIP.
static inline int64_t os64_gui_window_create(const char *title,
                                             int32_t x, int32_t y,
                                             uint32_t w, uint32_t h,
                                             uint64_t flags)
{
    return (int64_t)os64_syscall6(SYSCALL_GUI_WINDOW_CREATE, (uint64_t)title,
                                  (uint64_t)(int64_t)x, (uint64_t)(int64_t)y,
                                  w, h, flags);
}

static inline int64_t os64_gui_window_destroy(int64_t handle)
{
    return (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_DESTROY,
                                  (uint64_t)handle, 0);
}

// Fills *out with your canvas: a task-local pointer you draw through at
// memory speed. Geometry is the CONTENT area (frame minus chrome).
static inline int64_t os64_gui_window_get_surface(int64_t handle,
                                                  os64_gui_surface_t *out)
{
    return (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_GET_SURFACE,
                                  (uint64_t)handle, (uint64_t)out);
}

// damage NULL = the whole content. The kernel snapshots the damage rect
// canvas→content under its lock — the screen only ever shows frames you
// FINISHED (GRAPHICS.md "Atomic frames").
static inline int64_t os64_gui_window_publish(int64_t handle,
                                              const os64_gui_rect_t *damage)
{
    return (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_PUBLISH,
                                  (uint64_t)handle, (uint64_t)damage);
}

// 1 = event copied out, 0 = queue empty, negative = error.
static inline int64_t os64_gui_event_poll(int64_t handle,
                                          os64_gui_event_t *out)
{
    return (int64_t)os64_syscall2(SYSCALL_GUI_EVENT_POLL,
                                  (uint64_t)handle, (uint64_t)out);
}

static inline int64_t os64_gui_screen_info(uint32_t *width, uint32_t *height)
{
    return (int64_t)os64_syscall2(SYSCALL_GUI_SCREEN_INFO,
                                  (uint64_t)width, (uint64_t)height);
}

// Block until an event arrives (1 = copied out), the window dies
// (INVALID_HANDLE), or the caller is being killed (INTERRUPTED). The idle
// answer to a poll loop: an app that waits costs NOTHING until somebody
// types at it — no cadence, no polling, just sleep until the compositor
// says otherwise.
static inline int64_t os64_gui_event_wait(int64_t handle,
                                          os64_gui_event_t *out)
{
    return (int64_t)os64_syscall2(SYSCALL_GUI_EVENT_WAIT,
                                  (uint64_t)handle, (uint64_t)out);
}

// ── The layout lock, this side ──────────────────────────────────────────────
// The same numbers are asserted against the KERNEL structs in gui_client.c;
// a divergence on either side refuses to build.
_Static_assert(sizeof(os64_gui_rect_t) == 16, "gui rect ABI: 16 bytes");
_Static_assert(sizeof(os64_gui_surface_t) == 24, "gui surface ABI: 24 bytes");
_Static_assert(sizeof(os64_gui_event_t) == 32, "gui event ABI: 32 bytes");
_Static_assert(__builtin_offsetof(os64_gui_event_t, key) == 4,
               "gui event ABI: union at offset 4");
_Static_assert(__builtin_offsetof(os64_gui_event_t, tick) == 24,
               "gui event ABI: tick at offset 24");

#endif // OS64_GUI_H
