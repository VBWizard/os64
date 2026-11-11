#include "../userland/libdom/tests/realm_cases.h"
/* Explicit binding realm routing. A secondary realm owns the DOM while the
 * primary keeps an independent exception host and pending rejection queue.
 * This is not the cross-binding adoption/cluster fixture. */
static unsigned realm_primary_errors;
static void realm_primary_error(void *opaque, JSContext *ctx, JSValue error)
{
    (void)ctx; (void)error;
    check(opaque == &realm_primary_errors, "realm: primary provider identity");
    realm_primary_errors++;
}
static bool realm_script(os64_js_realm_t *realm, const char *source, const char *name)
{
    os64_js_outcome_t out;
    os64_js_status_t status = os64_js_realm_run(realm, source, strlen(source), name, &out);
    if (status != OS64_JS_OK)
        fprintf(stderr, "realm script %s: status=%d %s\n%s\n", name, status, out.message, out.stack_trace);
    check(status == OS64_JS_OK, name);
    return status == OS64_JS_OK;
}
static void dom_realm_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        os64_js_config_t config = fixture_config();
        DomFixture f = fixture_configured(0, 0, &config);
        os64_js_destroy(f.runtime);
        f.runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &f.runtime, &out) == OS64_JS_OK,
              "realm: configured runtime");
        os64_js_realm_t *primary = os64_js_primary_realm(f.runtime, OS64_JS_ABI_ID, &out), *child = NULL;
        check(os64_js_realm_create(f.runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK,
              "realm: secondary created");
        realm_primary_errors = 0;
        os64_js_script_host_t host = {.opaque = &realm_primary_errors, .exception = realm_primary_error};
        check(os64_js_realm_set_script_host(primary, OS64_JS_ABI_ID, &host, &out) == OS64_JS_OK,
              "realm: independent primary exception host");
        p2b_ms = 1000; p2b_us = 1000000;
        os64_dom_options_t options = p2b_options("https://child.example/page");
        os64_js_runtime_t *foreign = create_fixture(&config);
        os64_js_realm_t *foreign_realm = os64_js_primary_realm(foreign, OS64_JS_ABI_ID, &out);
        size_t before = dom_attempts;
        check(d_create_in_realm(f.runtime, foreign_realm, f.document, f.state, &options, &out) == NULL &&
              out.status == OS64_JS_BAD_ARGUMENT && dom_attempts == before,
              "realm: foreign-runtime installation refused before native allocation");
        os64_js_destroy(foreign);
        f.binding = d_create_in_realm(f.runtime, child, f.document, f.state, &options, &out);
        check(f.binding != NULL && out.status == OS64_JS_OK, "realm: DOM installs in secondary");
        if (!f.binding) { fixture_free(&f); continue; }
        realm_script(primary, "if(typeof document!=='undefined'||typeof setTimeout!=='undefined')throw Error('primary polluted')",
                     "realm: installation leaves primary globals alone");
        realm_script(child, STORAGE_ASSERT
            "assert(document.URL==='https://child.example/page');globalThis.seen=[];"
            "setTimeout(function(){assert(this===window);seen.push('timer');"
            "document.body.setAttribute('data-timer','child')},0);"
            "setTimeout(\"seen.push('string')\",0);"
            "requestAnimationFrame(function(){assert(this===window);seen.push('frame')});"
            "requestIdleCallback(()=>seen.push('idle'));"
            "addEventListener('resize',()=>{seen.push('event');document.body.dataset.event='child'});"
            "globalThis.observed=0;new MutationObserver(()=>observed++).observe(document.body,{attributes:true});",
            "realm: callbacks installed in secondary");
        check(os64_dom_timer_fire(f.binding, p2b_ms, &out) && out.status == OS64_JS_OK,
              "realm: host timer enters secondary");
        check(os64_dom_timer_fire(f.binding, p2b_ms, &out) && out.status == OS64_JS_OK,
              "realm: string timer evaluates in secondary");
        p2b_advance(20);
        check(os64_dom_animation_fire(f.binding, p2b_ms, &out) && out.status == OS64_JS_OK,
              "realm: host animation enters secondary");
        check(os64_dom_idle_run(f.binding, &out) && out.status == OS64_JS_OK,
              "realm: host idle enters secondary");
        os64_dom_event_t event = {.type = "resize"};
        bool prevented = false;
        check(os64_dom_dispatch(f.binding, NULL, &event, &prevented, &out) == OS64_JS_OK,
              "realm: host event enters secondary");
        realm_script(child, "assert(seen.join()==='timer,string,frame,idle,event',seen.join());"
            "assert(observed===2&&document.body.dataset.timer==='child'&&document.body.dataset.event==='child');"
            "globalThis.errors=0;globalThis.rejections=[];"
            "addEventListener('error',e=>{errors++;e.preventDefault()});"
            "addEventListener('unhandledrejection',e=>{rejections.push(e.reason);e.preventDefault()})",
            "realm: callbacks and observer use secondary DOM");
        const char *thrown = "throw Error('child failure')";
        check(os64_js_realm_run(child, thrown, strlen(thrown), "child-error.js", &out) == OS64_JS_EXCEPTION,
              "realm: child exception reported");
        check(os64_dom_script_ended(f.binding, "child-error.js", &out), "realm: child error delivery completes");
        realm_script(child, "assert(errors===1)", "realm: child error reaches child listener");
        check(realm_primary_errors == 0, "realm: child error bypasses primary provider");
        realm_script(child, "setTimeout(()=>{throw Error('child timer failure')},0);", "realm: throwing child timer");
        check(os64_dom_timer_fire(f.binding, p2b_ms, &out), "realm: throwing timer fired");
        realm_script(child, "assert(errors===2)", "realm: timer error uses child report host");
        check(realm_primary_errors == 0, "realm: timer error leaves primary provider alone");
        realm_script(primary, "Promise.reject('primary debt')", "realm: primary rejection queued");
        const char *rejected = "Promise.reject('child debt')";
        check(os64_js_realm_run(child, rejected, strlen(rejected), "child-rejection.js", &out) == OS64_JS_OK,
              "realm: child rejection queued");
        check(os64_dom_script_ended(f.binding, "child-rejection.js", &out), "realm: child rejection delivery completes");
        realm_script(child, "assert(rejections.join()==='child debt',rejections.join())", "realm: child takes its rejection");
        JSValue promise, reason; uint32_t tag;
        bool taken = os64_js_realm_take_rejection(primary, OS64_JS_ABI_ID, &promise, &reason, &tag);
        check(taken, "realm: primary rejection survives child delivery");
        if (taken) {
            JSContext *ctx = os64_js_realm_context(primary, OS64_JS_ABI_ID, &out);
            const char *text = JS_ToCString(ctx, reason);
            check(text && !strcmp(text, "primary debt"), "realm: retained rejection belongs to primary");
            JS_FreeCString(ctx, text); JS_FreeValue(ctx, promise); JS_FreeValue(ctx, reason);
        }
        check(os64_dom_create(f.runtime, f.document, f.state, &options, &out) == NULL && out.status == OS64_JS_BAD_ARGUMENT,
              "realm: duplicate installation rejected before changing primary");
        realm_script(child, "assert(document.body.dataset.timer==='child')", "realm: duplicate refusal preserves binding");
        memory_balanced(f.binding, "realm before drain");
        os64_dom_drain(f.binding);
        memory_balanced(f.binding, "realm after drain");
        thrown = "throw Error('primary still configured')";
        check(os64_js_realm_run(primary, thrown, strlen(thrown), "primary-error.js", &out) == OS64_JS_EXCEPTION &&
              realm_primary_errors == 1, "realm: child drain preserves primary exception host");
        os64_js_teardown_report_t report;
        os64_js_destroy_report(f.runtime, &report);
        os64_dom_free(f.binding);
        os64_page_state_free(f.state);
        os64_html_document_free(f.document);
        check(live == 0 && dom_live == 0 && !dom_allocations, "realm: both teardown policies release native owners");
        check(policy == OS64_JS_TEARDOWN_RECLAIM || !report.leaked, "realm: finalizer teardown has no leftovers");
    }
}

typedef struct {
    os64_html_document_t *document;
    int32_t width;
    unsigned calls, damages, scrolls, resets, reports, resize_calls;
    bool resize_refuse;
    double scroll_x, scroll_y;
} ClusterProvider;
static bool cluster_geometry(void *opaque, const os64_html_node_t *node, os64_dom_geometry_t *out)
{
    ClusterProvider *provider = opaque;
    check(os64_html_node_owner(node) == provider->document, "cluster provider: current document owns receiver");
    provider->calls++;
    *out = (os64_dom_geometry_t){.offset_width = provider->width, .boxed = true,
        .scroll_left = (int32_t)provider->scroll_x, .scroll_top = (int32_t)provider->scroll_y};
    return true;
}
static bool cluster_resizes(void *opaque, os64_dom_resize_t *items, size_t count,
                            os64_dom_geometry_stats_t *cost)
{
    ClusterProvider *p = opaque;
    p->resize_calls++;
    cost->layouts = 1;
    if (p->resize_refuse) return false;
    for (size_t i = 0; i < count; i++) {
        check(os64_html_node_owner(items[i].target) == p->document,
              "cluster resize: current document provider owns each target");
        items[i].boxed = true;
        items[i].content[0] = items[i].border[0] = items[i].device[0] = p->width;
        items[i].content[1] = items[i].border[1] = items[i].device[1] = 5;
    }
    return true;
}
static bool cluster_computed(void *opaque, const os64_html_node_t *node, os64_dom_computed_t *out)
{
    ClusterProvider *p = opaque;
    check(os64_html_node_owner(node) == p->document, "cluster computed: destination provider");
    const char *color = p->width == 100 ? "red" : p->width == 101 ? "green" : "blue";
    const os64_html_attr_t *display = os64_html_attr(node, "data-render");
    const char *box = display ? display->value : "block";
    return os64_dom_computed_put(out, GARB_COLOR, color, strlen(color)) &&
           os64_dom_computed_put(out, GARB_DISPLAY, box, strlen(box));
}
static os64_dom_image_status_t cluster_image(void *opaque, const os64_html_node_t *node,
                                            bool pixels, os64_dom_image_view_t *out)
{
    (void)pixels;
    ClusterProvider *p = opaque;
    check(os64_html_node_owner(node) == p->document, "cluster image: destination provider");
    *out = (os64_dom_image_view_t){.surface = {.width = p->width, .height = 1}};
    return OS64_DOM_IMAGE_READY;
}
static void cluster_damage(void *opaque, const os64_html_node_t *node, uint32_t width, uint32_t height,
                            const draw2d_damage_t *damage)
{
    (void)width; (void)height; (void)damage;
    ClusterProvider *provider = opaque;
    check(os64_html_node_owner(node) == provider->document, "cluster damage: destination host receives pixels");
    provider->damages++;
}

static bool cluster_scroll(void *opaque, const os64_html_node_t *node, double x, double y, bool by,
                           os64_dom_geometry_stats_t *cost)
{
    (void)cost;
    ClusterProvider *p = opaque;
    check(os64_html_node_owner(node) == p->document, "cluster scroll: destination provider owns target");
    p->scrolls++;
    if (!isnan(x)) p->scroll_x = by ? p->scroll_x + x : x;
    if (!isnan(y)) p->scroll_y = by ? p->scroll_y + y : y;
    return true;
}

