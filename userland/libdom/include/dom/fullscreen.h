#ifndef OS64_DOM_FULLSCREEN_H
#define OS64_DOM_FULLSCREEN_H
#include <dom/dom.h>

/* THE FULLSCREEN API'S HOST (docs/design/pending/VIDEO.md § 9; the design
 * state's Part B). libdom keeps the API's half: requestFullscreen and
 * exitFullscreen and their promises, the fullscreen element and its
 * `:fullscreen` state bit, and the fullscreenchange and fullscreenerror
 * events. The host keeps the screen: it puts its window over the screen
 * (or not) and says so afterwards. No call below enters script, and every
 * one is made on the window thread.
 *
 * A document with no provider is not allowed full screen:
 * `document.fullscreenEnabled` is false and every request is refused. A
 * host gives one to the document whose window it owns and none to a
 * frame's, which is how a frame is kept out (allowfullscreen is not
 * honoured; DEBTS). */

typedef struct {
    void *opaque;
    /* True once, when the person's last activating input (a press, a key
     * other than Escape) came recently enough, which it then uses up:
     * HTML's transient activation, which a request needs. */
    bool (*consume_activation)(void *opaque);
    /* Put the window over the screen for `element`, of the document the
     * provider was given; the answer comes later, as
     * os64_dom_fullscreen_settled. */
    void (*enter)(void *opaque, const os64_html_node_t *element);
    /* Bring the window back; the answer comes later, as
     * os64_dom_fullscreen_exit. Also asked when the fullscreen element
     * leaves the document. */
    void (*leave)(void *opaque);
} os64_dom_fullscreen_provider_t;

#pragma GCC visibility push(default)
/* Install or replace (NULL removes) the host, outside callbacks. */
void os64_dom_set_fullscreen(os64_dom_t *dom, const os64_dom_fullscreen_provider_t *provider);
/* The host's answer to `enter`: the window covers the screen (`entered`),
 * or it was refused. The element becomes the fullscreen element, or the
 * request fails, and the events and the promise follow at the next view
 * step. */
void os64_dom_fullscreen_settled(os64_dom_t *dom, bool entered);
/* The window is no longer over the screen: the host's answer to `leave`,
 * or its own decision (Escape, the window manager's chord, a navigation's
 * departure, anything the host asks the person). Fully exits; the events
 * and any exitFullscreen promise follow at the next view step. */
void os64_dom_fullscreen_exit(os64_dom_t *dom);
/* The fullscreen element of the provider's document, or NULL: for the
 * host's paint, its hit-testing and its rows. */
const os64_html_node_t *os64_dom_fullscreen_element(const os64_dom_t *dom);
#pragma GCC visibility pop

#endif
