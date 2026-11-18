#ifndef JS_OWNER_TASK_CASES_H
#define JS_OWNER_TASK_CASES_H
#include "os64/js_callbacks.h"
#include "os64/str.h"
#include "../runtime/owner_runtime.h"

typedef struct OwnerTaskCase OwnerTaskCase;
typedef struct {OwnerTaskCase *state;unsigned role;} OwnerTaskRoot;
struct OwnerTaskCase {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket,promoted,child;
    JSOwnerRuntimeScope *scope;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    const char *source;
    JSContext *exported;
    OwnerTaskRoot root[2];
    unsigned mode,calls,jobs,child_jobs,returned_freed,job_freed;
    bool in_scope;
};
static JSClassID owner_task_class;
static OwnerTaskCase *owner_task_current;
static void owner_task_loan(OwnerTaskCase *s,const char *message)
{
    JSOwnerRuntimeMetadata m;JSOwnerId id;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1 &&
        js_owner_scope_owner(s->scope,&id)==JS_OWNER_OK && id.serial==s->ticket.owner.serial &&
        id.incarnation==s->ticket.owner.incarnation,message);
}
static void owner_task_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;OwnerTaskRoot *root=JS_GetOpaque(value,owner_task_class);if(!root)return;
    OwnerTaskCase *s=root->state;
    if(s->in_scope) {
        owner_task_loan(s,"checked tasks: result and job-argument finalizers retain original loan");
        os64_js_outcome_t nested;
        s->check(js_owner_runtime_task_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"finalizer",NULL,s,&nested)==OS64_JS_BUSY &&
            os64_js_task_end(s->runtime,OS64_JS_ABI_ID,&nested)==OS64_JS_BUSY,
            "checked tasks: finalizer runs under task driver guard");
        if(root->role==1)s->check(!s->jobs,"checked tasks: returned-value cleanup precedes checkpoint execution");
    }
    if(root->role==1)s->returned_freed++;else s->job_freed++;
}
static JSValue owner_task_value(JSContext *context,OwnerTaskCase *s,unsigned role)
{
    owner_task_class=os64_js_class_id(&owner_task_class);
    JSClassDef definition={.class_name="CheckedTaskRoot",.finalizer=owner_task_finalizer};
    if(!JS_IsRegisteredClass(JS_GetRuntime(context),owner_task_class) &&
        JS_NewClass(JS_GetRuntime(context),owner_task_class,&definition)<0)return JS_EXCEPTION;
    JSValue value=JS_NewObjectClass(context,owner_task_class);
    if(!JS_IsException(value)) {s->root[role-1]=(OwnerTaskRoot){s,role};JS_SetOpaque(value,&s->root[role-1]);}
    return value;
}
static JSValue owner_task_job(JSContext *context,int argc,JSValueConst *argv)
{
    (void)context;OwnerTaskCase *s=owner_task_current;
    owner_task_loan(s,"checked tasks: checkpoint job retains admitting owner after holder close");
    if(argc)s->check(JS_GetOpaque(argv[0],owner_task_class)==&s->root[1],"checked tasks: job owns actual native argument");
    s->jobs++;return JS_UNDEFINED;
}
static JSValue owner_task_throw_job(JSContext *context,int argc,JSValueConst *argv)
{(void)argc;(void)argv;return JS_ThrowTypeError(context,"job-first");}
static JSValue owner_task_error_string(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;
    owner_task_loan(owner_task_current,"checked tasks: body exception coercion retains original loan");
    return JS_NewString(context,"body-first");
}
static JSValue owner_task_child_job(JSContext *context,int argc,JSValueConst *argv)
{(void)context;(void)argc;(void)argv;owner_task_current->child_jobs++;return JS_UNDEFINED;}
static os64_js_status_t owner_task_child_setup(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerTaskCase *s=opaque;JSOwnerRuntimeMetadata m;JSOwnerId id;
    s->check(js_owner_scope_owner(scope,&id)==JS_OWNER_OK && id.serial==s->child.owner.serial &&
        js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==2,
        "checked tasks: nested setup holds distinct child and task loans");
    return JS_EnqueueJobOwned(context,context,context,owner_task_child_job,0,NULL)<0?OS64_JS_EXCEPTION:OS64_JS_OK;
}
static JSValue owner_task_body(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerTaskCase *s=opaque;s->scope=scope;s->in_scope=true;s->calls++;
    owner_task_loan(s,"checked tasks: native task borrows original mapped owner's context");
    os64_js_outcome_t saved=*s->outer,nested;
    s->check(js_owner_runtime_task_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"recursive",owner_task_body,s,s->outer)==OS64_JS_BUSY &&
        !os64_memcmp(&saved,s->outer,sizeof saved) &&
        js_owner_runtime_task_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"recursive",owner_task_body,s,&nested)==OS64_JS_BUSY,
        "checked tasks: recursive task protects outcome and cannot renew budget");
    if(s->mode==1) {
        s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK &&
            js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK,
            "checked tasks: close preserves promotion from whole-task loan");
    }
    if(s->mode==5) {
        const char *code="globalThis.taskDone=0;globalThis.taskFR=new FinalizationRegistry(()=>taskDone++);(()=>{let target={};taskFR.register(target,1)})()";
        JSValue value=JS_Eval(context,code,os64_strlen(code),"deferred-task.js",JS_EVAL_TYPE_GLOBAL);
        if(!JS_IsException(value))JS_RunGC(JS_GetRuntime(context));
        return value;
    }
    if(s->mode==8) {
        s->check(js_owner_scope_export(scope,&s->exported)==JS_OWNER_OK && s->exported==context,
            "checked tasks: explicit export marks compatibility retention");return JS_UNDEFINED;
    }
    if(s->mode==9) {
        s->check(os64_js_context(s->runtime,OS64_JS_ABI_ID,&nested)!=NULL,
            "checked tasks: incidental raw getter has its legacy lifetime");return JS_UNDEFINED;
    }
    if(s->mode==10) {
        s->check(js_owner_runtime_setup(s->runtime,OS64_JS_ABI_ID,s->child,owner_task_child_setup,s,&nested)==OS64_JS_OK &&
            nested.jobs_executed==1 && s->child_jobs==1,"checked tasks: nested installation consumes original task job budget");
    }
    if(s->mode==0)return s->source?JS_Eval(context,s->source,os64_strlen(s->source),"native-task.js",JS_EVAL_TYPE_GLOBAL):JS_UNDEFINED;
    JSValue result=owner_task_value(context,s,1);
    if(JS_IsException(result))return result;
    if(s->mode==6){os64_js_cancel(s->runtime);return result;}
    if(s->mode==11) {
        JSValue functions[2],promise=JS_NewPromiseCapability(context,functions);
        if(JS_IsException(promise)){JS_FreeValue(context,result);return promise;}
        JSValue reason=owner_task_value(context,s,2);
        JSValue rejected=JS_IsException(reason)?JS_EXCEPTION:JS_Call(context,functions[1],JS_UNDEFINED,1,&reason);
        JS_FreeValue(context,reason);JS_FreeValue(context,promise);JS_FreeValue(context,functions[0]);JS_FreeValue(context,functions[1]);
        if(JS_IsException(rejected)){JS_FreeValue(context,result);return rejected;}
        JS_FreeValue(context,rejected);return result;
    }
    if(s->mode==7)s->check(js_owner_runtime_retire(s->runtime,OS64_JS_ABI_ID,s->ticket,&nested)==OS64_JS_OK,
        "checked tasks: retirement changes eligibility without ending task loan");
    JSValue argument=owner_task_value(context,s,2);
    if(JS_IsException(argument)){JS_FreeValue(context,result);return argument;}
    int enqueued=JS_EnqueueJobOwned(context,context,context,owner_task_job,1,&argument);
    JS_FreeValue(context,argument);
    if(enqueued<0){JS_FreeValue(context,result);return JS_EXCEPTION;}
    if(s->mode!=4 && s->mode!=7 && s->mode!=10 && JS_EnqueueJobOwned(context,context,context,owner_task_throw_job,0,NULL)<0) {
        JS_FreeValue(context,result);return JS_EXCEPTION;
    }
    if(JS_EnqueueJobOwned(context,context,context,owner_task_job,0,NULL)<0){JS_FreeValue(context,result);return JS_EXCEPTION;}
    if(s->mode==2) {
        if(JS_SetPropertyStr(context,result,"toString",JS_NewCFunction(context,owner_task_error_string,"toString",0))<0){JS_FreeValue(context,result);return JS_EXCEPTION;}
        return JS_Throw(context,result);
    }
    return result;
}
static os64_js_status_t owner_task_invoke(OwnerTaskCase *s)
{
    owner_task_current=s;
    os64_js_status_t result=js_owner_runtime_task_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"checked task",owner_task_body,s,s->outer);
    s->scope=NULL;s->in_scope=false;owner_task_current=NULL;return result;
}
static OwnerTaskCase owner_task_state(os64_js_runtime_t *runtime,JSOwnerTicket ticket,
    os64_js_outcome_t *out,void (*check)(bool,const char *))
{return (OwnerTaskCase){.runtime=runtime,.ticket=ticket,.outer=out,.check=check};}
static void owner_task_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};config.limits.memory_bytes=8*1024*1024;config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime;os64_js_outcome_t out;JSOwnerTicket primary,a,b,staged;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){4,5,2},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&a,&out)==OS64_JS_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&b,&out)==OS64_JS_OK,
            "checked tasks: distinct primary and child owner fixture");
        if(policy)check(os64_js_enable_host_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
            "checked tasks: select existing explicit settings without raw export");
        OwnerTaskCase s=owner_task_state(runtime,a,&out,check);s.source="globalThis.taskLabel='A'";
        check(owner_task_invoke(&s)==OS64_JS_OK,"checked tasks: child A writes its mapped global");
        s.ticket=b;s.source="globalThis.taskLabel='B'";check(owner_task_invoke(&s)==OS64_JS_OK,"checked tasks: child B writes independent global");
        s.ticket=a;s.source="if(taskLabel!=='A')throw Error('wrong task context')";
        check(owner_task_invoke(&s)==OS64_JS_OK,"checked tasks: original owner mapping survives sibling tasks");
        unsigned calls=s.calls;
        check(js_owner_runtime_task_run(runtime,"bad ABI",a,"bad",owner_task_body,&s,&out)==OS64_JS_ABI_MISMATCH &&
            js_owner_runtime_task_run(runtime,OS64_JS_ABI_ID,a,NULL,owner_task_body,&s,&out)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_task_run(runtime,OS64_JS_ABI_ID,a,"bad",NULL,&s,&out)==OS64_JS_BAD_ARGUMENT && s.calls==calls,
            "checked tasks: malformed task refusal invokes no author callback");
        check(os64_js_task_begin(runtime,OS64_JS_ABI_ID,"existing task",&out)==OS64_JS_OK &&
            owner_task_invoke(&s)==OS64_JS_BUSY && s.calls==calls && os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
            "checked tasks: open-task refusal preserves existing host task");
        s.mode=1;s.source=NULL;
        check(owner_task_invoke(&s)==OS64_JS_EXCEPTION && s.promoted.serial && s.jobs==2 && out.jobs_executed==3 &&
            s.returned_freed==1 && s.job_freed==1 && !out.jobs_pending,
            "checked tasks: loan spans close result cleanup and all checkpoint jobs after a job exception");
        a=s.promoted;s=owner_task_state(runtime,a,&out,check);s.mode=2;
        check(owner_task_invoke(&s)==OS64_JS_EXCEPTION && s.jobs==2 && s.returned_freed==1 && s.job_freed==1 &&
            os64_streq(out.message,"body-first") && out.jobs_executed==3 && !out.jobs_pending,
            "checked tasks: body exception stays first while checkpoint continues after later job error");
        JSOwnerRuntimeMetadata m;
        check(js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
            "checked tasks: completed native task leaves no loan or implicit raw export");
        for(unsigned i=0;i<8;i++){s=owner_task_state(runtime,a,&out,check);check(owner_task_invoke(&s)==OS64_JS_OK,
            "checked tasks: repeated task closure leaves no open task");}
        check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&staged,&out)==OS64_JS_OK,"checked tasks: staged refusal fixture");
        s.ticket=staged;calls=s.calls;check(owner_task_invoke(&s)==OS64_JS_REALM_INSTALLING && s.calls==calls,
            "checked tasks: staged ticket cannot begin executable task");
        check(js_owner_runtime_retire(runtime,OS64_JS_ABI_ID,b,&out)==OS64_JS_OK,"checked tasks: retire mapped sibling");
        s.ticket=b;check(owner_task_invoke(&s)==OS64_JS_REALM_RETIRED && s.calls==calls,
            "checked tasks: retired ticket cannot begin native task");
        check(js_owner_runtime_close(runtime,a)==JS_OWNER_OK,"checked tasks: close actual promoted holder");s.ticket=a;
        check(owner_task_invoke(&s)==OS64_JS_ADMISSION_REFUSED && s.calls==calls,
            "checked tasks: stale holder cannot use retained realm");os64_js_destroy(runtime);

        for(unsigned mode=4;mode<=11;mode++) {
            config.limits.jobs_per_turn=(mode==4?1:mode==10?2:os64_js_default_limits().jobs_per_turn);
            check(js_owner_runtime_create_with_failure_records(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
                (JSOwnerLimits){2,3,2},&runtime,&out)==OS64_JS_OK && js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK,
                "checked tasks: task termination and lane fixture");
            s=owner_task_state(runtime,primary,&out,check);s.mode=mode;
            if(mode==5)check(os64_js_enable_finalization_tasks(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                "checked tasks: opt into deferred finalization lane");
            if(mode==10)check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&s.child,&out)==OS64_JS_OK,
                "checked tasks: stage nested installation candidate");
            os64_js_status_t result=owner_task_invoke(&s);
            if(mode==4 || mode==10) {
                JSOwnerFailureSnapshot f;
                check(result==OS64_JS_LIMIT && out.limit==OS64_JS_LIMIT_JOBS && s.jobs==1 &&
                    out.jobs_executed==(mode==4?1u:2u) && js_owner_runtime_observe_failure(runtime,OS64_JS_ABI_ID,&f)==OS64_JS_OK &&
                    f.task_owner.owner.serial==primary.owner.serial && f.explicit_task,
                    "checked tasks: nested setup and checkpoint share original owner and bounded job counter");
                check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_FAILED_RUNTIME,
                    "checked tasks: limited task is already closed for terminal host cleanup");
            } else if(mode==5) {
                bool begun=false;
                check(result==OS64_JS_OK && os64_js_finalization_task_pending(runtime,OS64_JS_ABI_ID),
                    "checked tasks: ordinary checkpoint preserves deferred cleanup task");
                s.mode=0;s.source="if(taskDone!==0)throw Error('early deferred task')";
                check(owner_task_invoke(&s)==OS64_JS_OK && os64_js_finalization_task_begin(runtime,OS64_JS_ABI_ID,&begun,&out)==OS64_JS_OK && begun &&
                    os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                    "checked tasks: host selects deferred task after scoped task closure");
                s.source="if(taskDone!==1)throw Error('deferred task lost')";
                check(owner_task_invoke(&s)==OS64_JS_OK,"checked tasks: deferred callback needs no expired native scope");
            } else if(mode==6) {
                check(result==OS64_JS_CANCELLED && s.returned_freed==1 &&
                    os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_FAILED_RUNTIME,
                    "checked tasks: cancellation cleans native result and closes owned task");
            } else if(mode==11) {
                check(result==OS64_JS_UNHANDLED_REJECTION && s.returned_freed==1 && s.job_freed==1 && !out.jobs_pending &&
                    js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
                    "checked tasks: unhosted rejection roots release during final leave under original loan");
            } else if(mode==7) {
                check(result==OS64_JS_OK && !s.jobs && out.jobs_discarded==2 && s.returned_freed==1 && s.job_freed==1,
                    "checked tasks: retirement discards ineligible jobs under original loan");
            } else {
                check(result==OS64_JS_OK && js_owner_runtime_metadata(runtime,&m) && m.summary.unknown_escape && !m.summary.loans,
                    "checked tasks: explicit and incidental raw exports keep compatibility veto");
                if(mode==8){check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,"checked tasks: close exported task holder");
                    JSValue value=JS_Eval(s.exported,"1+1",3,"legacy.js",JS_EVAL_TYPE_GLOBAL);
                    check(!JS_IsException(value),"checked tasks: explicit context export survives task and holder close");JS_FreeValue(s.exported,value);}
            }
            os64_js_destroy(runtime);
        }
        config.limits.jobs_per_turn=os64_js_default_limits().jobs_per_turn;
    }
}
#endif
