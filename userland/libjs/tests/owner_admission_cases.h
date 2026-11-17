#ifndef JS_OWNER_ADMISSION_CASES_H
#define JS_OWNER_ADMISSION_CASES_H
#include "os64/js_engine.h"
#include "os64/js_retention.h"
#include "os64/fmt.h"
#include "quickjs.h"
#include "../runtime/owner_runtime.h"
typedef struct {
    os64_js_runtime_t *runtime;
    uint64_t incarnation;
    size_t charge;
    unsigned finalized;
    void (*check)(bool,const char *);
} OwnerAdmissionFinalizer;
static JSClassID owner_admission_class;
static void owner_admission_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;
    OwnerAdmissionFinalizer *s=JS_GetOpaque(value,owner_admission_class);
    if(!s)return;
    JSOwnerRuntimeMetadata m;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.incarnation==s->incarnation &&
        m.charged_bytes==s->charge,"owner admission: charged metadata survives engine finalizer unwind");
    s->finalized++;
}
static void owner_admission_cases(void (*check)(bool,const char *),
    void (*observe)(os64_js_runtime_t *,const JSOwnerRuntimeMetadata *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};
    config.limits.memory_bytes=8*1024*1024; config.limits.execution_ms=1000;
    JSOwnerLimits capacities={3,65536,2};
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *legacy=NULL,*runtime=NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,(os64_js_teardown_policy_t)policy,
            OS64_JS_ABI_ID,&legacy,&out)==OS64_JS_OK,"owner admission: legacy constructor remains available");
        JSOwnerRuntimeMetadata untouched={.incarnation=77};
        check(!js_owner_runtime_metadata(legacy,&untouched) && untouched.incarnation==77 &&
            !js_owner_runtime_metadata(NULL,&untouched),"owner admission: legacy has no selected arena or snapshot mutation");
        if(observe)observe(NULL,NULL);
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            capacities,&runtime,&out)==OS64_JS_OK,"owner admission: explicit private constructor admits bounded storage");
        JSOwnerRuntimeMetadata m;
        check(js_owner_runtime_metadata(runtime,&m) && !js_owner_runtime_metadata(runtime,NULL),
            "owner admission: private metadata is inspectable without exports");
        check(m.incarnation==js_owner_runtime_incarnation(runtime) && m.incarnation!=js_owner_runtime_incarnation(legacy) &&
            m.limits.owners==3 && m.limits.tickets==65536 && m.limits.loans==2,
            "owner admission: live incarnation and exact limits are bound to private table");
        check(m.charged_bytes>=m.requested_bytes && m.wrapper_bytes+m.charged_bytes+m.engine_limit==config.limits.memory_bytes &&
            !m.summary.owners && !m.summary.tickets && !m.summary.loans && !m.summary.unknown_escape,
            "owner admission: actual metadata charge partitions configured ceiling with wrapper and engine");
        if(observe)observe(runtime,&m);
        JSContext *context=os64_js_context(runtime,OS64_JS_ABI_ID,&out);
        owner_admission_class=os64_js_class_id(&owner_admission_class);
        JSClassDef definition={.class_name="OwnerAdmission",.finalizer=owner_admission_finalizer};
        check(context && JS_NewClass(JS_GetRuntime(context),owner_admission_class,&definition)==0,
            "owner admission: register unwind oracle");
        OwnerAdmissionFinalizer state={runtime,m.incarnation,m.charged_bytes,0,check};
        JSValue object=JS_NewObjectClass(context,owner_admission_class);
        check(!JS_IsException(object),"owner admission: allocate native unwind oracle");
        JS_SetOpaque(object,&state); JSValue global=JS_GetGlobalObject(context);
        check(JS_SetPropertyStr(context,global,"ownerAdmission",object)==1,"owner admission: root native unwind oracle");
        JS_FreeValue(context,global);
        JSMemoryUsage usage; JS_ComputeMemoryUsage(JS_GetRuntime(context),&usage);
        check((uint64_t)usage.malloc_size<m.engine_limit,"owner admission: engine has remaining allowance");
        size_t payload=m.engine_limit-(size_t)usage.malloc_size+16384;
        char script[96]; int n=os64_snprintf(script,sizeof script,"void new ArrayBuffer(%lu)",(unsigned long)payload);
        check(n>0 && (size_t)n<sizeof script,"owner admission: bounded pressure source");
        check(os64_js_run(legacy,script,(size_t)n,"legacy-pressure.js",&out)==OS64_JS_OK,
            "owner admission: legacy retains its original engine allowance");
        check(os64_js_run(runtime,script,(size_t)n,"owner-pressure.js",&out)==OS64_JS_LIMIT && out.limit==OS64_JS_LIMIT_MEMORY,
            "owner admission: metadata reduces actual engine admission, not just a reported number");
        JSOwnerRuntimeMetadata failed;
        check(js_owner_runtime_metadata(runtime,&failed) && failed.incarnation==m.incarnation && failed.charged_bytes==m.charged_bytes,
            "owner admission: engine refusal preserves identity and native metadata charge");
        os64_js_teardown_report_t report;
        os64_js_destroy_report(runtime,&report);
        check(!report.leaked && state.finalized==1,"owner admission: finalizer and explicit native cleanup balance");
        os64_js_destroy_report(legacy,&report);
        check(!report.leaked,"owner admission: legacy sibling teardown balances");
        if(m.charged_bytes>m.requested_bytes) {
            os64_js_config_t narrow=config;
            narrow.limits.memory_bytes=m.wrapper_bytes+m.requested_bytes+1;
            runtime=(void *)1;
            check(js_owner_runtime_create(&narrow,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
                capacities,&runtime,&out)==OS64_JS_LIMIT && !runtime && out.limit==OS64_JS_LIMIT_MEMORY,
                "owner admission: rounded host charge refuses before engine publication");
        }
        runtime=(void *)1;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){1,SIZE_MAX,1},&runtime,&out)==OS64_JS_BAD_ARGUMENT && !runtime,
            "owner admission: unrepresentable arena refuses without publication");
        runtime=(void *)1;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){1,0,1},&runtime,&out)==OS64_JS_BAD_ARGUMENT && !runtime,
            "owner admission: zero metadata capacity refuses without publication");
    }
}
#endif
