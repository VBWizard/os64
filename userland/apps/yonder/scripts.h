#ifndef YONDER_SCRIPTS_H
#define YONDER_SCRIPTS_H

#include <dom/dom.h>

typedef struct yonder_scripts yonder_scripts_t;

/* Finished-document integration fixture, not parser execution order. Queue
 * entries are native nodes, never engine values. Document and shared state
 * outlive this owner-thread object. Template contents, src and non-classic
 * scripts are excluded. The queue is bounded to 4096 entries. */
yonder_scripts_t *yonder_scripts_new(os64_html_document_t *doc,
                                     os64_page_state_t *state, const char *url,
                                     void (*alert)(void *, const char *, size_t),
                                     void *opaque);
bool yonder_scripts_pending(const yonder_scripts_t *scripts);
/* Runs at most one queued script, including its budgeted Promise jobs.
 * Exceptions permit later scripts; terminal runtime failures drop the queue.
 * False means no runnable script remained. Outcome has inline diagnostics. */
bool yonder_scripts_step(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
/* Drops queued nodes, drains bindings, destroys the engine, then native records.
 * NULL is a no-op; call outside script before widgets/model/document teardown. */
void yonder_scripts_free(yonder_scripts_t *scripts);

#endif
