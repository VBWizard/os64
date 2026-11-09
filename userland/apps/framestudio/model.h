#ifndef FRAME_STUDIO_MODEL_H
#define FRAME_STUDIO_MODEL_H
#include "os64/decoration_prepare.h"
#include "os64/font_config.h"
typedef struct { os64_decor_header_t style; os64_font_config_role_t font; } frame_draft_t;
void frame_preset(frame_draft_t *, unsigned preset);
bool frame_same(const frame_draft_t *, const frame_draft_t *);
#endif
