#ifndef YONDER_SCRIPTS_H
#define YONDER_SCRIPTS_H
#include <dom/canvas.h>

// A page's SCRIPT HOST (DOM_D7.md § The script host of a page): its runtime
// and binding, made at the first script that runs, and the lists HTML's
// script order needs: the parser-blocking script the parse is stopped at,
// the `defer` scripts in document order (a parser-inserted module among
// them), and the scripts READY to run — an `async` one whose source has
// landed, one a script connected, a module whose whole graph is compiled
// (the page's module map, modules.h). Every list holds NODES, each with a
// hold; the engine values are libdom's.
//
// One task per call: a script, a timer or one event. The host reports each
// task's outcome; a sticky one (a script that ran out its time, or memory)
// retires the runtime, and the page goes on without script.

#include <dom/dom.h>

#include "diag.h"
#include "script_job.h"
#include "storage.h"

typedef struct yonder_scripts yonder_scripts_t;
bool yonder_scripts_images_pending(const yonder_scripts_t *);
bool yonder_scripts_image_fire(yonder_scripts_t *,os64_js_outcome_t *);
bool yonder_scripts_canvas_bitmap(yonder_scripts_t *, const os64_html_node_t *, canvas_bitmap_t *);
bool yonder_scripts_canvas_dirty(yonder_scripts_t *, bool (*visible)(void *, const os64_html_node_t *), void *);
void yonder_scripts_canvas_painted(yonder_scripts_t *, const os64_html_node_t *);

typedef struct {
    const char *url;                // the page's address: names, location, a src's base
    uint64_t execution_ms;          // each task's budget (Settings' script time limit)
    uint64_t serial;                // names the page to a script's fetch job
    void (*alert)(void *opaque, const char *utf8, size_t length);
    // A `src` script's fetch: the host submits it and answers its job id, 0
    // when it could not; the source comes back through yonder_scripts_fetched.
    uint64_t (*fetch)(void *opaque, uint64_t serial, uint32_t token, const char *url,
                      const char *fallback);
    void (*cancel)(void *opaque, uint64_t job);
    // A `src` script or a module compiled from its source, whose body the
    // cache holds: its blob (libjs's, taken) to be kept where `where` says
    // (BYTECODE_CACHE.md). NULL keeps nothing.
    void (*keep_bytecode)(void *opaque, const yonder_bytecode_t *where, uint8_t *blob, size_t length);
    void (*activate)(void *opaque, const os64_html_node_t *node, os64_dom_activation_t what);
    // document.write: the stream's parser takes the text (os64_html_parser_
    // write) and answers its status. Asked only while the script the parse
    // is stopped at runs; any other write is OS64_HTML_BAD_ARGUMENT without
    // asking.
    int64_t (*write)(void *opaque, const char *utf8, size_t length);
    uint64_t (*now_ms)(void *opaque);
    // The page's current-layout provider (DOM.md § Geometry), given
    // `opaque`: it answers from a layout of the page as far as it has been
    // built, at the view's size. NULL: a script's measurement throws.
    os64_dom_geometry_provider_t geometry;
    void *opaque;
    // The window's view, the computed style's provider (given `opaque`)
    // and the session's history, for the Modern Web arc's surface
    // (MODERN_WEB.md § Slice 1b; dom.h): installed in the binding when it
    // is made. The view's and the history's carry their own opaque. Zeroed,
    // a page's view reads as zeros and its history is its own.
    os64_dom_view_provider_t view;
    os64_dom_computed_provider_t computed;
    os64_dom_image_provider_t images;
    os64_dom_text_provider_t canvas_text;
    os64_dom_history_t history;
    // The page's record (diag.h), borrowed for the host's life: a global
    // the page reached for and did not find, a module that failed (or, with
    // `modern` off, a module script or an import map at all) are written
    // there as they happen. NULL keeps no record.
    yonder_diag_t *diag;
    // The page arrived encrypted (way_head_t.encrypted): what its scripts
    // may do with Secure cookies.
    bool encrypted;
    // The Modern Web arc's surface (os64_dom_options_t.modern), as the
    // page's navigation found the switch.
    bool modern;
    // Where the page's localStorage and sessionStorage are kept (storage.h),
    // borrowed for the host's life; NULL keeps them for the page's life.
    yonder_storage_t *storage;
    // The page's navigation timing as far as it has gone, copied; later
    // milestones arrive by yonder_scripts_mark. NULL: its origin is the
    // host's creation.
    const os64_dom_timing_t *timing;
    // The display the page is told it is on (`screen`), in device pixels,
    // and the window's zoom in thousandths as it stands when asked
    // (devicePixelRatio), given `opaque`; NULL is 1000.
    os64_dom_screen_t screen;
    uint32_t (*zoom)(void *opaque);
    // A script's requests, fetch() and XMLHttpRequest's (dom.h § A
    // SCRIPT'S REQUESTS), given `opaque`. `request` submits the page
    // `serial`'s request `token` and answers its job id (cancelled through
    // `cancel`), 0 when it could not be begun; its answer comes back through
    // yonder_scripts_answered. `run` runs a synchronous XMLHttpRequest's to
    // its end on this thread, filling `*response`, which `release` frees.
    // A NULL `request`: every request is a network error.
    uint64_t (*request)(void *opaque, uint64_t serial, uint32_t token, const os64_dom_request_t *request);
    bool (*run)(void *opaque, uint64_t serial, const os64_dom_request_t *request, bool (*stop)(void *stop_ctx),
                void *stop_ctx, os64_dom_response_t *response);
    void (*release)(void *opaque, os64_dom_response_t *response);
    // The document's own request, for P1-Da's members (os64_dom_options_t):
    // the Referer it carried ("" or NULL for none), copied; and its
    // response's Last-Modified, epoch seconds, 0 for none (which a zeroed
    // options struct says; no server dates a page to 1970).
    const char *referrer;
    int64_t last_modified;
    // What has focus, given `opaque` (os64_dom_options_t.focus): the
    // element focused in this page's document, or NULL, and whether the
    // page has the focus (yonder's: whether it is the page on screen).
    // NULL: nothing has, and neither has it.
    bool (*focus)(void *opaque, const os64_html_node_t **focused);
} yonder_scripts_options_t;

