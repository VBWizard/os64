#ifndef OS64_DOM_H
#define OS64_DOM_H

#include <html/html.h>
#include <page/page.h>
#include <os64/js.h>
#include <os64/url.h>
#include <way/origin.h>

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
 * `redirects` Navigation Timing's redirectCount: the redirects it followed
 * when every one stayed on the origin, 0 when there were none or one did
 * not. */
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

/* A document's readiness (HTML § Current document readiness), what
 * document.readyState reads. */
typedef enum {
    OS64_DOM_LOADING = 0,
    OS64_DOM_INTERACTIVE,
    OS64_DOM_COMPLETE
} os64_dom_readiness_t;

/* Last-Modified's absence (os64_dom_options_t.last_modified). */
#define OS64_DOM_NO_DATE INT64_MIN

/* What a script asked the host to do to a node, after its task. These reach
 * widgets and the model, which a script's task must not. */
typedef enum {
    OS64_DOM_ACTIVATE_FOCUS = 1,
    OS64_DOM_ACTIVATE_BLUR,
    OS64_DOM_ACTIVATE_RESET,    /* a form whose reset event was not cancelled */
    /* A control whose constraint problem is reported to the person
     * (reportValidity, or a submission constraint validation stopped):
     * focus it and say os64_dom_report_message's sentence. */
    OS64_DOM_ACTIVATE_REPORT
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
     * script_connected: a verb connected a script element of a kind that
     * runs (os64_dom_script_kind: classic, module or import map) that had
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
    /* A rule the binding keeps was found broken, `sentence` (static text)
     * naming it: an Attr's attribute left its element by a verb outside
     * the binding (attr.c's backstop). On the owner thread, inside a
     * script's task; the host records it and must not enter JS. NULL: no
     * one is told, and the binding carries on as the sentence says. */
    void (*broken)(void *opaque, const char *sentence);
    /* A failure of the task being run past the one its outcome carries: an
     * unhandled rejection (`message` begins "unhandled rejection: "), or
     * an error the binding caught and reported itself (a listener's), of a
     * task whose outcome already holds a failure, which the host records
     * as it records the outcome's, under `task`, the task's source name.
     * A task's first sixteen such rejections are told one by one, the rest as
     * one line as the task closes, `and <n> more unhandled rejections`:
     * a page can leave hundreds of thousands in one task, and what each
     * costs the host must not be its to size (report.c's
     * D_REJECTIONS_TOLD). On the owner thread, between a task's calls or
     * as it closes; it must not enter JS. NULL: only the outcome's
     * failure is known. `stack` is the failure's as an outcome's
     * stack_trace is, "" when it has none. */
    void (*failed)(void *opaque, const char *task, const char *message, const char *stack);
    /* crypto.getRandomValues' and randomUUID's randomness: `length` bytes
     * (never more than 65,536) written into `out` from the host's
     * cryptographically strong source, true when every one was. False is
     * a refusal the page is thrown as an OperationError; what the hook
     * wrote then is never handed to the page. On the owner thread, inside
     * a script's task; it must not enter JS. NULL refuses every ask. */
    bool (*entropy)(void *opaque, uint8_t *out, size_t length);
    /* P1-Da, the document's state (WINDOW_DOCUMENT.md § P1-Da). Each is
     * the window's document's; a document of its own answers what HTML
     * gives a document with no browsing context.
     * readiness: what the document's readiness is as the binding is made;
     * from then on the host's own DOMContentLoaded and load dispatches
     * move it (os64_dom_readiness_after). A binding made during the parse
     * is LOADING, the zero value; one made after the page's load, COMPLETE.
     * referrer: document.referrer, the Referer the document's own request
     * carried (NULL or "" for none), copied.
     * last_modified: the response's Last-Modified, seconds since the Unix
     * epoch, OS64_DOM_NO_DATE when it said none (document.lastModified
     * then reads the time it is asked, as HTML says).
     * focus: what has focus, asked at every read of document.activeElement
     * and hasFocus(): the element focused in the window's document, or
     * NULL, into *focused (borrowed for the call), answering whether the
     * document has the system's focus. On the owner thread, inside a
     * script's task; it must not enter JS. NULL: nothing has focus, and
     * neither has the document. */
    os64_dom_readiness_t readiness;
    const char *referrer;
    int64_t last_modified;
    bool (*focus)(void *opaque, const os64_html_node_t **focused);
    void *host_opaque;
} os64_dom_options_t;