static void cluster_activate(void *opaque, const os64_html_node_t *node, os64_dom_activation_t what)
{
    ClusterProvider *p = opaque;
    check(os64_html_node_owner(node) == p->document, "cluster activation: destination provider owns target");
    if (what == OS64_DOM_ACTIVATE_RESET) p->resets++;
    if (what == OS64_DOM_ACTIVATE_REPORT) p->reports++;
}

static void cluster_navigation(os64_dom_t **bindings, ClusterProvider *providers, int destination,
                               os64_dom_navigation_kind_t kind, const char *label)
{
    for (int i = 0; i < 3; ++i) {
        os64_dom_navigation_t ask;
        bool asked = os64_dom_take_navigation(bindings[i], &ask);
        check(asked == (i == destination) && (!asked || ask.kind == kind), label);
        if (ask.node) {
            check(os64_html_node_owner(ask.node) == providers[i].document,
                  "cluster navigation: request belongs to current document");
            os64_html_release(providers[i].document, ask.node);
        }
        if (ask.submitter) os64_html_release(providers[i].document, ask.submitter);
    }
}

static void cluster_pointer(os64_dom_t *dom, os64_html_document_t *document, const char *id,
                            os64_dom_pointer_action_t action, int button, unsigned buttons)
{
    os64_dom_pointer_sample_t sample = {.action = action, .hit = native_id(document->document, id),
        .event = {.button = button, .buttons = buttons}};
    os64_js_outcome_t out;
    check(os64_dom_pointer_dispatch(dom, &sample, NULL, NULL, NULL, &out) == OS64_JS_OK,
          "cluster round6: destination pointer packet completes");
}

static unsigned cluster_started;
static void cluster_script_connected(void *opaque, const os64_html_node_t *node)
{
    (void)opaque; (void)node;
    cluster_started++;
}