// The script engine's stack, out of yonder's window thread's `stack` bytes
// (SCRIPT_STACK.md § 5): what is left above layout's headroom, which shares
// the thread, clamped to [YONDER_SCRIPT_STACK_MIN, YONDER_SCRIPT_STACK_MAX].
// A stack too small for the headroom and the least, the default 1 MiB or one
// whose size could not be read (0), gives the least: DOM_D10.md's 128 KiB.
#define YONDER_SCRIPT_STACK_MIN ((size_t)128 * 1024)
#define YONDER_SCRIPT_STACK_MAX ((size_t)8 * 1024 * 1024)
#define YONDER_SCRIPT_HEADROOM ((size_t)2 * 1024 * 1024)
size_t yonder_script_stack_limit(size_t stack);

// A host for `doc`, whose control state is `state` (which must outlive it).
// No runtime yet. NULL on no memory.
yonder_scripts_t *yonder_scripts_new(os64_html_document_t *doc, os64_page_state_t *state,
                                     const yonder_scripts_options_t *options);
// The binding's layout counts; after retirement, the last ones it had.
os64_dom_geometry_stats_t yonder_scripts_geometry_stats(yonder_scripts_t *scripts, bool reset);
// Session count of reclaimed teardown leaks, including owners already freed.
// Owner-thread only. Fixtures require zero except deliberate leak probes.
size_t yonder_scripts_teardown_leaks(void);
// Drops the lists, drains the binding, destroys the runtime (reporting and
// reclaiming a leak, DOM_D8.md; other engine invariant violations stay
// fatal), frees the binding's records and releases every hold, in DOM.md's
// teardown order. Cancels the fetches still out. NULL is a no-op.
void yonder_scripts_free(yonder_scripts_t *scripts);