static inline os64_dom_options_t os64_dom_default_options(void)
{
    const os64_dom_options_t options = {
        .max_bytes = OS64_DOM_DEFAULT_MAX_BYTES, .scripting = true, .modern = true,
        .screen = {1024, 768, 1024, 768, 24}, .last_modified = OS64_DOM_NO_DATE
    };
    return options;
}

/* Which interface a host event is, with the Modern Web switch on: an
 * Event, a MouseEvent, a KeyboardEvent, a FocusEvent, an InputEvent (a
 * person's edit of a text control), a SubmitEvent. With it off every one is
 * an Event, MOUSE and KEY carrying their fields as data properties. */
typedef enum {
    OS64_DOM_EVENT_PLAIN = 0,
    OS64_DOM_EVENT_MOUSE,
    OS64_DOM_EVENT_KEY,
    OS64_DOM_EVENT_FOCUS,
    OS64_DOM_EVENT_INPUT,
    OS64_DOM_EVENT_SUBMIT
} os64_dom_event_kind_t;

/* An event the host dispatches. Strings are borrowed for the call. Mouse
 * fields are read for MOUSE, key fields for KEY. related is the other node:
 * mouseover's and mouseout's, a focus change's (the node losing focus for
 * focus, the one gaining it for blur), a submission's submitter; or NULL.
 * detail is UIEvent's: a mouse button's click count, 0 for any other
 * event. buttons is MouseEvent's: the buttons held, a bit each (1 the
 * primary). */
typedef struct {
    const char *type;
    bool bubbles, cancelable;
    os64_dom_event_kind_t kind;
    int32_t client_x, client_y, screen_x, screen_y;
    int32_t button;
    int32_t detail;
    uint16_t buttons;
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
    OS64_DOM_SCRIPT_MODULE,     /* the host runs it as a module (yonder's modules.c) */
    OS64_DOM_SCRIPT_IMPORTMAP   /* the host registers it (yonder's importmap.c) */
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
 * The binding becomes the runtime's script host (libjs CONTRACT.md § A
 * script host) until os64_dom_drain: from then on no os64_js_run judges an
 * unhandled rejection, and a host that runs a script itself names it with
 * os64_dom_script_name and follows it with os64_dom_script_ended, which
 * judges.
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
    OS64_DOM_STORAGE_FAILED = -2
} os64_dom_storage_status_t;
/* A provider, all on the owner thread, none entering JS; `origin` is the
 * serialized origin ("https://example.com"), borrowed for the call.
 * identity: a value that changes whenever the store does (yonder's: the
 * file's size and modification time), 0 for no store; asked at the first
 * touch in each task, and a value other than the one the binding last
 * loaded means the store moved and is loaded again.
 * load: the store's image, written into `out` when it fits in `cap`;
 * answers its length either way (0 for none), negative when it cannot be
 * read or is not an image (an empty file is not one). The binding asks
 * again with room when it did not fit, a few times; a store still
 * outgrowing its room is read at the next task.
 * commit: this page's changes since its last commit (a change list in
 * os64_dom_storage_merge's format), merged onto the store as it stands. */
