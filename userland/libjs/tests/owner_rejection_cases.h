#ifndef JS_OWNER_REJECTION_CASES_H
#define JS_OWNER_REJECTION_CASES_H
#include "os64/js_callbacks.h"
#include "os64/str.h"
#include "../runtime/owner_runtime.h"

typedef struct OwnerRejectionCase OwnerRejectionCase;
typedef struct {OwnerRejectionCase *state;unsigned role;} OwnerRejectionRoot;
struct OwnerRejectionCase {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket,promoted;
    JSOwnerRuntimeScope *scope;
    JSContext *context;
    JSValueConst promise,reason;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    OwnerRejectionRoot root[2];
    const char *keep;
    unsigned mode,calls,reason_freed,result_freed,jobs,handled,rejected,exceptions;
    bool consumed,claimed;
};
static JSClassID owner_rejection_class;
static OwnerRejectionCase *owner_rejection_current;
static void owner_rejection_loan(OwnerRejectionCase *s,const char *message)
{
    JSOwnerRuntimeMetadata m;JSOwnerId owner;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1 &&
        js_owner_scope_owner(s->scope,&owner)==JS_OWNER_OK &&
        owner.serial==s->ticket.owner.serial && owner.incarnation==s->ticket.owner.incarnation,message);
}
static void owner_rejection_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;OwnerRejectionRoot *root=JS_GetOpaque(value,owner_rejection_class);
    if(!root)return;
    OwnerRejectionCase *s=root->state;
    if(s->claimed) {
        owner_rejection_loan(s,"checked rejections: queue and result finalizers retain original loan");
        os64_js_outcome_t nested;bool untouched=true;
        s->check(js_owner_runtime_consume_rejection(s->runtime,OS64_JS_ABI_ID,s->ticket,"finalizer",NULL,s,&untouched,&nested)==OS64_JS_BUSY && untouched,
            "checked rejections: queue and result cleanup retains active guard");
    }
    if(root->role==1)s->reason_freed++;
    else {
        JSValue result=JS_PromiseResult(s->context,s->promise);
        s->check(JS_PromiseState(s->context,s->promise)==JS_PROMISE_REJECTED &&
            JS_VALUE_GET_PTR(result)==JS_VALUE_GET_PTR(s->reason),
            "checked rejections: borrowed promise and reason survive returned value cleanup");
        JS_FreeValue(s->context,result);s->result_freed++;
    }
}
static JSValue owner_rejection_value(JSContext *context,OwnerRejectionCase *s,unsigned role)
{
    owner_rejection_class=os64_js_class_id(&owner_rejection_class);
    JSClassDef definition={.class_name="CheckedRejectionRoot",.finalizer=owner_rejection_finalizer};
    if(!JS_IsRegisteredClass(JS_GetRuntime(context),owner_rejection_class) &&
        JS_NewClass(JS_GetRuntime(context),owner_rejection_class,&definition)<0)return JS_EXCEPTION;
    JSValue value=JS_NewObjectClass(context,owner_rejection_class);
    if(!JS_IsException(value)) {
        s->root[role-1]=(OwnerRejectionRoot){s,role};JS_SetOpaque(value,&s->root[role-1]);
    }
    return value;
}
static JSValue owner_rejection_prepare(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;OwnerRejectionCase *s=opaque;JSValue functions[2];
    JSValue promise=JS_NewPromiseCapability(context,functions);
    if(JS_IsException(promise))return promise;
    JSValue reason=owner_rejection_value(context,s,1);
    JSValue result=JS_IsException(reason)?JS_EXCEPTION:JS_Call(context,functions[1],JS_UNDEFINED,1,&reason);
    JS_FreeValue(context,reason);JS_FreeValue(context,functions[0]);JS_FreeValue(context,functions[1]);
    if(s->keep && !JS_IsException(result)) {
        JSValue global=JS_GetGlobalObject(context);
        int stored=JS_SetPropertyStr(context,global,s->keep,JS_DupValue(context,promise));
        JS_FreeValue(context,global);
        if(stored<0){JS_FreeValue(context,result);result=JS_EXCEPTION;}
    }
    JS_FreeValue(context,promise);return result;
}
static JSValue owner_rejection_job(JSContext *context,int argc,JSValueConst *argv)
{(void)context;(void)argc;(void)argv;owner_rejection_current->jobs++;return JS_UNDEFINED;}
static uint32_t owner_rejection_tag(void *opaque,JSContext *context,JSValue promise)
{(void)context;(void)promise;OwnerRejectionCase *s=opaque;s->rejected++;return 73;}
static void owner_rejection_handled(void *opaque,JSContext *context,JSValue promise)
{(void)context;(void)promise;OwnerRejectionCase *s=opaque;s->handled++;}
static void owner_rejection_exception(void *opaque,JSContext *context,JSValue error)
{(void)context;(void)error;OwnerRejectionCase *s=opaque;s->exceptions++;}
static JSValue owner_rejection_handled_noop(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{(void)context;(void)self;(void)argc;(void)argv;return JS_UNDEFINED;}
static JSValue owner_rejection_body(JSOwnerRuntimeScope *scope,JSContext *context,
    JSValueConst promise,JSValueConst reason,uint32_t tag,void *opaque)
{
    OwnerRejectionCase *s=opaque;s->calls++;s->scope=scope;s->context=context;
    s->promise=promise;s->reason=reason;s->claimed=true;
    owner_rejection_loan(s,"checked rejections: callback borrows original mapped owner under one loan");
    s->check(s->consumed && JS_PromiseState(context,promise)==JS_PROMISE_REJECTED &&
        JS_GetOpaque(reason,owner_rejection_class)==&s->root[0] && tag==(s->rejected?73u:0u),
        "checked rejections: claim publishes matching promise reason and host tag");
    os64_js_outcome_t saved=*s->outer,nested;bool untouched=false;
    s->check(js_owner_runtime_consume_rejection(s->runtime,OS64_JS_ABI_ID,s->ticket,"reentry",owner_rejection_body,s,&untouched,s->outer)==OS64_JS_BUSY &&
        !untouched && !os64_memcmp(&saved,s->outer,sizeof saved),
        "checked rejections: recursive claim protects outer outcome and consumed output");
    s->check(js_owner_runtime_set_script_host(s->runtime,OS64_JS_ABI_ID,s->ticket,NULL,&nested)==OS64_JS_BUSY &&
        js_owner_runtime_set_script_host(s->runtime,OS64_JS_ABI_ID,s->ticket,NULL,s->outer)==OS64_JS_BUSY &&
        !os64_memcmp(&saved,s->outer,sizeof saved),
        "checked rejections: host detach during callback preserves guard and outer outcome");
    if(s->mode==1) {
        s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK &&
            js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK,
            "checked rejections: holder close preserves promotion from claimed loan");
        return owner_rejection_value(context,s,2);
    }
    if(s->mode==2)return JS_Throw(context,owner_rejection_value(context,s,2));
    if(s->mode==3)os64_js_cancel(s->runtime);
    if(s->mode==4 && JS_EnqueueJobOwned(context,context,context,owner_rejection_job,0,NULL)<0)return JS_EXCEPTION;
    if(s->mode==5) {
        JSValue then=JS_GetPropertyStr(context,promise,"catch"),noop=JS_NewCFunction(context,owner_rejection_handled_noop,"noop",0);
        JSValue result=JS_Call(context,then,promise,1,&noop);
        JS_FreeValue(context,noop);JS_FreeValue(context,then);return result;
    }
    if(s->mode==6) {
        const char *code="({items:Array.from({length:4},(_,i)=>({i}))})";
        return JS_Eval(context,code,os64_strlen(code),"allocate.js",JS_EVAL_TYPE_GLOBAL);
    }
    if(s->mode==7) {
        const char *code="secondPromise.catch(()=>{});secondPromise=null";
        return JS_Eval(context,code,os64_strlen(code),"handle-other.js",JS_EVAL_TYPE_GLOBAL);
    }
    (void)nested;return JS_UNDEFINED;
}
static os64_js_status_t owner_rejection_consume(OwnerRejectionCase *s)
{
    os64_js_status_t result=js_owner_runtime_consume_rejection(s->runtime,OS64_JS_ABI_ID,s->ticket,
        "checked rejection",owner_rejection_body,s,&s->consumed,s->outer);
    s->scope=NULL;s->context=NULL;s->claimed=false;return result;
}
static OwnerRejectionCase owner_rejection_state(os64_js_runtime_t *runtime,JSOwnerTicket ticket,
    os64_js_outcome_t *out,void (*check)(bool,const char *))
{
    return (OwnerRejectionCase){.runtime=runtime,.ticket=ticket,.outer=out,.check=check};
}
static void owner_rejection_enqueue(OwnerRejectionCase *s)
{
    s->check(js_owner_runtime_scoped_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"reject",owner_rejection_prepare,s,s->outer)==OS64_JS_OK,
        "checked rejections: queue actual promise with queue-owned native reason");
}
static void owner_rejection_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};
    config.limits.memory_bytes=8*1024*1024;config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;JSOwnerTicket primary,a,b,stage;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){4,5,1},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&a,&out)==OS64_JS_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&b,&out)==OS64_JS_OK,
            "checked rejections: construct mapped independent child queues");
        check(os64_js_enable_host_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
            "checked rejections: enable existing settings without raw export");
        os64_js_script_host_t host={0};
        check(js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,a,&host,&out)==OS64_JS_OK &&
            js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,b,&host,&out)==OS64_JS_OK,
            "checked rejections: checked empty host retains rejection queue without legacy delivery");
        OwnerRejectionCase s[3];s[0]=owner_rejection_state(runtime,a,&out,check);
        s[1]=owner_rejection_state(runtime,b,&out,check);s[2]=owner_rejection_state(runtime,a,&out,check);
        for(unsigned i=0;i<3;i++)owner_rejection_enqueue(&s[i]);
        bool unchanged=true;unsigned calls=s[0].calls;
        check(js_owner_runtime_consume_rejection(runtime,"bad ABI",a,"bad",owner_rejection_body,&s[0],&unchanged,&out)==OS64_JS_ABI_MISMATCH && unchanged &&
            js_owner_runtime_consume_rejection(runtime,OS64_JS_ABI_ID,a,NULL,owner_rejection_body,&s[0],&unchanged,&out)==OS64_JS_BAD_ARGUMENT && unchanged &&
            js_owner_runtime_consume_rejection(runtime,OS64_JS_ABI_ID,a,"bad",NULL,&s[0],&unchanged,&out)==OS64_JS_BAD_ARGUMENT && unchanged &&
            js_owner_runtime_consume_rejection(runtime,OS64_JS_ABI_ID,a,"bad",owner_rejection_body,&s[0],NULL,&out)==OS64_JS_BAD_ARGUMENT &&
            s[0].calls==calls && !s[0].reason_freed,
            "checked rejections: malformed polls preserve queued roots and consumed storage");
        s[1].mode=2;
        check(owner_rejection_consume(&s[1])==OS64_JS_EXCEPTION && s[1].consumed && s[1].reason_freed==1 && s[1].result_freed==1,
            "checked rejections: realm filtering and exception cleanup consume one child record");
        s[0].mode=1;
        check(owner_rejection_consume(&s[0])==OS64_JS_OK && s[0].consumed && s[0].reason_freed==1 && s[0].result_freed==1,
            "checked rejections: oldest matching record survives close through returned-value cleanup");
        a=s[0].promoted;s[2].ticket=a;
        check(owner_rejection_consume(&s[2])==OS64_JS_OK && s[2].consumed && s[2].reason_freed==1,
            "checked rejections: detached queue links preserve next same-owner rejection");
        s[2].consumed=true;calls=s[2].calls;
        JSOwnerRuntimeMetadata before,after;
        check(js_owner_runtime_metadata(runtime,&before) && owner_rejection_consume(&s[2])==OS64_JS_OK && !s[2].consumed &&
            s[2].calls==calls && js_owner_runtime_metadata(runtime,&after) && !after.summary.loans &&
            before.summary.tickets==after.summary.tickets && !after.summary.unknown_escape,
            "checked rejections: empty poll lends nothing and pure consumption records no raw export");
        s[0]=owner_rejection_state(runtime,a,&out,check);s[2]=owner_rejection_state(runtime,a,&out,check);
        s[2].keep="secondPromise";owner_rejection_enqueue(&s[0]);owner_rejection_enqueue(&s[2]);s[0].mode=7;
        check(owner_rejection_consume(&s[0])==OS64_JS_OK && s[0].reason_freed==1 && s[2].reason_freed==1 &&
            !s[2].calls && owner_rejection_consume(&s[2])==OS64_JS_OK && !s[2].consumed,
            "checked rejections: callback handling another queued promise cannot invalidate detached record");
        for(unsigned cycle=0;cycle<16;cycle++) {
            s[0]=owner_rejection_state(runtime,a,&out,check);owner_rejection_enqueue(&s[0]);
            check(owner_rejection_consume(&s[0])==OS64_JS_OK && s[0].consumed && s[0].reason_freed==1,
                "checked rejections: repeated queue cleanup releases its owned roots once");
        }
        check(js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&stage,&out)==OS64_JS_OK &&
            js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,stage,&host,&out)==OS64_JS_OK &&
            js_owner_runtime_consume_rejection(runtime,OS64_JS_ABI_ID,stage,"staged",owner_rejection_body,&s[0],&unchanged,&out)==OS64_JS_REALM_INSTALLING && unchanged,
            "checked rejections: staged ticket cannot consume author rejection");
        check(js_owner_runtime_retire(runtime,OS64_JS_ABI_ID,b,&out)==OS64_JS_OK &&
            js_owner_runtime_consume_rejection(runtime,OS64_JS_ABI_ID,b,"retired",owner_rejection_body,&s[1],&unchanged,&out)==OS64_JS_REALM_RETIRED && unchanged &&
            js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,b,&host,&out)==OS64_JS_REALM_RETIRED &&
            js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,b,NULL,&out)==OS64_JS_OK,
            "checked rejections: retirement forbids consumption but permits checked host detachment");
        check(js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,a,NULL,&out)==OS64_JS_OK &&
            js_owner_runtime_consume_rejection(runtime,OS64_JS_ABI_ID,a,"unhosted",owner_rejection_body,&s[2],&unchanged,&out)==OS64_JS_BAD_ARGUMENT && unchanged,
            "checked rejections: detached host rejects polling without changing output");
        check(js_owner_runtime_close(runtime,a)==JS_OWNER_OK &&
            js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,a,NULL,&out)==OS64_JS_ADMISSION_REFUSED &&
            js_owner_runtime_close(runtime,b)==JS_OWNER_OK && js_owner_runtime_close(runtime,stage)==JS_OWNER_OK &&
            js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,
            "checked rejections: closed holders cannot operate on the retained realm");
        os64_js_destroy(runtime);

        for(unsigned mode=0;mode<4;mode++) {
            check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
                (JSOwnerLimits){1,2,1},&runtime,&out)==OS64_JS_OK &&
                js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK,
                "checked rejections: legacy/cancellation fixture");
            OwnerRejectionCase legacy=owner_rejection_state(runtime,primary,&out,check);
            host=(os64_js_script_host_t){.opaque=&legacy,.rejected=mode==0?owner_rejection_tag:NULL,
                .handled=owner_rejection_handled,.exception=mode==3?owner_rejection_exception:NULL};
            check(js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,primary,&host,&out)==OS64_JS_OK &&
                js_owner_runtime_metadata(runtime,&after) && !after.summary.unknown_escape,
                "checked rejections: registration itself exports no raw view");
            owner_rejection_enqueue(&legacy);
            if(mode<2) {
                legacy.mode=5;
                check(owner_rejection_consume(&legacy)==OS64_JS_OK && legacy.consumed && legacy.handled==1 && legacy.rejected==(mode==0?1u:0u) &&
                    js_owner_runtime_metadata(runtime,&after) && after.summary.unknown_escape,
                    "checked rejections: legacy rejected/tag or late handled callback preserves sticky raw veto");
            } else if(mode==3) {
                legacy.mode=2;
                check(owner_rejection_consume(&legacy)==OS64_JS_EXCEPTION && legacy.consumed && legacy.exceptions==1 &&
                    legacy.reason_freed==1 && legacy.result_freed==1 && js_owner_runtime_metadata(runtime,&after) && after.summary.unknown_escape,
                    "checked rejections: legacy exception reporting retains its raw compatibility veto");
            } else {
                legacy.mode=3;
                check(owner_rejection_consume(&legacy)==OS64_JS_CANCELLED && legacy.consumed && legacy.reason_freed==1 &&
                    js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,primary,NULL,&out)==OS64_JS_OK,
                    "checked rejections: cancellation after claim frees roots and permits failed-host detach");
            }
            check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,"checked rejections: close legacy fixture holder");
            os64_js_destroy(runtime);
        }

        config.limits.jobs_per_turn=1;
        check(js_owner_runtime_create_with_failure_records(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){2,3,1},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&a,&out)==OS64_JS_OK,
            "checked rejections: original explicit-task budget fixture");
        host=(os64_js_script_host_t){0};
        check(js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,a,&host,&out)==OS64_JS_OK,
            "checked rejections: hosted task fixture");
        OwnerRejectionCase task[2];
        for(unsigned i=0;i<2;i++){task[i]=owner_rejection_state(runtime,a,&out,check);owner_rejection_enqueue(&task[i]);task[i].mode=4;}
        check(os64_js_task_begin(runtime,OS64_JS_ABI_ID,"original task",&out)==OS64_JS_OK,
            "checked rejections: open original task after hosted rejections");
        JSOwnerFailureSnapshot last;
        owner_rejection_current=&task[0];
        check(owner_rejection_consume(&task[0])==OS64_JS_OK && task[0].consumed && out.jobs_pending && !out.jobs_executed && !task[0].jobs,
            "checked rejections: borrowing leaves original task open and defers its queued job");
        unchanged=true;
        check(js_owner_runtime_consume_rejection(runtime,OS64_JS_ABI_ID,a,"pending",owner_rejection_body,&task[1],&unchanged,&out)==OS64_JS_BUSY &&
            unchanged && !task[1].calls && !task[1].reason_freed,
            "checked rejections: pending microtasks preserve next unclaimed record");
        check(os64_js_checkpoint(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK && task[0].jobs==1 && out.jobs_executed==1,
            "checked rejections: original checkpoint charges one consumed-callback job");
        check(owner_rejection_consume(&task[1])==OS64_JS_LIMIT && task[1].consumed && out.limit==OS64_JS_LIMIT_JOBS &&
            out.jobs_executed==1 && task[0].jobs==1 && task[1].reason_freed==1 &&
            js_owner_runtime_observe_failure(runtime,OS64_JS_ABI_ID,&last)==OS64_JS_OK && last.deadline>0 && last.explicit_task &&
            last.task_owner.owner.serial==primary.owner.serial && !os64_strcmp(out.source_name,"original task"),
            "checked rejections: second consumption cannot renew exhausted task counter or deadline");
        check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_LIMIT &&
            js_owner_runtime_set_script_host(runtime,OS64_JS_ABI_ID,a,NULL,&out)==OS64_JS_OK &&
            js_owner_runtime_close(runtime,a)==JS_OWNER_OK && js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,
            "checked rejections: failed original task closes and host detaches");
        os64_js_destroy(runtime);owner_rejection_current=NULL;config.limits.jobs_per_turn=os64_js_default_limits().jobs_per_turn;
    }
}
#endif