static void dom_cluster_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        os64_js_config_t config = fixture_config();
        config.limits.memory_bytes = 64u * 1024 * 1024;
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "cluster: runtime created");
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        check(cluster && out.status == OS64_JS_OK, "cluster: runtime claimed");
        check(!os64_dom_cluster_create(runtime, &limits, &out) && out.status == OS64_JS_BAD_ARGUMENT,
              "cluster: duplicate claim refused");
        if (!cluster) { os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[3] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out)};
        os64_dom_t *bindings[3] = {0};
        ClusterProvider providers[3] = {0};
        for (size_t i = 0; i < 3; ++i) {
            if (i) check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[i], &out) == OS64_JS_OK,
                         "cluster: child realm created");
            DomFixture f = fixture_configured(0, 0, &config);
            os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options(i == 0 ? "https://a.example/" :
                                                   i == 1 ? "https://b.example/" : "https://c.example/");
            options.ranges = options.shadow_trees = true;
            options.script_connected = cluster_script_connected;
            options.activate = cluster_activate;
            options.host_opaque = &providers[i];
            if (i == 2) f.document->quirks = OS64_HTML_QUIRKS;
            os64_dom_admission_t result = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            check(result.binding && result.consumed && out.status == OS64_JS_OK, "cluster: independent DOM installed");
            if (!result.binding) fprintf(stderr,"cluster installation %zu: %d %s\n",i,out.status,out.message);
            bindings[i] = result.binding;
            providers[i] = (ClusterProvider){.document = f.document, .width = (int32_t)(100 + i)};
            if (bindings[i]) {
                os64_dom_set_geometry(bindings[i], cluster_geometry, &providers[i]);
                os64_dom_set_computed(bindings[i], cluster_computed, &providers[i]);
                os64_dom_image_provider_t images = {.opaque = &providers[i], .acquire = cluster_image};
                os64_dom_set_images(bindings[i], &images);
                os64_dom_view_provider_t view = {.opaque = &providers[i], .scroll_element = cluster_scroll};
                os64_dom_set_view(bindings[i], &view);
            }
            if (!result.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
            if (!result.binding) break;
            realm_script(realms[i], STORAGE_ASSERT
                "globalThis.ownDocument=document;globalThis.ownNode=Node;"
                "assert(document instanceof Document);assert(document.body instanceof HTMLElement);"
                "assert(typeof MutationObserver==='function'&&typeof customElements==='object');"
                "assert(document.createElement('canvas').getContext('2d')!==null);"
                "document.body.dataset.owner=location.href;",
                "cluster: realm prototypes and modern interfaces installed");
            if (i == 0) realm_script(realms[0],
                "globalThis.earlyTarget=document.createElement('div');earlyTarget.id='early-effect';"
                "document.body.appendChild(earlyTarget);"
                "globalThis.earlyMotion=earlyTarget.animate({opacity:[0,1]},1000);"
                "earlyMotion.pause();earlyMotion.currentTime=50;",
                "cluster: effect exists before sibling admission");
        }
        for (size_t i = 0; i < 3; ++i) if (bindings[i]) {
            realm_script(realms[i], "assert(document===ownDocument&&Node===ownNode);"
                "assert(document.body.dataset.owner===location.href)", "cluster: later installation preserves earlier realm");
            memory_balanced(bindings[i], "cluster member");
        }
        if (bindings[0] && bindings[1] && bindings[2]) {
            size_t owner_bytes = os64_dom_bytes(bindings[0]), sibling_bytes = os64_dom_bytes(bindings[1]);
            void *record = dom_host_private_alloc(bindings[0], 123);
            check(record && os64_dom_bytes(bindings[0]) > owner_bytes && os64_dom_bytes(bindings[1]) == sibling_bytes,
                  "cluster allocation: retained record charged to creator");
            d_free(bindings[1], record);
            check(os64_dom_bytes(bindings[0]) == owner_bytes && os64_dom_bytes(bindings[1]) == sibling_bytes,
                  "cluster allocation: sibling release restores physical owner's ledger");
            check(os64_js_realm_task_begin(realms[0], OS64_JS_ABI_ID, "trusted cluster exchange", &out) == OS64_JS_OK,
                  "cluster: exchange task opens");
            for (size_t i = 0; i < 3; ++i) {
                JSContext *ctx = os64_js_realm_context(realms[i], OS64_JS_ABI_ID, &out);
                JSValue global = JS_GetGlobalObject(ctx);
                for (size_t j = 0; j < 3; ++j) {
                    JSContext *other = os64_js_realm_context(realms[j], OS64_JS_ABI_ID, &out);
                    JSValue peer = JS_GetGlobalObject(other);
                    check(JS_SetPropertyStr(ctx, global, j == 0 ? "A" : j == 1 ? "B" : "C", peer) >= 0,
                          "cluster: trusted native setup exchanges raw references");
                }
                JS_FreeValue(ctx, global);
            }
            check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "cluster: exchange task closes");
            realm_script(realms[0],
                "globalThis.errorCalls=[];globalThis.errorReturn=true;globalThis.hostCanceled=[];"
                "B.onerror=function(){errorCalls.push([arguments.length,this===B,window.event.target===B,"
                "...arguments]);return errorReturn};"
                "for(const answer of [true,false]){errorReturn=answer;"
                "let token=new C.Error('token'),ev=new C.ErrorEvent('error',{message:'cross',filename:'source.js',"
                "lineno:7,colno:8,error:token,cancelable:true});"
                "assert(B.dispatchEvent(ev)===!answer&&ev.defaultPrevented===answer,'onerror cancellation');"
                "let got=errorCalls.pop();assert(got.length===8&&got[0]===5&&got[1]&&got[2], 'onerror target and arguments');"
                "assert(got[3]==='cross'&&got[4]==='source.js'&&got[5]===7&&got[6]===8&&got[7]===token);"
                "assert(window.event===undefined&&B.event===undefined)}"
                "errorReturn=true;globalThis.hostErrorWatch=e=>hostCanceled.push(e.defaultPrevented);"
                "B.addEventListener('error',hostErrorWatch);",
                "cluster round2: foreign onerror keeps target-window arguments and cancellation");
            const char *foreign_throw = "throw new Error('foreign onerror host')";
            check(os64_js_realm_run(realms[1], foreign_throw, strlen(foreign_throw), "foreign-onerror.js", &out)
                == OS64_JS_EXCEPTION, "cluster round2: host queues B error");
            check(os64_dom_script_ended(bindings[1], "foreign-onerror.js", &out),
                "cluster round2: host delivers B error to A handler");
            realm_script(realms[0],
                "assert(errorCalls.length===1&&errorCalls[0][0]===5&&errorCalls[0][1]&&errorCalls[0][2]);"
                "assert(errorCalls[0][3].includes('foreign onerror host'));assert(hostCanceled.join()==='true');"
                "B.onerror=null;B.removeEventListener('error',hostErrorWatch);"
                "B.document.onerror=function(){errorCalls.push(arguments.length);return false};"
                "let ev=new C.ErrorEvent('error',{cancelable:true});"
                "assert(!B.document.dispatchEvent(ev)&&errorCalls.pop()===1,'Document handler stays ordinary');"
                "B.document.onerror=null;",
                "cluster round2: host onerror is special and Document onerror is ordinary");
            realm_script(realms[0],
                "globalThis.animC=C.document.createElement('div');C.document.body.appendChild(animC);"
                "globalThis.firstAnim=animC.animate({opacity:[0,1]},1000);firstAnim.pause();firstAnim.currentTime=20;"
                "globalThis.animA=document.createElement('div');document.body.appendChild(animA);"
                "globalThis.secondAnim=animA.animate({opacity:[0,1]},1000);"
                "globalThis.animB=B.document.createElement('div');B.document.body.appendChild(animB);"
                "globalThis.thirdAnim=animB.animate({opacity:[0,1]},1000);"
                "B.document.body.appendChild(animC);B.document.body.appendChild(animA);"
                "globalThis.queryAnimations=[firstAnim,secondAnim,thirdAnim];"
                "assert(!C.document.getAnimations().includes(firstAnim),'old document excludes moved animation');"
                "assert(!document.getAnimations().includes(secondAnim),'source excludes moved animation');"
                "let found=B.document.getAnimations().filter(a=>queryAnimations.includes(a));"
                "assert(found.length===3&&found.every((a,i)=>a===queryAnimations[i]),'cluster composite order');"
                "assert(B.Element.prototype.getAnimations.call(animC)[0]===firstAnim,'borrowed element query');"
                "assert(A.Document.prototype.getAnimations.call(B.document).includes(firstAnim),'borrowed document query');"
                "assert(B.document.body.getAnimations({subtree:true}).filter(a=>queryAnimations.includes(a)).length===3);"
                "assert(B.Element.prototype.getAnimations.call(animC,{get subtree(){C.document.body.appendChild(animC);return true}})[0]===firstAnim);"
                "assert(C.document.getAnimations().includes(firstAnim)&&!B.document.getAnimations().includes(firstAnim));"
                "animA.remove();assert(!B.document.getAnimations().includes(secondAnim));"
                "assert(B.Element.prototype.getAnimations.call(animA)[0]===secondAnim,'detached element query');"
                "queryAnimations.forEach(a=>a.cancel());animB.remove();animC.remove();",
                "cluster round2: animation discovery follows current document and global composite order");
            realm_script(realms[0],
                "globalThis.baseTarget=document.createElement('div');baseTarget.setAttribute('data-a','x');"
                "globalThis.baseAttr=baseTarget.getAttributeNode('data-a');B.document.adoptNode(baseTarget);"
                "assert(baseTarget.baseURI==='https://b.example/'&&baseAttr.baseURI===baseTarget.baseURI,'adopted base fallback');"
                "globalThis.relativeBase=B.document.createElement('base');relativeBase.setAttribute('href','assets/#part');"
                "B.document.head.appendChild(relativeBase);"
                "assert(baseTarget.baseURI==='https://b.example/assets/#part'&&baseAttr.baseURI===baseTarget.baseURI,'relative base');"
                "assert(Object.getOwnPropertyDescriptor(Node.prototype,'baseURI').get.call(B.document)===baseTarget.baseURI);"
                "baseTarget.removeAttributeNode(baseAttr);C.document.adoptNode(baseTarget);"
                "assert(baseTarget.baseURI==='https://c.example/'&&baseAttr.baseURI==='https://b.example/assets/#part','detached Attr retains document');"
                "globalThis.baseInert=B.document.implementation.createHTMLDocument();baseInert.adoptNode(baseTarget);"
                "assert(baseTarget.baseURI==='about:blank','windowless fallback');relativeBase.remove();",
                "cluster round3: baseURI follows current document for nodes and retained Attrs");
            realm_script(realms[0],
                "globalThis.commitTarget=document.createElement('div');document.body.appendChild(commitTarget);"
                "globalThis.commitMotion=commitTarget.animate({color:['red','blue']},1000);"
                "commitMotion.pause();commitMotion.currentTime=20;B.document.body.appendChild(commitTarget);"
                "commitMotion.commitStyles();assert(commitTarget.style.color==='green','B computed style committed');"
                "C.document.body.appendChild(commitTarget);B.Animation.prototype.commitStyles.call(commitMotion);"
                "assert(commitTarget.style.color==='blue','C computed style committed');"
                "commitTarget.setAttribute('data-render','contents');throws('InvalidStateError',()=>commitMotion.commitStyles());"
                "commitTarget.removeAttribute('data-render');globalThis.commitParent=C.document.createElement('div');"
                "commitParent.setAttribute('data-render','none');C.document.body.appendChild(commitParent);"
                "commitParent.appendChild(commitTarget);throws('InvalidStateError',()=>commitMotion.commitStyles());"
                "commitTarget.remove();throws('InvalidStateError',()=>commitMotion.commitStyles());"
                "commitMotion.cancel();commitParent.remove();",
                "cluster round3: commitStyles uses destination rendering and computed style");
            realm_script(realms[0],
                "globalThis.movedBody=document.createElement('body');B.document.adoptNode(movedBody);"
                "globalThis.bodyCalls=[];globalThis.bodyCallback=function(e){bodyCalls.push(this===B&&window.event===e)};"
                "movedBody.onload=bodyCallback;assert(movedBody.onload===bodyCallback&&B.onload===bodyCallback&&A.onload===null,'body reflects B');"
                "B.dispatchEvent(new C.Event('load'));assert(bodyCalls.join()==='true','callback keeps A realm');"
                "movedBody.onload=null;assert(B.onload===null);"
                "globalThis.bodyAttrs=[];globalThis.bodyListener=()=>bodyAttrs.push('listener');"
                "globalThis.savedBody=B.document.body;globalThis.attributeBody=document.createElement('body');"
                "try{attributeBody.setAttribute('onresize',{toString(){B.document.documentElement.replaceChild(attributeBody,savedBody);"
                "return 'A.bodyAttrs.push(window===B)'}});"
                "A.EventTarget.prototype.addEventListener.call(B,'resize',bodyListener);B.dispatchEvent(new C.Event('resize'));"
                "assert(bodyAttrs.join()==='true,listener','attribute handler precedes later listener')}"
                "finally{B.document.documentElement.replaceChild(savedBody,attributeBody);B.onresize=null;B.removeEventListener('resize',bodyListener)}"
                "globalThis.movedFrameset=document.createElement('frameset');C.document.adoptNode(movedFrameset);"
                "movedFrameset.onload=bodyCallback;assert(C.onload===bodyCallback&&movedFrameset.onload===bodyCallback,'frameset reflects C');"
                "globalThis.handlerInert=B.document.implementation.createHTMLDocument();handlerInert.adoptNode(movedFrameset);"
                "movedFrameset.onload=()=>{};assert(movedFrameset.onload===null&&C.onload===bodyCallback,'windowless ignores assignment');"
                "C.onload=null;",
                "cluster round3: adopted body and frameset reflect destination window handlers");
            realm_script(realms[0],
                "globalThis.paintCanvas=document.createElement('canvas');globalThis.paint=paintCanvas.getContext('2d');"
                "globalThis.paintPeer=B.document.createElement('canvas').getContext('2d');"
                "globalThis.peerGradient=paintPeer.createLinearGradient(0,0,10,10);peerGradient.addColorStop(0,'red');"
                "paint.fillStyle=peerGradient;paint.strokeStyle=peerGradient;"
                "assert(paint.fillStyle===peerGradient&&paint.strokeStyle===peerGradient,'cross-realm gradient identity');"
                "globalThis.styleGet=Object.getOwnPropertyDescriptor(C.CanvasRenderingContext2D.prototype,'fillStyle').get;"
                "assert(styleGet.call(paint)===peerGradient,'borrowed style getter');"
                "paint.fillStyle.addColorStop(1,'blue');paint.save();paint.fillStyle='green';paint.strokeStyle='red';"
                "paint.restore();C.document.adoptNode(paintCanvas);"
                "assert(paint.fillStyle===peerGradient&&paint.strokeStyle===peerGradient&&styleGet.call(paint)===peerGradient,'saved gradient survives adoption');",
                "cluster round4: gradient getters preserve sibling wrapper identity");
            realm_script(realms[0],
                "globalThis.clickNode=document.createElement('div');globalThis.otherClick=document.createElement('div');"
                "globalThis.clickCount=0;globalThis.otherClicks=0;"
                "otherClick.addEventListener('click',()=>otherClicks++);"
                "clickNode.addEventListener('click',()=>{clickCount++;if(clickCount<3){"
                "B.document.adoptNode(clickNode);clickNode.click();C.document.adoptNode(clickNode);"
                "B.HTMLElement.prototype.click.call(clickNode);otherClick.click()}});"
                "clickNode.click();assert(clickCount===1&&otherClicks===1,'adoption preserves active click guard');"
                "clickNode.click();assert(clickCount===2&&otherClicks===2,'guard clears after dispatch');",
                "cluster round4: recursive click stays guarded across document adoption");
            realm_script(realms[0],
                "globalThis.submitForm=document.createElement('form');document.body.appendChild(submitForm);"
                "globalThis.submitCount=0;submitForm.addEventListener('submit',e=>{e.preventDefault();submitCount++;"
                "if(submitCount<3){B.document.body.appendChild(submitForm);submitForm.requestSubmit();"
                "C.document.body.appendChild(submitForm);B.HTMLFormElement.prototype.requestSubmit.call(submitForm)}});"
                "submitForm.requestSubmit();assert(submitCount===1,'adoption preserves submission guard');"
                "submitForm.requestSubmit();assert(submitCount===2,'submission guard clears');submitForm.remove();",
                "cluster round4: recursive form submission stays guarded across adoption");
            realm_script(realms[0],
                "globalThis.hostForm=document.createElement('form');hostForm.id='round4-host-form';B.document.body.appendChild(hostForm);"
                "globalThis.hostSubmits=0;hostForm.addEventListener('submit',e=>{e.preventDefault();hostSubmits++;"
                "if(hostSubmits<3){C.document.body.appendChild(hostForm);hostForm.requestSubmit()}});"
                "globalThis.invalidForm=document.createElement('form');invalidForm.id='round4-invalid-form';"
                "invalidForm.innerHTML='<input required>';B.document.body.appendChild(invalidForm);"
                "globalThis.invalidCalls=0;invalidForm.firstChild.addEventListener('invalid',()=>{invalidCalls++;"
                "if(invalidCalls<3){C.document.body.appendChild(invalidForm);invalidForm.requestSubmit()}});",
                "cluster round4: prepare host submission and validation adoption");
            os64_dom_event_t submit_event = {.type = "submit", .bubbles = true, .cancelable = true};
            bool submit_prevented = false, form_valid = true, validation_ran = false;
            check(os64_dom_dispatch(bindings[1], native_id(providers[1].document->document, "round4-host-form"),
                &submit_event, &submit_prevented, &out) == OS64_JS_OK && submit_prevented,
                "cluster round4: host submission can adopt and cancel");
            check(os64_dom_form_validate(bindings[1], native_id(providers[1].document->document, "round4-invalid-form"),
                NULL, &form_valid, &validation_ran, &out) == OS64_JS_OK && validation_ran && !form_valid,
                "cluster round4: host validation can adopt invalid form");
            realm_script(realms[0],
                "assert(hostSubmits===1&&invalidCalls===1,'host entry guards survive adoption');"
                "hostForm.requestSubmit();invalidForm.requestSubmit();"
                "assert(hostSubmits===2&&invalidCalls===2,'host entry guards clear');hostForm.remove();invalidForm.remove();",
                "cluster round4: host submission and validation share stable form guard");
            realm_script(realms[0], "B.document.body.id='round4-body';globalThis.borrowedLoads=[];",
                "cluster round4: prepare native window handler fixtures");
            check(os64_html_set_attr(providers[1].document,
                native_id(providers[1].document->document, "round4-body"), "onload",
                "A.borrowedLoads.push(window===B)", strlen("A.borrowedLoads.push(window===B)")) == OS64_HTML_OK,
                "cluster round4: native body load attribute installed without a handler getter");
            realm_script(realms[0],
                "try{A.EventTarget.prototype.dispatchEvent.call(B,new C.Event('load'));"
                "assert(borrowedLoads.join()==='true','borrowed dispatch discovers B body handler: '+borrowedLoads.join());"
                "globalThis.compiledLoad=B.onload;assert(compiledLoad instanceof B.Function,'handler compiles in B');"
                "A.EventTarget.prototype.dispatchEvent.call(B,new C.Event('load'));"
                "assert(borrowedLoads.join()==='true,true'&&B.onload===compiledLoad,'borrowed dispatch preserves compiled slot')}"
                "finally{B.document.body.removeAttribute('onload');B.onload=null}",
                "cluster round4: borrowed window dispatch uses receiver binding");
            check(os64_html_set_attr(providers[1].document,
                native_id(providers[1].document->document, "round4-body"), "onhashchange",
                "A.borrowedLoads.push(window===B)", strlen("A.borrowedLoads.push(window===B)")) == OS64_HTML_OK,
                "cluster round4: native body hashchange attribute installed before borrowed registration");
            realm_script(realms[0],
                "globalThis.borrowedListener=e=>borrowedLoads.push(window.event===e?'listener':'wrong realm');"
                "try{borrowedLoads.length=0;A.EventTarget.prototype.addEventListener.call(B,'hashchange',borrowedListener);"
                "C.EventTarget.prototype.dispatchEvent.call(B,new A.Event('hashchange'));"
                "assert(borrowedLoads.join()==='true,listener','body attribute precedes A callback on B window');"
                "C.EventTarget.prototype.removeEventListener.call(B,'hashchange',borrowedListener);borrowedLoads.length=0;"
                "B.dispatchEvent(new A.Event('hashchange'));assert(borrowedLoads.join()==='true','borrowed removal')}"
                "finally{B.document.body.removeAttribute('onhashchange');B.onhashchange=null;B.removeEventListener('hashchange',borrowedListener)}",
                "cluster round4: borrowed window registration preserves handler order and callback realm");
            realm_script(realms[0], "C.document.body.id='round4-other-body';borrowedLoads.length=0;",
                "cluster round4: prepare untouched window handler slots");
            const char *other_handler = "A.borrowedLoads.push(window===C&&window.event===event)";
            const char *handler_names[] = {"ononline", "onoffline"};
            for (size_t i = 0; i < 2; ++i)
                check(os64_html_set_attr(providers[2].document,
                    native_id(providers[2].document->document, "round4-other-body"), handler_names[i],
                    other_handler, strlen(other_handler)) == OS64_HTML_OK,
                    "cluster round4: untouched native window handler installed");
            realm_script(realms[0],
                "try{A.EventTarget.prototype.dispatchEvent.call(C,new B.Event('online'));"
                "assert(borrowedLoads.join()==='true','untouched body handler discovered');"
                "let p=A;while(p&&!Object.getOwnPropertyDescriptor(p,'onoffline'))p=Object.getPrototypeOf(p);"
                "let get=Object.getOwnPropertyDescriptor(p,'onoffline').get;"
                "assert(get.call(C) instanceof C.Function,'borrowed handler getter compiles in target realm');"
                "A.EventTarget.prototype.dispatchEvent.call(C,new B.Event('offline'));"
                "assert(borrowedLoads.join()==='true,true','compiled handler retains target realm')}"
                "finally{C.document.body.removeAttribute('ononline');C.document.body.removeAttribute('onoffline');"
                "C.ononline=null;C.onoffline=null}",
                "cluster round4: untouched window slots and borrowed handler compilation use target realm");
            realm_script(realms[0],
                "globalThis.scrollMoved=document.createElement('div');B.document.body.appendChild(scrollMoved);"
                "scrollMoved.scroll(1,2);assert(scrollMoved.scrollLeft===1&&scrollMoved.scrollTop===2,'scroll in B');"
                "scrollMoved.scrollTo({left:3,top:4});scrollMoved.scrollBy(5,6);"
                "assert(scrollMoved.scrollLeft===8&&scrollMoved.scrollTop===10,'scrollTo and scrollBy in B');"
                "scrollMoved.scrollLeft=12;scrollMoved.scrollTop=14;"
                "assert(scrollMoved.scrollLeft===12&&scrollMoved.scrollTop===14,'position setters in B');"
                "B.Element.prototype.scrollTo.call(scrollMoved,{get left(){C.document.body.appendChild(scrollMoved);return 21},top:22});"
                "assert(scrollMoved.scrollLeft===21&&scrollMoved.scrollTop===22,'options conversion moves to C');"
                "scrollMoved.scrollTop={valueOf(){B.document.body.appendChild(scrollMoved);return 24}};"
                "assert(scrollMoved.scrollLeft===12&&scrollMoved.scrollTop===24,'setter conversion moves back to B');"
                "scrollMoved.scrollBy({valueOf(){C.document.body.appendChild(scrollMoved);return 2}},3);"
                "assert(scrollMoved.scrollLeft===23&&scrollMoved.scrollTop===25,'numeric conversion moves to C');"
                "globalThis.scrollInert=document.implementation.createHTMLDocument();scrollInert.adoptNode(scrollMoved);scrollMoved.scroll(90,91);",
                "cluster round5: scrolling follows current document after conversion");
            realm_script(realms[0],
                "globalThis.activationLink=document.createElement('a');activationLink.href='next';document.body.appendChild(activationLink);"
                "activationLink.onclick=()=>B.document.body.appendChild(activationLink);activationLink.click();",
                "cluster round5: link moves during click");
            cluster_navigation(bindings, providers, 1, OS64_DOM_NAVIGATE_FOLLOW,
                               "cluster round5: moved link navigates destination");
            realm_script(realms[0],
                "activationLink.onclick=e=>{C.document.body.appendChild(activationLink);e.preventDefault()};activationLink.click();",
                "cluster round5: canceled moved link");
            cluster_navigation(bindings, providers, -1, OS64_DOM_NAVIGATE_NONE,
                               "cluster round5: canceled click does not navigate");
            realm_script(realms[0],
                "activationLink.onclick=()=>B.document.body.appendChild(activationLink);"
                "C.EventTarget.prototype.dispatchEvent.call(activationLink,new A.MouseEvent('click',{bubbles:true,cancelable:true}));",
                "cluster round5: script-dispatched click uses post-listener document");
            cluster_navigation(bindings, providers, 1, OS64_DOM_NAVIGATE_FOLLOW,
                               "cluster round5: dispatched click navigates destination");
            realm_script(realms[0],
                "activationLink.onclick=()=>scrollInert.adoptNode(activationLink);activationLink.click();",
                "cluster round5: click moves link into windowless document");
            cluster_navigation(bindings, providers, -1, OS64_DOM_NAVIGATE_NONE,
                               "cluster round5: windowless activation cannot navigate");
            realm_script(realms[0],
                "globalThis.activationForm=B.document.createElement('form');B.document.body.appendChild(activationForm);"
                "globalThis.activationButton=document.createElement('button');document.body.appendChild(activationButton);"
                "globalThis.activationSubmits=0;activationForm.onsubmit=()=>activationSubmits++;"
                "activationButton.onclick=()=>activationForm.appendChild(activationButton);activationButton.click();"
                "assert(activationSubmits===1,'moved button submits destination form');",
                "cluster round5: submit button moves during click");
            cluster_navigation(bindings, providers, 1, OS64_DOM_NAVIGATE_SUBMIT,
                               "cluster round5: moved button requests destination submission");
            realm_script(realms[0],
                "activationForm.onsubmit=()=>C.document.body.appendChild(activationForm);activationForm.requestSubmit();",
                "cluster round5: form moves during submit listener");
            cluster_navigation(bindings, providers, 2, OS64_DOM_NAVIGATE_SUBMIT,
                               "cluster round5: submission follows post-listener document");
            realm_script(realms[0],
                "activationForm.onsubmit=()=>activationForm.remove();activationForm.requestSubmit();",
                "cluster round5: submit listener disconnects form");
            cluster_navigation(bindings, providers, -1, OS64_DOM_NAVIGATE_NONE,
                               "cluster round5: disconnected form cannot navigate after submit listener");
            unsigned resets_before = providers[1].resets;
            realm_script(realms[0],
                "activationForm.onreset=()=>B.document.body.appendChild(activationForm);activationForm.reset();",
                "cluster round5: form moves during reset listener");
            check(providers[1].resets == resets_before + 1, "cluster round5: reset notifies post-listener provider");
            realm_script(realms[0],
                "activationForm.remove();activationLink.remove();"
                "globalThis.replacementTarget=C.document.createElement('div');replacementTarget.id='round5-replacement';C.document.body.appendChild(replacementTarget);"
                "globalThis.makeFinished=(w,target,props)=>{let a=new w.Animation(new w.KeyframeEffect(target,props,{duration:10,fill:'forwards'}));a.finish();return a};"
                "globalThis.replacedOld=makeFinished(C,replacementTarget,{opacity:[0,1],color:['red','blue']});"
                "B.document.body.appendChild(replacementTarget);"
                "globalThis.replacedOpacity=makeFinished(A,replacementTarget,{opacity:[1,0]});"
                "globalThis.replacedColor=makeFinished(B,replacementTarget,{color:['blue','red']});"
                "globalThis.replacementEvents=[];replacedOld.onremove=e=>replacementEvents.push(e instanceof C.AnimationPlaybackEvent&&window.event===e);"
                "globalThis.persistedOld=makeFinished(C,replacementTarget,{left:['0px','1px']});persistedOld.persist();"
                "globalThis.persistedLater=makeFinished(A,replacementTarget,{left:['1px','2px']});"
                "globalThis.detachedTarget=document.createElement('div');"
                "globalThis.detachedOld=makeFinished(A,detachedTarget,{opacity:[0,1]});"
                "globalThis.detachedLater=makeFinished(B,detachedTarget,{opacity:[1,0]});",
                "cluster round5: cross-binding replacement uses composite order and property union");
            for (size_t step = 0; step < 2; ++step) {
                p2b_advance(20);
                const size_t order[] = {1, 2, 0};
                for (size_t j = 0; j < 3; ++j) {
                    size_t i = order[j];
                    if (os64_dom_web_animations_next(bindings[i]) <= p2b_ms)
                        check(os64_dom_web_animations_update(bindings[i], p2b_ms, &out) && out.status == OS64_JS_OK,
                              "cluster round5: replacement and removal events update in their owning bindings");
                }
            }
            const struct garb_effect *const *replacement_effects = NULL;
            check(os64_dom_script_effects(bindings[1], native_id(providers[1].document->document, "round5-replacement"),
                &replacement_effects) == 4, "cluster round5: layout omits removed cross-binding effect");
            realm_script(realms[0],
                "assert(replacedOld.replaceState==='removed'&&replacementEvents.join()==='true','cross-binding removal event');"
                "assert(replacedOpacity.replaceState==='active'&&replacedColor.replaceState==='active','later partial effects retained');"
                "assert(persistedOld.replaceState==='persisted'&&persistedLater.replaceState==='active','persisted effect retained');"
                "assert(detachedOld.replaceState==='active'&&detachedLater.replaceState==='active','detached effects not replaced');"
                "assert(!B.document.getAnimations().includes(replacedOld)&&replacementTarget.getAnimations().includes(replacedColor),'discovery excludes removed effect');"
                "for(let a of [replacedOld,replacedOpacity,replacedColor,persistedOld,persistedLater,detachedOld,detachedLater])a.cancel();"
                "replacementTarget.remove();",
                "cluster round5: replacement removes only covered active effects and preserves callback realm");
            realm_script(realms[0],
                "globalThis.captureNode=document.createElement('div');captureNode.id='round6-capture';B.document.body.appendChild(captureNode);"
                "globalThis.captureHit=B.document.createElement('div');captureHit.id='round6-hit';B.document.body.appendChild(captureHit);"
                "globalThis.captureOther=C.document.createElement('div');captureOther.id='round6-other';C.document.body.appendChild(captureOther);"
                "globalThis.captureEvents=[];captureNode.onpointerdown=e=>{captureNode.setPointerCapture(e.pointerId);"
                "captureEvents.push(captureNode.hasPointerCapture(e.pointerId)&&window.event===e)};"
                "captureNode.ongotpointercapture=e=>captureEvents.push('got');captureNode.onlostpointercapture=e=>captureEvents.push('lost');",
                "cluster round6: adopted pointer capture fixture");
            cluster_pointer(bindings[1], providers[1].document, "round6-capture", OS64_DOM_POINTER_DOWN, 0, 1);
            cluster_pointer(bindings[1], providers[1].document, "round6-hit", OS64_DOM_POINTER_MOVE, -1, 1);
            realm_script(realms[0],
                "assert(captureEvents.join()==='true,got','destination capture and callback realm');"
                "C.Element.prototype.releasePointerCapture.call(captureNode,1);assert(!captureNode.hasPointerCapture(1),'borrowed release');",
                "cluster round6: capture methods use destination pointer state");
            cluster_pointer(bindings[1], providers[1].document, "round6-hit", OS64_DOM_POINTER_MOVE, -1, 1);
            cluster_pointer(bindings[2], providers[2].document, "round6-other", OS64_DOM_POINTER_DOWN, 0, 1);
            realm_script(realms[0],
                "assert(captureEvents.join()==='true,got,lost','capture released');"
                "captureNode.setPointerCapture({valueOf(){C.document.body.appendChild(captureNode);return 1}});"
                "assert(B.Element.prototype.hasPointerCapture.call(captureNode,1),'capture follows ID conversion');"
                "assert(captureNode.hasPointerCapture({valueOf(){B.document.body.appendChild(captureNode);captureNode.setPointerCapture(1);return 1}}),'has follows ID conversion');"
                "captureNode.releasePointerCapture({valueOf(){C.document.body.appendChild(captureNode);captureNode.setPointerCapture(1);return 1}});"
                "assert(!captureNode.hasPointerCapture(1),'release follows ID conversion');"
                "throws('NotFoundError',()=>captureNode.setPointerCapture(7));captureNode.remove();"
                "throws('InvalidStateError',()=>captureNode.setPointerCapture(1));",
                "cluster round6: pointer ID conversions re-resolve adoption");
            cluster_pointer(bindings[1], providers[1].document, "round6-hit", OS64_DOM_POINTER_UP, 0, 0);
            cluster_pointer(bindings[2], providers[2].document, "round6-other", OS64_DOM_POINTER_UP, 0, 0);
            realm_script(realms[0],
                "globalThis.attrSource=document.createAttributeNS('urn:round6','p:test');attrSource.value='kept';"
                "globalThis.attrImported=B.document.importNode(attrSource);"
                "assert(attrImported instanceof B.Attr&&!(attrImported instanceof A.Attr)&&attrImported.ownerDocument===B.document,'import uses B prototype');"
                "assert(attrImported!==attrSource&&attrImported.name==='p:test'&&attrImported.namespaceURI==='urn:round6'&&attrImported.value==='kept');"
                "B.document.adoptNode(attrSource);globalThis.attrCopied=attrSource.cloneNode();"
                "assert(attrSource instanceof A.Attr&&attrCopied instanceof B.Attr&&attrCopied.ownerDocument===B.document,'adopted detached clone');"
                "globalThis.attrElement=document.createElement('div');attrElement.setAttribute('data-clone','value');"
                "globalThis.attachedSource=attrElement.getAttributeNode('data-clone');C.document.adoptNode(attrElement);"
                "globalThis.attachedCopy=B.Attr.prototype.cloneNode.call(attachedSource);"
                "assert(attachedCopy instanceof C.Attr&&attachedCopy.ownerDocument===C.document&&attachedCopy.ownerElement===null&&attachedCopy.value==='value','attached clone uses C');"
                "globalThis.attrInert=B.document.implementation.createHTMLDocument();"
                "assert(attrInert.importNode(attachedSource) instanceof B.Attr,'windowless destination binding');",
                "cluster round6: cloned Attr ownership and prototypes follow destination");
            realm_script(realms[0],
                "globalThis.borrowedAttr=A.Document.prototype.createAttribute.call(C.document,'DATA-X');"
                "assert(borrowedAttr instanceof C.Attr&&borrowedAttr.ownerDocument===C.document&&borrowedAttr.name==='data-x','borrowed Attr factory');"
                "globalThis.borrowedNS=B.Document.prototype.createAttributeNS.call(C.document,'urn:round6','p:y');"
                "assert(borrowedNS instanceof C.Attr&&borrowedNS.prefix==='p','borrowed namespace factory');"
                "globalThis.viewElement=C.document.createElement('div');viewElement.setAttribute('data-view','x');B.document.adoptNode(viewElement);"
                "globalThis.viewAttr=A.Element.prototype.getAttributeNode.call(viewElement,'data-view');"
                "assert(viewAttr instanceof C.Attr&&viewAttr.ownerDocument===B.document,'cached Attr view keeps element creation realm');"
                "globalThis.viewMap=Object.getOwnPropertyDescriptor(A.Element.prototype,'attributes').get.call(viewElement);"
                "assert(viewMap instanceof C.NamedNodeMap&&viewMap[0]===viewAttr,'cached map keeps creation realm');"
                "globalThis.attrChildren=Object.getOwnPropertyDescriptor(A.Attr.prototype,'childNodes').get.call(borrowedAttr);"
                "assert(attrChildren instanceof C.NodeList,'Attr child list keeps creation realm');",
                "cluster round6: sibling attribute factories and cached views select creation realm");
            unsigned reports_b = providers[1].reports, reports_c = providers[2].reports;
            realm_script(realms[0],
                "globalThis.reportMoved=document.createElement('input');reportMoved.id='round7-report';reportMoved.required=true;document.body.appendChild(reportMoved);"
                "reportMoved.oninvalid=()=>B.document.body.appendChild(reportMoved);assert(!reportMoved.reportValidity());"
                "globalThis.reportForm=document.createElement('form');document.body.appendChild(reportForm);"
                "globalThis.reportControl=document.createElement('input');reportControl.id='round7-form-report';reportControl.required=true;reportForm.appendChild(reportControl);"
                "reportControl.oninvalid=()=>C.document.body.appendChild(reportControl);assert(!reportForm.reportValidity());",
                "cluster round7: invalid listeners move control and form-validation targets");
            check(providers[1].reports == reports_b + 1 && providers[2].reports == reports_c + 1,
                  "cluster round7: reporting notifies destination providers");
            char report_text[256];
            check(os64_dom_report_message(bindings[1], native_id(providers[1].document->document, "round7-report"),
                report_text, sizeof(report_text)) && report_text[0], "cluster round7: B owns validation message");
            check(os64_dom_report_message(bindings[2], native_id(providers[2].document->document, "round7-form-report"),
                report_text, sizeof(report_text)) && report_text[0], "cluster round7: C owns form-validation message");
            realm_script(realms[0],
                "globalThis.selectionMoved=document.createElement('textarea');selectionMoved.value='abcdef';B.document.body.appendChild(selectionMoved);"
                "globalThis.selectionEvents=[];globalThis.selectionEventClass=B.Event;selectionMoved.addEventListener('select',e=>selectionEvents.push(window.event===e&&e instanceof selectionEventClass));"
                "globalThis.fullTimers=[];try{for(let i=0;i<4096;i++)fullTimers.push(setTimeout(()=>{},100000));"
                "selectionMoved.setSelectionRange(1,3)}finally{fullTimers.forEach(clearTimeout)}",
                "cluster round7: selection queues in B despite exhausted A timer quota");
            check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                  "cluster round7: B delivers selection task");
            realm_script(realms[0],
                "assert(selectionEvents.join()==='true','B selection event and A callback');selectionEvents=[];"
                "selectionEventClass=C.Event;"
                "selectionMoved.setSelectionRange({valueOf(){C.document.body.appendChild(selectionMoved);return 2}},4);",
                "cluster round7: selection conversion adopts into C");
            check(os64_dom_timer_fire(bindings[2], p2b_ms, &out) && out.status == OS64_JS_OK,
                  "cluster round7: C delivers post-conversion selection task");
            realm_script(realms[0],
                "assert(selectionEvents.join()==='true','C selection event');"
                "selectionEvents=[];B.HTMLTextAreaElement.prototype.setRangeText.call(selectionMoved,'x',0,1,'select');",
                "cluster round7: borrowed range-text setter uses destination queue");
            check(os64_dom_timer_fire(bindings[2], p2b_ms, &out) && out.status == OS64_JS_OK,
                  "cluster round7: C delivers range-text selection task");
            realm_script(realms[0], "assert(selectionEvents.join()==='true','range-text select event');",
                "cluster round7: range-text event delivered");
            realm_script(realms[0],
                "globalThis.borrowedResize=new B.ResizeObserver(()=>{});"
                "A.ResizeObserver.prototype.observe.call(borrowedResize,B.document.body);",
                "cluster round7: borrowed ResizeObserver observe");
            check(os64_dom_resizes_observed(bindings[0]) == 0 && os64_dom_resizes_observed(bindings[1]) == 1,
                  "cluster round7: resize registry belongs to observer B");
            realm_script(realms[0],
                "borrowedResize.disconnect();C.ResizeObserver.prototype.observe.call(borrowedResize,C.document.body);"
                "A.ResizeObserver.prototype.unobserve.call(borrowedResize,C.document.body);"
                "A.ResizeObserver.prototype.observe.call(borrowedResize,B.document.body);"
                "C.ResizeObserver.prototype.disconnect.call(borrowedResize);"
                "globalThis.borrowedIntersection=new C.IntersectionObserver(()=>{});"
                "A.IntersectionObserver.prototype.observe.call(borrowedIntersection,C.document.body);",
                "cluster round7: borrowed observer methods share their record owner's registries");
            check(os64_dom_resizes_observed(bindings[0]) == 0 && os64_dom_resizes_observed(bindings[1]) == 0 &&
                os64_dom_resizes_observed(bindings[2]) == 0, "cluster round7: resize counters drain without underflow");
            check(os64_dom_intersections_observed(bindings[0]) == 0 && os64_dom_intersections_observed(bindings[2]) == 1,
                  "cluster round7: intersection registry belongs to observer C");
            realm_script(realms[0],
                "B.IntersectionObserver.prototype.takeRecords.call(borrowedIntersection);"
                "borrowedIntersection.disconnect();A.IntersectionObserver.prototype.observe.call(borrowedIntersection,B.document.body);"
                "B.IntersectionObserver.prototype.unobserve.call(borrowedIntersection,B.document.body);"
                "B.IntersectionObserver.prototype.observe.call(borrowedIntersection,C.document.body);"
                "A.IntersectionObserver.prototype.disconnect.call(borrowedIntersection);",
                "cluster round7: borrowed intersection methods drain owner registry");
            check(os64_dom_intersections_observed(bindings[0]) == 0 && os64_dom_intersections_observed(bindings[1]) == 0 &&
                os64_dom_intersections_observed(bindings[2]) == 0, "cluster round7: intersection counters drain without underflow");
            realm_script(realms[0],
                "globalThis.borrowedPerformance=new B.PerformanceObserver(()=>{});"
                "A.PerformanceObserver.prototype.observe.call(borrowedPerformance,{entryTypes:['mark']});"
                "A.performance.mark('round7-A');B.performance.mark('round7-B');"
                "assert(C.PerformanceObserver.prototype.takeRecords.call(borrowedPerformance).map(e=>e.name).join()==='round7-B','performance observer uses owner timeline');"
                "C.PerformanceObserver.prototype.disconnect.call(borrowedPerformance);"
                "B.performance.mark('round7-after-disconnect');assert(borrowedPerformance.takeRecords().length===0);",
                "cluster round7: borrowed performance observer methods use owner registry");
            check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                  "cluster round7: drain disconnected performance task");
            realm_script(realms[0],
                "globalThis.performanceDelivered=false;"
                "globalThis.bufferedPerformance=new B.PerformanceObserver(list=>{performanceDelivered=list instanceof B.PerformanceObserverEntryList&&list.getEntries().some(e=>e.name==='round7-B')});"
                "A.PerformanceObserver.prototype.observe.call(bufferedPerformance,{type:'mark',buffered:true});",
                "cluster round7: borrowed buffered observe queues owner task");
            check(os64_dom_timer_fire(bindings[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                  "cluster round7: B delivers buffered performance task");
            realm_script(realms[0], "assert(performanceDelivered,'buffered delivery uses owner list prototype');bufferedPerformance.disconnect();",
                "cluster round7: performance delivery keeps owner realm");
            realm_script(realms[0],
                "globalThis.viewLink=document.createElement('link');viewLink.id='round8-link';B.document.body.appendChild(viewLink);"
                "globalThis.viewOutput=document.createElement('output');viewOutput.id='round8-output';B.document.body.appendChild(viewOutput);",
                "cluster round8: move elements before cached view allocation");
            realm_script(realms[1],
                "globalThis.tokenView=document.getElementById('round8-link').classList;"
                "globalThis.relView=A.viewLink.relList;globalThis.dataView=A.viewLink.dataset;globalThis.forView=A.viewOutput.htmlFor;"
                "assert(tokenView instanceof A.DOMTokenList,'class token creation realm');assert(relView instanceof A.DOMTokenList,'rel token creation realm');assert(forView instanceof A.DOMTokenList,'for token creation realm');"
                "assert(dataView instanceof A.DOMStringMap,'dataset creation realm');"
                "tokenView.add('b');relView.add('next');dataView.route='b';forView.add('field');",
                "cluster round8: token and dataset views keep element creation realm");
            realm_script(realms[0],
                "C.document.body.appendChild(viewLink);C.document.body.appendChild(viewOutput);"
                "assert(viewLink.classList===B.tokenView&&viewLink.relList===B.relView&&viewLink.dataset===B.dataView&&viewOutput.htmlFor===B.forView);"
                "B.tokenView.add('c');B.dataView.route='c';assert(viewLink.className==='b c'&&viewLink.getAttribute('data-route')==='c');"
                "globalThis.freshView=document.createElement('div');C.document.body.appendChild(freshView);"
                "assert(Object.getOwnPropertyDescriptor(C.Element.prototype,'classList').get.call(freshView) instanceof A.DOMTokenList,'borrowed first getter');",
                "cluster round8: cached views keep identity and mutate current document");
            realm_script(realms[0],
                "globalThis.foreignSheet=new CSSStyleSheet();globalThis.ownSheet=new B.CSSStyleSheet();ownSheet.replaceSync('p{}');"
                "globalThis.bSheets=Object.getOwnPropertyDescriptor(A.Document.prototype,'adoptedStyleSheets').get.call(B.document);"
                "assert(bSheets instanceof B.Array,'adopted array creation realm');"
                "B.document.adoptedStyleSheets=[ownSheet];"
                "for(const attempt of [()=>B.document.adoptedStyleSheets=[foreignSheet],()=>bSheets.push(foreignSheet),()=>Object.defineProperty(bSheets,'0',{value:foreignSheet})]){"
                "let refused=false;try{attempt()}catch(e){refused=e.name==='NotAllowedError'}assert(refused,'foreign constructor document refused');assert(bSheets.length===1&&bSheets[0]===ownSheet)}",
                "cluster round8: constructed sheets cannot cross documents through array mutation");
            uint64_t sheet_version = os64_html_version(providers[1].document);
            size_t sheet_bytes = os64_dom_bytes(bindings[1]);
            realm_script(realms[0], "A.CSSStyleSheet.prototype.replaceSync.call(ownSheet,'q'.repeat(4096));",
                "cluster round8: borrowed replacement uses sheet owner");
            check(os64_html_version(providers[1].document) > sheet_version,
                  "cluster round8: borrowed sheet replacement invalidates destination");
            check(os64_dom_bytes(bindings[1]) > sheet_bytes + 4000,
                  "cluster round8: borrowed sheet text is charged to constructor binding");
            realm_script(realms[0],
                "globalThis.sheetHost=document.createElement('div');globalThis.sheetRoot=sheetHost.attachShadow({mode:'open'});"
                "B.document.body.appendChild(sheetHost);sheetRoot.adoptedStyleSheets=[ownSheet];B.document.adoptedStyleSheets=[];",
                "cluster round8: moved shadow root accepts its current document's sheet");
            sheet_version = os64_html_version(providers[1].document);
            realm_script(realms[0], "ownSheet.replaceSync('span{}');",
                "cluster round8: sheet replacement finds moved shadow root");
            check(os64_html_version(providers[1].document) > sheet_version,
                  "cluster round8: moved shadow adoption invalidates current document");
            sheet_version = os64_html_version(providers[1].document);
            realm_script(realms[0], "sheetRoot.adoptedStyleSheets=[];",
                "cluster round8: clear adopted sheets");
            check(os64_html_version(providers[1].document) > sheet_version,
                  "cluster round8: moved adoption array invalidates current document");
            for (size_t i = 1; i < 3; i++) os64_dom_set_resizes(bindings[i], cluster_resizes, &providers[i]);
            realm_script(realms[0],
                "globalThis.resizeFirst=document.createElement('div');globalThis.resizeSecond=document.createElement('div');globalThis.resizeThird=document.createElement('div');"
                "B.document.body.appendChild(resizeFirst);C.document.body.appendChild(resizeSecond);B.document.body.appendChild(resizeThird);"
                "globalThis.resizeRows=[];globalThis.routedResize=new ResizeObserver(es=>{assert(es.every(e=>e instanceof A.ResizeObserverEntry));resizeRows.push(es.map(e=>e.contentRect.width).join(','))});"
                "routedResize.observe(resizeFirst);routedResize.observe(resizeSecond);routedResize.observe(resizeThird);",
                "cluster round8: one observer measures targets in two sibling documents");
            check(os64_dom_resizes_step(bindings[0], &out) && out.status == OS64_JS_OK,
                  "cluster round8: owner without provider delivers sibling measurements");
            realm_script(realms[0], "assert(resizeRows.join()==='101,102,101','sibling provider sizes in observer order');resizeRows=[];",
                "cluster round8: resize callback retains owner realm and target order");
            check(providers[1].resize_calls == 2 && providers[2].resize_calls == 2,
                  "cluster round8: one provider batch per document and gather");
            os64_dom_set_resizes(bindings[2], NULL, NULL);
            check(!os64_dom_resizes_step(bindings[0], &out) && os64_dom_resizes_owed(bindings[0]),
                  "cluster round8: missing destination provider leaves observation owed");
            os64_dom_set_resizes(bindings[2], cluster_resizes, &providers[2]);
            providers[2].resize_refuse = true;
            providers[1].width = 104;
            check(!os64_dom_resizes_step(bindings[0], &out) && os64_dom_resizes_owed(bindings[0]),
                  "cluster round8: refused destination leaves observation owed");
            realm_script(realms[0], "assert(resizeRows.length===0,'no partial measurement delivery');",
                "cluster round8: destination refusal publishes no partial batch");
            providers[2].resize_refuse = false;
            check(os64_dom_resizes_step(bindings[0], &out) && out.status == OS64_JS_OK,
                  "cluster round8: destination recovers and delivers changed size");
            realm_script(realms[0], "assert(resizeRows.join()==='104,104');resizeRows=[];document.body.appendChild(resizeFirst);",
                "cluster round8: resize target returns to observer document");
            os64_dom_set_resizes(bindings[0], cluster_resizes, &providers[0]);
            check(os64_dom_resizes_step(bindings[0], &out) && out.status == OS64_JS_OK,
                  "cluster round8: returned target uses original provider");
            realm_script(realms[0], "assert(resizeRows.join()==='100');routedResize.disconnect();resizeFirst.remove();resizeSecond.remove();resizeThird.remove();",
                "cluster round8: resize routing follows subsequent adoption");
            providers[1].width = 101;
            realm_script(realms[0],
                "globalThis.filler=document.createElement('select');globalThis.retainedOptions=filler.options;"
                "B.document.adoptNode(filler);filler.length=1;"
                "assert(retainedOptions[0] instanceof B.HTMLOptionElement,'select.length uses B');"
                "retainedOptions.length=2;assert(retainedOptions[1] instanceof B.HTMLOptionElement,'options.length uses B');"
                "retainedOptions[4]=document.createElement('option');"
                "assert(retainedOptions[2] instanceof B.HTMLOptionElement&&retainedOptions[3] instanceof B.HTMLOptionElement,'indexed fillers use B');"
                "filler.length={valueOf(){C.document.adoptNode(filler);return 6}};"
                "assert(retainedOptions[5] instanceof C.HTMLOptionElement,'length conversion can adopt');",
                "cluster review: filler options follow current Document after conversion");
            realm_script(realms[0],
                "globalThis.classRoot=C.document.createElement('div');classRoot.innerHTML='<i class=foo></i>';"
                "globalThis.classes=classRoot.getElementsByClassName('FOO');assert(classes.length===1,'quirks match');"
                "B.document.adoptNode(classRoot);assert(classes.length===0,'standards mismatch');"
                "C.document.adoptNode(classRoot);assert(classes.length===1,'quirks match restored');",
                "cluster review: retained class query follows current Document mode");
            cluster_started = 0;
            realm_script(realms[0],
                "globalThis.startedScript=document.createElement('script');startedScript.textContent='already started';"
                "document.body.appendChild(startedScript);",
                "cluster review: initial script handed to source provider");
            check(cluster_started == 1, "cluster review: script starts once");
            realm_script(realms[0],
                "B.document.body.appendChild(startedScript);C.document.body.appendChild(startedScript);"
                "B.document.body.appendChild(B.document.importNode(startedScript,true));"
                "globalThis.inertScripts=document.createElement('div');inertScripts.innerHTML='<script>inert</script>';"
                "B.document.body.appendChild(B.document.importNode(inertScripts,true));"
                "C.document.body.appendChild(inertScripts);"
                "B.document.body.appendChild(inertScripts.cloneNode(true));",
                "cluster review: moved and imported started or inert scripts remain started");
            check(cluster_started == 1, "cluster review: adoption and import do not restart scripts");
            realm_script(realms[0],
                "globalThis.foreignEventSeen=[];globalThis.foreignCallback=e=>foreignEventSeen.push(window.event===e);"
                "B.document.body.addEventListener('foreign-realm',foreignCallback);"
                "B.document.body.dispatchEvent(new C.Event('foreign-realm'));"
                "assert(foreignEventSeen.join()==='true','listener callback realm');"
                "B.document.body.onclick=foreignCallback;B.document.body.dispatchEvent(new C.Event('click'));"
                "assert(foreignEventSeen.join()==='true,true','handler callback realm');"
                "B.document.body.onclick=null;"
                "B.document.body.id='review-event-target';"
                "globalThis.boundForeign=foreignCallback.bind(null);globalThis.proxyForeign=new Proxy(foreignCallback,{});"
                "Object.setPrototypeOf(foreignCallback,B.Function.prototype);"
                "B.document.body.addEventListener('bound-realm',boundForeign);"
                "B.document.body.addEventListener('proxy-realm',proxyForeign);"
                "B.document.body.dispatchEvent(new C.Event('bound-realm'));"
                "B.document.body.dispatchEvent(new C.Event('proxy-realm'));"
                "globalThis.foreignAbort=new B.AbortController();foreignAbort.signal.onabort=foreignCallback;foreignAbort.abort();"
                "assert(foreignEventSeen.length===5&&foreignEventSeen.every(Boolean),'bound/proxy/named handler realm');"
                "assert(window.event===undefined&&B.event===undefined);",
                "cluster review: foreign method and handler retain function realm");
            os64_dom_event_t foreign_event = {.type = "foreign-realm"};
            bool foreign_prevented = false;
            check(os64_dom_dispatch(bindings[1], native_id(providers[1].document->document, "review-event-target"),
                &foreign_event, &foreign_prevented, &out) == OS64_JS_OK,
                "cluster review: host dispatch enters foreign callback realm");
            realm_script(realms[0],
                "assert(foreignEventSeen.length===6&&foreignEventSeen.every(Boolean));"
                "assert(window.event===undefined&&B.event===undefined);",
                "cluster review: host callback exposes and restores its own window.event");
            realm_script(realms[0],
                "globalThis.callbackErrors=[];"
                "globalThis.captureCallbackError=e=>{callbackErrors.push('A');e.preventDefault()};"
                "window.addEventListener('error',captureCallbackError);"
                "B.addEventListener('error',e=>{callbackErrors.push('B');e.preventDefault()});"
                "B.document.body.addEventListener('throw-realm',()=>{throw Error('foreign callback')});"
                "B.document.body.dispatchEvent(new C.Event('throw-realm'));",
                "cluster review: cross-realm callback exception dispatched");
            realm_script(realms[0],
                "assert(callbackErrors.join()==='A','callback errors belong to A: '+callbackErrors);"
                "window.removeEventListener('error',captureCallbackError);",
                "cluster review: exception reaches callback realm rather than method realm");
            realm_script(realms[0], "B.document.body.appendChild(earlyTarget);",
                         "cluster: existing effect moves into a later-admitted binding");
            const struct garb_effect *const *early_effects = NULL;
            check(os64_dom_script_effects(bindings[1],
                native_id(providers[1].document->document, "early-effect"), &early_effects) == 1,
                "cluster: new member discovers effects published before it joined");
            realm_script(realms[0], "earlyMotion.cancel();earlyTarget.remove();",
                         "cluster: early effect fixture released");
            realm_script(realms[0],
                dom_realm_adoption_oracle,
                "cluster: three-realm creation metadata, shared identity, borrowed factory and import");
            realm_script(realms[0],
                "globalThis.host=document.createElement('div');B.document.adoptNode(host);"
                "globalThis.shadow=host.attachShadow({mode:'open'});shadow.innerHTML='<b>shadow B</b>';"
                "C.document.adoptNode(host);assert(shadow instanceof B.ShadowRoot&&shadow.ownerDocument===C.document);"
                "assert(shadow.firstChild instanceof B.HTMLElement&&shadow.firstChild.ownerDocument===C.document);"
                "globalThis.template=document.createElement('template');template.innerHTML='<i>template A</i>';"
                "B.document.adoptNode(template);assert(template.content.ownerDocument!==B.document);"
                "assert(template.content.firstChild instanceof A.HTMLElement);"
                "globalThis.inert=template.content.ownerDocument;B.document.body.appendChild(template.content);"
                "assert(template.content.ownerDocument===inert&&template.content.childNodes.length===0);"
                "C.document.adoptNode(template.content);assert(template.content.ownerDocument===C.document);"
                "globalThis.xml=B.document.implementation.createDocument(null,'root');"
                "globalThis.pi=A.Document.prototype.createProcessingInstruction.call(xml,'test','data');"
                "assert(pi instanceof B.ProcessingInstruction&&pi.ownerDocument===xml);",
                "cluster: shadow/template creation metadata, fragment insertion and borrowed XML factory");
            realm_script(realms[1],
                "A.delivery=[];globalThis.mo=new MutationObserver(function(records){"
                "assert(this===mo);assert(records[0] instanceof MutationRecord);"
                "A.delivery.push('B:'+records.map(r=>r.type).join('/'))});"
                "A.MutationObserver.prototype.observe.call(mo,A.container,{attributes:true,childList:true});",
                "cluster: observer constructed in B registers through borrowed A method");
            realm_script(realms[0],
                "globalThis.mo=new MutationObserver(function(records){assert(records[0] instanceof MutationRecord);"
                "delivery.push('A:'+records.map(r=>r.type).join('/'))});"
                "mo.observe(container,{attributes:true,childList:true});"
                "B.document.adoptNode(container);B.document.body.appendChild(container);"
                "container.setAttribute('data-moved','yes');",
                "cluster: observers follow a target into another document");
            realm_script(realms[2],
                "assert(A.delivery.join(',')==='B:attributes,A:attributes',A.delivery.join(','));"
                "B.mo.disconnect();A.mo.disconnect();",
                "cluster: shared observer order follows creation, callbacks retain their realms");
            realm_script(realms[0],
                "globalThis.ceLog=[];globalThis.X=class extends HTMLElement {"
                "static get observedAttributes(){return ['data-x']}"
                "connectedCallback(){ceLog.push('A:connected')}"
                "disconnectedCallback(){ceLog.push('A:disconnected')}"
                "adoptedCallback(from,to){assert(this instanceof X);ceLog.push('A:adopt:'+from.URL+'>'+to.URL)}"
                "attributeChangedCallback(){ceLog.push('A:attribute')} };"
                "customElements.define('x-shared',X);globalThis.ce=document.createElement('x-shared');"
                "document.body.appendChild(ce);ceLog.length=0;",
                "cluster: source custom definition and stable element");
            realm_script(realms[1],
                "customElements.define('x-shared',class extends HTMLElement {"
                "connectedCallback(){A.ceLog.push('B:wrong-definition')}});"
                "document.body.appendChild(A.ce);"
                "assert(A.ceLog.join('|')==='A:disconnected|A:adopt:https://a.example/>https://b.example/|A:connected',A.ceLog.join('|'));"
                "A.ceLog.length=0;A.ce.setAttribute('data-x','kept');"
                "assert(A.ceLog.join('|')==='A:attribute',A.ceLog.join('|'));",
                "cluster: shared reactions preserve source definition and disconnect-adopt-connect order");
            realm_script(realms[2],
                "A.ceLog.length=0;document.adoptNode(A.ce);"
                "assert(A.ceLog.join('|')==='A:disconnected|A:adopt:https://b.example/>https://c.example/',A.ceLog.join('|'));"
                "document.body.appendChild(A.ce);assert(A.ce instanceof A.X&&A.ce.ownerDocument===document);"
                "assert(A.ceLog[A.ceLog.length-1]==='A:connected');",
                "cluster: explicit adoption uses the same reaction stack across all three bindings");
            realm_script(realms[0],
                "globalThis.nestedLog=[];class Nested extends HTMLElement {"
                "adoptedCallback(from,to){nestedLog.push(to===B.document?'B':'C');"
                "if(to===B.document)C.document.adoptNode(this);else throw Error('expected nested adoption throw')}}"
                "customElements.define('x-nested',Nested);globalThis.nested=document.createElement('x-nested');"
                "addEventListener('error',e=>e.preventDefault());"
                "B.document.adoptNode(nested);assert(nested.ownerDocument===C.document);"
                "assert(nestedLog.join()==='B,C',nestedLog.join());",
                "cluster: custom callback re-adopts and throws without changing definition or queue");
            realm_script(realms[0],
                "globalThis.control=document.getElementById('field');control.value='dirty';"
                "control.setSelectionRange(1,4);control.setCustomValidity('kept');"
                "globalThis.attr=control.getAttributeNode('value');globalThis.attrs=control.attributes;"
                "globalThis.inline=control.style;globalThis.events=[];"
                "control.addEventListener('moved',()=>events.push('original'));"
                "B.document.body.appendChild(control);"
                "assert(control.value==='dirty'&&control.selectionStart===1&&control.selectionEnd===4);"
                "assert(control.validationMessage==='kept'&&control.validity.customError);"
                "assert(control.getAttributeNode('value')===attr&&control.attributes===attrs&&attr.ownerDocument===B.document);"
                "assert(control.style===inline);inline.color='red';assert(control.getAttribute('style').includes('red'));"
                "assert(control.offsetWidth===101);"
                "B.document.body.addEventListener('moved',()=>events.push('destination'));"
                "control.dispatchEvent(new C.Event('moved',{bubbles:true}));"
                "assert(events.join()==='original,destination',events.join());"
                "control.value={toString(){C.document.adoptNode(control);return 'converted'}};"
                "assert(control.ownerDocument===C.document&&control.value==='converted');"
                "C.document.body.appendChild(control);assert(control.offsetWidth===102);",
                "cluster: dirty controls, Attr/style identity, providers, listeners and reentrant adoption");
            realm_script(realms[0],
                "globalThis.eventRealms=[];globalThis.handlerNode=document.createElement('button');handlerNode.id='realm-handler';"
                "handlerNode.addEventListener('click',e=>eventRealms.push(window.event===e));"
                "handlerNode.setAttribute('onclick','A.eventRealms.push(window===B,event===window.event)');"
                "B.document.body.appendChild(handlerNode);handlerNode.dispatchEvent(new C.Event('click'));"
                "assert(eventRealms.join()==='true,true,true',eventRealms.join());"
                "assert(window.event===undefined&&B.event===undefined);eventRealms.length=0;"
                "C.document.body.appendChild(handlerNode);handlerNode.dispatchEvent(new B.Event('click'));"
                "assert(eventRealms.join()==='true,true,true',eventRealms.join());"
                "assert(window.event===undefined&&B.event===undefined&&C.event===undefined);",
                "cluster: callbacks expose their realm's current event and compiled handlers retain their realm");
            check(os64_html_set_attr(providers[2].document,
                native_id(providers[2].document->document, "realm-handler"), "onclick",
                "A.eventRealms.push(window===C)", strlen("A.eventRealms.push(window===C)")) == OS64_HTML_OK,
                "cluster: native handler text changed after adoption");
            realm_script(realms[0],
                "assert(handlerNode.onclick instanceof C.Function);eventRealms.length=0;"
                "handlerNode.dispatchEvent(new A.Event('click'));assert(eventRealms.join()==='true,true');",
                "cluster: refreshing a compiled handler uses its current Document's realm");
            realm_script(realms[0],
                "globalThis.listRoot=document.createElement('div');listRoot.innerHTML='<i>A</i>';"
                "globalThis.children=listRoot.childNodes;assert(children.length===1);"
                "B.document.body.appendChild(listRoot);listRoot.appendChild(B.document.createElement('b'));"
                "assert(children===listRoot.childNodes&&children.length===2&&children[1].ownerDocument===B.document);"
                "globalThis.liveRange=document.createRange();liveRange.selectNodeContents(listRoot.firstChild);"
                "B.document.adoptNode(listRoot);assert(liveRange.startContainer===B.document.body&&liveRange.collapsed);"
                "liveRange.selectNodeContents(listRoot.firstChild);C.document.adoptNode(listRoot);"
                "assert(liveRange.startContainer===listRoot.firstChild&&liveRange.toString()==='A');"
                "globalThis.fragment=liveRange.createContextualFragment('<em>C</em>');"
                "assert(fragment instanceof C.DocumentFragment&&fragment.firstChild instanceof C.HTMLElement);"
                "assert(fragment.ownerDocument===C.document);",
                "cluster: cached collections and live Range use current document after adoption");
            realm_script(realms[0],
                "globalThis.canvas=document.createElement('canvas');canvas.width=4;canvas.height=4;"
                "globalThis.paint=canvas.getContext('2d');paint.fillStyle='red';paint.fillRect(0,0,4,4);"
                "B.document.body.appendChild(canvas);assert(canvas.getContext('2d')===paint);"
                "B.CanvasRenderingContext2D.prototype.fillRect.call(paint,0,0,1,1);"
                "assert(paint.getImageData(0,0,1,1).data[0]===255);",
                "cluster: Canvas bitmap and context survive adoption and borrowed methods");
            realm_script(realms[0],
                "globalThis.computed=getComputedStyle(control);assert(computed.color==='blue');"
                "B.document.body.appendChild(control);assert(computed.color==='green');"
                "globalThis.picture=document.createElement('img');assert(picture.naturalWidth===100);"
                "B.document.adoptNode(picture);assert(picture.naturalWidth===101);"
                "assert(CustomElementRegistry.prototype.get.call(B.customElements,'x-shared')===B.customElements.get('x-shared'));"
                "assert(Document.prototype.createElementNS.call(B.document,'http://www.w3.org/1999/xhtml','span') instanceof B.HTMLElement);"
                "assert(B.document.implementation.createHTMLDocument().body instanceof B.HTMLElement);",
                "cluster: adopted image/computed providers and borrowed factories/registry");
            realm_script(realms[0],
                "globalThis.moving=document.createElement('div');moving.id='effect-moving';document.body.appendChild(moving);"
                "globalThis.motion=moving.animate({opacity:[0,1]},1000);motion.pause();motion.currentTime=20;",
                "cluster: source effect published");
            const os64_html_node_t *moving = wa_by_id(providers[0].document->document, "effect-moving");
            const struct garb_effect *const *effects = NULL;
            check(os64_dom_script_effects(bindings[0], moving, &effects) == 1,
                  "cluster effects: source layout acquires effect");
            const struct garb_effect *borrowed = effects ? effects[0] : NULL;
            uint64_t source_mark = os64_dom_effects_mark(bindings[0]);
            realm_script(realms[0], "B.document.body.appendChild(moving);motion.effect.setKeyframes({opacity:[1,0]});",
                         "cluster: adopted effect changes while source layout borrows it");
            check(os64_dom_script_effects(bindings[0], moving, &effects) == 0 &&
                  os64_dom_script_effects(bindings[1], moving, &effects) == 1,
                  "cluster effects: destination layout finds source-owned animation");
            (void)os64_dom_effects_mark(bindings[1]);
            os64_dom_effects_release(bindings[1], UINT64_MAX);
            size_t retained = os64_dom_cluster_bytes(cluster).dom_bytes;
            os64_dom_effects_release(bindings[0], source_mark);
            check(borrowed && borrowed->nframes == 2 && os64_dom_cluster_bytes(cluster).dom_bytes == retained,
                  "cluster effects: destination release preserves source layout data");
            os64_dom_effects_release(bindings[0], UINT64_MAX);
            check(os64_dom_cluster_bytes(cluster).dom_bytes < retained,
                  "cluster effects: final borrower release recovers native data");
            for (size_t i = 0; i < 3; ++i) os64_dom_canvas_visit_damage(bindings[i], cluster_damage, &providers[i]);
            check(providers[0].damages == 0 && providers[1].damages == 1 && providers[2].damages == 0,
                  "cluster: adopted Canvas damage belongs to destination host");
        }
        os64_dom_cluster_limits_t bytes = os64_dom_cluster_bytes(cluster);
        check(bytes.html_bytes && bytes.page_bytes && bytes.dom_bytes && bytes.canvas_bytes,
              "cluster: native accounts include all categories in use");
        os64_dom_cluster_drain(cluster);
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime, &report);
        os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "cluster: native backing freed after shared runtime");
        check(policy == OS64_JS_TEARDOWN_RECLAIM || !report.leaked, "cluster: finalizers leave no engine leftovers");
    }
}

