/* This host seam exposes proxies alongside raw globals for independent oracles.
 * It does not install global-this, public frame APIs or event proxy identities. */
static bool proxy_open(CeFixture *f, int policy, DBrowsingContext **contexts)
{
    if (!authority_open(f,policy,0)) return false;
    os64_js_outcome_t out;
    const char *names[]={"root","duplicate","duplicate","opaque","then"};
    for (unsigned i=0;i<5;i++) {
        contexts[i]=d_browsing_context_create(f->cluster,i?contexts[0]:NULL,f->bindings[i],names[i],&out);
        if(!contexts[i])return false;
        JSValue value=d_window_proxy(ce_contexts[0],contexts[i]);
        if(JS_IsException(value))return false;
        JS_FreeValue(ce_contexts[0],value);
    }
    for(unsigned i=0;i<5;i++) {
        JSValue global=JS_GetGlobalObject(ce_contexts[i]);
        for(unsigned j=0;j<5;j++) {
            char name[]={'P',(char)('A'+j),0};
            check(JS_SetPropertyStr(ce_contexts[i],global,name,d_window_proxy(ce_contexts[i],contexts[j]))>=0,
                "WindowProxy: trusted private exchange");
        }
        JS_FreeValue(ce_contexts[i],global);
    }
    return true;
}

