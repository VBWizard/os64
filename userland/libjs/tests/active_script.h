#ifndef OS64_JS_ACTIVE_SCRIPT_CASES_H
#define OS64_JS_ACTIVE_SCRIPT_CASES_H

typedef struct {
    os64_js_runtime_t *runtime;
    os64_js_realm_t *realm[3];
    JSContext *ctx[3];
    JSValue records[16];
    unsigned loads[3], resolves[3];
    char bases[3][128];
} ActiveScriptFixture;
static ActiveScriptFixture *active_script;

static JSValue active_record(JSContext *ctx, JSValueConst self, int argc,
                             JSValueConst *argv, int magic)
{
    (void)self;
    JSRuntime *rt=JS_GetRuntime(ctx);
    JSValue record=JS_GetActiveScriptOrModule(rt),result=JS_UNDEFINED;
    if(magic==0) {
        int32_t index=-1;
        if(argc)JS_ToInt32(ctx,&index,argv[0]);
        if(index>=0&&index<16) {
            JS_FreeValue(ctx,active_script->records[index]);
            active_script->records[index]=JS_DupValue(ctx,record);
        }
    } else if(magic==1) {
        int index=-1;
        for(int i=0;i<16;i++)
            if(!JS_IsUndefined(record)&&JS_VALUE_GET_TAG(record)==JS_VALUE_GET_TAG(active_script->records[i])&&
               JS_VALUE_GET_PTR(record)==JS_VALUE_GET_PTR(active_script->records[i])){index=i;break;}
        result=JS_NewInt32(ctx,index);
    } else if(magic==2) {
        JSAtom name=JS_GetScriptOrModuleName(ctx,0);
        result=name==JS_ATOM_NULL?JS_NULL:JS_AtomToValue(ctx,name);JS_FreeAtom(ctx,name);
    } else {
        JSContext *realm=magic==3?JS_GetScriptOrModuleContext(record):JS_GetCurrentContext(rt);
        int index=-1;for(int i=0;i<3;i++)if(realm==active_script->ctx[i])index=i;
        result=JS_NewInt32(ctx,index);
    }
    JS_FreeValue(ctx,record);return result;
}

