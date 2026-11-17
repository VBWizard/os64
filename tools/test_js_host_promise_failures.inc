static void host_promise_failure_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned phase=0;phase<2;phase++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            NeutralPromise f;if(!neutral_open(&f,policy,true,true)){neutral_close(&f);continue;}
            os64_js_outcome_t out;
            JSValue global=JS_GetGlobalObject(f.ctx[3]);
            check(JS_SetPropertyStr(f.ctx[3],global,"make",JS_NewCFunctionMagic(f.ctx[3],host_promise_make,"make",1,JS_CFUNC_generic_magic,1))>=0,
                  "host Promise failure: private factory installed");JS_FreeValue(f.ctx[3],global);
            check(realm_execute(f.realm[3],"globalThis.input=Promise.resolve({answer:73})",&out)==OS64_JS_OK,
                  "host Promise failure: source prepared before seam");
            check(realm_execute(f.realm[0],"globalThis.run=()=>{globalThis.output=D.make(D.input);output.then(()=>{},()=>{})}",&out)==OS64_JS_OK,
                  "host Promise failure: conversion compiled before seam");
            JSValue fn=realm_property(f.ctx[0],"run");
            check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"host-promise-cut",&out)==OS64_JS_OK,
                  "host Promise failure: task begins before allocation seam");
            size_t start=attempts;if(!phase&&cut)fail_at=start+cut;
            os64_js_status_t called=os64_js_realm_call(f.realm[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out);
            if(phase){start=attempts;if(cut)fail_at=start+cut;}
            os64_js_status_t ended=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
            fail_at=0;if(!cut)inventory=attempts-start;
            check(cut?ended==OS64_JS_HOST_FAILURE:called==OS64_JS_OK&&ended==OS64_JS_OK,
                  "host Promise failure: refusal at capability enqueue forwarding or species stays terminal");
            JS_FreeValue(f.ctx[0],fn);neutral_close(&f);
            check(!live,"host Promise failure: partial capability and forwarding roots released");
        }
        printf("Host Promise allocation sweep: policy %d phase %u, %zu cuts\n",policy,phase,inventory);
    }
}