typedef struct {
    uint64_t (*identity)(void *opaque, const char *origin);
    int64_t (*load)(void *opaque, const char *origin, void *out, size_t cap);
    os64_dom_storage_status_t (*commit)(void *opaque, const char *origin, const void *changes,
                                        size_t length);
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
 * script holds is live; an element not in the document, and a
 * pseudo-element, answer an empty declaration without asking. */
typedef struct os64_dom_computed os64_dom_computed_t;
/* The provider makes the element's style current (a layout, when it was
 * stale), puts every property it knows, and answers true; an element with
 * no style puts nothing and answers true. False when no current style could
 * be made: the script sees InvalidStateError. Must not enter JS or pump
 * events. */
typedef bool (*os64_dom_computed_provider_t)(void *opaque, const os64_html_node_t *element,
                                            os64_dom_computed_t *out);
/* The provider's pen: property `prop` is `length` bytes of UTF-8 `value`,
 * copied. A property not put reads "". False when the binding could not
 * keep it (its budget), which the provider answers as its own false. */
bool os64_dom_computed_put(os64_dom_computed_t *out, int32_t prop, const char *value, size_t length);
/* What making the style current cost, added to os64_dom_geometry_stats. */
void os64_dom_computed_cost(os64_dom_computed_t *out, uint64_t layouts, uint64_t elapsed_us);
/* NULL removes it; a declaration's read then throws InvalidStateError for
 * an element in the document, and reads "" for a pseudo-element or an
 * element outside it. Install outside callbacks. */
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
     * when `replace`), scrolls to its fragment, and owes hashchange when
     * the fragment moved (os64_dom_history_event). On true the binding
     * makes `url` the document's address and null its state, and fires
     * popstate itself, inside the script that asked (HTML § Navigate to a
     * fragment, whose "update document for history step application"
     * fires it then and there). False when the host could not: nothing
     * changed, and the binding asks for the address as a navigation
     * instead (os64_dom_take_navigation). */
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
 * os64_dom_history_event. False on no memory, nothing changed; it is the
 * step of a move that can fail, so a host calls it before its own entries
 * move and moves them only on true, or the window would show one address
 * while the page's scripts read another. Owner thread, outside JS. */
bool os64_dom_history_moved(os64_dom_t *dom, const char *url, const char *state, size_t length);
/* The document's current entry and its state, named: a new name at every
 * state update, fragment navigation and move. A host takes it when it owes
 * a popstate, so the event knows whether its entry is still the current
 * one. 0 for a NULL binding. */
uint64_t os64_dom_history_entry(const os64_dom_t *dom);
/* One of the history's events as the host OWED it: what it carries is
 * captured then, not read when it runs, since other moves may come
 * between. */
typedef struct {
    bool hashchange;
    /* hashchange: the document's address before and after the move it was
     * owed for. */
    const char *old_url, *new_url;
    /* popstate: the state of the entry it was owed for, `length` bytes of
     * NUL-terminated JSON (NULL for null), and that entry's name
     * (os64_dom_history_entry) when it was owed. While that entry is still
     * the current one the event's state is history.state itself; after,
     * it is read from this JSON. */
    const char *state;
    size_t length;
    uint64_t entry;
} os64_dom_history_event_t;
/* `popstate` or `hashchange` at window, as one task. OK with nothing run
 * when modern is off or nothing could hear it; BAD_ARGUMENT for a
 * hashchange without both addresses, or a length with no state. */
os64_js_status_t os64_dom_history_event(os64_dom_t *dom, const os64_dom_history_event_t *event,
                                        os64_js_outcome_t *outcome);
/* Whether the page set history.scrollRestoration to "manual": the host
 * then leaves the scroll where it is when it traverses to another entry
 * of this document. */
bool os64_dom_history_manual(const os64_dom_t *dom);

/* Owner-thread only, outside JS callbacks. Idempotent; NULL is a no-op.
 * Drain closes bindings and releases C-retained JS values even when libjs
 * has entered a sticky failed state. It first commits each storage area's
 * changes to its provider, the last of a page's commits. Native
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
 * microtask checkpoint, settles what they left owed (window's error and
 * rejection events, as os64_dom_script_ended says), and closes it. A
 * listener that throws is reported and the next one runs; a sticky failure
 * ends the dispatch. The outcome is the task's: its first exception or
 * unhandled rejection (the rejections after it told to options.failed), else a reported error
 * from a nested dispatch, else OK; a sticky status means the runtime is
 * finished with. *prevented answers whether the default action is cancelled.
 * Owner thread, between tasks. Costs no engine entry when listens() says no.
 * A dispatch that moves the document's readiness (os64_dom_readiness_after)
 * moves it first, whether or not anything listens to the event itself, and
 * with the Modern Web switch on fires readystatechange at the document as
 * a task of its own just before it, the two outcomes folded into one (the
 * first failure, a sticky one over one that is not); a sticky outcome of
 * readystatechange's ends the dispatch there. */
os64_js_status_t os64_dom_dispatch(os64_dom_t *dom, const os64_html_node_t *node,
                                   const os64_dom_event_t *event, bool *prevented,
                                   os64_js_outcome_t *outcome);
