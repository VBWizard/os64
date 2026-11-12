/* Trusted native retirement during provider callbacks. A busy loan is the
 * prerequisite for safe-point finish, not a claim that finish is implemented. */
bool d_provider_enter(os64_dom_t *dom);
void d_provider_leave(os64_dom_t *dom);
bool d_provider_busy(const os64_dom_t *dom);
void d_storage_drain(os64_dom_t *dom);

enum {
    RP_GEOMETRY, RP_HIT, RP_RECTS, RP_SVG, RP_VIEW, RP_SCROLL, RP_COMPUTED,
    RP_IMAGE, RP_IMAGE_EVENT, RP_FONT, RP_RESIZE, RP_INTERSECTION, RP_DAMAGE, RP_DAMAGE_RESET,
    RP_ALERT, RP_WRITE, RP_OPEN, RP_ENTROPY, RP_FOCUS, RP_HAS_FOCUS, RP_ACTIVATE, RP_REPORT,
    RP_SCRIPT, RP_COOKIE_READ, RP_COOKIE_SET, RP_COOKIE_ENABLED, RP_AGENT, RP_MISS, RP_BROKEN,
    RP_CLOCK_TIMER, RP_CLOCK_INTERVAL, RP_CLOCK_RAF, RP_CLOCK_IDLE_TIMEOUT, RP_CLOCK_POST, RP_CLOCK_PORT,
    RP_CLOCK_NOW, RP_CLOCK_MARK, RP_CLOCK_MEASURE, RP_CLOCK_EVENT, RP_TIMING,
    RP_ZOOM_RATIO, RP_ZOOM_SCREEN, RP_ZOOM_MEDIA_NEW, RP_ZOOM_MEDIA_READ, RP_ZOOM_MEDIA_STEP,
    RP_CLOCK_IDLE_NATIVE, RP_CLOCK_RAF_NATIVE, RP_CLOCK_WA, RP_CLOCK_NOW_MS, RP_CLOCK_WA_NATIVE,
    RP_HISTORY_LENGTH, RP_HISTORY_PUSH, RP_HISTORY_REPLACE, RP_HISTORY_FRAGMENT,
    RP_HISTORY_FRAGMENT_REFUSE, RP_HISTORY_UPDATE_REFUSE,
    RP_STORAGE_IDENTITY, RP_STORAGE_LOAD, RP_STORAGE_GROW, RP_STORAGE_FAILED,
    RP_STORAGE_COMMIT, RP_STORAGE_CLOCK, RP_STORAGE_COMMIT_CLOCK, RP_STORAGE_PRESSURE,
    RP_UPDATE, RP_RELEASE, RP_MS, RP_US, RP_ZOOM, RP_COUNT
};
typedef struct {
    ClusterProvider base;
    os64_dom_t *binding, *sibling;
    os64_js_realm_t *realm;
    unsigned kind, calls[RP_COUNT];
    bool armed;
    unsigned storage_commits;
    bool storage_reentry, storage_expect_owed;
} RetirementProvider;

