// scribe.h — the editor as something that can be RUN, not only launched.
//
// /bin/scribe is a two-line main that calls scribe_main with no hooks.
// /tests/scribefonttest calls the same scribe_main with hooks of its own, so
// the fixture drives the real window, the real document model and the real
// event loop rather than a lookalike editor built to be tested. Everything a
// test needs to do that production scribe does not want lives in the hooks,
// which is why production scribe has no test keys at all.

#ifndef SCRIBE_H
#define SCRIBE_H

#include <stdbool.h>
#include "os64/ui.h"

typedef struct
{
    // Called once the window, the widgets and the document exist and before
    // the first paint: the place a fixture binds the fonts it is testing.
    void (*ready)(os64_ui_t *ui, void *user);
    // Offered every key event ahead of scribe's own shortcuts and widgets.
    // Return true to consume it. A fixture's controls live here.
    bool (*key)(os64_ui_t *ui, const os64_gui_event_t *ev, void *user);
    void *user;
} scribe_hooks_t;

// Run the editor on argv[1] (or an unnamed buffer). `hooks` may be NULL.
int scribe_main(int argc, char **argv, const scribe_hooks_t *hooks);

// What a fixture may ask of the document it is driving. These read state;
// they never edit it behind the editor's back.
bool   scribe_help_active(void);
bool   scribe_dirty(void);
size_t scribe_line_count(void);
const char *scribe_line(size_t index, size_t *len);
os64_ui_textview_t *scribe_view(void);
int64_t scribe_extent(void);
// Toggle the help page, exactly as Ctrl+G does.
void scribe_toggle_help(void);
// Open a file, and save to one, exactly as the Open and Save As fields do
// when their Enter is pressed. The save says whether the bytes reached the
// disk.
void scribe_open(const char *path);
bool scribe_save_as(const char *path);

#endif
