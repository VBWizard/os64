#ifndef OS64_DOM_H
#define OS64_DOM_H

#include <html/html.h>
#include <page/page.h>
#include <os64/js.h>
#include <os64/url.h>

typedef struct os64_dom os64_dom_t;

#define OS64_DOM_DEFAULT_MAX_BYTES ((size_t)8 * 1024 * 1024)
/* The longest address a navigation ask carries: os64_url_absolute's. */
#define OS64_DOM_URL_MAX OS64_URL_REF_MAX
/* Live timers per page; one more throws QuotaExceededError. */
#define OS64_DOM_TIMERS_MAX 4096

/* What a script asked the host to do to a node, after its task. These reach
 * widgets and the model, which a script's task must not. */
typedef enum {
    OS64_DOM_ACTIVATE_FOCUS = 1,
    OS64_DOM_ACTIVATE_BLUR,
    OS64_DOM_ACTIVATE_RESET     /* a form whose reset event was not cancelled */
} os64_dom_activation_t;

typedef struct {
    /* Covers binding-owned records, strings and live-query storage, including
     * staged allocations. Engine, document and control-state budgets remain
     * separately enforced by their owning libraries. Zero is a literal cap. */
    size_t max_bytes;
    bool scripting;
    /* The Modern Web arc's surface (MODERN_WEB.md § The rules that keep the
     * other 99% safe): set, every row of modern.c's table installs its
     * globals at creation; clear, none of the names the table installs
     * exists on window, so a page's feature detection takes the path it
     * takes where they are missing. What goes is the table's names and
     * what its rows put on them (each prototype's `constructor` and
     * Symbol.toStringTag, Node's constants); the prototype chain's levels,
     * which the arc corrected (HTMLElement's prototype between Element's
     * and the HTML elements'), are the same either way. */
    bool modern;
    /* The UTF-8 message is borrowed during this owner-thread call. The host
     * copies it if needed and must not enter JS or a nested event loop here. */
    void (*alert)(void *opaque, const char *utf8, size_t length);
    void *alert_opaque;
    /* The page's address, copied: location reads it and resolves against it.
     * NULL reads as about:blank and resolves only absolute references. */
    const char *url;
    /* The host's millisecond clock, for timers and timeStamp. NULL is a clock
     * that reads zero. */
    uint64_t (*now_ms)(void *opaque);
    /* Notices, each on the owner thread from inside a script's task. The
     * host records them and acts after the task; it must not enter JS here.
     * script_connected: a verb connected a classic script element that had
     * not started, which the binding has now marked started (NULL: nothing is
     * marked and nothing is told). activate: an ask that reaches widgets.
     * Both pass borrowed nodes; the host holds what it keeps. */
    void (*script_connected)(void *opaque, const os64_html_node_t *script);
    void (*activate)(void *opaque, const os64_html_node_t *node, os64_dom_activation_t what);
    /* document.write's text, `length` bytes of UTF-8 borrowed for the call,
     * for the host to hand to its parser (os64_html_parser_write): it answers
     * libhtml's status. OS64_HTML_BAD_ARGUMENT means there is no insertion
     * point, and the script gets InvalidStateError; anything else is not the
     * script's business. An empty write asks without writing (open()). NULL
     * is a host with no parser: every write is BAD_ARGUMENT. */
    int64_t (*write)(void *opaque, const char *utf8, size_t length);
    void *host_opaque;
} os64_dom_options_t;

static inline os64_dom_options_t os64_dom_default_options(void)
{
    const os64_dom_options_t options = {
        .max_bytes = OS64_DOM_DEFAULT_MAX_BYTES, .scripting = true, .modern = true
    };
    return options;
}

typedef enum {
    OS64_DOM_EVENT_PLAIN = 0,
    OS64_DOM_EVENT_MOUSE,
    OS64_DOM_EVENT_KEY
} os64_dom_event_kind_t;

/* An event the host dispatches. Strings are borrowed for the call. Mouse
 * fields are read for MOUSE, key fields for KEY; related is mouseover's and
 * mouseout's other node, or NULL. */
typedef struct {
    const char *type;
    bool bubbles, cancelable;
    os64_dom_event_kind_t kind;
    int32_t client_x, client_y, screen_x, screen_y;
    int32_t button;
    const os64_html_node_t *related;
    const char *key;
    uint32_t key_code, char_code;
    bool shift, ctrl, alt, meta;
    // Optional CSS-page coordinates captured with the event, before callbacks.
    bool page_coordinates;
    int32_t page_x, page_y;
} os64_dom_event_t;

