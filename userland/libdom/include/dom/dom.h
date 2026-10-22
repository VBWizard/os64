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

/* The screen a page is told it is on (CSSOM View § The Screen interface),
 * in device pixels: the whole display, and what of it a window may take.
 * A page reads them in CSS pixels at its zoom. */
typedef struct {
    int32_t width, height, avail_width, avail_height;
    uint32_t color_depth;
} os64_dom_screen_t;

/* A page's navigation timing (Navigation Timing; performance.timing and the
 * `navigation` entry), as far as it has gone. The time origin is when the
 * navigation began: `origin_us` on the host's now_us clock and
 * `origin_epoch_us` on the wall clock, microseconds since the Unix epoch.
 * Each milestone is microseconds after the origin, OS64_DOM_TIMING_NOT_YET
 * until it has happened; `type` is how the navigation came about and
 * `redirects` how many redirects it followed. */
#define OS64_DOM_TIMING_NOT_YET UINT64_MAX
typedef enum {
    OS64_DOM_TIMING_FETCH_START = 0,
    OS64_DOM_TIMING_RESPONSE_START,
    OS64_DOM_TIMING_RESPONSE_END,
    OS64_DOM_TIMING_DOM_LOADING,
    OS64_DOM_TIMING_DOM_INTERACTIVE,
    OS64_DOM_TIMING_DOM_CONTENT_LOADED_START,
    OS64_DOM_TIMING_DOM_CONTENT_LOADED_END,
    OS64_DOM_TIMING_DOM_COMPLETE,
    OS64_DOM_TIMING_LOAD_START,
    OS64_DOM_TIMING_LOAD_END,
    OS64_DOM_TIMING_COUNT
} os64_dom_milestone_t;
typedef enum {
    OS64_DOM_NAVIGATE = 0,       /* a link, an address, a form, a script's location = */
    OS64_DOM_NAVIGATE_RELOADED,  /* Reload, or location.reload() */
    OS64_DOM_NAVIGATE_TRAVERSED  /* back or forward through the history */
} os64_dom_navigation_type_t;
typedef struct {
    uint64_t origin_us;
    int64_t origin_epoch_us;
    uint64_t at[OS64_DOM_TIMING_COUNT];
    os64_dom_navigation_type_t type;
    uint32_t redirects;
} os64_dom_timing_t;

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
    /* The host's microsecond clock, monotonic: performance.now() reads it
     * against the timing's origin. NULL reads now_ms's milliseconds. */
    uint64_t (*now_us)(void *opaque);
    /* The page's navigation timing as it stands, written into `out`, asked
     * as the binding is made (for the clock's origin) and whenever a script
     * reads it. It must not enter JS. NULL: the origin is the binding's
     * creation and no milestone has happened. */
    void (*timing)(void *opaque, os64_dom_timing_t *out);
    /* What `screen` reports, copied at creation. */
    os64_dom_screen_t screen;
    /* The page's zoom in thousandths, asked when devicePixelRatio is read:
     * a page zoomed to 125% is told 1.25, as Chrome tells it. NULL is
     * 1000. It must not enter JS. */
    uint32_t (*zoom)(void *opaque);
    void *host_opaque;
} os64_dom_options_t;

