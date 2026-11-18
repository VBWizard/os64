#ifndef JS_OWNER_SETUP_CASES_H
#define JS_OWNER_SETUP_CASES_H
#include "os64/js_callbacks.h"
#include "os64/str.h"
#include "quickjs.h"
#include "../runtime/owner_runtime.h"

typedef struct {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket,promoted;
    JSOwnerRuntimeScope *scope;
    JSContext *context,*exported;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    unsigned mode,calls,jobs,finalized,forbidden,loans;
} OwnerSetupCase;
static OwnerSetupCase *owner_setup_current;
static JSClassID owner_setup_class;
static void owner_setup_loan(OwnerSetupCase *s,const char *message)
{
    JSOwnerRuntimeMetadata m;JSOwnerId id;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==s->loans &&
        js_owner_scope_owner(s->scope,&id)==JS_OWNER_OK && id.incarnation==s->ticket.owner.incarnation &&
        id.serial==s->ticket.owner.serial,message);
}
static JSValue owner_setup_job(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)context;(void)self;(void)argc;(void)argv;OwnerSetupCase *s=owner_setup_current;
    if(s) {owner_setup_loan(s,"checked setup: candidate job retains the original admitted loan");s->jobs++;}
    return JS_UNDEFINED;
}
static JSValue owner_setup_forbidden_job(JSContext *context,int argc,JSValueConst *argv)
{
    (void)context;(void)argc;(void)argv;
    if(owner_setup_current)owner_setup_current->forbidden++;
    return JS_UNDEFINED;
}
static void owner_setup_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;OwnerSetupCase *s=JS_GetOpaque(value,owner_setup_class);if(!s)return;
    owner_setup_loan(s,"checked setup: diagnostic or transaction finalizer retains setup loan after holder close");s->finalized++;
    if(s->mode==3) {
        s->check(js_owner_scope_promote(s->scope,&s->promoted)==JS_OWNER_OK,
            "checked setup: rollback finalizer can promote its independently live loan");
        s->check(JS_EnqueueJobOwned(s->context,NULL,NULL,owner_setup_forbidden_job,0,NULL)==0,
            "checked setup: transaction finalizer enqueues work into the selected rollback group");
    }
}
static os64_js_status_t owner_setup_eval(JSContext *context,const char *code)
{
    JSValue value=JS_Eval(context,code,os64_strlen(code),"checked-install.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value))return OS64_JS_EXCEPTION;
    JS_FreeValue(context,value);return OS64_JS_OK;
}
static os64_js_status_t owner_setup_body(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerSetupCase *s=opaque;s->scope=scope;s->context=context;s->calls++;
    owner_setup_loan(s,"checked setup: native installer receives a loan for its actual staged owner");
    if(s->mode==1) {
        s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK &&
            js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK && s->promoted.serial>s->ticket.serial,
            "checked setup: reentrant holder close preserves promotion from admitted setup loan");
        os64_js_outcome_t saved=*s->outer,nested;
        s->check(js_owner_runtime_setup(s->runtime,OS64_JS_ABI_ID,s->promoted,owner_setup_body,s,s->outer)==OS64_JS_BUSY &&
            !os64_memcmp(&saved,s->outer,sizeof saved),"checked setup: nested setup protects outer outcome alias");
        s->check(js_owner_runtime_setup(s->runtime,OS64_JS_ABI_ID,s->promoted,owner_setup_body,s,&nested)==OS64_JS_BUSY,
            "checked setup: recursive installation cannot enter a second transaction");
        s->check(js_owner_runtime_activate(s->runtime,OS64_JS_ABI_ID,s->promoted,&nested)==OS64_JS_BAD_ARGUMENT,
            "checked setup: activation refuses before installer completion");
    }
    if(s->mode==2 || s->mode==3) {
        owner_setup_class=os64_js_class_id(&owner_setup_class);
        JSClassDef definition={.class_name="CheckedSetupCleanup",.finalizer=owner_setup_finalizer};
        s->check(JS_IsRegisteredClass(JS_GetRuntime(context),owner_setup_class) ||
            JS_NewClass(JS_GetRuntime(context),owner_setup_class,&definition)==0,"checked setup: native cleanup oracle class");
        JSValue value=JS_NewObjectClass(context,owner_setup_class);
        if(JS_IsException(value))return OS64_JS_EXCEPTION;
        JS_SetOpaque(value,s);
        s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK,"checked setup: close holder before installation cleanup");
        if(s->mode==2) {JS_Throw(context,value);return OS64_JS_EXCEPTION;}
        JSValue global=JS_GetGlobalObject(context);
        int installed=JS_SetPropertyStr(context,global,"setupHeld",value);JS_FreeValue(context,global);
        if(installed<0)return OS64_JS_EXCEPTION;
        os64_js_status_t result=owner_setup_eval(context,
            "globalThis.setupRegistry=new FinalizationRegistry(()=>{throw Error('forbidden cleanup')});"
            "(()=>{let target={};setupRegistry.register(target,setupHeld)})();setupHeld=null");
        if(result!=OS64_JS_OK)return result;
        JS_RunGC(JS_GetRuntime(context));
        s->check(JS_IsFinalizationTaskPending(JS_GetRuntime(context)),"checked setup: candidate cleanup task is deferred");
        result=owner_setup_eval(context,"setupRegistry=null");
        return result==OS64_JS_OK?OS64_JS_ADMISSION_REFUSED:result;
    }
    if(s->mode==4) {
        if(JS_EnqueueJobOwned(context,NULL,NULL,owner_setup_forbidden_job,0,NULL)<0)return OS64_JS_EXCEPTION;
        return OS64_JS_ADMISSION_REFUSED;
    }
    if(s->mode==5) {
        s->check(js_owner_scope_export(scope,&s->exported)==JS_OWNER_OK && s->exported==context,
            "checked setup: explicit compatibility export preserves its longer context borrow");
    }
    JSValue global=JS_GetGlobalObject(context);
    int installed=JS_SetPropertyStr(context,global,"installJob",JS_NewCFunction(context,owner_setup_job,"installJob",0));
    JS_FreeValue(context,global);
    if(installed<0)return OS64_JS_EXCEPTION;
    return owner_setup_eval(context,"globalThis.installName='checked';globalThis.installJobs=0;Promise.resolve().then(()=>{installJobs++;installJob()})");
}
static os64_js_status_t owner_setup_invoke(OwnerSetupCase *s)
{
    owner_setup_current=s;
    os64_js_status_t result=js_owner_runtime_setup(s->runtime,OS64_JS_ABI_ID,s->ticket,owner_setup_body,s,s->outer);
    owner_setup_current=NULL;s->context=NULL;s->scope=NULL;return result;
}
static JSValue owner_setup_verify(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;(void)opaque;
    const char *code="if(installName!=='checked'||installJobs!==1)throw Error('setup state')";
    return JS_Eval(context,code,os64_strlen(code),"verify-setup.js",JS_EVAL_TYPE_GLOBAL);
}

