#ifndef JS_OWNER_SETTINGS_CASES_H
#define JS_OWNER_SETTINGS_CASES_H
#include "os64/str.h"
#include "../runtime/owner_runtime.h"

typedef struct {
    os64_js_runtime_t *runtime;
    JSOwnerTicket ticket,promoted,child,child_promoted;
    JSOwnerRuntimeScope *scope;
    JSContext *parent;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    JSOwnerSettingsSnapshot copied,staged_copy;
    unsigned mode,calls,finalized,native_calls;
} OwnerSettingsCase;
static OwnerSettingsCase *owner_settings_current;
static JSClassID owner_settings_class;
static bool owner_settings_id(JSOwnerId a,JSOwnerId b)
{return a.incarnation==b.incarnation && a.serial==b.serial;}
static JSOwnerSettingsSnapshot owner_settings_expect(OwnerSettingsCase *s,JSOwnerRuntimeScope *scope,
    os64_js_settings_role_t role,JSOwnerId admitting,JSOwnerSettingsKind kind,JSOwnerId selected)
{
    JSOwnerSettingsSnapshot out;
    s->check(js_owner_scope_settings(scope,role,&out)==JS_OWNER_OK && out.kind==kind &&
        owner_settings_id(out.admitting_owner,admitting) &&
        (kind==JS_OWNER_SETTINGS_MAPPED?owner_settings_id(out.realm.owner,selected):
            (!out.realm.owner.incarnation&&!out.realm.owner.serial&&!out.realm.primary&&!out.realm.staged&&!out.realm.retired&&!out.realm.failed)),
        "checked settings: selected role and admitting owner have explicit independent scalar identity");
    return out;
}
static void owner_settings_refuse(OwnerSettingsCase *s,JSOwnerRuntimeScope *scope,os64_js_settings_role_t role)
{
    JSOwnerSettingsSnapshot out,saved;os64_memset(&out,0xa5,sizeof out);saved=out;
    s->check(js_owner_scope_settings(scope,role,&out)==JS_OWNER_INVALID && !os64_memcmp(&out,&saved,sizeof out),
        "checked settings: invalid role or missing wrapper frame preserves caller output");
}
static void owner_settings_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;OwnerSettingsCase *s=JS_GetOpaque(value,owner_settings_class);if(!s)return;
    JSOwnerId id;
    s->check(js_owner_scope_owner(s->scope,&id)==JS_OWNER_OK && owner_settings_id(id,s->ticket.owner),
        "checked settings: cleanup retains original loan after holder close");
    owner_settings_refuse(s,s->scope,OS64_JS_SETTINGS_ENTRY);s->finalized++;
}
static JSValue owner_settings_foreign(JSContext *context,void *opaque)
{
    (void)context;OwnerSettingsCase *s=opaque;
    owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_ENTRY,s->child.owner,JS_OWNER_SETTINGS_UNMAPPED,(JSOwnerId){0});
    owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_CURRENT,s->child.owner,JS_OWNER_SETTINGS_UNMAPPED,(JSOwnerId){0});
    owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_INCUMBENT,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    return JS_UNDEFINED;
}
static JSValue owner_settings_staged(JSContext *context,void *opaque)
{
    OwnerSettingsCase *s=opaque;
    s->staged_copy=owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_ENTRY,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->child.owner);
    s->check(s->staged_copy.realm.staged && !s->staged_copy.realm.primary,
        "checked settings: staged realm facts grant no execution or target publication");
    owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_CURRENT,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->child.owner);
    owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_INCUMBENT,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_NATIVE_CALLER,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    JSContext *foreign=JS_NewContext(JS_GetRuntime(context));
    if(!foreign)return JS_ThrowOutOfMemory(context);
    JSValue value=JS_UNDEFINED;
    s->check(JS_RunWithSettings(foreign,s->parent,owner_settings_foreign,s,&value),
        "checked settings: temporary unmapped engine context supplies actual entry/current frames");
    JS_FreeValue(context,value);JS_FreeContext(foreign);
    owner_settings_expect(s,s->scope,OS64_JS_SETTINGS_ENTRY,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->child.owner);
    return JS_UNDEFINED;
}
static os64_js_status_t owner_settings_setup(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerSettingsCase *s=opaque;JSOwnerRuntimeScope *parent_scope=s->scope;s->scope=scope;
    owner_settings_expect(s,scope,OS64_JS_SETTINGS_ENTRY,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    owner_settings_expect(s,scope,OS64_JS_SETTINGS_CURRENT,s->child.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    JSValue result=JS_UNDEFINED;
    s->check(JS_RunWithSettings(context,s->parent,owner_settings_staged,s,&result),
        "checked settings: trusted staged bootstrap observes declared engine frames without publishing pointers");
    bool failed=JS_IsException(result);JS_FreeValue(context,result);
    s->check(js_owner_scope_promote(scope,&s->child_promoted)==JS_OWNER_OK &&
        owner_settings_id(s->child_promoted.owner,s->child.owner) && js_owner_runtime_close(s->runtime,s->child)==JS_OWNER_OK,
        "checked settings: observations never retarget loan promotion to a selected foreign owner");
    os64_js_outcome_t out;
    s->check(!os64_js_settings_realm(s->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out) && out.status==OS64_JS_REALM_INSTALLING,
        "checked settings: public raw query retains private-setup refusal");
    s->scope=parent_scope;return failed?OS64_JS_EXCEPTION:OS64_JS_OK;
}
static JSValue owner_settings_native(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)context;(void)self;(void)argc;(void)argv;OwnerSettingsCase *s=owner_settings_current;s->native_calls++;
    const os64_js_settings_role_t roles[]={OS64_JS_SETTINGS_ENTRY,OS64_JS_SETTINGS_CURRENT,OS64_JS_SETTINGS_INCUMBENT,OS64_JS_SETTINGS_NATIVE_CALLER};
    for(unsigned i=0;i<4;i++)owner_settings_expect(s,s->scope,roles[i],s->ticket.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    os64_js_outcome_t nested;
    s->check(js_owner_runtime_setup(s->runtime,OS64_JS_ABI_ID,s->child,owner_settings_setup,s,&nested)==OS64_JS_OK,
        "checked settings: nested candidate borrows parent frame under its independent owner loan");return JS_UNDEFINED;
}
static JSValue owner_settings_body(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerSettingsCase *s=opaque;s->scope=scope;s->parent=context;s->calls++;
    if(s->mode==1){owner_settings_refuse(s,scope,OS64_JS_SETTINGS_ENTRY);return JS_UNDEFINED;}
    s->copied=owner_settings_expect(s,scope,OS64_JS_SETTINGS_ENTRY,s->ticket.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    owner_settings_expect(s,scope,OS64_JS_SETTINGS_CURRENT,s->ticket.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    owner_settings_expect(s,scope,OS64_JS_SETTINGS_INCUMBENT,s->ticket.owner,JS_OWNER_SETTINGS_ABSENT,(JSOwnerId){0});
    owner_settings_expect(s,scope,OS64_JS_SETTINGS_NATIVE_CALLER,s->ticket.owner,JS_OWNER_SETTINGS_ABSENT,(JSOwnerId){0});
    owner_settings_refuse(s,scope,(os64_js_settings_role_t)99);
    s->check(js_owner_scope_settings(scope,OS64_JS_SETTINGS_ENTRY,NULL)==JS_OWNER_INVALID,
        "checked settings: missing caller output refuses");
    JS_Throw(context,JS_NewInt32(context,77));
    owner_settings_expect(s,scope,OS64_JS_SETTINGS_ENTRY,s->ticket.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    JSValue pending=JS_GetException(context);
    s->check(JS_IsNumber(pending) && JS_VALUE_GET_INT(pending)==77,
        "checked settings: observation preserves the engine's pending exception");JS_FreeValue(context,pending);
    if(s->mode==2)return JS_UNDEFINED;
    if(s->mode==3) {
        os64_js_outcome_t out;JSOwnerRuntimeMetadata m;
        s->check(os64_js_settings_realm(s->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)!=NULL &&
            js_owner_runtime_metadata(s->runtime,&m) && m.summary.unknown_escape,
            "checked settings: legacy raw query records compatibility retention");
        owner_settings_expect(s,scope,OS64_JS_SETTINGS_ENTRY,s->ticket.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
        s->check(js_owner_runtime_metadata(s->runtime,&m) && m.summary.unknown_escape,
            "checked settings: scalar observation never clears a previous raw veto");return JS_UNDEFINED;
    }
    s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK && js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK,
        "checked settings: holder close leaves original observation loan and promotion intact");
    JSValue global=JS_GetGlobalObject(context);
    int ready=JS_SetPropertyStr(context,global,"observeSettings",JS_NewCFunction(context,owner_settings_native,"observeSettings",0));
    JS_FreeValue(context,global);if(ready<0)return JS_EXCEPTION;
    const char *code="observeSettings()";JSValue value=JS_Eval(context,code,os64_strlen(code),"owner-settings.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value))return value;
    JS_FreeValue(context,value);
    os64_js_outcome_t nested;
    s->check(js_owner_runtime_retire(s->runtime,OS64_JS_ABI_ID,s->promoted,&nested)==OS64_JS_OK,
        "checked settings: retire selected realm during synchronous metadata cleanup");
    s->copied=owner_settings_expect(s,scope,OS64_JS_SETTINGS_ENTRY,s->ticket.owner,JS_OWNER_SETTINGS_MAPPED,s->ticket.owner);
    s->check(s->copied.realm.retired && !s->copied.realm.failed,
        "checked settings: scalar lifecycle observation does not grant eligibility");
    s->check(!os64_js_settings_realm(s->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&nested) && nested.status==OS64_JS_REALM_RETIRED,
        "checked settings: public raw query retains retired-realm refusal");
    owner_settings_class=os64_js_class_id(&owner_settings_class);
    JSClassDef definition={.class_name="SettingsObservationCleanup",.finalizer=owner_settings_finalizer};
    if(!JS_IsRegisteredClass(JS_GetRuntime(context),owner_settings_class) && JS_NewClass(JS_GetRuntime(context),owner_settings_class,&definition)<0)return JS_EXCEPTION;
    value=JS_NewObjectClass(context,owner_settings_class);if(!JS_IsException(value))JS_SetOpaque(value,s);return value;
}
static os64_js_status_t owner_settings_invoke(OwnerSettingsCase *s)
{
    owner_settings_current=s;
    os64_js_status_t result=js_owner_runtime_task_run(s->runtime,OS64_JS_ABI_ID,s->ticket,"settings observations",owner_settings_body,s,s->outer);
    s->scope=NULL;s->parent=NULL;owner_settings_current=NULL;return result;
}
static void owner_settings_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};config.limits.memory_bytes=8*1024*1024;config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        os64_js_runtime_t *runtime;os64_js_outcome_t out;JSOwnerTicket primary,a,b;
        check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){3,4,2},&runtime,&out)==OS64_JS_OK &&
            js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK && js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&a,&out)==OS64_JS_OK &&
            js_owner_runtime_stage(runtime,OS64_JS_ABI_ID,1,&b,&out)==OS64_JS_OK && os64_js_enable_host_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
            "checked settings: mapped child and staged independent owner fixture");
        OwnerSettingsCase s={.runtime=runtime,.ticket=a,.child=b,.outer=&out,.check=check};
        check(owner_settings_invoke(&s)==OS64_JS_OK && s.native_calls==1 && s.finalized==1 && s.child_promoted.serial,
            "checked settings: original owner observations span actual native and nested engine frames");
        JSOwnerSettingsSnapshot copied=s.copied,staged=s.staged_copy;
        check(js_owner_runtime_activate(runtime,OS64_JS_ABI_ID,s.child_promoted,&out)==OS64_JS_OK,
            "checked settings: successful candidate activates independently of copied settings facts");
        OwnerSettingsCase next={.runtime=runtime,.ticket=s.child_promoted,.outer=&out,.check=check,.mode=2};
        check(owner_settings_invoke(&next)==OS64_JS_OK && !next.copied.realm.staged && staged.realm.staged,
            "checked settings: copied lifecycle facts are immutable after later activation");
        JSOwnerRuntimeMetadata m;
        check(js_owner_runtime_metadata(runtime,&m) && !m.summary.unknown_escape && !m.summary.loans,
            "checked settings: numeric facts export no pointers or persistent holders");os64_js_destroy(runtime);
        check(owner_settings_id(copied.admitting_owner,a.owner) && owner_settings_id(copied.realm.owner,a.owner) && copied.realm.retired &&
            owner_settings_id(staged.realm.owner,b.owner) && staged.realm.staged,
            "checked settings: copied scalar identities and lifecycle facts survive root destruction");
        for(unsigned mode=1;mode<=3;mode+=2) {
            check(js_owner_runtime_create(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,2,1},&runtime,&out)==OS64_JS_OK &&
                js_owner_runtime_primary_ticket(runtime,&primary)==JS_OWNER_OK,"checked settings: frame refusal and legacy query fixture");
            if(mode==3)check(os64_js_enable_host_settings(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked settings: select explicit host settings");
            s=(OwnerSettingsCase){.runtime=runtime,.ticket=primary,.outer=&out,.check=check,.mode=mode};
            check(owner_settings_invoke(&s)==OS64_JS_OK,"checked settings: missing-frame and raw-query behavior remains independent");os64_js_destroy(runtime);
        }
    }
    JSOwnerSettingsSnapshot sentinel;os64_memset(&sentinel,0xa5,sizeof sentinel);
    check(js_owner_scope_settings(NULL,OS64_JS_SETTINGS_ENTRY,&sentinel)==JS_OWNER_INVALID,
        "checked settings: null scope refuses without a borrowed context");
}
#endif