typedef enum {
    OS64_DOM_NAVIGATE_NONE = 0,
    OS64_DOM_NAVIGATE_URL,       /* location.href =, assign(), hash = */
    OS64_DOM_NAVIGATE_REPLACE,   /* location.replace() */
    OS64_DOM_NAVIGATE_RELOAD,
    OS64_DOM_NAVIGATE_HISTORY,   /* history.back/forward/go: delta */
    OS64_DOM_NAVIGATE_FOLLOW,    /* a link's click() not cancelled: node */
    OS64_DOM_NAVIGATE_SUBMIT     /* form.submit(), or a submit button's click()
                                  * and submit event not cancelled: node is the
                                  * form, submitter the button or NULL */
} os64_dom_navigation_kind_t;

/* The last navigation a script asked for. Nodes arrive HELD: the taker
 * releases them with os64_html_release when it is done with them. */
typedef struct {
    os64_dom_navigation_kind_t kind;
    const os64_html_node_t *node, *submitter;
    int32_t delta;
    char url[OS64_DOM_URL_MAX];
} os64_dom_navigation_t;

typedef enum {
    OS64_DOM_SCRIPT_NONE = 0,   /* not a script element, or a type nothing runs */
    OS64_DOM_SCRIPT_CLASSIC,
    OS64_DOM_SCRIPT_MODULE      /* recognised, never run */
} os64_dom_script_kind_t;

/* Numeric snapshot in CSS pixels. Offset/client members are integer pixels;
 * rect members can be fractional after device-pixel layout is unzoomed.
 * offset_parent is borrowed from the provider's live document. */
typedef struct {
    double x, y, width, height;
    int32_t offset_left, offset_top, offset_width, offset_height;
    int32_t client_left, client_top, client_width, client_height;
    const os64_html_node_t *offset_parent;
    uint64_t layouts, elapsed_us;
    int32_t scroll_left, scroll_top;
} os64_dom_geometry_t;

typedef bool (*os64_dom_geometry_provider_t)(void *opaque,
    const os64_html_node_t *node, os64_dom_geometry_t *out);

typedef struct { uint64_t layouts, elapsed_us; } os64_dom_geometry_stats_t;

#pragma GCC visibility push(default)

/* Installs one document into a fresh, idle runtime before evaluation. All
 * three handles are borrowed; state must belong to document. NULL options
 * select the defaults; outcome is required. No runtime/context opaque slot
 * is used. Console output is granted separately by os64_js_install_output.
 *
 * NULL reports failure in outcome. Registration may have published engine
 * objects: destroy that runtime without further evaluation before retrying
 * with a new one. Failed construction clears native opaque pointers and
 * releases its retained engine values before freeing its native storage.
 * This idle installer cannot distinguish engine-cap exhaustion from host
 * allocation failure; evaluation's sticky-limit classification is separate.
 *
 * Native document/control/binding quota refusals throw QuotaExceededError.
 * Invalid tree placement throws HierarchyRequestError; a child/reference
 * mismatch throws NotFoundError; invalid names throw InvalidCharacterError.
 * Wrong receivers and native argument/text errors throw TypeError. During
 * evaluation, engine exhaustion follows libjs's sticky LIMIT/MEMORY contract,
 * not a resumable DOM exception. Retired bindings throw InvalidStateError.
 *
 * Wrappers hold their native nodes and retain identity and expandos through
 * detach/reinsert and GC. Collection roots and cached answers also hold nodes;
 * successful refresh releases the previous answer after holding its successor.
 * Collections are live objects exposing length, item(index) and indexed
 * access. Child collections omit template contents; innerHTML uses them.
 * Strings replace NUL and lone UTF-16 surrogates with U+FFFD. HTML names are
 * ASCII-folded where their DOM operation requires it.
 * Supported properties/methods live on kind-specific prototype chains;
 * a property absent from a node's chain reads undefined and is absent from in.
 *
 * Content setters stage detached replacements and preflight their insertion
 * before removing current children. A refusal preserves visible children;
 * detached staging remains document-owned until reclamation. Tree methods
 * prepare their return wrappers before making a visible change. Cloning
 * preserves INPUT current value/checkedness and dirty flags, and TEXTAREA
 * current value/dirty flag; other controls initialize from cloned markup. */
os64_dom_t *os64_dom_create(os64_js_runtime_t *runtime,
                            os64_html_document_t *document,
                            os64_page_state_t *state,
                            const os64_dom_options_t *options,
                            os64_js_outcome_t *outcome);

/* Owner-thread browser identity provider, borrowed text for the getter call.
 * NULL restores the native Yonder identity; install/replace outside callbacks. */
