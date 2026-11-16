struct DValue;
struct DValue *d_find(os64_dom_t *, const os64_html_node_t *);
/* Private installation has no author/provider entry until publication. Native
 * allocation cuts must leave the pre-existing cluster usable. */
typedef struct {
    os64_js_runtime_t *runtime;
    os64_dom_cluster_t *cluster;
    os64_js_realm_t *parent;
    os64_dom_t *binding;
    os64_dom_options_t options;
    os64_dom_installation_t installation;
    DomFixture pending;
    os64_dom_child_t child;
    os64_js_outcome_t *active_outcome;
    size_t cut;
    unsigned providers;
} ChildInstallFixture;
static ChildInstallFixture *child_install_current;
static uint64_t child_install_ms(void *opaque)
{ ChildInstallFixture *f=opaque; f->providers++; return 2000; }
static uint64_t child_install_us(void *opaque)
{ ChildInstallFixture *f=opaque; f->providers++; return 2000000; }
static void child_install_timing(void *opaque, os64_dom_timing_t *timing)
{ ChildInstallFixture *f=opaque; f->providers++; *timing=f->installation.timing; }
static DomFixture child_install_document(void)
{
    os64_js_config_t config=fixture_config();
    DomFixture f=fixture_configured(0,0,&config);
    os64_js_destroy(f.runtime); f.runtime=NULL;
    return f;
}
static void child_install_unconsumed(DomFixture *f, os64_dom_child_t child)
{
    if (!child.consumed) { os64_page_state_free(f->state); os64_html_document_free(f->document); }
    f->state=NULL; f->document=NULL;
}
static bool child_install_open_capped(ChildInstallFixture *f, int policy, bool modern, size_t engine_cap)
{
    memset(f,0,sizeof(*f));
    os64_js_config_t config=fixture_config(); config.limits.memory_bytes=engine_cap;
    os64_js_outcome_t out;
    check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&f->runtime,&out)==OS64_JS_OK,
          "child install: runtime created");
    if (!f->runtime) return false;
    os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
    f->cluster=os64_dom_cluster_create(f->runtime,&limits,&out);
    f->parent=os64_js_primary_realm(f->runtime,OS64_JS_ABI_ID,&out);
    f->options=p2b_options("https://install.example/"); f->options.modern=modern;
    f->options.shadow_trees=true; f->options.ranges=true;
    DomFixture doc=child_install_document();
    os64_dom_admission_t a=os64_dom_create_in_realm(f->cluster,f->parent,doc.document,doc.state,&f->options,&out);
    f->binding=a.binding;
    if (!a.consumed) { os64_page_state_free(doc.state); os64_html_document_free(doc.document); }
    check(a.binding && a.consumed,"child install: parent installed");
    f->options.modern=true;
    f->options.now_ms=child_install_ms; f->options.now_us=child_install_us;
    f->options.timing=child_install_timing; f->options.host_opaque=f;
    f->installation.engine_headroom=OS64_DOM_CHILD_ENGINE_HEADROOM;
    f->installation.now_ms=2000; f->installation.now_us=2000000;
    f->installation.timing.origin_us=1000000;
    f->installation.timing.origin_epoch_us=1234000000;
    for (unsigned i=0;i<OS64_DOM_TIMING_COUNT;++i) f->installation.timing.at[i]=OS64_DOM_TIMING_NOT_YET;
    return f->binding!=NULL;
}
static bool child_install_open(ChildInstallFixture *f, int policy, bool modern)
{ return child_install_open_capped(f,policy,modern,64u<<20); }
static void child_install_close(ChildInstallFixture *f, int policy)
{
    os64_dom_cluster_drain(f->cluster);
    os64_js_teardown_report_t report;
    os64_js_destroy_report(f->runtime,&report);
    os64_dom_cluster_free(f->cluster);
    check(live==0 && dom_live==0 && !dom_allocations,"child install: native ownership released");
    check(policy==OS64_JS_TEARDOWN_RECLAIM || !report.leaked,"child install: engine ownership released");
}
static os64_dom_child_t child_install_attempt(ChildInstallFixture *f, os64_js_outcome_t *out)
{
    unsigned before=f->providers;
    if(f->cut) dom_fail_at=dom_attempts+f->cut;
    os64_dom_child_t child=os64_dom_create_child(f->cluster,f->pending.document,f->pending.state,
                                               &f->options,&f->installation,out);
    dom_fail_at=0;
    check(f->providers==before,"child install: bootstrap never calls external providers");
    return child;
}
static JSValue child_install_nested(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self; (void)argc; (void)argv; (void)ctx;
    os64_js_outcome_t out;
    ChildInstallFixture *f=child_install_current;
    if (f->active_outcome) {
        os64_js_outcome_t saved=*f->active_outcome;
        size_t before=dom_attempts;
        os64_dom_child_t alias=os64_dom_create_child(f->cluster,f->pending.document,f->pending.state,
                                                    &f->options,&f->installation,f->active_outcome);
        check(alias.state==OS64_DOM_CHILD_REFUSED && !alias.consumed && !alias.binding &&
              dom_attempts==before && !memcmp(&saved,f->active_outcome,sizeof(saved)),
              "child install: active outcome alias refused without clearing parent state");
    }
    f->child=child_install_attempt(f,&out);
    return JS_NewInt32(ctx,f->child.state);
}
static void child_install_publish(ChildInstallFixture *f, os64_dom_child_t child, const char *name)
{
    os64_js_outcome_t out;
    JSContext *ctx=os64_js_realm_context(f->parent,OS64_JS_ABI_ID,&out);
    JSContext *other=os64_js_realm_context(child.realm,OS64_JS_ABI_ID,&out);
    JSValue global=JS_GetGlobalObject(ctx);
    check(JS_SetPropertyStr(ctx,global,name,JS_GetGlobalObject(other))>=0,"child install: explicitly publish after commit");
    JS_FreeValue(ctx,global);
}
static void dom_child_install_cases(void)
{
    for (int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;++policy) {
        ChildInstallFixture f;
        if (!child_install_open(&f,policy,true)) { child_install_close(&f,policy); continue; }
        os64_js_outcome_t out;
        f.pending=child_install_document();
        size_t before=dom_attempts;
        f.installation.engine_headroom=SIZE_MAX;
        os64_dom_child_t refused=child_install_attempt(&f,&out);
        check(refused.state==OS64_DOM_CHILD_REFUSED && !refused.consumed && !refused.binding &&
              out.status==OS64_JS_ADMISSION_REFUSED && dom_attempts==before,
              "child install: headroom refusal leaves native ownership untouched");
        f.installation.engine_headroom=OS64_DOM_CHILD_ENGINE_HEADROOM;
        size_t cap=f.options.max_bytes; f.options.max_bytes=1;
        refused=child_install_attempt(&f,&out);
        check(refused.state==OS64_DOM_CHILD_REFUSED && !refused.consumed && dom_attempts==before,
              "child install: native cap refusal leaves ownership untouched");
        f.options.max_bytes=cap;
        os64_dom_child_t child=child_install_attempt(&f,&out);
        check(child.state==OS64_DOM_CHILD_ACTIVE && child.consumed && out.status==OS64_JS_OK,
              "child install: standalone transaction commits");
        child_install_unconsumed(&f.pending,child);
        if (child.state==OS64_DOM_CHILD_ACTIVE) {
            realm_script(child.realm,STORAGE_ASSERT "assert(performance.timeOrigin===1234000);"
                         "assert(performance.now()===1000);assert(document.URL==='https://install.example/');",
                         "child install: copied creation timing and normal provider delivery after commit");
            check(f.providers>0,"child install: committed child can use its providers");
            child_install_publish(&f,child,"C");
            realm_script(f.parent,STORAGE_ASSERT "globalThis.order=[];globalThis.adopted=C.document.createElement('i');"
                "document.body.appendChild(document.adoptNode(adopted));"
                "new MutationObserver(()=>order.push('observer')).observe(adopted,{attributes:true});",
                "child install: sibling adoption and observer ready");
        }
        JSContext *ctx=os64_js_realm_context(f.parent,OS64_JS_ABI_ID,&out);
        JSValue global=JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx,global,"installChild",JS_NewCFunction(ctx,child_install_nested,"installChild",0));
        JS_FreeValue(ctx,global);
        child_install_current=&f;
        f.options.timing=NULL;
        for (unsigned cut=0;cut<2;++cut) {
            f.pending=child_install_document(); f.cut=cut?10:0;
            f.child=(os64_dom_child_t){0};
            const char *script="order=[];Promise.resolve().then(()=>order.push('first'));"
                "var state=installChild();assert(state===" /* ACTIVE=1, FAILED=2 */
                "(globalThis.expectFailed?2:1),'installation state '+state);"
                "assert(order.length===0,'installer drained parent jobs');order.push('sync');"
                "Promise.resolve().then(()=>order.push('last'));";
            f.active_outcome=&out;
            check(os64_js_realm_run(f.parent,script,strlen(script),"nested-install.js",&out)==OS64_JS_OK,
                  "child install: nested setup preserves parent turn and pending jobs");
            f.active_outcome=NULL;
            if(f.child.state==OS64_DOM_CHILD_ACTIVE)
                realm_script(f.child.realm,STORAGE_ASSERT "assert(performance.timeOrigin===1234000);assert(performance.now()===1000);",
                             "child install: copied timing survives absent timing provider");
            child_install_unconsumed(&f.pending,f.child);
            realm_script(f.parent,"assert(order.join()==='sync,first,last',order.join());"
                "adopted.setAttribute('data-after','yes');globalThis.expectFailed=true;",
                "child install: parent jobs retain order and adopted node stays usable");
            if(child.state==OS64_DOM_CHILD_ACTIVE)
                realm_script(child.realm,"assert(document.body);","child install: pre-existing sibling remains live");
        }
        child_install_current=NULL;
        child_install_close(&f,policy);
    }
}

