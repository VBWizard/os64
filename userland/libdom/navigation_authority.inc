/* Location's IDL caller, URL-base entry and navigation incumbent are distinct.
 * This private path carries the latter two through native host authorization. */
struct DNavigationAuthority {
    DNavigationPolicy policy;
    uint64_t generation;
    bool busy;
};

os64_js_status_t d_navigation_policy(os64_dom_cluster_t *cluster,
    const DNavigationPolicy *policy, os64_js_outcome_t *outcome)
{
    os64_js_status_t status = d_browsing_quiescent(cluster, outcome);
    if (status != OS64_JS_OK) return status;
    if (policy && (!policy->snapshot || !policy->allow)) return outcome->status = OS64_JS_BAD_ARGUMENT;
    struct DNavigationAuthority *authority = cluster->navigation_authority;
    if (!authority) {
        if (!policy) return OS64_JS_OK;
        if (sizeof(*authority) > cluster->limits.dom_bytes - cluster->bytes)
            return outcome->status = OS64_JS_ADMISSION_REFUSED;
        authority = os64_calloc(1, sizeof(*authority));
        if (!authority) return outcome->status = OS64_JS_ADMISSION_REFUSED;
        cluster->bytes += sizeof(*authority);
        cluster->navigation_authority = authority;
    }
    if (authority->generation == UINT64_MAX) return outcome->status = OS64_JS_ADMISSION_REFUSED;
    authority->generation++;
    authority->policy = policy ? *policy : (DNavigationPolicy){0};
    return OS64_JS_OK;
}

void d_navigation_authority_free(os64_dom_cluster_t *cluster)
{
    if (!cluster->navigation_authority) return;
    cluster->bytes -= sizeof(*cluster->navigation_authority);
    os64_free(cluster->navigation_authority);
    cluster->navigation_authority = NULL;
}

static os64_dom_t *origin_settings(os64_dom_t *target, os64_js_settings_role_t role)
{
    os64_js_outcome_t out;
    os64_js_realm_t *realm = os64_js_settings_realm(target->runtime, OS64_JS_ABI_ID, role, &out);
    for (size_t i = 0; realm && i < target->cluster->count; ++i) {
        os64_dom_t *binding = target->cluster->members[i];
        if (binding->realm == realm && binding->binding_origin && d_delivery_active(binding)) return binding;
    }
    return NULL;
}

static bool navigation_current(const DNavigationRequest *request)
{
    os64_dom_cluster_t *cluster = request->target->cluster;
    struct DNavigationAuthority *authority = cluster->navigation_authority;
    return authority && authority->policy.snapshot && authority->policy.allow &&
        authority->generation == request->policy_generation &&
        d_browsing_context_target(request->source_context, request->source_generation) == request->source &&
        d_browsing_context_target(request->entry_context, request->entry_generation) == request->entry &&
        d_browsing_context_target(request->target_context, request->target_generation) == request->target;
}

static void navigation_hold(const DNavigationRequest *request)
{
    d_provider_hold(request->source);
    d_provider_hold(request->target);
}

static void navigation_leave(const DNavigationRequest *request)
{
    d_provider_leave(request->target);
    d_provider_leave(request->source);
}

static JSValue navigation_denied(JSContext *ctx)
{
    return d_error(ctx, "SecurityError", "Navigation settings or host policy refuse this request");
}

