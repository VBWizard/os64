/* Trusted native retirement during provider callbacks. A busy loan is the
 * prerequisite for safe-point finish, not a claim that finish is implemented. */
bool d_provider_enter(os64_dom_t *dom);
void d_provider_leave(os64_dom_t *dom);
bool d_provider_busy(const os64_dom_t *dom);

enum {
    RP_GEOMETRY, RP_HIT, RP_RECTS, RP_SVG, RP_VIEW, RP_SCROLL, RP_COMPUTED,
    RP_IMAGE, RP_IMAGE_EVENT, RP_FONT, RP_RESIZE, RP_INTERSECTION, RP_DAMAGE, RP_DAMAGE_RESET, RP_UPDATE,
    RP_RELEASE, RP_COUNT
};
typedef struct {
    ClusterProvider base;
    os64_dom_t *binding, *sibling;
    os64_js_realm_t *realm;
    unsigned kind, calls[RP_COUNT];
    bool armed;
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
static void dom_provider_retirement_cases(void)
{
    const char *operations[] = {
        "B.box.getBoundingClientRect()", "B.document.elementsFromPoint(1,1)", "B.range.getClientRects()",
        "B.shape.isPointInFill({x:1,y:1})", "B.innerWidth", "B.scrollTo(1,2)",
        "B.getComputedStyle(B.box).width", "context.drawImage(B.picture,0,0)", NULL,
        "B.context.measureText('x')", NULL, NULL, NULL, NULL
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
                realm_script(realms[i], STORAGE_ASSERT
                    "globalThis.box=document.createElement('div');document.body.appendChild(box);"
                    "globalThis.text=document.createTextNode('words');box.appendChild(text);"
                    "globalThis.range=document.createRange();range.selectNodeContents(text);"
                    "globalThis.shape=document.createElementNS('http://www.w3.org/2000/svg','rect');"
                    "document.body.appendChild(shape);globalThis.picture=new Image();picture.src='image';"
                    "globalThis.canvas=document.createElement('canvas');canvas.id='provider-canvas';canvas.width=2;canvas.height=2;"
                    "globalThis.context=canvas.getContext('2d');document.body.appendChild(canvas);"
                    "globalThis.canvas2=document.createElement('canvas');canvas2.id='provider-canvas2';"
                    "document.body.appendChild(canvas2);canvas2.getContext('2d');",
                    "provider retirement: retained objects prepared");
            }
            JSContext *ac = os64_js_realm_context(realms[0], OS64_JS_ABI_ID, &out);
            JSContext *bc = os64_js_realm_context(realms[1], OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(ac);
            check(JS_SetPropertyStr(ac, global, "B", JS_GetGlobalObject(bc)) >= 0,
                  "provider retirement: child objects exported"); JS_FreeValue(ac, global);
            realm_script(realms[0],
                "globalThis.refused=f=>{try{f();return false}catch(e){return e.name==='InvalidStateError'}};"
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
            memset(p[1].calls, 0, sizeof(p[1].calls)); p[1].armed = true;
            if (operations[kind]) {
                char source[512];
                snprintf(source, sizeof(source), "globalThis.operationThrew=false;globalThis.operationError='';try{%s}catch(e){operationThrew=true;operationError=e.name}", operations[kind]);
                realm_script(realms[0], source, "provider retirement: operation unwinds after native mark");
                if (kind <= RP_COMPUTED)
                    realm_script(realms[0], "assert(operationThrew&&operationError==='InvalidStateError');",
                                 "provider retirement: synchronous query refuses post-mark result");
            } else if (kind == RP_IMAGE_EVENT) {
                check(!os64_dom_image_event(p[1].binding, NULL, NULL, false),
                      "provider retirement: native image task stops after acquisition retirement");
            } else if (kind == RP_RESIZE) {
                (void)os64_dom_resizes_step(p[0].binding, &out);
            } else if (kind == RP_INTERSECTION) {
                (void)os64_dom_intersections_update(p[0].binding);
            } else {
                os64_dom_canvas_visit_damage(p[1].binding, retirement_damage, &p[1]);
            }
            check(os64_js_realm_is_retired(realms[1]) && p[1].calls[kind] == 1,
                  "provider retirement: one provider call marks child and no second pass follows");
            check(!d_provider_busy(p[0].binding) && !d_provider_busy(p[1].binding),
                  "provider retirement: loans balance after success or refusal");
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
                "assert(refused(()=>B.box.getBoundingClientRect()));"
                "assert(refused(()=>B.document.elementsFromPoint(1,1)));assert(refused(()=>B.range.getClientRects()));"
                "assert(refused(()=>B.shape.isPointInFill({x:1,y:1})));assert(refused(()=>B.innerWidth));"
                "assert(refused(()=>B.scrollTo(1,2)));assert(refused(()=>B.getComputedStyle(B.box).width));"
                "assert(refused(()=>B.picture.naturalWidth));assert(refused(()=>B.context.measureText('x')));"
                "assert(refused(()=>context.drawImage(B.picture,0,0)));"
                "B.context.fillStyle='red';B.context.fillRect(0,0,1,1);"
                "assert(B.context.getImageData(0,0,1,1).data[0]===255);"
                "assert(box.getBoundingClientRect().width===100);",
                "provider retirement: inactive services refuse but pure pixels and sibling services survive");
            os64_dom_canvas_visit_damage(p[1].binding, retirement_damage, &p[1]);
            check(!os64_dom_image_event(p[1].binding, NULL, NULL, false) &&
                  !os64_dom_view_step(p[1].binding, &out) && !memcmp(calls, p[1].calls, sizeof(calls)),
                  "provider retirement: no late native or script provider calls");
            realm_script(realms[0], "document.body.appendChild(B.box);assert(B.box.getBoundingClientRect().width===100);",
                         "provider retirement: adopted retained node uses active destination provider");
            check(!d_provider_busy(p[0].binding), "provider retirement: sibling remains quiescent after adoption query");
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "provider retirement: native and engine holds released");
    }
}