/* A person's submission of `form`, by `submitter` (a submit button, or NULL
 * for none), as one task: HTML's interactive constraint validation (§
 * 4.10.21.3, the form submission algorithm), which the host asks BEFORE it
 * fires submit. Unless
 * the submitter's formnovalidate or the form's novalidate says not to, each
 * invalid control the form owns gets an `invalid` event, and the first
 * whose event nobody cancelled is reported (OS64_DOM_ACTIVATE_REPORT).
 * `valid` false means the host submits nothing. With the Modern Web switch
 * off, or with validation skipped, it is true and no task runs (`ran`
 * false): with the switch off the model judges the submission alone, and
 * a submission novalidate or formnovalidate skips is judged by nobody.
 * Owner thread, between tasks. */
os64_js_status_t os64_dom_form_validate(os64_dom_t *dom, const os64_html_node_t *form,
                                        const os64_html_node_t *submitter, bool *valid, bool *ran,
                                        os64_js_outcome_t *outcome);
/* The sentence the last report (OS64_DOM_ACTIVATE_REPORT) gave for `node`,
 * into `out` (truncated to `cap`, terminated): the control's validation
 * message, judged with the page's engine, which a pattern needs. False,
 * and `out` empty, when the last report was not of `node`. */
bool os64_dom_report_message(const os64_dom_t *dom, const os64_html_node_t *node, char *out, size_t cap);
/* HTML's readiness steps as the host's dispatches make them: DOMContentLoaded
 * at `doc`'s document node moves LOADING to INTERACTIVE, and load at window
 * (node NULL) moves either to COMPLETE; anything else leaves `now` as it is.
 * Pure, and the one copy of the rule: os64_dom_dispatch moves the binding's
 * readiness by it, and a host that makes its binding late (a page's first
 * script after its load) keeps the readiness it hands over by it. */
os64_dom_readiness_t os64_dom_readiness_after(os64_dom_readiness_t now, const os64_html_document_t *doc,
                                              const os64_html_node_t *node, const char *type);
/* document.currentScript (HTML § Document.currentScript): the classic script
 * element the host is about to run, set before it runs and set back to
 * NULL after it and the microtask checkpoint that ends its run (a host
 * that runs a script inside another's run sets back the outer one). The binding holds the node while it is set. A node that
 * is not of the binding's arena, or not an element, sets nothing. Owner
 * thread, outside JS callbacks. */
void os64_dom_set_current_script(os64_dom_t *dom, const os64_html_node_t *script);
/* Whether anything on the page could hear `type`: a listener, a handler
 * property, or an on<type> content attribute in the document. O(1) unless the
 * tree moved since the last answer, when the attributes are counted again.
 * The types counted are the ones the host dispatches; the error,
 * unhandledrejection and rejectionhandled the binding fires at window;
 * readystatechange, which the binding fires at the document; and online,
 * offline, languagechange, visibilitychange and prerenderingchange, which
 * window or the document has handlers for and nothing fires. Any other type
 * is not counted and answers true. */
bool os64_dom_listens(os64_dom_t *dom, const char *type);
/* Which event types the document's on<type> content attributes name, as a
 * mask os64_dom_handler_bit reads: the walk os64_dom_listens counts with.
 * It needs no runtime, so a host can ask before making one. It knows no
 * binding, so it names the Modern Web arc's types whatever the switch
 * says; os64_dom_listens counts them only with the switch on. */
uint64_t os64_dom_handler_attributes(const os64_html_document_t *doc);
/* The bit for `type` in that mask; 0 for a type no attribute can name
 * (DOMContentLoaded; readystatechange, visibilitychange and
 * prerenderingchange, whose handlers are the document's IDL properties
 * alone; or one outside the types os64_dom_listens counts). */
uint64_t os64_dom_handler_bit(const char *type);

/* TIMERS, in the host's clock (options.now_ms). next answers the earliest due
 * time, or UINT64_MAX with none. fire runs AT MOST ONE timer due at or before
 * now, as one task, and answers whether it ran one (outcome filled). An
 * interval is re-armed before its callback, so a callback that clears it
 * wins. */
uint64_t os64_dom_timer_next(const os64_dom_t *dom);
bool os64_dom_timer_fire(os64_dom_t *dom, uint64_t now, os64_js_outcome_t *outcome);