static void dom_window_proxy_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;DBrowsingContext *c[5];
        if(!proxy_open(&f,policy,c)){check(false,"WindowProxy: fixture");ce_close(&f);continue;}
        os64_js_outcome_t out;
        realm_script(ce_realms[0],
            "function typeError(f){let e;try{f()}catch(x){e=x}assert(e&&e.name==='TypeError')}"
            "assert(PA!==A&&PA.window===PA&&PA.self===PA&&PA.frames===PA);"
            "assert(PA.parent===PA&&PA.top===PA&&PA.length===4);"
            "assert(PA[0]===PB&&PA[1]===PC&&PC.parent===PA&&PC.top===PA);"
            "assert(Reflect.getPrototypeOf(PA)===Reflect.getPrototypeOf(A));"
            "assert(Reflect.setPrototypeOf(PA,Reflect.getPrototypeOf(A))&&!Reflect.setPrototypeOf(PA,null));"
            "typeError(()=>Object.setPrototypeOf(PA,{}));assert(Object.isExtensible(PA)&&!Reflect.preventExtensions(PA));"
            "typeError(()=>Object.preventExtensions(PA));typeError(()=>Object.freeze(PA));"
            "let index=Object.getOwnPropertyDescriptor(PA,'0');assert(index.value===PB&&!index.writable&&index.enumerable&&index.configurable);"
            "assert(!Reflect.set(PA,'0',1)&&!Reflect.set(PA,'40',1));typeError(()=>{'use strict';PA[0]=1});"
            "assert(!Reflect.defineProperty(PA,'0',index)&&!Reflect.defineProperty(PA,'40',{value:1}));"
            "typeError(()=>Object.defineProperty(PA,'40',{value:1}));"
            "assert(!Reflect.deleteProperty(PA,'0')&&Reflect.deleteProperty(PA,'40'));typeError(()=>{'use strict';delete PA[0]});"
            "assert(Object.getOwnPropertyDescriptor(PA,'40')===undefined&&!Reflect.has(PA,'40'));"
            "for(let key of ['01','-1','4294967295','4294967296']){PA[key]=key;assert(A[key]===key)}"
            "assert(!Reflect.set(PA,'4294967294',1));"
            "let sym=Symbol('own');PA[sym]=17;assert(PA[sym]===17&&Reflect.ownKeys(PA).includes(sym));"
            "PA.expando=4;let receiver={};assert(Reflect.set(PA,'expando',8,receiver)&&receiver.expando===8&&PA.expando===4);"
            "Object.defineProperty(PA,'fixed',{value:7});assert(!Object.getOwnPropertyDescriptor(PA,'fixed').configurable);"
            "assert(!Reflect.defineProperty(PA,'fixed',{value:8})&&!Reflect.deleteProperty(PA,'fixed'));"
            "Object.defineProperty(PA,'reader',{get(){return this.tag},configurable:true});assert(Reflect.get(PA,'reader',{tag:42})===42);"
            "Object.defineProperty(Object.getPrototypeOf(A),'inheritedProbe',{get(){return this.tag},configurable:true});"
            "assert(Reflect.has(PA,'inheritedProbe')&&Reflect.get(PA,'inheritedProbe',{tag:33})===33);"
            "assert(Reflect.ownKeys(PA).slice(0,4).join(',')==='0,1,2,3');"
            "assert(Reflect.get(PC,'window',PA)===PA);typeError(()=>Reflect.get(PC,'window',{}));"
            "PA.self=23;assert(A.self===23&&PA.window===PA);delete PA.self;"
            "let unsupported;try{PA.postMessage(1,'*')}catch(e){unsupported=e}assert(unsupported&&unsupported.name==='NotSupportedError');",
            "WindowProxy: same-origin internal methods, storage, prototype, indices and native receiver routing");
        realm_script(ce_realms[0],"globalThis.sideEffects=0;Object.defineProperty(A,'secret',{get(){sideEffects++;return 9}})",
            "WindowProxy: target getter sentinel");
        realm_script(ce_realms[1],
            "function typeError(f){let e;try{f()}catch(x){e=x}assert(e&&e.name==='TypeError')}"
            "let w=PA;assert(w.window===w&&w.self===w&&w.frames===w&&w.length===4&&w.parent===w&&w.top===w&&w.opener===null&&!w.closed);"
            "assert(w[0]===PB&&w[1]===PC);denied(()=>w[4]);denied(()=>w[4294967294]);denied(()=>w[4294967295]);"
            "denied(()=>w.secret);assert(A.sideEffects===0);denied(()=>w.document);denied(()=>w.constructor);"
            "denied(()=>w.duplicate);denied(()=>w.opaque);"
            "for(let key of ['secret','document','0','then']){denied(()=>Reflect.set(w,key,1));denied(()=>Reflect.deleteProperty(w,key));denied(()=>Reflect.defineProperty(w,key,{value:1}))}"
            "denied(()=>Reflect.has(w,'missing'));denied(()=>Object.getOwnPropertyDescriptor(w,'missing'));"
            "assert(Reflect.has(w,'location')&&Reflect.has(w,'0'));assert(Object.getPrototypeOf(w)===null);"
            "assert(Reflect.setPrototypeOf(w,null)&&!Reflect.setPrototypeOf(w,{}));assert(Object.isExtensible(w)&&!Reflect.preventExtensions(w));"
            "typeError(()=>Object.freeze(w));"
            "let names=['window','self','location','close','closed','focus','blur','frames','length','top','opener','parent','postMessage','then'];"
            "let keys=Reflect.ownKeys(w);assert(keys.length===21&&keys.slice(4,18).join(',')===names.join(','));"
            "assert(keys[18]===Symbol.toStringTag&&keys[19]===Symbol.hasInstance&&keys[20]===Symbol.isConcatSpreadable);"
            "assert(Object.keys(w).join(',')==='0,1,2,3');"
            "for(let key of ['then',Symbol.toStringTag,Symbol.hasInstance,Symbol.isConcatSpreadable]){let d=Object.getOwnPropertyDescriptor(w,key);"
            "assert(d.value===undefined&&!d.writable&&!d.enumerable&&d.configurable&&w[key]===undefined)}"
            "let d=Object.getOwnPropertyDescriptor(w,'location');assert(d.configurable&&!d.enumerable&&typeof d.get==='function'&&typeof d.set==='function');"
            "assert(Object.getPrototypeOf(d.get)===Function.prototype&&d.get.length===0&&d.get.name===''&&d.set.length===1);"
            "assert(d.get===Object.getOwnPropertyDescriptor(w,'location').get&&d.set===Object.getOwnPropertyDescriptor(w,'location').set);"
            "assert(Reflect.apply(d.get,PB,[])===A.loc);"
            "globalThis.cachedGet=d.get;globalThis.cachedSet=d.set;globalThis.cachedClose=w.close;"
            "assert(typeof w.close==='function'&&w.close===cachedClose&&w.close.length===0);w.close();w.focus();w.blur();"
            "let unsupported;try{w.postMessage(1,'*')}catch(e){unsupported=e}assert(unsupported&&unsupported.name==='NotSupportedError');"
            "assert(A.Reflect.ownKeys(w).includes('secret'));",
            "WindowProxy: cross-origin matrix, current-versus-entry builtin realm and stable descriptor identity");
        NavigationFixture nav={.fixture=&f,.permit=true,.loaded=true,
            .retire_snapshot=-1,.retire_allow=-1,.retire_dispatch=-1};
        DNavigationPolicy navigation={&nav,navigation_snapshot,navigation_allow};
        check(d_navigation_policy(f.cluster,&navigation,&out)==OS64_JS_OK,"WindowProxy: navigation policy");
        realm_script(ce_realms[0],"Reflect.apply(B.cachedSet,PC,['https://destination.test/window'])",
            "WindowProxy: cross-origin location setter ignores substituted receiver");
        check(d_navigation_dispatch(c[0],navigation_consume,&nav,&out)&&nav.seen.target==f.bindings[0]&&
            !d_navigation_dispatch(c[2],navigation_consume,&nav,&out),"WindowProxy: bound Window owns navigation target");
        realm_script(ce_realms[0],
            "D.close=17;assert(PD.close===17);D.close=18;assert(PD.close===17);"
            "E.close=()=>{throw Error('author replacement')};PE.close();",
            "WindowProxy: noncallable original method value is cached; callable replacement does not replace IDL steps");
        /* Same target/global/key, distinct source globals even when origins match. */
        realm_script(ce_realms[3],"globalThis.firstGet=Object.getOwnPropertyDescriptor(PA,'location').get;assert(firstGet!==B.cachedGet)",
            "WindowProxy: distinct source realm cache");
        realm_script(ce_realms[4],"assert(Object.getOwnPropertyDescriptor(PA,'location').get!==D.firstGet)",
            "WindowProxy: equal source origins keep different global identities");
        JS_RunGC(JS_GetRuntime(ce_contexts[0]));size_t charge=f.cluster->bytes;
        realm_script(ce_realms[1],"for(let i=0;i<100;i++){assert(PA.close===cachedClose);assert(Object.getOwnPropertyDescriptor(PA,'location').get===cachedGet)}",
            "WindowProxy: cache survives GC without duplicate native holds");
        check(f.cluster->bytes==charge,"WindowProxy: repeated cache lookups do not grow native charge");
        os64_dom_child_t good=location_candidate(&f,0,"https://named.test/good");
        DBrowsingContext *good_context=d_browsing_context_create(f.cluster,c[0],good.binding,"good",&out);
        check(good_context!=NULL,"WindowProxy: same-origin named child");
        /* Lazy proxy creation must ignore a replaced Symbol getter. */
        JSValue global=JS_GetGlobalObject(ce_contexts[1]);
        check(JS_SetPropertyStr(ce_contexts[1],global,"goodRaw",JS_DupValue(ce_contexts[1],good.binding->window->value))>=0,
            "WindowProxy: trusted new-child sentinel exchange");JS_FreeValue(ce_contexts[1],global);
        realm_script(ce_realms[1],
            "Object.defineProperty(goodRaw,'Symbol',{get(){throw Error('must not consult author Symbol')},configurable:true});"
            "assert(PA.good===PA[4]&&PA.good!==goodRaw);let goodDesc=Object.getOwnPropertyDescriptor(PA,'good');"
            "assert(goodDesc.value===PA.good&&!goodDesc.enumerable&&!goodDesc.writable&&goodDesc.configurable);assert(!Reflect.ownKeys(PA).includes('good'));denied(()=>PA.good.document);",
            "WindowProxy: restricted child names expose proxies without author getters or document access");
        os64_dom_child_t collision=location_candidate(&f,0,"https://named.test/collision");
        check(d_browsing_context_create(f.cluster,c[0],collision.binding,"location",&out)!=NULL,"WindowProxy: builtin collision child");
        realm_script(ce_realms[1],"assert(PA.location===A.loc&&PA.location!==PA[5])","WindowProxy: cross-origin builtin precedes child name");
        realm_script(ce_realms[0],
            "globalThis.oldProxy=PB;globalThis.oldLocation=PB.location;globalThis.oldGetter=Object.getOwnPropertyDescriptor(PB,'location').get;"
            "globalThis.oldParent=Object.getOwnPropertyDescriptor(PB,'parent').get;globalThis.oldClose=PB.close;"
            "globalThis.oldSetter=Object.getOwnPropertyDescriptor(PB,'location').set;",
            "WindowProxy: retain original relevant Window descriptors");
        os64_dom_child_t replacement=location_candidate(&f,0,"https://replacement.test/a");
        check(d_browsing_context_replace(c[1],1,replacement.binding,&out)==OS64_JS_OK,"WindowProxy: cross-to-same replacement");
        check(os64_dom_depart_finish(f.bindings[1],&out)==OS64_JS_OK,"WindowProxy: original providers detach");
        realm_script(ce_realms[0],
            "assert(PB===oldProxy&&PB.location!==oldLocation&&PB.document&&PB.window===PB&&PB.parent===PA);"
            "assert(Reflect.apply(oldGetter,PA,[])===oldLocation&&Reflect.apply(oldParent,PA,[])===null);"
            "PB.replacementData=91;Object.defineProperty(PB,'fixedDocument',{value:1});",
            "WindowProxy: stable identity follows storage; retained descriptors keep original Window");
        realm_script(ce_realms[0],"Reflect.apply(oldSetter,PA,['https://destination.test/replacement'])",
            "WindowProxy: retained Window location setter routes through retained Location");
        check(d_navigation_dispatch(c[1],navigation_consume,&nav,&out)&&nav.seen.target==replacement.binding&&
            nav.seen.target_generation==2,"WindowProxy: retained setter delivers to current document generation");
        check(d_navigation_policy(f.cluster,NULL,&out)==OS64_JS_OK,"WindowProxy: revoke policy");
        realm_script(ce_realms[0],"let conversions=0;denied(()=>oldSetter({toString(){conversions++;return 'x'}}));assert(conversions===0)",
            "WindowProxy: cached setter cannot preserve revoked navigation authority");
        os64_dom_child_t again=location_candidate(&f,1,"https://replacement.test/b");
        check(d_browsing_context_replace(c[1],2,again.binding,&out)==OS64_JS_OK,"WindowProxy: same-to-cross replacement");
        realm_script(ce_realms[0],
            "denied(()=>PB.replacementData);denied(()=>PB.fixedDocument);assert(PB===oldProxy&&PB.close!==oldClose);"
            "assert(Object.getOwnPropertyDescriptor(PB,'location').get!==oldGetter);",
            "WindowProxy: cache separates new relevant global and denies departed storage");
        size_t cap=f.cluster->limits.dom_bytes;f.cluster->limits.dom_bytes=f.cluster->bytes;
        realm_script(ce_realms[2],"let e;try{PB.close}catch(x){e=x}assert(e&&e.name==='QuotaExceededError')",
            "WindowProxy: cache growth respects aggregate native cap");f.cluster->limits.dom_bytes=cap;
        check(d_browsing_context_discard(c[1],3,&out)==OS64_JS_OK,"WindowProxy: discard target");
        realm_script(ce_realms[0],
            "assert(PB.closed&&PB.parent===null&&PB.top===null&&PB.length===0&&PB.window===PB);denied(()=>PB[0]);"
            "assert(Reflect.apply(oldGetter,null,[])===oldLocation);",
            "WindowProxy: discarded identity preserves last Window and safe relation answers");
        realm_script(ce_realms[3],"retireD()","WindowProxy: depart cached-function creation realm");
        realm_script(ce_realms[4],"assert(Reflect.apply(D.firstGet,null,[])===A.loc)",
            "WindowProxy: live caller invokes a retained getter from a departed creation realm");
        JSValue raw=JS_GetPropertyStr(ce_contexts[0],c[1]->proxy->value,"closed");
        check(JS_IsException(raw),"WindowProxy: raw context does not invent current settings");
        if(JS_IsException(raw))JS_FreeValue(ce_contexts[0],JS_GetException(ce_contexts[0]));
        JS_FreeValue(ce_contexts[0],raw);
        ce_close(&f);
    }
}

