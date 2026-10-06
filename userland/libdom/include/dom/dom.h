#ifndef OS64_DOM_H
#define OS64_DOM_H

#include <html/html.h>
#include <page/page.h>
#include <os64/js.h>

typedef struct os64_dom os64_dom_t;

#define OS64_DOM_DEFAULT_MAX_BYTES ((size_t)8 * 1024 * 1024)

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
} os64_dom_options_t;

static inline os64_dom_options_t os64_dom_default_options(void)
{
    const os64_dom_options_t options = {
        OS64_DOM_DEFAULT_MAX_BYTES, true, NULL, NULL
    };
    return options;
}

/* Numeric snapshot in CSS pixels. Offset/client members are integer pixels;
 * rect members can be fractional after device-pixel layout is unzoomed.
 * offset_parent is borrowed from the provider's live document. */
typedef struct {
    double x, y, width, height;
    int32_t offset_left, offset_top, offset_width, offset_height;
    int32_t client_left, client_top, client_width, client_height;
    const os64_html_node_t *offset_parent;
    uint64_t layouts, elapsed_us;
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

#pragma GCC visibility pop
#endif
