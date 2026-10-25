// modules.c — the page's module map (modules.h).

#include "modules.h"

#include "os64/fmt.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/url.h"
#include "script_job.h"
#include "way/origin.h"

typedef enum {
    MOD_FETCHING,
    // Its source in hand and not compiled yet: it arrived while a script
    // ran (an inline module a script connected, or document.write wrote),
    // and a compile waits for the runtime to be between tasks
    // (yonder_modules_settle).
    MOD_PENDING,
    MOD_COMPILED,           // in the engine under its name, unresolved until an entry runs
    // Fetched for the engine's loader (a dynamic import), which compiled it
    // and everything it imports itself: a whole graph, from the map's side.
    MOD_LOADED,
    // Fetched and typed, and its compile threw: HTML's "error to rethrow".
    // A graph that reaches it is complete; when its entry runs, the engine
    // asks the loader for it, compiles its source again and throws the
    // SyntaxError, which the window hears as a script's error.
    MOD_BROKEN,
    MOD_FAILED                      // not fetched, or not JavaScript: nothing runs
} ModState;

typedef struct {
    // The engine's name for it: the URL asked for (the map's key, as
    // HTML's map is keyed), or an inline module's own.
    char *name;
    // What its imports resolve against: its URL after its redirects; NULL
    // for an inline module, whose imports resolve as the document's links.
    char *base;
    char *meta;             // PENDING: the import.meta.url its compile will give it
    ModState state;
    uint64_t job;           // FETCHING
    char *source;           // the text the loader answers from (CONTRACT.md)
    size_t length;
    os64_js_module_t *handle;
    int32_t *deps;          // the modules its static imports name
    uint32_t ndeps, depcap;
    char *why;              // FAILED
    bool run;               // run as an entry
    uint32_t walk;          // the graph walk that last reached it
} Module;

struct yonder_modules {
    os64_js_runtime_t *runtime;
    yonder_modules_net_t net;
    yonder_diag_t *diag;
    Module *mods;
    uint32_t count, cap;
    size_t bytes;           // the sources kept
    uint32_t walk;
    bool dropped;
    int32_t reading;        // the module whose requests are being read
};

static char *copy(const char *text)
{
    size_t n = os64_strlen(text) + 1;
    char *c = os64_malloc(n);
    if (c != NULL)
        os64_memcpy(c, text, n);
    return c;
}

static int32_t find(const yonder_modules_t *m, const char *name)
{
    for (uint32_t i = 0; i < m->count; i++)
        if (os64_streq(m->mods[i].name, name))
            return (int32_t)i;
    return -1;
}

// A new slot named `name`, FETCHING with no fetch yet: its index, or -1 past
// YONDER_MODULES_MAX or out of memory. Slots are named by index everywhere,
// since a new one may move the table.
static int32_t slot_new(yonder_modules_t *m, const char *name)
{
    if (m->dropped || m->count == YONDER_MODULES_MAX)
        return -1;
    if (m->count == m->cap) {
        uint32_t cap = m->cap != 0 ? m->cap * 2 : 16;
        Module *mods = os64_realloc(m->mods, cap * sizeof(*mods));
        if (mods == NULL)
            return -1;
        m->mods = mods;
        m->cap = cap;
    }
    char *kept = copy(name);
    if (kept == NULL)
        return -1;
    Module *mod = &m->mods[m->count];
    os64_memset(mod, 0, sizeof(*mod));
    mod->name = kept;
    mod->state = MOD_FETCHING;
    return (int32_t)m->count++;
}

// What went wrong with the module, on the page's record under the script
// key the page's other scripts use.
static void record(yonder_modules_t *m, int32_t i, const char *why)
{
    char key[OS64_DOM_URL_MAX + 16];
    os64_snprintf(key, sizeof(key), "script %s", m->mods[i].name);
    yonder_diag_failed(m->diag, key, why);
}

// The module failed, said once on the record.
static void fail(yonder_modules_t *m, int32_t i, const char *why)
{
    Module *mod = &m->mods[i];
    if (mod->state == MOD_FAILED)
        return;
    mod->state = MOD_FAILED;
    mod->job = 0;
    os64_free(mod->why);
    mod->why = copy(why);
    record(m, i, why);
}

static void fetch_start(yonder_modules_t *m, int32_t i)
{
    uint64_t job = m->net.start != NULL
        ? m->net.start(m->net.opaque, m->mods[i].name, YONDER_MODULE_TOKEN | (uint32_t)(i + 1)) : 0;
    if (job == 0)
        fail(m, i, "the module could not be fetched");
    else
        m->mods[i].job = job;
}