static void retirement_provider_touch(RetirementProvider *p, unsigned kind)
{
    p->calls[kind]++;
    check(d_provider_busy(p->binding) && d_provider_busy(p->sibling),
          "provider retirement: loan visible to owner and sibling");
    if (p->armed && p->kind == kind) {
        p->armed = false;
        check(d_provider_enter(p->sibling), "provider retirement: nested sibling loan admitted");
        d_provider_leave(p->sibling);
        check(d_provider_busy(p->sibling), "provider retirement: outer loan survives nested return");
        os64_js_outcome_t out;
        check(os64_js_realm_retire(p->realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "provider retirement: native mark succeeds during loan");
        check(d_provider_busy(p->binding) && !d_provider_enter(p->binding),
              "provider retirement: mark keeps old loan but refuses a new one");
    }
}
static bool retirement_geometry(void *opaque, const os64_html_node_t *node, os64_dom_geometry_t *out)
{
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, RP_GEOMETRY);
    bool ready = cluster_geometry(&p->base, node, out);
    out->width = p->base.width;
    return ready;
}
static size_t retirement_hit(void *opaque, double x, double y, const os64_html_node_t **out,
                               size_t cap, os64_dom_geometry_stats_t *cost)
{
    (void)x; (void)y; *cost = (os64_dom_geometry_stats_t){.layouts=1, .elapsed_us=7};
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, RP_HIT);
    for (size_t i = 0; i < cap && i < 65; i++) out[i] = p->base.document->body;
    return 65;
}
static size_t retirement_rects(void *opaque, const os64_html_node_t *node, size_t from, size_t to,
                                 double *out, size_t cap, os64_dom_geometry_stats_t *cost)
{
    (void)node; (void)from; (void)to; *cost = (os64_dom_geometry_stats_t){.layouts=1, .elapsed_us=7};
    retirement_provider_touch(opaque, RP_RECTS);
    for (size_t i = 0; i < cap && i < 9; i++) {
        out[i * 4] = 1; out[i * 4 + 1] = 2; out[i * 4 + 2] = 3; out[i * 4 + 3] = 4;
    }
    return 9;
}
static os64_geometry_status_t retirement_svg(void *opaque, const os64_html_node_t *node, double x, double y,
    const os64_geometry_memory_t *memory, bool (*cancelled)(void *), void *cancel_opaque, bool *inside,
    os64_dom_geometry_stats_t *cost)
{
    (void)node; (void)x; (void)y; (void)memory; (void)cancelled; (void)cancel_opaque;
    *cost = (os64_dom_geometry_stats_t){0}; *inside = true;
    retirement_provider_touch(opaque, RP_SVG);
    return OS64_GEOMETRY_OK;
}
static void retirement_view(void *opaque, os64_dom_view_t *out)
{
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, RP_VIEW);
    cluster_view(&p->base, out);
}
static bool retirement_scroll(void *opaque, double x, double y, os64_dom_geometry_stats_t *cost)
{
    (void)x; (void)y; *cost = (os64_dom_geometry_stats_t){0};
    retirement_provider_touch(opaque, RP_SCROLL);
    return true;
}
static bool retirement_computed(void *opaque, const os64_html_node_t *node, os64_dom_computed_t *out)
{
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, RP_COMPUTED);
    return cluster_computed(&p->base, node, out);
}
static os64_dom_image_status_t retirement_image(void *opaque, const os64_html_node_t *node, bool pixels,
                                                 os64_dom_image_view_t *out)
{
    (void)node; (void)pixels;
    RetirementProvider *p = opaque;
    bool replace = p->armed;
    retirement_provider_touch(p, p->kind == RP_IMAGE_EVENT ? RP_IMAGE_EVENT : RP_IMAGE);
    if (replace) os64_dom_set_images(p->binding, NULL);
    static uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
    *out = (os64_dom_image_view_t){.surface = {colors, 2, 2, 8, sizeof(colors), DRAW2D_ARGB8_STRAIGHT},
                                 .origin_clean = true, .token = colors, .current_src = "retirement.png"};
    return OS64_DOM_IMAGE_READY;
}
static void retirement_image_release(void *opaque, os64_dom_image_view_t *view)
{
    check(view->token != NULL, "provider retirement: release receives the acquired loan");
    retirement_provider_touch(opaque, RP_RELEASE);
}
static void retirement_image_update(void *opaque)
{ retirement_provider_touch(opaque, RP_UPDATE); }
static os64_font_status_t retirement_font(void *opaque, const os64_font_family_list_t *families,
    bool bold, bool italic, uint32_t height, const uint8_t *text, size_t length, os64_text_run_t **out)
{
    (void)families; (void)bold; (void)italic; (void)height; (void)text; (void)length; *out = NULL;
    retirement_provider_touch(opaque, RP_FONT);
    return OS64_FONT_UNSUPPORTED;
}
static bool retirement_resizes(void *opaque, os64_dom_resize_t *items, size_t count,
                                 os64_dom_geometry_stats_t *cost)
{
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, RP_RESIZE);
    return cluster_resizes(&p->base, items, count, cost);
}
static bool retirement_intersections(void *opaque, os64_dom_intersection_t *items, size_t count,
                                       os64_dom_geometry_stats_t *cost)
{
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, RP_INTERSECTION);
    return cluster_intersections(&p->base, items, count, cost);
}
static void retirement_damage(void *opaque, const os64_html_node_t *node, uint32_t width, uint32_t height,
                              const draw2d_damage_t *damage)
{
    (void)node; (void)height;
    RetirementProvider *p = opaque;
    if (p->kind == RP_DAMAGE_RESET) check(width == 0 && !damage, "provider retirement: reset damage borrowed");
    retirement_provider_touch(p, p->kind);
}
static void retirement_alert(void *opaque, const char *text, size_t length)
{
    (void)text; (void)length;
    retirement_provider_touch(opaque, RP_ALERT);
}
static int64_t retirement_write(void *opaque, const char *text, size_t length)
{
    (void)text; (void)length;
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, p->kind == RP_OPEN ? RP_OPEN : RP_WRITE);
    return OS64_HTML_OK;
}
static bool retirement_entropy(void *opaque, uint8_t *out, size_t length)
{
    memset(out, 0x5a, length);
    retirement_provider_touch(opaque, RP_ENTROPY);
    return true;
}
static bool retirement_focus(void *opaque, const os64_html_node_t **out)
{
    RetirementProvider *p = opaque;
    *out = p->base.document->body;
    retirement_provider_touch(p, p->kind == RP_HAS_FOCUS ? RP_HAS_FOCUS : RP_FOCUS);
    return true;
}
static void retirement_activate(void *opaque, const os64_html_node_t *node, os64_dom_activation_t what)
{
    (void)node;
    retirement_provider_touch(opaque, what == OS64_DOM_ACTIVATE_REPORT ? RP_REPORT : RP_ACTIVATE);
}
static void retirement_script(void *opaque, const os64_html_node_t *node)
{
    (void)node;
    retirement_provider_touch(opaque, RP_SCRIPT);
}
static size_t retirement_cookie_get(void *opaque, char *out, size_t cap, bool *whole)
{
    RetirementProvider *p = opaque;
    bool was_armed = p->armed;
    retirement_provider_touch(p, RP_COOKIE_READ);
    check(cap >= 4, "provider retirement: cookie output has room");
    if (cap >= 4) memcpy(out, "k=v", 4);
    *whole = !was_armed;
    return 3;
}
static void retirement_cookie_set(void *opaque, const char *text, size_t length)
{
    (void)text; (void)length;
    retirement_provider_touch(opaque, RP_COOKIE_SET);
}
static bool retirement_cookie_enabled(void *opaque)
{
    retirement_provider_touch(opaque, RP_COOKIE_ENABLED);
    return true;
}
static const char *retirement_agent(void *opaque)
{
    retirement_provider_touch(opaque, RP_AGENT);
    return "Chrome/123";
}
static void retirement_service_miss(void *opaque, const char *name)
{
    if (!strcmp(name, "serviceMissing")) retirement_provider_touch(opaque, RP_MISS);
}
static void retirement_broken(void *opaque, const char *sentence)
{
    (void)sentence;
    retirement_provider_touch(opaque, RP_BROKEN);
}
static uint64_t retirement_now_ms(void *opaque)
{
    RetirementProvider *p = opaque;
    if (p->binding) {
        unsigned kind = (p->kind >= RP_CLOCK_TIMER && p->kind <= RP_CLOCK_PORT) ||
            p->kind == RP_CLOCK_EVENT || p->kind == RP_CLOCK_IDLE_NATIVE ||
            p->kind == RP_CLOCK_WA || p->kind == RP_CLOCK_NOW_MS ||
            p->kind == RP_STORAGE_CLOCK || p->kind == RP_STORAGE_COMMIT_CLOCK ? p->kind : RP_MS;
        bool postpone = kind == RP_STORAGE_COMMIT_CLOCK && p->calls[kind] == 0 && p->armed;
        if (postpone) p->armed = false;
        retirement_provider_touch(p, kind);
        if (postpone) p->armed = true;
    }
    return 1005;
}
static uint64_t retirement_now_us(void *opaque)
{
    RetirementProvider *p = opaque;
    if (p->binding) {
        unsigned kind = (p->kind >= RP_CLOCK_NOW && p->kind <= RP_CLOCK_MEASURE) ||
            p->kind == RP_CLOCK_RAF_NATIVE || p->kind == RP_CLOCK_WA_NATIVE ? p->kind : RP_US;
        retirement_provider_touch(p, kind);
    }
    return 1005000;
}
static void retirement_timing(void *opaque, os64_dom_timing_t *out)
{
    RetirementProvider *p = opaque;
    if (p->binding) retirement_provider_touch(p, RP_TIMING);
    out->origin_us = 1000000; out->origin_epoch_us = 2000000;
    out->at[OS64_DOM_TIMING_FETCH_START] = 5000;
    out->at[OS64_DOM_TIMING_LOAD_END] = 7000;
    out->type = 1; out->redirects = 3;
}
static uint32_t retirement_zoom(void *opaque)
{
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, p->kind >= RP_ZOOM_RATIO && p->kind <= RP_ZOOM_MEDIA_STEP ? p->kind : RP_ZOOM);
    return 1250;
}
static uint32_t retirement_history_length(void *opaque)
{
    retirement_provider_touch(opaque, RP_HISTORY_LENGTH);
    return 4;
}
static bool retirement_history_update(void *opaque, const char *url, const char *state, size_t length, bool push)
{
    (void)url; (void)state; (void)length;
    RetirementProvider *p = opaque;
    bool refuse = p->armed && p->kind == RP_HISTORY_UPDATE_REFUSE;
    retirement_provider_touch(p, p->kind == RP_HISTORY_UPDATE_REFUSE ? p->kind :
                              push ? RP_HISTORY_PUSH : RP_HISTORY_REPLACE);
    return !refuse;
}
static bool retirement_history_fragment(void *opaque, const char *url, bool replace)
{
    (void)url; (void)replace;
    RetirementProvider *p = opaque;
    retirement_provider_touch(p, p->kind == RP_HISTORY_FRAGMENT_REFUSE ? p->kind : RP_HISTORY_FRAGMENT);
    return p->kind != RP_HISTORY_FRAGMENT_REFUSE;
}
static void retirement_no_navigation(RetirementProvider *p, const char *label)
{
    os64_dom_navigation_t ask;
    bool taken = os64_dom_take_navigation(p->binding, &ask);
    check(!taken && ask.kind == OS64_DOM_NAVIGATE_NONE, label);
    /* A failing implementation can return held nodes; release those so the
     * assertion, rather than fixture teardown, diagnoses the regression. */
    if (ask.node) os64_html_release(p->base.document, ask.node);
    if (ask.submitter) os64_html_release(p->base.document, ask.submitter);
}
static uint64_t retirement_storage_identity(void *opaque, const char *origin)
{
    (void)origin;
    retirement_provider_touch(opaque, RP_STORAGE_IDENTITY);
    return 1;
}
static int64_t retirement_storage_load(void *opaque, const char *origin, void *out, size_t cap)
{
    (void)origin;
    RetirementProvider *p = opaque;
    unsigned kind = p->kind == RP_STORAGE_GROW || p->kind == RP_STORAGE_FAILED ? p->kind : RP_STORAGE_LOAD;
    bool armed = p->armed;
    retirement_provider_touch(p, kind);
    if (armed && kind == RP_STORAGE_GROW) return (int64_t)cap + 1;
    if (armed && kind == RP_STORAGE_FAILED) return -1;
    static const char image[] = "os64 storage 1\n+4 4\nseedkept\n";
    if (cap >= sizeof(image)-1) memcpy(out, image, sizeof(image)-1);
    return sizeof(image)-1;
}
static os64_dom_storage_status_t retirement_storage_commit(void *opaque, const char *origin,
                                                           const void *changes, size_t length)
{
    (void)origin;
    RetirementProvider *p = opaque;
    p->storage_commits++;
    /* Bound the regression fixture even when reentrant settle is broken. */
    if (!p->storage_reentry) {
        p->storage_reentry = true;
        os64_dom_storage_settle(p->binding);
        os64_dom_storage_forget(p->binding);
        d_storage_drain(p->binding);
        os64_dom_set_storage(p->binding, OS64_DOM_STORAGE_LOCAL, NULL);
        p->storage_reentry = false;
    }
    retirement_provider_touch(p, p->kind == RP_STORAGE_PRESSURE ? p->kind : RP_STORAGE_COMMIT);
    check(os64_dom_storage_walk(changes, length, NULL, NULL),
          "provider retirement: commit bytes survive native reentry");
    if (p->storage_expect_owed)
        check(bytes_find(changes, length, "owedbefore", 10) != NULL,
              "provider retirement: commit preserves owed changes during reentry");
    return OS64_DOM_STORAGE_OK;
}
static void provider_native_retirement_cases(void)
{
    const char *operations[] = {
        "B.box.getBoundingClientRect()", "B.document.elementsFromPoint(1,1)", "B.range.getClientRects()",
        "B.shape.isPointInFill({x:1,y:1})", "B.innerWidth", "B.scrollTo(1,2)",
        "B.getComputedStyle(B.box).width", "context.drawImage(B.picture,0,0)", NULL,
        "B.context.measureText('x')", NULL, NULL, NULL, NULL,
        "B.alert('hello')", "B.document.writeln('a','b')", "B.document.open()",
        "B.crypto.getRandomValues(B.random)", "assert(B.document.activeElement===null)",
        "assert(!B.document.hasFocus())", "B.box.focus()", "B.input.reportValidity()",
        "B.document.body.appendChild(B.scripts)", "B.document.cookie", "B.document.cookie='k=v'",
        "B.navigator.cookieEnabled", "assert(B.navigator.userAgent==='Chrome/123')", "B.missingProbe()",
        "assert(B.attr.value===''&&B.attr.ownerElement===null)",
        "B.setTimeout(()=>{},1)", "B.setInterval(()=>{},1)", "B.requestAnimationFrame(()=>{})",
        "B.requestIdleCallback(()=>{},{timeout:1})", "B.postMessage('x','*',[B.buffer])",
        "B.channel.port1.postMessage('x',[B.buffer])", "B.performance.now()", "B.performance.mark('implicit')",
        "B.performance.measure('implicit')", "new B.Event('mark')", "assert(B.performance.timing.loadEventEnd===2007)",
        "B.devicePixelRatio", "B.screen.width", "B.matchMedia('(resolution: 1.25dppx)')", "B.media.matches", NULL,
        NULL, NULL, "B.document.timeline.currentTime", "B.performance.now()", NULL,
        "B.history.length", "B.history.pushState({next:2},'', '#push')",
        "B.history.replaceState({next:2},'', '#replace')", "B.location.hash='fragment'",
        "B.location.replace('#refused')", "B.history.pushState({next:2},'', '#refused')",
        "B.store.getItem('seed')", "B.store.getItem('seed')", "B.store.getItem('seed')",
        "B.store.getItem('seed')", NULL, NULL, NULL,
        "B.store.setItem(B.bigKey+'c','1')"
    };
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned kind = 0; kind < sizeof(operations) / sizeof(operations[0]); kind++) {
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "provider retirement: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        if (!cluster) { check(false, "provider retirement: cluster created"); os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[2] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), NULL};
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK,
              "provider retirement: child realm created");
        RetirementProvider p[2] = {0};
        for (unsigned i = 0; i < 2 && realms[i]; i++) {
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://providers.example/");
            options.shadow_trees = options.ranges = true;
            options.host_opaque = options.alert_opaque = &p[i];
            options.alert = retirement_alert; options.write = retirement_write;
            options.entropy = retirement_entropy; options.focus = retirement_focus;
            options.activate = retirement_activate; options.script_connected = retirement_script;
            options.broken = retirement_broken;
            options.now_ms = retirement_now_ms;
            options.now_us = kind == RP_CLOCK_NOW_MS ? NULL : retirement_now_us;
            options.timing = retirement_timing; options.zoom = retirement_zoom;
            options.screen = (os64_dom_screen_t){.width=1920, .height=1080, .color_depth=24};
            os64_dom_admission_t admission = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            p[i] = (RetirementProvider){.binding = admission.binding, .realm = realms[i], .kind = kind,
                                       .base = {.document = f.document, .width = 100 + (int)i}};
            check(admission.binding && admission.consumed, "provider retirement: binding installed");
            if (!admission.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (p[0].binding && p[1].binding) {
            for (unsigned i = 0; i < 2; i++) {
                p[i].sibling = p[1-i].binding;
                os64_dom_t *dom = p[i].binding;
                os64_dom_set_geometry(dom, retirement_geometry, &p[i]);
                os64_dom_set_hit(dom, retirement_hit, &p[i]);
                os64_dom_set_text_rects(dom, retirement_rects, &p[i]);
                os64_dom_set_svg_hit(dom, retirement_svg, &p[i]);
                os64_dom_set_computed(dom, retirement_computed, &p[i]);
                os64_dom_view_provider_t view = {.read = retirement_view, .scroll = retirement_scroll, .opaque = &p[i]};
                os64_dom_set_view(dom, &view);
                os64_dom_image_provider_t images = {.acquire = retirement_image, .release = retirement_image_release,
                                                   .update = retirement_image_update, .opaque = &p[i]};
                os64_dom_set_images(dom, &images);
                os64_dom_text_provider_t fonts = {retirement_font, &p[i]}; os64_dom_set_text(dom, &fonts);
                os64_dom_set_resizes(dom, retirement_resizes, &p[i]);
                os64_dom_set_intersections(dom, retirement_intersections, &p[i]);
                os64_dom_cookies_t cookies = {retirement_cookie_get, retirement_cookie_set, &p[i], retirement_cookie_enabled};
                os64_dom_set_cookies(dom, &cookies);
                os64_dom_set_user_agent(dom, retirement_agent, &p[i]);
                os64_dom_set_global_miss(dom, retirement_service_miss, &p[i]);
                os64_dom_history_t history = {retirement_history_update, retirement_history_fragment,
                                             retirement_history_length, &p[i]};
                os64_dom_set_history(dom, &history);
                os64_dom_storage_t storage = {retirement_storage_identity, retirement_storage_load,
                                             retirement_storage_commit, &p[i]};
                os64_dom_set_storage(dom, OS64_DOM_STORAGE_LOCAL, &storage);
                os64_dom_set_storage(dom, OS64_DOM_STORAGE_SESSION, &storage);
                check(os64_dom_history_moved(dom, "https://providers.example/", "{\"kept\":1}", 10),
                      "provider retirement: initial history snapshot installed");
                realm_script(realms[i], STORAGE_ASSERT
                    "globalThis.box=document.createElement('div');document.body.appendChild(box);"
                    "globalThis.text=document.createTextNode('words');box.appendChild(text);"
                    "globalThis.range=document.createRange();range.selectNodeContents(text);"
                    "globalThis.shape=document.createElementNS('http://www.w3.org/2000/svg','rect');"
                    "document.body.appendChild(shape);globalThis.picture=new Image();picture.src='image';"
                    "globalThis.canvas=document.createElement('canvas');canvas.id='provider-canvas';canvas.width=2;canvas.height=2;"
                    "globalThis.context=canvas.getContext('2d');document.body.appendChild(canvas);"
                    "globalThis.canvas2=document.createElement('canvas');canvas2.id='provider-canvas2';"
                    "document.body.appendChild(canvas2);canvas2.getContext('2d');"
                    "globalThis.random=new Uint8Array(4);"
                    "box.id='service-box';box.setAttribute('data-native','yes');globalThis.attr=box.getAttributeNode('data-native');"
                    "globalThis.input=document.createElement('input');input.required=true;document.body.appendChild(input);"
                    "globalThis.scripts=document.createDocumentFragment();"
                    "for(let i=0;i<2;i++){let s=document.createElement('script');s.text='void 0';scripts.appendChild(s)}"
                    "globalThis.missingProbe=()=>typeof serviceMissing;",
                    "provider retirement: retained objects prepared");
                realm_script(realms[i],
                    "globalThis.buffer=new ArrayBuffer(16);globalThis.channel=new MessageChannel();"
                    "globalThis.savedEvent=new Event('saved');globalThis.savedStamp=savedEvent.timeStamp;"
                    "globalThis.savedTiming=performance.timing.loadEventEnd;"
                    "globalThis.savedState=history.state;"
                    "globalThis.store=localStorage;globalThis.session=sessionStorage;"
                    "globalThis.storeRead=Storage.prototype.getItem;globalThis.storeKey=Storage.prototype.key;"
                    "globalThis.storeClear=Storage.prototype.clear;"
                    "globalThis.storeLength=Object.getOwnPropertyDescriptor(Storage.prototype,'length').get;"
                    "globalThis.link=document.createElement('a');link.href='/pending';document.body.appendChild(link);"
                    "globalThis.form=document.createElement('form');document.body.appendChild(form);"
                    "globalThis.media=matchMedia('(resolution: 1.25dppx)');",
                    "provider retirement: clock and media snapshots prepared");
            }
            JSContext *ac = os64_js_realm_context(realms[0], OS64_JS_ABI_ID, &out);
            JSContext *bc = os64_js_realm_context(realms[1], OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(ac);
            check(JS_SetPropertyStr(ac, global, "B", JS_GetGlobalObject(bc)) >= 0,
                  "provider retirement: child objects exported"); JS_FreeValue(ac, global);
            realm_script(realms[0],
                "globalThis.refused=f=>{try{f();return false}catch(e){return e.name==='InvalidStateError'}};"
                "globalThis.historyCallbacks=0;B.addEventListener('popstate',()=>historyCallbacks++);"
                "B.addEventListener('hashchange',()=>historyCallbacks++);"
                "globalThis.ro=new ResizeObserver(()=>{});ro.observe(B.box);"
                "globalThis.io=new IntersectionObserver(()=>{},{root:B.document});io.observe(B.box);",
                "provider retirement: active observer owns child target");
            if (kind == RP_DAMAGE || kind == RP_DAMAGE_RESET) {
                os64_dom_canvas_painted(p[1].binding, native_id(p[1].base.document->document, "provider-canvas"));
                os64_dom_canvas_painted(p[1].binding, native_id(p[1].base.document->document, "provider-canvas2"));
                realm_script(realms[1], kind == RP_DAMAGE ?
                    "context.fillRect(0,0,1,1);canvas2.getContext('2d').fillRect(0,0,1,1);" :
                    "canvas.width=3;canvas2.width=4;", "provider retirement: two dirty canvases prepared");
            }
            if (kind == RP_CLOCK_IDLE_NATIVE || kind == RP_CLOCK_RAF_NATIVE)
                realm_script(realms[1], kind == RP_CLOCK_IDLE_NATIVE ?
                    "requestIdleCallback(()=>{throw Error('retired idle ran')});" :
                    "requestAnimationFrame(()=>{throw Error('retired frame ran')});",
                    "provider retirement: native scheduler opportunity prepared");
            if (kind == RP_ZOOM_MEDIA_STEP) p[1].base.width++;
            if (kind == RP_CLOCK_WA_NATIVE)
                realm_script(realms[1], "box.animate([{opacity:0},{opacity:1}],1000);",
                             "provider retirement: animation update prepared");
            realm_script(realms[1], "link.click();", "provider retirement: navigation pending before mark");
            if (kind >= RP_STORAGE_IDENTITY && kind <= RP_STORAGE_COMMIT_CLOCK) {
                p[1].storage_expect_owed = true;
                realm_script(realms[1], "store.setItem('owed','before');session.setItem('owed','before');",
                             "provider retirement: two storage areas owe changes");
                if (kind <= RP_STORAGE_FAILED) {
                    os64_dom_storage_t storage = {retirement_storage_identity, retirement_storage_load,
                                                 retirement_storage_commit, &p[1]};
                    os64_dom_set_storage(p[1].binding, OS64_DOM_STORAGE_LOCAL, &storage);
                }
            }
            if (kind == RP_STORAGE_PRESSURE)
                realm_script(realms[1],
                    "globalThis.bigKey='k'.repeat(1300000);"
                    "for(let end of ['a','b']){store.setItem(bigKey+end,'1');store.removeItem(bigKey+end)}",
                    "provider retirement: removed keys fill storage change list near quota");
            uint64_t entry = os64_dom_history_entry(p[1].binding);
            memset(p[1].calls, 0, sizeof(p[1].calls)); p[1].armed = true;
            if (kind == RP_BROKEN)
                check(os64_html_remove_attr(p[1].base.document, native_id(p[1].base.document->document, "service-box"),
                      "data-native") == OS64_HTML_OK, "provider retirement: native attribute removal precedes diagnostic");
            if (operations[kind]) {
                char source[512];
                snprintf(source, sizeof(source), "globalThis.operationThrew=false;globalThis.operationError='';try{%s}catch(e){operationThrew=true;operationError=e.name}", operations[kind]);
                realm_script(realms[0], source, "provider retirement: operation unwinds after native mark");
                if (kind <= RP_COMPUTED)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='InvalidStateError');",
                                 "provider retirement: synchronous query refuses post-mark result");
                if (kind == RP_ENTROPY || kind == RP_COOKIE_READ)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='InvalidStateError');",
                                 "provider retirement: entropy and retrying cookies refuse after mark");
                if (kind == RP_FOCUS || kind == RP_HAS_FOCUS || kind == RP_AGENT || kind == RP_BROKEN)
                    realm_script(realms[0], "assert(!operationThrew);",
                                 "provider retirement: focus becomes inactive and borrowed agent text is copied");
                if ((kind >= RP_CLOCK_TIMER && kind <= RP_CLOCK_MEASURE) ||
                    (kind >= RP_ZOOM_RATIO && kind <= RP_ZOOM_MEDIA_READ) || kind == RP_CLOCK_NOW_MS)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='InvalidStateError');",
                                 "provider retirement: clock or zoom retirement refuses service completion");
                if (kind == RP_CLOCK_EVENT || kind == RP_TIMING || kind == RP_CLOCK_WA)
                    realm_script(realms[0], "assert(!operationThrew);",
                                 "provider retirement: passive snapshot survives native mark");
                if (kind == RP_HISTORY_LENGTH || kind == RP_HISTORY_PUSH || kind == RP_HISTORY_REPLACE ||
                    kind == RP_HISTORY_UPDATE_REFUSE)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='SecurityError');",
                                 "provider retirement: history refuses post-mark completion");
                if (kind == RP_HISTORY_FRAGMENT || kind == RP_HISTORY_FRAGMENT_REFUSE)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='InvalidStateError');",
                                 "provider retirement: fragment retirement prevents commit and fallback");
                if (kind >= RP_STORAGE_IDENTITY && kind <= RP_STORAGE_FAILED)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='InvalidStateError');",
                                 "provider retirement: storage read stops after native mark");
                if (kind == RP_STORAGE_PRESSURE)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='InvalidStateError');",
                                 "provider retirement: pressure commit retirement refuses new write");
            } else if (kind == RP_IMAGE_EVENT) {
                check(!os64_dom_image_event(p[1].binding, NULL, NULL, false),
                      "provider retirement: native image task stops after acquisition retirement");
            } else if (kind == RP_RESIZE) {
                (void)os64_dom_resizes_step(p[0].binding, &out);
            } else if (kind == RP_INTERSECTION) {
                (void)os64_dom_intersections_update(p[0].binding);
            } else if (kind == RP_ZOOM_MEDIA_STEP) {
                check(!os64_dom_view_step(p[1].binding, &out), "provider retirement: media sampling stops after mark");
            } else if (kind == RP_CLOCK_IDLE_NATIVE) {
                check(!os64_dom_idle_run(p[1].binding, &out), "provider retirement: native idle clock stops delivery");
            } else if (kind == RP_CLOCK_RAF_NATIVE) {
                check(!os64_dom_animation_fire(p[1].binding, 2000, &out), "provider retirement: native frame clock stops delivery");
            } else if (kind == RP_CLOCK_WA_NATIVE) {
                check(!os64_dom_web_animations_update(p[1].binding, 2000, &out),
                      "provider retirement: native animation clock stops update");
            } else if (kind == RP_STORAGE_COMMIT || kind == RP_STORAGE_CLOCK || kind == RP_STORAGE_COMMIT_CLOCK) {
                os64_dom_storage_settle(p[1].binding);
                check(p[1].storage_commits == (kind == RP_STORAGE_COMMIT ? 1u : 0u),
                      "provider retirement: ordinary storage settlement stops at retirement");
            } else {
                os64_dom_canvas_visit_damage(p[1].binding, retirement_damage, &p[1]);
            }
            char completion[160];
            snprintf(completion, sizeof(completion),
                     "provider retirement: one provider call marks child and no second pass follows (kind %u, calls %u)",
                     kind, p[1].calls[kind]);
            check(os64_js_realm_is_retired(realms[1]) &&
                  p[1].calls[kind] == (kind == RP_STORAGE_COMMIT_CLOCK ? 2u : 1u), completion);
            check(!d_provider_busy(p[0].binding) && !d_provider_busy(p[1].binding),
                  "provider retirement: loans balance after success or refusal");
            retirement_no_navigation(&p[1], "provider retirement: pending navigation discarded after mark");
            retirement_no_navigation(&p[1], "provider retirement: discarded navigation stays empty");
            check(os64_dom_history_entry(p[1].binding) == entry &&
                  !os64_dom_history_moved(p[1].binding, "https://providers.example/late", NULL, 0),
                  "provider retirement: no local history commit after mark");
            os64_dom_history_event_t late_history = {.entry=entry};
            check(os64_dom_history_event(p[1].binding, &late_history, &out) == OS64_JS_OK,
                  "provider retirement: obsolete native history event discarded");
            late_history = (os64_dom_history_event_t){.hashchange=true, .old_url="https://providers.example/",
                                                    .new_url="https://providers.example/late"};
            check(os64_dom_history_event(p[1].binding, &late_history, &out) == OS64_JS_OK,
                  "provider retirement: obsolete native hashchange discarded");
            check(os64_dom_timer_next(p[1].binding) == UINT64_MAX &&
                  os64_dom_animation_next(p[1].binding) == UINT64_MAX &&
                  os64_dom_idle_next(p[1].binding) == UINT64_MAX &&
                  os64_dom_web_animations_next(p[1].binding) == UINT64_MAX &&
                  os64_dom_message_next(p[1].binding) == UINT64_MAX,
                  "provider retirement: clock retirement publishes no scheduler opportunity");
            if (kind == RP_HIT || kind == RP_RECTS) {
                os64_dom_geometry_stats_t stats = os64_dom_geometry_stats(p[1].binding, false);
                check(stats.layouts == 1 && stats.elapsed_us == 7,
                      "provider retirement: refused query retains completed provider cost");
            }
            if (kind == RP_IMAGE || kind == RP_IMAGE_EVENT)
                check(p[1].calls[RP_RELEASE] == 1, "provider retirement: acquired image released after mark");
            if (kind == RP_IMAGE)
                realm_script(realms[0], "assert(!operationThrew&&context.getImageData(0,0,1,1).data[0]===255);",
                             "provider retirement: borrowed pixels remain usable until release");
            unsigned calls[RP_COUNT]; memcpy(calls, p[1].calls, sizeof(calls));
            realm_script(realms[0],
                "assert(refused(()=>B.localStorage));assert(refused(()=>B.sessionStorage));"
                "assert(refused(()=>B.storeRead.call(B.store,'seed')));"
                "assert(refused(()=>B.storeKey.call(B.store,0)));assert(refused(()=>B.storeLength.call(B.store)));"
                "assert(refused(()=>B.storeClear.call(B.store)));assert(refused(()=>{B.store.x='late'}));"
                "assert(refused(()=>{delete B.store.seed}));assert(refused(()=>Object.keys(B.store)));"
                "assert(refused(()=>Storage.prototype.getItem.call(B.store,'seed')));"
                "assert(B.storeRead.call(localStorage,'seed')==='kept');"
                "assert(B.storeKey.call(localStorage,0)==='seed'&&B.storeLength.call(localStorage)===1);"
                "B.storeClear.call(localStorage);assert(localStorage.length===0);",
                "provider retirement: storage receiver owns service despite borrowed method realm");
            check(os64_dom_storage_due(p[1].binding) == UINT64_MAX,
                  "provider retirement: departed storage has no ordinary due work");
            os64_dom_storage_settle(p[1].binding);
            os64_dom_storage_forget(p[1].binding);
            realm_script(realms[0],
                "let security=f=>{try{f();return false}catch(e){return e.name==='SecurityError'}};"
                "assert(security(()=>B.history.length));assert(security(()=>B.history.state));"
                "assert(security(()=>B.history.scrollRestoration));"
                "assert(security(()=>{B.history.scrollRestoration='manual'}));"
                "assert(security(()=>B.history.back()));assert(security(()=>B.history.forward()));"
                "assert(security(()=>B.history.go(0)));assert(security(()=>B.history.go(-1)));"
                "assert(security(()=>B.history.pushState(null,'')));assert(security(()=>B.history.replaceState(null,'')));"
                "assert(refused(()=>B.location.reload()));assert(refused(()=>B.location.assign('/late')));"
                "assert(refused(()=>B.location.replace('/late')));assert(refused(()=>{B.location.hash='late'}));"
                "assert(refused(()=>{B.location.href='/late'}));assert(refused(()=>{B.location.hash=''}));"
                "assert(B.location.href==='https://providers.example/'&&B.savedState.kept===1);"
                "assert(historyCallbacks===0);"
                "B.savedState.kept=3;assert(B.savedState.kept===3);B.link.click();B.form.submit();"
                "assert(history.length===4);history.pushState({sibling:1},'','#sibling');"
                "assert(history.state.sibling===1&&location.hash==='#sibling');",
                "provider retirement: history services refuse while exported state and sibling history survive");
            retirement_no_navigation(&p[1], "provider retirement: inactive actions enqueue no new navigation");
            if (kind != RP_BROKEN)
                check(os64_html_remove_attr(p[1].base.document, native_id(p[1].base.document->document, "service-box"),
                      "data-native") == OS64_HTML_OK, "provider retirement: native attribute removal after mark");
            realm_script(realms[0],
                "assert(refused(()=>B.box.getBoundingClientRect()));"
                "assert(refused(()=>B.document.elementsFromPoint(1,1)));assert(refused(()=>B.range.getClientRects()));"
                "assert(refused(()=>B.shape.isPointInFill({x:1,y:1})));assert(refused(()=>B.innerWidth));"
                "assert(refused(()=>B.scrollTo(1,2)));assert(refused(()=>B.getComputedStyle(B.box).width));"
                "assert(refused(()=>B.picture.naturalWidth));assert(refused(()=>B.context.measureText('x')));"
                "assert(refused(()=>context.drawImage(B.picture,0,0)));"
                "assert(refused(()=>B.alert('late')));assert(refused(()=>B.document.write('late')));"
                "assert(refused(()=>B.document.open()));assert(refused(()=>B.crypto.getRandomValues(B.random)));"
                "assert(refused(()=>B.crypto.randomUUID()));assert(B.random.every(x=>x===0));"
                "assert(B.document.activeElement===null&&!B.document.hasFocus());"
                "B.box.focus();B.box.blur();B.input.reportValidity();B.missingProbe();"
                "B.document.body.appendChild(B.scripts);"
                "assert(refused(()=>B.document.cookie));assert(refused(()=>{B.document.cookie='late'}));"
                "assert(refused(()=>B.navigator.cookieEnabled));assert(refused(()=>B.navigator.userAgent));"
                "assert(B.navigator.appName==='Netscape'&&B.navigator.language.length>0);"
                "assert(B.attr.value===''&&B.attr.ownerElement===null);"
                "assert(B.buffer.byteLength===16);"
                "assert(refused(()=>B.performance.now()));assert(refused(()=>B.performance.mark('late')));"
                "assert(refused(()=>B.performance.measure('late')));"
                "assert(B.performance.mark('explicit',{startTime:3}).startTime===3);"
                "assert(B.performance.measure('explicit',{start:3,end:5}).duration===2);"
                "assert(B.savedEvent.timeStamp===B.savedStamp&&new B.Event('late').timeStamp===B.savedStamp);"
                "assert(B.performance.timing.loadEventEnd===B.savedTiming&&B.performance.timeOrigin===2000);"
                "assert(B.performance.navigation.type===1&&B.performance.navigation.redirectCount===3);"
                "assert(refused(()=>B.devicePixelRatio));assert(refused(()=>B.screen.width));"
                "assert(refused(()=>B.media.matches));assert(refused(()=>B.matchMedia('screen')));"
                "assert(B.screen.colorDepth===24&&typeof B.media.media==='string');"
                "assert(B.crypto.getRandomValues(new Uint8Array(0)).length===0);"
                "globalThis.digestLength=0;B.crypto.subtle.digest('SHA-256',new Uint8Array()).then(b=>digestLength=b.byteLength);"
                "B.context.fillStyle='red';B.context.fillRect(0,0,1,1);"
                "assert(B.context.getImageData(0,0,1,1).data[0]===255);"
                "assert(box.getBoundingClientRect().width===100);",
                "provider retirement: inactive services refuse but pure pixels and sibling services survive");
            realm_script(realms[0], "assert(digestLength===32);",
                         "provider retirement: retained pure cryptography needs no host entropy");
            os64_dom_canvas_visit_damage(p[1].binding, retirement_damage, &p[1]);
            check(!os64_dom_image_event(p[1].binding, NULL, NULL, false) &&
                  !os64_dom_view_step(p[1].binding, &out) && !memcmp(calls, p[1].calls, sizeof(calls)),
                  "provider retirement: no late native or script provider calls");
            os64_dom_set_history(p[1].binding, NULL);
            realm_script(realms[0],
                "assert(security(()=>B.history.length));assert(security(()=>B.history.pushState(null,'','#none')));"
                "assert(refused(()=>B.location.assign('#none')));"
                "assert(B.location.href==='https://providers.example/'&&B.savedState.kept===3);",
                "provider retirement: removing history host does not revive local navigation");
            realm_script(realms[0], "document.body.appendChild(B.box);assert(B.box.getBoundingClientRect().width===100);",
                         "provider retirement: adopted retained node uses active destination provider");
            check(!d_provider_busy(p[0].binding), "provider retirement: sibling remains quiescent after adoption query");
            /* The native cleanup entry is exercised directly until public
             * departure finish supplies its broader safe-point barriers. */
            d_storage_drain(p[1].binding);
            unsigned commits = kind >= RP_STORAGE_IDENTITY && kind <= RP_STORAGE_COMMIT_CLOCK ? 2 :
                kind == RP_STORAGE_PRESSURE ? 1 : 0;
            check(p[1].storage_commits == commits && !d_provider_busy(p[0].binding),
                  "provider retirement: cleanup settles both owed areas exactly once");
            d_storage_drain(p[1].binding);
            check(p[1].storage_commits == commits, "provider retirement: repeated storage cleanup is inert");
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "provider retirement: native and engine holds released");
    }
}

