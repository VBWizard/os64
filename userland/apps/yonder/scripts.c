#include "scripts.h"
#include "os64/js_engine.h"
#include "os64/date.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"
#include "os64/io.h"
#include "tls/tls.h"

/* Read on the window owner thread, including after a page owner is freed. */
static size_t teardown_leaks;

size_t yonder_scripts_teardown_leaks(void) { return teardown_leaks; }

// A page's scripts, at most: a page with more is refused the rest, as a
// page with more sheets than SHEETS_MAX is.
#define SCRIPT_COUNT_MAX 4096

typedef enum {
    MODE_BLOCKING,      // parser-inserted, neither defer nor async: the parse waits
    MODE_DEFER,         // parser-inserted `defer src`: after the parse, in order
    MODE_ASYNC,         // `async src`, or a connected `src`: as it lands
    MODE_INLINE,        // a connected inline script: ready at once
    MODE_ADDRESS        // a `javascript:` link's code: ready at once, no node
} Mode;

typedef enum { STATE_FETCHING, STATE_READY, STATE_FAILED, STATE_DONE } State;

typedef struct {
    const os64_html_node_t *node;   // held; NULL for MODE_ADDRESS
    Mode mode;
    State state;
    uint64_t job;                   // while FETCHING
    char *source;
    size_t length;
    const char *why;                // FAILED: what the status line says instead
    char name[OS64_JS_SOURCE_NAME_CAP];
    // A `src` script's whole address, which `name` (the engine's source
    // name) may be too short to hold; NULL for an inline script. Freed with
    // the host. The address asked for until the fetch answers, then the
    // response's, after its redirects (yonder_scripts_fetched): where the
    // script came from is what HTML mutes it by and names it by.
    char *address;
} Item;

struct yonder_scripts {
    os64_html_document_t *doc;
    os64_page_state_t *state;
    yonder_scripts_options_t options;
    char url[OS64_DOM_URL_MAX];
    // The address did not fit `url`, which holds its start: set wherever
    // `url` is copied, read by yonder_scripts_referrer.
    bool url_cut;
    // The entry state the session moved the document to before its
    // binding was made (yonder_scripts_history_moved), handed to the
    // binding when ensure_runtime makes it. JSON, owned; NULL for none.
    char *entry_state;
    os64_js_runtime_t *runtime;
    os64_dom_t *dom;
    // The layout counts a retired binding had, kept for the report that
    // asks after its task.
    const char *(*user_agent)(void *);
    void *user_agent_opaque;
    os64_dom_cookies_t cookies;
    os64_dom_geometry_stats_t last_geometry_stats;
    bool dead;                      // a sticky outcome retired the runtime
    bool parse_ended;
    Item *items;
    uint32_t count, cap;
    int32_t blocking;               // the item the parse is stopped at, or -1
    // READY items in the order they became ready (HTML runs an async
    // script "as soon as possible", which is the order it landed in).
    uint32_t *ready;
    uint32_t ready_head, ready_count;
    uint32_t defer_next;            // the next `defer` item to consider, by index
    uint32_t inline_count;          // names an inline script: #inline-<n>
    uint32_t address_count;         // names a link's script: javascript:#inline-<n>
    uint64_t tasks;
    // The blocking script is running: its document.write reaches the
    // parser. `written` is what the last task handed over, for the audit.
    bool writing;
    uint64_t written;
    // Which types the document's on<type> attributes name, at the tree
    // version it was last looked at: a page whose only script is a handler
    // attribute has no runtime until an event one of them names.
    uint64_t handlers_version;
    bool handlers_scanned;
    uint32_t handlers;
    char note[256];
    // The whole address of the script the last task ran, NULL when it was
    // an inline script, an event or a timer (yonder_scripts_task_address).
    const char *task_address;
    // The page's navigation timing, which its binding reads when asked.
    os64_dom_timing_t timing;
    // How long the last task's script took the engine to evaluate, in
    // microseconds; -1 when the last task evaluated none
    // (yonder_scripts_task_us).
    int64_t task_us;
    // The document's readiness, moved by the dispatches this host makes
    // (os64_dom_readiness_after) whether or not a binding hears them yet:
    // a binding made late, at the first script after the page's load, is
    // made with it. And the options' referrer, the host's own copy.
    os64_dom_readiness_t readiness;
    char *referrer;
};

static void release_item(yonder_scripts_t *s, Item *it)
{
    if (it->state == STATE_FETCHING && it->job != 0 && s->options.cancel != NULL)
        s->options.cancel(s->options.opaque, it->job);
    it->job = 0;
    os64_free(it->source);
    it->source = NULL;
    if (it->node != NULL)
        os64_html_release(s->doc, it->node);
    it->node = NULL;
    it->state = STATE_DONE;
}

// Everything the runtime touched goes, in DOM.md's order: the lists (no
// queued task may reach a dead runtime), the binding's registries, the
// engine, the binding's records. Holds on nodes outlive the engine.
static void retire(yonder_scripts_t *s)
{
    s->dead = true;
    s->blocking = -1;
    for (uint32_t i = 0; i < s->count; i++)
        if (s->items[i].state == STATE_FETCHING || s->items[i].state == STATE_READY)
            release_item(s, &s->items[i]);
    if (s->dom != NULL)
        s->last_geometry_stats = os64_dom_geometry_stats(s->dom, false);
    os64_dom_drain(s->dom);
    os64_js_teardown_report_t report;
    os64_js_destroy_report(s->runtime, &report);
    if (report.leaked) {
        if (teardown_leaks != SIZE_MAX) teardown_leaks++;
        // A desktop launch's stderr belongs to another VT. Keep the verdict
        // in the kernel log so logd preserves the page and reclaimed totals.
        char line[OS64_JS_SOURCE_NAME_CAP + 128];
        os64_snprintf(line, sizeof(line),
                      "Yonder: reclaimed JavaScript teardown leak at %s (%lu blocks, %lu bytes)",
                      s->url, (unsigned long)report.reclaimed_blocks, (unsigned long)report.reclaimed_bytes);
        os64_debug_log(line);
    }
    os64_dom_free(s->dom);
    s->runtime = NULL;
    s->dom = NULL;
}

