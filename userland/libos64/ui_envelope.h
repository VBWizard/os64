#ifndef OS64_UI_ENVELOPE_H
#define OS64_UI_ENVELOPE_H
#include "os64/font_config.h"
#include "os64/ui.h"
#define UI_ENVELOPE_FONTS 4u
#define UI_ENVELOPE_GEOMETRY 8u
#define UI_ENVELOPE_INHERIT 16u
#define UI_ENVELOPE_THEME (1u | 2u | 8u | 16u)
/* Known theme ownership, or zero for a key absent from the theme schema. */
unsigned ui_theme_key_owner(const char *key);
/* Syntax validation and lossless selection of lines. Unknown dotted settings,
 * comments and blank lines have owner zero. Negative returns are CONF errors.
 * `keep_unowned` retains these lines in addition to the selected components. */
int64_t ui_envelope_select(const char *, size_t, unsigned owners, bool keep_unowned,
                            char *, size_t);
/* Validate known font keys and extract a complete defaulted font component.
 * Relative paths are refused in a session/saved envelope. */
int64_t ui_envelope_fonts(const char *, size_t, os64_font_config_t *, bool *present, uint64_t *serial);
/* Remove component-owned lines, including duplicates, and append replacements.
 * The caller validates the resulting complete snapshot before publication. */
int64_t ui_envelope_merge(const char *, size_t, const char *, size_t, unsigned owners,
                           char *, size_t);
#endif