typedef struct {
    DBrowsingContext *context;
    unsigned operation;
    size_t cut, allocations, before, after;
} WindowProxyCut;

static JSValue window_proxy_cut(JSContext *ctx, void *opaque)
{
    WindowProxyCut *probe=opaque;
    JSAtom key=JS_NewAtom(ctx,"location");
    if(key==JS_ATOM_NULL)return JS_EXCEPTION;
    size_t attempts_before=attempts;
    probe->before=probe->context->cluster->bytes;
    fail_at=probe->cut?attempts+probe->cut:0;
    JSValue result=JS_UNDEFINED;
    if(!probe->operation) result=d_window_proxy(ctx,probe->context);
    else if(probe->operation==1) {
        JSPropertyDescriptor desc;
        int found=JS_GetOwnProperty(ctx,&desc,probe->context->proxy->value,key);
        if(found<0)result=JS_EXCEPTION;
        else if(found){JS_FreeValue(ctx,desc.value);JS_FreeValue(ctx,desc.getter);JS_FreeValue(ctx,desc.setter);}
    } else {
        JSPropertyEnum *keys=NULL;uint32_t count=0;
        if(JS_GetOwnPropertyNames(ctx,&keys,&count,probe->context->proxy->value,JS_GPN_STRING_MASK|JS_GPN_SYMBOL_MASK)<0)
            result=JS_EXCEPTION;
        JS_FreePropertyEnum(ctx,keys,count);
    }
    fail_at=0;probe->allocations=attempts-attempts_before;
    probe->after=probe->context->cluster->bytes;
    JS_FreeAtom(ctx,key);
    return result;
}