void os64_dom_set_user_agent(os64_dom_t *dom, const char *(*provider)(void *opaque), void *opaque);

/* Owner-thread cookie provider for document.cookie (HTML § Cookies): `get`
 * writes the cookie-string the page's script may read into `out`, at most
 * `cap` bytes with the NUL, answers the length written, and sets `*whole`
 * false when more did not fit — the read then asks again with more room;
 * `set` hears one string a script assigned, as a Set-Cookie value. With no
 * provider the document is cookie-averse: it reads "" and an assignment
 * does nothing. The callbacks must not enter JS. NULL removes it; install
 * outside callbacks. */
typedef struct {
    size_t (*get)(void *opaque, char *out, size_t cap, bool *whole);
    void (*set)(void *opaque, const char *text, size_t length);
    void *opaque;
} os64_dom_cookies_t;
void os64_dom_set_cookies(os64_dom_t *dom, const os64_dom_cookies_t *cookies);

/* Owner-thread census hook (YONDER_DIAGNOSTICS.md): `heard` is told each
 * name a lookup on window did not find, before the lookup answers as it
 * always does, so what the page sees is unchanged. The name is borrowed
 * for the call; `heard` must not enter JS. NULL stops it. Install outside
 * callbacks. */
void os64_dom_set_global_miss(os64_dom_t *dom, void (*heard)(void *opaque, const char *name),
                              void *opaque);

/* Install/replace the native provider outside script callbacks. A provider
 * ensures a current layout, copies a snapshot and records attempted layouts
 * and elapsed work even on refusal. It must not enter JS or pump events.
 * NULL removes it; subsequent HTML geometry reads refuse. Non-HTML elements
 * return zero snapshots without a provider. Existing options ABI is unchanged.
 * Browser owners install this before their first script. */
void os64_dom_set_geometry(os64_dom_t *dom, os64_dom_geometry_provider_t provider,
                            void *opaque);
/* Owner-thread counters for forced native layouts and elapsed time. Reset at
 * the start of a host task; records survive script exceptions/failed reads. */
os64_dom_geometry_stats_t os64_dom_geometry_stats(os64_dom_t *dom, bool reset);

/* THE VIEW (CSSOM View § Extensions to the Window interface; MODERN_WEB.md
 * § Slice 1b): what the window shows the page through, in CSS pixels. All
 * of it is the Modern Web arc's surface: with options.modern clear none of
 * the names exists and nothing below dispatches. */
typedef struct {
    /* The viewport: innerWidth and innerHeight, and the width and height a
     * media query reads. */
    int32_t width, height;
    /* The window's frame: outerWidth and outerHeight. */
    int32_t outer_width, outer_height;
    /* Where the page is scrolled to: scrollX and scrollY. */
    double scroll_x, scroll_y;
    /* What prefers-color-scheme and prefers-reduced-motion answer. */
    bool dark, reduced_motion;
} os64_dom_view_t;

typedef struct {
    /* The view as it is now: no layout. */
    void (*read)(void *opaque, os64_dom_view_t *out);
    /* scrollTo's door: the page's layout made current, then the view
     * scrolled to (x, y), clamped as the host clamps any scroll; what the
     * layout cost goes in `cost`, which the binding adds to its geometry
     * counts (os64_dom_geometry_stats). False when no current layout could
     * be made: the script sees InvalidStateError. */
    bool (*scroll)(void *opaque, double x, double y, os64_dom_geometry_stats_t *cost);
    void *opaque;
} os64_dom_view_provider_t;
/* Copied; NULL removes it, after which the view reads as zeros and a
 * scroll moves nothing. Both callbacks are owner-thread and must not enter
 * JS or pump events. The view is read once now, so the first
 * os64_dom_view_step compares against the view as it stood here. Install
 * outside callbacks. */
void os64_dom_set_view(os64_dom_t *dom, const os64_dom_view_provider_t *view);
/* THE VIEW'S EVENTS (HTML § Update the rendering, the steps CSSOM View
 * adds), at most ONE per call, as one task: `resize` at window when the
 * view's size moved since the last step; else `scroll` at the document,
 * bubbling to window, when the scroll moved; else `change` at the first
 * MediaQueryList, in the order the page made them, whose `matches` moved,
 * as a MediaQueryListEvent. A change nothing could hear is noted and
 * passed over without a task. True when one ran, its outcome filled;
 * false when nothing was owed, the binding has no view, or modern is off.
 * Owner thread, between tasks: the host calls it until it answers false. */
bool os64_dom_view_step(os64_dom_t *dom, os64_js_outcome_t *outcome);

