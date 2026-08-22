#ifndef VT_SELECT_H
#define VT_SELECT_H

#include <stdbool.h>
#include "gui/input.h"

// vt_select.h — mouse selection on a TEXT terminal. The last consumer in
// CLIPBOARD.md's ratified order, and the one Chris named as the headline
// wish: highlight on a text console auto-copies, right-click pastes into the
// console's input.
//
// LINEAGE, and the answer to "shouldn't this be a mouse daemon?": on Linux it
// was — gpm, Alessandro Rubini, 1994. But gpm was a daemon because of where
// the pieces lived, not because a daemon is the right shape: Linux's mouse
// arrived as a character device with no in-kernel consumer, so gpm read
// /dev/mouse in userland and then handed the SELECTION back to the kernel
// through ioctl(TIOCLINUX) — the highlight and the paste were always kernel
// code. os64 already has all three pieces on this side of the boundary: the
// PS/2 driver, the console's cell grid (tty.h), and the clipboard
// (clipboard.h). A daemon here would mean inventing a /dev/mouse to export
// events and a control path to feed them straight back in — two new ABIs to
// carry data out of the kernel and return it unchanged. The seam stays open
// if selection POLICY ever wants to live in userland (word/line modes,
// gpm's real value-add); until then this is where the shortest honest path
// runs.
//
// Who drives it: the COMPOSITOR THREAD, which already drains the one input
// ring every frame whether or not it owns the glass. That is why there is no
// new thread and no IRQ-context painting here — the fork is a routing
// decision in compositor.c, not a second input path.

// Route one mouse event to the focused text terminal. Called by the
// compositor while the GUI does NOT own the glass; runs under kGuiLock, and
// only records state — nothing is painted here.
void vtsel_mouse_event(const input_event_t *ev);

// Paint the overlay (pointer + highlight) if anything changed. Called by the
// compositor once per frame with kGuiLock RELEASED, because painting takes
// the tty and renderer locks and those must never be reached while holding
// the compositor's.
void vtsel_paint(void);

// Drop everything and forget what was painted: the glass changed hands, or a
// VT switch happened, and whatever we drew is gone with the repaint.
void vtsel_forget(void);

#endif