static bool sticky(os64_js_status_t status)
{
    return status == OS64_JS_LIMIT || status == OS64_JS_CANCELLED ||
           status == OS64_JS_HOST_FAILURE || status == OS64_JS_FAILED_RUNTIME;
}

// A script that threw leaves the jobs it queued before it threw: `run`
// drains only after a script that finished (CONTRACT.md § Results: "the
// host drains them or destroys the runtime"). HTML runs them, after any
// script, at the checkpoint that follows it, so they run here. Until they
// do, the runtime refuses every next task as busy: the page's `load`
// went undelivered on google.com. The script's own failure stays the
// task's; a job that throws is passed over as the next job runs, and a
// sticky outcome (the time limit, memory) replaces it, so the runtime is
// retired.
static void drain_after_exception(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    os64_js_outcome_t jobs = *out;
    while (jobs.jobs_pending) {
        os64_js_status_t r = os64_js_drain_jobs(s->runtime, UINT64_MAX, &jobs);
        if (sticky(r)) {
            *out = jobs;
            return;
        }
        if (r != OS64_JS_OK && r != OS64_JS_MORE_JOBS && r != OS64_JS_EXCEPTION &&
            r != OS64_JS_UNHANDLED_REJECTION)
            return;
    }
}

// A task's outcome, judged: the task is over for the page's storage (its
// next touch asks the store again, and what is due commits), and a sticky
// outcome retires the runtime.
static void judged(yonder_scripts_t *s, const os64_js_outcome_t *out)
{
    s->tasks++;
    os64_dom_storage_settle(s->dom);
    if (sticky(out->status))
        retire(s);
}

static void connected(void *opaque, const os64_html_node_t *script)
{
    yonder_scripts_connected(opaque, script);
}

static void activate(void *opaque, const os64_html_node_t *node, os64_dom_activation_t what)
{
    yonder_scripts_t *s = opaque;
    if (s->options.activate != NULL)
        s->options.activate(s->options.opaque, node, what);
}

// document.write from the page's binding. Only the script the parse is
// stopped at has an insertion point; a listener, a timer or a script that
// did not stop the parse has none, as HTML's would open a new document.
static int64_t write(void *opaque, const char *utf8, size_t length)
{
    yonder_scripts_t *s = opaque;
    if (!s->writing || s->options.write == NULL)
        return OS64_HTML_BAD_ARGUMENT;
    s->written += length;
    return s->options.write(s->options.opaque, utf8, length);
}

// A name the page's window did not have: the census's, with the answer
// the page gets left as it is (YONDER_DIAGNOSTICS.md).
static void global_missed(void *opaque, const char *name)
{
    yonder_scripts_t *s = opaque;
    yonder_diag_missing(s->options.diag, "global", name, 1);
}

// A rule the binding keeps, found broken (libdom's attribute backstop): the
// page's record says which, as a page failure, since no one script did it.
static void broken(void *opaque, const char *sentence)
{
    yonder_scripts_t *s = opaque;
    yonder_diag_failed(s->options.diag, "page", sentence);
}

// A rejection the task left beyond the failure its outcome carries
// (libdom's options.failed): a FAILED line under the key the outcome's
// gets (yonder.c's task_said), the script's whole address when the task
// is a `src` script's.
static void failed(void *opaque, const char *task, const char *message)
{
    yonder_scripts_t *s = opaque;
    char key[OS64_DOM_URL_MAX + 16];
    os64_snprintf(key, sizeof(key), "script %s", s->task_address != NULL ? s->task_address : task);
    yonder_diag_failed(s->options.diag, key, message);
}

static uint64_t now_ms(void *opaque)
{
    yonder_scripts_t *s = opaque;
    return s->options.now_ms != NULL ? s->options.now_ms(s->options.opaque) : 0;
}

// performance.now()'s clock: os64_micros, the clock the timing's
// milestones are marked on.
static uint64_t now_us(void *opaque)
{
    (void)opaque;
    int64_t now = os64_micros();
    return now > 0 ? (uint64_t)now : 0;
}

static void timing(void *opaque, os64_dom_timing_t *out)
{
    yonder_scripts_t *s = opaque;
    *out = s->timing;
}

static uint32_t zoom(void *opaque)
{
    yonder_scripts_t *s = opaque;
    return s->options.zoom != NULL ? s->options.zoom(s->options.opaque) : 1000;
}

// document.activeElement's and hasFocus()'s: the window's answer for this
// page's document.
static bool focus(void *opaque, const os64_html_node_t **focused)
{
    yonder_scripts_t *s = opaque;
    return s->options.focus(s->options.opaque, focused);
}

// crypto.getRandomValues' and randomUUID's bytes: the system's entropy, read
// by the door every TLS handshake draws from (libtls's os64_tls_entropy).
static bool entropy(void *opaque, uint8_t *out, size_t length)
{
    (void)opaque;
    return os64_tls_entropy(out, length);
}

// The page's storage providers: the window's store (storage.h), with the
// page's record for a commit the store was too full to keep.
static uint64_t local_identity(void *opaque, const char *origin)
{
    yonder_scripts_t *s = opaque;
    return yonder_storage_identity(s->options.storage, OS64_DOM_STORAGE_LOCAL, origin);
}

static int64_t local_load(void *opaque, const char *origin, void *out, size_t cap)
{
    yonder_scripts_t *s = opaque;
    return yonder_storage_load(s->options.storage, OS64_DOM_STORAGE_LOCAL, origin, out, cap);
}

static os64_dom_storage_status_t local_commit(void *opaque, const char *origin, const void *changes,
                                              size_t length)
{
    yonder_scripts_t *s = opaque;
    return yonder_storage_commit(s->options.storage, OS64_DOM_STORAGE_LOCAL, origin, changes, length,
                                 s->options.diag);
}

