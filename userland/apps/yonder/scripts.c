#include "scripts.h"
#include "os64/js_engine.h"
#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/io.h"

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
    MODE_INLINE         // a connected inline script: ready at once
} Mode;

typedef enum { STATE_FETCHING, STATE_READY, STATE_FAILED, STATE_DONE } State;

typedef struct {
    const os64_html_node_t *node;   // held
    Mode mode;
    State state;
    uint64_t job;                   // while FETCHING
    char *source;
    size_t length;
    const char *why;                // FAILED: what the status line says instead
    char name[OS64_JS_SOURCE_NAME_CAP];
    // A `src` script's whole address, which `name` (the engine's source
    // name) may be too short to hold; NULL for an inline script. Freed with
    // the host.
    char *address;
} Item;

struct yonder_scripts {
    os64_html_document_t *doc;
    os64_page_state_t *state;
    yonder_scripts_options_t options;
    char url[OS64_DOM_URL_MAX];
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

// A task's outcome, judged: a sticky one retires the runtime.
static void judged(yonder_scripts_t *s, const os64_js_outcome_t *out)
{
    s->tasks++;
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

static uint64_t now_ms(void *opaque)
{
    yonder_scripts_t *s = opaque;
    return s->options.now_ms != NULL ? s->options.now_ms(s->options.opaque) : 0;
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
    options.alert = s->options.alert;
    options.alert_opaque = s->options.opaque;
    options.url = s->url;
    options.now_ms = now_ms;
    options.script_connected = connected;
    options.activate = activate;
    options.write = write;
    options.host_opaque = s;
    s->dom = os64_dom_create(s->runtime, s->doc, s->state, &options, out);
    os64_dom_set_geometry(s->dom, s->options.geometry, s->options.opaque);
    os64_dom_set_user_agent(s->dom, s->user_agent, s->user_agent_opaque);
    os64_dom_set_cookies(s->dom, &s->cookies);
    if (s->options.diag != NULL)
        os64_dom_set_global_miss(s->dom, global_missed, s);
    if (s->dom == NULL || os64_js_install_output(s->runtime, 1, OS64_JS_OUTPUT_CONSOLE, out) != OS64_JS_OK) {
        // A runtime a binding could not be built in is not evaluated again.
        out->status = OS64_JS_HOST_FAILURE;
        return false;
    }
    return true;
}

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
    os64_strcopy(s->url, sizeof(s->url), options->url != NULL ? options->url : "about:blank");
    s->blocking = -1;
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
    os64_free(s);
}

// A new item for `script`, holding it. NULL past the page's cap or out of
// memory: that script does not run, as a script past a browser's limit does
// not.
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
    const os64_html_node_t *root = it->node;
    while (root->parent != NULL)
        root = root->parent;
    bool runs = it->state == STATE_READY && root == s->doc->document &&
                os64_dom_script_kind(it->node) == OS64_DOM_SCRIPT_CLASSIC;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_strcopy(name, sizeof(name), it->name);
    s->task_address = it->address;
    if (it->state == STATE_FAILED) {
        out->status = OS64_JS_EXCEPTION;
        os64_strcopy(out->message, sizeof(out->message), it->why);
        os64_strcopy(out->source_name, sizeof(out->source_name), name);
    }
    if (runs && ensure_runtime(s, out)) {
        (void)os64_dom_script_start(s->dom, it->node);
        // The source moves out of the table before the run, for the same
        // reason the item is named by its index.
        char *source = it->source;
        size_t length = it->length;
        it->source = NULL;
        os64_js_run(s->runtime, source, length, name, out);
        os64_free(source);
        if (out->status == OS64_JS_EXCEPTION && out->jobs_pending)
            drain_after_exception(s, out);
        os64_js_outcome_t nested;
        if (out->status == OS64_JS_OK && os64_dom_take_report(s->dom, &nested))
            *out = nested;
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

void yonder_scripts_fetched(yonder_scripts_t *s, uint32_t token, const char *source, size_t length)
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
    if (s != NULL)
        s->task_address = NULL;
    if (prevented != NULL)
        *prevented = false;
    if (s == NULL || s->dead)
        return OS64_JS_OK;
    s->written = 0;
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
    if (!os64_dom_listens(s->dom, event->type))
        return OS64_JS_OK;
    os64_dom_dispatch(s->dom, node, event, prevented, out);
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
    }
    if (s == NULL || s->dead || !os64_dom_timer_fire(s->dom, now, out))
        return false;
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

void yonder_scripts_set_user_agent(yonder_scripts_t *s, const char *(*provider)(void *), void *opaque)
{
    if (s == NULL) return;
    s->user_agent = provider;
    s->user_agent_opaque = opaque;
    os64_dom_set_user_agent(s->dom, provider, opaque);
}
