#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include "spinlock.h"

// GUI-internal shared state — for compositor.c / window.c / gui_client.c
// ONLY. Nothing here is client API (that's gui_client.h).
//
// kGuiLock serializes ALL mutable window-system state: the z-order list,
// per-window event queues, the damage accumulator, and the handle table.
// Rules:
//  * gui_client.c and the compositor ACQUIRE it; window.c (wm_*) functions
//    assume the caller already holds it.
//  * Compositing into the backbuffer happens UNDER the lock (sub-millisecond
//    RAM work; keeps window lifetimes trivially safe), but the flush to the
//    slow uncached framebuffer happens strictly AFTER release.
//  * Never taken from IRQ context, so the irqsave discipline is belt-and-
//    suspenders — it keeps a panic-during-composite from ever deadlocking a
//    fault path that might want GUI state later.

extern spinlock_t kGuiLock;

#include "gui/gui_types.h"

// Damage accumulation for callers that ALREADY hold kGuiLock (wm_* internals,
// the compositor's routing). The public gui_damage_add() (compositor.h) is
// the take-the-lock-yourself variant; kGuiLock is not recursive, so picking
// the wrong one deadlocks — hence the loud name.
void gui_damage_add_locked(rect_t screen_rect);

#endif // GUI_INTERNAL_H