// The parser stopped at `script`. What the stream does next: RESUME at
// once (a script that does not run, a `defer`/`async` one whose fetch has
// been started, or a module, whose graph is begun), or BLOCK: the parse
// waits for this script, which runs
// once yonder_scripts_blocking_ready says its source is in hand (an inline
// script's is already, an import map's too; a `src` script's comes with
// its fetch).
typedef enum { YONDER_STOP_RESUME, YONDER_STOP_BLOCK } yonder_stop_t;
yonder_stop_t yonder_scripts_parser_stop(yonder_scripts_t *scripts, os64_html_node_t *script);
// Whether the script the parse is stopped at can run now: its source is in
// hand, or its fetch failed (it is then skipped).
bool yonder_scripts_blocking_ready(const yonder_scripts_t *scripts);
// Runs the script the parse is stopped at (one task), the one task whose
// document.write reaches the parser. False when there is none to run; a
// failed fetch is said in the outcome and runs nothing.
bool yonder_scripts_run_blocking(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);

// The parse has ended (`os64_html_parser_end` answered OK): the `defer`
// scripts may run, in order.
void yonder_scripts_parse_ended(yonder_scripts_t *scripts);
// Whether a `defer` script has yet to run (DOMContentLoaded waits for it).
bool yonder_scripts_deferring(const yonder_scripts_t *scripts);