static char *active_resolve(void *opaque,const char *base,const char *specifier)
{
    unsigned i=(unsigned)(uintptr_t)opaque;
    active_script->resolves[i]++;
    size_t n=os64_strlen(base);if(n>=sizeof(active_script->bases[i]))n=sizeof(active_script->bases[i])-1;
    os64_memcpy(active_script->bases[i],base,n);active_script->bases[i][n]=0;
    n=os64_strlen(specifier);char *result=os64_malloc(n+3);
    if(result){result[0]='A'+i;result[1]='/';os64_memcpy(result+2,specifier,n+1);}return result;
}
static bool active_load(void *opaque,const char *name,const char **bytes,size_t *length)
{
    unsigned i=(unsigned)(uintptr_t)opaque;
    check(name[0]==(char)('A'+i),"active script: module provider agrees with exact referrer realm");
    active_script->loads[i]++;
    static const char *sources[]={"export const value=await Promise.resolve(10);globalThis.moduleCallback=Promise.resolve().then(()=>recordName())", "export const value=20", "export const value=30"};
    *bytes=sources[i];*length=os64_strlen(*bytes);return true;
}
static const char active_clear_source[]="function clearable(){clearRecord(clearable);if(recordName()!=='clear-source.js')throw 1}";
static JSValue active_clear(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)self;
    check(argc==1&&JS_ClearFunctionScriptProvenance(argv[0],active_clear_source,sizeof(active_clear_source)-1),
          "active script: executing closure drops its stored provenance");
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static bool active_open(ActiveScriptFixture *f,int policy)
{
    *f=(ActiveScriptFixture){0};active_script=f;
    for(int i=0;i<16;i++)f->records[i]=JS_UNDEFINED;
    os64_js_config_t config=fixture_config();os64_js_outcome_t out;
    check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&f->runtime,&out)==OS64_JS_OK,
          "active script: runtime created");if(!f->runtime)return false;
    check(os64_js_enable_promise_settings(f->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
          "active script: Promise host preparation enabled");
    f->realm[0]=os64_js_primary_realm(f->runtime,OS64_JS_ABI_ID,&out);
    for(unsigned i=0;i<3;i++) {
        if(i)check(os64_js_realm_create(f->runtime,OS64_JS_ABI_ID,&f->realm[i],&out)==OS64_JS_OK,
                   "active script: child created");
        f->ctx[i]=os64_js_realm_context(f->realm[i],OS64_JS_ABI_ID,&out);if(!f->ctx[i])return false;
        os64_js_module_loader_t loader={(void *)(uintptr_t)i,active_resolve,active_load};
        check(os64_js_realm_set_module_loader(f->realm[i],OS64_JS_ABI_ID,&loader,&out)==OS64_JS_OK,
              "active script: distinct module provider installed");
    }
    for(unsigned i=0;i<3;i++) {
        JSValue global=JS_GetGlobalObject(f->ctx[i]);
        for(unsigned j=0;j<3;j++){char name[]={(char)('A'+j),0};
            check(JS_SetPropertyStr(f->ctx[i],global,name,JS_GetGlobalObject(f->ctx[j]))>=0,
                  "active script: fixture globals exchanged");}
        const char *names[]={"captureRecord","recordId","recordName","recordRealm","currentRealm"};
        for(int j=0;j<5;j++)check(JS_SetPropertyStr(f->ctx[i],global,names[j],
              JS_NewCFunctionMagic(f->ctx[i],active_record,names[j],1,JS_CFUNC_generic_magic,j))>=0,
              "active script: record inspection installed");
        check(JS_SetPropertyStr(f->ctx[i],global,"clearRecord",JS_NewCFunction(f->ctx[i],active_clear,"clearRecord",1))>=0,
              "active script: executing provenance clear seam installed");
        JS_FreeValue(f->ctx[i],global);
    }
    return true;
}
static os64_js_status_t active_run(ActiveScriptFixture *f,unsigned realm,const char *source,const char *name)
{
    os64_js_outcome_t out;
    return os64_js_realm_run(f->realm[realm],source,os64_strlen(source),name,&out);
}
static void active_close(ActiveScriptFixture *f)
{
    if(!f->runtime)return;
    JSValue record=JS_GetActiveScriptOrModule(JS_GetRuntime(f->ctx[0]));
    check(JS_IsUndefined(record),"active script: no record escapes its activation");JS_FreeValue(f->ctx[0],record);
    for(int i=0;i<16;i++)JS_FreeValue(f->ctx[0],f->records[i]);
    os64_js_teardown_report_t report={0};os64_js_destroy_report(f->runtime,&report);
    check(!report.leaked,"active script: closure frame module and queue edges release");active_script=NULL;
}

