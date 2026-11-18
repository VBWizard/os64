/* Actual Yonder task selection, with explicit native construction opt-in.
 * GC is forced through the fixture's retained runtime, not a page API. */
static void browser_jobs_collect(void)
{
    os64_js_outcome_t out;
    JSContext *ctx=os64_js_context(teardown_fixture_runtime,OS64_JS_ABI_ID,&out);
    check(ctx!=NULL,"browser jobs: fixture runtime accessible");
    if(ctx) { JS_RunGC(JS_GetRuntime(ctx)); JS_RunGC(JS_GetRuntime(ctx)); }
}
static bool browser_jobs_task(int mode)
{
    return mode==1?scripts_task(g.page.scripts,page_doc(&g.page),true):script_turn();
}
static void browser_jobs_window_cases(void)
{
    for(int mode=0;mode<3;mode++) {
        fixture_browser_jobs=true;
        loop_page("<p id=out></p><script>var log=[];var errors=0;"
          "function note(s){log.push(s);document.getElementById('out').textContent=log.join(',')}"
          "onerror=()=>{errors++;note('error')};"
          "var reg=new FinalizationRegistry(x=>{note('cleanup');queueMicrotask(()=>note('micro'));"
          "if(globalThis.failCleanup)throw Error('cleanup-failure')});"
          "for(let i=0;i<3;i++){let t={};t.self=t;reg.register(t,i)}"
          "function ordinary(){note('timer');setTimeout(ordinary,0)}"
          "</script>");
        check(loop_settle(),"browser jobs: opted-in page installs and loads");
        os64_js_outcome_t out;
        check(yonder_scripts_alive(g.page.scripts),"browser jobs: standalone settings installation succeeds");
        if(!yonder_scripts_alive(g.page.scripts)) { fprintf(stderr,"browser jobs install: %s\n",window_record_text()); loop_drop();fixture_browser_jobs=false;continue; }
        check(os64_js_run(teardown_fixture_runtime,mode==1?"failCleanup=true;ordinary()":
              mode==2?"failCleanup=true":"ordinary()",
              mode==1?strlen("failCleanup=true;ordinary()"):mode==2?strlen("failCleanup=true"):strlen("ordinary()"),
              "fixture-setup",&out)==OS64_JS_OK,"browser jobs: ordinary source prepared");
        browser_jobs_collect();
        check(yonder_scripts_finalization_pending(g.page.scripts)&&scripts_owed(g.page.scripts),
              "browser jobs: cleanup alone is visible to the wake predicate");
        for(int i=0;i<3;i++) {
            check(browser_jobs_task(mode),"browser jobs: one cleanup selected");
            check(script_rebuild(),"browser jobs: cleanup DOM changes rendered");
            check(strstr(probe_text_of("out"),"cleanup,micro")!=NULL || mode!=0,
                  "browser jobs: cleanup checkpoint runs its microtasks");
            if(mode!=2 && i<2) {
                unsigned before=s_cost.timers;
                check(browser_jobs_task(mode)&&s_cost.timers==before+1,
                      "browser jobs: ordinary timer progresses between cleanup tasks");
            }
        }
        check(!yonder_scripts_finalization_pending(g.page.scripts),"browser jobs: exactly three cells consumed");
        check(yonder_scripts_alive(g.page.scripts),"browser jobs: ordinary cleanup errors do not retire host");
        const char *verify=mode ?
          "if(log.filter(x=>x==='cleanup').length!==3||log.filter(x=>x==='micro').length!==3||errors!==3)throw Error('cleanup counts:'+JSON.stringify([log,errors]))" :
          "if(log.filter(x=>x==='cleanup').length!==3||log.filter(x=>x==='micro').length!==3||errors!==0)throw Error('cleanup counts:'+JSON.stringify([log,errors]))";
        os64_js_status_t verdict=os64_js_run(teardown_fixture_runtime,verify,strlen(verify),"verify",&out);
        if(verdict!=OS64_JS_OK)fprintf(stderr,"browser jobs mode %d: %s\n",mode,out.message);
        check(verdict==OS64_JS_OK,
              "browser jobs: cleanup, checkpoint and registry error delivery occur exactly once per cell");
        if(mode)check(strstr(window_record_text(),"cleanup-failure")!=NULL,
                      "browser jobs: cleanup error reaches browser diagnostic");
        loop_drop();fixture_browser_jobs=false;
    }
    fixture_browser_jobs=true;
    loop_page("<p id=out>quiet</p><script>onerror=()=>document.getElementById('out').textContent='wrong';"
      "class P extends Promise{static get [Symbol.species](){return function(e){e(()=>{throw Error('neutral-report')},()=>{})}}}"
      "new P(r=>r(1)).then();</script>");
    check(loop_settle()&&script_rebuild(),"browser jobs: neutral failure completes browser task");
    check(probe_text_is("out","quiet")&&strstr(window_record_text(),"neutral-report"),
          "browser jobs: neutral error is diagnosed without an invented window event");
    loop_drop();fixture_browser_jobs=false;
}
