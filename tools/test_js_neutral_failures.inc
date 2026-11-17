static void neutral_promise_failure_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            NeutralPromise f;if(!neutral_open(&f,policy,true,true)){neutral_close(&f);continue;}
            os64_js_outcome_t out;
            check(realm_execute(f.realm[2],
                "cap=x=>{let a=[];for(let i=0;i<16;i++)a.push({value:x});globalThis.values=a}",&out)==OS64_JS_OK,
                "neutral failure: allocating custom capability compiled");
            check(realm_execute(f.realm[3],"globalThis.run=()=>{A.pending.then(null,null);resolve(73)}",&out)==OS64_JS_OK,
                  "neutral failure: registration and settlement compiled before injection");
            JSValue fn=realm_property(f.ctx[3],"run");
            check(os64_js_realm_task_begin(f.realm[3],OS64_JS_ABI_ID,"neutral-failure",&out)==OS64_JS_OK,
                  "neutral failure: task begins before injection");
            size_t start=attempts;if(cut)fail_at=start+cut;
            os64_js_status_t called=os64_js_realm_call(f.realm[3],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out);
            os64_js_status_t ended=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
            fail_at=0;if(!cut)inventory=attempts-start;
            check((!cut&&called==OS64_JS_OK&&ended==OS64_JS_OK)||(cut&&ended==OS64_JS_HOST_FAILURE),
                  "neutral failure: allocation refusal stays terminal through capability execution");
            JS_FreeValue(f.ctx[3],fn);neutral_close(&f);
            check(live==0,"neutral failure: partial reactions and capability values leave no allocation roots");
        }
        printf("Neutral Promise allocation sweep: policy %d, %zu cuts\n",policy,inventory);
    }
}
