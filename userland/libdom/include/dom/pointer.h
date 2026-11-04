#ifndef OS64_DOM_POINTER_H
#define OS64_DOM_POINTER_H
#include <dom/dom.h>
#pragma GCC visibility push(default)

typedef enum {
    OS64_DOM_POINTER_MOVE, OS64_DOM_POINTER_DOWN, OS64_DOM_POINTER_UP,
    OS64_DOM_POINTER_BOUNDARY, OS64_DOM_POINTER_CANCEL
} os64_dom_pointer_action_t;
/* One normalized native sample. Coordinates and modifiers are captured before
 * callbacks; buttons uses the DOM bit order. hit is borrowed for the call and
 * may be NULL outside the document. Boundary updates do not synthesize motion. */
typedef struct {
    os64_dom_pointer_action_t action;
    os64_dom_event_t event;
    const os64_html_node_t *hit;
} os64_dom_pointer_sample_t;
/* The browser's default follows a dispatched mouse/activation event, outside
 * listener execution but within the sample's task. It may update native
 * widgets; it must not start a nested script task. False abandons the packet.
 * node/event are borrowed for this call. */
typedef bool (*os64_dom_pointer_default_t)(void *opaque, const os64_html_node_t *node,
                                          const os64_dom_event_t *event, bool prevented);
/* ran reports whether a script task was opened; quiet hover still updates
 * native state and its default without creating a script task. */
os64_js_status_t os64_dom_pointer_dispatch(os64_dom_t *, const os64_dom_pointer_sample_t *,
                                            os64_dom_pointer_default_t, void *, bool *ran, os64_js_outcome_t *);
/* A host interruption is recorded without allocating or calling script. The
 * next dispatch services it before considering a later sample. */
void os64_dom_pointer_cancel(os64_dom_t *);
bool os64_dom_pointer_active(const os64_dom_t *);
#pragma GCC visibility pop
#endif