static uint64_t session_identity(void *opaque, const char *origin)
{
    yonder_scripts_t *s = opaque;
    return yonder_storage_identity(s->options.storage, OS64_DOM_STORAGE_SESSION, origin);
}

static int64_t session_load(void *opaque, const char *origin, void *out, size_t cap)
{
    yonder_scripts_t *s = opaque;
    return yonder_storage_load(s->options.storage, OS64_DOM_STORAGE_SESSION, origin, out, cap);
}

static os64_dom_storage_status_t session_commit(void *opaque, const char *origin, const void *changes,
                                                size_t length)
{
    yonder_scripts_t *s = opaque;
    return yonder_storage_commit(s->options.storage, OS64_DOM_STORAGE_SESSION, origin, changes, length,
                                 s->options.diag);
}

// A script's requests (dom.h § A SCRIPT'S REQUESTS): the window's, for
// this page by its serial.
static uint64_t net_start(void *opaque, const os64_dom_request_t *request, uint32_t token)
{
    yonder_scripts_t *s = opaque;
    return s->options.request != NULL ? s->options.request(s->options.opaque, s->options.serial, token, request) : 0;
}

static void net_cancel(void *opaque, uint64_t id)
{
    yonder_scripts_t *s = opaque;
    if (s->options.cancel != NULL)
        s->options.cancel(s->options.opaque, id);
}

static bool net_run(void *opaque, const os64_dom_request_t *request, bool (*stop)(void *stop_ctx), void *stop_ctx,
                    os64_dom_response_t *response)
{
    yonder_scripts_t *s = opaque;
    return s->options.run != NULL &&
           s->options.run(s->options.opaque, s->options.serial, request, stop, stop_ctx, response);
}

static void net_release(void *opaque, os64_dom_response_t *response)
{
    yonder_scripts_t *s = opaque;
    if (s->options.release != NULL)
        s->options.release(s->options.opaque, response);
}

// The runtime and its binding, made at the first script that runs. Heap
// and source keep the profile measured for libdom (DOM.md § D5b); the stack
// is the 128 KiB DOM_D10.md measured, because geometry shares it with native
// layout. The deadline is the page's task budget, and the job cap a backstop
// only: a job costs time, which the deadline already bounds. Teardown
// reclaims and reports a leak rather than ending the window (DOM_D8.md).
static bool ensure_runtime(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    if (s->runtime != NULL)
        return true;
    os64_js_config_t config = {os64_js_default_limits()};
    config.limits.stack_bytes = 128 * 1024;
    config.limits.execution_ms = s->options.execution_ms;
    config.limits.jobs_per_turn = (uint64_t)1 << 20;
    if (os64_js_create_with_teardown(&config, OS64_JS_TEARDOWN_RECLAIM, OS64_JS_ABI_ID,
                                     &s->runtime, out) != OS64_JS_OK)
        return false;
    os64_dom_options_t options = os64_dom_default_options();
    options.modern = s->options.modern;
    options.alert = s->options.alert;
    options.alert_opaque = s->options.opaque;
    options.url = s->url;
    options.now_ms = now_ms;
    options.script_connected = connected;
    options.activate = activate;
    options.write = write;
    options.now_us = now_us;
    options.timing = timing;
    if (s->options.screen.width > 0 && s->options.screen.height > 0)
        options.screen = s->options.screen;
    options.zoom = zoom;
    options.broken = broken;
    options.failed = failed;
    options.entropy = entropy;
    options.readiness = s->readiness;
    options.referrer = s->referrer;
    options.last_modified = s->options.last_modified != 0 ? s->options.last_modified : OS64_DOM_NO_DATE;
    options.focus = s->options.focus != NULL ? focus : NULL;
    options.host_opaque = s;
    s->dom = os64_dom_create(s->runtime, s->doc, s->state, &options, out);
    os64_dom_set_geometry(s->dom, s->options.geometry, s->options.opaque);
    if (s->options.view.read != NULL)
        os64_dom_set_view(s->dom, &s->options.view);
    os64_dom_set_computed(s->dom, s->options.computed, s->options.opaque);
    if (s->options.history.update != NULL)
        os64_dom_set_history(s->dom, &s->options.history);
    // The state the session gave the document before there was a binding
    // to tell (a traversal's or a reload's entry): a binding that cannot
    // take it is not built, so the page never reads a state the window
    // does not have.
    if (s->dom != NULL && s->entry_state != NULL &&
        !os64_dom_history_moved(s->dom, s->url, s->entry_state, os64_strlen(s->entry_state))) {
        out->status = OS64_JS_HOST_FAILURE;
        return false;
    }
    os64_free(s->entry_state);
    s->entry_state = NULL;
    os64_dom_set_user_agent(s->dom, s->user_agent, s->user_agent_opaque);
    os64_dom_set_cookies(s->dom, &s->cookies);
    if (s->options.storage != NULL) {
        const os64_dom_storage_t local = {local_identity, local_load, local_commit, s};
        const os64_dom_storage_t session = {session_identity, session_load, session_commit, s};
        os64_dom_set_storage(s->dom, OS64_DOM_STORAGE_LOCAL, &local);
        os64_dom_set_storage(s->dom, OS64_DOM_STORAGE_SESSION, &session);
    }
    if (s->options.diag != NULL)
        os64_dom_set_global_miss(s->dom, global_missed, s);
    if (s->options.request != NULL) {
        const os64_dom_network_t network = {net_start, net_cancel, net_run, net_release, s};
        os64_dom_set_network(s->dom, &network);
    }
    if (s->dom == NULL || os64_js_install_output(s->runtime, 1, OS64_JS_OUTPUT_CONSOLE, out) != OS64_JS_OK) {
        // A runtime a binding could not be built in is not evaluated again.
        out->status = OS64_JS_HOST_FAILURE;
        return false;
    }
    return true;
}

static char *keep(const char *text);