/* Each native bootstrap allocation is cut once; repeat with no modern parent
 * so newly created shared observer/reaction storage also rolls back. */
static void dom_child_install_sweeps(bool nested)
{
    /* CSS shorthand discovery allocates once per process. Warm that immutable
     * table before measuring repeatable installation cuts. A single-cut run
     * deliberately keeps the cold path for the isolated startup regression. */
    if (!getenv("DOM_CHILD_NATIVE_CUT")) {
        ChildInstallFixture warm;
        if (!child_install_open(&warm,OS64_JS_TEARDOWN_FATAL,false)) abort();
        warm.pending=child_install_document(); os64_js_outcome_t out;
        os64_dom_child_t child=child_install_attempt(&warm,&out);
        check(child.state==OS64_DOM_CHILD_ACTIVE,"child install: warm immutable CSS metadata");
        child_install_unconsumed(&warm.pending,child);
        child_install_close(&warm,OS64_JS_TEARDOWN_FATAL);
    }
    for (int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;++policy)
    for (unsigned modern=0;modern<2;++modern) {
        const char *selected_policy=getenv("DOM_CHILD_SWEEP_POLICY");
        const char *selected_modern=getenv("DOM_CHILD_SWEEP_MODERN");
        if ((selected_policy && atoi(selected_policy)!=policy) ||
            (selected_modern && atoi(selected_modern)!=(int)modern)) continue;
        size_t count=0;
        const char *only=getenv("DOM_CHILD_NATIVE_CUT");
        if (only) count=strtoull(only,NULL,10);
        for (size_t cut=only?count:0;cut<=count;++cut) {
            ChildInstallFixture f;
            if (!child_install_open(&f,policy,modern)) { child_install_close(&f,policy); return; }
            os64_js_outcome_t out;
            os64_js_realm_t *sibling=NULL;
            check(os64_js_realm_create(f.runtime,OS64_JS_ABI_ID,&sibling,&out)==OS64_JS_OK,
                  "child install: pre-existing sibling realm");
            DomFixture sibling_doc=child_install_document();
            os64_dom_options_t sibling_options=p2b_options("https://install.example/");
            sibling_options.modern=modern;
            sibling_options.shadow_trees=true; sibling_options.ranges=true;
            os64_dom_admission_t admitted=os64_dom_create_in_realm(f.cluster,sibling,sibling_doc.document,
                                                                  sibling_doc.state,&sibling_options,&out);
            if (!admitted.consumed) { os64_page_state_free(sibling_doc.state); os64_html_document_free(sibling_doc.document); }
            check(admitted.binding!=NULL,"child install: pre-existing sibling binding");
            if (!admitted.binding) abort();
            child_install_publish(&f,(os64_dom_child_t){.realm=sibling},"C");
            realm_script(f.parent,STORAGE_ASSERT "globalThis.order=[];globalThis.shared=C.document.createElement('i');"
                         "document.body.appendChild(shared);",
                         "child install: pre-existing adopted object");
            if (modern) realm_script(f.parent,
                "new MutationObserver(()=>order.push('A')).observe(shared,{attributes:true});"
                "new C.MutationObserver(()=>order.push('C')).observe(shared,{attributes:true});"
                "customElements.define('x-survives',class extends HTMLElement{static get observedAttributes(){return ['data-x']}"
                "attributeChangedCallback(){order.push('custom')}});"
                "globalThis.custom=document.createElement('x-survives');document.body.appendChild(custom);",
                "child install: pre-existing observer ordering and custom registry");
            f.pending=child_install_document(); f.cut=cut;
            size_t before=dom_attempts;
            os64_dom_child_t child;
            if (nested) {
                JSContext *ctx=os64_js_realm_context(f.parent,OS64_JS_ABI_ID,&out);
                JSValue global=JS_GetGlobalObject(ctx);
                check(JS_SetPropertyStr(ctx,global,"installChild",JS_NewCFunction(ctx,child_install_nested,"installChild",0))>=0,
                      "child install: nested sweep callback installed");
                JS_FreeValue(ctx,global);
                child_install_current=&f; f.active_outcome=&out;
                const char *script="globalThis.installationJobs=[];Promise.resolve().then(()=>installationJobs.push('first'));"
                    "installChild();assert(installationJobs.length===0);installationJobs.push('sync');"
                    "Promise.resolve().then(()=>installationJobs.push('last'));";
                check(os64_js_realm_run(f.parent,script,strlen(script),"nested-cut.js",&out)==OS64_JS_OK,
                      "child install: nested native cut preserves enclosing task outcome");
                f.active_outcome=NULL; child_install_current=NULL; child=f.child;
                realm_script(f.parent,"assert(installationJobs.join()==='sync,first,last',installationJobs.join());",
                             "child install: nested allocation cuts preserve parent job order");
            } else child=child_install_attempt(&f,&out);
            size_t made=dom_attempts-before;
            if (!cut) { check(child.state==OS64_DOM_CHILD_ACTIVE,"child install: measured bootstrap succeeds"); count=made; fprintf(stderr,"child install native sweep: policy %d modern %u nested %u allocations %zu\n",policy,modern,nested,count); }
            else {
                if (made<cut) fprintf(stderr,"unreached native cut %zu: made %zu state %d status %d message %s\n",cut,made,child.state,out.status,out.message);
                check(made>=cut,"child install: native allocation cut reached"); dom_sweeps++;
            }
            check(child.state!=OS64_DOM_CHILD_RUNTIME_FAILED,"child install: native refusal never poisons engine");
            check(child.state==OS64_DOM_CHILD_ACTIVE ||
                  (child.state==OS64_DOM_CHILD_REFUSED && !child.consumed && !child.binding) ||
                  (child.state==OS64_DOM_CHILD_FAILED && child.consumed && child.binding && os64_js_realm_is_retired(child.realm)),
                  "child install: ownership and eligibility match result");
            if (child.state==OS64_DOM_CHILD_FAILED) {
                check(d_find(f.binding,f.pending.document->document)==NULL,
                      "child install: failed candidate wrappers leave shared lookup");
                check(os64_dom_registry_count(child.binding)==0,
                      "child install: failed candidate releases retained engine values");
                memory_balanced(child.binding,"failed child installation");
            }
            child_install_unconsumed(&f.pending,child);
            realm_script(sibling,STORAGE_ASSERT "assert(document.body);document.body.dataset.live='yes';",
                         "child install: pre-existing sibling survives native cut");
            realm_script(f.parent,"shared.setAttribute('data-survived','yes');assert(shared.ownerDocument===document);",
                         "child install: adopted object survives native cut");
            if (modern) realm_script(f.parent,"assert(order.join()==='A,C',order.join());custom.setAttribute('data-x','1');"
                "assert(order.join()==='A,C,custom',order.join());",
                "child install: sibling observer order and custom reaction survive native cut");
            realm_script(f.parent,STORAGE_ASSERT "document.body.setAttribute('data-survived','yes');"
                "assert(document.body.getAttribute('data-survived')==='yes');",
                "child install: parent survives native cut");
            f.pending=child_install_document(); f.cut=0;
            os64_dom_child_t next=child_install_attempt(&f,&out);
            check(next.state==OS64_DOM_CHILD_ACTIVE,"child install: another child admitted after native cut");
            child_install_unconsumed(&f.pending,next);
            if (next.state==OS64_DOM_CHILD_ACTIVE) {
                realm_script(next.realm,STORAGE_ASSERT "globalThis.seen=0;new MutationObserver(()=>seen++).observe(document.body,{attributes:true});"
                    "document.body.setAttribute('data-live','yes');"
                    "customElements.define('x-alive',class extends HTMLElement{connectedCallback(){this.textContent='alive'}});"
                    "document.body.appendChild(document.createElement('x-alive'));",
                    "child install: successor observer and custom registries work");
                realm_script(next.realm,"assert(seen===1);assert(document.querySelector('x-alive').textContent==='alive');",
                    "child install: successor reactions delivered");
            }
            child_install_close(&f,policy);
            if (failures) { fprintf(stderr,"child install native sweep stopped at cut %zu\n",cut); return; }
        }
        fprintf(stderr,"child install native sweep completed: policy %d modern %u nested %u cuts %zu\n",
                policy,modern,nested,only?1:count);
    }
}

