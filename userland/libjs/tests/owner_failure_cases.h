#ifndef JS_OWNER_FAILURE_CASES_H
#define JS_OWNER_FAILURE_CASES_H
#include "os64/js_callbacks.h"
#include "os64/str.h"
#include "quickjs.h"
#include "../runtime/owner_runtime.h"

typedef struct {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket;
    JSOwnerFailureSnapshot inside;
    void (*check)(bool,const char *);
    size_t (*attempts)(void);
    unsigned finalized;
} OwnerFailureCase;
static JSClassID owner_failure_class;
static bool owner_failure_id(JSOwnerId a,JSOwnerId b)
{ return a.incarnation==b.incarnation && a.serial==b.serial; }
static bool owner_failure_ref_equal(JSOwnerFailureRef a,JSOwnerFailureRef b)
{ return a.kind==b.kind && owner_failure_id(a.owner,b.owner); }
static void owner_failure_read(OwnerFailureCase *s,JSOwnerFailureSnapshot *out)
{
    size_t before=s->attempts?s->attempts():0;
    s->check(js_owner_runtime_observe_failure(s->runtime,OS64_JS_ABI_ID,out)==OS64_JS_OK,
        "checked failures: scalar observation succeeds without engine entry");
    if(s->attempts)s->check(s->attempts()==before,"checked failures: observation attempts no allocation");
}
static void owner_failure_original_same(void (*check)(bool,const char *),
    const JSOwnerFailureSnapshot *a,const JSOwnerFailureSnapshot *b)
{
    check(a->incarnation==b->incarnation && a->original.status==b->original.status &&
        a->original.limit==b->original.limit && a->original.host_error==b->original.host_error &&
        a->deadline==b->deadline && a->jobs_executed==b->jobs_executed && a->jobs_discarded==b->jobs_discarded &&
        a->realm_count==b->realm_count && a->scope_issues==b->scope_issues && a->turn_open==b->turn_open &&
        a->explicit_task==b->explicit_task && a->scopes_classified==b->scopes_classified &&
        a->owners_mapped==b->owners_mapped && owner_failure_ref_equal(a->task_owner,b->task_owner) &&
        owner_failure_ref_equal(a->job_execution,b->job_execution) &&
        owner_failure_ref_equal(a->job_callback,b->job_callback) && owner_failure_ref_equal(a->job_activity,b->job_activity),
        "checked failures: original scalar identity and failure facts remain frozen");
    for(size_t i=0;i<a->realm_count;i++)check(owner_failure_ref_equal(a->realms[i],b->realms[i]),
        "checked failures: original code candidate IDs remain frozen");
}
static void owner_failure_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;OwnerFailureCase *s=JS_GetOpaque(value,owner_failure_class);if(!s)return;
    JSOwnerFailureSnapshot out={.incarnation=91};
    s->check(js_owner_runtime_observe_failure(s->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_FAILED_RUNTIME && out.incarnation==91,
        "checked failures: destruction refuses observation without publishing output");s->finalized++;
}
static JSValue owner_failure_cancel(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;OwnerFailureCase *s=opaque;JSOwnerRuntimeMetadata m;
    owner_failure_class=os64_js_class_id(&owner_failure_class);
    JSClassDef definition={.class_name="FailureSnapshotCleanup",.finalizer=owner_failure_finalizer};
    s->check(JS_NewClass(JS_GetRuntime(context),owner_failure_class,&definition)==0,
        "checked failures: install synchronous root-finalizer observation oracle");
    JSValue value=JS_NewObjectClass(context,owner_failure_class),global=JS_GetGlobalObject(context);
    s->check(!JS_IsException(value),"checked failures: native finalizer object");JS_SetOpaque(value,s);
    s->check(JS_SetPropertyStr(context,global,"failureCleanup",value)==1,"checked failures: retain finalizer through root cleanup");JS_FreeValue(context,global);
    s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK,"checked failures: close admitting holder before terminal observation");
    os64_js_cancel(s->runtime);os64_js_outcome_t out;
    s->check(os64_js_check_budget(s->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_CANCELLED,
        "checked failures: native budget observes cancellation inside admitted turn");
    owner_failure_read(s,&s->inside);
    s->check(s->inside.original.status==OS64_JS_CANCELLED && s->inside.global.status==OS64_JS_CANCELLED &&
        s->inside.task_owner.kind==JS_OWNER_FAILURE_ASSIGNED && owner_failure_id(s->inside.task_owner.owner,s->ticket.owner) &&
        s->inside.turn_open && !s->inside.explicit_task && s->inside.deadline>0 && s->inside.owners_mapped &&
        !s->inside.scopes_classified && (s->inside.scope_issues&OS64_JS_FAILURE_NO_SCOPE),
        "checked failures: closed-holder task owner maps without inventing complete engine scopes");
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1 && !m.summary.unknown_escape,
        "checked failures: in-callback snapshot preserves loan and causes no legacy escape");
    return JS_UNDEFINED;
}
static void owner_failure_cases(void (*check)(bool,const char *),size_t (*attempts)(void))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};config.limits.memory_bytes=8*1024*1024;
    config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime=NULL,*ordinary=NULL;os64_js_outcome_t out;
        JSOwnerLimits limits={3,3,2};JSOwnerRuntimeMetadata before,selected;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,limits,&ordinary,&out)==OS64_JS_OK &&
            js_owner_runtime_metadata(ordinary,&before),"checked failures: ordinary selected metadata fixture");
        JSOwnerFailureSnapshot snapshot={.incarnation=91};
        check(js_owner_runtime_observe_failure(ordinary,OS64_JS_ABI_ID,&snapshot)==OS64_JS_ADMISSION_REFUSED && snapshot.incarnation==91,
            "checked failures: existing private constructor keeps diagnostics disabled");
        check(js_owner_runtime_create_with_failure_records(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            limits,&runtime,&out)==OS64_JS_OK && js_owner_runtime_metadata(runtime,&selected),
            "checked failures: explicit metadata and terminal-record selection succeeds");
        check(before.requested_bytes==selected.requested_bytes && before.charged_bytes==selected.charged_bytes &&
            before.wrapper_bytes==selected.wrapper_bytes && before.engine_limit==selected.engine_limit,
            "checked failures: diagnostic selection adds no arena or wrapper reservation");os64_js_destroy(ordinary);
        OwnerFailureCase s={.runtime=runtime,.check=check,.attempts=attempts};
        check(js_owner_runtime_observe_failure(NULL,OS64_JS_ABI_ID,&snapshot)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_observe_failure(runtime,NULL,&snapshot)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_observe_failure(runtime,OS64_JS_ABI_ID,NULL)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_observe_failure(runtime,"wrong ABI",&snapshot)==OS64_JS_ABI_MISMATCH && snapshot.incarnation==91,
            "checked failures: invalid observations preserve output");
        owner_failure_read(&s,&snapshot);
        check(snapshot.incarnation==selected.incarnation && snapshot.original.status==OS64_JS_OK && snapshot.global.status==OS64_JS_OK &&
            snapshot.task_owner.kind==JS_OWNER_FAILURE_NONE && !snapshot.task_owner.owner.serial &&
            snapshot.job_execution.kind==JS_OWNER_FAILURE_NONE && !snapshot.realm_count && snapshot.owners_mapped,
            "checked failures: healthy record has no invented owner references");
        JSOwnerTicket primary,child,retired;
        check(js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&child,&out)==OS64_JS_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&retired,&out)==OS64_JS_OK &&
            js_owner_runtime_retire(runtime,OS64_JS_ABI_ID,retired,&out)==OS64_JS_OK &&
            js_owner_runtime_close(runtime,retired)==JS_OWNER_OK,"checked failures: real child and retained retired owner fixture");
        s.ticket=child;
        check(js_owner_runtime_scoped_run(runtime,OS64_JS_ABI_ID,child,"checked failure",owner_failure_cancel,&s,&out)==OS64_JS_CANCELLED,
            "checked failures: snapshot does not change cancelled invocation result");
        owner_failure_read(&s,&snapshot);owner_failure_original_same(check,&s.inside,&snapshot);
        check(js_owner_runtime_metadata(runtime,&selected) && !selected.summary.loans && !selected.summary.unknown_escape,
            "checked failures: pointer-free observation neither leaks loans nor changes compatibility retention");
        os64_js_failure_record_t raw;
        check(os64_js_observe_failure(runtime,OS64_JS_ABI_ID,&raw)==OS64_JS_OK && raw.task_owner &&
            raw.original.status==snapshot.original.status && raw.deadline==snapshot.deadline &&
            raw.scope_issues==snapshot.scope_issues && raw.realm_count==snapshot.realm_count &&
            js_owner_runtime_metadata(runtime,&selected) && selected.summary.unknown_escape,
            "checked failures: legacy observation preserves pointer record and sticky retention");
        check(os64_js_realm_retire(raw.task_owner,OS64_JS_ABI_ID,&out)==OS64_JS_OK && os64_js_realm_is_retired(raw.task_owner),
            "checked failures: original legacy realm handle stays valid after holder close");
        owner_failure_read(&s,&snapshot);owner_failure_original_same(check,&s.inside,&snapshot);
        check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,"checked failures: close remaining host holder");
        JSOwnerFailureSnapshot saved=snapshot;os64_js_destroy(runtime);check(s.finalized==1,"checked failures: finalizer refuses observation during destruction");
        runtime=NULL;
        check(js_owner_runtime_create_with_failure_records(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            limits,&runtime,&out)==OS64_JS_OK,"checked failures: new actual runtime incarnation");
        OwnerFailureCase fresh={.runtime=runtime,.check=check,.attempts=attempts};owner_failure_read(&fresh,&snapshot);
        check(snapshot.incarnation!=saved.incarnation && saved.task_owner.owner.incarnation==saved.incarnation &&
            owner_failure_id(saved.task_owner.owner,child.owner) && js_owner_runtime_close(runtime,child)==JS_OWNER_FOREIGN,
            "checked failures: copied diagnostics survive teardown without rebinding to a new runtime");
        owner_failure_original_same(check,&saved,&s.inside);os64_js_destroy(runtime);
    }
}
#endif
