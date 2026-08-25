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
#include "os64/signal.h"   // OS64_INTERRUPTED — the one answer for "a signal cut your wait short"

// ── Errors (gui_client.h's values, verbatim) ────────────────────────────────
#define OS64_GUI_ERR_INVALID_HANDLE  (-1)
#define OS64_GUI_ERR_NO_RESOURCES    (-2)
#define OS64_GUI_ERR_BAD_ARGS        (-3)
// A blocking wait (os64_gui_event_wait) cut short by a signal. NOT a GUI
// error of its own: it is THE system-wide sentinel, OS64_INTERRUPTED, because
// the signal contract promises every interrupted blocking call answers the
// same value and a program written to that contract must not have to know
// which subsystem it was waiting on. Was -6 until Codex #29 rd19, with
// NOT_RUNNING on -4 — so a GUI program checking for OS64_INTERRUPTED would
// have read "no desktop" instead.
#define OS64_GUI_ERR_INTERRUPTED     OS64_INTERRUPTED
#define OS64_GUI_ERR_NOT_OWNER       (-5)   // exists, and is none of your business
#define OS64_GUI_ERR_NOT_RUNNING     (-6)   // boot without the GUI flag — treat as SKIP (was -4; moved for the sentinel above)

// ── Window flags ────────────────────────────────────────────────────────────
#define OS64_GUI_WINDOW_NO_DECORATIONS  (1u << 0)   // no titlebar; 1px border stays (honored since 2026-08-23)
#define OS64_GUI_WINDOW_START_UNFOCUSED (1u << 1)   // born on top, declines focus
#define OS64_GUI_WINDOW_PINNED           (1u << 2)   // born in the always-on-top band
// THE DESKTOP BAND: born at the BOTTOM of the z-list, where nothing can get
// beneath it. This is what lets a desktop shell be an ordinary ring-3
// program — the same arrangement X11 has always had, where the server owns
// the root window and the thing drawing your wallpaper is just a client.
//
// It buys a z-band and NOTHING ELSE. A desktop window is still focusable and
// still receives keys and clicks, because being the target for a click that
// lands on no application is half the point (that click is where a root menu
// and a launcher come from). It is skipped by the Alt+Tab walk, and the
// destructive chords decline it — Alt+F4 on your desktop is a gesture nobody
// means.
//
// Setting it is not a claim of authority: a second desktop window is legal
// and simply stacks at the bottom too. os64 has no privilege model, and
// inventing one for a stacking hint would be a lock on the wrong door.
//
// The ONE combination refused is DESKTOP together with PINNED, which asks to
// be at the bottom and the top at once. create() answers BAD_ARGS rather than
// picking a winner — see gui_client.c for why silently resolving it was worse
// than refusing.
#define OS64_GUI_WINDOW_DESKTOP          (1u << 5)   // born in the bottom band

// READ-ONLY state, reported by os64_gui_window_get_state and IGNORED at
// create — gui_window_create masks the flag word down to the CREATION bits
// above, so naming these here cannot let an app claim them at birth.
// They are published because an app SAVING its geometry needs to know it is
// not saving a maximized or minimized frame as its ordinary position.
#define OS64_GUI_WINDOW_MAXIMIZED        (1u << 3)
#define OS64_GUI_WINDOW_MINIMIZED        (1u << 4)

// Where a window IS and what state it is in — the readback half of create.
//
// x/y/width/height describe the FRAME, in exactly the units
// os64_gui_window_create takes, so a saved state hands straight back to a
// later create with nothing to convert. (get_surface reports the CONTENT
// area, which is the drawing question; this is the placement question.)
typedef struct {
    int32_t  x, y;              // frame origin on screen
    uint32_t width, height;     // frame size, chrome included
    uint32_t flags;             // OS64_GUI_WINDOW_* as they stand right now
} os64_gui_window_state_t;

