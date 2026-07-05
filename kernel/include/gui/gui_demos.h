#ifndef GUI_DEMOS_H
#define GUI_DEMOS_H

#include <stdbool.h>

// Demo GUI apps — kernel threads that exercise the client API exactly the
// way future userland apps will (they include ONLY gui/gui_client.h for
// their GUI work; these prototypes exist for task_create's name-match).
//
//  /gbounce — animated bouncing ball; proves damage-limited animation and
//             flicker-free overlap while other windows drag across it.
//  /gkeys   — echoes typed characters and clicks; proves focus routing,
//             per-window event queues, and content-local coordinates.

bool gbounce_thread(bool daemon);
bool gkeys_thread(bool daemon);

#endif // GUI_DEMOS_H
