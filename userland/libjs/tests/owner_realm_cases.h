#ifndef JS_OWNER_REALM_CASES_H
#define JS_OWNER_REALM_CASES_H
#include "os64/js_engine.h"
#include "os64/str.h"
#include "quickjs.h"
#include "../runtime/owner_runtime.h"

typedef struct {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    unsigned calls;
    JSOwnerTicket spawned;
    bool destroying;
} OwnerRealmReentry;
static OwnerRealmReentry *owner_realm_reentry;
static JSClassID owner_realm_finalizer_class;
static void owner_realm_close_in_call(OwnerRealmReentry *s)
{
    JSOwnerRuntimeMetadata metadata;
    if(s->destroying) {
        s->check(js_owner_runtime_metadata(s->runtime,&metadata) && !metadata.summary.loans &&
            js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK,
            "realm owners: metadata close remains available during root finalizer cleanup");
        JSOwnerTicket refused={{91,92},93};
        s->check(js_owner_runtime_primary_ticket(s->runtime,&refused)==JS_OWNER_INVALID && refused.serial==93,
            "realm owners: destruction cannot publish a new owner ticket");
        s->calls++;return;
    }
    s->check(js_owner_runtime_metadata(s->runtime,&metadata) && metadata.summary.loans==1,
        "realm owners: admitted loan survives into callback or finalizer");
    s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK,
        "realm owners: reentrant close ends holder without ending invocation");
    JSOwnerTicket refused={{91,92},93};
    s->check(js_owner_runtime_duplicate(s->runtime,s->ticket,&refused)==JS_OWNER_STALE && refused.serial==93,
        "realm owners: closed invocation ticket cannot duplicate");
    os64_js_outcome_t saved=*s->outer,other;
    s->check(js_owner_runtime_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"",0,"nested.js",s->outer)==OS64_JS_BUSY &&
        !os64_memcmp(&saved,s->outer,sizeof saved),"realm owners: outer outcome alias refuses before clearing storage");
    s->check(js_owner_runtime_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"",0,"nested.js",&other)==OS64_JS_BUSY &&
        js_owner_runtime_metadata(s->runtime,&metadata) && metadata.summary.loans==1,
        "realm owners: nested BUSY neither creates another loan nor ends this one");
    if(!s->spawned.serial) {
        s->check(js_owner_runtime_child(s->runtime,OS64_JS_ABI_ID,1,&refused,s->outer)==OS64_JS_BUSY &&
            refused.serial==93 && !os64_memcmp(&saved,s->outer,sizeof saved),
            "realm owners: nested child admission protects outer outcome alias");
        s->check(js_owner_runtime_child(s->runtime,OS64_JS_ABI_ID,1,&s->spawned,&other)==OS64_JS_OK &&
            other.jobs_executed>0 && !os64_memcmp(&saved,s->outer,sizeof saved),
            "realm owners: admitted nested child borrows existing execution budget");
    }
    s->calls++;
}
static JSValue owner_realm_reenter(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)ctx;(void)self;(void)argc;(void)argv;
    if(owner_realm_reentry)owner_realm_close_in_call(owner_realm_reentry);
    return JS_UNDEFINED;
}
static void owner_realm_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;
    OwnerRealmReentry *s=JS_GetOpaque(value,owner_realm_finalizer_class);
    if(s)owner_realm_close_in_call(s);
}
static JSValue owner_realm_throwing_object(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;
    JSValue object=JS_NewObjectClass(ctx,owner_realm_finalizer_class);
    if(!JS_IsException(object))JS_SetOpaque(object,owner_realm_reentry);
    return object;
}
static os64_js_status_t owner_realm_run(os64_js_runtime_t *runtime,JSOwnerTicket ticket,
    const char *code,os64_js_outcome_t *out)
{ return js_owner_runtime_run(runtime,OS64_JS_ABI_ID,ticket,code,os64_strlen(code),"owner.js",out); }
static bool owner_realm_id_equal(JSOwnerId a,JSOwnerId b)
{ return a.incarnation==b.incarnation && a.serial==b.serial; }
static os64_js_status_t owner_realm_stage_refuse(os64_js_realm_t *realm,JSContext *context,void *opaque)
{ (void)realm;(void)context;(void)opaque;return OS64_JS_ADMISSION_REFUSED; }