// The window title's capacity, NUL included (gui/window.h's value, verbatim
// — window.c static-asserts the two stay equal). This is ABI because the
// boundary REFUSES an over-long title with BAD_ARGS rather than truncating
// it — the house convention for every string crossing ring 3 — which makes
// fitting the title the APP's job and this number the app's business.
// (Discovered the honest way, 2026-08-21: `scribe /fat/boot/limine/
// limine.conf` put the whole path in its title and got -3 at the door.)
#define OS64_GUI_TITLE_MAX 32

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
    // Pixels per row — and NOT the same thing as width. A canvas is mapped at
    // its CAPACITY (see below), so pitch_px is the capacity's width and stays
    // put for the window's whole life while width/height move with the
    // window. Always address a pixel as pixels[y * pitch_px + x]; a canvas
    // whose pitch you assumed was its width draws a beautiful diagonal
    // staircase the first time somebody resizes it.
    //
    // WHY IT WORKS THIS WAY (the resize contract, 2026-08-19): the canvas is
    // reserved and mapped ONCE at capacity, so a resize never moves your
    // pointer and never relocates a pixel — the image you already drew is
    // still exactly where you left it, and the kernel never has to unmap a
    // page out from under a running app. That is the whole reason resizing
    // is cheap here instead of a lifetime problem.
    uint32_t pitch_px;
} os64_gui_surface_t;

// Input event types (input.h's values, verbatim).
#define OS64_GUI_EVENT_KEY_DOWN          1
#define OS64_GUI_EVENT_KEY_UP            2
#define OS64_GUI_EVENT_MOUSE_MOVE        3
#define OS64_GUI_EVENT_MOUSE_BUTTON_DOWN 4
#define OS64_GUI_EVENT_MOUSE_BUTTON_UP   5
// Your content area changed size. Re-fetch geometry with
// os64_gui_window_get_surface (the POINTER is unchanged — only width/height
// move) and repaint the whole thing: the newly exposed strip holds the
// window's background color, not your pixels.
#define OS64_GUI_EVENT_WINDOW_RESIZE     6
// The user asked this window to close (Alt+F4). A REQUEST: the window is
// yours, so you decide — save, ask, or just exit. Ignore it and the user's
// next Alt+F4 within five seconds is SIGTERM to your whole task, which is the
// answer you would have given anyway minus the chance to say goodbye. libui
// handles it (os64_ui_t.on_close, else it sets `quit`).
#define OS64_GUI_EVENT_WINDOW_CLOSE      7

// Modifier bits (the kernel's keyboard_modifiers_t, verbatim). Carried by
// key events and — since resize — by mouse events too.
#define OS64_GUI_MOD_SHIFT (1u << 0)
#define OS64_GUI_MOD_CTRL  (1u << 1)
#define OS64_GUI_MOD_ALT   (1u << 2)
#define OS64_GUI_MOD_CAPS  (1u << 3)
#define OS64_GUI_MOD_NUM   (1u << 4)
// Not a modifier: WHICH KEYBOARD. Set on every event from a USB (HID)
// keyboard, clear from PS/2. It rides in this byte because the `scancode`
// field means different things on the two paths — PS/2 set-1 make code
// versus HID usage — and a key with no ASCII (an F-key, an arrow) cannot
// be named any other way. Match printable keys by `ascii` and never look
// at this; match F-keys with it (F1 is 0x3B on PS/2 and 0x3A on HID).
#define OS64_GUI_MOD_HID   (1u << 7)

// Mouse buttons: bit positions in `mouse.buttons`, and the value carried in
// `mouse.button` on DOWN/UP. These crossed the boundary from the day mouse
// events did — the number was in every event and its MEANING was kernel-only
// (gui/input.h), so a client comparing `button == 1` was writing a magic
// number and hoping. gterm's right-click paste is the consumer that asked
// (2026-08-21); the kernel static-asserts the two copies agree.
#define OS64_GUI_MOUSE_LEFT   0
#define OS64_GUI_MOUSE_RIGHT  1
#define OS64_GUI_MOUSE_MIDDLE 2

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
            uint8_t modifiers;  // OS64_GUI_MOD_* at event time
        } mouse;
        struct {
            int32_t w, h;       // the NEW content size
        } resize;
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

// Where is my window, and what state is it in? Fills *out with the FRAME
// rect and the live flag word.
//
// THE POINT IS PERSISTENCE. Everything a user does to a window — drag it,
// resize it, Ctrl+Alt+P to pin it, Ctrl+Alt+T to drop the titlebar — happens
// in the window system, and until this call existed an app had no way to
// learn any of it. So it could not save what you had arranged, and every
// launch put the window back where the PROGRAM thought it belonged. Reading
// this at exit and writing it through os64_conf_write is the whole recipe.
static inline int64_t os64_gui_window_get_state(int64_t handle,
                                                os64_gui_window_state_t *out)
{
    return (int64_t)os64_syscall2(SYSCALL_GUI_WINDOW_GET_STATE,
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
