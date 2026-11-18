#ifndef OS64_JS_GLOBAL_RECEIVER_CASES_H
#define OS64_JS_GLOBAL_RECEIVER_CASES_H

/* Deliberately expose storage to the fixture. The embedding receiver is an
 * ordinary object with disjoint properties, so redirecting storage to it
 * cannot accidentally satisfy the receiver assertions. */
static JSValue receiver_native_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_DupValue(ctx, self);
}

static JSValue receiver_native_set(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    JSValue global = JS_GetGlobalObject(ctx);
    int ok = JS_SetPropertyStr(ctx, global, "nativeSeen", JS_DupValue(ctx, self));
    JS_FreeValue(ctx, global);
    (void)argc; (void)argv;
    return ok < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static int receiver_descriptor(JSContext *ctx, JSPropertyDescriptor *desc, JSValueConst obj, JSAtom atom)
{
    (void)obj;
    const char *name = JS_AtomToCString(ctx, atom);
    if (!name) return -1;
    bool match = !os64_strcmp(name, "descriptorReceiver");
    JS_FreeCString(ctx, name);
    if (!match) return 0;
    if (desc) {
        desc->flags = JS_PROP_GETSET | JS_PROP_CONFIGURABLE;
        desc->value = JS_UNDEFINED;
        desc->getter = JS_NewCFunction(ctx, receiver_native_get, "get descriptorReceiver", 0);
        desc->setter = JS_NewCFunction(ctx, receiver_native_set, "set descriptorReceiver", 1);
        if (JS_IsException(desc->getter) || JS_IsException(desc->setter)) {
            JS_FreeValue(ctx, desc->getter); JS_FreeValue(ctx, desc->setter);
            return -1;
        }
    }
    return 1;
}

static JSValue receiver_exotic_get(JSContext *ctx, JSValueConst obj, JSAtom atom, JSValueConst receiver)
{
    (void)obj; (void)atom;
    return JS_DupValue(ctx, receiver);
}

static int receiver_exotic_set(JSContext *ctx, JSValueConst obj, JSAtom atom,
                              JSValueConst value, JSValueConst receiver, int flags)
{
    (void)obj; (void)atom; (void)flags;
    JSValue result = receiver_native_set(ctx, receiver, 1, &value);
    bool failed = JS_IsException(result);
    JS_FreeValue(ctx, result);
    return failed ? -1 : 1;
}

static void global_receiver_cases(void)
{
    /* The exported global can outlive its context. Its receiver context
     * must be traced rather than accessed through an unowned pointer. */
    JSRuntime *raw_runtime = JS_NewRuntime();
    JSContext *holder = raw_runtime ? JS_NewContext(raw_runtime) : NULL;
    JSContext *departing = holder ? JS_NewContextRaw(raw_runtime) : NULL;
    check(departing != NULL, "exported global lifetime contexts created");
    if (departing) {
        JSValue global = JS_GetGlobalObject(departing);
        JSValue expected = JS_NewObject(holder);
        check(JS_SetPrototype(departing, global, JS_NULL) == 1 && JS_SetGlobalThis(departing, expected),
              "minimal global has independent embedding receiver");
        JSAtom atom = JS_NewAtom(holder, "readReceiver");
        check(JS_DefinePropertyGetSet(holder, global, atom,
              JS_NewCFunction(holder, receiver_native_get, "get readReceiver", 0), JS_UNDEFINED,
              JS_PROP_CONFIGURABLE) == 1, "foreign native accessor installed on exported global");
        JS_FreeContext(departing);
        JS_RunGC(raw_runtime);
        JSValue value = JS_GetProperty(holder, global, atom);
        check(JS_IsObject(value) && JS_VALUE_GET_PTR(value) == JS_VALUE_GET_PTR(expected),
              "exported global retains receiver after original context release and GC");
        JS_FreeValue(holder, value); JS_FreeAtom(holder, atom);
        JS_FreeValue(holder, global); JS_FreeValue(holder, expected);
    }
    if (holder) JS_FreeContext(holder);
    if (raw_runtime) JS_FreeRuntime(raw_runtime);
    static const struct { const char *name, *source; } cases[] = {
        {"readonly global definitions preserve SameValue",
         "Object.defineProperty(raw,'fixed',{value:7});"
         "if(Reflect.defineProperty(raw,'fixed',{value:8})||raw.fixed!==7)throw 1;"
         "if(!Reflect.defineProperty(raw,'fixed',{value:7}))throw 2;"
         "var caught=false;try{Object.defineProperty(raw,'fixed',{value:8})}catch(e){caught=e instanceof TypeError}if(!caught)throw 3;"
         "Object.defineProperty(raw,'nanFixed',{value:NaN});if(!Reflect.defineProperty(raw,'nanFixed',{value:NaN}))throw 4;"
         "Object.defineProperty(raw,'zeroFixed',{value:0});if(Reflect.defineProperty(raw,'zeroFixed',{value:-0}))throw 5;"
         "var obj={};Object.defineProperty(raw,'objectFixed',{value:obj});"
         "if(!Reflect.defineProperty(raw,'objectFixed',{value:obj})||Reflect.defineProperty(raw,'objectFixed',{value:{}}))throw 6;"
         "Object.defineProperty(raw,'configurableFixed',{value:1,configurable:true});"
         "if(!Reflect.defineProperty(raw,'configurableFixed',{value:2})||raw.configurableFixed!==2)throw 7;"
         "Object.defineProperty(raw,'writableFixed',{value:1,writable:true});"
         "if(!Reflect.defineProperty(raw,'writableFixed',{value:2})||raw.writableFixed!==2)throw 8;"},
        {"global accessors keep storage and substitute strict receivers",
         "var seen, count=0;"
         "Object.defineProperty(raw,'accessor',{configurable:true,"
         "get:function(){'use strict';count++;return this},"
         "set:function(v){'use strict';count++;seen=[this,v]}});"
         "if(accessor!==expected||typeof accessor!=='object'||raw.accessor!==expected||raw['accessor']!==expected)throw 1;"
         "accessor=17;if(seen[0]!==expected||seen[1]!==17)throw 2;"
         "raw.accessor=18;raw['accessor']=19;if(seen[0]!==expected||seen[1]!==19)throw 3;"
         "var stored=23;stored++;implicit=31;let lexical=29;"
         "if(raw.stored!==24||raw.implicit!==31||'stored' in expected||'implicit' in expected||'lexical' in raw)throw 4;"},
        {"global reference and with paths preserve storage and receivers",
         "var n=3;Object.defineProperty(raw,'numberSlot',{configurable:true,"
         "get:function(){'use strict';if(this!==expected)throw 1;return n},"
         "set:function(v){'use strict';if(this!==expected)throw 2;n=v}});"
         "numberSlot++;numberSlot+=4;[numberSlot]=[12];({x:numberSlot}={x:14});"
         "with({}){numberSlot++;missingThroughWith=42}"
         "with(raw){numberSlot+=2}"
         "if(n!==17||raw.missingThroughWith!==42||'missingThroughWith' in expected)throw 3;"
         "with({}){var newBinding=18}if(raw.newBinding!==18||'newBinding' in expected)throw 4;"},
        {"inherited accessors and alternate receivers",
         "var proto=Object.getPrototypeOf(raw), alt={};"
         "Object.defineProperty(proto,'inheritedReceiver',{configurable:true,get:function(){'use strict';return this},"
         "set:function(v){'use strict';seen=[this,v]}});"
         "if(inheritedReceiver!==expected||raw.inheritedReceiver!==expected)throw 1;"
         "inheritedReceiver=5;if(seen[0]!==expected||seen[1]!==5)throw 2;"
         "if(Reflect.get(raw,'inheritedReceiver',alt)!==alt)throw 3;"
         "Reflect.set(raw,'inheritedReceiver',8,alt);if(seen[0]!==alt||seen[1]!==8)throw 4;"
         "if(Reflect.get(raw,'inheritedReceiver',null)!==null)throw 5;"
         "Reflect.set(raw,'inheritedReceiver',9,undefined);if(seen[0]!==undefined)throw 6;"
         "Reflect.set(raw,'alternateData',6,alt);if(alt.alternateData!==6||'alternateData' in raw)throw 7;"
         "delete proto.inheritedReceiver;"},
        {"native getter and setter receive embedding object",
         "if(nativeReceiver!==expected||raw.nativeReceiver!==expected)throw 1;"
         "nativeReceiver=1;if(nativeSeen!==expected)throw 2;"
         "var nativeGroup=Object.getOwnPropertyDescriptor(raw,'nativeReceiver').get;"
         "if(Object.keys(Object.groupBy([1],nativeGroup))[0]!=='undefined'||!Map.groupBy([1],nativeGroup).has(undefined))throw 3;"},
        {"bound and Proxy accessor invocation keeps explicit function semantics",
         "var fixed={};Object.defineProperty(raw,'boundReceiver',{configurable:true,get:(function(){'use strict';return this}).bind(fixed)});"
         "if(boundReceiver!==fixed)throw 1;"
         "Object.defineProperty(raw,'proxyReceiver',{configurable:true,get:new Proxy(function(){},{apply(t,self,args){return self}}),"
         "set:new Proxy(function(){},{apply(t,self,args){seen=[self,args[0]]}})});"
         "if(proxyReceiver!==expected)throw 2;proxyReceiver=11;if(seen[0]!==expected||seen[1]!==11)throw 3;"},
        {"getter throws and self-deletion leave valid references",
         "var sentinel={};Object.defineProperty(raw,'throwsReceiver',{configurable:true,get:function(){'use strict';"
         "if(this!==expected)throw 1;delete raw.throwsReceiver;throw sentinel}});"
         "try{throwsReceiver;throw 2}catch(e){if(e!==sentinel)throw e}"
         "if('throwsReceiver' in raw||accessor!==expected)throw 3;"
         "Object.defineProperty(raw,'throwsSetter',{configurable:true,set:function(){'use strict';"
         "if(this!==expected)throw 4;delete raw.throwsSetter;throw sentinel}});"
         "try{throwsSetter=1;throw 5}catch(e){if(e!==sentinel)throw e}"},
        {"globalThis property cannot select accessor receiver",
         "globalThis={};if(accessor!==expected||this!==expected||(0,eval)('accessor')!==expected||eval('accessor')!==expected)throw 1;"
         "if(Function('return accessor')()!==expected)throw 2;"},
        {"Object and Map GroupBy strict sloppy bound and Proxy receivers",
         "for(var group of [Object.groupBy,Map.groupBy]){"
         "var indices=[];group([7,8],function(v,i){'use strict';if(this!==undefined)throw 1;indices.push(i);return 0});"
         "if(indices.join(',')!=='0,1')throw 2;"
         "group([1],function(){if(this!==expected)throw 3;return 0});"
         "group([1],(function(){'use strict';if(this!==fixed)throw 4;return 0}).bind(fixed));"
         "group([1],new Proxy(function(){},{apply(t,self,args){if(self!==undefined||args[0]!==1||args[1]!==0)throw 5;return 0}}));"
         "var closed=0;var iterable={[Symbol.iterator](){return {next(){return {value:1,done:false}},return(){closed++;return {}}}}};"
         "try{group(iterable,function(){'use strict';if(this!==undefined)throw 6;throw sentinel})}catch(e){if(e!==sentinel)throw e}"
         "if(closed!==1)throw 7;}"},
        {"trapless Proxy prototypes preserve global storage and inherited accessors",
         "var savedProto=Object.getPrototypeOf(raw);"
         "var traplessTarget=Object.create(savedProto), alternateStorage={};"
         "Object.defineProperty(traplessTarget,'inheritedData',{value:12,writable:true,configurable:true});"
         "Object.defineProperty(traplessTarget,'forwardedAccessor',{configurable:true,"
         "get:function(){'use strict';return this},set:function(v){'use strict';seen=[this,v]}});"
         "var trapless=new Proxy(new Proxy(traplessTarget,{}),{});"
         "Object.setPrototypeOf(raw,trapless);"
         "try{raw.traplessDot=42;raw['traplessComputed']=43;implicitThroughProxy=44;"
         "with({}){implicitProxyReference=45}"
         "raw.inheritedData=46;"
         "if(!Object.hasOwn(raw,'traplessDot')||!Object.hasOwn(raw,'traplessComputed')||"
         "!Object.hasOwn(raw,'implicitThroughProxy')||!Object.hasOwn(raw,'implicitProxyReference')||"
         "!Object.hasOwn(raw,'inheritedData'))throw 1;"
         "if(traplessDot!==42||traplessComputed!==43||implicitThroughProxy!==44||implicitProxyReference!==45||inheritedData!==46)throw 2;"
         "if('traplessDot' in expected||'traplessComputed' in expected||'implicitThroughProxy' in expected||"
         "'implicitProxyReference' in expected||'inheritedData' in expected||traplessTarget.inheritedData!==12)throw 3;"
         "if(forwardedAccessor!==expected)throw 4;forwardedAccessor=47;"
         "if(seen[0]!==expected||seen[1]!==47)throw 5;"
         "if(Reflect.get(raw,'forwardedAccessor',alternateStorage)!==alternateStorage)throw 6;"
         "Reflect.set(raw,'forwardedAccessor',48,alternateStorage);if(seen[0]!==alternateStorage)throw 7;"
         "if(!Reflect.set(raw,'alternateSlot',49,alternateStorage)||alternateStorage.alternateSlot!==49||"
         "Object.hasOwn(raw,'alternateSlot')||'alternateSlot' in expected)throw 8;"
         "}finally{Object.setPrototypeOf(raw,savedProto)}"},
        {"nested Proxy forwarding substitutes only when a trap is called",
         "var proxyBase=Object.getPrototypeOf(raw), trapCalls=0, deepTrapReceiver;"
         "var trapped=new Proxy(proxyBase,{get(t,p,r){if(p==='deepReceiver')return r;return Reflect.get(t,p,r)},"
         "set(t,p,v,r){if(p==='deepReceiver'){trapCalls++;deepTrapReceiver=r;return true}return Reflect.set(t,p,v,r)},"
         "has(t,p){return p==='deepReceiver'||Reflect.has(t,p)}});"
         "Object.setPrototypeOf(raw,new Proxy(new Proxy(trapped,{}),{}));"
         "try{if(deepReceiver!==expected)throw 1;deepReceiver=50;"
         "if(trapCalls!==1||deepTrapReceiver!==expected)throw 2;"
         "Reflect.set(raw,'deepReceiver',51,alternateStorage);"
         "if(trapCalls!==2||deepTrapReceiver!==alternateStorage||Reflect.get(raw,'deepReceiver',alternateStorage)!==alternateStorage)throw 3;"
         "}finally{Object.setPrototypeOf(raw,proxyBase)}"},
        {"prototype Proxy callbacks cannot see global storage receiver",
         "var originalProto=Object.getPrototypeOf(raw), received;"
         "var exoticProto=new Proxy(originalProto,{get(t,p,r){if(p==='trapReceiver')return r;return Reflect.get(t,p,r)},"
         "set(t,p,v,r){if(p==='trapReceiver'){received=[r,v];return true}return Reflect.set(t,p,v,r)},"
         "has(t,p){return p==='trapReceiver'||Reflect.has(t,p)}});"
         "Object.setPrototypeOf(raw,exoticProto);"
         "try{if(trapReceiver!==expected||raw.trapReceiver!==expected)throw 1;"
         "trapReceiver=9;if(received[0]!==expected||received[1]!==9)throw 2;"
         "if(Reflect.get(raw,'trapReceiver',alt)!==alt)throw 3;"
         "Reflect.set(raw,'trapReceiver',10,alt);if(received[0]!==alt)throw 4;"
         "}finally{Object.setPrototypeOf(raw,originalProto)}"}
    };
    for (unsigned policy=0; policy<2; ++policy) {
        os64_js_config_t config=fixture_config();
        os64_js_runtime_t *runtime=NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy ? OS64_JS_TEARDOWN_RECLAIM : OS64_JS_TEARDOWN_FATAL,
              OS64_JS_ABI_ID, &runtime, &out)==OS64_JS_OK, "receiver runtime created");
        if (!runtime) continue;
        os64_js_realm_t *a=os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out), *b=NULL;
        check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&b,&out)==OS64_JS_OK,"receiver child created");
        if (!b) {os64_js_destroy(runtime);continue;}
        check(realm_execute(a,
            "for(var group of [Object.groupBy,Map.groupBy]){"
            "group([1],function(){'use strict';if(this!==undefined)throw 1;return 0});"
            "group([1],function(){if(this!==globalThis)throw 2;return 0})}", &out)==OS64_JS_OK,
            "unconfigured GroupBy preserves sloppy global and strict undefined receivers");
        os64_js_realm_t *realms[]={a,b};
        JSContext *contexts[]={os64_js_realm_context(a,OS64_JS_ABI_ID,&out),os64_js_realm_context(b,OS64_JS_ABI_ID,&out)};
        for (unsigned i=0;i<2;++i) {
            JSContext *ctx=contexts[i];JSValue global=JS_GetGlobalObject(ctx), receiver=JS_NewObject(ctx);
            check(JS_SetGlobalThis(ctx,receiver) && JS_SetPropertyStr(ctx,global,"raw",JS_DupValue(ctx,global))==1 &&
                JS_SetPropertyStr(ctx,global,"expected",JS_DupValue(ctx,receiver))==1,
                "disjoint global receiver and storage installed");
            JSAtom atom=JS_NewAtom(ctx,"nativeReceiver");
            check(JS_DefinePropertyGetSet(ctx,global,atom,JS_NewCFunction(ctx,receiver_native_get,"get nativeReceiver",0),
                JS_NewCFunction(ctx,receiver_native_set,"set nativeReceiver",1),JS_PROP_CONFIGURABLE)==1,"native accessors installed");
            JS_FreeAtom(ctx,atom);JS_FreeValue(ctx,receiver);JS_FreeValue(ctx,global);
            for (size_t c=0;c<sizeof(cases)/sizeof(cases[0]);++c)
                check(realm_execute(realms[i],cases[c].source,&out)==OS64_JS_OK,cases[c].name);
            const char *source="if(accessor!==expected)throw 1;numberSlot++;with({}){numberSlot+=2}";
            os64_js_bytecode_t write={.keep=true};
            check(os64_js_realm_run_bytecode(realms[i],source,os64_strlen(source),"receiver-cache",&write,&out)==OS64_JS_OK && write.kept,
                  "compiled global references substitute callback receiver");
            os64_js_bytecode_t read={.blob=write.kept,.blob_length=write.kept_length};
            check(os64_js_realm_run_bytecode(realms[i],source,os64_strlen(source),"receiver-cache",&read,&out)==OS64_JS_OK && read.from_blob,
                  "cached global references substitute callback receiver");
            os64_free(write.kept);
            JSClassID id=0;JS_NewClassID(&id);
            static JSClassExoticMethods exotic={.get_own_property=receiver_descriptor};
            JSClassDef def={.class_name="ReceiverDescriptor",.exotic=&exotic};
            check(JS_NewClass(JS_GetRuntime(ctx),id,&def)==0,"descriptor exotic registered");
            global=JS_GetGlobalObject(ctx);JSValue proto=JS_GetPrototype(ctx,global);
            JSValue custom=JS_NewObjectProtoClass(ctx,JS_NULL,id);
            check(JS_SetPrototype(ctx,global,custom)==1,"descriptor prototype installed");
            check(realm_execute(realms[i],"if(descriptorReceiver!==expected)throw 1;descriptorReceiver=1;if(nativeSeen!==expected)throw 2",&out)==OS64_JS_OK,
                  "native exotic descriptor getter and setter receive embedding object");
            check(JS_SetPrototype(ctx,global,proto)==1,"descriptor prototype restored");
            JS_FreeValue(ctx,custom);
            JSClassID direct_id=0;JS_NewClassID(&direct_id);
            static JSClassExoticMethods direct_exotic={.get_own_property=receiver_descriptor,
                .get_property=receiver_exotic_get,.set_property=receiver_exotic_set};
            JSClassDef direct_def={.class_name="ReceiverCallbacks",.exotic=&direct_exotic};
            check(JS_NewClass(JS_GetRuntime(ctx),direct_id,&direct_def)==0,"native exotic callbacks registered");
            custom=JS_NewObjectProtoClass(ctx,JS_NULL,direct_id);
            check(JS_SetPrototype(ctx,global,custom)==1,"native callback prototype installed");
            check(realm_execute(realms[i],
                  "if(descriptorReceiver!==expected)throw 1;descriptorReceiver=1;if(nativeSeen!==expected)throw 2;"
                  "if(Reflect.get(raw,'descriptorReceiver',alt)!==alt)throw 3;"
                  "Reflect.set(raw,'descriptorReceiver',2,alt);if(nativeSeen!==alt)throw 4;",&out)==OS64_JS_OK,
                  "native exotic callback boundary still substitutes globals and preserves alternate receivers");
            check(JS_SetPrototype(ctx,global,proto)==1,"native callback prototype restored");
            JS_FreeValue(ctx,custom);JS_FreeValue(ctx,proto);JS_FreeValue(ctx,global);
        }
        realm_share(contexts[1],contexts[0],"raw");
        check(realm_execute(a,"var foreignRaw=raw;",&out)==OS64_JS_OK,"foreign raw fixture captured");
        JSValue global=JS_GetGlobalObject(contexts[0]);
        JS_SetPropertyStr(contexts[0],global,"raw",JS_DupValue(contexts[0],global));JS_FreeValue(contexts[0],global);
        check(realm_execute(a,
            "Object.defineProperty(foreignRaw,'foreignGetter',{configurable:true,get:function(){'use strict';return this},"
            "set:function(v){'use strict';seen=[this,v]}});"
            "if(foreignRaw.foreignGetter!==foreignRaw.expected||Reflect.get(raw,'accessor',foreignRaw)!==foreignRaw.expected)throw 1;"
            "foreignRaw.foreignGetter=7;if(seen[0]!==foreignRaw.expected||seen[1]!==7)throw 2;"
            "Object.groupBy([1],foreignRaw.sloppyGroup=function(){if(this!==expected)throw 3;return 0});",&out)==OS64_JS_OK,
            "foreign getter code cannot choose global receiver by its execution realm");
        check(realm_execute(b,"var strictGroup=function(){'use strict';if(this!==undefined)throw 1;return 0};"
            "var sloppyGroup=function(){if(this!==expected)throw 2;return 0};",&out)==OS64_JS_OK,"foreign GroupBy callbacks created");
        realm_share(contexts[1],contexts[0],"strictGroup");realm_share(contexts[1],contexts[0],"sloppyGroup");
        check(realm_execute(a,"for(var group of [Object.groupBy,Map.groupBy]){group([1],strictGroup);group([1],sloppyGroup)}",&out)==OS64_JS_OK,
            "GroupBy sloppy callback uses callback realm rather than builtin realm");
        JS_RunGC(JS_GetRuntime(contexts[0]));
        check(realm_execute(a,"if(accessor!==expected||foreignRaw.foreignGetter!==foreignRaw.expected)throw 1",&out)==OS64_JS_OK,
            "receiver edges survive collection across realms");
        os64_js_teardown_report_t report;os64_js_destroy_report(runtime,&report);
        check(!report.leaked,"receiver graph tears down under both policies");
    }
}
#endif
