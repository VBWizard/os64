#ifndef LIBDOM_LIFETIME_H
#define LIBDOM_LIFETIME_H
#include "browsing_context.h"
#include "way/origin.h"
#include "../libjs/runtime/owner_runtime.h"

typedef struct {
    size_t window_proxy_roots, window_descriptor_roots;
    size_t location_backing_roots, location_descriptor_roots;
    size_t window_identity_aliases;
} DExposureCounts;
typedef struct {
    JSOwnerRealmSnapshot relevant, active, last_window;
    JSOwnerContextId context, parent;
    uint64_t generation;
    bool associated, discarded, has_active, active_eligible, has_last_window;
    bool navigation_cell, navigation_pending;
    way_origin_t relevant_origin, active_origin, last_window_origin;
    bool has_relevant_origin, has_active_origin, has_last_window_origin;
    DExposureCounts known;
    /* Enumerated native categories above do not enumerate escaped JS roots,
     * graph reachability or the complete native borrower population. */
    bool escaped_unknown, graph_unknown, native_borrows_unknown;
} DLifetimeSnapshot;
typedef struct {
    uint64_t incarnation;
    size_t capacity, bindings, contexts, charged_bytes;
    bool closed;
} DLifetimeSummary;
/* Explicit private host bridge across the library boundary. These trusted
 * wrappers describe this cluster's selected runtime, convert its original raw
 * realms through compatibility admission, copy owner facts and issue context
 * IDs. They execute no author/provider code; scalar calls allocate/poll
 * nothing. The table is copied; opaque backing lives until cluster drain and
 * any admitted bridge call returns. Cluster/runtime shells cannot be freed
 * from a bridge call. Native drain/poison during a call prevents publication.
 * No hidden libjs symbol is imported by libdom or added to its public ABI. */
typedef struct {
    void *opaque;
    bool (*describe)(void *, os64_js_runtime_t *, uint64_t *incarnation);
    bool (*map)(void *, const os64_js_realm_t *, JSOwnerId *);
    bool (*facts)(void *, JSOwnerId, JSOwnerRealmSnapshot *);
    bool (*context_id)(void *, JSOwnerContextId *);
} DLifetimeHost;

/* Private owner-thread diagnostic adapter. Select once at quiescence on a
 * live cluster with an explicitly selected runtime owner arena. Storage is
 * bounded/charged and retained until native cluster free. The host follows
 * cluster drain -> engine destroy -> cluster free. No public activation. */
__attribute__((visibility("hidden")))
os64_js_status_t d_lifetime_select(os64_dom_cluster_t *, size_t capacity, const DLifetimeHost *, os64_js_outcome_t *);
/* Register a validated original binding and optionally its already associated
 * context. Parent contexts must be registered first. Compatibility realm
 * conversion keeps the sticky raw-retention veto. Outputs are scalar copies,
 * outside adapter/cluster storage, unchanged on refusal; IDs pin nothing. */
__attribute__((visibility("hidden")))
os64_js_status_t d_lifetime_register(os64_dom_cluster_t *, os64_dom_t *, JSOwnerId *out, os64_js_outcome_t *);
/* Read-only, allocation-free scalar observation. The expected publication is
 * required (zero for unassociated); missing mappings, busy/native departure,
 * drain, poison or stale generation refuse before publishing output. The
 * relevant owner remains the original binding across context replacement.
 * Zero known counts are not a release certificate or authority check. */
__attribute__((visibility("hidden")))
os64_js_status_t d_lifetime_observe(os64_dom_cluster_t *, JSOwnerId relevant, uint64_t expected,
    DLifetimeSnapshot *out, os64_js_outcome_t *);
/* Scalar summary remains available while the cluster shell lives, including
 * after engine destruction. It consults no runtime or native saved identity. */
__attribute__((visibility("hidden")))
bool d_lifetime_summary(const os64_dom_cluster_t *, DLifetimeSummary *out);
#endif
