#ifndef OS64_JS_IMPORT_SETTINGS_CASES_H
#define OS64_JS_IMPORT_SETTINGS_CASES_H
#include "os64/js_callbacks.h"

/* Entry A, backup incumbent B and executing C deliberately disagree. */
typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[3];
    JSContext *ctx[3];
    JSValue fn, specifier, options, error;
    const char *base[3];
    char resolved[160];
    unsigned mode, providers, resolves, loads, load_realm;
    bool resolve_refused;
} ImportSettings;
static ImportSettings *import_settings;
static const char import_function[] = "(function(specifier,options){return import(specifier,options)})";

static JSValue import_base_provider(JSContext *ctx, void *opaque)
{
    ImportSettings *f=import_settings;unsigned i=(unsigned)(uintptr_t)opaque;
    os64_js_outcome_t out;f->providers++;
    check(ctx==f->ctx[2]&&i==2,"import settings: current C selects the base provider");
    check(os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==f->realm[2]&&
          os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==f->realm[0]&&
          os64_js_settings_realm(f->runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==f->realm[1],
          "import settings: current differs from entry and incumbent");
    check(os64_js_realm_set_import_base(f->realm[2],OS64_JS_ABI_ID,NULL,NULL,&out)==OS64_JS_BUSY,
          "import settings: provider cannot detach its active loan");
    check(os64_js_task_end(f->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_BUSY,
          "import settings: provider retains enclosing task guard");
    if(f->mode==8)return JS_Throw(ctx,JS_DupValue(ctx,f->error));
    if(f->mode==9)return JS_NewObject(ctx);
    if(f->mode==10)check(os64_js_realm_retire(f->realm[2],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                        "import settings: retirement during provider call recorded");
    if(f->mode==11)os64_js_cancel(f->runtime);
    return JS_NewString(ctx,f->base[i]);
}
static char *import_resolve(void *opaque,const char *base,const char *specifier)
{
    ImportSettings *f=import_settings;unsigned i=(unsigned)(uintptr_t)opaque;
    f->resolves++;f->load_realm=i;
    size_t n=os64_strlen(base);if(n>=sizeof(f->resolved))n=sizeof(f->resolved)-1;
    os64_memcpy(f->resolved,base,n);f->resolved[n]=0;
    n=os64_strlen(specifier);char *name=os64_malloc(n+1);if(name)os64_memcpy(name,specifier,n+1);
    f->resolve_refused=name==NULL;return name;
}
static bool import_load(void *opaque,const char *name,const char **bytes,size_t *length)
{
    ImportSettings *f=import_settings;unsigned i=(unsigned)(uintptr_t)opaque;
    (void)name;f->loads++;check(i==f->load_realm,"import settings: loader matches resolver realm");
    static const char source[]="export const value=await Promise.resolve(42)";
    *bytes=source;*length=sizeof(source)-1;return true;
}
static JSValue import_conversion(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;
    ImportSettings *f=import_settings;
    check(JS_GetCurrentContext(JS_GetRuntime(ctx))==f->ctx[f->mode==3?1:2],
          "import settings: argument conversion enters its own executable realm");
    f->base[2]="https://C.example/converted/";
    return f->mode==2?JS_UNDEFINED:JS_NewString(ctx,"./module.js");
}
static bool import_open(ImportSettings *f,int policy,unsigned mode)
{
    *f=(ImportSettings){.mode=mode,.fn=JS_UNDEFINED,.specifier=JS_UNDEFINED,
        .options=JS_UNDEFINED,.error=JS_UNDEFINED,
        .base={"https://A.example/","https://B.example/","https://C.example/initial/"}};
    import_settings=f;os64_js_config_t config=fixture_config();os64_js_outcome_t out;
    check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&f->runtime,&out)==OS64_JS_OK,
          "import settings: runtime created");if(!f->runtime)return false;
    check((mode==7?os64_js_enable_finalization_settings(f->runtime,OS64_JS_ABI_ID,&out):
          os64_js_enable_import_settings(f->runtime,OS64_JS_ABI_ID,&out))==OS64_JS_OK,
          "import settings: opt-in configured before execution");
    f->realm[0]=os64_js_primary_realm(f->runtime,OS64_JS_ABI_ID,&out);
    for(unsigned i=0;i<3;i++) {
        if(i)check(os64_js_realm_create(f->runtime,OS64_JS_ABI_ID,&f->realm[i],&out)==OS64_JS_OK,
                   "import settings: child created");
        f->ctx[i]=os64_js_realm_context(f->realm[i],OS64_JS_ABI_ID,&out);if(!f->ctx[i])return false;
        os64_js_module_loader_t loader={(void *)(uintptr_t)i,import_resolve,import_load};
        check(os64_js_realm_set_module_loader(f->realm[i],OS64_JS_ABI_ID,&loader,&out)==OS64_JS_OK,
              "import settings: module loader installed");
        if(mode!=5)check(os64_js_realm_set_import_base(f->realm[i],OS64_JS_ABI_ID,
              import_base_provider,(void *)(uintptr_t)i,&out)==OS64_JS_OK,"import settings: provider installed");
    }
    if(mode==12)check(os64_js_realm_set_import_base(f->realm[2],OS64_JS_ABI_ID,NULL,NULL,&out)==OS64_JS_OK,
                     "import settings: provider detaches while idle");
    f->fn=JS_Eval(f->ctx[2],import_function,sizeof(import_function)-1,"source-C.js",JS_EVAL_TYPE_GLOBAL);
    check(JS_IsFunction(f->ctx[2],f->fn),"import settings: import closure compiled");
    if(mode!=4)check(JS_ClearFunctionScriptProvenance(f->fn,import_function+1,sizeof(import_function)-3),
                     "import settings: host-generated closure has no source record");
    f->specifier=JS_NewString(f->ctx[2],"./module.js");f->error=JS_NewObject(f->ctx[2]);
    if(mode>=1&&mode<=3) {
        JSValue object=JS_NewObject(f->ctx[2]);
        JSValue conversion=JS_NewCFunction(f->ctx[mode==3?1:2],import_conversion,"convert",0);
        if(mode==2) {
            JSAtom key=JS_NewAtom(f->ctx[2],"with");
            check(JS_DefinePropertyGetSet(f->ctx[2],object,key,conversion,JS_UNDEFINED,JS_PROP_CONFIGURABLE)>=0,
                  "import settings: options getter installed");JS_FreeAtom(f->ctx[2],key);f->options=object;
        } else {
            check(JS_SetPropertyStr(f->ctx[2],object,"toString",conversion)>=0,"import settings: specifier conversion installed");
            JS_FreeValue(f->ctx[2],f->specifier);f->specifier=object;
        }
    }
    return true;
}
static JSValue import_invoke(JSContext *ctx,void *opaque)
{
    ImportSettings *f=opaque;JSValue args[]={f->specifier,f->options};
    if(f->mode==13) {
        JSValue global=JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx,global,"foreignImport",JS_DupValue(ctx,f->fn));JS_FreeValue(ctx,global);
        const char source[]="foreignImport('./module.js')";
        return JS_Eval(ctx,source,sizeof(source)-1,"source-A.js",JS_EVAL_TYPE_GLOBAL);
    }
    return JS_Call(ctx,f->fn,JS_UNDEFINED,2,args);
}
static void import_close(ImportSettings *f)
{
    if(!f->runtime)return;
    JS_FreeValue(f->ctx[0],f->fn);JS_FreeValue(f->ctx[0],f->specifier);
    JS_FreeValue(f->ctx[0],f->options);JS_FreeValue(f->ctx[0],f->error);
    check(!JS_GetEntryContext(JS_GetRuntime(f->ctx[0]))&&!JS_GetIncumbentContext(JS_GetRuntime(f->ctx[0])),
          "import settings: no settings scope escapes");
    os64_js_teardown_report_t report={0};os64_js_destroy_report(f->runtime,&report);
    check(!report.leaked,"import settings: base promise and module ownership releases");import_settings=NULL;
}
static void import_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<14;mode++) {
        ImportSettings f;if(!import_open(&f,policy,mode)){import_close(&f);continue;}
        os64_js_outcome_t out;JSValue promise=JS_UNDEFINED;
        check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"import-base",&out)==OS64_JS_OK,
              "import settings: task begins");
        if(mode==6)promise=import_invoke(f.ctx[0],&f);
        else {
            os64_js_status_t status=os64_js_realm_with_settings(f.realm[0],f.realm[1],OS64_JS_ABI_ID,
                import_invoke,&f,&promise,&out);
            check(status==(mode==11?OS64_JS_CANCELLED:OS64_JS_OK),"import settings: guarded import returns expected status");
        }
        if(mode!=11) {
            JSValue proto=JS_GetPrototype(f.ctx[2],promise),ctor=realm_property(f.ctx[2],"Promise");
            JSValue expected=JS_GetPropertyStr(f.ctx[2],ctor,"prototype");
            check(JS_IsObject(proto)&&JS_VALUE_GET_PTR(proto)==JS_VALUE_GET_PTR(expected),
                  "import settings: Promise belongs to executing C");
            JS_FreeValue(f.ctx[2],proto);JS_FreeValue(f.ctx[2],ctor);JS_FreeValue(f.ctx[2],expected);
        }
        /* This pointer change must not affect the queued owned string. */
        f.base[2]="https://C.example/too-late/";
        os64_js_status_t ended=os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out);
        check(ended==(mode==11?OS64_JS_CANCELLED:((mode>=5&&mode<=9)||mode==12)?OS64_JS_UNHANDLED_REJECTION:OS64_JS_OK),"import settings: task settles or preserves cancellation");
        if(mode<=4||mode==13) {
            check(f.providers==((mode==4||mode==13)?0u:1u)&&f.resolves==1&&f.loads==1&&f.load_realm==(mode==13?0u:2u),
                  "import settings: source record or current realm routes one module");
            const char *expected=mode==13?"source-A.js":mode==4?"source-C.js":mode?"https://C.example/converted/":"https://C.example/initial/";
            check(!os64_strcmp(f.resolved,expected),"import settings: base captured after conversions before enqueue");
            check(JS_PromiseState(f.ctx[2],promise)==JS_PROMISE_FULFILLED,"import settings: awaited module fulfills import");
            JSValue ns=JS_PromiseResult(f.ctx[2],promise),value=JS_GetPropertyStr(f.ctx[2],ns,"value");
            check(JS_IsNumber(value)&&JS_VALUE_GET_INT(value)==42,"import settings: module namespace delivered");
            JS_FreeValue(f.ctx[2],value);JS_FreeValue(f.ctx[2],ns);
        } else if(mode!=11) {
            check(!f.resolves&&!f.loads,"import settings: refused base cannot reach module provider");
            check(JS_PromiseState(f.ctx[2],promise)==JS_PROMISE_REJECTED,"import settings: refusal rejects returned Promise");
            if(mode==8) {
                JSValue error=JS_PromiseResult(f.ctx[2],promise);
                check(JS_IsObject(error)&&JS_VALUE_GET_PTR(error)==JS_VALUE_GET_PTR(f.error),
                      "import settings: provider exception identity preserved");JS_FreeValue(f.ctx[2],error);
            }
        }
        JS_FreeValue(f.ctx[0],promise);import_close(&f);
    }
}
#endif
