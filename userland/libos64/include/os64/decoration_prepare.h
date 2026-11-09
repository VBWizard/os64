#ifndef OS64_DECORATION_PREPARE_H
#define OS64_DECORATION_PREPARE_H
#include "os64/decoration.h"
#include "os64/font_provider.h"

/* Prepare an independently owned immutable bundle from a borrowed font role.
 * Scratch uses an arena; the result belongs to the ordinary heap and must be
 * released with os64_free. Failure clears outputs and retains the font role. */
os64_font_status_t os64_decor_prepare(const os64_font_role_view_t *,
    const os64_decor_header_t *style, void **bytes, size_t *length);
/* Copy embedded font assets into the current bundle version and regenerate
 * finishes using a full current recipe. Asset offsets follow the new header size.
 * The old bundle remains usable on failure; the result is ordinary heap storage. */
os64_font_status_t os64_decor_restyle(const void *,size_t,const os64_decor_header_t *,void **,size_t *);
void os64_decor_defaults(os64_decor_header_t *style);
/* Read the installed generation, or publish a complete validated bundle.
 * Publication success is acknowledged by the COMMIT write, not by close. */
int os64_decor_generation(uint64_t *generation);
/* Immutable per-open status; legacy kernels return a zero-length fingerprint. */
int os64_decor_current(os64_decor_status_t *);
int os64_decor_apply(const void *bytes, size_t length, uint64_t expected);
#endif