static void provider_conversion_retirement_cases(void)
{
    static const struct { const char *source, *error; bool memory_storage; } operations[] = {
        {"alert({toString(){retire();return 'late'}})", "InvalidStateError", false},
        {"document.write('first',{toString(){retire();return 'late'}})", "InvalidStateError", false},
        {"document.writeln({toString(){retire();return 'late'}},'last')", "InvalidStateError", false},
        {"document.cookie={toString(){retire();return 'late'}}", "InvalidStateError", false},
        {"input.reportValidity()", "", false},
        {"history.pushState({}, {toString(){retire();return ''}}, '#title')", "SecurityError", false},
        {"history.replaceState({}, '', {toString(){retire();return '#url'}})", "SecurityError", false},
        {"history.pushState({get x(){retire();return 1}}, '', '#state')", "SecurityError", false},
        {"history.go({valueOf(){retire();return -1}})", "SecurityError", false},
        {"history.scrollRestoration={toString(){retire();return 'manual'}}", "SecurityError", false},
        {"location.assign({toString(){retire();return '/late'}})", "InvalidStateError", false},
        {"location.replace({toString(){retire();return '#late'}})", "InvalidStateError", false},
        {"location.hash={toString(){retire();return ''}}", "InvalidStateError", false},
        {"form.requestSubmit()", "", false},
        {"link.click()", "", false},
        {"Storage.prototype.getItem.call(store,{toString(){retire();return 'seed'}})", "InvalidStateError", false},
        {"Storage.prototype.setItem.call(store,'key',{toString(){retire();return 'late'}})", "InvalidStateError", false},
        {"Storage.prototype.removeItem.call(store,{toString(){retire();return 'seed'}})", "InvalidStateError", false},
        {"Storage.prototype.key.call(store,{valueOf(){retire();return 0}})", "InvalidStateError", false},
        {"store.key={toString(){retire();return 'late'}}", "InvalidStateError", false},
        {"Object.defineProperty(store,'key',{value:{toString(){retire();return 'late'}}})", "InvalidStateError", false},
        {"Object.setPrototypeOf(store,new Proxy({}, {has(){visibilityCalls++;retire();return false}}));Object.keys(store)",
         "InvalidStateError", true},
        {"x.open('POST',{toString(){retire();return '/late'}})", "InvalidStateError", false},
        {"x.send({toString(){retire();return 'late'}})", "InvalidStateError", false},
        {"x.onloadstart=retire;x.send('late')", "InvalidStateError", false},
        {"x.upload.onloadstart=retire;x.send('late')", "InvalidStateError", false}
    };
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
        os64_js_config_t config = fixture_config();
        DomFixture f = fixture_configured(0, 0, &config);
        os64_js_destroy(f.runtime); f.runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &f.runtime, &out) == OS64_JS_OK,
              "provider conversion: runtime created");
        retirement_realms[1] = os64_js_primary_realm(f.runtime, OS64_JS_ABI_ID, &out);
        RetirementProvider p = {.realm=retirement_realms[1], .base.document=f.document};
        os64_dom_options_t options = p2b_options("https://providers.example/");
        options.alert = retirement_alert; options.alert_opaque = &p;
        options.host_opaque = &p; options.write = retirement_write; options.activate = retirement_activate;
        f.binding = os64_dom_create(f.runtime, f.document, f.state, &options, &out);
        p.binding = p.sibling = f.binding;
        check(f.binding != NULL, "provider conversion: binding installed");
        if (f.binding) {
            os64_dom_cookies_t cookies = {retirement_cookie_get, retirement_cookie_set, &p, retirement_cookie_enabled};
            os64_dom_set_cookies(f.binding, &cookies);
            os64_dom_history_t history = {retirement_history_update, retirement_history_fragment,
                                         retirement_history_length, &p};
            os64_dom_set_history(f.binding, &history);
            if (!operations[i].memory_storage) {
                os64_dom_storage_t storage = {retirement_storage_identity, retirement_storage_load,
                                             retirement_storage_commit, &p};
                os64_dom_set_storage(f.binding, OS64_DOM_STORAGE_LOCAL, &storage);
            }
            check(os64_js_realm_task_begin(p.realm, OS64_JS_ABI_ID, "provider conversion", &out) == OS64_JS_OK,
                  "provider conversion: setup task begins");
            JSContext *ctx = os64_js_realm_context(p.realm, OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(ctx);
            check(JS_SetPropertyStr(ctx, global, "retire", JS_NewCFunctionMagic(ctx, retirement_mark,
                  "retire", 0, JS_CFUNC_generic_magic, 1)) >= 0, "provider conversion: trusted mark exposed");
            JS_FreeValue(ctx, global);
            check(os64_js_task_end(f.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                  "provider conversion: setup task ends");
            char source[2400];
            snprintf(source, sizeof(source), STORAGE_ASSERT
                "globalThis.input=document.createElement('input');input.required=true;document.body.appendChild(input);"
                "input.addEventListener('invalid',retire);"
                "globalThis.form=document.createElement('form');document.body.appendChild(form);"
                "globalThis.link=document.createElement('a');link.href='/late';document.body.appendChild(link);"
                "form.addEventListener('submit',retire);link.addEventListener('click',retire);"
                "globalThis.store=localStorage;globalThis.visibilityCalls=0;%s"
                "globalThis.x=new XMLHttpRequest();x.open('POST','/send');"
                "let error='';try{%s}catch(e){error=e.name}assert(error==='%s');"
                "assert(visibilityCalls<=1);",
                operations[i].memory_storage ? "store.setItem('a','1');store.setItem('b','2');" : "",
                operations[i].source, operations[i].error);
            realm_script(p.realm, source, "provider conversion: retirement before host service");
            check(os64_js_realm_is_retired(p.realm), "provider conversion: conversion or event marked realm");
            unsigned total = 0;
            for (size_t j = 0; j < RP_COUNT; j++) total += p.calls[j];
            check(total == 0 && !d_provider_busy(f.binding),
                  "provider conversion: no host call or outstanding loan after mark");
            retirement_no_navigation(&p, "provider conversion: no navigation delivered after conversion or event retirement");
        }
        fixture_free(&f);
    }
}

