/* Trusted host exchange exercises retained Location members without publishing
 * an incomplete WindowProxy. Origins intentionally differ from document URLs. */
static os64_dom_child_t location_candidate(CeFixture *f, unsigned origin_index, const char *url)
{
    DomFixture doc=child_install_document();
    os64_dom_options_t options=p2b_options(url);
    os64_dom_installation_t installation={.engine_headroom=OS64_DOM_CHILD_ENGINE_HEADROOM,
        .now_ms=4000,.now_us=4000000};
    for(unsigned i=0;i<OS64_DOM_TIMING_COUNT;i++)installation.timing.at[i]=OS64_DOM_TIMING_NOT_YET;
    os64_js_outcome_t out;
    os64_dom_child_t child=d_create_child_with_origin(f->cluster,doc.document,doc.state,
        &options,&installation,f->bindings[origin_index]->binding_origin,&out);
    child_install_unconsumed(&doc,child);
    check(child.state==OS64_DOM_CHILD_ACTIVE,"Location: replacement candidate admitted");
    return child;
}

static void dom_location_context_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;DBrowsingContext *c[5];
        if(!navigation_open(&f,policy,c)){check(false,"Location: fixture");ce_close(&f);continue;}
        NavigationFixture n={.fixture=&f,.permit=true,.loaded=true,
            .retire_snapshot=-1,.retire_allow=-1,.retire_dispatch=-1};
        DNavigationPolicy host={&n,navigation_snapshot,navigation_allow};os64_js_outcome_t out;
        check(d_navigation_policy(f.cluster,&host,&out)==OS64_JS_OK,"Location: policy installed");
        realm_script(ce_realms[0],
            "globalThis.oldLoc=C.loc;globalThis.readOld=()=>C.loc.href;"
            "globalThis.assignOld=x=>Reflect.apply(C.assign,C.loc,[x]);"
            "globalThis.hashOld=x=>Reflect.apply(C.setHash,C.loc,[x]);"
            "denied(()=>B.getHref.call(C.loc));assert(Reflect.apply(B.getHref,C.loc,[])==='https://authority-b.test/document');"
            "assert(Reflect.apply(C.getHref,A.loc,[])===A.loc.href);"
            "denied(()=>Reflect.apply(C.getHref,B.loc,[]));"
            "let e;try{Reflect.apply(C.getHref,Object.create(C.loc),[])}catch(x){e=x}assert(e&&e.name==='TypeError');"
            "e=null;try{Reflect.apply(C.getHref,new Proxy(C.loc,{}),[])}catch(x){e=x}assert(e&&e.name==='TypeError');",
            "Location: borrowed members select branded receiver and reject lookalikes");
        realm_script(ce_realms[1],"denied(()=>A.readOld());let conversions=0;"
            "denied(()=>A.assignOld({toString(){conversions++;return 'next'}}));assert(conversions===1);"
            "denied(()=>A.hashOld({toString(){conversions++;return 'next'}}));assert(conversions===2)",
            "Location: entry origin is independent of pre-WebIDL caller; conversion precedes algorithm check");
        os64_dom_child_t same=location_candidate(&f,0,"https://url-only.test/new?query#fragment");
        check(same.binding&&d_browsing_context_replace(c[2],1,same.binding,&out)==OS64_JS_OK,
            "Location: same-origin replacement committed");
        check(os64_dom_depart_finish(f.bindings[2],&out)==OS64_JS_OK,"Location: old providers detached");
        JSValue global=JS_GetGlobalObject(ce_contexts[0]);
        check(JS_SetPropertyStr(ce_contexts[0],global,"newLoc",JS_DupValue(ce_contexts[0],same.binding->location->value))>=0,
            "Location: host exports new document Location for identity comparison");
        JS_FreeValue(ce_contexts[0],global);
        size_t charge=f.cluster->bytes;
        realm_script(ce_realms[0],
            "assert(oldLoc===C.loc&&oldLoc!==A.loc&&oldLoc!==newLoc);assert(readOld()==='https://url-only.test/new?query#fragment');"
            "assert(String(oldLoc)===readOld());assert(oldLoc.protocol==='https:'&&oldLoc.hostname==='url-only.test');"
            "assert(oldLoc.pathname==='/new'&&oldLoc.search==='?query'&&oldLoc.hash==='#fragment');"
            "assert(oldLoc.origin==='https://url-only.test');",
            "Location: retained identity reads the current document after provider detachment");
        check(f.cluster->bytes==charge,"Location: lookup adds no retained native allocation");
        realm_script(ce_realms[0],"denied(()=>C.assign.call(oldLoc,'next'));Reflect.apply(C.assign,oldLoc,['next'])","Location: old assign targets replacement");
        check(d_navigation_dispatch(c[2],navigation_consume,&n,&out)&&n.seen.target==same.binding&&
            n.seen.source==f.bindings[0]&&n.seen.entry==f.bindings[0]&&n.seen.target_generation==2&&
            !strcmp(n.seen.url,"https://base-a.test/folder/next"),"Location: new target, original source and entry base");
        realm_script(ce_realms[0],"Reflect.apply(C.setHash,oldLoc,['new hash'])","Location: retained hash setter");
        check(d_navigation_dispatch(c[2],navigation_consume,&n,&out)&&
            !strcmp(n.seen.url,"https://url-only.test/new?query#new%20hash"),"Location: hash uses current target URL");
        realm_script(ce_realms[0],"oldLoc.reload()","Location: retained reload");
        check(d_navigation_dispatch(c[2],navigation_consume,&n,&out)&&n.seen.source==same.binding&&
            n.seen.target==same.binding&&n.seen.kind==OS64_DOM_NAVIGATE_RELOAD,"Location: reload current document source");
        os64_dom_child_t cross=location_candidate(&f,1,"https://other-url.test/private");
        check(cross.binding&&d_browsing_context_replace(c[2],2,cross.binding,&out)==OS64_JS_OK,
            "Location: cross-origin replacement committed");
        realm_script(ce_realms[0],"denied(()=>oldLoc.href);denied(()=>oldLoc.origin);denied(()=>String(oldLoc));"
            "denied(()=>oldLoc.reload());denied(()=>oldLoc.assign('next'));denied(()=>oldLoc.hash='x')",
            "Location: matching original global does not authorize current document");
        realm_script(ce_realms[1],"denied(()=>C.loc.href);assert(A.readOld()==='https://other-url.test/private')",
            "Location: original-global caller and active-document entry checks stay distinct");
        unsigned calls=n.snapshots;
        realm_script(ce_realms[1],"let converted=0;denied(()=>C.loc.assign({toString(){converted++;return 'x'}}));assert(!converted)",
            "Location: IDL rejection precedes conversion even when entry matches new document");
        check(n.snapshots==calls,"Location: denied access never calls navigation provider");
        realm_script(ce_realms[0],"oldLoc.href='written';Reflect.apply(C.replace,oldLoc,['replaced'])",
            "Location: cross-origin-exempt members route through current target policy");
        check(d_navigation_dispatch(c[2],navigation_consume,&n,&out)&&n.seen.target==cross.binding&&
            n.seen.target_generation==3&&!strcmp(n.seen.url,"https://base-a.test/folder/replaced")&&
            n.seen.kind==OS64_DOM_NAVIGATE_REPLACE,"Location: replacement request records new generation");
        os64_dom_child_t back=location_candidate(&f,0,"https://back.test/again");
        check(back.binding&&d_browsing_context_replace(c[2],3,back.binding,&out)==OS64_JS_OK,
            "Location: return to original origin");
        realm_script(ce_realms[0],"assert(readOld()==='https://back.test/again');oldLoc.href='stale'",
            "Location: same-cross-same preserves old identity and current data");
        check(d_browsing_context_discard(c[2],4,&out)==OS64_JS_OK,"Location: discard context");
        check(!d_navigation_dispatch(c[2],navigation_consume,&n,&out),"Location: discard invalidates pending navigation");
        realm_script(ce_realms[1],"void C.loc.replace;Object.getOwnPropertyDescriptor(C.loc,'href')",
            "Location: prepare cross-origin descriptors before measuring tombstone calls");
        calls=n.snapshots;charge=f.cluster->bytes;
        realm_script(ce_realms[0],
            "assert(oldLoc.href==='about:blank'&&String(oldLoc)==='about:blank');"
            "assert(oldLoc.protocol==='about:'&&oldLoc.pathname==='blank'&&oldLoc.origin==='null');"
            "assert(oldLoc.host===''&&oldLoc.hostname===''&&oldLoc.port===''&&oldLoc.search===''&&oldLoc.hash==='');"
            "let conversions=0;let value={toString(){conversions++;return 'next'}};"
            "oldLoc.href=value;oldLoc.assign(value);oldLoc.replace(value);oldLoc.hash=value;oldLoc.reload();assert(conversions===4);"
            "let thrown={},caught;try{oldLoc.replace({toString(){throw thrown}})}catch(e){caught=e}assert(caught===thrown)",
            "Location: tombstone blank URL, conversion and no-op navigation");
        realm_script(ce_realms[1],"denied(()=>C.loc.href);C.loc.replace('ignored');C.loc.href='ignored'",
            "Location: discard retains original-global IDL authority");
        check(n.snapshots==calls&&f.cluster->bytes==charge&&!d_navigation_dispatch(c[2],navigation_consume,&n,&out),
            "Location: tombstone never allocates requests or touches policy");
        dom_fail_at=dom_attempts+1;
        realm_script(ce_realms[0],"let allocationError;try{oldLoc.replace('allocation probe')}catch(e){allocationError=e}"
            "assert(allocationError&&allocationError.name==='QuotaExceededError')",
            "Location: tombstone conversion allocation refusal");
        dom_fail_at=0;
        check(n.snapshots==calls&&f.cluster->bytes==charge,
            "Location: refused conversion preserves charge and never enters policy");
        ce_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;DBrowsingContext *c[5];
        if(!navigation_open(&f,policy,c)){ce_close(&f);continue;}
        realm_script(ce_realms[0],"retireC();denied(()=>C.loc.href);denied(()=>C.loc.replace('next'))",
            "Location: unpublished departure is unavailable, not a discarded blank document");
        ce_close(&f);
    }
}