yonder_scripts_t *yonder_scripts_new(os64_html_document_t *doc, os64_page_state_t *state,
                                     const yonder_scripts_options_t *options)
{
    if (doc == NULL || state == NULL || options == NULL || os64_page_state_document(state) != doc)
        return NULL;
    yonder_scripts_t *s = os64_calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->doc = doc;
    s->state = state;
    s->options = *options;
    s->options.url = NULL;
    s->options.timing = NULL;
    s->options.referrer = NULL;
    // A referrer that cannot be kept is none: the page reads "" rather
    // than a page that is not its own.
    s->referrer = keep(options->referrer);
    s->url_cut = os64_strcopy(s->url, sizeof(s->url), options->url != NULL ? options->url : "about:blank") >=
                 sizeof(s->url);
    s->blocking = -1;
    s->task_us = -1;
    if (options->timing != NULL) {
        s->timing = *options->timing;
    } else {
        // No navigation said when it began: the host's making is the origin.
        s->timing.origin_us = now_us(NULL);
        os64_time_t wall;
        if (os64_time(&wall) == 0 && wall.ticks_per_second != 0)
            s->timing.origin_epoch_us = wall.epoch * 1000000 +
                (int64_t)((uint64_t)wall.ticks_into_second * 1000000 / wall.ticks_per_second);
        for (int i = 0; i < OS64_DOM_TIMING_COUNT; i++)
            s->timing.at[i] = OS64_DOM_TIMING_NOT_YET;
    }
    return s;
}

os64_dom_geometry_stats_t yonder_scripts_geometry_stats(yonder_scripts_t *s, bool reset)
{
    if (s == NULL)
        return (os64_dom_geometry_stats_t){0};
    if (s->dom != NULL)
        return os64_dom_geometry_stats(s->dom, reset);
    os64_dom_geometry_stats_t result = s->last_geometry_stats;
    if (reset)
        s->last_geometry_stats = (os64_dom_geometry_stats_t){0};
    return result;
}

void yonder_scripts_free(yonder_scripts_t *s)
{
    if (s == NULL)
        return;
    retire(s);
    for (uint32_t i = 0; i < s->count; i++) {
        release_item(s, &s->items[i]);
        os64_free(s->items[i].address);
    }
    os64_free(s->items);
    os64_free(s->ready);
    os64_free(s->entry_state);
    os64_free(s->referrer);
    os64_free(s);
}

// A copy of `text`, NULL for none or no memory: the address is for the
// page's record, and a script runs the same without it.
static char *keep(const char *text)
{
    if (text == NULL)
        return NULL;
    size_t n = os64_strlen(text) + 1;
    char *c = os64_malloc(n);
    if (c != NULL)
        os64_memcpy(c, text, n);
    return c;
}

// A new item for `script`, holding it (a link's script has no element, and
// holds nothing). NULL past the page's cap or out of memory: that script
// does not run, as a script past a browser's limit does not.
static Item *item_new(yonder_scripts_t *s, const os64_html_node_t *script, Mode mode)
{
    if (s->count == SCRIPT_COUNT_MAX)
        return NULL;
    if (s->count == s->cap) {
        uint32_t cap = s->cap != 0 ? s->cap * 2 : 16;
        Item *items = os64_realloc(s->items, cap * sizeof(*items));
        if (items == NULL)
            return NULL;
        s->items = items;
        uint32_t *ready = os64_malloc(cap * sizeof(*ready));
        if (ready == NULL)
            return NULL;
        // The ready ring is unrolled into its new storage.
        for (uint32_t i = 0; i < s->ready_count; i++)
            ready[i] = s->ready[(s->ready_head + i) % s->cap];
        os64_free(s->ready);
        s->ready = ready;
        s->ready_head = 0;
        s->cap = cap;
    }
    Item *it = &s->items[s->count++];
    os64_memset(it, 0, sizeof(*it));
    it->node = script;
    it->mode = mode;
    it->state = STATE_READY;
    if (script != NULL)
        os64_html_hold(s->doc, script);
    return it;
}

static void make_ready(yonder_scripts_t *s, Item *it)
{
    it->state = STATE_READY;
    if (it->mode == MODE_BLOCKING || it->mode == MODE_DEFER)
        return;                         // run by the parse's order, not the ring
    s->ready[(s->ready_head + s->ready_count++) % s->cap] = (uint32_t)(it - s->items);
}

// A script that cannot run is not dropped in silence: it keeps its place
// in the order and is reported there, once, as a failure of its own.
static void fail_item(yonder_scripts_t *s, Item *it, const char *why)
{
    it->job = 0;
    it->why = why;
    if (it->state == STATE_FETCHING || it->state == STATE_READY) {
        it->state = STATE_READY;    // make_ready's queue, for the async and inline kinds
        make_ready(s, it);
    }
    it->state = STATE_FAILED;
}

// An inline script's source: its text children, copied now, as HTML takes
// a script's source text when it is prepared.
static bool inline_source(yonder_scripts_t *s, Item *it)
{
    os64_snprintf(it->name, sizeof(it->name), "%.*s#inline-%u", (int)(sizeof(it->name) - 24),
                  s->url, ++s->inline_count);
    // At most the source a runtime is created with (ensure_runtime).
    size_t cap = os64_js_default_limits().source_bytes, length = 0;
    for (const os64_html_node_t *n = it->node->first_child; n != NULL; n = n->next)
        if (n->kind == OS64_HTML_TEXT) {
            if (n->text_len > cap - length)
                return false;
            length += n->text_len;
        }
    it->source = os64_malloc(length + 1);
    if (it->source == NULL)
        return false;
    size_t at = 0;
    for (const os64_html_node_t *n = it->node->first_child; n != NULL; n = n->next)
        if (n->kind == OS64_HTML_TEXT) {
            os64_memcpy(it->source + at, n->text, n->text_len);
            at += n->text_len;
        }
    it->source[length] = '\0';
    it->length = length;
    return true;
}

