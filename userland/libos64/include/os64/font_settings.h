#ifndef OS64_FONT_SETTINGS_H
#define OS64_FONT_SETTINGS_H
#include "os64/font_config.h"

/* Read effective choices and their font publication serial (zero for startup). Parsing/I/O refusal
 * leaves out unchanged. Absence of a font component uses fonts.conf. */
int os64_font_settings_current(os64_font_config_t *out, uint64_t *generation);
/* Validate/load the complete candidate before generation-checked publication.
 * Returns UI_APPLY_*; publication does not mean every process adopted it. */
int os64_font_settings_apply(os64_text_context_t *, const os64_font_config_t *,
                             uint64_t *published, os64_font_config_error_t *);
/* Capture the complete usable session before changing next-boot font choices. */
int os64_font_settings_save(const os64_font_config_t *, os64_font_config_error_t *);
#endif
