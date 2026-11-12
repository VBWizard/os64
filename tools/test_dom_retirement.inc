/* Trusted engine retirement exercises shared delivery without a WindowProxy
 * or a browser navigation. It does not stand in for DOM departure cleanup. */
static os64_js_realm_t *retirement_realms[3];
static os64_html_document_t *retirement_documents[3];
static size_t retirement_misses[3];
/* Task-payload cleanup seam; public departure finish will coordinate it with
 * engine, DOM-dispatch and provider-borrow safe points. */
void d_message_cancel(os64_dom_t *dom);

static void retirement_missing(void *opaque, const char *name)
{
    if (!strcmp(name, "retirementMissingName")) (*(size_t *)opaque)++;
}

static JSValue retirement_mark(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv, int magic)
{
    (void)self; (void)argc; (void)argv;
    os64_js_outcome_t out;
    if (os64_js_realm_retire(retirement_realms[magic], OS64_JS_ABI_ID, &out) != OS64_JS_OK)
        return JS_ThrowInternalError(ctx, "retirement mark failed");
    return JS_UNDEFINED;
}

/* Native tree changes have no [CEReactions] scope: their custom callbacks use
 * the backup queue. B deliberately supplies its first microtask context. */
static JSValue retirement_pulse(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv;
    static const struct { int document; const char *id; } targets[] = {
        {1, "pulse"}, {0, "pulse"}, {2, "pulse"},
        {2, "adopted"}, {0, "dead-callback"}, {1, "dead-owner"}
    };
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        os64_html_document_t *doc = retirement_documents[targets[i].document];
        os64_html_node_t *node = (os64_html_node_t *)native_id(doc->document, targets[i].id);
        if (!node || os64_html_set_attr(doc, node, "data-pulse", "yes", 3) != OS64_HTML_OK)
            return JS_ThrowInternalError(ctx, "native reaction stimulus failed");
    }
    for (size_t i = 0; i < 3; ++i) {
        os64_html_document_t *doc = retirement_documents[i];
        int64_t status;
        os64_html_node_t *node = os64_html_create_element(doc, OS64_HTML_NS_HTML, "x-late", &status);
        if (!node || os64_html_insert(doc, doc->body, node, NULL) != OS64_HTML_OK)
            return JS_ThrowInternalError(ctx, "native upgrade stimulus failed");
    }
    return JS_UNDEFINED;
}

static void retirement_reentry_cases(void)
{
    static const char *operations[] = {
        "postMessage({get value(){retire();return 1}},'*',[buffer])",
        "postMessage('value',{transfer:[buffer],get targetOrigin(){retire();return '*'}})",
        "channel.port1.postMessage({get value(){retire();return 1}},[buffer])",
        "channel.port1.postMessage('value',{get transfer(){retire();return [buffer]}})",
        "Reflect.construct(MessageChannel,[],new Proxy(function(){},{get(t,k){"
        "if(k==='prototype')retire();return Reflect.get(t,k)}}))",
        "new URLSearchParams('a=1&b=2').forEach(()=>{calls++;retire()})",
        "new Headers({a:'1',b:'2'}).forEach(()=>{calls++;retire()})",
        "(()=>{let form=new FormData();form.append('a','1');form.append('b','2');"
        "form.forEach(()=>{calls++;retire()})})()"
    };
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        for (size_t i = 0; i < sizeof(operations)/sizeof(operations[0]); ++i) {
            os64_js_config_t config = fixture_config();
            DomFixture f = fixture_configured(0, 0, &config);
            os64_js_destroy(f.runtime);
            f.runtime = NULL;
            os64_js_outcome_t out;
            check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &f.runtime, &out) == OS64_JS_OK,
                  "retirement reentry: runtime created");
            retirement_realms[1] = os64_js_primary_realm(f.runtime, OS64_JS_ABI_ID, &out);
            os64_dom_options_t options = p2b_options("https://messages.example/");
            f.binding = os64_dom_create(f.runtime, f.document, f.state, &options, &out);
            check(f.binding != NULL, "retirement reentry: binding installed");
            if (f.binding) {
                check(os64_js_realm_task_begin(retirement_realms[1], OS64_JS_ABI_ID, "retirement reentry", &out) == OS64_JS_OK,
                      "retirement reentry: fixture entry opens");
                JSContext *ctx = os64_js_realm_context(retirement_realms[1], OS64_JS_ABI_ID, &out);
                JSValue global = JS_GetGlobalObject(ctx);
                check(JS_SetPropertyStr(ctx, global, "retire", JS_NewCFunctionMagic(ctx, retirement_mark,
                      "retire", 0, JS_CFUNC_generic_magic, 1)) >= 0,
                      "retirement reentry: native mark installed");
                JS_FreeValue(ctx, global);
                check(os64_js_task_end(f.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "retirement reentry: fixture entry closes");
                char source[1600];
                snprintf(source, sizeof(source), STORAGE_ASSERT
                    "globalThis.channel=new MessageChannel();globalThis.buffer=new ArrayBuffer(16);"
                    "globalThis.calls=0;"
                    "let refused=false;try{%s}catch(e){refused=e.name==='InvalidStateError'}"
                    "assert(refused,'conversion or callback retirement must refuse work');"
                    "assert(buffer.byteLength===16,'refusal must not detach transfers');"
                    "assert(calls<=1,'later data callbacks must recheck their realm');"
                    "channel.port1.close();channel.port2.close();", operations[i]);
                char name[100];
                snprintf(name, sizeof(name), "retirement reentry: operation %zu refuses without transfer", i);
                realm_script(retirement_realms[1], source, name);
                check(os64_dom_message_next(f.binding) == UINT64_MAX && !os64_dom_message_fire(f.binding, &out),
                      "retirement reentry: no host message delivery");
            }
            fixture_free(&f);
        }
    }
}