// A `src` script's fetch, its address resolved against the document's base.
// False when there is no address to fetch: HTML then fires `error` at the
// element, which nothing here listens to, and runs nothing.
static bool src_fetch(yonder_scripts_t *s, Item *it)
{
    const os64_html_attr_t *src = os64_html_attr(it->node, "src");
    char url[OS64_DOM_URL_MAX];
    os64_strcopy(it->name, sizeof(it->name), src != NULL ? src->value : "script");
    it->address = keep(src != NULL ? src->value : NULL);
    // The base as the tree stands at preparation: a script before this one
    // may have added or moved it.
    if (src == NULL || src->value[0] == '\0' ||
        !os64_dom_resolve(s->doc, s->url, src->value, url, sizeof(url)))
        return false;
    os64_strcopy(it->name, sizeof(it->name), url);
    os64_free(it->address);
    it->address = keep(url);
    const os64_html_attr_t *charset = os64_html_attr(it->node, "charset");
    const char *fallback = charset != NULL ? charset->value : s->doc->charset;
    it->state = STATE_FETCHING;
    it->job = s->options.fetch != NULL
        ? s->options.fetch(s->options.opaque, s->options.serial, (uint32_t)(it - s->items) + 1, url,
                           fallback != NULL ? fallback : "")
        : 0;
    if (it->job == 0) {
        it->state = STATE_READY;
        return false;
    }
    return true;
}

yonder_stop_t yonder_scripts_parser_stop(yonder_scripts_t *s, os64_html_node_t *script)
{
    if (s == NULL || s->dead || os64_dom_script_kind(script) != OS64_DOM_SCRIPT_CLASSIC)
        return YONDER_STOP_RESUME;
    bool src = os64_html_attr(script, "src") != NULL;
    Mode mode = !src ? MODE_BLOCKING
              : os64_html_attr(script, "async") != NULL ? MODE_ASYNC
              : os64_html_attr(script, "defer") != NULL ? MODE_DEFER
              : MODE_BLOCKING;
    Item *it = item_new(s, script, mode);
    if (it == NULL)
        return YONDER_STOP_RESUME;
    if (s->dom != NULL)
        (void)os64_dom_script_start(s->dom, script);
    if (!src) {
        if (!inline_source(s, it))
            fail_item(s, it, "the inline script is longer than a script may be");
        s->blocking = (int32_t)(it - s->items);
        return YONDER_STOP_BLOCK;
    }
    bool fetching = src_fetch(s, it);
    if (!fetching)
        fail_item(s, it, "the script's address cannot be fetched");
    if (mode != MODE_BLOCKING)
        return YONDER_STOP_RESUME;
    s->blocking = (int32_t)(it - s->items);
    return YONDER_STOP_BLOCK;
}

bool yonder_scripts_blocking_ready(const yonder_scripts_t *s)
{
    return s != NULL && s->blocking >= 0 && s->items[s->blocking].state != STATE_FETCHING;
}

// One script, as one task. A script a mutation took out of the document
// before its turn is skipped: HTML runs a script only while it is connected
// to the document that prepared it. Its kind is asked again for the same
// reason a language can change. The item is named by its index: a script
// that connects another grows the table under this call.
static bool run_item(yonder_scripts_t *s, uint32_t index, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    s->written = 0;
    Item *it = &s->items[index];
    // A link's script has no element to be connected or typed: it is the
    // page's own code, and runs while the page does.
    const os64_html_node_t *root = it->node;
    while (root != NULL && root->parent != NULL)
        root = root->parent;
    bool runs = it->state == STATE_READY &&
                (it->mode == MODE_ADDRESS ||
                 (root == s->doc->document && os64_dom_script_kind(it->node) == OS64_DOM_SCRIPT_CLASSIC));
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_strcopy(name, sizeof(name), it->name);
    s->task_address = it->address;
    s->task_us = -1;
    if (it->state == STATE_FAILED) {
        out->status = OS64_JS_EXCEPTION;
        os64_strcopy(out->message, sizeof(out->message), it->why);
        os64_strcopy(out->source_name, sizeof(out->source_name), name);
    }
    if (runs && ensure_runtime(s, out)) {
        if (it->node != NULL)
            (void)os64_dom_script_start(s->dom, it->node);
        // The source moves out of the table before the run, for the same
        // reason the item is named by its index.
        char *source = it->source;
        size_t length = it->length;
        it->source = NULL;
        // The engine's whole evaluation, compiling the source included: the
        // number a bytecode cache would make smaller (MODERN_WEB.md slice 7).
        // The engine knows the script by the binding's name for it, which is
        // how the binding tells another site's script's failures from the
        // page's (libdom's report.c); the outcome keeps the item's name.
        char engine_name[32];
        if (!os64_dom_script_name(s->dom, s->task_address, engine_name, sizeof(engine_name)))
            os64_strcopy(engine_name, sizeof(engine_name), name);
        int64_t began = os64_micros();
        // document.currentScript, while it runs, while the microtasks it
        // queued drain, and while its failures' events fire: HTML's "run a
        // classic script" reports its exception and ends in the microtask
        // checkpoint (which notifies about rejected promises), and only
        // then does executing the script element put currentScript back
        // (WPT's Document.currentScript "Script microtask" sees the
        // element, and so does an error listener). os64_js_run drains them
        // itself; a run that threw drains them below. A link's script has
        // no element, and leaves it null.
        os64_dom_set_current_script(s->dom, it->node);
        os64_js_run(s->runtime, source, length, engine_name, out);
        int64_t ended = os64_micros();
        s->task_us = began >= 0 && ended >= began ? ended - began : -1;
        os64_free(source);
        if (out->status == OS64_JS_EXCEPTION && out->jobs_pending)
            drain_after_exception(s, out);
        os64_strcopy(out->source_name, sizeof(out->source_name), name);
        // What the script left owed: the window's error event for what it
        // threw, an unhandledrejection for each rejection nothing handled,
        // which is a failure of this script's when no listener cancels it,
        // and an error a nested dispatch reported (libdom's report.c).
        os64_dom_script_ended(s->dom, name, out);
        os64_dom_set_current_script(s->dom, NULL);
        judged(s, out);
    } else if (runs) {
        os64_strcopy(out->source_name, sizeof(out->source_name), name);
        retire(s);
    }
    if (!s->dead)
        release_item(s, &s->items[index]);
    return true;
}