static void window_proxy_allocation_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned operation=0;operation<3;operation++) {
        size_t count=0;
        for(size_t cut=0;cut<=count;cut++) {
            CeFixture f;if(!authority_open(&f,policy,0)){ce_close(&f);continue;}
            os64_js_outcome_t out;
            DBrowsingContext *context=d_browsing_context_create(f.cluster,NULL,f.bindings[1],"target",&out);
            check(context!=NULL,"WindowProxy allocations: native context");
            if(operation) {
                JSValue proxy=d_window_proxy(ce_contexts[0],context);
                check(!JS_IsException(proxy),"WindowProxy allocations: prepared identity");
                JS_FreeValue(ce_contexts[0],proxy);
            }
            check(os64_js_realm_task_begin(ce_realms[0],OS64_JS_ABI_ID,"WindowProxy cut",&out)==OS64_JS_OK,
                "WindowProxy allocations: checked task");
            WindowProxyCut probe={.context=context,.operation=operation,.cut=cut};
            JSValue result=JS_UNDEFINED;
            os64_js_status_t status=os64_js_realm_with_settings(ce_realms[0],ce_realms[0],OS64_JS_ABI_ID,
                window_proxy_cut,&probe,&result,&out);
            if(!cut){count=probe.allocations;check((count>0||operation==2)&&status==OS64_JS_OK&&!JS_IsException(result),
                "WindowProxy allocations: measure operation");}
            else check((status!=OS64_JS_OK||JS_IsException(result))&&probe.before==probe.after&&
                (operation||!context->proxy),"WindowProxy allocations: refusal is atomic and uncharged");
            if(JS_IsException(result))JS_FreeValue(ce_contexts[0],JS_GetException(ce_contexts[0]));
            JS_FreeValue(ce_contexts[0],result);
            check(os64_js_task_end(ce_runtime,OS64_JS_ABI_ID,&out)!=OS64_JS_BUSY,"WindowProxy allocations: unwind");
            ce_close(&f);
        }
        printf("WindowProxy allocation cuts policy %d operation %u: %zu\n",policy,operation,count);
    }
}
