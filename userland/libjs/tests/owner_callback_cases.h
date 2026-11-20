#ifndef JS_OWNER_CALLBACK_CASES_H
#define JS_OWNER_CALLBACK_CASES_H
#include "os64/str.h"
#include "../runtime/owner_runtime.h"

typedef struct OwnerCallbackCase OwnerCallbackCase;
typedef struct {OwnerCallbackCase *state;JSOwnerCallbackKey key;unsigned index;} OwnerCallbackRoot;
struct OwnerCallbackCase {
    os64_js_runtime_t *runtime;
    JSOwnerTicket primary,incumbent,entry,code,promoted;
    JSOwnerCallbackKey keys[3],replacement;
    JSOwnerRuntimeScope *scope;
    JSContext *capture_context;
    JSValueConst *capture_value;
    os64_js_outcome_t *outer;
    void (*check)(bool,const char *);
    OwnerCallbackRoot roots[3];
    size_t expected_live,external_holders;
    unsigned capture_at,calls,native_calls,finalized,mode;
    bool nested_clear,destroying;
};
static JSClassID owner_callback_class;
static OwnerCallbackCase *owner_callback_current;
static void (*owner_callback_loan_probe)(OwnerCallbackCase *);
static void (*owner_callback_teardown_probe)(os64_js_runtime_t *);
static bool owner_callback_id(JSOwnerId a,JSOwnerId b)
{return a.incarnation==b.incarnation && a.serial==b.serial;}
static void owner_callback_roles(OwnerCallbackCase *s,JSOwnerRuntimeScope *scope)
{
    JSOwnerSettingsSnapshot entry,incumbent;JSOwnerId owner;
    s->check(js_owner_scope_owner(scope,&owner)==JS_OWNER_OK && owner_callback_id(owner,s->entry.owner) &&
        js_owner_scope_settings(scope,OS64_JS_SETTINGS_ENTRY,&entry)==JS_OWNER_OK && entry.kind==JS_OWNER_SETTINGS_MAPPED &&
        owner_callback_id(entry.realm.owner,s->entry.owner) &&
        js_owner_scope_settings(scope,OS64_JS_SETTINGS_INCUMBENT,&incumbent)==JS_OWNER_OK && incumbent.kind==JS_OWNER_SETTINGS_MAPPED &&
        owner_callback_id(incumbent.realm.owner,s->incumbent.owner),
        "checked callbacks: actual entry and captured incumbent are independent original owners");
}
static JSValue owner_callback_never(JSOwnerRuntimeScope *scope,JSContext *context,JSValueConst callback,void *opaque)
{(void)scope;(void)context;(void)callback;(*(unsigned *)opaque)++;return JS_UNDEFINED;}
static void owner_callback_finalizer(JSRuntime *engine,JSValue value)
{
    (void)engine;OwnerCallbackRoot *root=JS_GetOpaque(value,owner_callback_class);if(!root)return;
    OwnerCallbackCase *s=root->state;JSOwnerCallbackSummary counts;JSOwnerRuntimeMetadata m;
    s->check(js_owner_callback_summary(s->runtime,&counts) && js_owner_runtime_metadata(s->runtime,&m) &&
        (counts.closing || counts.invocations) && m.summary.tickets==s->external_holders+counts.live+counts.closing,
        "checked callbacks: incumbent holder and bounded storage survive value finalizer reentry");
    s->check(js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,root->key)==JS_OWNER_STALE,
        "checked callbacks: key invalidation precedes last-reference finalization");
    unsigned calls=0;os64_js_outcome_t out;
    os64_js_status_t result=js_owner_callback_invoke(s->runtime,OS64_JS_ABI_ID,s->keys[1],s->entry,owner_callback_never,&calls,&out);
    s->check((result==OS64_JS_BUSY || result==OS64_JS_FAILED_RUNTIME) && !calls,
        "checked callbacks: cleanup guard refuses a replacement invocation");
    if(s->scope)owner_callback_roles(s,s->scope);
    if(s->nested_clear && root->index==0) {
        JSOwnerStatus closed=js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,s->keys[1]);
        s->check(closed==(s->destroying?JS_OWNER_STALE:JS_OWNER_OK),
            "checked callbacks: nested close invalidates another key without recursive release");
        s->check(s->finalized==0,"checked callbacks: nested payload finalizer waits for outer cleanup loop");
    }
    s->finalized++;
}
static JSValue owner_callback_native(JSContext *context,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)self;(void)argc;(void)argv;(void)magic;OwnerCallbackCase *s=owner_callback_current;
    s->native_calls++;owner_callback_roles(s,s->scope);
    JSOwnerSettingsSnapshot current;
    s->check(js_owner_scope_settings(s->scope,OS64_JS_SETTINGS_CURRENT,&current)==JS_OWNER_OK &&
        current.kind==JS_OWNER_SETTINGS_MAPPED && owner_callback_id(current.realm.owner,s->code.owner),
        "checked callbacks: wrapped executable context differs from associated entry and captured incumbent");
    s->check(JS_GetOpaque(data[0],owner_callback_class)!=NULL,"checked callbacks: managed C root retains actual callback native data");
    return JS_NewInt32(context,77);
}
static JSValue owner_callback_capture_here(JSContext *context,void *opaque)
{
    OwnerCallbackCase *s=opaque;JSValue global=JS_GetGlobalObject(context),proxy=JS_GetPropertyStr(context,global,"Proxy");
    JS_FreeValue(context,global);if(JS_IsException(proxy))return proxy;
    JSValue handler=JS_NewObject(context);
    if(JS_IsException(handler)){JS_FreeValue(context,proxy);return handler;}
    JSValue args[2]={*s->capture_value,handler};JSValue value=JS_CallConstructor(context,proxy,2,args);
    JS_FreeValue(context,handler);JS_FreeValue(context,proxy);if(JS_IsException(value))return value;
    s->check(JS_GetCallbackRealm(context,value)==context,"checked callbacks: proxy associated realm is its constructor owner");
    JSOwnerSettingsSnapshot facts;
    s->check(js_owner_scope_settings(s->scope,OS64_JS_SETTINGS_INCUMBENT,&facts)==JS_OWNER_OK &&
        owner_callback_id(facts.realm.owner,s->incumbent.owner),"checked callbacks: capture reads conversion-time incumbent from engine settings");
    JS_Throw(context,JS_NewInt32(context,123));
    s->check(js_owner_callback_capture(s->scope,OS64_JS_ABI_ID,value,&s->keys[s->capture_at])==JS_OWNER_OK,
        "checked callbacks: capture reserves independent incumbent holder and callback key");
    JSValue pending=JS_GetException(context);
    s->check(JS_IsNumber(pending) && JS_VALUE_GET_INT(pending)==123,
        "checked callbacks: allocation-free capture preserves pending exception");JS_FreeValue(context,pending);
    s->roots[s->capture_at].key=s->keys[s->capture_at];
    JS_FreeValue(context,value);return JS_UNDEFINED;
}
static JSValue owner_callback_create_code(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;OwnerCallbackCase *s=opaque;owner_callback_class=os64_js_class_id(&owner_callback_class);
    JSClassDef definition={.class_name="CheckedCallbackPayload",.finalizer=owner_callback_finalizer};
    if(!JS_IsRegisteredClass(JS_GetRuntime(context),owner_callback_class) && JS_NewClass(JS_GetRuntime(context),owner_callback_class,&definition)<0)return JS_EXCEPTION;
    JSValue payload=JS_NewObjectClass(context,owner_callback_class);if(JS_IsException(payload))return payload;
    s->roots[s->capture_at]=(OwnerCallbackRoot){.state=s,.index=s->capture_at};JS_SetOpaque(payload,&s->roots[s->capture_at]);
    JSValue function=JS_NewCFunctionData(context,owner_callback_native,0,0,1,&payload);
    JS_FreeValue(context,payload);if(JS_IsException(function))return function;
    s->capture_value=&function;JSValue result=JS_UNDEFINED;
    s->check(JS_RunWithSettings(s->capture_context,JS_GetIncumbentContext(JS_GetRuntime(context)),owner_callback_capture_here,s,&result),
        "checked callbacks: temporary native construction keeps both source and associated contexts loaned");
    s->capture_value=NULL;JS_FreeValue(context,function);return result;
}
static JSValue owner_callback_capture_outer(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerCallbackCase *s=opaque;s->scope=scope;s->capture_context=context;os64_js_outcome_t out;
    s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,s->code,&s->incumbent,owner_callback_create_code,s,&out)==OS64_JS_OK,
        "checked callbacks: checked nested construction captures a third-realm executable");return JS_UNDEFINED;
}
static void owner_callback_capture_fixture(OwnerCallbackCase *s,unsigned index)
{
    s->capture_at=index;owner_callback_current=s;os64_js_outcome_t out;
    s->check(js_owner_runtime_with_settings(s->runtime,OS64_JS_ABI_ID,s->entry,&s->incumbent,owner_callback_capture_outer,s,&out)==OS64_JS_OK,
        "checked callbacks: callback publication uses actual capture frame without raw exports");s->scope=NULL;s->capture_context=NULL;
}
static JSValue owner_callback_gc(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    (void)scope;OwnerCallbackCase *s=opaque;JS_RunGC(JS_GetRuntime(context));
    s->check(!s->finalized,"checked callbacks: native C roots survive engine cycle collection until close");return JS_UNDEFINED;
}
static JSValue owner_callback_body(JSOwnerRuntimeScope *scope,JSContext *context,JSValueConst callback,void *opaque)
{
    OwnerCallbackCase *s=opaque;s->scope=scope;s->calls++;owner_callback_roles(s,scope);
    if(owner_callback_loan_probe)owner_callback_loan_probe(s);
    JSOwnerRuntimeMetadata m;JSOwnerCallbackSummary counts;
    s->check(js_owner_callback_summary(s->runtime,&counts) && counts.live==s->expected_live && counts.invocations==1 &&
        js_owner_runtime_metadata(s->runtime,&m) && m.summary.loans==2,
        "checked callbacks: invocation uses one temporary root and one loan for each supplied role");
    os64_js_outcome_t saved=*s->outer;
    s->check(js_owner_callback_invoke(s->runtime,OS64_JS_ABI_ID,s->keys[0],s->entry,owner_callback_body,s,s->outer)==OS64_JS_BUSY &&
        !os64_memcmp(&saved,s->outer,sizeof saved),"checked callbacks: active outcome alias preserves original callback frame");
    if(s->mode==1 || s->mode==2 || s->mode==4) {
        s->check(js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,s->keys[0])==JS_OWNER_OK && s->finalized==0 &&
            js_owner_runtime_close(s->runtime,s->entry)==JS_OWNER_OK && js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK,
            "checked callbacks: self-close and host entry close preserve admitted snapshot and promotion");
        unsigned calls=0;os64_js_outcome_t out;
        s->check(js_owner_callback_invoke(s->runtime,OS64_JS_ABI_ID,s->keys[0],s->promoted,owner_callback_never,&calls,&out)==OS64_JS_ADMISSION_REFUSED && !calls,
            "checked callbacks: old key aliases cannot start a new invocation after close");
        if(s->mode==4)s->check(js_owner_callback_capture(scope,OS64_JS_ABI_ID,callback,&s->replacement)==JS_OWNER_OK &&
            s->replacement.serial!=s->keys[0].serial,
            "checked callbacks: record cell reuse publishes fresh key without changing active snapshot");
    }
    JSValue value=JS_Call(context,callback,JS_UNDEFINED,0,NULL);
    if(s->mode==2 && !JS_IsException(value)) {JS_FreeValue(context,value);return JS_ThrowTypeError(context,"managed callback exception");}
    return value;
}
static void owner_callback_destroy(OwnerCallbackCase *s)
{
    s->destroying=true;s->scope=NULL;os64_js_teardown_report_t report;
    if(owner_callback_teardown_probe)owner_callback_teardown_probe(s->runtime);
    os64_js_destroy_report(s->runtime,&report);
    if(owner_callback_teardown_probe)owner_callback_teardown_probe(NULL);
    s->check(!report.leaked,"checked callbacks: managed-root shutdown leaves no leaked-root reclamation");
}
typedef struct {
    os64_js_runtime_t *runtime;JSOwnerTicket ticket,promoted;
    JSOwnerCallbackKey key;void (*check)(bool,const char *);unsigned mode,calls;
} OwnerCallbackAdmission;
static JSValue owner_callback_admit(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerCallbackAdmission *s=opaque;s->calls++;JSValue value=JS_NewObject(context);if(JS_IsException(value))return value;
    JSOwnerCallbackKey sentinel={71,72},key=sentinel;JSOwnerRuntimeMetadata before,after;JSOwnerCallbackSummary counts;
    s->check(js_owner_runtime_metadata(s->runtime,&before),"checked callbacks: capture refusal metadata baseline");
    s->check(js_owner_callback_capture(scope,"bad",value,&key)==JS_OWNER_INVALID && key.incarnation==71 && key.serial==72 &&
        js_owner_callback_capture(scope,OS64_JS_ABI_ID,JS_NULL,&key)==JS_OWNER_INVALID &&
        js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,NULL)==JS_OWNER_INVALID,
        "checked callbacks: bad ABI type or output refuses before capture publication");
    if(s->mode==3) {
        os64_js_outcome_t raw;
        s->check(os64_js_settings_realm(s->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&raw)!=NULL,
            "checked callbacks: explicit legacy getter establishes sticky compatibility retention");
    }
    JSOwnerStatus result=js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&key);
    if(s->mode==0 || s->mode==1) {
        s->check(result==JS_OWNER_INVALID && key.incarnation==71 && key.serial==72 &&
            js_owner_runtime_metadata(s->runtime,&after) && before.summary.tickets==after.summary.tickets &&
            js_owner_callback_summary(s->runtime,&counts) && !counts.live && !counts.closing,
            "checked callbacks: missing frame or incumbent refuses without fallback and leaves output unchanged");
    } else {
        if(s->mode==2) {
            s->check(result==JS_OWNER_CAPACITY && key.incarnation==71 && key.serial==72 &&
                js_owner_callback_summary(s->runtime,&counts) && !counts.live &&
                js_owner_runtime_metadata(s->runtime,&after) && before.summary.tickets==after.summary.tickets,
                "checked callbacks: full holder pool publishes no callback or partial record");
            s->check(js_owner_runtime_close(s->runtime,s->ticket)==JS_OWNER_OK &&
                js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&key)==JS_OWNER_OK,
                "checked callbacks: partial capture refusal returns record capacity for later admission");
        } else s->check(result==JS_OWNER_OK && js_owner_runtime_metadata(s->runtime,&after) && after.summary.unknown_escape,
            "checked callbacks: successful capture never clears a prior legacy retention veto");
        s->key=key;key=sentinel;
        s->check(js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&key)==JS_OWNER_CAPACITY && key.incarnation==71 && key.serial==72 &&
            js_owner_callback_summary(s->runtime,&counts) && counts.live==1,
            "checked callbacks: full record pool preserves published record and caller sentinel");
        s->check(js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,s->key)==JS_OWNER_OK &&
            js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&key)==JS_OWNER_OK && key.serial!=s->key.serial &&
            js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,s->key)==JS_OWNER_STALE &&
            js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,key)==JS_OWNER_OK,
            "checked callbacks: capacity recycles with fresh identity and stale close cannot affect replacement");
        if(s->mode==3) {
            JSOwnerRuntimeMetadata stable;JSOwnerCallbackKey previous=key;
            s->check(js_owner_runtime_metadata(s->runtime,&stable),"checked callbacks: record churn starts with fixed charged arena");
            for(unsigned cycle=0;cycle<128;cycle++) {
                JSOwnerCallbackKey next;
                s->check(js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&next)==JS_OWNER_OK && next.serial>previous.serial &&
                    js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,previous)==JS_OWNER_STALE &&
                    js_owner_callback_close(s->runtime,OS64_JS_ABI_ID,next)==JS_OWNER_OK,
                    "checked callbacks: bounded record churn preserves stale identity beyond capacity");previous=next;
            }
            s->check(js_owner_runtime_metadata(s->runtime,&after) && stable.charged_bytes==after.charged_bytes &&
                stable.requested_bytes==after.requested_bytes && stable.summary.tickets==after.summary.tickets,
                "checked callbacks: callback-cell reuse creates no growing charged history");
        }
        if(s->mode==2)s->check(js_owner_scope_promote(scope,&s->promoted)==JS_OWNER_OK,
            "checked callbacks: closed records return captured holder capacity without ending admitting loan");
    }
    JS_FreeValue(context,value);return JS_UNDEFINED;
}
static void owner_callback_admission_cases(const os64_js_config_t *config,unsigned policy,void (*check)(bool,const char *))
{
    for(unsigned mode=0;mode<4;mode++) {
        os64_js_runtime_t *runtime;os64_js_outcome_t out;JSOwnerTicket ticket;
        check(js_owner_runtime_create_with_callbacks(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){1,mode==2?1:3,2},1,&runtime,&out)==OS64_JS_OK && js_owner_runtime_primary_ticket(runtime,&ticket)==JS_OWNER_OK,
            "checked callbacks: frame and finite-capacity fixture");
        OwnerCallbackAdmission s={.runtime=runtime,.ticket=ticket,.check=check,.mode=mode};
        os64_js_status_t result;
        if(mode==0)result=js_owner_runtime_task_run(runtime,OS64_JS_ABI_ID,ticket,"missing settings frame",owner_callback_admit,&s,&out);
        else {
            check(os64_js_task_begin(runtime,OS64_JS_ABI_ID,"capture admission",&out)==OS64_JS_OK,"checked callbacks: admission borrows explicit task");
            result=js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,ticket,mode==1?NULL:&ticket,owner_callback_admit,&s,&out);
            check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked callbacks: refusal leaves enclosing task valid");
        }
        check(result==OS64_JS_OK && s.calls==1,"checked callbacks: capture capacity and frame refusal stay local to operation");
        os64_js_teardown_report_t report;os64_js_destroy_report(runtime,&report);
        check(!report.leaked,"checked callbacks: admission refusal leaves no callback or metadata leak");
    }
    os64_js_runtime_t *runtime=(void *)1;os64_js_outcome_t out;
    check(js_owner_runtime_create_with_callbacks(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
        (JSOwnerLimits){1,1,1},0,&runtime,&out)==OS64_JS_BAD_ARGUMENT && !runtime &&
        js_owner_runtime_create_with_callbacks(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,
            (JSOwnerLimits){1,1,1},SIZE_MAX,&runtime,&out)==OS64_JS_BAD_ARGUMENT && !runtime,
        "checked callbacks: zero or overflowing callback storage refuses before runtime publication");
    check(js_owner_runtime_create(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,1,1},&runtime,&out)==OS64_JS_OK,
        "checked callbacks: existing private constructor remains available without selected callback pool");
    JSOwnerCallbackSummary counts={.incarnation=91};
    check(!js_owner_callback_summary(runtime,&counts) && counts.incarnation==91 && !js_owner_callback_summary(NULL,&counts),
        "checked callbacks: unselected scalar query refuses unchanged without creating a registry");os64_js_destroy(runtime);
    uint64_t issuer=UINT64_MAX,unchanged=83;
    check(js_owner_callback_serial_next(&issuer,&unchanged)==JS_OWNER_EXHAUSTED && issuer==UINT64_MAX && unchanged==83,
        "checked callbacks: record issuer refuses wrap without changing output");
}
typedef struct {
    os64_js_runtime_t *runtime;JSOwnerTicket entry;JSOwnerCallbackKey key;
    JSOwnerRuntimeScope *scope;void (*check)(bool,const char *);unsigned gets,coercions;
} OwnerCallbackObject;
static OwnerCallbackObject *owner_callback_object_current;
static void owner_callback_object_roles(void)
{
    OwnerCallbackObject *s=owner_callback_object_current;JSOwnerSettingsSnapshot entry,inc;
    s->check(js_owner_scope_settings(s->scope,OS64_JS_SETTINGS_ENTRY,&entry)==JS_OWNER_OK &&
        js_owner_scope_settings(s->scope,OS64_JS_SETTINGS_INCUMBENT,&inc)==JS_OWNER_OK &&
        owner_callback_id(entry.realm.owner,s->entry.owner) && owner_callback_id(inc.realm.owner,s->entry.owner),
        "checked callbacks: callback-interface lookup and coercion share selected settings");
}
static JSValue owner_callback_object_string(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;owner_callback_object_roles();owner_callback_object_current->coercions++;
    return JS_NewString(context,"managed object callback");
}
static JSValue owner_callback_object_method(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;owner_callback_object_roles();JSValue value=JS_NewObject(context);
    if(JS_IsException(value))return value;
    if(JS_SetPropertyStr(context,value,"toString",JS_NewCFunction(context,owner_callback_object_string,"toString",0))<0) {
        JS_FreeValue(context,value);return JS_EXCEPTION;
    }
    return value;
}
static JSValue owner_callback_object_get(JSContext *context,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;owner_callback_object_roles();owner_callback_object_current->gets++;
    return JS_NewCFunction(context,owner_callback_object_method,"handleEvent",0);
}
static JSValue owner_callback_object_capture(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerCallbackObject *s=opaque;JSValue value=JS_NewObject(context);if(JS_IsException(value))return value;
    JSAtom key=JS_NewAtom(context,"handleEvent");
    if(key==JS_ATOM_NULL){JS_FreeValue(context,value);return JS_EXCEPTION;}
    int added=JS_DefinePropertyGetSet(context,value,key,JS_NewCFunction(context,owner_callback_object_get,"handleEvent",0),JS_UNDEFINED,JS_PROP_CONFIGURABLE);
    JS_FreeAtom(context,key);
    if(added<0){JS_FreeValue(context,value);return JS_EXCEPTION;}
    s->check(js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&s->key)==JS_OWNER_OK && !s->gets,
        "checked callbacks: capture retains callback-interface object without inspecting properties");
    JS_FreeValue(context,value);return JS_UNDEFINED;
}
static JSValue owner_callback_object_body(JSOwnerRuntimeScope *scope,JSContext *context,JSValueConst callback,void *opaque)
{
    OwnerCallbackObject *s=opaque;s->scope=scope;JSValue method=JS_GetPropertyStr(context,callback,"handleEvent");
    if(JS_IsException(method))return method;
    JSValue value=JS_Call(context,method,callback,0,NULL);JS_FreeValue(context,method);if(JS_IsException(value))return value;
    const char *text=JS_ToCString(context,value);bool ok=text && !os64_strcmp(text,"managed object callback");
    if(text)JS_FreeCString(context,text);
    JS_FreeValue(context,value);
    s->check(ok,"checked callbacks: callback-interface return conversion stays inside scoped invocation");return text?JS_UNDEFINED:JS_EXCEPTION;
}
static void owner_callback_object_cases(const os64_js_config_t *config,unsigned policy,void (*check)(bool,const char *))
{
    os64_js_runtime_t *runtime;os64_js_outcome_t out;JSOwnerTicket entry;
    check(js_owner_runtime_create_with_callbacks(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,2,2},1,&runtime,&out)==OS64_JS_OK &&
        js_owner_runtime_primary_ticket(runtime,&entry)==JS_OWNER_OK && os64_js_task_begin(runtime,OS64_JS_ABI_ID,"object callback",&out)==OS64_JS_OK,
        "checked callbacks: object-interface scoped conversion fixture");
    OwnerCallbackObject s={.runtime=runtime,.entry=entry,.check=check};owner_callback_object_current=&s;
    check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,entry,&entry,owner_callback_object_capture,&s,&out)==OS64_JS_OK &&
        js_owner_callback_invoke(runtime,OS64_JS_ABI_ID,s.key,entry,owner_callback_object_body,&s,&out)==OS64_JS_OK && s.gets==1 && s.coercions==1,
        "checked callbacks: object callback property access and return coercion run once at invocation");s.scope=NULL;
    os64_js_runtime_t *foreign;JSOwnerTicket other;
    check(js_owner_runtime_create_with_callbacks(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,2,2},1,&foreign,&out)==OS64_JS_OK &&
        js_owner_runtime_primary_ticket(foreign,&other)==JS_OWNER_OK && js_owner_callback_close(foreign,OS64_JS_ABI_ID,s.key)==JS_OWNER_FOREIGN,
        "checked callbacks: key from another live runtime refuses without touching registry");
    unsigned calls=0;
    check(js_owner_callback_invoke(foreign,OS64_JS_ABI_ID,s.key,other,owner_callback_never,&calls,&out)==OS64_JS_ADMISSION_REFUSED && !calls,
        "checked callbacks: foreign callback key cannot borrow another runtime entry");os64_js_destroy(foreign);
    check(js_owner_callback_close(runtime,"bad",s.key)==JS_OWNER_INVALID && js_owner_callback_close(runtime,OS64_JS_ABI_ID,s.key)==JS_OWNER_OK &&
        os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked callbacks: valid close follows malformed close without losing ownership");
    os64_js_teardown_report_t report;os64_js_destroy_report(runtime,&report);check(!report.leaked,"checked callbacks: object-interface fixture balances root ownership");
    check(js_owner_runtime_create_with_callbacks(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){1,1,1},1,&runtime,&out)==OS64_JS_OK &&
        js_owner_callback_close(runtime,OS64_JS_ABI_ID,s.key)==JS_OWNER_FOREIGN,
        "checked callbacks: stale numeric key cannot resolve after original runtime destruction");os64_js_destroy(runtime);owner_callback_object_current=NULL;
}
typedef struct {os64_js_runtime_t *runtime;JSOwnerTicket primary,staged;JSOwnerCallbackKey key;
    void (*check)(bool,const char *);} OwnerCallbackLifecycle;
