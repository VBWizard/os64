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

// Panic path: kill the console diversion with a single lock-free store so
// panic text renders raw on the framebuffer. Safe from ANY context.
void gui_emergency_disable(void);

#endif // GUI_COMPOSITOR_H
