#ifndef LIBDOM_NAVIGATION_AUTHORITY_H
#define LIBDOM_NAVIGATION_AUTHORITY_H
#include "browsing_context.h"

/* Private host policy, independent of cross-origin member access. These native
 * records hold no JS values. Binding/context backing stays cluster-retained. */
typedef enum { D_NAV_HREF, D_NAV_ASSIGN, D_NAV_REPLACE, D_NAV_HASH, D_NAV_RELOAD } DNavigationAction;
typedef enum { D_NAV_ENQUEUE, D_NAV_DISPATCH } DNavigationPhase;
typedef struct {
    bool transient_activation, target_loaded;
    uint64_t restrictions;
} DNavigationSnapshot;
typedef struct {
    os64_dom_t *source, *entry, *target;
    DBrowsingContext *source_context, *entry_context, *target_context;
    uint64_t source_generation, entry_generation, target_generation, policy_generation;
    DNavigationAction action;
    os64_dom_navigation_kind_t kind;
    DNavigationSnapshot snapshot;
    char url[OS64_DOM_URL_MAX];
} DNavigationRequest;
typedef struct {
    void *opaque;
    bool (*snapshot)(void *, os64_dom_t *source, os64_dom_t *target, DNavigationSnapshot *);
    bool (*allow)(void *, const DNavigationRequest *, DNavigationPhase);
} DNavigationPolicy;
/* Configure/revoke at quiescence. Host storage must remain live until a
 * successful replacement/revocation or root teardown. Changes invalidate asks. */
__attribute__((visibility("hidden")))
os64_js_status_t d_navigation_policy(os64_dom_cluster_t *, const DNavigationPolicy *, os64_js_outcome_t *);
/* One last-successful ask per context. Dispatch consumes it and rechecks policy
 * and source/entry/target generations before calling the host under provider
 * loans. The request is borrowed for that callback only. Async loaders must
 * carry generations and revalidate before their eventual commit (F.7). */
__attribute__((visibility("hidden")))
bool d_navigation_dispatch(DBrowsingContext *, void (*consume)(void *, const DNavigationRequest *),
    void *, os64_js_outcome_t *);
#endif
