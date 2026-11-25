#ifndef GUI_PLANE_H
#define GUI_PLANE_H

// plane.h — a window's planes, the kernel's side (docs/design/pending/
// VIDEO_PLANE.md § 3; os64/gui.h § Planes is the contract ring 3 reads).
//
// A plane is a picture the compositor draws behind a window's content,
// where the content lets it through. Its two stores — the canvas its
// creating thread draws in and the front the compositor reads — come as a
// PAIR with a record of its own, and the record lives on its task's list
// from the pair's making to the task's burial, in use or KEPT: a destroyed
// plane's pair is kept for the next plane of its capacity class or a
// smaller one, so a canvas is never unmapped while a thread of its task
// lives and a stale pointer to one can only spoil a later plane of the same
// task. Giving a pair back is a flag under kGuiLock; nothing is allocated,
// mapped or freed under it.
//
// Every field of a window's planes is read and written under kGuiLock.

#include <stdbool.h>
#include <stdint.h>

#include "gui/gui_types.h"
#include "gui/plane_compose.h"
#include "os64/gui.h"   // the plane ABI: the surface and the edit

#define GUI_WINDOW_PLANES 4
// The most a task's planes and kept pairs may hold, pairs being prepared
// counted: two 3840x2160-class pairs, or eight 1920x1080-class ones.
#define GUI_PLANE_BYTES (128ull << 20)
// The most pixels a place's overlay rectangles may cover together: about a
// player's control strip across a 1080p picture. The compositor blends
// each of them under kGuiLock at every picture, so this is a time bound
// (VIDEO_PLANE.md § 3.3) for a hold of about a millisecond. PROVISIONAL:
// the P5 priced the first composer at 19.77 ns a blended pixel (1.9 ms at
// this number); the number is set again from the P5's price for the
// composer as it now is (planetest prints it).
#define GUI_PLANE_OVERLAY_PIXELS 98304u

typedef enum {
	GUI_PLANE_FREE,       // no plane in this slot
	GUI_PLANE_CREATED,    // stores, no picture, no place
	GUI_PLANE_PICTURED,   // a picture, no place: hidden
	GUI_PLANE_PLACED,     // a picture and a place: shown where the content lets it
} gui_plane_state_t;

struct s_thread;
struct plane_store;

typedef struct gui_plane {
	uint8_t state;            // gui_plane_state_t
	uint32_t generation;      // 24 bits: the name is the slot and this
	struct s_thread *creator; // the thread that made it, the only one that may ask for its surface, publish or destroy
	struct plane_store *store;
	uint32_t *front;          // the store's front, the compositor's copy
	uint32_t cap_w, cap_h;    // the stores' capacity (the front's pitch is cap_w), its class or a kept larger one
	uint32_t pic_w, pic_h;    // the picture the last publish made, 0 x 0 before one
	pc_place_t place;         // the committed place; an empty clip is no place
	uint64_t publishes;
	pc_counts_t composed;     // what the compositor drew of it
	uint64_t compose_cycles;  // and how long that took, in TSC cycles
} gui_plane_t;

// Why a plane call or a commit was refused, for /sys/gui.
typedef enum {
	GUI_PLANE_REFUSED_NOT_CREATOR,
	GUI_PLANE_REFUSED_NO_PLANE,
	GUI_PLANE_REFUSED_NO_SLOT,
	GUI_PLANE_REFUSED_TOO_BIG,
	GUI_PLANE_REFUSED_BUDGET,
	GUI_PLANE_REFUSED_MEMORY,
	GUI_PLANE_REFUSED_BUSY,
	GUI_PLANE_REFUSED_DAMAGE,
	GUI_PLANE_REFUSED_OVERLAP,
	GUI_PLANE_REFUSED_PICTURE,
	GUI_PLANE_REFUSALS
} gui_plane_refusal_t;

struct task;
struct window;

// The calls (os64/gui.h § Planes). `handle` is the caller's window; the
// owner check is every GUI call's. A surface's `out`, an edit list and a
// damage rectangle are kernel copies (the syscall copied them in).
int64_t gui_plane_create(int64_t handle, uint32_t width, uint32_t height);
int64_t gui_plane_surface(int64_t handle, uint32_t plane, os64_gui_plane_surface_t *out);
int64_t gui_plane_publish(int64_t handle, uint32_t plane, uint32_t width, uint32_t height);
int64_t gui_plane_destroy(int64_t handle, uint32_t plane);
int64_t gui_window_publish_edits(int64_t handle, const os64_gui_rect_t *damage,
                                 const os64_gui_plane_edit_t *edits, uint32_t count);

// A thread leaving by its exit call takes the planes it created out with
// it: hidden (their clips damaged), then destroyed, their pairs kept. From
// syscall context, nothing held.
void gui_thread_planes_exit(struct s_thread *self);
// A buried task's pairs, every one, back to the allocator: after its
// windows (and so its planes) are gone and every thread of it is retired,
// with its page tables intact. Takes and releases kGuiLock itself, and
// frees after it.
void gui_task_planes_release(struct task *t);
// Whether a window has a plane in any slot (a window with planes cannot be
// destroyed). Under kGuiLock.
bool gui_window_has_planes(const struct window *w);
// The compositor's part of a window whose content is at `content_screen`:
// where the back buffer's part `damage` (screen coordinates, `view` its
// pixels) meets a placed plane, the content and the plane are drawn
// together, each clip cut to the content as it stands. True when it drew
// the content's whole part of the view itself; false when no placed plane
// meets it and the caller draws the content as it always does. Under
// kGuiLock; the counts it adds to are the compositor's.
bool gui_planes_compose(struct window *w, struct surface *view, rect_t damage, rect_t content_screen);

// One row of /sys/gui's plane report.
typedef struct {
	uint32_t window, name, state;
	uint32_t cap_w, cap_h, pic_w, pic_h;
	pc_rect_t origin, clip;
	uint32_t noverlays;
	uint64_t overlay_area, publishes, compose_ns;
	pc_counts_t composed;
} gui_plane_status_t;
// The live planes, at most `cap` of them, and the refusals by reason; the
// count of planes there were. Takes kGuiLock.
uint32_t gui_planes_status(gui_plane_status_t *out, uint32_t cap, uint64_t refused[GUI_PLANE_REFUSALS],
                           uint64_t *pairs, uint64_t *pairs_kept, uint64_t *pair_bytes);
const char *gui_plane_refusal_name(gui_plane_refusal_t why);

// For the kernel's own tests: the budget a kernel task's create is held to
// (every other task's stays GUI_PLANE_BYTES; 0 puts it back), and a kernel
// task's kept pairs freed (its canvases are kernel memory, so nothing in
// ring 3 can hold them).
void gui_plane_budget_for_test(uint64_t bytes);
void gui_plane_release_kept_for_test(struct task *t);

#endif
