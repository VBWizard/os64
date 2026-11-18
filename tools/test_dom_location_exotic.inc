/* No WindowProxy is published: the host exchanges raw globals to reach the
 * private origin-bearing Locations and vary the property caller independently. */
/* Link-time interception confines engine allocation cuts to private bootstrap;
 * ordinary host probes and shipping code call the real implementation. */
extern int __real_d_location_exotic_install(os64_dom_t *, JSContext *, JSValueConst);
static bool location_install_armed;
static size_t location_install_cut, location_install_allocations;
int __wrap_d_location_exotic_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    if(!location_install_armed||!dom->binding_origin)
        return __real_d_location_exotic_install(dom,ctx,global);
    size_t before=attempts;
    fail_at=location_install_cut?attempts+location_install_cut:0;
    int result=__real_d_location_exotic_install(dom,ctx,global);
    fail_at=0;location_install_allocations=attempts-before;
    return result;
}

static void location_install_engine_cuts(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        size_t count=0;
        for(size_t cut=0;cut<=count;cut++) {
            CeFixture f;if(!authority_open(&f,policy,0)){ce_close(&f);continue;}
            os64_js_outcome_t out;location_install_cut=cut;location_install_armed=true;
            os64_dom_child_t child=context_candidate_prepare(&f,0,&out);
            location_install_armed=false;
            if(!cut) {
                count=location_install_allocations;
                check(child.state==OS64_DOM_CHILD_ACTIVE&&child.binding->location_exotic&&count>0,
                    "Location exotic: measure private bootstrap allocation path");
            } else check(child.consumed&&child.state!=OS64_DOM_CHILD_ACTIVE,
                "Location exotic: bootstrap refusal never publishes a partial object");
            ce_close(&f);
        }
        printf("Location bootstrap allocation cuts policy %d: %zu\n",policy,count);
    }
}

typedef struct {
    os64_dom_t *target;
    size_t cut, allocations;
} LocationCacheCut;

static JSValue location_cache_cut(JSContext *ctx, void *opaque)
{
    LocationCacheCut *probe=opaque;size_t before=attempts;
    fail_at=probe->cut?attempts+probe->cut:0;
    JSValue result=JS_GetPropertyStr(ctx,probe->target->location->value,"replace");
    fail_at=0;probe->allocations=attempts-before;
    return result;
}

static void location_cache_engine_cuts(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        size_t count=0;
        for(size_t cut=0;cut<=count;cut++) {
            CeFixture f;if(!authority_open(&f,policy,0)){ce_close(&f);continue;}
            os64_js_outcome_t out;
            check(os64_js_realm_task_begin(ce_realms[0],OS64_JS_ABI_ID,"Location cache cut",&out)==OS64_JS_OK,
                "Location exotic: checked cache allocation task");
            LocationCacheCut probe={.target=f.bindings[1],.cut=cut};JSValue result=JS_UNDEFINED;
            os64_js_status_t status=os64_js_realm_with_settings(ce_realms[0],ce_realms[0],OS64_JS_ABI_ID,
                location_cache_cut,&probe,&result,&out);
            if(!cut) {
                count=probe.allocations;
                check(status==OS64_JS_OK&&!JS_IsException(result)&&count>0,
                    "Location exotic: measure first cached-function allocation path");
            } else check(status!=OS64_JS_OK||JS_IsException(result),
                "Location exotic: engine/native cache allocation refusal does not return a usable descriptor");
            if(JS_IsException(result)){JSValue error=JS_GetException(ce_contexts[0]);JS_FreeValue(ce_contexts[0],error);}
            JS_FreeValue(ce_contexts[0],result);
            check(os64_js_task_end(ce_runtime,OS64_JS_ABI_ID,&out)!=OS64_JS_BUSY,
                "Location exotic: refusal unwinds task and settings loans");
            ce_close(&f);
        }
        printf("Location descriptor allocation cuts policy %d: %zu\n",policy,count);
    }
}

