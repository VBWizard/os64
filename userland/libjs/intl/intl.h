#ifndef LIBJS_INTL_H
#define LIBJS_INTL_H
#include "os64/js_engine.h"

/* INTL.md: ECMA-402 for a realm. The script is self-hosted (intl.js, over
 * CLDR's data in intl_data.js); this is the C under it, private to libjs,
 * whose public door is os64_js_install_intl (runtime.c).
 *
 * The Intl object for `locale` (the realm's default locale, a BCP 47 tag),
 * made in `ctx`, or JS_EXCEPTION with the context's exception pending. It
 * must run before any other script in the realm: intl.js captures the
 * built-ins it calls as it loads. Nothing is defined on the global. */
JSValue js_intl_build(JSContext *ctx, const char *locale);

#endif
