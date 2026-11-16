/* Inject after fixture setup: construction, registration, then GC queue
 * allocation have separate inventories and terminal-outcome assertions. */
static void finalization_failure_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<3;mode++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            FinalizationSettings f;if(!finalization_open(&f,policy,0)){finalization_close(&f);continue;}
            os64_js_outcome_t out;
            const char *source=mode==0?
                "globalThis.construct=()=>{captureRecord();globalThis.registry=new D.FinalizationRegistry(C.cleanup)}":
                "globalThis.construct=()=>{captureRecord();globalThis.registry=new D.FinalizationRegistry(C.cleanup);"
                "let target={};target.self=target;registry.register(target,73)}";
            check(realm_execute(f.realm[0],source,&out)==OS64_JS_OK,"finalization failure: operation compiled");
            JSValue fn=realm_property(f.ctx[0],"construct");
            check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"cleanup-allocation",&out)==OS64_JS_OK,
                  "finalization failure: task begins before injection");
            if(mode==2) {
                check(os64_js_realm_call(f.realm[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out)==OS64_JS_OK,
                      "finalization failure: registry and cyclic weak target prepared");
                JS_RunGC(JS_GetRuntime(f.ctx[0]));
            }
            size_t start=attempts;if(cut)fail_at=start+cut;
            os64_js_status_t status=OS64_JS_OK;
            if(mode==2)JS_RunGC(JS_GetRuntime(f.ctx[0]));
            else status=os64_js_realm_call(f.realm[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out);
            fail_at=0;if(!cut)inventory=attempts-start;
            check(mode==2||status==(cut?OS64_JS_HOST_FAILURE:OS64_JS_OK),
                  "finalization failure: allocation refusal classified as terminal host failure");
            JS_FreeValue(f.ctx[0],fn);
            os64_js_status_t ended=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
            check(ended==(cut?OS64_JS_HOST_FAILURE:OS64_JS_OK),
                  "finalization failure: GC enqueue refusal propagates through task outcome");
            check(f.calls==(mode==2&&!cut?1u:0u),"finalization failure: no callback executes after allocation refusal");
            finalization_close(&f);
            check(live==0,"finalization failure: partial registry and queued capture roots release");
        }
        printf("Finalization allocation sweep: policy %d, mode %u, %zu cuts\n",policy,mode,inventory);
    }
}
