static void promise_failure_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<3;mode++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            PromiseSettings f;if(!promise_settings_open(&f,policy,true)){promise_settings_close(&f);continue;}
            os64_js_outcome_t out;
            const char *source=mode==2
                ?"globalThis.register=()=>{globalThis.result=Promise.resolve({then:B.handler,tag:'thenable'})}"
                :mode==1
                ?"globalThis.register=()=>{globalThis.result=D.pending.then(B.handler,B.handler);D.resolve(73)}"
                :"globalThis.register=()=>{globalThis.result=D.pending.then(B.handler,B.handler)}";
            check(realm_execute(f.realm[3],"globalThis.pending=new Promise(r=>{globalThis.resolve=r})",&out)==OS64_JS_OK,
                  "Promise failure: pending fixture initialized");
            check(realm_execute(f.realm[0],source,&out)==OS64_JS_OK,"Promise failure: operation compiled before injection");
            f.thenable=mode==2;
            JSValue fn=realm_property(f.ctx[0],"register");
            check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"promise-failure",&out)==OS64_JS_OK,
                  "Promise failure: task begins before injection");
            size_t start=attempts;
            if(cut)fail_at=start+cut;
            os64_js_status_t status=os64_js_realm_call(f.realm[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out);
            fail_at=0;
            if(!cut)inventory=attempts-start;
            check(status==(cut?OS64_JS_HOST_FAILURE:OS64_JS_OK),
                  "Promise failure: allocation refusal remains terminal and classified");
            JS_FreeValue(f.ctx[0],fn);
            (void)os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
            JS_RunGC(JS_GetRuntime(f.ctx[0]));
            promise_settings_close(&f);
            check(live==0,"Promise failure: partial captures jobs and cycles leave no allocation roots");
        }
        printf("Promise allocation sweep: policy %d, mode %u, %zu cuts\n",policy,mode,inventory);
    }
}