bool yonder_scripts_run_blocking(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    if (!yonder_scripts_blocking_ready(s))
        return false;
    uint32_t index = (uint32_t)s->blocking;
    s->blocking = -1;
    s->writing = true;
    bool ran = run_item(s, index, out);
    s->writing = false;
    return ran;
}

void yonder_scripts_parse_ended(yonder_scripts_t *s)
{
    if (s != NULL)
        s->parse_ended = true;
}

// The next `defer` item that has not run, or NULL.
static Item *next_defer(yonder_scripts_t *s)
{
    while (s->defer_next < s->count &&
           (s->items[s->defer_next].mode != MODE_DEFER || s->items[s->defer_next].state == STATE_DONE))
        s->defer_next++;
    return s->defer_next < s->count ? &s->items[s->defer_next] : NULL;
}

bool yonder_scripts_deferring(const yonder_scripts_t *s)
{
    return s != NULL && !s->dead && next_defer((yonder_scripts_t *)s) != NULL;
}

bool yonder_scripts_pending(const yonder_scripts_t *s)
{
    if (s == NULL || s->dead)
        return false;
    if (s->ready_count != 0)
        return true;
    Item *d = s->parse_ended ? next_defer((yonder_scripts_t *)s) : NULL;
    return d != NULL && d->state != STATE_FETCHING;
}

bool yonder_scripts_step(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (!yonder_scripts_pending(s))
        return false;
    if (s->ready_count != 0) {
        uint32_t index = s->ready[s->ready_head];
        s->ready_head = (s->ready_head + 1) % s->cap;
        s->ready_count--;
        return run_item(s, index, out);
    }
    return run_item(s, (uint32_t)(next_defer(s) - s->items), out);
}

// The ring position of the first ready connected inline script, or -1.
static int32_t connected_at(const yonder_scripts_t *s)
{
    for (uint32_t i = 0; i < s->ready_count; i++)
        if (s->items[s->ready[(s->ready_head + i) % s->cap]].mode == MODE_INLINE)
            return (int32_t)i;
    return -1;
}

bool yonder_scripts_connected_pending(const yonder_scripts_t *s)
{
    return s != NULL && !s->dead && connected_at(s) >= 0;
}

bool yonder_scripts_step_connected(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (!yonder_scripts_connected_pending(s))
        return false;
    uint32_t at = (uint32_t)connected_at(s);
    uint32_t index = s->ready[(s->ready_head + at) % s->cap];
    // Taken out of the ring; the ones behind it keep their order.
    for (uint32_t i = at; i + 1 < s->ready_count; i++)
        s->ready[(s->ready_head + i) % s->cap] = s->ready[(s->ready_head + i + 1) % s->cap];
    s->ready_count--;
    return run_item(s, index, out);
}

void yonder_scripts_connected(yonder_scripts_t *s, const os64_html_node_t *script)
{
    if (s == NULL || s->dead)
        return;
    // A module is recognised and never run: what the page asked for and
    // did not get.
    if (os64_dom_script_kind(script) == OS64_DOM_SCRIPT_MODULE)
        yonder_diag_missing(s->options.diag, "script", "module", 1);
    bool src = os64_html_attr(script, "src") != NULL;
    Item *it = item_new(s, script, src ? MODE_ASYNC : MODE_INLINE);
    if (it == NULL)
        return;
    if (src) {
        if (!src_fetch(s, it))
            fail_item(s, it, "the script's address cannot be fetched");
        return;
    }
    if (!inline_source(s, it)) {
        fail_item(s, it, "the inline script is longer than a script may be");
        return;
    }
    make_ready(s, it);
}

// A hex digit's value, or -1.
static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// HTML's "navigate to a javascript: URL": the address after its scheme,
// percent-decoded, its fragment joined as given, then read as UTF-8, each
// byte that is not UTF-8 becoming U+FFFD.
bool yonder_scripts_address(yonder_scripts_t *s, const char *url, const char *fragment)
{
    if (s == NULL || s->dead || url == NULL)
        return false;
    const char *rest = os64_strchr(url, ':');
    rest = rest != NULL ? rest + 1 : url;
    size_t length = os64_strlen(rest), extra = fragment != NULL ? os64_strlen(fragment) + 1 : 0;
    size_t cap = os64_js_default_limits().source_bytes;
    if (length > cap || extra > cap - length)
        return false;
    // What the percent-decoding leaves, then the same as UTF-8: no byte
    // grows past three, the replacement character's.
    char *bytes = os64_malloc(length + extra + 1);
    char *source = bytes != NULL ? os64_malloc(3 * (length + extra) + 1) : NULL;
    if (source == NULL) {
        os64_free(bytes);
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; rest[i] != '\0'; i++) {
        int high = rest[i] == '%' ? hex_value(rest[i + 1]) : -1;
        int low = high >= 0 ? hex_value(rest[i + 2]) : -1;
        if (low >= 0) {
            bytes[n++] = (char)(high * 16 + low);
            i += 2;
        } else {
            bytes[n++] = rest[i];
        }
    }
    // libpage hands a fragment over decoded already: it is joined as it is.
    if (fragment != NULL) {
        bytes[n++] = '#';
        for (size_t i = 0; fragment[i] != '\0'; i++)
            bytes[n++] = fragment[i];
    }
    size_t at = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        i += os64_utf8_decode(bytes + i, n - i, &cp);
        at += os64_utf8_encode(cp, source + at);
    }
    source[at] = '\0';
    os64_free(bytes);
    Item *it = item_new(s, NULL, MODE_ADDRESS);
    if (it == NULL) {
        os64_free(source);
        return false;
    }
    os64_snprintf(it->name, sizeof(it->name), "javascript:#inline-%u", ++s->address_count);
    it->source = source;
    it->length = at;
    make_ready(s, it);
    return true;
}