/* POSTED MESSAGES (HTML § 9.3.3; MODERN_WEB.md § P2b): what the page's
 * window.postMessage queued, one task each, oldest first. next answers
 * when the oldest was posted on the host's clock (options.now_ms), so a
 * host can run it in its turn among the timers by time, or UINT64_MAX with
 * none. fire runs AT MOST ONE as one task, a `message` (or `messageerror`)
 * at window, and answers whether it ran one (outcome filled). A message
 * whose targetOrigin the document does not have, or that nothing could
 * hear, is dropped without a task, and the next is tried, unless it was
 * posted after the page's next timer fell due (a timer queued first, whose
 * task may add a listener for it): then fire answers false and leaves it.
 * A message a message's task posts is queued behind it. Owner thread,
 * between tasks. */
uint64_t os64_dom_message_next(const os64_dom_t *dom);
bool os64_dom_message_fire(os64_dom_t *dom, os64_js_outcome_t *outcome);

/* IDLE CALLBACKS (W3C requestIdleCallback; MODERN_WEB.md § P2b): the host
 * says when its loop is idle, and the binding runs the page's idle
 * callbacks then. next answers when one may run on the host's clock: 0
 * when one may run now, a later time when the only ones waiting were asked
 * for by an idle callback (the next idle period starts no sooner, so a
 * callback that asks for itself again runs once in each period, not once
 * a turn), UINT64_MAX with none. run is the host saying it has nothing of
 * its own to do: when the page has no timer due and no message queued, it
 * starts an idle period if none is open and one may start, and runs AT MOST
 * ONE idle callback as one task, answering whether it ran one (outcome
 * filled). A callback's {timeout} is a timer of the page's, run by
 * os64_dom_timer_fire. Owner thread, between tasks. */
uint64_t os64_dom_idle_next(const os64_dom_t *dom);
bool os64_dom_idle_run(os64_dom_t *dom, os64_js_outcome_t *outcome);

/* The navigation a script asked for since the last take, if any: the last
 * ask wins, but with the Modern Web switch on history.go()s in a row add
 * up to one delta (OS64_DOM_NAVIGATE_HISTORY), as HTML runs each after the
 * one before it. False when there is none. */
bool os64_dom_take_navigation(os64_dom_t *dom, os64_dom_navigation_t *ask);
/* An error a nested dispatch or a handler compile reported inside a task the
 * host did not open through libdom (a script run with os64_js_run). The
 * first one is kept until taken, here or by os64_dom_script_ended, which
 * always takes it: the outcome when that is OK, else a line told to
 * options.failed. False when there is none. */
bool os64_dom_take_report(os64_dom_t *dom, os64_js_outcome_t *outcome);
/* A script the host is about to run with os64_js_run is compiled under the
 * name this writes to `out` (`os64-script:<n>`, the binding's), never its
 * address: `address` is the script's own, where its response came from
 * after any redirects (HTML mutes a script by its response), NULL for an
 * inline one, which is the page's. The name is how the binding knows a script's code wherever
 * it runs later (a listener it added, a promise it rejected), so it is
 * how HTML's muted errors are decided: a script from another origin than
 * the page's tells the page "Script error." for anything it throws or
 * reports, and its rejections fire no event, not even rejectionhandled
 * when a handler reaches one later. The record keeps them whole. False
 * for a missing argument, a drained binding, or `cap` too small. */
bool os64_dom_script_name(os64_dom_t *dom, const char *address, char *out, size_t cap);
/* The inverse: the address a name os64_dom_script_name gave was given for,
 * borrowed while the binding lives; NULL for an inline script's name (the
 * page's) and for any name not the binding's. A dynamic import() in a
 * classic script resolves against it (HTML's referencing script's base
 * URL). */
const char *os64_dom_script_address(const os64_dom_t *dom, const char *name);
/* The first frame of an engine stack text (an Error's `stack`, an
 * outcome's stack_trace) that names a place in a script, as the engine
 * writes a frame a line, ending it with a newline: "    at f
 * (file:line:col)", or "    at file:line:col" for a script that did not
 * compile. A native frame names no place, nor does the second form in a
 * file the engine or the binding names in angle brackets (JSON text, or
 * eval'd source, that did not parse), and a last line with no newline was
 * cut short: each is passed over. `file` and
 * `function` point into `stack`, with their lengths; `function` is NULL
 * for the second form, and for a name the engine wrote in angle brackets:
 * an anonymous function's, a script's own code's. The file of a classic
 * script run through the binding is its name for it
 * (os64_dom_script_name), which os64_dom_frame_script reads back. False
 * when no frame names a place. */
