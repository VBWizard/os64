#ifndef OS64_DOM_H
#define OS64_DOM_H

#include <html/html.h>
#include <page/page.h>
#include <os64/js.h>

typedef struct os64_dom os64_dom_t;

#define OS64_DOM_DEFAULT_MAX_BYTES ((size_t)8 * 1024 * 1024)
/* The longest address a navigation ask carries: os64_url_absolute's. */
#define OS64_DOM_URL_MAX 2048
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
    void *host_opaque;
} os64_dom_options_t;

static inline os64_dom_options_t os64_dom_default_options(void)
{
    const os64_dom_options_t options = {
        OS64_DOM_DEFAULT_MAX_BYTES, true, NULL, NULL, NULL, NULL, NULL, NULL, NULL
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

/* Owner-thread only, outside JS callbacks. Idempotent; NULL is a no-op.
 * Drain closes bindings and releases C-retained JS values even when libjs
 * has entered a sticky failed state. Native records remain alive through
 * engine finalizers; os64_dom_free releases their native node holds.
 * The required order is drain, os64_js_destroy, then
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

/* A relative reference resolved as the document resolves its own links
 * (libpage's os64_page_url_absolute, file:/// included): against the first
 * `base` with an href, read from the tree as it is NOW, else against
 * document_url. A base that will not resolve is no base, and an opaque
 * one (`mailto:`) is a base nothing relative resolves against; an empty
 * reference names the base without its fragment; an absolute reference is
 * returned as written. False when nothing resolves or the answer does not
 * fit in cap. Works without a binding. */
bool os64_dom_resolve(const os64_html_document_t *document, const char *document_url,
                      const char *reference, char *out, size_t cap);

#pragma GCC visibility pop
#endif