static bool relative(const char *specifier)
{
    return specifier[0] == '/' || (specifier[0] == '.' && specifier[1] == '/') ||
           (specifier[0] == '.' && specifier[1] == '.' && specifier[2] == '/');
}

// Whether `text` begins with a scheme and its colon: an absolute URL.
static bool absolute(const char *text)
{
    bool alpha = (text[0] | 0x20) >= 'a' && (text[0] | 0x20) <= 'z';
    if (!alpha)
        return false;
    for (size_t i = 1; text[i] != '\0'; i++) {
        char c = text[i];
        if (c == ':')
            return true;
        bool ok = ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                  c == '.';
        if (!ok)
            return false;
    }
    return false;
}

// HTML's "resolve a module specifier", without import maps: a URL relative
// to `base` (NULL: to the document, as an inline script's are), or an
// absolute one. A bare name ("lodash") names nothing: false.
static bool resolve_against(yonder_modules_t *m, const char *base, const char *specifier, char *out, size_t cap)
{
    if (relative(specifier)) {
        if (base == NULL)
            return m->net.resolve != NULL && m->net.resolve(m->net.opaque, specifier, out, cap);
        os64_url_t parsed;
        return os64_url_parse(base, &parsed) == OS64_URL_OK && os64_url_absolute(&parsed, specifier, out, cap);
    }
    if (!absolute(specifier))
        return false;
    return os64_strcopy(out, cap, specifier) < cap;
}

// A specifier as the module in slot `from` means it.
static bool resolve_specifier(yonder_modules_t *m, int32_t from, const char *specifier, char *out, size_t cap)
{
    return resolve_against(m, m->mods[from].base, specifier, out, cap);
}

// Whether a header's name (not NUL-terminated) is `want`, in any case.
static bool header_named(const char *name, size_t length, const char *want)
{
    if (length != os64_strlen(want))
        return false;
    for (size_t i = 0; i < length; i++) {
        char a = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + 32) : name[i];
        if (a != want[i])
            return false;
    }
    return true;
}

// A response a module may be made from (HTML's "fetch a single module
// script"): ok, a 2xx status, typed as JavaScript, no longer than a script
// may be. Its text read as UTF-8 into `*source`, or `why` says what was
// wrong.
static bool accept(const os64_dom_response_t *response, char **source, size_t *length, const char **why)
{
    *source = NULL;
    if (!response->ok) {
        *why = "the module could not be fetched";
        return false;
    }
    if (response->status < 200 || response->status > 299) {
        *why = "the module's server did not answer it";
        return false;
    }
    bool typed = false;
    way_header_t h;
    for (size_t at = 0; way_header_next(response->headers, response->headers_length, &at, &h);) {
        if (header_named(h.name, h.name_len, "content-type"))
            typed = os64_dom_javascript_mime(h.value, h.value_len);
    }
    if (!typed) {
        *why = "the module is not typed as JavaScript";
        return false;
    }
    if (!response->has_body || response->body_length > SCRIPT_SOURCE_MAX) {
        *why = response->has_body ? "the module is longer than a script may be" : "the module has no body";
        return false;
    }
    *source = yonder_script_decode(response->body, response->body_length, "utf-8", NULL, length);
    if (*source == NULL) {
        *why = "no memory for the module's source";
        return false;
    }
    return true;
}

static void each_request(void *opaque, const char *specifier);

// The module's source compiled under its name with import.meta.url `url`,
// and its imports found or fetched. A runtime in a call (a script is
// running) cannot compile: the module waits, PENDING, for settle.
static void try_compile(yonder_modules_t *m, int32_t i, const char *url)
{
    os64_js_outcome_t out;
    os64_js_module_t *handle = NULL;
    os64_js_status_t status = os64_js_module_compile(m->runtime, OS64_JS_ABI_ID, m->mods[i].source,
                                                     m->mods[i].length, m->mods[i].name, url, &handle, &out);
    if (status == OS64_JS_BUSY) {
        if (m->mods[i].meta == NULL && (m->mods[i].meta = copy(url)) == NULL) {
            fail(m, i, "no memory for the module's address");
            return;
        }
        m->mods[i].state = MOD_PENDING;
        return;
    }
    if (handle == NULL && status == OS64_JS_EXCEPTION) {
        // A syntax error: the module's to rethrow when an entry runs.
        m->mods[i].state = MOD_BROKEN;
        record(m, i, out.message[0] != '\0' ? out.message : "the module did not compile");
        return;
    }
    if (handle == NULL) {
        fail(m, i, out.message[0] != '\0' ? out.message : "the module did not compile");
        return;
    }
    m->mods[i].handle = handle;
    m->mods[i].state = MOD_COMPILED;
    m->reading = i;
    os64_js_module_requests(m->runtime, OS64_JS_ABI_ID, handle, each_request, m, &out);
    m->reading = -1;
    if (out.status != OS64_JS_OK)
        fail(m, i, "the module's imports could not be read");
}