typedef struct {
    const char *function, *file;
    size_t function_length, file_length;
    uint32_t line, column;
} os64_dom_frame_t;
bool os64_dom_stack_frame(const char *stack, os64_dom_frame_t *frame);
/* The script a frame's file names, when it is one of the binding's names:
 * its address, as os64_dom_script_address reads one. The page's own name,
 * its inline scripts', is NULL with *page true; any other file, and one of
 * another site's scripts the binding keeps no address for, NULL with
 * *page false. */
const char *os64_dom_frame_script(const os64_dom_t *dom, const os64_dom_frame_t *frame, bool *page);
/* After that script ran (and drained): what it left owed, settled as one
 * task named `name`, the host's own name for it (MODERN_WEB.md § P2a). The
 * window's error event for each exception it threw, said to come from its
 * address; an unhandledrejection for each rejection it left unhandled; and
 * what those events' listeners leave in turn. The failures are folded into
 * `outcome` after the script's own: a rejection no listener cancelled is
 * an UNHANDLED_REJECTION whose message begins "unhandled rejection: ", and
 * those past the outcome's first failure are told to options.failed. No
 * engine entry when nothing is owed. False for a missing argument or a
 * drained binding. */
bool os64_dom_script_ended(os64_dom_t *dom, const char *name, os64_js_outcome_t *outcome);

/* SCRIPT ELEMENTS. kind follows HTML's "prepare the script element" type
 * rules (type, else language, the sixteen JavaScript MIME essences, module,
 * importmap);
 * it reads attributes only and works without a binding. start marks a script
 * ALREADY STARTED: 1 when it was not, 0 when it was, -1 when the binding
 * could not record it. A script parsed by innerHTML, or copied from a started
 * one, is born started; a started script never runs again wherever it moves. */
os64_dom_script_kind_t os64_dom_script_kind(const os64_html_node_t *node);
int os64_dom_script_start(os64_dom_t *dom, const os64_html_node_t *node);
/* Whether a Content-Type value (`length` bytes) names JavaScript: its
 * essence, before any `;` and without the white space around it, in any
 * case, is one of the same sixteen essences a script's type is judged by.
 * HTML fetches a module script only with such a type. */
bool os64_dom_javascript_mime(const char *value, size_t length);

/* IMPORT MAPS (HTML § 8.1.5.3; MODERN_WEB.md § Import maps, as built). An
 * import map script's text, `length` bytes NUL-terminated, parsed as JSON
 * as one task named `name`: HTML's "parse an import map string" up to normalizing,
 * which the host does. The walk is handed every entry of "imports" (scope
 * NULL) and of each scope (its key as written), in the JSON's order, the
 * value when it is a string and NULL when it is anything else; a key, or
 * a string, holding a NUL is passed over and a NUL value is NULL. An entry
 * the walk cannot keep (false) fails the parse with a RangeError. What
 * the parse throws (a SyntaxError for the JSON; a TypeError for a
 * top-level value that is no object, an "imports", "scopes" or "integrity"
 * that is none, or a scope that is none; each thrown before any entry is
 * walked) is reported at the window as a task's exception is, and is the
 * outcome: the host registers nothing then. Integrity's entries are not
 * walked. Owner thread, between tasks. */
typedef struct {
    void *opaque;
    bool (*entry)(void *opaque, const char *scope, const char *key, const char *value);
} os64_dom_import_map_walk_t;
os64_js_status_t os64_dom_import_map(os64_dom_t *dom, const char *name, const char *text, size_t length,
                                     const os64_dom_import_map_walk_t *walk, os64_js_outcome_t *outcome);

/* A reference a script names, resolved exactly as a link in the document
 * would be: os64_page_resolve_in, so the input cleaning, the `<base href>`
 * as the tree stands NOW, the fallbacks and the canonical spelling are
 * libpage's, not a copy of them. False when it will not resolve or does
 * not fit in cap. Works without a binding. */
