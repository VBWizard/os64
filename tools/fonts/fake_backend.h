#ifndef OS64_FAKE_FONT_BACKEND_H
#define OS64_FAKE_FONT_BACKEND_H
#include "os64/font_backend.h"
/* Test-only format: one immutable byte, 'P' proportional, 'M' monospace, or
 * 'L' proportional with W missing to exercise fallback, or 'S' proportional
 * with a line height that follows the requested pixel size. 'T' also varies
 * line height, but supplies fixed-width printable ASCII for terminal tests.
 * Other metrics remain fixed; S exercises layout-height transitions without
 * a real font. */
const os64_font_backend_t *os64_fake_font_backend(void);
#endif
