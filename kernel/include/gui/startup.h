#ifndef GUI_STARTUP_H
#define GUI_STARTUP_H

#include <stdbool.h>
#include <stddef.h>

// gui/startup.h — WHAT STARTS WITH THE DESKTOP (2026-08-23).
//
// THE PROBLEM (Chris, the afternoon the search path landed): the GUI's
// startup was two hardcoded facts in compositor.c — a "hello os64" window and
// a pair of demo apps — and everything ELSE he wanted at boot lived in
// husk.rc, which is the wrong place for it twice over:
//
//   1. husk.rc runs in EVERY husk, and VT1 and VT2 both start one. Two
//      shells, two gclocks, two gterms.
//   2. husk.rc runs whether or not there is a GUI, so on a text boot every
//      GUI line failed loudly, once per terminal.
//
// Both symptoms are the same mistake: a GUI app's startup was tied to a
// SHELL's startup. It belongs to the DESKTOP, which starts exactly once and
// only when there is a desktop. Move the line and both symptoms go.
//
// THE FILE is `gui.conf`, found through the config search path (conf.h), so
// /home's copy beats /etc's like every other config:
//
//   start = /bin/gterm      a program to launch, repeatable, in order
//   start = /bin/gclock
//   hello = no              the legacy "hello os64" window (default yes)
//
// THE ONE RULE WORTH KNOWING: if the file EXISTS, its `start` lines are the
// whole list — including when there are none, which is how you say "start
// nothing". Only when no gui.conf is found anywhere does the built-in list
// (the two demos) apply. Otherwise "the file is present and starts nothing"
// would be unspellable, and a config you cannot use to turn something OFF is
// half a config.

#define GUI_STARTUP_MAX_APPS 16
#define GUI_STARTUP_PATH_MAX 128

// Read gui.conf and settle the answers. Called once, at the TOP of
// gui_start() — before the compositor task is submitted, because the
// compositor thread reads gui_startup_hello() when it builds its scene and a
// parse that raced it would be a coin toss.
void gui_startup_load(void);

// Should the legacy "hello os64" window be created? Default true: it has been
// there since the first frame the compositor ever drew, and an absent config
// must not quietly retire it.
bool gui_startup_hello(void);

// The programs to launch, in file order.
size_t gui_startup_app_count(void);
const char *gui_startup_app(size_t index);

#endif // GUI_STARTUP_H