static JSValue context_navigate(os64_dom_t *target, JSContext *ctx, JSValueConst value,
    DNavigationAction action)
{
    os64_dom_cluster_t *cluster = target->cluster;
    struct DNavigationAuthority *authority = cluster->navigation_authority;
    os64_dom_t *entry = origin_settings(target, OS64_JS_SETTINGS_ENTRY);
    os64_dom_t *source = action == D_NAV_RELOAD ? target : origin_settings(target, OS64_JS_SETTINGS_INCUMBENT);
    if (!authority || authority->busy || !entry || !source) return navigation_denied(ctx);
    DNavigationRequest request = {.source=source, .entry=entry, .target=target,
        .source_context=source->browsing_context, .entry_context=entry->browsing_context,
        .target_context=target->browsing_context, .action=action,
        .kind=action==D_NAV_RELOAD ? OS64_DOM_NAVIGATE_RELOAD :
              action==D_NAV_REPLACE ? OS64_DOM_NAVIGATE_REPLACE : OS64_DOM_NAVIGATE_URL,
        .policy_generation=authority->generation};
    request.source_generation = d_browsing_context_generation(request.source_context);
    request.entry_generation = d_browsing_context_generation(request.entry_context);
    request.target_generation = d_browsing_context_generation(request.target_context);
    if (!navigation_current(&request)) return navigation_denied(ctx);
    DString text = {0};
    if (action != D_NAV_RELOAD && !d_string(target, ctx, value, &text)) return JS_EXCEPTION;
    /* Conversion can retire documents or change a live <base>. Check identity
     * again, then resolve against the entry document's current base. */
    if (!navigation_current(&request) ||
        ((action==D_NAV_ASSIGN || action==D_NAV_HASH || action==D_NAV_RELOAD) &&
         !way_origin_same(entry->binding_origin, target->binding_origin))) {
        d_string_free(target, &text);
        return navigation_denied(ctx);
    }
    bool resolved = true;
    if (action == D_NAV_RELOAD) os64_strcopy(request.url, sizeof(request.url), page_url(target));
    else if (action == D_NAV_HASH) {
        const char *base = page_url(target), *name = text.data[0]=='#' ? text.data+1 : text.data;
        size_t keep = before_fragment(base);
        char encoded[OS64_DOM_URL_MAX];
        resolved = fragment_encoded(name, encoded, sizeof(encoded));
        if (resolved && !os64_strcmp(encoded, base[keep]=='#' ? base+keep+1 : "")) {
            d_string_free(target, &text);
            return JS_UNDEFINED;
        }
        resolved = resolved && keep + 1 + os64_strlen(encoded) < sizeof(request.url);
        if (resolved) os64_snprintf(request.url, sizeof(request.url), "%.*s#%s", (int)keep, base, encoded);
    } else resolved = os64_dom_resolve(entry->document, page_url(entry), text.data, request.url, sizeof(request.url));
    d_string_free(target, &text);
    if (!resolved) return d_error(ctx, "SyntaxError", "The address cannot be resolved against the entry document");
    authority->busy = true;
    navigation_hold(&request);
    bool permitted = authority->policy.snapshot(authority->policy.opaque, source, target, &request.snapshot);
    if (permitted && navigation_current(&request)) {
        if (action != D_NAV_RELOAD && !request.snapshot.target_loaded && !request.snapshot.transient_activation)
            request.kind = OS64_DOM_NAVIGATE_REPLACE;
        permitted = authority->policy.allow(authority->policy.opaque, &request, D_NAV_ENQUEUE);
    } else permitted = false;
    navigation_leave(&request);
    authority->busy = false;
    os64_js_outcome_t out;
    if (os64_js_check_budget(target->runtime, OS64_JS_ABI_ID, &out) != OS64_JS_OK) return JS_EXCEPTION;
    if (!permitted || !navigation_current(&request) ||
        origin_settings(target, OS64_JS_SETTINGS_ENTRY) != entry ||
        (action != D_NAV_RELOAD && origin_settings(target, OS64_JS_SETTINGS_INCUMBENT) != source)) return navigation_denied(ctx);
    DBrowsingContext *context = request.target_context;
    if (!context->navigation) {
        if (sizeof(request) > cluster->limits.dom_bytes - cluster->bytes)
            return d_error(ctx, "QuotaExceededError", "Navigation request quota exceeded");
        context->navigation = os64_malloc(sizeof(request));
        if (!context->navigation) return d_error(ctx, "QuotaExceededError", "Navigation request allocation refused");
        cluster->bytes += sizeof(request);
    }
    *context->navigation = request;
    return JS_UNDEFINED;
}

bool d_navigation_dispatch(DBrowsingContext *context,
    void (*consume)(void *, const DNavigationRequest *), void *opaque, os64_js_outcome_t *outcome)
{
    os64_dom_cluster_t *cluster = context ? context->cluster : NULL;
    if (d_browsing_quiescent(cluster, outcome) != OS64_JS_OK) return false;
    if (!consume) { outcome->status=OS64_JS_BAD_ARGUMENT;return false; }
    if (!context->navigation || context->navigation->kind==OS64_DOM_NAVIGATE_NONE) return false;
    DNavigationRequest request = *context->navigation;
    context->navigation->kind = OS64_DOM_NAVIGATE_NONE;
    if (!navigation_current(&request)) { outcome->status=OS64_JS_BAD_ARGUMENT;return false; }
    struct DNavigationAuthority *authority = cluster->navigation_authority;
    authority->busy = true;
    navigation_hold(&request);
    bool permitted = authority->policy.allow(authority->policy.opaque, &request, D_NAV_DISPATCH);
    navigation_leave(&request);
    authority->busy = false;
    if (d_browsing_quiescent(cluster, outcome) != OS64_JS_OK) return false;
    if (!permitted || !navigation_current(&request)) { outcome->status=OS64_JS_BAD_ARGUMENT;return false; }
    authority->busy = true;
    navigation_hold(&request);
    consume(opaque, &request);
    navigation_leave(&request);
    authority->busy = false;
    return true;
}
