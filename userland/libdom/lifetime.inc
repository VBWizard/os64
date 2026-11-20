/* A cluster owns these bounded native mappings. They are diagnostics over
 * grouped retention, with no new JS roots or permission/release authority. */
typedef struct { os64_dom_t *binding; JSOwnerId owner; } DLifetimeBinding;
typedef struct { DBrowsingContext *context; JSOwnerContextId id; } DLifetimeContext;
typedef struct DLifetimeAdapter {
    DLifetimeSummary summary;
    DLifetimeHost host;
    DLifetimeBinding *bindings;
    DLifetimeContext *contexts;
    bool preparing;
} DLifetimeAdapter;

static bool lifetime_owner_equal(JSOwnerId a, JSOwnerId b)
{ return a.incarnation == b.incarnation && a.serial == b.serial; }
static DLifetimeBinding *lifetime_binding(DLifetimeAdapter *adapter, const os64_dom_t *binding)
{
    for (size_t i = 0; i < adapter->summary.bindings; ++i)
        if (adapter->bindings[i].binding == binding) return &adapter->bindings[i];
    return NULL;
}
static DLifetimeContext *lifetime_context(DLifetimeAdapter *adapter, const DBrowsingContext *context)
{
    for (size_t i = 0; i < adapter->summary.contexts; ++i)
        if (adapter->contexts[i].context == context) return &adapter->contexts[i];
    return NULL;
}
static os64_js_status_t lifetime_gate(os64_dom_cluster_t *cluster, os64_js_outcome_t *outcome)
{
    if (!outcome) return OS64_JS_BAD_ARGUMENT;
    if (!cluster || !cluster->lifetime || cluster->lifetime->summary.closed)
        return outcome->status = OS64_JS_ADMISSION_REFUSED;
    if (cluster->lifetime->preparing) return outcome->status = OS64_JS_BUSY;
    return d_browsing_quiescent(cluster, outcome);
}
os64_js_status_t d_lifetime_select(os64_dom_cluster_t *cluster, size_t capacity,
    const DLifetimeHost *host, os64_js_outcome_t *outcome)
{
    if (!outcome) return OS64_JS_BAD_ARGUMENT;
    if (cluster && (cluster->drained || (cluster->lifetime && cluster->lifetime->summary.closed)))
        return outcome->status = OS64_JS_ADMISSION_REFUSED;
    os64_js_status_t status = d_browsing_quiescent(cluster, outcome);
    if (status != OS64_JS_OK) return status;
    if (cluster->lifetime || !capacity || capacity > D_REALMS || !host || !host->describe ||
        !host->map || !host->facts || !host->context_id)
        return outcome->status = OS64_JS_BAD_ARGUMENT;
    uint64_t incarnation;
    cluster->provider_borrows++;
    bool selected = host->describe(host->opaque, cluster->runtime, &incarnation);
    cluster->provider_borrows--;
    if (!selected || !incarnation || cluster->drained || cluster->poisoned || cluster->lifetime)
        return outcome->status = OS64_JS_ADMISSION_REFUSED;
    /* capacity <= D_REALMS bounds both arithmetic and retained map entries. */
    size_t bytes = sizeof(DLifetimeAdapter) + capacity * (sizeof(DLifetimeBinding) + sizeof(DLifetimeContext));
    if (bytes > cluster->limits.dom_bytes - cluster->bytes)
        return outcome->status = OS64_JS_ADMISSION_REFUSED;
    DLifetimeAdapter *adapter = os64_calloc(1, bytes);
    if (!adapter) return outcome->status = OS64_JS_ADMISSION_REFUSED;
    adapter->summary = (DLifetimeSummary){.incarnation=incarnation,
        .capacity=capacity, .charged_bytes=bytes};
    adapter->host = *host;
    adapter->bindings = (DLifetimeBinding *)(adapter + 1);
    adapter->contexts = (DLifetimeContext *)(adapter->bindings + capacity);
    cluster->bytes += bytes;
    cluster->lifetime = adapter;
    return OS64_JS_OK;
}
static void lifetime_bridge(os64_dom_cluster_t *cluster, bool begin)
{
    cluster->lifetime->preparing = begin;
    if (begin) cluster->provider_borrows++;
    else cluster->provider_borrows--;
}
os64_js_status_t d_lifetime_register(os64_dom_cluster_t *cluster, os64_dom_t *binding,
    JSOwnerId *out, os64_js_outcome_t *outcome)
{
    os64_js_status_t status = lifetime_gate(cluster, outcome);
    if (status != OS64_JS_OK) return status;
    bool member = false;
    for (size_t i = 0; i < cluster->count; ++i)
        if (cluster->members[i] == binding) member = true;
    if (!out || !member) return outcome->status = OS64_JS_BAD_ARGUMENT;
    DLifetimeAdapter *adapter = cluster->lifetime;
    DBrowsingContext *context = binding->browsing_context;
    if (context && !context_member(cluster, context)) return outcome->status = OS64_JS_BAD_ARGUMENT;
    DLifetimeBinding *known_binding = lifetime_binding(adapter, binding);
    DLifetimeContext *known_context = context ? lifetime_context(adapter, context) : NULL;
    if ((!known_binding && adapter->summary.bindings == adapter->summary.capacity) ||
        (context && !known_context && adapter->summary.contexts == adapter->summary.capacity) ||
        (context && context->parent && !lifetime_context(adapter, context->parent)))
        return outcome->status = OS64_JS_ADMISSION_REFUSED;
    JSOwnerId owner;
    if (known_binding) owner = known_binding->owner;
    else {
        lifetime_bridge(cluster, true);
        bool mapped = adapter->host.map(adapter->host.opaque, binding->realm, &owner);
        lifetime_bridge(cluster, false);
        if (!mapped || cluster->drained || cluster->poisoned || adapter->summary.closed ||
            owner.incarnation != adapter->summary.incarnation || !owner.serial)
            return outcome->status = OS64_JS_ADMISSION_REFUSED;
        for (size_t i = 0; i < adapter->summary.bindings; ++i)
            if (lifetime_owner_equal(adapter->bindings[i].owner, owner))
                return outcome->status = OS64_JS_ADMISSION_REFUSED;
    }
    JSOwnerContextId id;
    if (context && !known_context) {
        lifetime_bridge(cluster, true);
        bool issued = adapter->host.context_id(adapter->host.opaque, &id);
        lifetime_bridge(cluster, false);
        if (!issued || cluster->drained || cluster->poisoned || adapter->summary.closed ||
            id.incarnation != adapter->summary.incarnation || !id.serial)
            return outcome->status = OS64_JS_ADMISSION_REFUSED;
    }
    /* Publication follows all fallible preparation; no callback intervenes. */
    if (!known_binding)
        adapter->bindings[adapter->summary.bindings++] = (DLifetimeBinding){binding, owner};
    if (context && !known_context)
        adapter->contexts[adapter->summary.contexts++] = (DLifetimeContext){context, id};
    *out = owner;
    return OS64_JS_OK;
}
static bool lifetime_facts(os64_dom_cluster_t *cluster, os64_dom_t *binding, JSOwnerRealmSnapshot *out)
{
    DLifetimeBinding *mapped = lifetime_binding(cluster->lifetime, binding);
    if (!mapped) return false;
    lifetime_bridge(cluster, true);
    bool copied = cluster->lifetime->host.facts(cluster->lifetime->host.opaque, mapped->owner, out);
    lifetime_bridge(cluster, false);
    return copied && !cluster->drained && !cluster->poisoned && !cluster->lifetime->summary.closed &&
        lifetime_owner_equal(out->owner, mapped->owner);
}
os64_js_status_t d_lifetime_observe(os64_dom_cluster_t *cluster, JSOwnerId relevant, uint64_t expected,
    DLifetimeSnapshot *out, os64_js_outcome_t *outcome)
{
    os64_js_status_t status = lifetime_gate(cluster, outcome);
    if (status != OS64_JS_OK) return status;
    if (!out) return outcome->status = OS64_JS_BAD_ARGUMENT;
    DLifetimeAdapter *adapter = cluster->lifetime;
    DLifetimeBinding *mapped = NULL;
    for (size_t i = 0; i < adapter->summary.bindings; ++i)
        if (lifetime_owner_equal(adapter->bindings[i].owner, relevant)) mapped = &adapter->bindings[i];
    if (!mapped) return outcome->status = OS64_JS_ADMISSION_REFUSED;
    DLifetimeSnapshot facts = {.escaped_unknown=true, .graph_unknown=true, .native_borrows_unknown=true};
    if (!lifetime_facts(cluster, mapped->binding, &facts.relevant))
        return outcome->status = OS64_JS_ADMISSION_REFUSED;
    if (mapped->binding->binding_origin) {
        facts.has_relevant_origin = true;
        facts.relevant_origin = *mapped->binding->binding_origin;
    }
    DBrowsingContext *context = mapped->binding->browsing_context;
    if (!context) {
        if (expected) return outcome->status = OS64_JS_ADMISSION_REFUSED;
    } else {
        DLifetimeContext *known = lifetime_context(adapter, context);
        if (!known || !context_member(cluster, context) || expected != context->generation)
            return outcome->status = OS64_JS_ADMISSION_REFUSED;
        facts.associated = true;
        facts.context = known->id;
        facts.generation = context->generation;
        facts.discarded = context->discarded;
        if (context->parent) {
            DLifetimeContext *parent = lifetime_context(adapter, context->parent);
            if (!parent) return outcome->status = OS64_JS_ADMISSION_REFUSED;
            facts.parent = parent->id;
        }
        if (context->active) {
            if (!lifetime_facts(cluster, context->active, &facts.active))
                return outcome->status = OS64_JS_ADMISSION_REFUSED;
            facts.has_active = true;
            if (context->active->binding_origin) {
                facts.has_active_origin = true;
                facts.active_origin = *context->active->binding_origin;
            }
            facts.active_eligible = d_delivery_active(context->active) && !facts.active.staged &&
                !facts.active.retired && !facts.active.failed;
        }
        if (context->window) {
            if (!lifetime_facts(cluster, context->window, &facts.last_window))
                return outcome->status = OS64_JS_ADMISSION_REFUSED;
            facts.has_last_window = true;
            if (context->window->binding_origin) {
                facts.has_last_window_origin = true;
                facts.last_window_origin = *context->window->binding_origin;
            }
        }
        facts.navigation_cell = context->navigation != NULL;
        facts.navigation_pending = context->navigation && context->navigation->kind != OS64_DOM_NAVIGATE_NONE;
    }
    d_exposure_counts(mapped->binding, &facts.known);
    *out = facts;
    return OS64_JS_OK;
}
bool d_lifetime_summary(const os64_dom_cluster_t *cluster, DLifetimeSummary *out)
{
    if (!cluster || !cluster->lifetime || !out) return false;
    *out = cluster->lifetime->summary;
    return true;
}
static void lifetime_stop(os64_dom_cluster_t *cluster)
{
    DLifetimeAdapter *adapter = cluster->lifetime;
    if (!adapter || adapter->summary.closed) return;
    adapter->summary.closed = true;
    adapter->host = (DLifetimeHost){0};
    for (size_t i = 0; i < adapter->summary.bindings; ++i) adapter->bindings[i].binding = NULL;
    for (size_t i = 0; i < adapter->summary.contexts; ++i) adapter->contexts[i].context = NULL;
}
static void lifetime_free(os64_dom_cluster_t *cluster)
{
    if (!cluster->lifetime) return;
    lifetime_stop(cluster);
    cluster->bytes -= cluster->lifetime->summary.charged_bytes;
    os64_free(cluster->lifetime);
    cluster->lifetime = NULL;
}
