#ifndef LIBDOM_BROWSING_CONTEXT_H
#define LIBDOM_BROWSING_CONTEXT_H

#include <dom/dom.h>

typedef struct DBrowsingContext DBrowsingContext;
#define D_CONTEXT_NAME_MAX 255

/* Private owner-thread context registry. Records, copied names and tombstones
 * live until cluster teardown. Binding admission and context publication are
 * separate: prepare an origin-bearing child, then publish it at a safe point.
 * Refusal does not consume that candidate; the host may retry or depart it.
 * No WindowProxy or script-visible tree is supplied by this interface. */
__attribute__((visibility("hidden")))
DBrowsingContext *d_browsing_context_create(os64_dom_cluster_t *, DBrowsingContext *parent,
    os64_dom_t *initial, const char *name, os64_js_outcome_t *);
/* Expected generation prevents a stale native request from switching a newer
 * document. Replacement discards descendant contexts owned by the old document.
 * Success retires old realms and commits native identity without host callbacks.
 * The host must finish departure before releasing old provider storage. */
__attribute__((visibility("hidden")))
os64_js_status_t d_browsing_context_replace(DBrowsingContext *, uint64_t expected,
    os64_dom_t *candidate, os64_js_outcome_t *);
__attribute__((visibility("hidden")))
os64_js_status_t d_browsing_context_discard(DBrowsingContext *, uint64_t expected,
    os64_js_outcome_t *);
/* A generation-qualified target is usable only while its binding is active.
 * A returned pointer is borrowed, not a lease across native/engine reentry. */
__attribute__((visibility("hidden")))
os64_dom_t *d_browsing_context_target(const DBrowsingContext *, uint64_t expected);
__attribute__((visibility("hidden")))
uint64_t d_browsing_context_generation(const DBrowsingContext *);
__attribute__((visibility("hidden")))
DBrowsingContext *d_browsing_context_parent(const DBrowsingContext *);
__attribute__((visibility("hidden")))
DBrowsingContext *d_browsing_context_top(const DBrowsingContext *);
__attribute__((visibility("hidden")))
DBrowsingContext *d_browsing_context_child(const DBrowsingContext *, size_t index);
__attribute__((visibility("hidden")))
const char *d_browsing_context_name(const DBrowsingContext *);

#endif
