#ifndef LIBJS_INTL_H
#define LIBJS_INTL_H
#include "os64/js_engine.h"

/* INTL.md: ECMA-402 for a realm. The script is self-hosted (intl.js, over
 * CLDR's data in intl_data.js); this is the C under it, private to libjs,
 * whose public door is os64_js_install_intl (runtime.c).
 *
 * The Intl object for `locale` (the realm's default locale, a BCP 47 tag),
 * made in `ctx`, or JS_EXCEPTION with the context's exception pending (a
 * locale the data does not hold among its causes). It must run before any
 * page script in the realm: intl.js captures the built-ins it calls as it
 * loads. Nothing is defined on the global; as its last step the script
 * redefines the built-ins ECMA-402 redefines (toLocaleString and its kin,
 * localeCompare) on the realm's prototypes. */
JSValue js_intl_build(JSContext *ctx, const char *locale);

#endif