/* GETCOMPUTEDSTYLE (CSSOM § getComputedStyle): the provider writes an
 * element's resolved style, one text per property of libgarb's table
 * (garb_prop_t is `prop`; the table is the list, and a declaration's
 * length, order and names are the table's), each as CSSOM serialises it.
 * The binding asks for the whole element at every read, so a declaration a
 * script holds is live. */
typedef struct os64_dom_computed os64_dom_computed_t;
/* The provider makes the element's style current (a layout, when it was
 * stale), puts every property it knows, and answers true; an element with
 * no style (not in the document's tree) puts nothing and answers true. False
 * when no current style could be made: the script sees InvalidStateError.
 * Must not enter JS or pump events. */
typedef bool (*os64_dom_computed_provider_t)(void *opaque, const os64_html_node_t *element,
                                            os64_dom_computed_t *out);
/* The provider's pen: property `prop` is `length` bytes of UTF-8 `value`,
 * copied. A property not put reads "". False when the binding could not
 * keep it (its budget), which the provider answers as its own false. */
bool os64_dom_computed_put(os64_dom_computed_t *out, int32_t prop, const char *value, size_t length);
/* What making the style current cost, added to os64_dom_geometry_stats. */
void os64_dom_computed_cost(os64_dom_computed_t *out, uint64_t layouts, uint64_t elapsed_us);
/* NULL removes it; a script's getComputedStyle then throws
 * InvalidStateError for an element and reads "" for a pseudo-element.
 * Install outside callbacks. */
void os64_dom_set_computed(os64_dom_t *dom, os64_dom_computed_provider_t provider, void *opaque);

/* HISTORY (HTML § The History interface; MODERN_WEB.md § Slice 1b): the
 * session's entries are the host's, and a script reaches them through
 * these. The state a script leaves with an entry is JSON text: what JSON
 * can carry of a structured clone (null, booleans, finite numbers but -0,
 * strings, arrays without holes and plain objects of them; `undefined` in
 * an object is left out, as JSON leaves it, and in an array it is null),
 * anything else a DataCloneError, and at most OS64_DOM_STATE_MAX bytes, a
 * QuotaExceededError past that. */
#define OS64_DOM_STATE_MAX ((size_t)64 * 1024)
typedef struct {
    /* pushState (push) or replaceState: the session's current entry
     * becomes `url` (absolute, and a URL the document may rewrite its own
     * to, which the binding has checked) with `state`, `length` bytes of
     * JSON (NULL for null; copied), and when `push` the entry that was
     * current stays behind it. No event, whatever the fragment did. False
     * when the host could not (no memory): QuotaExceededError, and nothing
     * changed. */
    bool (*update)(void *opaque, const char *url, const char *state, size_t length, bool push);
    /* A fragment navigation the document's script asked for (location.hash
     * =, or an address that differs from the document's only in its
     * fragment): the host adds the entry `url` (replaces the current one
     * when `replace`), scrolls to its fragment, and owes popstate and
     * hashchange (os64_dom_history_event). The binding has already made
     * `url` the document's address and null its state. False when the host
     * could not: the address is put back. */
    bool (*fragment)(void *opaque, const char *url, bool replace);
    /* history.length: the entries in the session. */
    uint32_t (*length)(void *opaque);
    void *opaque;
} os64_dom_history_t;
/* Copied; NULL removes it, after which pushState and replaceState change
 * only the document's own address and state, a fragment navigation is an
 * ask (os64_dom_take_navigation), and history.length reads 1. The
 * callbacks are owner-thread, inside a script's task: they must not enter
 * JS. Install outside callbacks. */
void os64_dom_set_history(os64_dom_t *dom, const os64_dom_history_t *history);
/* The session moved to another entry of this document without a fetch: a
 * traversal (Back, Forward, history.go) or a fragment navigation the host
 * began (a link to `#x`). The document's address becomes `url`, its state
 * `length` bytes of the JSON the host kept (NULL for null), read the next
 * time history.state is. Nothing is dispatched: the host follows with
 * os64_dom_history_event. False on no memory, nothing changed. Owner
 * thread, outside JS. */
bool os64_dom_history_moved(os64_dom_t *dom, const char *url, const char *state, size_t length);
/* `popstate` at window, its state history.state's (`hashchange` false), or
 * `hashchange` from `old_url` to the document's address now, each as one
 * task. OK with nothing run when modern is off or nothing could hear it. */
os64_js_status_t os64_dom_history_event(os64_dom_t *dom, bool hashchange, const char *old_url,
                                        os64_js_outcome_t *outcome);