// The module's source, its own now (`source` is moved in), kept for the
// loader and compiled (try_compile).
static void compile(yonder_modules_t *m, int32_t i, char *source, size_t length, const char *url)
{
    if (length > YONDER_MODULES_BYTES_MAX - m->bytes) {
        os64_free(source);
        fail(m, i, "the page's modules are more than yonder keeps");
        return;
    }
    m->mods[i].source = source;
    m->mods[i].length = length;
    m->bytes += length;
    try_compile(m, i, url);
}

// One of the reading module's static imports: resolved, found in the map
// or fetched, and named among its dependencies.
static void each_request(void *opaque, const char *specifier)
{
    yonder_modules_t *m = opaque;
    int32_t from = m->reading;
    if (from < 0 || m->mods[from].state != MOD_COMPILED)
        return;
    char url[OS64_DOM_URL_MAX];
    if (!resolve_specifier(m, from, specifier, url, sizeof(url))) {
        // Not the graph's failure: the run resolves it again and throws a
        // TypeError, which the window hears (HTML's specifier error).
        char why[160];
        os64_snprintf(why, sizeof(why), "the import \"%.100s\" names no module (a bare name needs an import map)",
                      specifier);
        record(m, from, why);
        return;
    }
    int32_t dep = find(m, url);
    if (dep < 0) {
        dep = slot_new(m, url);
        if (dep < 0) {
            fail(m, from, "the page has more modules than yonder keeps");
            return;
        }
        fetch_start(m, dep);
    }
    Module *mod = &m->mods[from];
    if (mod->ndeps == mod->depcap) {
        uint32_t cap = mod->depcap != 0 ? mod->depcap * 2 : 4;
        int32_t *deps = os64_realloc(mod->deps, cap * sizeof(*deps));
        if (deps == NULL) {
            fail(m, from, "no memory for the module's imports");
            return;
        }
        mod->deps = deps;
        mod->depcap = cap;
    }
    mod->deps[mod->ndeps++] = dep;
}

// THE ENGINE'S LOADER (libjs CONTRACT.md § Modules). resolve: a specifier
// as the module named `base` means it. source: a module's text, from the
// map when it holds it (the engine unlists modules a failed run had not
// resolved, and asks again), else fetched now on this thread, which is how
// a dynamic import() of a module no graph reached arrives.
static char *engine_resolve(void *opaque, const char *base, const char *specifier)
{
    yonder_modules_t *m = opaque;
    int32_t from = find(m, base);
    char url[OS64_DOM_URL_MAX];
    // A base that is no module is a classic script's dynamic import(): it
    // resolves against that script's address (the page's when inline).
    const char *script = from < 0 && m->net.script_base != NULL ? m->net.script_base(m->net.opaque, base) : NULL;
    bool resolved = from >= 0 ? resolve_specifier(m, from, specifier, url, sizeof(url))
                              : resolve_against(m, script, specifier, url, sizeof(url));
    return resolved ? copy(url) : NULL;
}

static bool engine_source(void *opaque, const char *name, const char **bytes, size_t *length)
{
    yonder_modules_t *m = opaque;
    int32_t i = find(m, name);
    if (i >= 0 && m->mods[i].source != NULL) {
        *bytes = m->mods[i].source;
        *length = m->mods[i].length;
        return true;
    }
    // A BROKEN module's source is answered above, and compiling it throws.
    if (i >= 0 && m->mods[i].state == MOD_FAILED)
        return false;
    if (i < 0 && (i = slot_new(m, name)) < 0)
        return false;
    // Not here yet: fetched now, its async fetch (if one is out) let go.
    if (m->mods[i].job != 0 && m->net.cancel != NULL)
        m->net.cancel(m->net.opaque, m->mods[i].job);
    m->mods[i].job = 0;
    os64_dom_response_t response;
    if (m->net.run == NULL || !m->net.run(m->net.opaque, name, &response)) {
        fail(m, i, "the module could not be fetched");
        return false;
    }
    char *source;
    size_t got;
    const char *why = NULL;
    bool ok = accept(&response, &source, &got, &why);
    char *base = ok ? copy(response.url != NULL && response.url[0] != '\0' ? response.url : name) : NULL;
    if (m->net.release != NULL)
        m->net.release(m->net.opaque, &response);
    if (!ok || base == NULL || got > YONDER_MODULES_BYTES_MAX - m->bytes) {
        os64_free(source);
        os64_free(base);
        fail(m, i, ok ? "the page's modules are more than yonder keeps" : why);
        return false;
    }
    m->mods[i].base = base;
    m->mods[i].source = source;
    m->mods[i].length = got;
    m->bytes += got;
    m->mods[i].state = MOD_LOADED;
    *bytes = source;
    *length = got;
    return true;
}