static void storage_cleanup_retirement_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (int mode = 0; mode < 5; ++mode) {
        os64_js_config_t config = fixture_config();
        DomFixture f = fixture_configured(0, 0, &config);
        os64_js_destroy(f.runtime); f.runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &f.runtime, &out) == OS64_JS_OK,
              "storage cleanup: runtime created");
        os64_js_realm_t *realm = os64_js_primary_realm(f.runtime, OS64_JS_ABI_ID, &out);
        os64_dom_options_t options = p2b_options("https://cleanup.example/");
        f.binding = os64_dom_create(f.runtime, f.document, f.state, &options, &out);
        FakeStore store = {0};
        os64_dom_storage_t provider = {fake_identity, fake_load, fake_commit, &store};
        check(f.binding != NULL, "storage cleanup: binding created");
        if (f.binding) {
            os64_dom_set_storage(f.binding, OS64_DOM_STORAGE_LOCAL, &provider);
            realm_script(realm, "localStorage.setItem('owed','before');", "storage cleanup: owed write prepared");
            if (mode == 1) store.forced = OS64_DOM_STORAGE_FULL;
            if (mode == 2) store.forced = OS64_DOM_STORAGE_FAILED;
            if (mode == 4) os64_dom_drain(f.binding);
            else {
                check(os64_js_realm_retire(realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "storage cleanup: mark before native cleanup");
                if (mode == 3) dom_fail_at = dom_attempts + 1;
                d_storage_drain(f.binding);
                if (mode == 3) check(dom_attempts >= dom_fail_at, "storage cleanup: serialization allocation cut reached");
                dom_fail_at = 0;
            }
            unsigned expected = mode == 3 ? 0 : 1;
            check(store.commits == expected && !d_provider_busy(f.binding),
                  "storage cleanup: one final attempt balances provider loans");
            d_storage_drain(f.binding);
            os64_dom_storage_settle(f.binding);
            os64_dom_set_storage(f.binding, OS64_DOM_STORAGE_LOCAL, &provider);
            d_storage_drain(f.binding);
            check(store.commits == expected && os64_dom_storage_due(f.binding) == UINT64_MAX,
                  "storage cleanup: completed or failed final attempt cannot be retried or reattached");
        }
        fixture_free(&f);
        fake_store_free(&store);
    }
}

static void dom_provider_retirement_cases(void)
{
    provider_native_retirement_cases();
    provider_conversion_retirement_cases();
    storage_cleanup_retirement_cases();
}
