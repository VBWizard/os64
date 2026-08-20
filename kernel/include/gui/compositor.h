#ifndef GUI_COMPOSITOR_H
#define GUI_COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>

// The GUI is an optional subsystem, like AHCI/NVMe: the OS runs fully without
// it. Enabled via the GUI kernel cmdline flag; when off, nothing GUI-related
// is initialized and boot behavior is identical to a pre-GUI kernel.
extern bool kEnableGUI;

// Create and submit the compositor kernel task ("/guicomp") and, later, the
// demo app tasks. Called from kernel_init() after post-boot tests, only when
// kEnableGUI is set.
void gui_start(void);

// Where the compositor will be pinned: an APIC id, or THREAD_NO_AFFINITY
// (tickless mode / single core). kernel_init uses this to route the input
// IRQs (1, 12) at the same core, so a keystroke or mouse packet ends the
// compositor's hlt-wait directly instead of waiting for a scheduler pass.
uint64_t gui_compositor_affinity(void);

// The compositor daemon thread body (task_create name-matches "/guicomp" to
// this, same recipe as logd/kworker). daemon=true means loop forever.
bool guicomp_thread(bool daemon);

#include "gui/gui_types.h"

// Mark a screen-space rectangle as needing recomposite+flush. Safe to call
// from any thread context (takes the GUI lock); NOT from IRQ handlers.
void gui_damage_add(rect_t screen_rect);

// Panic path: unseat the compositor with a single lock-free store so its
// flushes stop and panic text renders raw on the framebuffer. Safe from ANY
// context.
void gui_emergency_disable(void);

// A window is about to be freed — drop any interactive grab that names it.
// Called by wm_destroy under kGuiLock, which makes it the ONE place a dying
// window can undo the compositor's pointer to it.
//
// This closes a hole that predates resize and was never reachable by accident
// only because nothing had ever died mid-gesture: the drag state held a raw
// window_t*, and a task exiting while the user held its titlebar (a `kill`
// from another VT is all it takes) left the next MOUSE_MOVE calling wm_move on
// freed memory. A write, not a fault — the lazy-HHDM tripwire would not have
// caught it, since the kmalloc'd window_t stays mapped until it is reused.
struct window;
void gui_grab_release(const struct window *w);

// ── VT8 glass ownership (the VT8 chapter in GRAPHICS.md, 2026-08-19) ────────
// True once gui_start has seated the compositor as VT8's shell. Stays false
// for the machine's whole life on a boot without the GUI flag — VT8 is then
// an ordinary text terminal.
bool gui_vt8_seated(void);
// The one predicate everything paints by: seated AND VT8 focused. tty.c's
// projection and keyboard.c's input fork read it; the compositor's flush
// loop is gated on it.
bool gui_owns_glass(void);
// tty_focus's handoff INTO the GUI: one lock-free store (safe from the
// keyboard IRQ, where VT switches happen); the compositor converts it to a
// full-screen repaint on its next frame.
void gui_vt8_focus_gained(void);

// The window census, locked and safe to call from anywhere in thread context
// — /sys/gui's supplier. Answers 0/0 when the GUI is off, so a caller needs
// no kEnableGUI check of its own. See wm_census_locked for why the byte count
// is worth publishing at all.
void gui_census(uint32_t *windows, uint64_t *surface_bytes);

// Task-lifecycle hook: destroy every window the dying task still owns,
// BEFORE its pages are torn down — GRAPHICS.md's ownership rule, "windows
// die before pages, always, on every exit path". Called from task.c's exit
// teardown (windows vanish when the app dies, not when it is buried) and
// again from task_destroy as belt-and-suspenders for any path that buries
// without a clean exit. Idempotent — the second call finds nothing — and a
// free no-op when the GUI is off or the task never made a window. Takes
// kGuiLock; GUI state is upper-half, so this is safe under any CR3.
//
// Takes the TASK, not just its id, since the surface pivot: a task-backed
// canvas is mapped in the dying task's own page tables, and giving those
// pages back (nothing else records them — they are deliberately not VMAs)
// needs the pml4v to unmap from. Must therefore run while the address
// space is still intact — which both call sites already guarantee.
struct task;
void gui_task_destroy_windows(struct task *t);

#endif // GUI_COMPOSITOR_H