static void dom_retirement_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        p2b_ms = 500; p2b_us = 500000;
        memset(retirement_misses, 0, sizeof(retirement_misses));
        os64_js_config_t config = fixture_config();
        config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL;
        os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "retirement queues: runtime created");
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        check(cluster != NULL, "retirement queues: cluster created");
        if (!cluster) { os64_js_destroy(runtime); continue; }
        retirement_realms[0] = os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out);
        os64_dom_t *bindings[3] = {0};
        ClusterProvider render_providers[3] = {0};
        for (size_t i = 0; i < 3; ++i) {
            if (i) check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &retirement_realms[i], &out) == OS64_JS_OK,
                         "retirement queues: child realm created");
            DomFixture f = fixture_configured(0, 0, &config);
            os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options(i == 0 ? "https://a.example/" :
                                                   i == 1 ? "https://b.example/" : "https://c.example/");
            options.shadow_trees = options.ranges = true;
            os64_dom_admission_t result = os64_dom_create_in_realm(cluster, retirement_realms[i], f.document,
                                                                  f.state, &options, &out);
            bindings[i] = result.binding;
            retirement_documents[i] = f.document;
            check(result.binding && result.consumed && out.status == OS64_JS_OK,
                  "retirement queues: independent binding installed");
            if (!result.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
            if (!result.binding) break;
            render_providers[i] = (ClusterProvider){.document = f.document, .width = 100 + (int)i};
            os64_dom_set_resizes(bindings[i], cluster_resizes, &render_providers[i]);
            os64_dom_set_intersections(bindings[i], cluster_intersections, &render_providers[i]);
            os64_dom_view_provider_t view = {.opaque = &render_providers[i], .read = cluster_view};
            os64_dom_set_view(bindings[i], &view);
        }
        if (bindings[0] && bindings[1] && bindings[2]) {
            check(os64_js_realm_task_begin(retirement_realms[0], OS64_JS_ABI_ID, "retirement exchange", &out) == OS64_JS_OK,
                  "retirement queues: exchange opens");
            for (size_t i = 0; i < 3; ++i) {
                JSContext *ctx = os64_js_realm_context(retirement_realms[i], OS64_JS_ABI_ID, &out);
                JSValue global = JS_GetGlobalObject(ctx);
                for (size_t j = 0; j < 3; ++j) {
                    JSContext *other = os64_js_realm_context(retirement_realms[j], OS64_JS_ABI_ID, &out);
                    check(JS_SetPropertyStr(ctx, global, j == 0 ? "A" : j == 1 ? "B" : "C", JS_GetGlobalObject(other)) >= 0,
                          "retirement queues: trusted references exported");
                }
                if (i == 0) {
                    check(JS_SetPropertyStr(ctx, global, "retireB", JS_NewCFunctionMagic(ctx, retirement_mark,
                          "retireB", 0, JS_CFUNC_generic_magic, 1)) >= 0, "retirement queues: B mark exposed to fixture");
                    check(JS_SetPropertyStr(ctx, global, "retireC", JS_NewCFunctionMagic(ctx, retirement_mark,
                          "retireC", 0, JS_CFUNC_generic_magic, 2)) >= 0, "retirement queues: C mark exposed to fixture");
                    check(JS_SetPropertyStr(ctx, global, "pulse", JS_NewCFunction(ctx, retirement_pulse,
                          "pulse", 0)) >= 0, "retirement queues: native changes exposed to fixture");
                }
                JS_FreeValue(ctx, global);
            }
            check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "retirement queues: exchange closes");
            for (size_t i = 0; i < 3; ++i) {
                realm_script(retirement_realms[i], STORAGE_ASSERT
                    "globalThis.observed=document.createElement('div');document.body.appendChild(observed);"
                    "globalThis.plain=()=>42;globalThis.missing=()=>typeof retirementMissingName;",
                    "retirement queues: retained objects installed");
                os64_dom_set_global_miss(bindings[i], retirement_missing, &retirement_misses[i]);
            }
            realm_script(retirement_realms[0], "globalThis.moLog=[];globalThis.ceLog=[];globalThis.upgrades=[];globalThis.renderLog=[];"
                "globalThis.slots=[];globalThis.stopC=false;"
                "globalThis.liveA=function(){moLog.push('A');if(stopC)retireC()};",
                "retirement queues: A callbacks installed");
            realm_script(retirement_realms[2], "globalThis.liveC=function(){A.moLog.push('C')};"
                "globalThis.reaction=function(){A.ceLog.push(this.id)};"
                "globalThis.liveEvent=function(){A.eventLog.push('C')};",
                "retirement queues: C callbacks installed");
            realm_script(retirement_realms[0], "globalThis.eventLog=[];globalThis.getters=0;globalThis.taskLog=[];"
                "globalThis.refused=f=>{try{f();return false}catch(e){return e.name==='InvalidStateError'}};",
                "retirement events: shared oracle installed");
            realm_script(retirement_realms[1],
                "globalThis.eventNode=document.createElement('div');eventNode.id='depart-event';"
                "C.document.body.appendChild(eventNode);"
                "eventNode.addEventListener('life',()=>A.eventLog.push('dead')) ;"
                "eventNode.addEventListener('life',{handleEvent:C.liveEvent});"
                "eventNode.onclick=()=>A.eventLog.push('dead-slot');"
                "globalThis.inactiveTarget=document.createElement('div');document.body.appendChild(inactiveTarget);"
                "inactiveTarget.addEventListener('life',C.liveEvent);"
                "inactiveTarget.addEventListener('life',{get handleEvent(){A.getters+=100;return C.liveEvent}});"
                "globalThis.deadEvent=()=>A.eventLog.push('dead-getter-result');"
                "globalThis.deadError=()=>{A.eventLog.push('dead-error');return true};",
                "retirement events: B-created adopted target and B-registered C method");
            realm_script(retirement_realms[2],
                "B.eventNode.addEventListener('life',liveEvent);"
                "globalThis.microLive=()=>A.taskLog.push('micro');"
                "globalThis.timerLive=()=>A.taskLog.push('timer');"
                "globalThis.rafLive=()=>A.taskLog.push('raf');"
                "globalThis.perfLive=()=>A.renderLog.push('performance');"
                "globalThis.resizeLive=()=>A.renderLog.push('resize');"
                "globalThis.intersectionLive=()=>A.renderLog.push('intersection');"
                "globalThis.filterLive=()=>{A.walkLog.push('C');return 1};"
                "globalThis.streamLive=c=>{c.enqueue('live');c.close()};"
                "globalThis.idleLive=d=>A.taskLog.push(d.didTimeout?'timeout-idle':'idle');",
                "retirement events: live callback on adopted target");
            realm_script(retirement_realms[0],
                "globalThis.getterTarget=document.createElement('div');"
                "getterTarget.addEventListener('life',{get handleEvent(){getters++;retireB();return B.deadEvent}});"
                "getterTarget.addEventListener('life',()=>eventLog.push('tail'));"
                "globalThis.swapTarget=document.createElement('div');"
                "swapTarget.addEventListener('life',{get handleEvent(){getters++;B.document.adoptNode(swapTarget);return C.liveEvent}});"
                "swapTarget.addEventListener('life',()=>eventLog.push('inactive-tail'));",
                "retirement events: getter retirement and adoption stimuli installed");
            realm_script(retirement_realms[1],
                "globalThis.deadlineSaved=null;requestIdleCallback(d=>deadlineSaved=d);",
                "retirement tasks: retain an idle deadline from B");
            check(os64_dom_idle_run(bindings[1], &out) && out.status == OS64_JS_OK,
                  "retirement tasks: capture deadline while B is active");
            realm_script(retirement_realms[1],
                "globalThis.deadTask=()=>A.taskLog.push('dead');setTimeout(C.timerLive,0);"
                "requestAnimationFrame(C.rafLive);requestIdleCallback(C.idleLive);"
                "globalThis.channel=new MessageChannel();channel.port1.postMessage({kept:'pending'});"
                "globalThis.retainedResponse=new Response('settled');globalThis.retainedBlob=new Blob(['bytes']);"
                "globalThis.retainedPull=new ReadableStream({pull:deadTask},{highWaterMark:0});"
                "globalThis.retainedBytePull=new ReadableStream({type:'bytes',pull:deadTask},{highWaterMark:0});"
                "globalThis.retainedLivePull=new ReadableStream({pull:C.streamLive},{highWaterMark:0});"
                "globalThis.retainedByteResponse=new Response(new ReadableStream({type:'bytes',start(c){"
                "c.enqueue(new TextEncoder().encode('byte-streamed'));c.close()}}));"
                "globalThis.retainedStreamResponse=new Response(new ReadableStream({start(c){"
                "c.enqueue(new TextEncoder().encode('streamed'));c.close()}}));"
                "globalThis.ro=new ResizeObserver(C.resizeLive);ro.observe(C.observed);"
                "globalThis.io=new IntersectionObserver(C.intersectionLive,{root:C.document});io.observe(C.observed);"
                "postMessage('queued','*');onmessage=C.timerLive;assert(missing()==='undefined');",
                "retirement tasks: pending B activity has live C callbacks");
            check(retirement_misses[1] == 1, "retirement providers: B diagnostic hook receives active lookup");
            realm_script(retirement_realms[0],
                "globalThis.deadInterval=setInterval(B.deadTask,0);setTimeout(C.timerLive,0);"
                "globalThis.deadPerf=new PerformanceObserver(B.deadTask);deadPerf.observe({entryTypes:['mark']});"
                "globalThis.livePerf=new PerformanceObserver(C.perfLive);livePerf.observe({entryTypes:['mark']});"
                "performance.mark('before-retirement');"
                "globalThis.deadResize=new ResizeObserver(B.deadTask);deadResize.observe(observed);"
                "globalThis.liveResize=new ResizeObserver(C.resizeLive);liveResize.observe(B.eventNode);"
                "globalThis.retainedResize=new ResizeObserver(C.resizeLive);retainedResize.observe(B.observed);"
                "globalThis.deadIntersection=new IntersectionObserver(B.deadTask);deadIntersection.observe(observed);"
                "globalThis.liveIntersection=new IntersectionObserver(C.intersectionLive);liveIntersection.observe(observed);"
                "globalThis.retainedIntersection=new IntersectionObserver(C.intersectionLive,{root:B.document});"
                "retainedIntersection.observe(B.observed);"
                "requestAnimationFrame(B.deadTask);requestAnimationFrame(C.rafLive);"
                "requestIdleCallback(B.deadTask,{timeout:1});requestIdleCallback(C.idleLive,{timeout:1});"
                "requestIdleCallback(B.deadTask);requestIdleCallback(C.idleLive);",
                "retirement tasks: live owner queues both dead and live callback realms");
            realm_script(retirement_realms[1],
                "globalThis.dead=function(){A.moLog.push('dead-callback')};"
                "globalThis.mo=new MutationObserver(C.liveC);mo.observe(observed,{attributes:true});",
                "retirement queues: live callback with B activity owner");
            realm_script(retirement_realms[0],
                "globalThis.mo=new MutationObserver(liveA);mo.observe(observed,{attributes:true});"
                "globalThis.foreign=new MutationObserver(B.dead);foreign.observe(observed,{attributes:true});"
                "globalThis.bound=new MutationObserver(B.dead.bind(null));bound.observe(observed,{attributes:true});"
                "globalThis.proxied=new MutationObserver(new Proxy(B.dead,{}));proxied.observe(observed,{attributes:true});"
                "globalThis.rev=Proxy.revocable(()=>moLog.push('revoked'),{});"
                "globalThis.revmo=new MutationObserver(rev.proxy);revmo.observe(observed,{attributes:true});rev.revoke();",
                "retirement queues: B callback with live activity owner");
            realm_script(retirement_realms[2],
                "globalThis.mo=new MutationObserver(liveC);mo.observe(observed,{attributes:true});",
                "retirement queues: C observer installed");
            for (size_t i = 0; i < 3; ++i) {
                realm_script(retirement_realms[i],
                    "class Pulse extends HTMLElement {static get observedAttributes(){return ['data-pulse']}"
                    "attributeChangedCallback(){A.ceLog.push(this.ownerDocument===A.document?'A':"
                    "this.ownerDocument===B.document?'B':'C')}}"
                    "customElements.define('x-pulse',Pulse);globalThis.ce=document.createElement('x-pulse');"
                    "ce.id='pulse';document.body.appendChild(ce);"
                    "customElements.define('x-late',class extends HTMLElement {constructor(){super();"
                    "A.upgrades.push(document===A.document?'A':document===B.document?'B':'C')}});"
                    "globalThis.host=document.createElement('div');document.body.appendChild(host);"
                    "globalThis.slot=document.createElement('slot');host.attachShadow({mode:'open'}).appendChild(slot);"
                    "slot.addEventListener('slotchange',()=>A.slots.push(document===A.document?'A':document===B.document?'B':'C'));",
                    "retirement queues: custom definition installed");
            }
            realm_script(retirement_realms[1],
                "class Foreign extends HTMLElement {static get observedAttributes(){return ['data-pulse']}}"
                "Foreign.prototype.attributeChangedCallback=C.reaction;customElements.define('x-foreign',Foreign);"
                "globalThis.adopted=document.createElement('x-foreign');adopted.id='adopted';"
                "C.document.body.appendChild(adopted);"
                "globalThis.deadOwner=document.createElement('x-foreign');deadOwner.id='dead-owner';"
                "document.body.appendChild(deadOwner);"
                "globalThis.deadReaction=function(){A.ceLog.push('dead-callback')};",
                "retirement queues: retained B definition with live C callback and adopted element");
            realm_script(retirement_realms[0],
                "class Foreign extends HTMLElement {static get observedAttributes(){return ['data-pulse']}}"
                "Foreign.prototype.attributeChangedCallback=B.deadReaction;customElements.define('x-foreign',Foreign);"
                "globalThis.deadCallback=document.createElement('x-foreign');deadCallback.id='dead-callback';"
                "document.body.appendChild(deadCallback);"
                "B.observed.setAttribute('data-first','1');observed.setAttribute('data-first','1');"
                "C.observed.setAttribute('data-first','1');"
                "queueMicrotask(B.deadTask);B.queueMicrotask(C.microLive);queueMicrotask(C.microLive);"
                "for(const w of [B,A,C])w.host.appendChild(w.document.createElement('i'));pulse();"
                "let converted=0;assert(refused(()=>B.setTimeout(C.timerLive,{valueOf(){"
                "converted++;getterTarget.dispatchEvent(new Event('life'));return 0}})));"
                "assert(converted===1,'timer converts delay before checking reentrant departure');"
                "assert(eventLog.join()==='tail','getter retires returned callback realm: '+eventLog);eventLog=[];"
                "swapTarget.dispatchEvent(new Event('life'));"
                "assert(eventLog.length===0&&getters===2,'getter changes activity document');retireB();",
                "retirement queues: B enqueues shared jobs then retires before checkpoint");
            realm_script(retirement_realms[0],
                "assert(moLog.join()==='A,C','observer delivery: '+moLog);",
                "retirement queues: observers gate activity and bound/proxy callback realms");
            realm_script(retirement_realms[0],
                "assert(ceLog.join()==='A,C,adopted','custom delivery: '+ceLog);",
                "retirement queues: shared backup preserves sibling reactions");
            realm_script(retirement_realms[0],
                "assert(upgrades.join()==='A,C','queued constructors: '+upgrades);",
                "retirement queues: queued constructors gate current document and function realm");
            realm_script(retirement_realms[0],
                "assert(slots.join()==='A,C','slotchange delivery: '+slots);",
                "retirement queues: shared slot signals skip retired document");
            realm_script(retirement_realms[0],
                "assert(B.eventNode.dispatchEvent(new Event('life')));"
                "assert(B.inactiveTarget.dispatchEvent(new Event('life')));"
                "B.eventNode.click();onerror=B.deadError;"
                "assert(dispatchEvent(new ErrorEvent('error',{cancelable:true})));onerror=null;"
                "assert(eventLog.join()==='C,C'&&getters===2,'script dispatch eligibility: '+eventLog);eventLog=[];",
                "retirement events: retained dispatch gates target activity and callback realm");
            os64_dom_event_t event = {.type = "life", .kind = OS64_DOM_EVENT_PLAIN};
            bool prevented = true;
            uint64_t b_calls = os64_dom_listener_calls(bindings[1]), c_calls = os64_dom_listener_calls(bindings[2]);
            check(os64_dom_dispatch(bindings[2], native_id(retirement_documents[2]->document, "depart-event"),
                  &event, &prevented, &out) == OS64_JS_OK && !prevented,
                  "retirement events: host dispatch avoids retired registration entry");
            check(os64_dom_listener_calls(bindings[1]) == b_calls + 1 &&
                  os64_dom_listener_calls(bindings[2]) == c_calls + 1,
                  "retirement events: suppressed listeners do not count as host calls");
            realm_script(retirement_realms[0],
                "assert(eventLog.join()==='C,C','host dispatch eligibility: '+eventLog);eventLog=[];"
                "globalThis.hostSwap=document.createElement('div');hostSwap.id='host-swap';document.body.appendChild(hostSwap);"
                "B.EventTarget.prototype.addEventListener.call(hostSwap,'life',{get handleEvent(){"
                "getters++;B.document.adoptNode(hostSwap);return C.liveEvent}});"
                "hostSwap.addEventListener('life',()=>eventLog.push('host-inactive-tail'));",
                "retirement events: host dispatch preserves live object handler and function");
            os64_js_outcome_t reported;
            check(os64_dom_take_report(bindings[0], &reported) && reported.status == OS64_JS_EXCEPTION &&
                  strstr(reported.message, "revoked") != NULL,
                  "retirement queues: revoked observer lookup retains its ordinary report");
            check(os64_dom_dispatch(bindings[0], native_id(retirement_documents[0]->document, "host-swap"),
                  &event, &prevented, &out) == OS64_JS_OK && !prevented,
                  "retirement events: host getter may adopt target into inactive document");
            realm_script(retirement_realms[0],
                "assert(hostSwap.ownerDocument===B.document&&eventLog.length===0&&getters===3);",
                "retirement events: host getter and later listener recheck current activity");
            realm_script(retirement_realms[0],
                "assert(taskLog.join()==='micro','microtask owners: '+taskLog);"
                "assert(refused(()=>B.queueMicrotask(C.microLive)));"
                "assert(refused(()=>B.setTimeout(C.timerLive,0)));"
                "assert(refused(()=>B.requestAnimationFrame(C.rafLive)));"
                "assert(refused(()=>B.requestIdleCallback(C.idleLive)));"
                "assert(refused(()=>B.postMessage('late','*')));"
                "assert(refused(()=>B.channel.port1.postMessage('late')));"
                "let serialized=0;let payload={get value(){serialized++;return 1}};"
                "assert(refused(()=>B.postMessage(payload,'*')));"
                "assert(refused(()=>B.channel.port1.postMessage(payload)));assert(serialized===0);"
                "assert(refused(()=>new B.MessageChannel()));"
                "B.channel.port2.start();assert(B.missing()==='undefined');assert(C.missing()==='undefined');"
                "assert(B.deadlineSaved.timeRemaining()===0&&!B.deadlineSaved.didTimeout);"
                "const retainedInput=B.document.createElement('input');retainedInput.value='retained';"
                "retainedInput.setSelectionRange(1,3);assert(retainedInput.selectionStart===1&&retainedInput.selectionEnd===3);",
                "retirement tasks: microtasks filter callback and activity; dead services refuse rearming");
            check(retirement_misses[1] == 1 && retirement_misses[2] == 1,
                  "retirement providers: retained B function skips old diagnostic provider while C remains active");
            check(os64_dom_message_next(bindings[1]) == UINT64_MAX && !os64_dom_message_fire(bindings[1], &out),
                  "retirement messages: inactive target does not deliver queued window or port messages");
            size_t queued_bytes = os64_dom_bytes(bindings[1]);
            d_message_cancel(bindings[1]);
            size_t cancelled_bytes = os64_dom_bytes(bindings[1]);
            d_message_cancel(bindings[1]);
            check(cancelled_bytes < queued_bytes && os64_dom_bytes(bindings[1]) == cancelled_bytes,
                  "retirement messages: task cleanup releases payloads idempotently");
            realm_script(retirement_realms[0],
                "assert(B.channel.port1 instanceof B.MessagePort);"
                "B.channel.port1.close();B.channel.port2.close();",
                "retirement messages: exported ports remain usable after task cleanup");
            check(os64_dom_bytes(bindings[1]) == cancelled_bytes,
                  "retirement messages: unstarted port payloads were released before retained close");
            realm_script(retirement_realms[0],
                "globalThis.liveChannel=new MessageChannel();globalThis.portHeard=[];"
                "liveChannel.port2.onmessage=e=>portHeard.push(e.data);"
                "B.MessagePort.prototype.postMessage.call(liveChannel.port1,'live');"
                "assert(refused(()=>MessagePort.prototype.postMessage.call(B.channel.port1,'dead')));",
                "retirement messages: borrowed methods follow receiver activity");
            check(os64_dom_message_fire(bindings[0], &out) && out.status == OS64_JS_OK,
                  "retirement messages: borrowed live port delivery succeeds");
            realm_script(retirement_realms[0], "assert(portHeard.join()==='live');",
                "retirement messages: borrowed post keeps live target and payload");
            check(os64_dom_timer_next(bindings[1]) == UINT64_MAX &&
                  os64_dom_animation_next(bindings[1]) == UINT64_MAX &&
                  os64_dom_idle_next(bindings[1]) == UINT64_MAX &&
                  !os64_dom_timer_fire(bindings[1], 1000, &out) &&
                  !os64_dom_animation_fire(bindings[1], 1000, &out) &&
                  !os64_dom_idle_run(bindings[1], &out),
                  "retirement tasks: inactive B has no host scheduler opportunity");
            unsigned fired = 0;
            while (fired < 10 && os64_dom_timer_fire(bindings[0], 1000, &out)) {
                check(out.status == OS64_JS_OK, "retirement tasks: discarded callback is not a task failure");
                fired++;
            }
            check(fired == 5 && os64_dom_timer_next(bindings[0]) == UINT64_MAX,
                  "retirement tasks: dead interval removed and timeout callbacks consumed");
            realm_script(retirement_realms[0], "assert(renderLog.join()==='performance');assert(deadPerf.takeRecords().length===0);",
                "retirement observers: live performance callback follows suppressed callback");
            check(!os64_dom_resizes_step(bindings[1], &out) && !os64_dom_resizes_owed(bindings[1]) &&
                  !os64_dom_intersections_update(bindings[1]) && !os64_dom_intersections_owed(bindings[1]) &&
                  !os64_dom_intersections_due(bindings[1]) && !os64_dom_intersections_fire(bindings[1], &out) &&
                  os64_dom_intersections_next(bindings[1]) == UINT64_MAX &&
                  render_providers[1].resize_calls == 0 && render_providers[1].intersection_calls == 0,
                  "retirement observers: inactive owner neither measures nor delivers");
            check(os64_dom_resizes_step(bindings[0], &out) && out.status == OS64_JS_OK &&
                  render_providers[2].resize_calls != 0,
                  "retirement observers: live resize follows dead callback and measures adopted target in C");
            check(os64_dom_intersections_update(bindings[0]) && os64_dom_intersections_fire(bindings[0], &out) &&
                  out.status == OS64_JS_OK && render_providers[1].resize_calls == 0 &&
                  render_providers[1].intersection_calls == 0,
                  "retirement observers: live intersection follows dead callback");
            realm_script(retirement_realms[0],
                "assert(renderLog.join()==='performance,resize,intersection','observer deliveries: '+renderLog);"
                "assert(deadIntersection.takeRecords().length===0);"
                "deadResize.disconnect();liveResize.disconnect();deadIntersection.disconnect();liveIntersection.disconnect();"
                "B.ro.disconnect();B.io.disconnect();",
                "retirement observers: obsolete records consumed and exported observers disconnect");
            check(!os64_dom_resizes_owed(bindings[0]) && !os64_dom_intersections_owed(bindings[0]) &&
                  os64_dom_intersections_next(bindings[0]) == UINT64_MAX,
                  "retirement observers: retained inactive targets do not keep measurement work owed");
            realm_script(retirement_realms[0],
                "assert(retainedIntersection.takeRecords().length===0);retainedIntersection.disconnect();"
                "C.document.body.appendChild(C.document.adoptNode(B.observed));",
                "retirement observers: retained target moves from inactive B into active C");
            check(os64_dom_resizes_owed(bindings[0]) && os64_dom_resizes_step(bindings[0], &out) &&
                  out.status == OS64_JS_OK && render_providers[1].resize_calls == 0,
                  "retirement observers: retained registration resumes with current active target owner");
            realm_script(retirement_realms[0],
                "assert(renderLog.join()==='performance,resize,intersection,resize');retainedResize.disconnect();",
                "retirement observers: adopted retained target receives its first eligible resize");
            check(os64_dom_animation_fire(bindings[0], 1000, &out) && out.status == OS64_JS_OK,
                  "retirement tasks: live rAF follows a dead callback in the same batch");
            for (int i = 0; i < 2; ++i)
                check(os64_dom_idle_run(bindings[0], &out) && out.status == OS64_JS_OK,
                      "retirement tasks: idle delivery consumes dead callback without failing siblings");
            realm_script(retirement_realms[0],
                "assert(taskLog.join()==='micro,timer,timeout-idle,raf,idle','scheduled delivery: '+taskLog);",
                "retirement tasks: exact surviving scheduled callback order");
            realm_script(retirement_realms[0],
                "globalThis.walkLog=[];let root=document.createElement('div'),child=document.createElement('span');"
                "root.appendChild(child);let deadWalk=document.createTreeWalker(root,1,B.deadTask);"
                "assert(refused(()=>deadWalk.nextNode()));"
                "let liveWalk=B.document.createTreeWalker(root,1,C.filterLive);"
                "assert(liveWalk.nextNode()===child&&walkLog.join()==='C');"
                "let inactiveChild=B.document.createElement('span');B.inactiveTarget.appendChild(inactiveChild);"
                "let plainWalk=B.document.createTreeWalker(B.inactiveTarget,1);assert(plainWalk.nextNode()===inactiveChild);"
                "let lookups=0;let inactiveWalk=document.createTreeWalker(B.inactiveTarget,1,{get acceptNode(){lookups++;return C.filterLive}});"
                "assert(refused(()=>inactiveWalk.nextNode())&&lookups===0);"
                "let movingWalk=document.createTreeWalker(root,1,{get acceptNode(){B.document.adoptNode(child);return C.filterLive}});"
                "assert(refused(()=>movingWalk.nextNode())&&walkLog.join()==='C');"
                "let iter=document.createNodeIterator(root,1,B.deadTask);assert(refused(()=>iter.nextNode()));"
                "assert(taskLog.join()==='micro,timer,timeout-idle,raf,idle');",
                "retirement traversal: gate filters by current target and function realm while pure traversal survives");
            realm_script(retirement_realms[0],
                "walkLog=[];let dataSets=[new B.URLSearchParams('x=1'),new B.Headers({x:'1'}),new B.FormData()];"
                "dataSets[2].append('x','1');for(let data of dataSets){"
                "assert(refused(()=>data.forEach(B.deadTask)));data.forEach(C.filterLive)}"
                "assert(walkLog.join()==='C,C,C');"
                "assert(taskLog.join()==='micro,timer,timeout-idle,raf,idle');",
                "retirement iteration: retained data admits live callbacks without reviving dead callbacks");
            realm_script(retirement_realms[0],
                "globalThis.streamResults=[];globalThis.streamBad=(name,p)=>p.then("
                "()=>streamResults.push(name+':unexpected'),e=>streamResults.push(name+':'+e.name));"
                "assert(refused(()=>new ReadableStream({start:B.deadTask})));"
                "assert(refused(()=>new ReadableStream({type:'bytes',start:B.deadTask})));"
                "assert(refused(()=>new WritableStream({start:B.deadTask})));"
                "assert(refused(()=>new TransformStream({start:B.deadTask})));"
                "assert(refused(()=>new ReadableStream({start:B.deadTask.bind(null)})));"
                "assert(refused(()=>new ReadableStream({start:new Proxy(B.deadTask,{})})));"
                "let revokedStart=false;try{new ReadableStream({start:rev.proxy})}catch(e){revokedStart=e instanceof TypeError}"
                "assert(revokedStart);"
                "assert(refused(()=>new ReadableStream({start(c){c.enqueue('x')}},{size:B.deadTask})));"
                "for(let type of [undefined,'bytes']){let reader=new ReadableStream({type,pull:B.deadTask},{highWaterMark:0}).getReader();"
                "reader.closed.catch(()=>{});streamBad(type?'byte-pull':'pull',reader.read())}"
                "streamBad('cancel',new ReadableStream({cancel:B.deadTask}).cancel());",
                "retirement streams: readable callback refusal");
            realm_script(retirement_realms[0],
                "for(let method of ['write','close','abort']){"
                "let writer=new WritableStream({[method]:B.deadTask}).getWriter();writer.closed.catch(()=>{});"
                "streamBad(method,writer[method]('x'))}"
                "for(let method of ['transform','flush']){let ts=new TransformStream({[method]:B.deadTask});"
                "let reader=ts.readable.getReader(),writer=ts.writable.getWriter();reader.closed.catch(()=>{});"
                "reader.read().catch(()=>{});writer.closed.catch(()=>{});"
                "streamBad(method,method==='transform'?writer.write('x'):writer.close())}",
                "retirement streams: writable and transform callback refusal");
            realm_script(retirement_realms[0],
                "streamBad('transform-cancel',new TransformStream({cancel:B.deadTask}).readable.cancel());"
                "let sizedWriter=new WritableStream({},{size:B.deadTask}).getWriter();sizedWriter.closed.catch(()=>{});"
                "streamBad('write-size',sizedWriter.write('x'));"
                "let revokedReader=new ReadableStream({pull:rev.proxy},{highWaterMark:0}).getReader();"
                "revokedReader.closed.catch(()=>{});streamBad('revoked-pull',revokedReader.read());",
                "retirement streams: author callbacks refuse retired realms in synchronous and promise paths");
            realm_script(retirement_realms[0],
                "for(let [name,stream] of [['retained-pull',B.retainedPull],['retained-byte-pull',B.retainedBytePull]]){"
                "let reader=stream.getReader();reader.closed.catch(()=>{});streamBad(name,reader.read())}"
                "B.retainedLivePull.getReader().read().then(v=>globalThis.retainedLiveValue=v.value);",
                "retirement streams: retained algorithms gate author callbacks and admit live foreign callbacks");
            realm_script(retirement_realms[0],
                "assert(retainedLiveValue==='live');"
                "assert(streamResults.length===13,streamResults.join());"
                "assert(streamResults.includes('revoked-pull:TypeError'));"
                "assert(streamResults.filter(v=>v.endsWith(':InvalidStateError')).length===12,streamResults.join());"
                "assert(taskLog.join()==='micro,timer,timeout-idle,raf,idle');"
                "new B.ReadableStream({start:C.filterLive});assert(walkLog.join()==='C,C,C,C');",
                "retirement streams: callback refusal settles promises and live callback remains callable");
            realm_script(retirement_realms[0],
                "globalThis.pipeAborts=[];globalThis.pipeControllers=[new AbortController(),new B.AbortController()];"
                "for(let controller of pipeControllers){"
                "controller.signal.addEventListener('abort',B.deadTask);"
                "new B.ReadableStream().pipeTo(new B.WritableStream(),{signal:controller.signal,preventAbort:true,preventCancel:true})"
                ".then(()=>pipeAborts.push('unexpected'),e=>pipeAborts.push(e))}" ,
                "retirement streams: retained pipe receives active and retired signal abort algorithms");
            JSContext *collection_context = os64_js_context(runtime, OS64_JS_ABI_ID, &out);
            if (collection_context) {
                JS_RunGC(JS_GetRuntime(collection_context));
                JS_RunGC(JS_GetRuntime(collection_context));
            }
            realm_script(retirement_realms[0], "for(let controller of pipeControllers)controller.abort('stopped');",
                "retirement streams: pending pipe algorithms survive cycle collection");
            realm_script(retirement_realms[0],
                "assert(pipeAborts.join()==='stopped,stopped',pipeAborts.join());"
                "assert(taskLog.join()==='micro,timer,timeout-idle,raf,idle');",
                "retirement streams: abort settles internal pipes without delivering retired event callbacks");
            realm_script(retirement_realms[0],
                "globalThis.abortOrder=[];globalThis.orderController=new AbortController();"
                "orderController.signal.addEventListener('abort',e=>e.stopImmediatePropagation());"
                "for(let i=1;i<=3;i++){new B.ReadableStream({start(c){if(i===1)c.close()},cancel(){abortOrder.push(i)}})"
                ".pipeTo(new B.WritableStream(),{signal:orderController.signal}).catch(()=>{})}",
                "retirement streams: completed pipe removes its internal abort algorithm");
            realm_script(retirement_realms[0], "orderController.abort('ordered');",
                "retirement streams: abort algorithms precede stopped event delivery");
            realm_script(retirement_realms[0], "assert(abortOrder.join()==='2,3',abortOrder.join());",
                "retirement streams: removing first pipe preserves later algorithm order");
            realm_script(retirement_realms[0],
                "(()=>{let controller=new AbortController();globalThis.pipeWeak=new WeakRef(controller.signal);"
                "new B.ReadableStream().pipeTo(new B.WritableStream(),{signal:controller.signal}).catch(()=>{})})()",
                "retirement streams: unobserved pending pipe forms a collectible signal cycle");
            if (collection_context) {
                JS_RunGC(JS_GetRuntime(collection_context));
                JS_RunGC(JS_GetRuntime(collection_context));
            }
            realm_script(retirement_realms[0], "assert(pipeWeak.deref()===undefined);",
                "retirement streams: native abort holds are traced for cycle collection");
            realm_script(retirement_realms[0],
                "globalThis.retainedReads=[];B.retainedBlob.text().then(v=>retainedReads.push(v));"
                "B.retainedResponse.text().then(v=>retainedReads.push(v));",
                "retirement bodies: read settled data retained from B");
            realm_script(retirement_realms[0], "assert(retainedReads.includes('bytes')&&retainedReads.includes('settled'));",
                "retirement bodies: settled Blob and Response remain readable");
            realm_script(retirement_realms[0],
                "globalThis.retainedClone=B.retainedStreamResponse.clone();"
                "retainedClone.text().then(v=>retainedReads.push('clone:'+v));"
                "B.retainedStreamResponse.text().then(v=>retainedReads.push(v));",
                "retirement bodies: read retained closed stream response");
            realm_script(retirement_realms[0], "assert(retainedReads.includes('streamed')&&retainedReads.includes('clone:streamed'));",
                "retirement bodies: closed stream response algorithms survive creator retirement");
            realm_script(retirement_realms[0],
                "let cloned=B.retainedByteResponse.clone();cloned.text().then(v=>retainedReads.push('clone:'+v));"
                "B.retainedByteResponse.text().then(v=>retainedReads.push(v));",
                "retirement bodies: clone retained closed byte stream response");
            realm_script(retirement_realms[0],
                "assert(retainedReads.includes('byte-streamed')&&retainedReads.includes('clone:byte-streamed'));",
                "retirement bodies: byte stream tee algorithms survive creator retirement");
            realm_script(retirement_realms[0],
                "assert(B.plain()===42,'ordinary retained function remains callable');"
                "assert(B.adopted.ownerDocument===C.document&&B.adopted instanceof B.HTMLElement);"
                "assert(B.document.createElement('x-late') instanceof B.HTMLElement);"
                "customElements.define('x-dead-ctor',B.customElements.get('x-late'));"
                "assert(document.createElement('x-dead-ctor') instanceof HTMLElement);"
                "assert(upgrades.join()==='A,C','synchronous constructors remain gated: '+upgrades);"
                "assert(B.mo.takeRecords().length===0);B.mo.disconnect();"
                "B.observed.setAttribute('data-after','2');assert(B.observed.getAttribute('data-after')==='2');"
                "moLog=[];ceLog=[];observed.setAttribute('data-next','2');C.observed.setAttribute('data-next','2');"
                "B.adopted.setAttribute('data-pulse','again');",
                "retirement queues: dead deliveries suppressed and retained storage usable");
            realm_script(retirement_realms[0],
                "assert(moLog.join()==='A,C','later observer delivery: '+moLog);"
                "assert(ceLog.join()==='adopted','adopted live callback: '+ceLog);"
                "moLog=[];stopC=true;observed.setAttribute('data-last','3');C.observed.setAttribute('data-last','3');",
                "retirement queues: queues rearm and first callback retires later recipient");
            realm_script(retirement_realms[0],
                "assert(moLog.join()==='A','reentrant retirement: '+moLog);"
                "assert(C.mo.takeRecords().length===0);mo.disconnect();foreign.disconnect();"
                "bound.disconnect();proxied.disconnect();revmo.disconnect();C.mo.disconnect();",
                "retirement queues: per-record eligibility rechecked at delivery");
            const char *revoked_job = "globalThis.queuedRevoked=false;queueMicrotask(rev.proxy);queuedRevoked=true";
            check(os64_js_realm_run(retirement_realms[0], revoked_job, strlen(revoked_job),
                  "revoked-microtask.js", &out) == OS64_JS_EXCEPTION && strstr(out.message, "revoked") != NULL,
                  "retirement tasks: revoked microtask fails at execution");
            realm_script(retirement_realms[0], "assert(queuedRevoked);",
                "retirement tasks: revoked callback discovery does not throw while enqueueing");
        }
        os64_dom_cluster_drain(cluster);
        os64_js_destroy(runtime);
        os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations,
              "retirement queues: retained native backing released under both teardown policies");
    }
    retirement_reentry_cases();
}