typedef struct {OwnerSetupCase child;os64_js_outcome_t *outer;unsigned forbidden;} OwnerSetupBudget;
static OwnerSetupBudget *owner_setup_budget;
static JSValue owner_setup_nested(JSContext *context,int argc,JSValueConst *argv)
{
    (void)context;(void)argc;(void)argv;OwnerSetupBudget *b=owner_setup_budget;
    os64_js_outcome_t saved=*b->outer,child_out;b->child.outer=&child_out;
    b->child.check(owner_setup_invoke(&b->child)==OS64_JS_OK && child_out.jobs_executed==2 &&
        !os64_memcmp(&saved,b->outer,sizeof saved),"checked setup: nested candidate consumes the original task job budget");
    JSOwnerRuntimeMetadata m;b->child.check(js_owner_runtime_metadata(b->child.runtime,&m) && m.summary.loans==1,
        "checked setup: nested setup unwind leaves the parent invocation loan intact");
    return JS_UNDEFINED;
}
static JSValue owner_setup_budget_forbidden(JSContext *context,int argc,JSValueConst *argv)
{ (void)context;(void)argc;(void)argv;if(owner_setup_budget)owner_setup_budget->forbidden++;return JS_UNDEFINED; }
static JSValue owner_setup_budget_body(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;(void)opaque;
    if(JS_EnqueueJobOwned(context,context,context,owner_setup_nested,0,NULL)<0 ||
        JS_EnqueueJobOwned(context,context,context,owner_setup_budget_forbidden,0,NULL)<0)return JS_EXCEPTION;
    return JS_UNDEFINED;
}
static void owner_setup_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};config.limits.memory_bytes=8*1024*1024;
    config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;JSOwnerTicket primary,ticket;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){5,6,2},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            os64_js_enable_finalization_tasks(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked setup: selected owner and deferred cleanup fixture");
        check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&ticket,&out)==OS64_JS_OK,"checked setup: staged admission publishes only a checked ticket");
        JSOwnerRealmSnapshot state;JSOwnerRuntimeMetadata m;
        check(js_owner_runtime_owner(runtime,ticket,&state)==JS_OWNER_OK && state.staged && !state.retired &&
            js_owner_runtime_run(runtime,OS64_JS_ABI_ID,ticket,"",0,"not active",&out)==OS64_JS_REALM_INSTALLING &&
            js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,ticket,&out)==OS64_JS_BAD_ARGUMENT,
            "checked setup: staged ticket does not grant execution or premature activation");
        OwnerSetupCase s={.runtime=runtime,.ticket=ticket,.outer=&out,.check=check,.mode=1,.loans=1};
        check(owner_setup_invoke(&s)==OS64_JS_OK && s.calls==1 && s.jobs==1,
            "checked setup: admitted setup drains its candidate jobs after holder close");
        ticket=s.promoted;
        check(js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,s.ticket,&out)==OS64_JS_ADMISSION_REFUSED &&
            js_owner_runtime_setup(runtime,OS64_JS_ABI_ID,ticket,owner_setup_body,&s,&out)==OS64_JS_BAD_ARGUMENT &&
            s.calls==1 && js_owner_runtime_owner(runtime,ticket,&state)==JS_OWNER_OK && state.staged &&
            js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,ticket,&out)==OS64_JS_OK,
            "checked setup: only completed original owner activates through its live promoted holder");
        check(js_owner_runtime_scoped_run(runtime,OS64_JS_ABI_ID,ticket,"verify",owner_setup_verify,NULL,&out)==OS64_JS_OK &&
            js_owner_runtime_metadata(runtime,&m) && !m.summary.unknown_escape && !m.summary.loans,
            "checked setup: scoped installation and checked activation cause no legacy export");
        check(js_owner_runtime_close(runtime,ticket)==JS_OWNER_OK,"checked setup: close active promoted holder");
        for(unsigned mode=2;mode<=4;mode++) {
            check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&ticket,&out)==OS64_JS_OK,"checked setup: failure candidate admitted cumulatively");
            s=(OwnerSetupCase){.runtime=runtime,.ticket=ticket,.outer=&out,.check=check,.mode=mode,.loans=1};
            check(owner_setup_invoke(&s)==(mode==2?OS64_JS_EXCEPTION:OS64_JS_ADMISSION_REFUSED),
                "checked setup: local exception and refusal retain candidate failure results");
            if(mode==3)check(s.finalized==1 && !s.forbidden && out.jobs_discarded==2 && !out.jobs_pending && s.promoted.serial,
                "checked setup: both-lane rollback discards finalizer-enqueued work before returning");
            if(mode==2)check(s.finalized==1,"checked setup: pending exception is freed while setup loan is alive");
            if(mode==4)check(!s.forbidden && out.jobs_discarded==1,"checked setup: refused candidate jobs never execute");
            JSOwnerTicket retained=mode==3?s.promoted:ticket;
            if(mode!=2)check(js_owner_runtime_owner(runtime,retained,&state)==JS_OWNER_OK && state.staged && state.retired &&
                js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,retained,&out)==OS64_JS_REALM_RETIRED &&
                js_owner_runtime_close(runtime,retained)==JS_OWNER_OK,"checked setup: failed original candidate stays mapped and non-activatable");
            check(js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
                "checked setup: failed installation cleanup ends loan without raw escape or slot reuse");
        }
        JSOwnerTicket untouched={{91,92},93};
        check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&untouched,&out)==OS64_JS_ADMISSION_REFUSED && untouched.serial==93 &&
            js_owner_runtime_metadata(runtime,&m) && m.summary.owners==5 && m.assigned_owners==5,
            "checked setup: retired candidates retain cumulative owner capacity without partial admission");
        check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,"checked setup: close primary holder");os64_js_destroy(runtime);

        runtime=NULL;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){2,3,1},&runtime,&out)==OS64_JS_OK,
            "checked setup: guard and explicit-export fixture");
        untouched=(JSOwnerTicket){{91,92},93};
        check(js_owner_runtime_stage(runtime,"wrong ABI",1,&untouched,&out)==OS64_JS_ABI_MISMATCH &&
            js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,0,&untouched,&out)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,SIZE_MAX,&untouched,&out)==OS64_JS_ADMISSION_REFUSED &&
            js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,NULL,&out)==OS64_JS_BAD_ARGUMENT && untouched.serial==93 &&
            js_owner_runtime_metadata(runtime,&m) && m.summary.owners==1 && !m.summary.unknown_escape,
            "checked setup: stage argument and headroom refusal publish no ticket or owner");
        check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&ticket,&out)==OS64_JS_OK &&
            js_owner_runtime_setup(runtime,OS64_JS_ABI_ID,ticket,NULL,NULL,&out)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_owner(runtime,ticket,&state)==JS_OWNER_OK && state.staged && !state.retired,
            "checked setup: missing installer leaves an unused staged candidate");
        s=(OwnerSetupCase){.runtime=runtime,.ticket=ticket,.outer=&out,.check=check,.mode=5,.loans=1};
        check(owner_setup_invoke(&s)==OS64_JS_OK && s.exported &&
            js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,ticket,&out)==OS64_JS_OK &&
            js_owner_runtime_close(runtime,ticket)==JS_OWNER_OK &&
            js_owner_runtime_setup(runtime,OS64_JS_ABI_ID,ticket,owner_setup_body,&s,&out)==OS64_JS_ADMISSION_REFUSED &&
            js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,ticket,&out)==OS64_JS_ADMISSION_REFUSED,
            "checked setup: explicit export keeps ABI 5 lifetime while closed aliases cannot setup or activate");
        if(s.exported) {
            const char *code="if(installName!=='checked'||installJobs!==1)throw Error('legacy setup export')";
            JSValue value=JS_Eval(s.exported,code,os64_strlen(code),"legacy-setup.js",JS_EVAL_TYPE_GLOBAL);
            check(!JS_IsException(value),"checked setup: explicitly exported context survives setup and holder close");JS_FreeValue(s.exported,value);
        }
        check(js_owner_runtime_metadata(runtime,&m) && m.summary.unknown_escape && !m.summary.tickets && !m.summary.loans,
            "checked setup: compatibility veto survives completed installation and zero holders");os64_js_destroy(runtime);

        runtime=NULL;
        check(os64_js_create_with_teardown(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
            "checked setup: legacy default runtime fixture");
        check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&untouched,&out)==OS64_JS_ADMISSION_REFUSED && untouched.serial==93,
            "checked setup: default construction does not select checked metadata");os64_js_destroy(runtime);

        runtime=NULL;os64_js_config_t budgeted=config;budgeted.limits.jobs_per_turn=2;
        check(js_owner_runtime_create(&budgeted,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){2,3,2},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&ticket,&out)==OS64_JS_OK,"checked setup: nested original-budget fixture");
        OwnerSetupBudget budget={.child={.runtime=runtime,.ticket=ticket,.check=check,.loans=2},.outer=&out};owner_setup_budget=&budget;
        check(js_owner_runtime_scoped_run(runtime,OS64_JS_ABI_ID,primary,"outer budget",owner_setup_budget_body,&budget,&out)==OS64_JS_LIMIT &&
            out.limit==OS64_JS_LIMIT_JOBS && out.jobs_executed==2 && !budget.forbidden && budget.child.jobs==1,
            "checked setup: installation cannot renew parent budget to execute a forbidden sibling job");owner_setup_budget=NULL;
        check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK && js_owner_runtime_close(runtime,ticket)==JS_OWNER_OK &&
            js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
            "checked setup: nested failure unwinds both independent loans and host holders");os64_js_destroy(runtime);
    }
}
#endif
