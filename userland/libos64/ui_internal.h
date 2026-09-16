#ifndef OS64_UI_INTERNAL_H
#define OS64_UI_INTERNAL_H
#include "os64/ui.h"
// Shared escape-burst decoder for text widgets and numeric controls.
typedef enum
{
	K_NONE,        // consumed mid-burst, or ignorable
	K_CHAR,        // printable; the byte is in *ch
	K_ENTER, K_BACKSPACE, K_TAB, K_ESC,
	K_UP, K_DOWN, K_LEFT, K_RIGHT,
	K_HOME, K_END, K_PGUP, K_PGDN, K_DELETE,
} ui_key_t;

ui_key_t os64_ui_decode_key(uint8_t *seq, const os64_gui_event_t *ev, char *ch);
// Detailed decode status lets the session cache retry allocation failures.
int64_t os64_ui_theme_parse_status(os64_ui_theme_t *t, const char *text,
                                  size_t length, bool session);
int os64_ui_theme_preserve_session(void);
int64_t os64_ui_theme_snapshot_startup(char *text, size_t cap);
int64_t os64_ui_theme_decode_session(os64_ui_theme_t *t, uint64_t *fields,
                                     bool *inherited, const char *text, size_t length);
void os64_ui_theme_merge_fields(os64_ui_theme_t *dst, const os64_ui_theme_t *src,
                                uint64_t fields);
int64_t os64_ui_theme_parse_saved_status(os64_ui_theme_t *t, const char *text, size_t length);
#endif
