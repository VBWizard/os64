/* Private native publication. No engine entry or host callback may intervene
 * between the quiescence check and the allocation-free retirement/commit. */
_Static_assert(D_REALMS <= OS64_JS_MAX_REALMS, "context cap fits retained realm namespace");

static bool context_member(const os64_dom_cluster_t *cluster, const DBrowsingContext *context)
{
    for (const DBrowsingContext *at = cluster->contexts; at; at = at->registry_next)
        if (at == context) return true;
    return false;
}

os64_js_status_t d_browsing_quiescent(os64_dom_cluster_t *cluster, os64_js_outcome_t *outcome)
{
    os64_js_status_t status = os64_js_quiescent(cluster ? cluster->runtime : NULL,
        OS64_JS_ABI_ID, outcome);
    if (status != OS64_JS_OK) return status;
    if (cluster->poisoned || cluster->drained) return outcome->status = OS64_JS_FAILED_RUNTIME;
    if (cluster->recovering || cluster->provider_borrows || cluster->dispatching)
        return outcome->status = OS64_JS_BUSY;
    for (size_t i = 0; i < cluster->count; ++i)
        if (cluster->members[i]->provider_borrows || departure_busy(cluster->members[i]))
            return outcome->status = OS64_JS_BUSY;
    return OS64_JS_OK;
}

static bool context_candidate(os64_dom_cluster_t *cluster, os64_dom_t *binding)
{
    bool member = false;
    for (size_t i = 0; i < cluster->count; ++i)
        if (cluster->members[i] == binding) member = true;
    return member && binding->binding_origin && !binding->browsing_context && d_delivery_active(binding);
}

DBrowsingContext *d_browsing_context_create(os64_dom_cluster_t *cluster, DBrowsingContext *parent,
    os64_dom_t *initial, const char *name, os64_js_outcome_t *outcome)
{
    if (d_browsing_quiescent(cluster, outcome) != OS64_JS_OK) return NULL;
    if (!name || !context_candidate(cluster, initial) ||
        (parent && (!context_member(cluster, parent) || parent->discarded || !d_delivery_active(parent->active)))) {
        outcome->status = OS64_JS_BAD_ARGUMENT;
        return NULL;
    }
    if (cluster->context_count == D_REALMS) { outcome->status = OS64_JS_REALM_LIMIT; return NULL; }
    size_t length = 0;
    while (length <= D_CONTEXT_NAME_MAX && name[length]) ++length;
    if (length > D_CONTEXT_NAME_MAX) { outcome->status = OS64_JS_BAD_ARGUMENT; return NULL; }
    size_t bytes = sizeof(DBrowsingContext) + length + 1;
    if (bytes > cluster->limits.dom_bytes - cluster->bytes) {
        outcome->status = OS64_JS_ADMISSION_REFUSED;
        return NULL;
    }
    DBrowsingContext *context = os64_calloc(1, bytes);
    if (!context) { outcome->status = OS64_JS_ADMISSION_REFUSED; return NULL; }
    context->cluster = cluster;
    context->parent = parent;
    context->active = initial;
    context->generation = 1;
    context->charged_bytes = bytes;
    os64_memcpy(context->name, name, length + 1);
    context->registry_next = cluster->contexts;
    cluster->contexts = context;
    cluster->context_count++;
    cluster->bytes += bytes;
    if (parent) {
        context->previous_sibling = parent->last_child;
        if (parent->last_child) parent->last_child->next_sibling = context;
        else parent->first_child = context;
        parent->last_child = context;
    }
    initial->browsing_context = context;
    return context;
}

static bool context_descendant(const DBrowsingContext *context, const DBrowsingContext *ancestor)
{
    for (const DBrowsingContext *at = context->parent; at; at = at->parent)
        if (at == ancestor) return true;
    return false;
}