/* Every native binding-allocation cut while installing a second member.
 * A pre-admission refusal keeps ownership; later failures consume it and
 * retain backing until shared-engine teardown. */
static void dom_cluster_failure_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
    size_t cuts = 0;
    for (size_t cut = 0; cut <= cuts; ++cut) {
        os64_js_config_t config = fixture_config();
        config.limits.memory_bytes = 64u << 20;
        DomFixture a = fixture_configured(0, 0, &config);
        os64_js_destroy(a.runtime);
        os64_js_outcome_t created;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &a.runtime, &created) == OS64_JS_OK,
              "cluster cut: teardown policy");
        DomFixture b = fixture_configured(0, 0, &config);
        os64_js_destroy(b.runtime);
        os64_js_outcome_t out;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(a.runtime, &limits, &out);
        os64_js_realm_t *primary = os64_js_primary_realm(a.runtime, OS64_JS_ABI_ID, &out), *child = NULL;
        check(os64_js_realm_create(a.runtime, OS64_JS_ABI_ID, &child, &out) == OS64_JS_OK,
              "cluster cut: child created");
        os64_dom_options_t options = p2b_options("https://a.example/");
        os64_dom_admission_t first = os64_dom_create_in_realm(cluster, primary, a.document, a.state, &options, &out);
        check(first.binding && first.consumed, "cluster cut: healthy first member");
        if (!first.binding) abort();
        size_t before = dom_attempts;
        dom_fail_at = cut ? before + cut : 0;
        os64_dom_admission_t second = os64_dom_create_in_realm(cluster, child, b.document, b.state, &options, &out);
        dom_fail_at = 0;
        if (!cut) cuts = dom_attempts - before;
        if (!second.consumed) {
            os64_page_state_free(b.state);
            os64_html_document_free(b.document);
            realm_script(primary, "if(!document.body)throw Error('lost first binding')", "cluster cut: refusal preserves sibling");
        } else if (!second.binding) {
            check(os64_dom_registry_count(first.binding) == 0, "cluster cut: failure drains sibling references");
            os64_dom_admission_t retry = os64_dom_create_in_realm(cluster, child, b.document, b.state, &options, &out);
            check(!retry.binding && !retry.consumed, "cluster cut: poisoned cluster refuses retry");
        }
        os64_dom_cluster_drain(cluster);
        os64_js_destroy(a.runtime);
        os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "cluster cut: retained native owners released");
    }
    printf("Cluster installation allocation cuts policy %d: %zu\n", policy, cuts);
    }
}