yonder_modules_t *yonder_modules_new(os64_js_runtime_t *runtime, const yonder_modules_net_t *net,
                                     yonder_diag_t *diag)
{
    if (runtime == NULL || net == NULL)
        return NULL;
    yonder_modules_t *m = os64_calloc(1, sizeof(*m));
    if (m == NULL)
        return NULL;
    m->runtime = runtime;
    m->net = *net;
    m->diag = diag;
    m->reading = -1;
    os64_js_outcome_t out;
    const os64_js_module_loader_t loader = {m, engine_resolve, engine_source};
    if (os64_js_set_module_loader(runtime, OS64_JS_ABI_ID, &loader, &out) != OS64_JS_OK) {
        os64_free(m);
        return NULL;
    }
    return m;
}

void yonder_modules_drop(yonder_modules_t *m)
{
    if (m == NULL || m->dropped)
        return;
    m->dropped = true;
    os64_js_outcome_t out;
    (void)os64_js_set_module_loader(m->runtime, OS64_JS_ABI_ID, NULL, &out);
    for (uint32_t i = 0; i < m->count; i++) {
        Module *mod = &m->mods[i];
        if (mod->job != 0 && m->net.cancel != NULL)
            m->net.cancel(m->net.opaque, mod->job);
        mod->job = 0;
        os64_js_module_release(m->runtime, mod->handle);
        mod->handle = NULL;
    }
    m->runtime = NULL;
}

void yonder_modules_free(yonder_modules_t *m)
{
    if (m == NULL)
        return;
    for (uint32_t i = 0; i < m->count; i++) {
        os64_free(m->mods[i].name);
        os64_free(m->mods[i].base);
        os64_free(m->mods[i].meta);
        os64_free(m->mods[i].source);
        os64_free(m->mods[i].deps);
        os64_free(m->mods[i].why);
    }
    os64_free(m->mods);
    os64_free(m);
}

int32_t yonder_modules_entry_src(yonder_modules_t *m, const char *url)
{
    if (m == NULL || m->dropped || url == NULL)
        return -1;
    int32_t i = find(m, url);
    if (i >= 0)
        return i;
    i = slot_new(m, url);
    if (i >= 0)
        fetch_start(m, i);
    return i;
}

int32_t yonder_modules_entry_inline(yonder_modules_t *m, const char *source, size_t length, const char *name,
                                    const char *url)
{
    if (m == NULL || m->dropped || source == NULL || name == NULL)
        return -1;
    int32_t i = slot_new(m, name);
    if (i < 0)
        return -1;
    char *text = os64_malloc(length + 1);
    if (text == NULL) {
        fail(m, i, "no memory for the module's source");
        return i;
    }
    os64_memcpy(text, source, length);
    text[length] = '\0';
    compile(m, i, text, length, url);
    return i;
}

yonder_graph_t yonder_modules_graph(yonder_modules_t *m, int32_t entry, const char **why)
{
    *why = NULL;
    if (m == NULL || entry < 0 || (uint32_t)entry >= m->count) {
        *why = "the module could not be begun";
        return YONDER_GRAPH_FAILED;
    }
    // Iterative: a chain of imports is as deep as a page makes it. Each
    // module is pushed once per walk, so `count` slots hold the stack.
    int32_t *stack = os64_malloc(m->count * sizeof(*stack));
    if (stack == NULL) {
        *why = "no memory to walk the module's imports";
        return YONDER_GRAPH_FAILED;
    }
    uint32_t walk = ++m->walk, top = 0;
    yonder_graph_t result = YONDER_GRAPH_READY;
    stack[top++] = entry;
    m->mods[entry].walk = walk;
    while (top > 0) {
        Module *mod = &m->mods[stack[--top]];
        if (mod->state == MOD_FAILED) {
            *why = mod->why != NULL ? mod->why : "a module it imports failed";
            result = YONDER_GRAPH_FAILED;
            break;
        }
        if (mod->state == MOD_FETCHING || mod->state == MOD_PENDING) {
            result = YONDER_GRAPH_FETCHING;
            continue;               // a failure further on still outranks it
        }
        for (uint32_t d = 0; d < mod->ndeps; d++) {
            Module *dep = &m->mods[mod->deps[d]];
            if (dep->walk != walk) {
                dep->walk = walk;
                stack[top++] = mod->deps[d];
            }
        }
    }
    os64_free(stack);
    return result;
}

