/* Trusted engine retirement exercises shared delivery without a WindowProxy
 * or a browser navigation. It does not stand in for DOM departure cleanup. */
static os64_js_realm_t *retirement_realms[3];
static os64_html_document_t *retirement_documents[3];

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

static void dom_retirement_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
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
                    "globalThis.plain=()=>42;",
                    "retirement queues: retained objects installed");
            }
            realm_script(retirement_realms[0], "globalThis.moLog=[];globalThis.ceLog=[];globalThis.upgrades=[];"
                "globalThis.slots=[];globalThis.stopC=false;"
                "globalThis.liveA=function(){moLog.push('A');if(stopC)retireC()};",
                "retirement queues: A callbacks installed");
            realm_script(retirement_realms[2], "globalThis.liveC=function(){A.moLog.push('C')};"
                "globalThis.reaction=function(){A.ceLog.push(this.id)};"
                "globalThis.liveEvent=function(){A.eventLog.push('C')};",
                "retirement queues: C callbacks installed");
            realm_script(retirement_realms[0], "globalThis.eventLog=[];globalThis.getters=0;",
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
                "B.eventNode.addEventListener('life',liveEvent);",
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
                "for(const w of [B,A,C])w.host.appendChild(w.document.createElement('i'));pulse();"
                "getterTarget.dispatchEvent(new Event('life'));"
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
        }
        os64_dom_cluster_drain(cluster);
        os64_js_destroy(runtime);
        os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations,
              "retirement queues: retained native backing released under both teardown policies");
    }
}
