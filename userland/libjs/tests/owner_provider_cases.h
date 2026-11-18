#ifndef JS_OWNER_PROVIDER_CASES_H
#define JS_OWNER_PROVIDER_CASES_H
#include "os64/mem.h"
#include "os64/str.h"
#include "../runtime/owner_runtime.h"

typedef struct {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket,promoted;
    JSOwnerRuntimeScope *scope;
    JSContext *context;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    void (*after_source)(void *),(*before_base)(void *);
    const char *command,*dependency;
    char slow[192];
    unsigned mode,calls,resolves,loads,bases;
} OwnerProviderCase;
static JSOwnerTicket owner_provider_ticket(OwnerProviderCase *s)
{return s->promoted.serial?s->promoted:s->ticket;}
static void owner_provider_loan(OwnerProviderCase *s,const char *message)
{
    JSOwnerRuntimeMetadata m;JSOwnerId id;
    s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==1 &&
        js_owner_scope_owner(s->scope,&id)==JS_OWNER_OK && id.serial==s->ticket.owner.serial &&
        id.incarnation==s->ticket.owner.incarnation,message);
}
static void owner_provider_guard(OwnerProviderCase *s)
{
    os64_js_outcome_t original=*s->outer,nested;JSOwnerTicket live=owner_provider_ticket(s);
    s->outer->line=731;os64_js_outcome_t saved=*s->outer;
    owner_provider_loan(s,"checked providers: callbacks retain original task owner loan");
    s->check(js_owner_runtime_set_module_loader(s->runtime,OS64_JS_ABI_ID,live,NULL,&nested)==OS64_JS_BUSY &&
        js_owner_runtime_set_import_base(s->runtime,OS64_JS_ABI_ID,live,NULL,NULL,&nested)==OS64_JS_BUSY &&
        js_owner_runtime_set_module_loader(s->runtime,OS64_JS_ABI_ID,live,NULL,s->outer)==OS64_JS_BUSY &&
        !os64_memcmp(&saved,s->outer,sizeof saved),
        "checked providers: active detach refuses and preserves outer outcome");
    *s->outer=original;
}
static char *owner_provider_resolve(void *opaque,const char *base,const char *specifier)
{
    OwnerProviderCase *s=opaque;s->resolves++;owner_provider_guard(s);
    if(s->mode==1 && !s->promoted.serial)s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK &&
        js_owner_scope_promote(s->scope,&s->promoted)==JS_OWNER_OK,
        "checked providers: callback holder close preserves promotion and native source loan");
    if(s->mode==3 && s->resolves==1)s->check(!os64_strcmp(base,"https://provider.example/"),
        "checked providers: native import base reaches matching child resolver");
    size_t n=os64_strlen(specifier);char *name=os64_malloc(n+1);if(name)os64_memcpy(name,specifier,n+1);
    return name;
}
static bool owner_provider_source(void *opaque,const char *name,const char **bytes,size_t *length)
{
    OwnerProviderCase *s=opaque;s->loads++;owner_provider_guard(s);
    if(s->mode==4) {
        os64_js_outcome_t out;s->check(os64_js_context(s->runtime,OS64_JS_ABI_ID,&out)!=NULL,
            "checked providers: incidental legacy getter inside source has no exemption");
    }
    if(s->mode==5) {
        os64_js_outcome_t out;s->check(js_owner_runtime_retire(s->runtime,OS64_JS_ABI_ID,owner_provider_ticket(s),&out)==OS64_JS_OK,
            "checked providers: source callback may retire original owner");
    }
    if(s->mode==6)os64_js_cancel(s->runtime);
    if(!os64_strcmp(name,"entry"))*bytes="import {v} from 'dep';export const answer=await Promise.resolve(v+1);globalThis.providerAnswer=answer";
    else if(!os64_strcmp(name,"dep"))*bytes=s->dependency;
    else if(!os64_strcmp(name,"slow"))*bytes=s->slow;
    else return false;
    *length=os64_strlen(*bytes);if(s->after_source)s->after_source(s);return true;
}
static JSValue owner_provider_base(JSContext *context,void *opaque)
{
    OwnerProviderCase *s=opaque;s->bases++;owner_provider_guard(s);JSOwnerRuntimeMetadata m;
    s->check(context==s->context && js_owner_runtime_metadata(s->runtime,&m) && m.summary.unknown_escape,
        "checked providers: legacy base delivery marks raw veto before publishing context");
    if(s->before_base)s->before_base(s);
    return JS_NewString(context,"https://provider.example/");
}
static JSValue owner_provider_body(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerProviderCase *s=opaque;s->scope=scope;s->context=context;s->calls++;
    if(s->mode==7) {
        JSValue global=JS_GetGlobalObject(context),fn=JS_GetPropertyStr(context,global,"providerImporter");
        JS_FreeValue(context,global);
        JSValue result=JS_IsException(fn)?JS_EXCEPTION:JS_Call(context,fn,JS_UNDEFINED,0,NULL);
        JS_FreeValue(context,fn);return result;
    }
    if(s->mode==3 && !s->command) {
        const char *code="(function(specifier){return import(specifier)})";
        JSValue fn=JS_Eval(context,code,os64_strlen(code),"provider-generated.js",JS_EVAL_TYPE_GLOBAL);
        if(JS_IsException(fn))return fn;
        s->check(JS_ClearFunctionScriptProvenance(fn,code+1,os64_strlen(code)-2),
            "checked providers: host-generated import closure uses current base fallback");
        JSValue name=JS_NewString(context,"entry");
        JSValue result=JS_IsException(name)?JS_EXCEPTION:JS_Call(context,fn,JS_UNDEFINED,1,&name);
        JS_FreeValue(context,name);JS_FreeValue(context,fn);return result;
    }
    const char *code=s->command?s->command:"import('entry').then(ns=>{globalThis.observed=ns.answer})";
    return JS_Eval(context,code,os64_strlen(code),"provider-root.js",JS_EVAL_TYPE_GLOBAL);
}
static os64_js_status_t owner_provider_invoke(OwnerProviderCase *s)
{
    os64_js_status_t result=js_owner_runtime_task_run(s->runtime,OS64_JS_ABI_ID,owner_provider_ticket(s),
        "provider task",owner_provider_body,s,s->outer);
    s->scope=NULL;s->context=NULL;return result;
}
static OwnerProviderCase owner_provider_state(os64_js_runtime_t *runtime,JSOwnerTicket ticket,
    os64_js_outcome_t *out,void (*check)(bool,const char *))
{
    OwnerProviderCase s={.runtime=runtime,.ticket=ticket,.outer=out,.check=check,.dependency="export const v=41"};
    os64_strcopy(s.slow,sizeof s.slow,"export const v=await new Promise(r=>{globalThis.providerResume=r});globalThis.providerAnswer=v");return s;
}
static void owner_provider_register(OwnerProviderCase *s)
{
    os64_js_module_loader_t loader={s,owner_provider_resolve,owner_provider_source};
    s->check(js_owner_runtime_set_module_loader(s->runtime,OS64_JS_ABI_ID,owner_provider_ticket(s),&loader,s->outer)==OS64_JS_OK &&
        js_owner_runtime_set_import_base(s->runtime,OS64_JS_ABI_ID,owner_provider_ticket(s),owner_provider_base,s,s->outer)==OS64_JS_OK,
        "checked providers: checked registration publishes no raw realm or context");
}
static os64_js_status_t owner_provider_setup(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)context;OwnerProviderCase *s=opaque;s->scope=scope;os64_js_outcome_t nested;os64_js_outcome_t *saved=s->outer;s->outer=&nested;
    owner_provider_register(s);s->outer=saved;s->scope=NULL;return OS64_JS_OK;
}
static void owner_provider_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};config.limits.memory_bytes=8*1024*1024;config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime;os64_js_outcome_t out;JSOwnerTicket primary,a,b;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){3,4,1},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
            js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&a,&out)==OS64_JS_OK &&
            js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&b,&out)==OS64_JS_OK,
            "checked providers: independent child and staged provider fixture");
        if(policy)check(os64_js_enable_host_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
            "checked providers: select host settings without legacy Promise capture");
        OwnerProviderCase first=owner_provider_state(runtime,a,&out,check),second=owner_provider_state(runtime,b,&out,check);
        second.dependency="export const v=7";owner_provider_register(&first);
        check(js_owner_runtime_setup(runtime,OS64_JS_ABI_ID,b,owner_provider_setup,&second,&out)==OS64_JS_OK &&
            js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,b,&out)==OS64_JS_OK,
            "checked providers: first provider registration works inside scoped staged bootstrap");
        JSOwnerRuntimeMetadata m;check(js_owner_runtime_metadata(runtime,&m) && !m.summary.unknown_escape && !m.summary.loans &&
            !first.calls && !first.resolves && !first.loads && !first.bases,
            "checked providers: registration and activation create no callback or compatibility export");
        check(js_owner_runtime_set_module_loader(runtime,"bad ABI",a,NULL,&out)==OS64_JS_ABI_MISMATCH &&
            js_owner_runtime_set_import_base(runtime,OS64_JS_ABI_ID,a,NULL,&first,&out)==OS64_JS_BAD_ARGUMENT,
            "checked providers: malformed provider arguments refuse without callback");
        first.mode=1;
        check(owner_provider_invoke(&first)==OS64_JS_OK && first.promoted.serial && first.resolves>=2 && first.loads==2,
            "checked providers: dependency graph executes after callback closes admitting holder");
        a=first.promoted;first.command="if(observed!==42||providerAnswer!==42)throw Error('A provider graph')";
        check(owner_provider_invoke(&first)==OS64_JS_OK && !first.bases,"checked providers: child A module graph uses copied dependency and TLA");
        check(owner_provider_invoke(&second)==OS64_JS_OK && second.loads==2,"checked providers: child B has independent loader graph");
        second.command="if(observed!==8||providerAnswer!==8)throw Error('B provider graph')";
        check(owner_provider_invoke(&second)==OS64_JS_OK && js_owner_runtime_metadata(runtime,&m) && !m.summary.unknown_escape && !m.summary.loans,
            "checked providers: string and byte callbacks under checked tasks create no raw export");
        first.mode=0;first.command="import('slow').then(ns=>{globalThis.slowObserved=ns.v})";
        check(owner_provider_invoke(&first)==OS64_JS_OK && first.loads==3,"checked providers: pending TLA stores no native module handle");
        unsigned loads=first.loads;
        check(js_owner_runtime_set_module_loader(runtime,OS64_JS_ABI_ID,a,NULL,&out)==OS64_JS_OK &&
            js_owner_runtime_set_import_base(runtime,OS64_JS_ABI_ID,a,NULL,NULL,&out)==OS64_JS_OK,
            "checked providers: idle detach ends provider storage loan before late module continuation");
        os64_memset(first.slow,'!',sizeof first.slow);first.command="providerResume(99)";
        check(owner_provider_invoke(&first)==OS64_JS_OK,"checked providers: later task resumes engine-owned module source after native storage poison");
        first.command="if(slowObserved!==99||providerAnswer!==99)throw Error('borrowed source survived')";
        check(owner_provider_invoke(&first)==OS64_JS_OK && first.loads==loads,
            "checked providers: compiled pending module uses copied source without detached provider");
        check(js_owner_runtime_retire(runtime,OS64_JS_ABI_ID,b,&out)==OS64_JS_OK &&
            js_owner_runtime_set_module_loader(runtime,OS64_JS_ABI_ID,b,NULL,&out)==OS64_JS_OK &&
            js_owner_runtime_set_import_base(runtime,OS64_JS_ABI_ID,b,NULL,NULL,&out)==OS64_JS_OK &&
            js_owner_runtime_set_import_base(runtime,OS64_JS_ABI_ID,b,owner_provider_base,&second,&out)==OS64_JS_REALM_RETIRED,
            "checked providers: retired provider detaches but cannot be replaced");
        check(js_owner_runtime_close(runtime,a)==JS_OWNER_OK &&
            js_owner_runtime_set_module_loader(runtime,OS64_JS_ABI_ID,a,NULL,&out)==OS64_JS_ADMISSION_REFUSED,
            "checked providers: closed holder cannot detach retained realm provider");os64_js_destroy(runtime);

        for(unsigned mode=3;mode<=6;mode++) {
            check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,2,1},&runtime,&out)==OS64_JS_OK &&
                js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK &&
                (mode==3?os64_js_enable_import_settings:os64_js_enable_host_settings)(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                "checked providers: legacy base and callback failure fixture");
            OwnerProviderCase s=owner_provider_state(runtime,primary,&out,check);s.mode=mode;owner_provider_register(&s);
            os64_js_status_t result=owner_provider_invoke(&s);
            if(mode<=4)check(result==OS64_JS_OK && (mode==4 || s.bases==1) &&
                js_owner_runtime_metadata(runtime,&m) && m.summary.unknown_escape && !m.summary.loans,
                "checked providers: raw base delivery or incidental getter keeps sticky compatibility veto");
            else if(mode==5){JSOwnerRealmSnapshot state;check(js_owner_runtime_owner(runtime,primary,&state)==JS_OWNER_OK && state.retired && !state.failed,
                "checked providers: callback retirement preserves ordinary retained-realm state");}
            else check(result==OS64_JS_CANCELLED,"checked providers: callback cancellation keeps global failure");
            check(js_owner_runtime_set_module_loader(runtime,OS64_JS_ABI_ID,primary,NULL,&out)==OS64_JS_OK &&
                js_owner_runtime_set_import_base(runtime,OS64_JS_ABI_ID,primary,NULL,NULL,&out)==OS64_JS_OK,
                "checked providers: cancellation or retirement still permits checked cleanup detach");
            os64_js_destroy(runtime);
        }
    }
}
#endif
