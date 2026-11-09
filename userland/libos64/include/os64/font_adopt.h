#ifndef OS64_FONT_ADOPT_H
#define OS64_FONT_ADOPT_H

#include "os64/font_provider.h"
#define OS64_FONT_CONSUMERS_MAX 16u

typedef struct {
    void *user;
    /* Prepare owns cleanup on failure and returns a non-NULL plan on success.
     * It stages replacement state without changing the active consumer. Each
     * successful plan retains candidate if commit will keep it after this call. */
    os64_font_status_t (*prepare)(void *user, os64_font_set_t *candidate, void **plan);
    /* Optional final fallible operation, e.g. PTY resize. Failure must leave
     * external state unchanged. At most one barrier is allowed in a batch. */
    os64_font_status_t (*barrier)(void *user, void *plan);
    /* Commit consumes a prepared plan and cannot fail or allocate. Abort also
     * consumes it. Neither callback may pump events or reenter adoption. */
    void (*commit)(void *user, void *plan);
    void (*abort)(void *user, void *plan);
} os64_font_consumer_t;

/* Validate -> prepare in order -> optional barrier -> commit in order. On
 * failure abort prepared plans in reverse order and set failed_index (SIZE_MAX
 * for batch validation/retention failure). Success also sets SIZE_MAX. Candidate
 * is borrowed and retained around callbacks. Serialize the complete batch with
 * document/window/font mutations; publication and session generations are F5's
 * responsibility. A batch has 1..16 consumers with distinct user pointers. */
os64_font_status_t os64_font_adopt(os64_font_set_t *candidate,
    const os64_font_consumer_t *consumers, size_t count, size_t *failed_index);
#endif
