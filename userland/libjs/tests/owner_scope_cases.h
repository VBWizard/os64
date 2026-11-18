#ifndef JS_OWNER_SCOPE_CASES_H
#define JS_OWNER_SCOPE_CASES_H
#include "os64/js_engine.h"
#include "os64/js_callbacks.h"
#include "os64/str.h"
#include "quickjs.h"
#include "../runtime/owner_runtime.h"

typedef struct {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket, promoted;
    JSOwnerId owner;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    JSOwnerRuntimeScope *scope;
    JSContext *exported;
    const char *source;
    unsigned mode, calls, finalized, jobs;
} OwnerScopeCase;
static OwnerScopeCase *owner_scope_active;
static JSClassID owner_scope_class;
static void owner_scope_finalizer(JSRuntime *engine, JSValue value)
{
    (void)engine;
    OwnerScopeCase *s=JS_GetOpaque(value,owner_scope_class);
    if(!s)return;
    JSOwnerRuntimeMetadata m;JSOwnerId id;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1 &&
        js_owner_scope_owner(s->scope,&id)==JS_OWNER_OK && id.serial==s->owner.serial,
        "owner scopes: returned value or diagnostic finalizer retains its original loan");
    s->finalized++;
}
static JSValue owner_scope_job(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)context;(void)self;(void)argc;(void)argv;
    OwnerScopeCase *s=owner_scope_active;
    JSOwnerRuntimeMetadata m;JSOwnerId id;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1 &&
        js_owner_scope_owner(s->scope,&id)==JS_OWNER_OK && id.serial==s->owner.serial,
        "owner scopes: scoped loan spans queued native callback execution");
    s->jobs++;return JS_UNDEFINED;
}
static JSValue owner_scope_body(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerScopeCase *s=opaque;s->calls++;s->scope=scope;
    JSOwnerRuntimeMetadata m;JSOwnerId id;
    s->check(js_owner_scope_owner(scope,&id)==JS_OWNER_OK && id.incarnation==s->owner.incarnation &&
        id.serial==s->owner.serial && js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1,
        "owner scopes: callback context is protected by the original owner's admitted loan");
    if(s->mode==1) {
        JSOwnerTicket untouched={{81,82},83};
        s->check(js_owner_scope_promote(scope,&untouched)==JS_OWNER_CAPACITY && untouched.serial==83 &&
            js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1,
            "owner scopes: full holder pool refuses promotion without ending loan");
        s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK &&
            js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK && s->promoted.serial>s->ticket.serial &&
            s->promoted.owner.serial==s->owner.serial,
            "owner scopes: reentrant close preserves explicit promotion from live loan");
        s->check(js_owner_runtime_duplicate(s->runtime,s->ticket,&untouched)==JS_OWNER_STALE && untouched.serial==83,
            "owner scopes: promotion does not revive closed admitting ticket");
        os64_js_outcome_t saved=*s->outer,nested;
        s->check(js_owner_runtime_scoped_run(s->runtime,OS64_JS_ABI_ID,s->promoted,"nested",
            owner_scope_body,s,s->outer)==OS64_JS_BUSY && !os64_memcmp(&saved,s->outer,sizeof saved),
            "owner scopes: scoped reentry protects outer outcome alias");
        s->check(js_owner_runtime_scoped_run(s->runtime,OS64_JS_ABI_ID,s->promoted,"nested",
            owner_scope_body,s,&nested)==OS64_JS_BUSY &&
            js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1,
            "owner scopes: nested invocation is BUSY without renewing budget or loan");
    } else if(s->mode==2 || s->mode==3) {
        owner_scope_class=os64_js_class_id(&owner_scope_class);
        JSClassDef definition={.class_name="ScopedCleanup",.finalizer=owner_scope_finalizer};
        s->check(JS_IsRegisteredClass(JS_GetRuntime(context),owner_scope_class) ||
            JS_NewClass(JS_GetRuntime(context),owner_scope_class,&definition)==0,
            "owner scopes: install scoped returned-value cleanup oracle");
        JSValue value=JS_NewObjectClass(context,owner_scope_class);
        if(JS_IsException(value))return value;
        JS_SetOpaque(value,s);
        s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK,
            "owner scopes: close holder before returned-value cleanup");
        return s->mode==3 ? JS_Throw(context,value) : value;
    } else if(s->mode==4) {
        JSContext *view=(void *)1;
        s->check(js_owner_scope_export(scope,NULL)==JS_OWNER_INVALID,
            "owner scopes: explicit export requires distinct output storage");
        s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK &&
            js_owner_scope_export(scope,&view)==JS_OWNER_OK && view==context &&
            js_owner_runtime_metadata(s->runtime,&m) && m.summary.unknown_escape,
            "owner scopes: explicit export records sticky veto before publishing context after close");
        s->exported=view;
    } else if(s->mode==5) {
        os64_js_outcome_t out;
        s->check(os64_js_context(s->runtime,OS64_JS_ABI_ID,&out)!=NULL &&
            js_owner_runtime_metadata(s->runtime,&m) && m.summary.unknown_escape,
            "owner scopes: public raw getter inside scope has no ambient exemption");
    } else if(s->mode==6) {
        JSValue global=JS_GetGlobalObject(context);
        int result=JS_SetPropertyStr(context,global,"scopedJob",JS_NewCFunction(context,owner_scope_job,"scopedJob",0));
        JS_FreeValue(context,global);
        if(result<0)return JS_EXCEPTION;
    } else if(s->mode==7) {
        os64_js_cancel(s->runtime);
        JSContext *view=(void *)1;
        s->check(js_owner_scope_export(scope,&view)==JS_OWNER_INVALID && view==(void *)1 &&
            js_owner_runtime_metadata(s->runtime,&m) && !m.summary.unknown_escape,
            "owner scopes: failed getter admission publishes no explicit export or veto");
    }
    return s->source ? JS_Eval(context,s->source,os64_strlen(s->source),"scope.js",JS_EVAL_TYPE_GLOBAL) : JS_UNDEFINED;
}
static os64_js_status_t owner_scope_invoke(OwnerScopeCase *s)
{
    return js_owner_runtime_scoped_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"scope.js",owner_scope_body,s,s->outer);
}
static void owner_scope_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};
    config.limits.memory_bytes=8*1024*1024;config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){3,3,2},&runtime,&out)==OS64_JS_OK,"owner scopes: construct native scope fixture");
        JSOwnerTicket primary,a,b;
        check(js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&a,&out)==OS64_JS_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&b,&out)==OS64_JS_OK,
            "owner scopes: admit distinct primary and child owners");
        check(os64_js_enable_host_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
            "owner scopes: select existing settings without exporting context");
        OwnerScopeCase s={.runtime=runtime,.ticket=a,.owner=a.owner,.outer=&out,.check=check,
            .source="globalThis.scopeName='A';({answer:42})"};
        check(owner_scope_invoke(&s)==OS64_JS_OK,"owner scopes: native body returns owned engine value for wrapper cleanup");
        s.ticket=b;s.owner=b.owner;s.source="globalThis.scopeName='B'";
        check(owner_scope_invoke(&s)==OS64_JS_OK,"owner scopes: native scoped child B writes its own global");
        s.ticket=a;s.owner=a.owner;s.source="if(scopeName!=='A')throw Error('scope mapping')";
        check(owner_scope_invoke(&s)==OS64_JS_OK,"owner scopes: native scope uses original mapped child context");
        JSOwnerRuntimeMetadata m;
        check(js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
            "owner scopes: explicit scoped settings and values create no legacy export");
        unsigned before_calls=s.calls;
        check(js_owner_runtime_scoped_run(runtime,"wrong ABI",a,"bad",owner_scope_body,&s,&out)==OS64_JS_ABI_MISMATCH &&
            js_owner_runtime_scoped_run(runtime,OS64_JS_ABI_ID,a,"bad",NULL,&s,&out)==OS64_JS_BAD_ARGUMENT &&
            js_owner_runtime_scoped_run(runtime,OS64_JS_ABI_ID,a,NULL,owner_scope_body,&s,&out)==OS64_JS_BAD_ARGUMENT &&
            s.calls==before_calls && js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
            "owner scopes: ABI and body/name refusal publish no borrowed view or loan");
        s.mode=1;s.source="globalThis.afterPromotion=42";
        check(owner_scope_invoke(&s)==OS64_JS_OK && s.calls==4,"owner scopes: close and promotion preserve admitted callback completion");
        s.ticket=s.promoted;s.mode=6;s.source="Promise.resolve().then(scopedJob)";owner_scope_active=&s;
        check(owner_scope_invoke(&s)==OS64_JS_OK && s.jobs==1,"owner scopes: complete-run drains jobs before scope loan ends");owner_scope_active=NULL;
        check(js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
            "owner scopes: no implicit raw export from scoped body or complete-run drain");
        s.mode=2;s.source=NULL;
        check(owner_scope_invoke(&s)==OS64_JS_OK && s.finalized==1,
            "owner scopes: returned engine value finalizer runs before loan end");
        check(js_owner_runtime_primary_ticket(runtime,&s.ticket)==JS_OWNER_OK,
            "owner scopes: reserve primary holder for diagnostic cleanup");
        s.owner=s.ticket.owner;s.mode=3;
        check(owner_scope_invoke(&s)==OS64_JS_EXCEPTION && s.finalized==2,
            "owner scopes: exception diagnostic and native finalizer cleanup retain admitted loan");
        check(js_owner_runtime_metadata(runtime,&m) && !m.summary.loans && !m.summary.unknown_escape,
            "owner scopes: cleanup of closed holders leaves no loan or compatibility escape");
        check(js_owner_runtime_retire(runtime,OS64_JS_ABI_ID,b,&out)==OS64_JS_OK,
            "owner scopes: retire original child B");
        s.ticket=b;s.owner=b.owner;unsigned calls=s.calls;
        check(owner_scope_invoke(&s)==OS64_JS_REALM_RETIRED && s.calls==calls,
            "owner scopes: valid ticket cannot invoke a retired native scope");
        s.ticket=primary;s.owner=primary.owner;s.mode=4;
        check(owner_scope_invoke(&s)==OS64_JS_OK && s.exported,
            "owner scopes: explicitly export a closed holder's admitted context");
        check(js_owner_runtime_close(runtime,b)==JS_OWNER_OK,"owner scopes: close remaining retired holder");
        const char *legacy="if(typeof scopeName!=='undefined')throw Error('primary')";
        JSValue value=JS_Eval(s.exported,legacy,os64_strlen(legacy),"legacy.js",JS_EVAL_TYPE_GLOBAL);
        check(!JS_IsException(value),"owner scopes: explicit ABI 5 context export survives scope and holder close");JS_FreeValue(s.exported,value);
        check(js_owner_runtime_metadata(runtime,&m) && !m.summary.tickets && m.summary.unknown_escape,
            "owner scopes: compatibility veto survives zero holders and completed loan");
        os64_js_destroy(runtime);

        for(unsigned mode=5;mode<=7;mode+=2) {
            runtime=NULL;
            check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
                (JSOwnerLimits){1,2,1},&runtime,&out)==OS64_JS_OK &&
                js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK,"owner scopes: export/admission fixture");
            s=(OwnerScopeCase){.runtime=runtime,.ticket=primary,.owner=primary.owner,.outer=&out,.check=check,.mode=mode};
            check(owner_scope_invoke(&s)==(mode==7?OS64_JS_CANCELLED:OS64_JS_OK),
                "owner scopes: native cancellation and incidental legacy export retain existing results");
            check(js_owner_runtime_close(runtime,primary)==JS_OWNER_OK,"owner scopes: close export/admission holder");
            os64_js_destroy(runtime);
        }
        runtime=NULL;os64_js_realm_t *staged=NULL;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){2,2,1},&runtime,&out)==OS64_JS_OK &&
            os64_js_realm_stage(runtime,OS64_JS_ABI_ID,1,&staged,&out)==OS64_JS_OK &&
            js_owner_runtime_realm_ticket(runtime,staged,&a)==JS_OWNER_OK,"owner scopes: staged compatibility fixture");
        s=(OwnerScopeCase){.runtime=runtime,.ticket=a,.owner=a.owner,.outer=&out,.check=check};
        check(owner_scope_invoke(&s)==OS64_JS_REALM_INSTALLING && !s.calls &&
            js_owner_runtime_metadata(runtime,&m) && !m.summary.loans,
            "owner scopes: staged native context cannot be exposed through a valid ticket");
        check(js_owner_runtime_close(runtime,a)==JS_OWNER_OK,"owner scopes: close staged holder");os64_js_destroy(runtime);
        check(js_owner_scope_owner(NULL,NULL)==JS_OWNER_INVALID && js_owner_scope_promote(NULL,NULL)==JS_OWNER_INVALID &&
            js_owner_scope_export(NULL,NULL)==JS_OWNER_INVALID,"owner scopes: null scope operations refuse");
    }
}
#endif
