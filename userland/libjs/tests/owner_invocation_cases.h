#ifndef JS_OWNER_INVOCATION_CASES_H
#define JS_OWNER_INVOCATION_CASES_H
#include "os64/str.h"
#include "../runtime/owner_runtime.h"

typedef struct OwnerInvocationCase OwnerInvocationCase;
struct OwnerInvocationCase {
    os64_js_runtime_t *runtime;
    JSOwnerTicket entry,incumbent,promoted;
    JSOwnerRuntimeScope *scope;
    os64_js_outcome_t *out,*outer;
    void (*check)(bool,const char *);
    size_t loans;
    unsigned calls,finalized,jobs;
    bool backup,close,exception,nested,queue,legacy,cancel;
};
static JSClassID owner_invocation_class;
static OwnerInvocationCase *owner_invocation_current;
static bool owner_invocation_id(JSOwnerId a,JSOwnerId b)
{return a.incarnation==b.incarnation && a.serial==b.serial;}
static void owner_invocation_live(OwnerInvocationCase *s)
{
    JSOwnerRuntimeMetadata m;JSOwnerId id;JSOwnerSettingsSnapshot entry,incumbent;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==s->loans &&
        js_owner_scope_owner(s->scope,&id)==JS_OWNER_OK && owner_invocation_id(id,s->entry.owner),
        "checked invocation: independent role loans protect original entry through synchronous cleanup");
    s->check(js_owner_scope_settings(s->scope,OS64_JS_SETTINGS_ENTRY,&entry)==JS_OWNER_OK &&
        entry.kind==JS_OWNER_SETTINGS_MAPPED && owner_invocation_id(entry.realm.owner,s->entry.owner) &&
        js_owner_scope_settings(s->scope,OS64_JS_SETTINGS_INCUMBENT,&incumbent)==JS_OWNER_OK &&
        (s->backup?incumbent.kind==JS_OWNER_SETTINGS_MAPPED && owner_invocation_id(incumbent.realm.owner,s->incumbent.owner):
            incumbent.kind==JS_OWNER_SETTINGS_ABSENT),
        "checked invocation: explicit entry and optional incumbent remain independent inside settings frame");
}
static void owner_invocation_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;OwnerInvocationCase *s=JS_GetOpaque(value,owner_invocation_class);if(!s)return;
    owner_invocation_live(s);os64_js_outcome_t out;
    s->check(os64_js_task_end(s->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
        "checked invocation: result and exception finalizers run before guard closure");
    s->finalized++;
}
static JSValue owner_invocation_string(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;owner_invocation_live(owner_invocation_current);
    return JS_NewString(context,"checked invocation exception");
}
static JSValue owner_invocation_job(JSContext *context,int argc,JSValueConst *argv)
{(void)context;(void)argc;(void)argv;owner_invocation_current->jobs++;return JS_UNDEFINED;}
static JSValue owner_invocation_body(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerInvocationCase *s=opaque;s->scope=scope;s->calls++;owner_invocation_live(s);
    s->out->host_error=417;
    os64_js_outcome_t saved=*s->out;
    s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,s->entry,NULL,owner_invocation_body,s,s->out)==OS64_JS_BUSY &&
        !os64_memcmp(s->out,&saved,sizeof saved),"checked invocation: active outcome alias refuses before clearing caller storage");
    if(s->outer) {
        saved=*s->outer;
        s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,s->entry,NULL,owner_invocation_body,s,s->outer)==OS64_JS_BUSY &&
            !os64_memcmp(s->outer,&saved,sizeof saved),"checked invocation: nested settings preserve enclosing task outcome");
    }
    if(s->nested) {
        os64_js_outcome_t out;OwnerInvocationCase child={.runtime=s->runtime,.entry=s->incumbent,.incumbent=s->entry,
            .out=&out,.outer=s->out,.check=s->check,.loans=s->loans+2,.backup=true};
        s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,child.entry,&child.incumbent,owner_invocation_body,&child,&out)==OS64_JS_OK &&
            child.calls==1 && child.finalized==1,"checked invocation: nested selected roles borrow outer guard without ending its loans");
        child=(OwnerInvocationCase){.runtime=s->runtime,.entry=s->incumbent,.incumbent=s->incumbent,
            .out=&out,.outer=s->out,.check=s->check,.loans=s->loans+1,.backup=true};
        s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,child.entry,NULL,owner_invocation_body,&child,&out)==OS64_JS_OK && child.calls==1,
            "checked invocation: null backup preserves enclosing incumbent without a new role loan");
        owner_invocation_live(s);
    }
    if(s->close) {
        s->check(js_owner_runtime_close(s->runtime,s->entry)==JS_OWNER_OK &&
            js_owner_runtime_close(s->runtime,s->incumbent)==JS_OWNER_OK && js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK,
            "checked invocation: closing both admitting holders preserves entry promotion and incumbent loan");
        os64_js_outcome_t out;
        s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,s->entry,NULL,owner_invocation_body,s,&out)==OS64_JS_ADMISSION_REFUSED,
            "checked invocation: closed entry cannot admit a replacement call");owner_invocation_live(s);
    }
    if(s->legacy) {
        os64_js_outcome_t out;
        s->check(os64_js_settings_realm(s->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)!=NULL,
            "checked invocation: incidental public getter retains its legacy borrow");
    }
    if(s->queue && JS_EnqueueJobOwned(context,context,context,owner_invocation_job,0,NULL)<0)return JS_EXCEPTION;
    owner_invocation_class=os64_js_class_id(&owner_invocation_class);
    JSClassDef definition={.class_name="CheckedSettingsResult",.finalizer=owner_invocation_finalizer};
    if(!JS_IsRegisteredClass(JS_GetRuntime(context),owner_invocation_class) &&
        JS_NewClass(JS_GetRuntime(context),owner_invocation_class,&definition)<0)return JS_EXCEPTION;
    JSValue value=JS_NewObjectClass(context,owner_invocation_class);
    if(JS_IsException(value))return value;
    JS_SetOpaque(value,s);
    if(s->cancel)os64_js_cancel(s->runtime);
    if(s->exception) {
        if(JS_SetPropertyStr(context,value,"toString",JS_NewCFunction(context,owner_invocation_string,"toString",0))<0) {
            JS_FreeValue(context,value);return JS_EXCEPTION;
        }
        return JS_Throw(context,value);
    }
    return value;
}
static JSValue owner_invocation_borrow(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;(void)context;OwnerInvocationCase *s=opaque;os64_js_outcome_t out;
    s->out=&out;s->loans=3;
    s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,s->entry,&s->incumbent,owner_invocation_body,s,&out)==OS64_JS_OK &&
        s->calls==1 && s->finalized==1 && !s->jobs && out.jobs_pending,
        "checked invocation: task borrower cleans results but leaves ordinary jobs for enclosing checkpoint");
    return JS_UNDEFINED;
}
static JSValue owner_invocation_empty(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{(void)scope;(void)context;(*(unsigned *)opaque)++;return JS_UNDEFINED;}
static os64_js_status_t owner_invocation_private(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;(void)context;OwnerInvocationCase *s=opaque;unsigned calls=0;os64_js_outcome_t out;
    s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,s->entry,&s->incumbent,owner_invocation_empty,&calls,&out)==OS64_JS_REALM_INSTALLING && !calls,
        "checked invocation: trusted setup cannot publish an ordinary settings scope");return OS64_JS_OK;
}
static void owner_invocation_destroy(os64_js_runtime_t *runtime,void (*check)(bool,const char *))
{
    os64_js_teardown_report_t report;os64_js_destroy_report(runtime,&report);
    check(!report.leaked,"checked invocation: clean root shutdown needs no leaked engine reclamation");
}
typedef struct {OwnerInvocationCase *state;unsigned depth;} OwnerInvocationNesting;
static JSValue owner_invocation_nesting(JSContext *context,void *opaque)
{
    OwnerInvocationNesting *n=opaque;JSValue value=JS_UNDEFINED;
    n->depth++;
    if(JS_RunWithSettings(context,NULL,owner_invocation_nesting,n,&value)) {
        JS_FreeValue(context,value);
    } else {
        unsigned calls=0;os64_js_outcome_t out;JSOwnerRuntimeMetadata before,after;
        n->state->check(js_owner_runtime_metadata(n->state->runtime,&before) &&
            js_owner_runtime_with_settings(n->state->runtime,OS64_JS_ABI_ID,n->state->entry,&n->state->incumbent,
                owner_invocation_empty,&calls,&out)==OS64_JS_ADMISSION_REFUSED && !calls &&
            js_owner_runtime_metadata(n->state->runtime,&after) && before.summary.loans==after.summary.loans,
            "checked invocation: engine nesting refusal unwinds both loans without callback");
    }
    return JS_UNDEFINED;
}
static JSValue owner_invocation_nested_limit(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;OwnerInvocationNesting n={opaque,0};return owner_invocation_nesting(context,&n);
}
static void owner_invocation_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};config.limits.memory_bytes=8*1024*1024;config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime;os64_js_outcome_t out;JSOwnerTicket primary,a,b,staged;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){4,8,8},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK && js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&a,&out)==OS64_JS_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&b,&out)==OS64_JS_OK && js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&staged,&out)==OS64_JS_OK,
            "checked invocation: distinct eligible and staged role owners");
        unsigned calls=0;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,owner_invocation_empty,&calls,&out)==OS64_JS_BAD_ARGUMENT && !calls,
            "checked invocation: idle runtime cannot invent a settings task");
        if(policy)check(os64_js_enable_host_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked invocation: explicit host settings opt-in");
        OwnerInvocationCase setup={.runtime=runtime,.entry=a,.incumbent=b,.check=check};
        check(js_owner_runtime_setup(runtime,OS64_JS_ABI_ID,staged,owner_invocation_private,&setup,&out)==OS64_JS_OK,
            "checked invocation: private setup refusal leaves candidate installation intact");
        check(os64_js_task_begin(runtime,OS64_JS_ABI_ID,"selected settings",&out)==OS64_JS_OK,"checked invocation: explicit task fixture");
        os64_js_runtime_t *foreign;os64_js_outcome_t foreign_out;JSOwnerTicket foreign_ticket;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,1,1},&foreign,&foreign_out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(foreign,&foreign_ticket)==JS_OWNER_OK &&
            js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,foreign_ticket,&b,owner_invocation_empty,&calls,&out)==OS64_JS_ADMISSION_REFUSED &&
            js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&foreign_ticket,owner_invocation_empty,&calls,&out)==OS64_JS_ADMISSION_REFUSED && !calls,
            "checked invocation: foreign entry or incumbent never resolve by recycled owner serial");owner_invocation_destroy(foreign,check);
        OwnerInvocationCase s={.runtime=runtime,.entry=a,.incumbent=b,.out=&out,.check=check,.loans=2,.backup=true,.nested=true};
        owner_invocation_current=&s;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,owner_invocation_body,&s,&out)==OS64_JS_OK && s.calls==1 && s.finalized==1,
            "checked invocation: explicit distinct roles run and clean result without raw export");
        s.nested=false;s.exception=true;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,owner_invocation_body,&s,&out)==OS64_JS_EXCEPTION &&
            os64_streq(out.message,"checked invocation exception") && s.finalized==2,
            "checked invocation: exception coercion and finalization complete inside selected settings");
        s.exception=false;s.backup=false;s.loans=1;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,NULL,owner_invocation_body,&s,&out)==OS64_JS_OK,
            "checked invocation: missing incumbent stays absent with one loan");
        s.backup=true;s.incumbent=a;s.loans=2;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&a,owner_invocation_body,&s,&out)==OS64_JS_OK,
            "checked invocation: same-owner roles reserve independent bounded loans");s.incumbent=b;
        s.queue=true;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,owner_invocation_body,&s,&out)==OS64_JS_OK &&
            !s.jobs && out.jobs_pending && os64_js_checkpoint(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK && s.jobs==1 && out.jobs_executed==1,
            "checked invocation: settings invocation does not checkpoint ordinary jobs");s.queue=false;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,owner_invocation_empty,&calls,&out)==OS64_JS_OK && out.jobs_executed==1,
            "checked invocation: later settings preserve cumulative task job counter");calls=0;
        check(js_owner_runtime_with_settings(runtime,"bad",a,&b,owner_invocation_empty,&calls,&out)==OS64_JS_ABI_MISMATCH &&
            js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,NULL,&calls,&out)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,staged,&b,owner_invocation_empty,&calls,&out)==OS64_JS_REALM_INSTALLING &&
            js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&staged,owner_invocation_empty,&calls,&out)==OS64_JS_REALM_INSTALLING && !calls,
            "checked invocation: malformed and staged entry or incumbent refuse before body");
        s.close=true;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,owner_invocation_body,&s,&out)==OS64_JS_OK && s.promoted.serial,
            "checked invocation: closed holders do not truncate admitted settings lifetime");a=s.promoted;
        check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,&b,owner_invocation_empty,&calls,&out)==OS64_JS_ADMISSION_REFUSED && !calls,
            "checked invocation: stale incumbent does not become absent or select a replacement");
        JSOwnerRuntimeMetadata m;
        check(js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape &&
            os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked invocation: settings return preserves host task and leaves no raw veto or loan");
        check(js_owner_runtime_duplicate(runtime,primary,&b)==JS_OWNER_OK,"checked invocation: independent primary role holder");
        s=(OwnerInvocationCase){.runtime=runtime,.entry=a,.incumbent=b,.check=check,.outer=&out,.queue=true,.backup=true};owner_invocation_current=&s;
        check(js_owner_runtime_task_run(runtime,OS64_JS_ABI_ID,primary,"borrow selected settings",owner_invocation_borrow,&s,&out)==OS64_JS_OK && s.jobs==1,
            "checked invocation: outer task alone checkpoints queued work after callback scope ends");
        s=(OwnerInvocationCase){.runtime=runtime,.entry=a,.incumbent=b,.check=check};
        check(js_owner_runtime_task_run(runtime,OS64_JS_ABI_ID,primary,"nesting bound",owner_invocation_nested_limit,&s,&out)==OS64_JS_OK,
            "checked invocation: engine settings refusal preserves enclosing task");
        check(js_owner_runtime_retire(runtime,OS64_JS_ABI_ID,a,&out)==OS64_JS_OK &&
            os64_js_task_begin(runtime,OS64_JS_ABI_ID,"retired role refusal",&out)==OS64_JS_OK &&
            js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,primary,&a,owner_invocation_empty,&calls,&out)==OS64_JS_REALM_RETIRED && !calls &&
            js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,a,NULL,owner_invocation_empty,&calls,&out)==OS64_JS_REALM_RETIRED &&
            os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked invocation: either retired role refuses without ending host task");
        owner_invocation_destroy(runtime,check);
        for(unsigned mode=0;mode<2;mode++) {
            check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,1,2},&runtime,&out)==OS64_JS_OK &&
                js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK && os64_js_task_begin(runtime,OS64_JS_ABI_ID,"retention and cancellation",&out)==OS64_JS_OK,
                "checked invocation: legacy retention and sticky unwind fixture");
            s=(OwnerInvocationCase){.runtime=runtime,.entry=primary,.incumbent=primary,.out=&out,.check=check,.loans=2,.backup=true,
                .legacy=mode==0,.cancel=mode==1};owner_invocation_current=&s;
            check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,primary,&primary,owner_invocation_body,&s,&out)==(mode?OS64_JS_CANCELLED:OS64_JS_OK) &&
                s.finalized==1 && js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && m.summary.unknown_escape==(mode==0),
                "checked invocation: legacy raw retention survives and cancellation still cleans both roles");
            check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==(mode?OS64_JS_CANCELLED:OS64_JS_OK),
                "checked invocation: caller closes its original task after scoped failure");owner_invocation_destroy(runtime,check);
        }
        for(size_t capacity=1;capacity<=2;capacity++) {
            check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,1,capacity},&runtime,&out)==OS64_JS_OK &&
                js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK && os64_js_task_begin(runtime,OS64_JS_ABI_ID,"partial loans",&out)==OS64_JS_OK,
                "checked invocation: finite role-loan capacity fixture");calls=0;
            os64_js_status_t result=js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,primary,&primary,owner_invocation_empty,&calls,&out);
            check(result==(capacity==1?OS64_JS_ADMISSION_REFUSED:OS64_JS_OK) && calls==(capacity==1?0u:1u) &&
                js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape &&
                js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,primary,NULL,owner_invocation_empty,&calls,&out)==OS64_JS_OK &&
                os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                "checked invocation: partial role-loan refusal rolls back entry and preserves task capacity");owner_invocation_destroy(runtime,check);
        }
    }
    owner_invocation_current=NULL;
}
#endif
