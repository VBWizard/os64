/* Standalone callback composition must not depend on grouped document ownership.
 * Sample early and late binding allocation refusals on the new constructor. */
static void dom_browser_jobs_cases(void)
{
    size_t allocations=0, refused=0;
    for(unsigned cut=0;cut<12;cut++) {
        DomFixture f=fixture_base(0,0,64u<<20);
        os64_dom_options_t options=os64_dom_default_options();
        options.modern=true; options.shadow_trees=true; options.ranges=true;
        options.url="https://browser.test/";
        options.now_ms=p2b_now_ms; options.now_us=p2b_now_us;
        dom_attempts=0;
        dom_fail_at=cut==0?0:cut<9?cut:allocations-(11-cut);
        os64_js_outcome_t out;
        f.binding=os64_dom_create_with_browser_jobs(f.runtime,f.document,f.state,&options,&out);
        if(cut==0)allocations=dom_attempts;
        dom_fail_at=0;
        if(cut) {
            if(!f.binding)refused++;
            check((f.binding!=NULL)==(out.status==OS64_JS_OK),
                  "browser DOM: sampled allocation result agrees with binding publication");
        }
        else {
            check(f.binding!=NULL,"browser DOM: composed standalone installation succeeds");
            if(f.binding) {
                const char *source="globalThis.seen=[];"
                  "let b=document.getElementById('box');"
                  "b.addEventListener('x',()=>seen.push('listener'));b.dispatchEvent(new Event('x'));"
                  "b.onclick=()=>seen.push('handler');b.dispatchEvent(new Event('click'));"
                  "new MutationObserver(()=>seen.push('observer')).observe(b,{attributes:true});b.setAttribute('data-x','1');"
                  "class C extends HTMLElement{connectedCallback(){seen.push('custom')}}customElements.define('x-job',C);"
                  "document.body.appendChild(document.createElement('x-job'));"
                  "document.createTreeWalker(b,NodeFilter.SHOW_ELEMENT,{acceptNode(){seen.push('filter');return NodeFilter.FILTER_ACCEPT}}).nextNode();"
                  "new ReadableStream({start(c){seen.push('stream');c.close()}});"
                  "queueMicrotask(()=>seen.push('micro'));setTimeout(()=>seen.push('timer'),0);";
                check(os64_js_run(f.runtime,source,strlen(source),"browser-dom",&out)==OS64_JS_OK,
                      "browser DOM: standalone callbacks capture and execute settings");
                check(os64_dom_timer_fire(f.binding,p2b_ms,&out),"browser DOM: timer delivery executes");
                source="for(const x of ['listener','handler','observer','custom','filter','stream','micro','timer'])"
                       "if(!seen.includes(x))throw Error(x)";
                check(os64_js_run(f.runtime,source,strlen(source),"browser-dom-check",&out)==OS64_JS_OK,
                      "browser DOM: callback families compose without a cluster");
            }
        }
        fixture_free(&f);
    }
    check(refused>=8,"browser DOM: mandatory early allocations refuse construction");
    printf("Browser DOM: sampled 11 of %zu binding allocation points\n",allocations);
}

static void dom_browser_jobs_budget_case(void)
{
    os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
    config.limits.jobs_per_turn=2;
    DomFixture f=fixture_configured(0,0,&config);
    os64_dom_options_t options=p2b_options("https://browser.test/");
    os64_js_outcome_t out;
    f.binding=os64_dom_create_with_browser_jobs(f.runtime,f.document,f.state,&options,&out);
    check(f.binding!=NULL,"browser DOM budget: binding created");
    if(f.binding) {
        const char *source="globalThis.cleaned=0;globalThis.reported=0;globalThis.followed=0;"
          "onerror=()=>{reported++;queueMicrotask(()=>followed++)};"
          "globalThis.reg=new FinalizationRegistry(()=>{cleaned++;queueMicrotask(()=>{});throw Error('cleanup')});"
          "let t={};t.self=t;reg.register(t,1);t=null";
        check(os64_js_run(f.runtime,source,strlen(source),"browser-budget",&out)==OS64_JS_OK,
              "browser DOM budget: cleanup and reporting configured");
        JSContext *ctx=os64_js_context(f.runtime,OS64_JS_ABI_ID,&out);
        JS_RunGC(JS_GetRuntime(ctx));JS_RunGC(JS_GetRuntime(ctx));
        check(os64_dom_finalization_task_fire(f.binding,&out)&&out.status==OS64_JS_LIMIT&&out.limit==OS64_JS_LIMIT_JOBS,
              "browser DOM budget: report microtask cannot renew cleanup budget");
        JSValue global=JS_GetGlobalObject(ctx);
        const char *names[]={"cleaned","reported","followed"};
        for(unsigned i=0;i<3;i++) {
            JSValue value=JS_GetPropertyStr(ctx,global,names[i]);int32_t n=-1;
            check(JS_ToInt32(ctx,&n,value)==0&&n==(i==2?0:1),
                  "browser DOM budget: cleanup and report run once before over-budget microtask refuses");
            JS_FreeValue(ctx,value);
        }
        JS_FreeValue(ctx,global);
    }
    fixture_free(&f);
}