/* Whether the page set history.scrollRestoration to "manual": the host
 * then leaves the scroll where it is when it traverses to another entry
 * of this document. */
bool os64_dom_history_manual(const os64_dom_t *dom);

/* Owner-thread only, outside JS callbacks. Idempotent; NULL is a no-op.
 * Drain closes bindings and releases C-retained JS values even when libjs
 * has entered a sticky failed state. Native records remain alive through
 * engine finalizers; os64_dom_free releases their native node holds.
 * The required order is drain, os64_js_destroy (or destroy_report), then
 * os64_dom_free, followed by model/state/document teardown. */
void os64_dom_drain(os64_dom_t *dom);
void os64_dom_free(os64_dom_t *dom);

/* Native bytes remain measurable after drain. Registry count is the number
 * of C-retained engine values; it becomes zero on drain. NULL answers zero. */
size_t os64_dom_bytes(const os64_dom_t *dom);
size_t os64_dom_registry_count(const os64_dom_t *dom);

/* EVENTS. The host dispatches at a node, or at window when node is NULL
 * (`load`, whose target reads as the document). One TASK: libdom opens it,
 * builds the Event, runs each listener as one os64_js_call followed by a
 * microtask checkpoint, and closes it. A listener that throws is reported and
 * the next one runs; a sticky failure ends the dispatch. The outcome is the
 * task's: its first exception or unhandled rejection, else a reported error
 * from a nested dispatch, else OK; a sticky status means the runtime is
 * finished with. *prevented answers whether the default action is cancelled.
 * Owner thread, between tasks. Costs no engine entry when listens() says no. */
os64_js_status_t os64_dom_dispatch(os64_dom_t *dom, const os64_html_node_t *node,
                                   const os64_dom_event_t *event, bool *prevented,
                                   os64_js_outcome_t *outcome);
/* Whether anything on the page could hear `type`: a listener, a handler
 * property, or an on<type> content attribute in the document. O(1) unless the
 * tree moved since the last answer, when the attributes are counted again. A
 * type outside the ones the host dispatches is not counted and answers true. */
bool os64_dom_listens(os64_dom_t *dom, const char *type);
/* Which event types the document's on<type> content attributes name, as a
 * mask os64_dom_handler_bit reads: the walk os64_dom_listens counts with.
 * It needs no runtime, so a host can ask before making one. */
uint32_t os64_dom_handler_attributes(const os64_html_document_t *doc);
/* The bit for `type` in that mask; 0 for a type no attribute can name
 * (DOMContentLoaded, or one outside the types the host dispatches). */
uint32_t os64_dom_handler_bit(const char *type);

/* TIMERS, in the host's clock (options.now_ms). next answers the earliest due
 * time, or UINT64_MAX with none. fire runs AT MOST ONE timer due at or before
 * now, as one task, and answers whether it ran one (outcome filled). An
 * interval is re-armed before its callback, so a callback that clears it
 * wins. */
uint64_t os64_dom_timer_next(const os64_dom_t *dom);
bool os64_dom_timer_fire(os64_dom_t *dom, uint64_t now, os64_js_outcome_t *outcome);

/* The navigation a script asked for since the last take, if any: the last
 * ask wins. False when there is none. */
bool os64_dom_take_navigation(os64_dom_t *dom, os64_dom_navigation_t *ask);
/* An error a nested dispatch or a handler compile reported inside a task the
 * host did not open through libdom (a script run with os64_js_run). The
 * first one is kept until taken. False when there is none. */
bool os64_dom_take_report(os64_dom_t *dom, os64_js_outcome_t *outcome);

/* SCRIPT ELEMENTS. kind follows HTML's "prepare the script element" type
 * rules (type, else language, the sixteen JavaScript MIME essences, module);
 * it reads attributes only and works without a binding. start marks a script
 * ALREADY STARTED: 1 when it was not, 0 when it was, -1 when the binding
 * could not record it. A script parsed by innerHTML, or copied from a started
 * one, is born started; a started script never runs again wherever it moves. */
os64_dom_script_kind_t os64_dom_script_kind(const os64_html_node_t *node);
int os64_dom_script_start(os64_dom_t *dom, const os64_html_node_t *node);

/* A reference a script names, resolved exactly as a link in the document
 * would be: os64_page_resolve_in, so the input cleaning, the `<base href>`
 * as the tree stands NOW, the fallbacks and the canonical spelling are
 * libpage's, not a copy of them. False when it will not resolve or does
 * not fit in cap. Works without a binding. */
bool os64_dom_resolve(const os64_html_document_t *document, const char *document_url,
                      const char *reference, char *out, size_t cap);

#pragma GCC visibility pop
#endif