static void dom_location_exotic_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;DBrowsingContext *c[5];
        if(!navigation_open(&f,policy,c)){check(false,"Location exotic: fixture");ce_close(&f);continue;}
        NavigationFixture n={.fixture=&f,.permit=true,.loaded=true,
            .retire_snapshot=-1,.retire_allow=-1,.retire_dispatch=-1};
        DNavigationPolicy host={&n,navigation_snapshot,navigation_allow};os64_js_outcome_t out;
        check(d_navigation_policy(f.cluster,&host,&out)==OS64_JS_OK,"Location exotic: policy");
        realm_script(ce_realms[0],
            "function typeError(f){let e;try{f()}catch(x){e=x}assert(e&&e.name==='TypeError')}"
            "assert(Object.getPrototypeOf(loc)===Location.prototype&&loc instanceof Location);"
            "assert(Object.isExtensible(loc)&&!Reflect.preventExtensions(loc));typeError(()=>Object.preventExtensions(loc));"
            "assert(Reflect.setPrototypeOf(loc,Location.prototype)&&!Reflect.setPrototypeOf(loc,null));"
            "typeError(()=>Object.setPrototypeOf(loc,{}));"
            "let d=Object.getOwnPropertyDescriptor(loc,'href');assert(d.configurable&&d.enumerable&&typeof d.get==='function'&&typeof d.set==='function');"
            "assert(!Reflect.defineProperty(loc,'href',d)&&!Reflect.deleteProperty(loc,'href'));"
            "typeError(()=>Object.defineProperty(loc,'href',d));typeError(()=>{'use strict';delete loc.href});"
            "assert(!Reflect.set(loc,'replace',1));typeError(()=>{'use strict';loc.replace=1});"
            "loc.extra=4;assert(loc.extra===4&&Reflect.has(loc,'extra'));"
            "let receiver={};assert(Reflect.set(loc,'extra',7,receiver)&&receiver.extra===7&&loc.extra===4);"
            "Object.defineProperty(loc,'fixed',{value:19});assert(Object.getOwnPropertyDescriptor(loc,'fixed').configurable===false);"
            "assert(!Reflect.deleteProperty(loc,'fixed')&&!Reflect.defineProperty(loc,'fixed',{value:20}));"
            "Object.defineProperty(loc,'read',{get(){return this.tag},configurable:true});assert(Reflect.get(loc,'read',{tag:42})===42);"
            "typeError(()=>Reflect.get(loc,'href',{}));"
            "assert(Reflect.deleteProperty(loc,'extra')&&!Reflect.has(loc,'extra'));"
            "loc[0]='zero';loc[4294967295]='large';let symbol=Symbol('expando');loc[symbol]=23;"
            "assert(loc[0]==='zero'&&loc[4294967295]==='large'&&loc[symbol]===23);"
            "assert(Reflect.ownKeys(loc).includes(symbol)&&Object.keys(loc).includes('0'));",
            "Location exotic: same-origin reflection, immutable prototype, default descriptors and alternate receivers");
        realm_script(ce_realms[1],"globalThis.probeCount=0;Object.defineProperty(Location.prototype,'probe',{get(){probeCount++;return 5}})",
            "Location exotic: install target-side getter sentinel");
        realm_script(ce_realms[0],
            "denied(()=>B.loc.probe);assert(B.probeCount===0);let conversionCount=0;"
            "denied(()=>B.loc.hash={toString(){conversionCount++;return 'x'}});assert(conversionCount===0);"
            "let foreign=B.loc;let keys=Reflect.ownKeys(foreign);"
            "assert(B.Reflect.ownKeys(foreign).includes('assign'));assert(keys.length===6&&keys[0]==='href'&&keys[1]==='replace'&&keys[2]==='then');"
            "assert(keys[3]===Symbol.toStringTag&&keys[4]===Symbol.hasInstance&&keys[5]===Symbol.isConcatSpreadable);"
            "assert(Object.keys(foreign).length===0&&Object.getPrototypeOf(foreign)===null);"
            "assert(Object.isExtensible(foreign)&&!Reflect.preventExtensions(foreign));typeError(()=>Object.freeze(foreign));"
            "assert(Reflect.setPrototypeOf(foreign,null)&&!Reflect.setPrototypeOf(foreign,{}));"
            "for(let key of ['href','replace','then',Symbol.toStringTag,Symbol.hasInstance,Symbol.isConcatSpreadable])assert(Reflect.has(foreign,key));"
            "for(let key of ['assign','constructor','__proto__','toString','origin','unknown','0','4294967295',Symbol.toPrimitive,Symbol('unknown')]){"
            "denied(()=>Reflect.get(foreign,key));denied(()=>Reflect.has(foreign,key));denied(()=>Object.getOwnPropertyDescriptor(foreign,key));"
            "denied(()=>Reflect.set(foreign,key,1));denied(()=>Reflect.deleteProperty(foreign,key));denied(()=>Reflect.defineProperty(foreign,key,{value:1}));}"
            "denied(()=>foreign.href);denied(()=>Reflect.set(foreign,'replace',1));denied(()=>Reflect.deleteProperty(foreign,'then'));"
            "for(let key of ['then',Symbol.toStringTag,Symbol.hasInstance,Symbol.isConcatSpreadable]){let d=Object.getOwnPropertyDescriptor(foreign,key);"
            "assert(d.value===undefined&&!d.writable&&!d.enumerable&&d.configurable&&foreign[key]===undefined)}"
            "let crossHref=Object.getOwnPropertyDescriptor(foreign,'href');assert(crossHref.get===undefined&&typeof crossHref.set==='function'&&!crossHref.enumerable&&crossHref.configurable);"
            "globalThis.savedSetter=crossHref.set;globalThis.savedReplace=foreign.replace;assert(savedSetter!==savedReplace);"
            "assert(savedSetter===Object.getOwnPropertyDescriptor(foreign,'href').set&&savedReplace===foreign.replace);"
            "assert(Object.getPrototypeOf(savedReplace)===Function.prototype&&savedReplace.length===1&&savedReplace.name==='');"
            "let r=Object.getOwnPropertyDescriptor(foreign,'replace');assert(r.value===savedReplace&&!r.writable&&!r.enumerable&&r.configurable);"
            "assert(savedReplace!==E.loc.replace);",
            "Location exotic: complete cross-origin property matrix and stable descriptor shapes");
        realm_script(ce_realms[2],
            "assert(B.loc.replace!==A.savedReplace);assert(Object.getOwnPropertyDescriptor(B.loc,'href').set!==A.savedSetter);"
            "assert(Object.getPrototypeOf(B.loc.replace)===Function.prototype)",
            "Location exotic: equal origins do not collapse accessing-global cache identity");
        JS_RunGC(JS_GetRuntime(ce_contexts[0]));
        size_t charge=f.cluster->bytes;
        realm_script(ce_realms[0],"for(let i=0;i<100;i++){assert(B.loc.replace===savedReplace);assert(Object.getOwnPropertyDescriptor(B.loc,'href').set===savedSetter)}",
            "Location exotic: descriptor reuse remains stable");
        check(f.cluster->bytes==charge,"Location exotic: repeated descriptor lookups add no native holds");
        realm_script(ce_realms[0],"Reflect.apply(savedSetter,E.loc,['from-setter']);Reflect.apply(savedReplace,E.loc,['from-replace'])",
            "Location exotic: cached functions ignore a substituted receiver");
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.seen.target==f.bindings[1]&&n.seen.kind==OS64_DOM_NAVIGATE_REPLACE&&
            !strcmp(n.seen.url,"https://base-a.test/folder/from-replace")&&!d_navigation_dispatch(c[4],navigation_consume,&n,&out),
            "Location exotic: original descriptor target receives navigation");
        realm_script(ce_realms[0],"assert(Reflect.set(B.loc,'href','alternate',E.loc))",
            "Location exotic: cross-origin Reflect.set forwards through bound setter");
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&!strcmp(n.seen.url,"https://base-a.test/folder/alternate"),
            "Location exotic: alternate receiver cannot retarget navigation");
        os64_dom_child_t replacement=location_candidate(&f,0,"https://replacement.test/current");
        check(replacement.binding&&d_browsing_context_replace(c[1],1,replacement.binding,&out)==OS64_JS_OK,
            "Location exotic: replace the descriptor target's active document");
        check(os64_dom_depart_finish(f.bindings[1],&out)==OS64_JS_OK,"Location exotic: detach old providers");
        realm_script(ce_realms[0],"assert(B.loc.replace===savedReplace);denied(()=>B.loc.href);savedReplace('after-replacement')",
            "Location exotic: old relevant-global gate and cached identity survive replacement");
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.seen.target==replacement.binding&&n.seen.target_generation==2,
            "Location exotic: retained descriptor resolves current generation");
        check(d_navigation_policy(f.cluster,NULL,&out)==OS64_JS_OK,"Location exotic: revoke navigation policy");
        realm_script(ce_realms[0],"denied(()=>savedReplace('revoked'))","Location exotic: cached permission does not bypass policy revocation");
        check(d_browsing_context_discard(c[1],2,&out)==OS64_JS_OK,"Location exotic: discard retained target");
        realm_script(ce_realms[0],"savedReplace('ignored');assert(B.loc.replace===savedReplace);denied(()=>B.loc.href)",
            "Location exotic: discard retains property authority but invokes no navigation");
        check(!d_navigation_dispatch(c[1],navigation_consume,&n,&out),"Location exotic: discarded descriptor target produces no delivery");
        size_t cap=f.cluster->limits.dom_bytes;f.cluster->limits.dom_bytes=f.cluster->bytes;
        realm_script(ce_realms[3],"let capError;try{B.loc.replace}catch(e){capError=e}assert(capError&&capError.name==='QuotaExceededError')",
            "Location exotic: aggregate cap refuses a new accessing-global cache entry");
        f.cluster->limits.dom_bytes=cap;
        JSValue raw=JS_GetPropertyStr(ce_contexts[0],f.bindings[1]->location->value,"replace");
        check(JS_IsException(raw),"Location exotic: raw ctx cannot invent current settings");
        if(JS_IsException(raw)){JSValue error=JS_GetException(ce_contexts[0]);JS_FreeValue(ce_contexts[0],error);}
        JS_FreeValue(ce_contexts[0],raw);
        check(d_navigation_policy(f.cluster,&host,&out)==OS64_JS_OK,"Location exotic: policy restored for live sibling");
        realm_script(ce_realms[0],"globalThis.cachedE=E.loc.replace;retireA()",
            "Location exotic: retire descriptor creation realm");
        realm_script(ce_realms[2],"Reflect.apply(A.cachedE,null,['live-caller'])",
            "Location exotic: retained descriptor checks the invoking caller, not its creation realm");
        check(d_navigation_dispatch(c[4],navigation_consume,&n,&out)&&n.seen.source==f.bindings[2]&&
            n.seen.target==f.bindings[4]&&!strcmp(n.seen.url,"https://base-c.test/folder/live-caller"),
            "Location exotic: live caller retains its entry base and navigation source");
        ce_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned cut=1;cut<=2;cut++) {
        CeFixture f;if(!authority_open(&f,policy,0)){ce_close(&f);continue;}
        size_t charge=f.cluster->bytes;dom_fail_at=dom_attempts+cut;
        realm_script(ce_realms[0],"let e;try{B.loc.replace}catch(x){e=x}assert(e&&e.name==='QuotaExceededError')",
            "Location exotic: cache allocation/hold refusal");dom_fail_at=0;
        check(f.cluster->bytes==charge,"Location exotic: cache refusal rolls back native charge");
        realm_script(ce_realms[0],"assert(typeof B.loc.replace==='function'&&B.loc.replace===B.loc.replace)",
            "Location exotic: clean retry after cache allocation failure");
        ce_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;if(!authority_open(&f,policy,0)){ce_close(&f);continue;}
        os64_js_outcome_t out;size_t before=dom_attempts;
        os64_dom_child_t probe=context_candidate_prepare(&f,0,&out);
        size_t allocations=dom_attempts-before;
        check(probe.state==OS64_DOM_CHILD_ACTIVE&&probe.binding->location_exotic&&allocations>=3,
            "Location exotic: measure origin-bearing installation tail");
        ce_close(&f);
        for(size_t cut=allocations-2;cut<=allocations;cut++) {
            if(!authority_open(&f,policy,0)){ce_close(&f);continue;}
            os64_dom_child_t refused=context_candidate_prepare(&f,cut,&out);
            check(refused.consumed&&refused.state!=OS64_DOM_CHILD_ACTIVE&&refused.state!=OS64_DOM_CHILD_RUNTIME_FAILED,
                "Location exotic: native record/backing hold/post-install refusal retains failed candidate");
            realm_script(ce_realms[0],"assert(location.href==='https://authority-b.test/document')",
                "Location exotic: failed candidate leaves original binding usable");
            ce_close(&f);
        }
    }

    location_cache_engine_cuts();
    location_install_engine_cuts();

}