static void context_unlink(DBrowsingContext *context)
{
    if (context->previous_sibling) context->previous_sibling->next_sibling = context->next_sibling;
    else if (context->parent) context->parent->first_child = context->next_sibling;
    if (context->next_sibling) context->next_sibling->previous_sibling = context->previous_sibling;
    else if (context->parent) context->parent->last_child = context->previous_sibling;
    context->previous_sibling = context->next_sibling = NULL;
}

static void context_retire(os64_dom_t *binding)
{
    if (!binding) return;
    os64_js_outcome_t out;
    /* Membership and quiescence are already established. Retirement is an
     * allocation-free mark, with a fresh outcome and no callbacks; refusal
     * here would violate the native transaction's embedding invariants. */
    if (os64_dom_depart(binding, &out) != OS64_JS_OK) os64_exit((int32_t)OS64_PAGE_FATAL_EXIT);
}

static os64_js_status_t context_change(DBrowsingContext *context, uint64_t expected,
    os64_dom_t *candidate, bool discard, os64_js_outcome_t *outcome)
{
    os64_dom_cluster_t *cluster = context ? context->cluster : NULL;
    os64_js_status_t status = d_browsing_quiescent(cluster, outcome);
    if (status != OS64_JS_OK) return status;
    if (!context_member(cluster, context) || context->discarded ||
        expected != context->generation || (!discard && !context_candidate(cluster, candidate)))
        return outcome->status = OS64_JS_BAD_ARGUMENT;
    if (context->generation == UINT64_MAX) return outcome->status = OS64_JS_ADMISSION_REFUSED;
    /* Check the complete descendant set before retiring any document. A name
     * or script-facing cache is not consulted while selecting these records. */
    for (DBrowsingContext *at = cluster->contexts; at; at = at->registry_next)
        if (!at->discarded && context_descendant(at, context) && at->generation == UINT64_MAX)
            return outcome->status = OS64_JS_ADMISSION_REFUSED;
    context_retire(context->active);
    for (DBrowsingContext *at = cluster->contexts; at; at = at->registry_next) {
        if (at->discarded || !context_descendant(at, context)) continue;
        context_retire(at->active);
        context_unlink(at);
        at->active = NULL;
        at->discarded = true;
        at->generation++;
    }
    context->active = discard ? NULL : candidate;
    context->generation++;
    if (discard) {
        context_unlink(context);
        context->discarded = true;
    } else candidate->browsing_context = context;
    return OS64_JS_OK;
}

os64_js_status_t d_browsing_context_replace(DBrowsingContext *context, uint64_t expected,
    os64_dom_t *candidate, os64_js_outcome_t *outcome)
{
    return context_change(context, expected, candidate, false, outcome);
}

os64_js_status_t d_browsing_context_discard(DBrowsingContext *context, uint64_t expected,
    os64_js_outcome_t *outcome)
{
    return context_change(context, expected, NULL, true, outcome);
}

os64_dom_t *d_browsing_context_target(const DBrowsingContext *context, uint64_t expected)
{
    return context && !context->discarded && expected == context->generation &&
        d_delivery_active(context->active) ? context->active : NULL;
}

uint64_t d_browsing_context_generation(const DBrowsingContext *context)
{
    return context ? context->generation : 0;
}

DBrowsingContext *d_browsing_context_parent(const DBrowsingContext *context)
{
    return context ? context->parent : NULL;
}

DBrowsingContext *d_browsing_context_top(const DBrowsingContext *context)
{
    if (!context) return NULL;
    while (context->parent) context = context->parent;
    return (DBrowsingContext *)context;
}

DBrowsingContext *d_browsing_context_child(const DBrowsingContext *context, size_t index)
{
    DBrowsingContext *child = context && !context->discarded ? context->first_child : NULL;
    while (child && index--) child = child->next_sibling;
    return child;
}

const char *d_browsing_context_name(const DBrowsingContext *context)
{
    return context ? context->name : NULL;
}