void yonder_scripts_fetched(yonder_scripts_t *s, uint32_t token, const char *source, size_t length,
                            const char *final_url)
{
    if (s == NULL || s->dead || token == 0 || token > s->count)
        return;
    Item *it = &s->items[token - 1];
    if (it->state != STATE_FETCHING)
        return;
    it->job = 0;
    if (source == NULL) {
        fail_item(s, it, "could not fetch the script");
        return;
    }
    // A script that came through redirects is the last address's: a
    // same-origin address that sends it on to another site is that site's
    // script, muted. One whose address cannot be kept is not run under the
    // address it was asked for.
    if (final_url != NULL && final_url[0] != '\0' &&
        (it->address == NULL || !os64_streq(it->address, final_url))) {
        char *moved = keep(final_url);
        if (moved == NULL) {
            fail_item(s, it, "no memory for the script's address");
            return;
        }
        os64_free(it->address);
        it->address = moved;
        os64_strcopy(it->name, sizeof(it->name), final_url);
    }
    it->source = os64_malloc(length + 1);
    if (it->source == NULL) {
        fail_item(s, it, "no memory for the script's source");
        return;
    }
    os64_memcpy(it->source, source, length);
    it->source[length] = '\0';
    it->length = length;
    make_ready(s, it);
}

// Which types the document's on<type> attributes name (libdom's own walk):
// one walk per tree version, and only while the page has no runtime.
static uint32_t handler_types(yonder_scripts_t *s)
{
    uint64_t version = os64_html_version(s->doc);
    if (s->handlers_scanned && s->handlers_version == version)
        return s->handlers;
    s->handlers = os64_dom_handler_attributes(s->doc);
    s->handlers_version = version;
    s->handlers_scanned = true;
    return s->handlers;
}

os64_js_status_t yonder_scripts_dispatch(yonder_scripts_t *s, const os64_html_node_t *node,
                                         const os64_dom_event_t *event, bool *prevented,
                                         os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (s != NULL) {
        s->task_address = NULL;
        s->task_us = -1;
    }
    if (prevented != NULL)
        *prevented = false;
    if (s == NULL || s->dead)
        return OS64_JS_OK;
    s->written = 0;
    // The document's readiness moves with the dispatch, heard or not; a
    // binding made by it below is made with the readiness it leaves. One
    // that already hears it moves its own by the same rule, and fires
    // readystatechange, so a move is dispatched even when nothing listens
    // to the event itself.
    os64_dom_readiness_t was = s->readiness;
    s->readiness = os64_dom_readiness_after(was, s->doc, node, event->type);
    // A page that never ran a script still runs its handler attributes:
    // their runtime is made by the first event one of them names.
    if (s->dom == NULL) {
        if ((handler_types(s) & os64_dom_handler_bit(event->type)) == 0)
            return OS64_JS_OK;
        if (!ensure_runtime(s, out)) {
            retire(s);
            return out->status;
        }
    }
    bool moved = s->readiness != was;
    bool heard = os64_dom_listens(s->dom, event->type) ||
                 (moved && s->options.modern && os64_dom_listens(s->dom, "readystatechange"));
    if (!heard && !moved)
        return OS64_JS_OK;
    os64_dom_dispatch(s->dom, node, event, prevented, out);
    // A move nothing heard ran no task, and is not counted as one.
    if (heard)
        judged(s, out);
    return out->status;
}

bool yonder_scripts_listens(yonder_scripts_t *s, const char *type)
{
    if (s == NULL || s->dead)
        return false;
    return s->dom != NULL ? os64_dom_listens(s->dom, type)
                          : (handler_types(s) & os64_dom_handler_bit(type)) != 0;
}

void yonder_scripts_set_note(yonder_scripts_t *s, const char *line)
{
    if (s != NULL)
        os64_strcopy(s->note, sizeof(s->note), line);
}

const char *yonder_scripts_note(const yonder_scripts_t *s)
{
    return s != NULL && s->note[0] != '\0' ? s->note : NULL;
}

uint64_t yonder_scripts_timer_next(const yonder_scripts_t *s)
{
    return s != NULL && !s->dead ? os64_dom_timer_next(s->dom) : UINT64_MAX;
}

bool yonder_scripts_timer_fire(yonder_scripts_t *s, uint64_t now, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (s != NULL) {
        s->written = 0;
        s->task_address = NULL;
        s->task_us = -1;
    }
    if (s == NULL || s->dead || !os64_dom_timer_fire(s->dom, now, out))
        return false;
    judged(s, out);
    return true;
}

uint64_t yonder_scripts_message_next(const yonder_scripts_t *s)
{
    return s != NULL && !s->dead ? os64_dom_message_next(s->dom) : UINT64_MAX;
}

bool yonder_scripts_message_fire(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (s == NULL || s->dead)
        return false;
    s->written = 0;
    s->task_address = NULL;
    s->task_us = -1;
    if (!os64_dom_message_fire(s->dom, out))
        return false;
    judged(s, out);
    return true;
}

uint64_t yonder_scripts_idle_next(const yonder_scripts_t *s)
{
    return s != NULL && !s->dead ? os64_dom_idle_next(s->dom) : UINT64_MAX;
}

bool yonder_scripts_idle_run(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (s == NULL || s->dead)
        return false;
    s->written = 0;
    s->task_address = NULL;
    s->task_us = -1;
    if (!os64_dom_idle_run(s->dom, out))
        return false;
    judged(s, out);
    return true;
}

bool yonder_scripts_answered(yonder_scripts_t *s, uint32_t token, const os64_dom_response_t *response,
                             os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (s == NULL || s->dead || !os64_dom_fetch_waiting(s->dom, token))
        return false;
    s->written = 0;
    s->task_address = NULL;
    os64_dom_fetched(s->dom, token, response, out);
    judged(s, out);
    return true;
}

bool yonder_scripts_take_navigation(yonder_scripts_t *s, os64_dom_navigation_t *ask)
{
    os64_memset(ask, 0, sizeof(*ask));
    return s != NULL && s->dom != NULL && os64_dom_take_navigation(s->dom, ask);
}