void yonder_modules_answered(yonder_modules_t *m, uint32_t token, const os64_dom_response_t *response)
{
    if (m == NULL || m->dropped || (token & YONDER_MODULE_TOKEN) == 0 || response == NULL)
        return;
    uint32_t slot = (token & ~YONDER_MODULE_TOKEN) - 1;
    if (slot >= m->count || m->mods[slot].state != MOD_FETCHING || m->mods[slot].job == 0)
        return;
    m->mods[slot].job = 0;
    char *source;
    size_t length;
    const char *why = NULL;
    if (!accept(response, &source, &length, &why)) {
        fail(m, (int32_t)slot, why);
        return;
    }
    char *base = copy(response->url != NULL && response->url[0] != '\0' ? response->url : m->mods[slot].name);
    if (base == NULL) {
        os64_free(source);
        fail(m, (int32_t)slot, "no memory for the module's address");
        return;
    }
    m->mods[slot].base = base;
    compile(m, (int32_t)slot, source, length, base);
}

bool yonder_modules_settle(yonder_modules_t *m)
{
    if (m == NULL || m->dropped)
        return false;
    bool any = false;
    for (uint32_t i = 0; i < m->count; i++)
        if (m->mods[i].state == MOD_PENDING) {
            try_compile(m, (int32_t)i, m->mods[i].meta);
            any = true;
        }
    return any;
}

bool yonder_modules_owed(const yonder_modules_t *m)
{
    if (m == NULL || m->dropped)
        return false;
    for (uint32_t i = 0; i < m->count; i++)
        if (m->mods[i].state == MOD_PENDING)
            return true;
    return false;
}

// An entry whose own compile threw has no module to run: a one-line module
// importing it runs in its place, so the engine compiles it again and
// throws, as it does for a graph that reaches a broken module. Kept as the
// entry's handle. NULL on no memory.
static os64_js_module_t *rethrower(yonder_modules_t *m, int32_t entry)
{
    char source[2 * OS64_DOM_URL_MAX + 16], name[OS64_DOM_URL_MAX + 16];
    size_t at = 0;
    const char *head = "import '";
    while (*head != '\0')
        source[at++] = *head++;
    for (const char *p = m->mods[entry].name; *p != '\0' && at + 4 < sizeof(source); p++) {
        if (*p == '\'' || *p == '\\')
            source[at++] = '\\';
        source[at++] = *p;
    }
    source[at++] = '\'';
    source[at++] = ';';
    source[at] = '\0';
    os64_snprintf(name, sizeof(name), "%s#rethrow", m->mods[entry].name);
    os64_js_outcome_t out;
    os64_js_module_t *handle = NULL;
    os64_js_module_compile(m->runtime, OS64_JS_ABI_ID, source, at, name, NULL, &handle, &out);
    return handle;
}

os64_js_status_t yonder_modules_run(yonder_modules_t *m, int32_t entry, os64_js_outcome_t *outcome)
{
    os64_memset(outcome, 0, sizeof(*outcome));
    if (m == NULL || m->dropped || entry < 0 || (uint32_t)entry >= m->count) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return outcome->status;
    }
    if (m->mods[entry].run)
        return OS64_JS_OK;
    if (m->mods[entry].handle == NULL && m->mods[entry].state == MOD_BROKEN)
        m->mods[entry].handle = rethrower(m, entry);
    if (m->mods[entry].handle == NULL) {
        // The script's failure, not the runtime's (a sticky status would
        // retire the page's scripts for one module).
        outcome->status = OS64_JS_EXCEPTION;
        os64_strcopy(outcome->message, sizeof(outcome->message), "no memory to run the module");
        return outcome->status;
    }
    m->mods[entry].run = true;
    return os64_js_module_run(m->runtime, OS64_JS_ABI_ID, m->mods[entry].handle, outcome);
}