bool os64_dom_resolve(const os64_html_document_t *document, const char *document_url,
                      const char *reference, char *out, size_t cap);

/* A SCRIPT'S REQUESTS (MODERN_WEB.md § Slice 2): fetch() and
 * XMLHttpRequest build a request; the HOST runs it, and every judgement
 * the Fetch standard makes of it (the origin rules, CORS, the cookies, the
 * redirects) is the host's (yonder's fetch job, over libway's origin.c).
 * The page's origin is the host's too: the binding never says what it is.
 * What comes back is already filtered for the page.
 *
 * The request, borrowed for the call: its URL (absolute, as the binding
 * parsed it), its normalized method, its author headers as a header block
 * (way/origin.h), its body bytes (NULL for none), its mode, credentials
 * mode, redirect mode and referrer policy, whether an upload listener asks
 * for a preflight, and the address its Referer names ("" for none). */
typedef struct {
    const char *url;
    const char *method;
    const char *headers;
    size_t headers_length;
    const void *body;
    size_t body_length;
    way_mode_t mode;
    way_credentials_t credentials;
    way_redirect_t redirect;
    way_policy_t policy;
    bool preflight;
    const char *referrer;
} os64_dom_request_t;
/* The answer, borrowed for the call: `ok` false is a network error, which
 * carries nothing else (Fetch: a network error tells the page nothing; the
 * host keeps its reason for its own record); else the response's type,
 * status and status text, URL ("" for an opaque one), whether it was
 * redirected, its filtered header block, and its body (has_body false: a
 * null body). */
typedef struct {
    bool ok;
    way_type_t type;
    int32_t status;
    const char *status_text;
    const char *url;
    bool redirected;
    const char *headers;
    size_t headers_length;
    bool has_body;
    const void *body;
    size_t body_length;
} os64_dom_response_t;
/* The host's network, all on the owner thread, none entering JS.
 * start: begin `request` for the binding's request `token`, answering an id
 * for cancel, or 0 when it could not be begun (the request fails as a
 * network error); its answer comes back through os64_dom_fetched.
 * cancel: the id's answer is no longer wanted (a fetch aborted, an XHR's
 * abort(), a page leaving); a cancelled request never answers.
 * run: a synchronous XMLHttpRequest's request, run to its end before this
 * returns, filling `*response`, which stays the host's until `release`;
 * false when it could not be run, which the page sees as a network error.
 * The run asks `stop(stop_ctx)` between its waits and gives up when it says
 * yes: the binding's answer is the script task's own budget (the task's
 * deadline, and libjs's cancel), so a synchronous request ends when the
 * task that made it would, as one mechanism.
 * With no network, every request is a network error. Copied; NULL removes
 * it. Install before the first script. */
typedef struct {
    uint64_t (*start)(void *opaque, const os64_dom_request_t *request, uint32_t token);
    void (*cancel)(void *opaque, uint64_t id);
    bool (*run)(void *opaque, const os64_dom_request_t *request, bool (*stop)(void *stop_ctx), void *stop_ctx,
                os64_dom_response_t *response);
    void (*release)(void *opaque, os64_dom_response_t *response);
    void *opaque;
} os64_dom_network_t;
void os64_dom_set_network(os64_dom_t *dom, const os64_dom_network_t *network);
/* The answer to request `token`, as ONE TASK: the bytes are copied into the
 * runtime's memory (a body the runtime has no room for is the runtime's
 * memory limit, as any allocation a script makes is), the fetch's promise
 * is settled or the XMLHttpRequest's events fire, and the microtasks run.
 * An answer for a token nothing waits on (aborted, or after drain) does
 * nothing. The outcome is the task's, as os64_dom_dispatch's is. */
os64_js_status_t os64_dom_fetched(os64_dom_t *dom, uint32_t token, const os64_dom_response_t *response,
                                  os64_js_outcome_t *outcome);
/* How many requests the binding has out. os64_dom_drain cancels each
 * through the network's cancel and forgets it. */
size_t os64_dom_fetches_pending(const os64_dom_t *dom);
/* Whether request `token` is out, so its answer would run a task. */
bool os64_dom_fetch_waiting(const os64_dom_t *dom, uint32_t token);

#pragma GCC visibility pop
#endif