// One ready script, if there is one: an async or connected script whose
// source is in hand, else the next `defer` script once the parse has ended.
// False when nothing is ready (a fetch may still be out).
bool yonder_scripts_step(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
bool yonder_scripts_pending(const yonder_scripts_t *scripts);
// Whether a script of the page is still being fetched: a `src` script, or
// a module script whose graph is not all in.
bool yonder_scripts_fetching(const yonder_scripts_t *scripts);
// A script a verb connected with no `src`, which a browser runs inside the
// verb: so it runs before the parse goes on, ahead of any async script that
// landed first. False when none waits.
bool yonder_scripts_step_connected(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
bool yonder_scripts_connected_pending(const yonder_scripts_t *scripts);
// A connected script, as libdom reports one (and the finished-document
// fixtures queue them): a classic inline one is ready at once, and so is
// an import map, a `src` one is fetched, and a module is prepared as a
// parser's would be, but runs when its graph is ready (never inside the
// verb).
void yonder_scripts_connected(yonder_scripts_t *scripts, const os64_html_node_t *script);
// The page's `<link rel=modulepreload>` elements (HTML § Link type
// "modulepreload"): each one's module, with its imports, begun in the page's
// module map, where a module script or an import finds it fetched once; it
// runs nothing. It walks the whole tree, so it may be asked again as the
// parse goes on: a link it saw already names a module the map holds, and
// begins nothing again. With the Modern Web surface only, which runs
// modules.
void yonder_scripts_preload(yonder_scripts_t *scripts);
// A `javascript:` link followed (HTML § 7.4.2.3.3): the address after its
// scheme, percent-decoded, then `#` and the fragment when it has one, which
// arrives decoded (libpage's os64_page_request_t), all read as UTF-8, is
// queued as a classic script of the page's, ready now and run as one task
// in its turn, named `javascript:#inline-<n>` on the page's record. A
// string it evaluates to is not made the page (DECLINED.md). False, and
// nothing queued, when the host is retired, the code is longer than a
// script may be, the page has run out of scripts, or memory ran out.
bool yonder_scripts_address(yonder_scripts_t *scripts, const char *url, const char *fragment);
// A fetch came back: `source` is UTF-8 and borrowed, NULL when it failed;
// `final_url` the response's address after its redirects (NULL or empty:
// the address asked for), which the script is named and muted by from
// then on. `bytecode`, when the cache holds the script's body, is TAKEN
// (freed whatever becomes of the script): the compiled form read beside
// it, and where a compile's is kept.
void yonder_scripts_fetched(yonder_scripts_t *scripts, uint32_t token, const char *source,
                            size_t length, const char *final_url, yonder_bytecode_t *bytecode);

// One event at `node`, or at window (NULL), as os64_dom_dispatch. A page
// whose script never ran makes its runtime here if an element carries an
// on<type> attribute for this event (a page may be all handlers); otherwise
// nothing hears.
os64_js_status_t yonder_scripts_dispatch(yonder_scripts_t *scripts, const os64_html_node_t *node,
                                         const os64_dom_event_t *event, bool *prevented,
                                         os64_js_outcome_t *outcome);
// Whether `type` could be heard. Before the page has a runtime, whether an
// on<type> attribute in the document names it, looked at once per tree
// version.
bool yonder_scripts_listens(yonder_scripts_t *scripts, const char *type);
// A person's submission of `form` by `submitter`, validated by the page as
// os64_dom_form_validate does, before its submit event. A page whose script
// never ran makes its runtime here if an element carries an oninvalid or an
// onsubmit attribute; a page with no runtime otherwise leaves `valid` true, and the
// model's own validation in the submission decides. True when a task ran
// (or the runtime it needed could not be made), and `outcome` says how.
bool yonder_scripts_form_validate(yonder_scripts_t *scripts, const os64_html_node_t *form,
                                  const os64_html_node_t *submitter, bool *valid, os64_js_outcome_t *outcome);
// The sentence the page's last report gave for `node`
// (os64_dom_report_message); false when it was not of `node`.
bool yonder_scripts_report_message(const yonder_scripts_t *scripts, const os64_html_node_t *node, char *out,
                                   size_t cap);
// The last thing the page's scripts said on the status line, kept so the
// page's arrival, which writes the status line, can say it again. NULL when
// nothing was said.
void yonder_scripts_set_note(yonder_scripts_t *scripts, const char *line);
const char *yonder_scripts_note(const yonder_scripts_t *scripts);
uint64_t yonder_scripts_timer_next(const yonder_scripts_t *scripts);
bool yonder_scripts_timer_fire(yonder_scripts_t *scripts, uint64_t now, os64_js_outcome_t *outcome);
// One rendering opportunity's ordered callback batch; the host owns visibility,
// pacing and the rendering step after it (os64_dom_animation_fire).
uint64_t yonder_scripts_animation_next(const yonder_scripts_t *scripts);
bool yonder_scripts_animation_fire(yonder_scripts_t *scripts, uint64_t now, os64_js_outcome_t *outcome);
// Custom element reactions the parser's insertions left owed
// (os64_dom_reactions_run): whether any are, and running them as one task,
// true when it ran. The first failure of the constructor `name` defines,
// for the census (os64_dom_custom_failure).
bool yonder_scripts_reactions_owed(const yonder_scripts_t *scripts);
bool yonder_scripts_reactions_run(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
bool yonder_scripts_custom_failure(const yonder_scripts_t *scripts, const char *name, char *out, size_t cap);
// The page's posted messages (os64_dom_message_next/_fire): when the oldest
// was posted, on yonder_now_ms' clock (UINT64_MAX for none), and the oldest
// run as one task. False when nothing ran.
uint64_t yonder_scripts_message_next(const yonder_scripts_t *scripts);
bool yonder_scripts_message_fire(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
// The page's idle callbacks (os64_dom_idle_next/_run): when one may run,
// on yonder_now_ms' clock (0: now; UINT64_MAX: none), and one run as one
// task, which the binding refuses while the page has a task of its own
// due. False when nothing ran.
uint64_t yonder_scripts_idle_next(const yonder_scripts_t *scripts);
bool yonder_scripts_idle_run(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
bool yonder_scripts_idle_before(yonder_scripts_t *scripts, uint64_t render_due, os64_js_outcome_t *outcome);
// The answer to the page's request `token` (os64_dom_fetched), as one
// task. False when nothing ran: the runtime is gone, or never was, or the
// answer was a module's (YONDER_MODULE_TOKEN), which the page's module map
// compiles without a task. `bytecode`, a module's compiled form when the
// cache holds its body (fetch_job.h), is TAKEN, and freed for any other.
bool yonder_scripts_answered(yonder_scripts_t *scripts, uint32_t token, const os64_dom_response_t *response,
                             yonder_bytecode_t *bytecode, os64_js_outcome_t *outcome);
// The navigation the last task asked for; nodes arrive held.
bool yonder_scripts_take_navigation(yonder_scripts_t *scripts, os64_dom_navigation_t *ask);
// A new task budget, from the next task on, and the budget tasks have.
void yonder_scripts_set_execution_ms(yonder_scripts_t *scripts, uint64_t ms);
uint64_t yonder_scripts_execution_ms(const yonder_scripts_t *scripts);
// False once a sticky outcome retired the runtime: nothing runs again.
bool yonder_scripts_alive(const yonder_scripts_t *scripts);
// The whole address of the `src` script the last task ran, which a task's
// source name (OS64_JS_SOURCE_NAME_CAP) may cut short; NULL when the task
// was an inline script, an event or a timer. Good until the host is freed.
const char *yonder_scripts_task_address(const yonder_scripts_t *scripts);
// A failure as its FAILED line says it: `message`, then where the first
// frame of its stack that names a place is (libdom's os64_dom_stack_frame):
// `TypeError: x is undefined, at f 12:5`. A place in `own`, the failing
// task's script's address (NULL for an inline script, an event or a
// timer), is its line and column; in another script, that script's
// address before them; in one of the page's classic inline scripts,
// `inline`; and anywhere else, the engine's own name for it. A script's
// own code and an anonymous function have no name there. `scripts`, the
// page's host, says which script a classic script's file is, and may be
// NULL where only modules, which are compiled under their address, can
// have failed.
// `message` alone when the stack names no place, which a DOMException's
// and a thrown primitive's never do; cut short at `cap`.
void yonder_scripts_failure(const yonder_scripts_t *scripts, const char *own, const char *message,
                            const char *stack, char *out, size_t cap);
// How long the engine took to evaluate the script the last task ran
// (compiling it, or reading its kept bytecode, included), in microseconds;
// -1 when the last task was an event or a timer, or ran no script.
int64_t yonder_scripts_task_us(const yonder_scripts_t *scripts);
// Whether that script was run from its kept bytecode (BYTECODE_CACHE.md)
// rather than compiled; false whenever task_us is -1.
bool yonder_scripts_task_from_bytecode(const yonder_scripts_t *scripts);
// The record the host was given (options.diag).
yonder_diag_t *yonder_scripts_diag(const yonder_scripts_t *scripts);
// The serial a script's fetch job carries back.
uint64_t yonder_scripts_serial(const yonder_scripts_t *scripts);
// How many tasks this host has run: the audit's count.
uint64_t yonder_scripts_tasks(const yonder_scripts_t *scripts);
// How many bytes the last task handed to document.write: the audit's too.
uint64_t yonder_scripts_written(const yonder_scripts_t *scripts);

void yonder_scripts_set_user_agent(yonder_scripts_t *scripts, const char *(*provider)(void *), void *opaque);
// document.cookie's provider (os64_dom_set_cookies), copied, for the
// binding made now or later. NULL: the document is cookie-averse.
void yonder_scripts_set_cookies(yonder_scripts_t *scripts, const os64_dom_cookies_t *cookies);
// The address the page's scripts run at (the options' url, until the
// session moves the document; only its start when it is longer than
// OS64_DOM_URL_MAX), and whether the page arrived encrypted (the options'
// encrypted).
const char *yonder_scripts_url(const yonder_scripts_t *scripts);
bool yonder_scripts_encrypted(const yonder_scripts_t *scripts);
// What a request the page makes names as its Referer: that address, or
// NULL when it was longer than the host keeps (OS64_DOM_URL_MAX) and so
// holds only its start — a cut address would name a page that is not this
// one, so such a request names none.
const char *yonder_scripts_referrer(const yonder_scripts_t *scripts);
// The page's own document.referrer (the options' referrer, kept): what a
// Reload of the page keeps for the page that replaces it. "" for none.
const char *yonder_scripts_document_referrer(const yonder_scripts_t *scripts);

// THE SESSION'S HALF (MODERN_WEB.md § Slice 1b). The document's address
// moved (a state update, a fragment navigation, a traversal): what a
// script's name and a `src` resolve against from now on. NULL is a no-op.
void yonder_scripts_set_url(yonder_scripts_t *scripts, const char *url);
// The session is moving the document to another of its entries without a
// fetch, or a page arriving is given its entry's state: its address and
// its state (JSON, NULL for none), told to the binding
// (os64_dom_history_moved), or kept for it until the first task makes it,
// and taken as the address. False, with nothing taken, when there was no
// room to keep them; the caller moves the window only on true. True for
// NULL scripts, which have nothing to tell.
bool yonder_scripts_history_moved(yonder_scripts_t *scripts, const char *url, const char *state,
                                  size_t length);
// The binding's name for the document's current entry (os64_dom_history_
// entry), which an owed popstate carries; 0 with no binding.
uint64_t yonder_scripts_history_entry(const yonder_scripts_t *scripts);
// popstate or hashchange as it was owed, as one task (os64_dom_history_
// event). False when nothing ran: no binding, a dead one, or nothing that
// could hear it; the outcome is judged as a task's.
bool yonder_scripts_history_event(yonder_scripts_t *scripts, const os64_dom_history_event_t *event,
                                  os64_js_outcome_t *outcome);
// The view's events, one task a call (os64_dom_view_step). False when
// nothing ran.
bool yonder_scripts_view_step(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
// Whether the view's events have gone unasked for a rendering opportunity
// (YONDER_SCRIPTS_VIEW_CADENCE_MS on yonder_now_ms' clock): a page whose
// tasks keep coming (a message that posts another) still has its resize,
// scroll and media events run, as HTML's event loop updates the rendering
// between tasks.
#define YONDER_SCRIPTS_VIEW_CADENCE_MS 16
bool yonder_scripts_view_due(const yonder_scripts_t *scripts);
// Whether the page asked to keep its own scroll across its entries
// (history.scrollRestoration = "manual").
bool yonder_scripts_scroll_manual(const yonder_scripts_t *scripts);
// A milestone of the page's navigation timing reached at `now_us`
// (os64_micros' clock), for performance.timing and the navigation entry.
void yonder_scripts_mark(yonder_scripts_t *scripts, os64_dom_milestone_t which, uint64_t now_us);
// The page's navigation timing as far as its milestones have been told it;
// NULL for no host.
const os64_dom_timing_t *yonder_scripts_timing(const yonder_scripts_t *scripts);
// The page's storage (dom.h § Web storage): every task the host runs ends
// with os64_dom_storage_settle, which commits what is due; `due` is when
// the next commit is (UINT64_MAX for none, yonder_now_ms' clock), and
// `settle` commits it when no task has. `forget` drops the page's maps and
// the changes it has not committed: "Clear page storage". What is left is
// committed as the runtime is retired (yonder_scripts_depart, or the
// host's teardown), the last of the page's commits.
uint64_t yonder_scripts_storage_due(const yonder_scripts_t *scripts);
void yonder_scripts_storage_settle(yonder_scripts_t *scripts);
// THE PAGE DEPARTS (HTML's unload, yonder.c's page_departs): its runtime
// is retired at once, as a runtime that failed is. Its queued scripts,
// timers and handlers never run, the fetches for its scripts are let go,
// it asks the window nothing more, and its storage makes its last commit
// now. The host stays, dead, until the page is freed. Idempotent.
void yonder_scripts_depart(yonder_scripts_t *scripts);
void yonder_scripts_storage_forget(yonder_scripts_t *scripts);

#endif
