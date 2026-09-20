#ifndef APPEARANCE_FONT_PAGE_H
#define APPEARANCE_FONT_PAGE_H
#include "os64/ui.h"
void font_page_init(os64_ui_t *, os64_ui_t *, os64_ui_widget_t *, void (*status)(const char *));
void font_page_activate(void);
void font_page_layout(bool visible);
void font_page_apply(void);
void font_page_save(void);
void font_page_reset(void);
void font_page_undo(void);
bool font_page_dirty(void);
bool font_page_can_undo(void);
void font_page_close(void);
#endif