void yonder_scripts_set_execution_ms(yonder_scripts_t *s, uint64_t ms)
{
    if (s == NULL || ms == 0)
        return;
    s->options.execution_ms = ms;
    os64_js_outcome_t out;
    if (s->runtime != NULL)
        (void)os64_js_set_execution_ms(s->runtime, ms, &out);
}

uint64_t yonder_scripts_execution_ms(const yonder_scripts_t *s)
{
    return s != NULL ? s->options.execution_ms : 0;
}

bool yonder_scripts_alive(const yonder_scripts_t *s)
{
    return s != NULL && !s->dead;
}

uint64_t yonder_scripts_tasks(const yonder_scripts_t *s)
{
    return s != NULL ? s->tasks : 0;
}

uint64_t yonder_scripts_written(const yonder_scripts_t *s)
{
    return s != NULL ? s->written : 0;
}

const char *yonder_scripts_task_address(const yonder_scripts_t *s)
{
    return s != NULL ? s->task_address : NULL;
}

int64_t yonder_scripts_task_us(const yonder_scripts_t *s)
{
    return s != NULL ? s->task_us : -1;
}

yonder_diag_t *yonder_scripts_diag(const yonder_scripts_t *s)
{
    return s != NULL ? s->options.diag : NULL;
}

uint64_t yonder_scripts_serial(const yonder_scripts_t *s)
{
    return s != NULL ? s->options.serial : 0;
}

void yonder_scripts_set_cookies(yonder_scripts_t *s, const os64_dom_cookies_t *cookies)
{
    if (s == NULL) return;
    if (cookies != NULL) s->cookies = *cookies;
    else os64_memset(&s->cookies, 0, sizeof(s->cookies));
    os64_dom_set_cookies(s->dom, &s->cookies);
}

const char *yonder_scripts_url(const yonder_scripts_t *s)
{
    return s != NULL ? s->url : NULL;
}

const char *yonder_scripts_referrer(const yonder_scripts_t *s)
{
    return s != NULL && !s->url_cut ? s->url : NULL;
}

const char *yonder_scripts_document_referrer(const yonder_scripts_t *s)
{
    return s != NULL && s->referrer != NULL ? s->referrer : "";
}

bool yonder_scripts_encrypted(const yonder_scripts_t *s)
{
    return s != NULL && s->options.encrypted;
}

void yonder_scripts_set_url(yonder_scripts_t *s, const char *url)
{
    if (s != NULL && url != NULL)
        s->url_cut = os64_strcopy(s->url, sizeof(s->url), url) >= sizeof(s->url);
}

bool yonder_scripts_history_moved(yonder_scripts_t *s, const char *url, const char *state, size_t length)
{
    if (url == NULL)
        return false;
    if (s == NULL)
        return true;
    if (s->dom != NULL && !s->dead && !os64_dom_history_moved(s->dom, url, state, length))
        return false;
    if (s->dom == NULL && !s->dead) {
        // No binding yet: the state waits for the one ensure_runtime makes.
        char *kept = NULL;
        if (state != NULL) {
            kept = os64_malloc(length + 1);
            if (kept == NULL)
                return false;
            os64_memcpy(kept, state, length);
            kept[length] = '\0';
        }
        os64_free(s->entry_state);
        s->entry_state = kept;
    }
    s->url_cut = os64_strcopy(s->url, sizeof(s->url), url) >= sizeof(s->url);
    return true;
}

uint64_t yonder_scripts_history_entry(const yonder_scripts_t *s)
{
    return s != NULL && s->dom != NULL ? os64_dom_history_entry(s->dom) : 0;
}

bool yonder_scripts_history_event(yonder_scripts_t *s, const os64_dom_history_event_t *event,
                                  os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (s == NULL || s->dead || s->dom == NULL ||
        !os64_dom_listens(s->dom, event->hashchange ? "hashchange" : "popstate"))
        return false;
    s->written = 0;
    s->task_address = NULL;
    os64_dom_history_event(s->dom, event, out);
    judged(s, out);
    return true;
}

bool yonder_scripts_view_step(yonder_scripts_t *s, os64_js_outcome_t *out)
{
    os64_memset(out, 0, sizeof(*out));
    if (s == NULL || s->dead || s->dom == NULL)
        return false;
    s->written = 0;
    s->task_address = NULL;
    if (!os64_dom_view_step(s->dom, out))
        return false;
    judged(s, out);
    return true;
}

bool yonder_scripts_scroll_manual(const yonder_scripts_t *s)
{
    return s != NULL && !s->dead && os64_dom_history_manual(s->dom);
}

void yonder_scripts_set_user_agent(yonder_scripts_t *s, const char *(*provider)(void *), void *opaque)
{
    if (s == NULL) return;
    s->user_agent = provider;
    s->user_agent_opaque = opaque;
    os64_dom_set_user_agent(s->dom, provider, opaque);
}

void yonder_scripts_mark(yonder_scripts_t *s, os64_dom_milestone_t which, uint64_t now_us)
{
    if (s == NULL || (unsigned)which >= OS64_DOM_TIMING_COUNT)
        return;
    s->timing.at[which] = now_us > s->timing.origin_us ? now_us - s->timing.origin_us : 0;
}

uint64_t yonder_scripts_storage_due(const yonder_scripts_t *s)
{
    return s != NULL && s->dom != NULL ? os64_dom_storage_due(s->dom) : UINT64_MAX;
}

void yonder_scripts_storage_settle(yonder_scripts_t *s)
{
    if (s != NULL && s->dom != NULL)
        os64_dom_storage_settle(s->dom);
}

void yonder_scripts_depart(yonder_scripts_t *s)
{
    if (s != NULL && !s->dead)
        retire(s);
}

void yonder_scripts_storage_forget(yonder_scripts_t *s)
{
    if (s != NULL && s->dom != NULL)
        os64_dom_storage_forget(s->dom);
}
