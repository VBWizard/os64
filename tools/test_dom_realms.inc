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
    unsigned calls, damages;
} ClusterProvider;
static bool cluster_geometry(void *opaque, const os64_html_node_t *node, os64_dom_geometry_t *out)
{
    ClusterProvider *provider = opaque;
    check(os64_html_node_owner(node) == provider->document, "cluster provider: current document owns receiver");
    provider->calls++;
    *out = (os64_dom_geometry_t){.offset_width = provider->width, .boxed = true};
    return true;
}
static bool cluster_computed(void *opaque, const os64_html_node_t *node, os64_dom_computed_t *out)
{
    ClusterProvider *p = opaque;
    check(os64_html_node_owner(node) == p->document, "cluster computed: destination provider");
    const char *color = p->width == 100 ? "red" : p->width == 101 ? "green" : "blue";
    return os64_dom_computed_put(out, GARB_COLOR, color, strlen(color));
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
