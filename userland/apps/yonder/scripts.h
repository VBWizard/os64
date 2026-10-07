#ifndef YONDER_SCRIPTS_H
#define YONDER_SCRIPTS_H

// A page's SCRIPT HOST (DOM_D7.md § The script host of a page): its runtime
// and binding, made at the first script that runs, and the lists HTML's
// script order needs: the parser-blocking script the parse is stopped at,
// the `defer` scripts in document order, and the scripts READY to run —
// an `async` one whose source has landed, one a script connected. Every
// list holds NODES, each with a hold; the engine values are libdom's.
//
// One task per call: a script, a timer or one event. The host reports each
// task's outcome; a sticky one (a script that ran out its time, or memory)
// retires the runtime, and the page goes on without script.

#include <dom/dom.h>

typedef struct yonder_scripts yonder_scripts_t;

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
} yonder_scripts_options_t;

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
// once (a script that does not run, or a `defer`/`async` one whose fetch
// has been started), or BLOCK: the parse waits for this script, which runs
// once yonder_scripts_blocking_ready says its source is in hand (an inline
// script's is already; a `src` script's comes with its fetch).
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
// A script a verb connected with no `src`, which a browser runs inside the
// verb: so it runs before the parse goes on, ahead of any async script that
// landed first. False when none waits.
bool yonder_scripts_step_connected(yonder_scripts_t *scripts, os64_js_outcome_t *outcome);
bool yonder_scripts_connected_pending(const yonder_scripts_t *scripts);
// A connected script, as libdom reports one (and the finished-document
// fixtures queue them): inline is ready at once, `src` is fetched.
void yonder_scripts_connected(yonder_scripts_t *scripts, const os64_html_node_t *script);
// A fetch came back: `source` is UTF-8 and borrowed, NULL when it failed.
void yonder_scripts_fetched(yonder_scripts_t *scripts, uint32_t token, const char *source,
                            size_t length);

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
// The last thing the page's scripts said on the status line, kept so the
// page's arrival, which writes the status line, can say it again. NULL when
// nothing was said.
void yonder_scripts_set_note(yonder_scripts_t *scripts, const char *line);
const char *yonder_scripts_note(const yonder_scripts_t *scripts);
uint64_t yonder_scripts_timer_next(const yonder_scripts_t *scripts);
bool yonder_scripts_timer_fire(yonder_scripts_t *scripts, uint64_t now, os64_js_outcome_t *outcome);
// The navigation the last task asked for; nodes arrive held.
bool yonder_scripts_take_navigation(yonder_scripts_t *scripts, os64_dom_navigation_t *ask);
// A new task budget, from the next task on, and the budget tasks have.
void yonder_scripts_set_execution_ms(yonder_scripts_t *scripts, uint64_t ms);
uint64_t yonder_scripts_execution_ms(const yonder_scripts_t *scripts);
// False once a sticky outcome retired the runtime: nothing runs again.
bool yonder_scripts_alive(const yonder_scripts_t *scripts);
// The serial a script's fetch job carries back.
uint64_t yonder_scripts_serial(const yonder_scripts_t *scripts);
// How many tasks this host has run: the audit's count.
uint64_t yonder_scripts_tasks(const yonder_scripts_t *scripts);
// How many bytes the last task handed to document.write: the audit's too.
uint64_t yonder_scripts_written(const yonder_scripts_t *scripts);

#endif