static os64_js_status_t owner_callback_private(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerCallbackLifecycle *s=opaque;JSValue value=JS_NewObject(context);if(JS_IsException(value))return OS64_JS_EXCEPTION;
    JSOwnerCallbackKey key={71,72};
    s->check(js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&key)==JS_OWNER_INVALID && key.incarnation==71 && key.serial==72,
        "checked callbacks: private staged setup cannot publish a persistent callback");JS_FreeValue(context,value);return OS64_JS_OK;
}
static JSValue owner_callback_lifecycle(JSOwnerRuntimeScope *scope,JSContext *context,void *opaque)
{
    OwnerCallbackLifecycle *s=opaque;os64_js_outcome_t out;
    s->check(js_owner_runtime_setup(s->runtime,OS64_JS_ABI_ID,s->staged,owner_callback_private,s,&out)==OS64_JS_OK,
        "checked callbacks: staged capture refusal leaves setup transaction intact");
    JSValue value=JS_NewObject(context);if(JS_IsException(value))return value;
    s->check(js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&s->key)==JS_OWNER_OK &&
        js_owner_runtime_retire(s->runtime,OS64_JS_ABI_ID,s->primary,&out)==OS64_JS_OK,
        "checked callbacks: capture precedes retained-incumbent retirement");
    JSOwnerCallbackKey key={71,72};
    s->check(js_owner_callback_capture(scope,OS64_JS_ABI_ID,value,&key)==JS_OWNER_INVALID && key.incarnation==71 && key.serial==72,
        "checked callbacks: retired incumbent facts do not admit another capture");JS_FreeValue(context,value);return JS_UNDEFINED;
}
static void owner_callback_lifecycle_cases(const os64_js_config_t *config,unsigned policy,void (*check)(bool,const char *))
{
    OwnerCallbackLifecycle s={.check=check};os64_js_outcome_t out;
    check(js_owner_runtime_create_with_callbacks(config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){2,4,3},2,&s.runtime,&out)==OS64_JS_OK &&
        js_owner_runtime_primary_ticket(s.runtime,&s.primary)==JS_OWNER_OK && js_owner_runtime_stage(s.runtime,OS64_JS_ABI_ID,1,&s.staged,&out)==OS64_JS_OK &&
        os64_js_task_begin(s.runtime,OS64_JS_ABI_ID,"callback lifecycle",&out)==OS64_JS_OK,
        "checked callbacks: staged and retired lifetime fixture");
    check(js_owner_runtime_with_settings(s.runtime,OS64_JS_ABI_ID,s.primary,&s.primary,owner_callback_lifecycle,&s,&out)==OS64_JS_OK,
        "checked callbacks: admitted scope can finish after its incumbent retires");unsigned calls=0;
    check(js_owner_callback_invoke(s.runtime,OS64_JS_ABI_ID,s.key,s.primary,owner_callback_never,&calls,&out)==OS64_JS_REALM_RETIRED && !calls &&
        js_owner_callback_close(s.runtime,OS64_JS_ABI_ID,s.key)==JS_OWNER_OK,
        "checked callbacks: retained callback holder grants no retired-entry execution but still cleans up");
    check(os64_js_task_end(s.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked callbacks: lifecycle refusal leaves caller task closable");
    os64_js_teardown_report_t report;os64_js_destroy_report(s.runtime,&report);check(!report.leaked,"checked callbacks: staged and retired root cleanup balances");
}
static void owner_callback_cases(void (*check)(bool,const char *))
{
    os64_js_config_t config={.limits=os64_js_default_limits()};config.limits.memory_bytes=8*1024*1024;config.limits.execution_ms=1000;
    for(unsigned policy=0;policy<2;policy++) {
        for(unsigned mode=0;mode<6;mode++) {
            os64_js_runtime_t *runtime;os64_js_outcome_t out;
            check(js_owner_runtime_create_with_callbacks(&config,(os64_js_teardown_policy_t)policy,OS64_JS_ABI_ID,(JSOwnerLimits){4,12,6},2,&runtime,&out)==OS64_JS_OK,
                "checked callbacks: explicit charged callback construction fixture");
            OwnerCallbackCase s={.runtime=runtime,.outer=&out,.check=check,.expected_live=2,.external_holders=4,.mode=mode};owner_callback_current=&s;
            check(js_owner_runtime_primary_ticket(runtime,&s.primary)==JS_OWNER_OK && js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&s.incumbent,&out)==OS64_JS_OK &&
                js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&s.entry,&out)==OS64_JS_OK && js_owner_runtime_child(runtime,OS64_JS_ABI_ID,1,&s.code,&out)==OS64_JS_OK &&
                os64_js_task_begin(runtime,OS64_JS_ABI_ID,"managed callback lifetime",&out)==OS64_JS_OK,
                "checked callbacks: independent task incumbent entry and executable fixture");
            owner_callback_capture_fixture(&s,0);owner_callback_capture_fixture(&s,1);
            JSOwnerRuntimeMetadata m;JSOwnerCallbackSummary counts;
            check(js_owner_runtime_metadata(runtime,&m) && m.summary.tickets==6 && !m.summary.loans && !m.summary.unknown_escape &&
                js_owner_callback_summary(runtime,&counts) && counts.capacity==2 && counts.live==2 && !counts.closing && !counts.invocations,
                "checked callbacks: two records own separate roots and incumbent holders without legacy escape");
            check(js_owner_runtime_with_settings(runtime,OS64_JS_ABI_ID,s.entry,&s.incumbent,owner_callback_gc,&s,&out)==OS64_JS_OK,
                "checked callbacks: collecting while C-root records are populated preserves data");
            check(js_owner_runtime_close(runtime,s.incumbent)==JS_OWNER_OK,"checked callbacks: close original captured-incumbent holder");
            s.external_holders--;
            if(mode!=3 && mode!=5) {
                os64_js_status_t result=js_owner_callback_invoke(runtime,OS64_JS_ABI_ID,s.keys[0],s.entry,owner_callback_body,&s,&out);s.scope=NULL;
                check(result==(mode==2?OS64_JS_EXCEPTION:OS64_JS_OK) && s.calls==1 && s.native_calls==1 && s.finalized==((mode==1 || mode==2)?1u:0u),
                    "checked callbacks: independent incumbent and temporary callback survive host ticket and registration close");
                if(mode)s.entry=s.promoted;
                if(!mode)check(js_owner_callback_close(runtime,OS64_JS_ABI_ID,s.keys[0])==JS_OWNER_OK,"checked callbacks: idle close releases first registration");
                if(mode==4) {
                    check(js_owner_callback_close(runtime,OS64_JS_ABI_ID,s.keys[0])==JS_OWNER_STALE &&
                        js_owner_callback_summary(runtime,&counts) && counts.live==2,
                        "checked callbacks: stale close cannot remove replacement sharing the same record cell");
                    s.mode=0;
                    check(js_owner_callback_invoke(runtime,OS64_JS_ABI_ID,s.replacement,s.entry,owner_callback_body,&s,&out)==OS64_JS_OK && s.native_calls==2,
                        "checked callbacks: fresh replacement remains independently callable");s.scope=NULL;
                    check(js_owner_callback_close(runtime,OS64_JS_ABI_ID,s.replacement)==JS_OWNER_OK,"checked callbacks: close replacement after original invocation unwind");
                }
                check(s.finalized==1 && js_owner_callback_close(runtime,OS64_JS_ABI_ID,s.keys[0])==JS_OWNER_STALE,
                    "checked callbacks: closing one registration invalidates copies without touching the other");
            } else {
                s.nested_clear=true;
                if(mode==5)check(js_owner_callback_close(runtime,OS64_JS_ABI_ID,s.keys[0])==JS_OWNER_OK && s.finalized==2 &&
                    js_owner_callback_summary(runtime,&counts) && !counts.live && !counts.closing,
                    "checked callbacks: outermost idle close drains nested invalidations without recursive finalization");
            }
            if(mode!=3)check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"checked callbacks: caller retains original task close");
            owner_callback_destroy(&s);
            check(s.finalized==2,"checked callbacks: managed-root drain precedes engine teardown and frees remaining callback");
        }
        owner_callback_admission_cases(&config,policy,check);
        owner_callback_object_cases(&config,policy,check);
        owner_callback_lifecycle_cases(&config,policy,check);
    }
    owner_callback_current=NULL;
}
#endif