/* Tiny aggregate payload limits are independent of each binding's local cap.
 * A refused allocation leaves both realms usable and releases staging charges. */
static void dom_cluster_cap_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        os64_js_config_t config = fixture_config();
        config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "cluster cap: runtime");
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 1u<<20, 4096, 32768};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        os64_js_realm_t *realms[2] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out)};
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK, "cluster cap: child");
        for (unsigned i = 0; i < 2; ++i) {
            DomFixture f = fixture_configured(0, 0, &config);
            os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://cap.example/");
            os64_dom_admission_t a = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            check(a.binding && a.consumed, "cluster cap: member admitted");
            if (!a.binding) abort();
            os64_dom_admission_t duplicate = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            check(!duplicate.binding && !duplicate.consumed && out.status == OS64_JS_BAD_ARGUMENT,
                  "cluster cap: duplicate realm refused before ownership transfer");
            realm_script(realms[i], STORAGE_ASSERT
                "globalThis.body=new Blob(['x'.repeat(1200)]);assert(body.size===1200);"
                "globalThis.canvas=document.createElement('canvas');canvas.width=8;canvas.height=8;"
                "globalThis.pen=canvas.getContext('2d');pen.fillRect(0,0,1,1);",
                "cluster cap: both realms spend one shared allowance");
        }
        size_t kept = os64_dom_cluster_bytes(cluster).kept_bytes;
        check(kept > 2400 && kept <= 4096, "cluster cap: Blob bodies include headers in aggregate charge");
        realm_script(realms[1], "throws('QuotaExceededError',()=>new Blob(['x'.repeat(2000)]));assert(body.size===1200);",
                     "cluster cap: aggregate Blob refusal preserves existing bodies");
        check(os64_dom_cluster_bytes(cluster).kept_bytes == kept, "cluster cap: refused body returns staged charge");
        realm_script(realms[0], "body=null;", "cluster cap: release source body");
        os64_js_outcome_t context;
        JSContext *ctx = os64_js_realm_context(realms[0], OS64_JS_ABI_ID, &context);
        JS_RunGC(JS_GetRuntime(ctx));
        check(os64_dom_cluster_bytes(cluster).kept_bytes < kept, "cluster cap: source finalizer recovers shared charge");
        realm_script(realms[1], "globalThis.recovered=new Blob(['x'.repeat(2000)]);assert(recovered.size===2000);",
                     "cluster cap: sibling spends recovered allowance");
        realm_script(realms[0], "canvas.width=1000;canvas.height=1000;canvas.width=8;canvas.height=8;"
                     "assert(canvas.getContext('2d')===pen);pen.fillStyle='red';pen.fillRect(0,0,1,1);"
                     "assert(pen.getImageData(0,0,1,1).data[0]===255);",
                     "cluster cap: oversized Canvas resets then recovers without replacing context");
        realm_script(realms[0], "globalThis.hitCap=false;for(let i=0;i<20000;i++){try{document.createTextNode('cap')}"
                     "catch(e){assert(e.name==='QuotaExceededError');hitCap=true;break}}assert(hitCap);",
                     "cluster cap: retained wrapper charge obeys aggregate DOM cap");
        realm_script(realms[1], "assert(document.URL==='https://cap.example/');assert(1+1===2);",
                     "cluster cap: native DOM refusal leaves sibling runtime usable");
        os64_dom_cluster_limits_t bytes = os64_dom_cluster_bytes(cluster);
        check(bytes.dom_bytes <= limits.dom_bytes, "cluster cap: retained wrappers stay charged");
        check(bytes.canvas_bytes <= limits.canvas_bytes && bytes.kept_bytes <= limits.kept_bytes,
              "cluster cap: payload accounts stay bounded");
        os64_dom_cluster_drain(cluster);
        os64_js_destroy(runtime);
        os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "cluster cap: both teardown policies release native payloads");
    }
}