static void owner_realm_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};
    config.limits.memory_bytes=8*1024*1024; config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime=NULL; os64_js_outcome_t out;
        bool created=js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){5,4,2},&runtime,&out)==OS64_JS_OK;
        check(created,"realm owners: construct selected runtime");if(!created)return;
        JSOwnerRuntimeMetadata metadata;
        check(js_owner_runtime_metadata(runtime,&metadata) && metadata.summary.owners==1 &&
            metadata.assigned_owners==1 && !metadata.unpublished_owners && !metadata.summary.tickets,
            "realm owners: primary is assigned without permanent internal holder");
        JSOwnerTicket primary,duplicate,a,b,refused={{81,82},83};
        check(js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_duplicate(runtime,primary,&duplicate)==JS_OWNER_OK && primary.serial!=duplicate.serial,
            "realm owners: primary tickets explicitly duplicate independent holders");
        check(js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1024*1024,&a,&out)==OS64_JS_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1024*1024,&b,&out)==OS64_JS_OK,
            "realm owners: checked child admissions publish actual distinct owners");
        check(!owner_realm_id_equal(primary.owner,a.owner) && !owner_realm_id_equal(a.owner,b.owner),
            "realm owners: realm owners are distinct within runtime incarnation");
        check(js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&refused,&out)==OS64_JS_ADMISSION_REFUSED &&
            refused.serial==83 && js_owner_runtime_metadata(runtime,&metadata) && metadata.summary.owners==3 &&
            metadata.assigned_owners==3 && !metadata.unpublished_owners,
            "realm owners: full holder pool cannot partially reserve another owner");
        check(owner_realm_run(runtime,primary,"globalThis.ownerName='primary'",&out)==OS64_JS_OK &&
            owner_realm_run(runtime,a,"globalThis.ownerName='A'",&out)==OS64_JS_OK &&
            owner_realm_run(runtime,b,"globalThis.ownerName='B'",&out)==OS64_JS_OK,
            "realm owners: checked source executes in the mapped realm");
        check(owner_realm_run(runtime,a,"if(ownerName!=='A')throw Error('wrong owner')",&out)==OS64_JS_OK &&
            owner_realm_run(runtime,b,"if(ownerName!=='B')throw Error('wrong owner')",&out)==OS64_JS_OK,
            "realm owners: independent globals cannot be substituted by the primary slot");
        JSOwnerTicket alias=primary;
        JSOwnerRealmSnapshot closed_view={.owner={91,92}};
        check(js_owner_runtime_close(runtime,alias)==JS_OWNER_OK &&
            js_owner_runtime_close(runtime,primary)==JS_OWNER_STALE &&
            js_owner_runtime_owner(runtime,primary,&closed_view)==JS_OWNER_STALE && closed_view.owner.serial==92 &&
            owner_realm_run(runtime,primary,"throw Error('closed')",&out)==OS64_JS_ADMISSION_REFUSED,
            "realm owners: copied aliases close together and cannot execute");
        JSOwnerTicket replacement;
        check(js_owner_runtime_primary_ticket(runtime,&replacement)==JS_OWNER_OK && replacement.serial>primary.serial &&
            owner_realm_run(runtime,duplicate,"if(ownerName!=='primary')throw Error('lost duplicate')",&out)==OS64_JS_OK,
            "realm owners: explicit duplicate and new holder retain original primary owner");
        check(js_owner_runtime_close(runtime,primary)==JS_OWNER_STALE &&
            owner_realm_run(runtime,replacement,"if(ownerName!=='primary')throw Error('stale close')",&out)==OS64_JS_OK,
            "realm owners: stale close cannot invalidate a reused holder cell");
        check(owner_realm_run(runtime,b,"throw Error('ordinary exception')",&out)==OS64_JS_EXCEPTION &&
            owner_realm_run(runtime,b,"globalThis.completed=0;Promise.resolve().then(()=>completed=7)",&out)==OS64_JS_OK &&
            owner_realm_run(runtime,b,"if(completed!==7)throw Error('pending job')",&out)==OS64_JS_OK,
            "realm owners: existing exception and complete-run semantics remain intact");
        check(js_owner_runtime_metadata(runtime,&metadata) && !metadata.summary.loans && !metadata.summary.unknown_escape,
            "realm owners: checked source and copied diagnostics cause no legacy raw export");
        JSOwnerRealmSnapshot snapshot;
        check(js_owner_runtime_retire(runtime,OS64_JS_ABI_ID,a,&out)==OS64_JS_OK &&
            js_owner_runtime_owner(runtime,a,&snapshot)==JS_OWNER_OK && snapshot.retired &&
            owner_realm_run(runtime,a,"throw Error('retired')",&out)==OS64_JS_REALM_RETIRED,
            "realm owners: valid identity does not bypass actual realm retirement");
        check(js_owner_runtime_close(runtime,duplicate)==JS_OWNER_OK &&
            js_owner_runtime_duplicate(runtime,a,&duplicate)==JS_OWNER_OK &&
            js_owner_runtime_close(runtime,a)==JS_OWNER_OK &&
            js_owner_runtime_owner(runtime,duplicate,&snapshot)==JS_OWNER_OK && snapshot.retired,
            "realm owners: metadata lifetime stays independent of executable eligibility");
        JSContext *context=os64_js_context(runtime,OS64_JS_ABI_ID,&out);
        check(context && js_owner_runtime_metadata(runtime,&metadata) && metadata.summary.unknown_escape,
            "realm owners: ABI 5 context export records sticky compatibility retention");
        JSValue global=JS_GetGlobalObject(context);
        check(JS_SetPropertyStr(context,global,"ownerReenter",JS_NewCFunction(context,owner_realm_reenter,"ownerReenter",0))==1,
            "realm owners: install native reentry oracle through legacy raw context");
        owner_realm_finalizer_class=os64_js_class_id(&owner_realm_finalizer_class);
        JSClassDef definition={.class_name="OwnerCleanup",.finalizer=owner_realm_finalizer};
        check(JS_NewClass(JS_GetRuntime(context),owner_realm_finalizer_class,&definition)==0 &&
            JS_SetPropertyStr(context,global,"ownerObject",JS_NewCFunction(context,owner_realm_throwing_object,"ownerObject",0))==1,
            "realm owners: install diagnostic finalizer oracle");JS_FreeValue(context,global);
        OwnerRealmReentry state={.runtime=runtime,.ticket=replacement,.outer=&out,.check=check};owner_realm_reentry=&state;
        check(owner_realm_run(runtime,replacement,
            "globalThis.afterClose=0;Promise.resolve().then(()=>{ownerReenter();afterClose=42})",&out)==OS64_JS_OK && state.calls==1,
            "realm owners: reentrant holder close preserves admitted microtask completion");
        check(js_owner_runtime_metadata(runtime,&metadata) && !metadata.summary.loans && metadata.summary.unknown_escape,
            "realm owners: invocation loan ends after draining while compatibility veto stays sticky");
        check(js_owner_runtime_primary_ticket(runtime,&replacement)==JS_OWNER_OK &&
            owner_realm_run(runtime,replacement,"if(afterClose!==42)throw Error('lost admitted call')",&out)==OS64_JS_OK,
            "realm owners: admitted close does not erase primary state");
        state.ticket=replacement;state.calls=0;
        check(owner_realm_run(runtime,replacement,"throw ownerObject()",&out)==OS64_JS_EXCEPTION && state.calls==1,
            "realm owners: loan survives diagnostic value finalizer cleanup");owner_realm_reentry=NULL;
        check(js_owner_runtime_metadata(runtime,&metadata) && !metadata.summary.loans && metadata.summary.owners==4 &&
            metadata.assigned_owners==4 && !metadata.unpublished_owners,
            "realm owners: finalizer close cannot recycle owners or leak invocation loans");
        check(owner_realm_run(runtime,state.spawned,"if(typeof ownerName!=='undefined')throw Error('nested owner')",&out)==OS64_JS_OK &&
            js_owner_runtime_close(runtime,state.spawned)==JS_OWNER_OK,
            "realm owners: nested admission retains its distinct actual realm after parent unwind");
        check(js_owner_runtime_close(runtime,duplicate)==JS_OWNER_OK && js_owner_runtime_close(runtime,b)==JS_OWNER_OK,
            "realm owners: final external holders close explicitly");
        JSOwnerTicket old=alias;
        check(js_owner_runtime_primary_ticket(runtime,&state.ticket)==JS_OWNER_OK,
            "realm owners: reserve holder for root finalizer cleanup");
        state.destroying=true;state.calls=0;
        JSValue root=JS_NewObjectClass(context,owner_realm_finalizer_class);
        check(!JS_IsException(root),"realm owners: root finalizer object");JS_SetOpaque(root,&state);
        global=JS_GetGlobalObject(context);
        check(JS_SetPropertyStr(context,global,"rootOwnerFinalizer",root)==1,
            "realm owners: retain metadata holder through root destruction");JS_FreeValue(context,global);
        os64_js_destroy(runtime);
        check(state.calls==1,"realm owners: root teardown observes finalizer's explicit ticket close");
        runtime=NULL;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){1,2,1},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK,
            "realm owners: construct new actual runtime lifetime");
        check(js_owner_runtime_owner(runtime,old,&snapshot)==JS_OWNER_FOREIGN &&
            js_owner_runtime_close(runtime,old)==JS_OWNER_FOREIGN &&
            owner_realm_run(runtime,old,"throw Error('foreign')",&out)==OS64_JS_ADMISSION_REFUSED,
            "realm owners: old actual-runtime ticket cannot bind a new lifetime");
        check(js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&refused,&out)==OS64_JS_ADMISSION_REFUSED &&
            js_owner_runtime_metadata(runtime,&metadata) && metadata.summary.owners==1 && !metadata.unpublished_owners,
            "realm owners: full owner map refuses without consuming a reservation");
        check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,"realm owners: close final primary ticket");
        os64_js_destroy(runtime);

        runtime=NULL;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){3,3,1},&runtime,&out)==OS64_JS_OK,"realm owners: construct legacy/staged mapping fixture");
        os64_js_realm_t *raw=NULL,*staged=NULL;
        check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&raw,&out)==OS64_JS_OK &&
            js_owner_runtime_metadata(runtime,&metadata) && metadata.summary.unknown_escape,
            "realm owners: raw child export records compatibility before ticket conversion");
        check(js_owner_runtime_realm_ticket(runtime,raw,&a)==JS_OWNER_OK &&
            js_owner_runtime_owner(runtime,a,&snapshot)==JS_OWNER_OK && !snapshot.primary && !snapshot.staged,
            "realm owners: ABI 5 child admission maps its original retained realm");
        check(owner_realm_run(runtime,a,"globalThis.rawAlive=19",&out)==OS64_JS_OK &&
            js_owner_runtime_close(runtime,a)==JS_OWNER_OK &&
            os64_js_realm_run(raw,"if(rawAlive!==19)throw Error('legacy revoked')",os64_strlen("if(rawAlive!==19)throw Error('legacy revoked')"),"legacy.js",&out)==OS64_JS_OK &&
            os64_js_realm_context(raw,OS64_JS_ABI_ID,&out)!=NULL,
            "realm owners: ticket close preserves ABI 5 realm and context lifetime");
        check(os64_js_realm_stage(runtime,OS64_JS_ABI_ID,1,&staged,&out)==OS64_JS_OK &&
            js_owner_runtime_realm_ticket(runtime,staged,&b)==JS_OWNER_OK &&
            js_owner_runtime_owner(runtime,b,&snapshot)==JS_OWNER_OK && snapshot.staged &&
            owner_realm_run(runtime,b,"throw Error('installing')",&out)==OS64_JS_REALM_INSTALLING,
            "realm owners: staged legacy admission is mapped but not executable through a ticket");
        check(os64_js_realm_setup(staged,OS64_JS_ABI_ID,owner_realm_stage_refuse,NULL,&out)==OS64_JS_ADMISSION_REFUSED &&
            js_owner_runtime_owner(runtime,b,&snapshot)==JS_OWNER_OK && snapshot.staged && snapshot.retired &&
            owner_realm_run(runtime,b,"throw Error('failed setup')",&out)==OS64_JS_REALM_RETIRED,
            "realm owners: failed staged installation retains original mapped identity");
        raw=(void *)1;
        check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&raw,&out)==OS64_JS_ADMISSION_REFUSED && !raw &&
            js_owner_runtime_metadata(runtime,&metadata) && metadata.summary.owners==3 && metadata.assigned_owners==3 &&
            !metadata.unpublished_owners && metadata.summary.unknown_escape,
            "realm owners: selected raw admission also respects the cumulative owner bound");
        check(js_owner_runtime_close(runtime,b)==JS_OWNER_OK,"realm owners: close staged holder");
        os64_js_destroy(runtime);

        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){1,2,1},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK,
            "realm owners: construct admission refusal fixture");
        check(js_owner_runtime_run(runtime,"mismatched-ABI",primary,"",0,"bad.js",&out)==OS64_JS_ABI_MISMATCH &&
            js_owner_runtime_run(runtime,OS64_JS_ABI_ID,primary,NULL,1,"bad.js",&out)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_metadata(runtime,&metadata) && !metadata.summary.loans && !metadata.summary.unknown_escape,
            "realm owners: ABI and source refusal creates no loan or compatibility escape");
        os64_js_cancel(runtime);
        check(owner_realm_run(runtime,primary,"throw Error('cancelled')",&out)==OS64_JS_CANCELLED &&
            js_owner_runtime_metadata(runtime,&metadata) && !metadata.summary.loans &&
            js_owner_runtime_owner(runtime,primary,&snapshot)==JS_OWNER_OK &&
            owner_realm_run(runtime,primary,"throw Error('failed runtime')",&out)==OS64_JS_FAILED_RUNTIME,
            "realm owners: valid ticket cannot bypass cancellation or failed runtime");
        check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,"realm owners: cancellation preserves cleanup metadata");
        os64_js_destroy(runtime);
    }
}
#endif
