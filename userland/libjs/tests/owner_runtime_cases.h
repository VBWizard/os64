#ifndef JS_OWNER_RUNTIME_CASES_H
#define JS_OWNER_RUNTIME_CASES_H
#include "os64/js_engine.h"
#include "quickjs.h"
#include "../runtime/owner_runtime.h"

typedef struct {
    os64_js_runtime_t *runtime;
    uint64_t incarnation;
    unsigned finalized;
    void (*check)(bool, const char *);
} OwnerRuntimeFinalizer;
static JSClassID owner_runtime_class;
static void owner_runtime_finalizer(JSRuntime *engine, JSValue value)
{
    (void)engine;
    OwnerRuntimeFinalizer *state=JS_GetOpaque(value,owner_runtime_class);
    if(!state)return;
    state->check(js_owner_runtime_incarnation(state->runtime)==state->incarnation,
        "runtime identity: original incarnation survives synchronous destroy finalizer");
    state->finalized++;
}
static void owner_runtime_cases(void (*check)(bool,const char *))
{
    check(js_owner_runtime_incarnation(NULL)==0,"runtime identity: null query is zero");
    os64_js_config_t config={.limits=os64_js_default_limits()};
    config.limits.memory_bytes=8*1024*1024;
    config.limits.execution_ms=1000;
    uint64_t previous=0;
    for(unsigned policy=0;policy<2;policy++)for(unsigned i=0;i<16;i++) {
        os64_js_outcome_t out; os64_js_runtime_t *a=NULL,*b=NULL;
        check(os64_js_create_with_teardown(&config,(os64_js_teardown_policy_t)policy,
            OS64_JS_ABI_ID,&a,&out)==OS64_JS_OK && a,"runtime identity: create actual first runtime");
        uint64_t first=js_owner_runtime_incarnation(a);
        check(first>previous,"runtime identity: fresh creation never rebinds previous lifetime");
        check(os64_js_create_with_teardown(&config,(os64_js_teardown_policy_t)policy,
            OS64_JS_ABI_ID,&b,&out)==OS64_JS_OK && b,"runtime identity: create simultaneous second runtime");
        uint64_t second=js_owner_runtime_incarnation(b);
        check(second>first && js_owner_runtime_incarnation(a)==first,
            "runtime identity: independent live runtimes retain distinct identities");
        JSContext *context=os64_js_context(a,OS64_JS_ABI_ID,&out);
        owner_runtime_class=os64_js_class_id(&owner_runtime_class);
        JSClassDef definition={.class_name="RuntimeIdentity",.finalizer=owner_runtime_finalizer};
        check(context && JS_NewClass(JS_GetRuntime(context),owner_runtime_class,&definition)==0,
            "runtime identity: install finalizer oracle");
        OwnerRuntimeFinalizer state={a,first,0,check};
        JSValue value=JS_NewObjectClass(context,owner_runtime_class);
        check(!JS_IsException(value),"runtime identity: allocate finalizer object");
        JS_SetOpaque(value,&state);
        JSValue global=JS_GetGlobalObject(context);
        check(JS_SetPropertyStr(context,global,"ownerIdentity",value)==1,
            "runtime identity: root native finalizer object");
        JS_FreeValue(context,global);
        check(os64_js_run(a,"globalThis.answer=42",20,"identity.js",&out)==OS64_JS_OK &&
            js_owner_runtime_incarnation(a)==first,"runtime identity: execution preserves identity");
        os64_js_cancel(a);
        check(js_owner_runtime_incarnation(a)==first,"runtime identity: cancellation does not replace identity");
        os64_js_teardown_report_t report;
        os64_js_destroy_report(a,&report);
        check(!report.leaked && state.finalized==1 && js_owner_runtime_incarnation(b)==second,
            "runtime identity: first teardown balances and preserves live sibling");
        os64_js_destroy_report(b,&report);
        check(!report.leaked,"runtime identity: second teardown balances");
        previous=second;
    }
}
#endif
