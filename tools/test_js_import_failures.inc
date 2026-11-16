/* Failure inventory starts after the closure and providers exist. Separate
 * sweeps cover base snapshot/enqueue and the later module job/settlement. */
static void import_failure_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned phase=0;phase<2;phase++) {
        size_t inventory=0;
        for(size_t cut=0;cut<=inventory;cut++) {
            ImportSettings f;if(!import_open(&f,policy,0)){import_close(&f);continue;}
            os64_js_outcome_t out;JSValue promise=JS_UNDEFINED;
            check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"import-allocation",&out)==OS64_JS_OK,
                  "import failure: task opened before injection");
            if(phase)check(os64_js_realm_with_settings(f.realm[0],f.realm[1],OS64_JS_ABI_ID,
                import_invoke,&f,&promise,&out)==OS64_JS_OK,"import failure: import queued before job sweep");
            size_t start=attempts;if(cut)fail_at=start+cut;
            os64_js_status_t status;
            if(!phase)status=os64_js_realm_with_settings(f.realm[0],f.realm[1],OS64_JS_ABI_ID,
                import_invoke,&f,&promise,&out);
            else status=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
            fail_at=0;if(!cut)inventory=attempts-start;
            /* The native resolver's NULL contract is a resolution miss; engine
             * allocation refusal instead latches the terminal host failure. */
            check(status==(f.resolve_refused?OS64_JS_UNHANDLED_REJECTION:cut?OS64_JS_HOST_FAILURE:OS64_JS_OK),
                  "import failure: resolver miss and engine allocation refusal retain their contracts");
            if(!phase)check(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==(cut?OS64_JS_HOST_FAILURE:OS64_JS_OK),
                           "import failure: queue drain preserves first allocation failure");
            JS_FreeValue(f.ctx[0],promise);import_close(&f);
            check(live==0,"import failure: queued strings and partial module graph release");
        }
        printf("Import allocation sweep: policy %d, phase %u, %zu cuts\n",policy,phase,inventory);
    }
}
