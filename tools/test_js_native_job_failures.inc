static void native_job_failure_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            NeutralPromise f;if(!neutral_open(&f,policy,true,true)){neutral_close(&f);continue;}
            os64_js_outcome_t out;
            check(realm_execute(f.realm[2],"cap=()=>{let a=[];for(let i=0;i<16;i++)a.push({value:i});globalThis.values=a}",&out)==OS64_JS_OK,
                  "native job failure: allocating author compiled before seam");
            JSValue fn=realm_property(f.ctx[2],"cap");
            check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"native-job-cut",&out)==OS64_JS_OK,
                  "native job failure: task admitted before allocation seam");
            size_t start=attempts;if(cut)fail_at=start+cut;
            int queued=JS_EnqueueJobNeutralSettings(f.ctx[3],native_settings_job,1,&fn);
            os64_js_status_t ended=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
            fail_at=0;if(!cut)inventory=attempts-start;
            check(cut?ended==OS64_JS_HOST_FAILURE:queued==0&&ended==OS64_JS_OK,
                  "native job failure: partial enqueue or execution stays terminal");
            JS_FreeValue(f.ctx[2],fn);neutral_close(&f);
            check(!live,"native job failure: queue argument and execution roots released");
        }
        printf("Native job allocation sweep: policy %d, %zu cuts\n",policy,inventory);
    }
}