static inline os64_dom_options_t os64_dom_default_options(void)
{
    const os64_dom_options_t options = {
        .max_bytes = OS64_DOM_DEFAULT_MAX_BYTES, .scripting = true, .modern = true,
        .screen = {1024, 768, 1024, 768, 24}
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

/* WEB STORAGE (HTML § 12.2): localStorage and sessionStorage, each a map
 * of strings per origin, the page's origin being its address's scheme,
 * host and port. libdom holds the map a page sees; where an area is KEPT
 * is the host's, through a provider per area, and the store is the shared
 * state, not any one page's copy of it: another window on the same origin
 * may change it at any time. So the binding reads the store when a task
 * first touches the area and the store has moved since it last read it,
 * and hands back only what this page changed, never its whole map; the
 * host merges that onto the store as it then stands
 * (os64_dom_storage_merge). A page whose origin is opaque (about:blank, a
 * file, a gopher menu), or an area with no provider, keeps its map for its
 * own life and nowhere else.
 *
 * Strings keep every code unit a script gave them, a lone surrogate and
 * U+0000 included, as WTF-8; the store is bytes the host moves and does
 * not read. Browsers keep them so, WPT's webstorage holds a binding to it,
 * and pages that pack binary into UTF-16 strings (LZ-string's compressors)
 * read back garbage from a store that replaced them. An area holds at most OS64_DOM_STORAGE_QUOTA bytes, counted as
 * the standard's quota counts them: the UTF-16 code units of every key and
 * value, two bytes each, and at most 65536 keys. A setItem that would pass
 * either throws QuotaExceededError and changes nothing. */
#define OS64_DOM_STORAGE_QUOTA ((size_t)5 * 1024 * 1024)
/* Changes are handed over at most this often, and once more as the
 * binding is drained (the page departing: os64_dom_drain); sooner when the
 * changes not yet handed over would pass the quota or the keys' cap. */
#define OS64_DOM_STORAGE_COMMIT_MS 1000
typedef enum {
    OS64_DOM_STORAGE_LOCAL = 0,
    OS64_DOM_STORAGE_SESSION,
    OS64_DOM_STORAGE_AREAS
} os64_dom_storage_area_t;
typedef enum {
    OS64_DOM_STORAGE_OK = 0,
    /* The store and the changes together would pass the quota: nothing was
     * kept, and the page sees the store again. */
    OS64_DOM_STORAGE_FULL = -1,
    /* The store could not be read or written: the page's area keeps its
     * map for the page's life from here, and asks the provider nothing
     * more. */
    OS64_DOM_STORAGE_FAILED = -2,
    /* The store is another's for now (another window is committing to
     * it): nothing was kept, and the changes are handed over again at the
     * next commit, COMMIT_MS from now, or at drain's, whichever is first.
     * Drain's commit is the last: there it is the provider's to wait
     * longer (nothing on screen waits for it), and a BUSY it answers loses
     * the changes, which the provider reports. */
    OS64_DOM_STORAGE_BUSY = -3
} os64_dom_storage_status_t;
/* A provider, all on the owner thread, none entering JS; `origin` is the
 * serialized origin ("https://example.com"), borrowed for the call.
 * identity: a value that changes whenever the store does (yonder's: the
 * file's size and modification time), 0 for no store; asked at the first
 * touch in each task, and a value other than the one the binding last
 * loaded means the store moved and is loaded again.
 * load: the store's image, written into `out` when it fits in `cap`;
 * answers its length either way (0 for none), negative when it cannot be
 * read or is not an image (an empty file is not one). The binding asks again with room when it did not fit, a few
 * times; a store still outgrowing its room is read at the next task.
 * commit: this page's changes since its last commit (a change list in
 * os64_dom_storage_merge's format), merged onto the store as it stands.
 * `last` is drain's commit, after which there is none. */
typedef struct {
    uint64_t (*identity)(void *opaque, const char *origin);
    int64_t (*load)(void *opaque, const char *origin, void *out, size_t cap);
    os64_dom_storage_status_t (*commit)(void *opaque, const char *origin, const void *changes,
                                        size_t length, bool last);
    void *opaque;
} os64_dom_storage_t;
/* Copied; NULL removes the area's provider. Install before the first
 * script: an area already touched keeps what it has and reads the store
 * at its next touch. */
void os64_dom_set_storage(os64_dom_t *dom, os64_dom_storage_area_t area,
                          const os64_dom_storage_t *storage);
/* Between tasks: the task that just ran is over, so the next touch asks the
 * provider again, and an area with changes commits them when they are due:
 * at once when the area has not committed for OS64_DOM_STORAGE_COMMIT_MS
 * of the host's now_ms (or ever), else that long after its last commit.
 * os64_dom_drain commits what is left, due or not. A host that never
 * settles asks the provider once per area and commits at drain. */
void os64_dom_storage_settle(os64_dom_t *dom);
/* When the next commit is due on the now_ms clock: UINT64_MAX with nothing
 * to commit. */
uint64_t os64_dom_storage_due(const os64_dom_t *dom);
/* Forgets both areas' maps and their changes not yet committed, so their
 * next touch reads the store as it is: "Clear page storage" while a page
 * is open. Owner thread, outside callbacks. */
void os64_dom_storage_forget(os64_dom_t *dom);

/* THE STORE'S BYTES, libdom's format and the host's to move: an image is a
 * store's whole content, a change list is a commit's. Both are
 * "os64 storage 1\n" and then records, `+<key bytes> <value bytes>\n`
 * followed by the key, the value and "\n" for a set, `-<key bytes>\n`, the
 * key and "\n" for a removal (an image holds only sets). The merge applies
 * a change list to an image (NULL with length 0 for no store; anything
 * else, an empty file's bytes included, must be in the format) as it stands
 * and answers the new image in `*out` (os64_malloc'd; the caller frees it
 * with os64_free; NULL, length 0, for an image with nothing in it):
 * OS64_DOM_STORAGE_FULL when the result would hold more than the quota and
 * more than the image did, OS64_DOM_STORAGE_FAILED when either input is not
 * in the format or there was no memory. *out is NULL on any refusal. */
os64_dom_storage_status_t os64_dom_storage_merge(const void *image, size_t image_length,
                                                 const void *changes, size_t changes_length,
                                                 void **out, size_t *out_length);
/* Each record of an image or a change list, in order: `value` is NULL for a
 * removal. `each` answers false to stop. False when the bytes are not in
 * the format. */
bool os64_dom_storage_walk(const void *bytes, size_t length,
                           bool (*each)(void *opaque, const char *key, size_t key_length,
                                        const char *value, size_t value_length),
                           void *opaque);

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

/* Owner-thread only, outside JS callbacks. Idempotent; NULL is a no-op.
 * Drain closes bindings and releases C-retained JS values even when libjs
 * has entered a sticky failed state. It first commits each storage area's
 * changes to its provider, the last of a page's commits (`last`). Native
 * records remain alive through engine finalizers; os64_dom_free releases
 * their native node holds.
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