static JSValue active_eval_function(JSContext *ctx,void *opaque)
{
    JSValue *code=opaque,value=*code;*code=JS_UNDEFINED;
    return JS_EvalFunction(ctx,value);
}
static void active_evaluate_code(ActiveScriptFixture *f,unsigned which,JSValue code)
{
    os64_js_outcome_t out;JSValue result=JS_UNDEFINED;
    check(os64_js_realm_task_begin(f->realm[which],OS64_JS_ABI_ID,"active-code",&out)==OS64_JS_OK,
          "active script: compiled code task begins");
    check(os64_js_realm_with_settings(f->realm[which],NULL,OS64_JS_ABI_ID,
          active_eval_function,&code,&result,&out)==OS64_JS_OK,"active script: compiled record runs inside checked scope");
    if(JS_IsException(result))JS_FreeValue(f->ctx[which],JS_GetException(f->ctx[which]));
    JS_FreeValue(f->ctx[which],result);JS_FreeValue(f->ctx[which],code);
    check(os64_js_task_end(f->runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"active script: compiled code task closes");
}

static void active_script_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        ActiveScriptFixture f;if(!active_open(&f,policy)){active_close(&f);continue;}
        check(active_run(&f,0,"captureRecord(0);globalThis.foreign=C.Function('return [recordId(),recordRealm(),currentRealm(),recordName()]');","same-name.js")==OS64_JS_OK,
              "active script: foreign dynamic function captures original script");
        check(active_run(&f,0,"captureRecord(1);if(recordId()!==1)throw 1;","same-name.js")==OS64_JS_OK,
              "active script: identical filenames do not merge script identities");
        check(active_run(&f,1,"captureRecord(2);let r=A.foreign();if(r[0]!==0||r[1]!==0||r[2]!==2||r[3]!=='same-name.js'||recordId()!==2)throw 1;","caller.js")==OS64_JS_OK,
              "active script: creation record and execution realm remain independent");
        JS_RunGC(JS_GetRuntime(f.ctx[0]));
        check(active_run(&f,0,"captureRecord(3);Promise.resolve(\"globalThis.delayed=import('./delayed')\").then(C.eval);","delayed-source.js")==OS64_JS_OK,
              "active script: Promise eval restores source before dynamic import");
        check(f.loads[0]==1&&f.loads[1]==0&&f.loads[2]==0&&os64_strcmp(f.bases[0],"delayed-source.js")==0,
              "active script: delayed eval imports through captured source provider and base");
        check(active_run(&f,2,"globalThis.delayed.then(m=>{globalThis.delayedValue=m.value});","verify.js")==OS64_JS_OK,
              "active script: foreign eval receives source-owned module namespace");
        check(active_run(&f,2,"if(globalThis.delayedValue!==10)throw 1;","verify-settled.js")==OS64_JS_OK,
              "active script: delayed import settles rather than leaving verification pending");
        check(active_run(&f,0,"globalThis.moduleCallback.then(x=>globalThis.moduleSource=x)","check-module.js")==OS64_JS_OK&&
              active_run(&f,0,"if(globalThis.moduleSource!=='A/./delayed')throw 1","check-module-settled.js")==OS64_JS_OK,
              "active script: imported module await and author callbacks run under prepared module entry");
        check(active_run(&f,0,"globalThis.generator=function*(){captureRecord(4);yield recordId();yield recordId()};globalThis.asyncFunction=async()=>{captureRecord(5);await 1;return recordId()};","suspension.js")==OS64_JS_OK,
              "active script: suspended functions retain creation record");
        check(active_run(&f,1,"let it=A.generator();let x=it.next().value;if(it.next().value!==x)throw 1;A.asyncFunction().then(x=>{if(x!==4)throw 2});","resume.js")==OS64_JS_OK,
              "active script: generator and await restore exact record across suspension");
        check(active_run(&f,0,"captureRecord(6);if(eval('recordId()')!==6)throw 1;globalThis.indirect=(0,C.eval)('(function(){return [recordId(),currentRealm()]})');","eval-source.js")==OS64_JS_OK&&
              active_run(&f,1,"let q=A.indirect();if(q[0]!==6||q[1]!==2)throw 1;","eval-caller.js")==OS64_JS_OK,
              "active script: direct eval inherits immediate record and indirect-eval closures inherit active record");
        check(active_run(&f,0,active_clear_source,"clear-source.js")==OS64_JS_OK,"active script: clearable function defined");
        os64_js_outcome_t out;
        JSValue fn=realm_property(f.ctx[0],"clearable");
        check(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"clear-frame",&out)==OS64_JS_OK&&
              os64_js_realm_call(f.realm[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out)==OS64_JS_OK&&
              os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,
              "active script: active frame retains record through closure mutation and GC");
        JS_FreeValue(f.ctx[0],fn);
        JSRuntime *rt=JS_GetRuntime(f.ctx[0]);JS_SetStripInfo(rt,JS_STRIP_DEBUG);
        const char cache_source[]="captureRecord(7);globalThis.cached=()=>[recordName(),recordRealm()]";
        JSValue code=JS_Eval(f.ctx[0],cache_source,sizeof(cache_source)-1,"cached-original.js",JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
        JS_SetStripInfo(rt,0);size_t size=0;uint8_t *bytes=NULL;
        check(!JS_IsException(code),"active script: stripped script compiles");
        if(!JS_IsException(code))bytes=JS_WriteObject(f.ctx[0],&size,code,JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(f.ctx[0],code);
        check(bytes!=NULL,"active script: stripped bytecode serializes semantic name");
        if(bytes) {
            /* Every proper prefix refuses without keeping decoded atom holds. */
            for(size_t cut=0;cut<size;cut++) {
                JSValue partial=JS_ReadObject(f.ctx[1],bytes,cut,JS_READ_OBJ_BYTECODE);
                check(JS_IsException(partial),"active script: truncated semantic bytecode refuses");
                if(JS_IsException(partial))JS_FreeValue(f.ctx[1],JS_GetException(f.ctx[1]));
                JS_FreeValue(f.ctx[1],partial);
            }
            uint8_t version=bytes[0];bytes[0]=version-1;
            JSValue stale=JS_ReadObject(f.ctx[1],bytes,size,JS_READ_OBJ_BYTECODE);
            check(JS_IsException(stale),"active script: previous bytecode layout refuses");
            if(JS_IsException(stale))JS_FreeValue(f.ctx[1],JS_GetException(f.ctx[1]));
            JS_FreeValue(f.ctx[1],stale);bytes[0]=version;
            code=JS_ReadObjectNamed(f.ctx[1],bytes,size,JS_READ_OBJ_BYTECODE,"cached-rebound.js");
            js_free(f.ctx[0],bytes);check(!JS_IsException(code),"active script: stripped bytecode reads in another realm");
            if(!JS_IsException(code))active_evaluate_code(&f,1,code);
            else JS_FreeValue(f.ctx[1],JS_GetException(f.ctx[1]));
            check(active_run(&f,2,"let z=B.cached();if(z[0]!=='cached-rebound.js'||z[1]!==1)throw 1;","cached-caller.js")==OS64_JS_OK,
                  "active script: cached source name and record realm rebind without debug data");
        }
        const char *module_source[]={"captureRecord(10);globalThis.firstMeta=()=>import.meta.token;",
                                     "captureRecord(11);globalThis.secondMeta=()=>import.meta.token;"};
        for(int i=0;i<2;i++) {
            code=JS_Eval(f.ctx[0],module_source[i],os64_strlen(module_source[i]),"duplicate-module",JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
            check(!JS_IsException(code),"active script: same-name module compiles independently");
            if(!JS_IsException(code)) {
                JSValue meta=JS_GetImportMeta(f.ctx[0],JS_VALUE_GET_PTR(code));
                check(JS_SetPropertyStr(f.ctx[0],meta,"token",JS_NewInt32(f.ctx[0],i+11))>=0,"active script: exact module metadata initialized");
                JS_FreeValue(f.ctx[0],meta);active_evaluate_code(&f,0,code);
            } else JS_FreeValue(f.ctx[0],JS_GetException(f.ctx[0]));
        }
        check(active_run(&f,1,"if(A.firstMeta()!==11||A.secondMeta()!==12)throw 1;","module-caller.js")==OS64_JS_OK,
              "active script: import.meta follows exact retained module instead of filename lookup");
        check(JS_VALUE_GET_TAG(f.records[10])==JS_TAG_MODULE&&JS_VALUE_GET_TAG(f.records[11])==JS_TAG_MODULE&&
              JS_VALUE_GET_PTR(f.records[10])!=JS_VALUE_GET_PTR(f.records[11]),"active script: equal module names keep distinct record identities");
        check(active_run(&f,1,"globalThis.pending=new Promise(r=>globalThis.settle=r)","pending-owner.js")==OS64_JS_OK&&
              active_run(&f,0,"B.pending.then(C.eval.bind(null,\"globalThis.pendingName=recordName()\"))","pending-source.js")==OS64_JS_OK,
              "active script: native bound callback retains otherwise unreferenced script");
        JS_RunGC(rt);
        check(active_run(&f,1,"settle()","settlement-source.js")==OS64_JS_OK&&
              active_run(&f,2,"if(globalThis.pendingName!=='pending-source.js')throw 1","pending-check.js")==OS64_JS_OK,
              "active script: pending capture survives GC and unrelated settlement");
        check(active_run(&f,0,"Promise.reject(1).catch(C.eval.bind(null,\"globalThis.rejectName=recordName();throw 42\")).catch(()=>{})","reject-source.js")==OS64_JS_OK&&
              active_run(&f,2,"if(globalThis.rejectName!=='reject-source.js')throw 1","reject-check.js")==OS64_JS_OK,
              "active script: abrupt native callback restores and unwinds captured record");
        check(active_run(&f,0,"Promise.resolve({then:C.eval.bind(null,\"globalThis.thenName=recordName()\")})","then-source.js")==OS64_JS_OK&&
              active_run(&f,2,"if(globalThis.thenName!=='then-source.js')throw 1","then-check.js")==OS64_JS_OK,
              "active script: thenable invocation restores captured source");
        check(active_run(&f,0,"globalThis.abandoned=new Promise(()=>{});abandoned.then(C.eval)","abandoned-source.js")==OS64_JS_OK,
              "active script: pending native reaction keeps record until root teardown");
        JS_RunGC(rt);
        active_close(&f);
    }
}
#endif