static void dom_child_install_engine_cases(void)
{
    for (int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;++policy) {
        ChildInstallFixture measured;
        if (!child_install_open(&measured,policy,true)) abort();
        size_t used=(64u<<20)-os64_js_heap_headroom(measured.runtime);
        size_t before=os64_js_heap_bytes(measured.runtime);
        size_t parent_temporary=os64_js_heap_peak(measured.runtime)-before;
        measured.pending=child_install_document(); os64_js_outcome_t out;
        os64_dom_child_t child=child_install_attempt(&measured,&out);
        check(child.state==OS64_DOM_CHILD_ACTIVE,"child install: engine peak measurement succeeds");
        size_t peak=os64_js_heap_peak(measured.runtime)-before;
        fprintf(stderr,"child install engine measurement: policy %d parent-used %zu parent-temporary %zu child-peak %zu child-retained %zu native %zu\n",
                policy,used,parent_temporary,peak,os64_js_heap_bytes(measured.runtime)-before,
                os64_dom_bytes(child.binding));
        check(peak<OS64_DOM_CHILD_ENGINE_HEADROOM,"child install: measured peak fits fixture reserve with sibling margin");
        child_install_unconsumed(&measured.pending,child);
        child_install_close(&measured,policy);
        size_t allowances[]={parent_temporary+65536,peak/2,peak-65536};
        for (unsigned i=0;i<3;++i) {
            if(allowances[i]<=parent_temporary || allowances[i]>=peak) continue;
            ChildInstallFixture f;
            if(!child_install_open_capped(&f,policy,true,used+allowances[i])) abort();
            f.pending=child_install_document();
            /* Deliberately lie about the reserve to force real engine failures
             * after the allocation-free admission check. */
            f.installation.engine_headroom=1;
            child=child_install_attempt(&f,&out);
            check(child.state==OS64_DOM_CHILD_RUNTIME_FAILED,"child install: engine limit remains a shared failure");
            check(out.status==OS64_JS_LIMIT || out.status==OS64_JS_HOST_FAILURE || out.status==OS64_JS_FAILED_RUNTIME,
                  "child install: original sticky engine outcome preserved");
            child_install_unconsumed(&f.pending,child);
            check(os64_js_realm_run(f.parent,"1",1,"after-engine-cut",&out)==OS64_JS_FAILED_RUNTIME,
                  "child install: shared failure refuses parent script");
            child_install_close(&f,policy);
        }
        ChildInstallFixture f;
        if(!child_install_open(&f,policy,true)) abort();
        f.pending=child_install_document();
        /* Binding header is the first target allocation; the next allocation
         * is the fresh engine context. This is actual host OOM, not a cap. */
        fail_at=attempts+2;
        child=child_install_attempt(&f,&out);
        fail_at=0;
        check(child.state==OS64_DOM_CHILD_RUNTIME_FAILED && !child.consumed,
              "child install: engine context host OOM preserves unconsumed backing and sticky failure");
        child_install_unconsumed(&f.pending,child);
        check(os64_js_realm_run(f.parent,"1",1,"after-engine-oom",&out)==OS64_JS_FAILED_RUNTIME,
              "child install: host OOM refuses parent script");
        child_install_close(&f,policy);
    }
}

