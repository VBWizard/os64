#ifndef YONDER_MODULES_H
#define YONDER_MODULES_H

// THE PAGE'S MODULE MAP (HTML § "fetch a module script graph"; MODERN_WEB.md
// § Slice 3): every module a page's module scripts reach, by its URL, each
// fetched once, compiled as it lands, and found by that name when a module
// that imports it runs (libjs CONTRACT.md § Modules). IFRAME.md: the map is
// per realm, and a page's realm is its document, so each script host has
// one. A graph is READY when its entry and everything it imports, all the
// way down, have been fetched and compiled; FAILED when any of them could
// not be fetched or was not JavaScript, and then nothing of it runs. A
// module whose compile threw, and an import that names no module, do not
// fail the graph: its run throws the SyntaxError or TypeError, which the
// window hears as a script's error (HTML's error to rethrow).
//
// A specifier means what the window's import map says (importmap.h), which
// the map holds: every import, static or dynamic, resolves through it.
//
// A module is fetched as HTML fetches one: a CORS GET through the page's
// own request path (scripts.h's options.request, slice 2's fetch job), so
// its origin rules, cookies and redirects are a script's request's. Its
// response must be ok and typed as JavaScript, and is read as UTF-8.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dom/dom.h>
#include "os64/js_engine.h"

#include "bytecode.h"
#include "diag.h"
#include "importmap.h"

typedef struct yonder_modules yonder_modules_t;

// A module fetch's token carries this bit, so the page's request answers
// (yonder_scripts_answered) tell a module's from its scripts' own requests,
// whose tokens libdom counts up from 1.
#define YONDER_MODULE_TOKEN 0x80000000u

// The most modules one page's graphs may hold, and the most source bytes
// the map keeps for them (the loader answers from them: CONTRACT.md).
#define YONDER_MODULES_MAX 2048
#define YONDER_MODULES_BYTES_MAX ((size_t)48 * 1024 * 1024)

typedef struct {
    void *opaque;
    // The fetch of the module at `url` for map token `token` (a CORS GET),
    // its answer through yonder_modules_answered; 0 when it was not begun.
    uint64_t (*start)(void *opaque, const char *url, uint32_t token);
    void (*cancel)(void *opaque, uint64_t id);
    // A dynamic import's fetch, run to its end on this thread inside the
    // task that asked (the task's own budget stops it), `*response` the
    // host's until `release`; false for a network error.
    bool (*run)(void *opaque, const char *url, os64_dom_response_t *response);
    void (*release)(void *opaque, os64_dom_response_t *response);
    // The document's base URL as the tree stands (its first <base href>,
    // else its address): an inline module's base URL, taken when it is
    // prepared, and a classic inline script's when it imports. False when
    // it cannot be read or will not fit in `cap`.
    bool (*base)(void *opaque, char *out, size_t cap);
    // The address of the classic script the engine knows by `name`
    // (os64_dom_script_address), which a dynamic import() in it resolves
    // against; NULL for an inline one, which resolves against the
    // document's base URL (`base`).
    const char *(*script_base)(void *opaque, const char *name);
    // A module compiled from the source whose body the cache holds: its
    // blob (libjs's, taken) to be kept where `where` says (BYTECODE_CACHE.md
    // § 9). NULL keeps nothing.
    void (*keep)(void *opaque, const yonder_bytecode_t *where, uint8_t *blob, size_t length);
} yonder_modules_net_t;

// A map for a page, its record `diag` (borrowed; NULL keeps none). NULL on
// no memory. The modules compile in `runtime`, the page's, which must
// outlive the map's handles: yonder_modules_drop lets them go before the
// runtime is destroyed.
yonder_modules_t *yonder_modules_new(os64_js_runtime_t *runtime, const yonder_modules_net_t *net,
                                     yonder_diag_t *diag);
// Every fetch still out cancelled and every module handle let go: before
// the runtime is destroyed. The map stays, empty and refusing, until freed.
void yonder_modules_drop(yonder_modules_t *m);
void yonder_modules_free(yonder_modules_t *m);

// An entry: a module script with `src`, its graph fetched from `url` (one
// already in the map is shared, as HTML's map shares it), or an inline
// one, compiled from `source` (copied) under `name` (one of its own, since
// the engine finds modules by name), the document's base URL now its base
// URL and its import.meta.url, as HTML fixes an inline module script's when
// it is prepared: now, or at the next settle when a script is running. The
// entry's id, or -1 when the map is full or out of memory.
int32_t yonder_modules_entry_src(yonder_modules_t *m, const char *url);
int32_t yonder_modules_entry_inline(yonder_modules_t *m, const char *source, size_t length, const char *name);

// HTML's "register an import map", for a map that parsed: `parsed` (a
// parse result, importmap.h; consumed) merged into the window's map, which
// every resolution from now on reads. False when the map is dropped or
// memory ran out (what merged before it did stays).
bool yonder_modules_import_map(yonder_modules_t *m, yonder_import_map_t *parsed);

typedef enum { YONDER_GRAPH_FETCHING, YONDER_GRAPH_READY, YONDER_GRAPH_FAILED } yonder_graph_t;
// How `entry`'s graph stands; FAILED with what failed in `*why` (the map's,
// good until it is freed).
yonder_graph_t yonder_modules_graph(yonder_modules_t *m, int32_t entry, const char **why);

// The answer to module fetch `token` (YONDER_MODULE_TOKEN set), borrowed:
// the module is compiled, and its imports fetched. Runs no script.
// `bytecode`, its compiled form when the cache holds its body, is TAKEN:
// the module is read from its blob when the blob is for this module, and a
// compile of it kept where it says.
void yonder_modules_answered(yonder_modules_t *m, uint32_t token, const os64_dom_response_t *response,
                             yonder_bytecode_t *bytecode);

// A module whose source arrived while a script ran (an inline module a
// script connected, or document.write wrote) cannot be compiled until the
// runtime is between tasks; and a fetch a dynamic import() took over from
// a graph's has no answer to come, so the graphs waiting on it must be
// walked again. owed says either waits; settle compiles the modules
// (outside every task) and clears the walk owed, true when it did either,
// which is the caller's cue to walk its graphs.
bool yonder_modules_owed(const yonder_modules_t *m);
bool yonder_modules_settle(yonder_modules_t *m);

// `entry`'s graph, READY, run inside the task the caller has open
// (os64_js_module_run's outcome). An entry whose own compile threw runs a
// one-line module importing it, which throws its error. An entry already
// run is OK: HTML evaluates a module once.
os64_js_status_t yonder_modules_run(yonder_modules_t *m, int32_t entry, os64_js_outcome_t *outcome);

#endif