static void dom_child_install_budget_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;++policy)
    for(unsigned modern=0;modern<2;++modern) {
        ChildInstallFixture f;
        if(!child_install_open(&f,policy,true)) abort();
        f.options.modern=modern;
        unsigned active=0,failed=0,refused=0;
        os64_js_outcome_t out;
        for(unsigned i=0;i<OS64_JS_MAX_REALMS;++i) {
            f.pending=child_install_document();
            f.cut=!modern && i%2?2:0;
            size_t before=os64_dom_cluster_bytes(f.cluster).dom_bytes;
            os64_dom_child_t child=child_install_attempt(&f,&out);
            child_install_unconsumed(&f.pending,child);
            if(child.state==OS64_DOM_CHILD_REFUSED) {
                check(!child.consumed && !child.binding,"child install: exhausted admission retains caller ownership");
                check(modern?out.status==OS64_JS_ADMISSION_REFUSED:out.status==OS64_JS_REALM_LIMIT,
                      "child install: measured engine reserve or retained slots refuses before exhaustion");
                refused++;
                break;
            }
            check(child.consumed && child.binding,"child install: admitted slot keeps native owner");
            if(child.state==OS64_DOM_CHILD_ACTIVE) {
                active++;
                check(os64_dom_depart(child.binding,&out)==OS64_JS_OK && os64_dom_depart_finish(child.binding,&out)==OS64_JS_OK,
                      "child install: repeated departure completes");
            } else { check(child.state==OS64_DOM_CHILD_FAILED,"child install: repeated native cut remains local"); failed++; }
            check(os64_dom_cluster_bytes(f.cluster).dom_bytes>before,"child install: departed and failed shells remain charged");
            check(os64_js_heap_peak(f.runtime)<(64u<<20),"child install: production heap cap is preserved");
        }
        check(refused==1,"child install: repeated admission eventually refuses locally");
        if(!modern) check(active+failed==OS64_JS_MAX_REALMS-1 && failed>0,
                          "child install: failed and departed realms both consume the 65-slot ledger");
        realm_script(f.parent,STORAGE_ASSERT "document.body.dataset.live='yes';assert(document.body.dataset.live==='yes');",
                     "child install: parent remains usable after admission exhaustion");
        fprintf(stderr,"child install budget: policy %d modern %u active %u failed %u headroom %zu native %zu peak %zu\n",
                policy,modern,active,failed,os64_js_heap_headroom(f.runtime),os64_dom_cluster_bytes(f.cluster).dom_bytes,
                os64_js_heap_peak(f.runtime));
        child_install_close(&f,policy);
    }
}
