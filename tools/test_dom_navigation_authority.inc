typedef struct {
    CeFixture *fixture;
    bool permit, activation, loaded;
    int retire_snapshot, retire_allow, retire_dispatch;
    unsigned snapshots, authorizations, deliveries;
    DNavigationRequest seen;
} NavigationFixture;
static bool navigation_snapshot(void *opaque,os64_dom_t *source,os64_dom_t *target,DNavigationSnapshot *out)
{
    NavigationFixture *f=opaque;f->snapshots++;
    check(source&&target,"navigation: policy receives explicit source and target");
    *out=(DNavigationSnapshot){f->activation,f->loaded,UINT64_C(0x1234)};
    if(f->retire_snapshot>=0){os64_js_outcome_t result;os64_dom_depart(f->fixture->bindings[f->retire_snapshot],&result);}
    return true;
}
static bool navigation_allow(void *opaque,const DNavigationRequest *request,DNavigationPhase phase)
{
    NavigationFixture *f=opaque;f->authorizations++;f->seen=*request;
    os64_js_outcome_t result;
    check(d_navigation_policy(f->fixture->cluster,NULL,&result)==OS64_JS_BUSY,
          "navigation: policy loan prevents reentrant revocation");
    int retire=phase==D_NAV_ENQUEUE?f->retire_allow:f->retire_dispatch;
    if(retire>=0)os64_dom_depart(f->fixture->bindings[retire],&result);
    return f->permit;
}
static void navigation_consume(void *opaque,const DNavigationRequest *request)
{
    NavigationFixture *f=opaque;f->deliveries++;f->seen=*request;
    os64_js_outcome_t out;
    check(d_browsing_context_discard(request->target_context,request->target_generation,&out)==OS64_JS_BUSY,
          "navigation: delivery loan excludes target replacement/discard");
}
static JSValue navigation_scoped_body(JSContext *ctx,void *opaque)
{
    (void)opaque;
    JSValue global=JS_GetGlobalObject(ce_contexts[1]);
    JSValue loc=JS_GetPropertyStr(ctx,global,"loc"),fn=JS_GetPropertyStr(ctx,global,"replace");
    JSValue text=JS_NewString(ctx,"next");
    JSValue result=JS_Call(ctx,fn,loc,1,&text);
    JS_FreeValue(ctx,text);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,loc);JS_FreeValue(ctx,global);
    return result;
}
static bool navigation_open(CeFixture *f,int policy,DBrowsingContext **contexts)
{
    if(!authority_open(f,policy,0))return false;
    os64_js_outcome_t out;
    for(unsigned i=0;i<5;i++) {
        char name[]={(char)('A'+i),0};
        contexts[i]=d_browsing_context_create(f->cluster,NULL,f->bindings[i],name,&out);
        check(contexts[i]!=NULL,"navigation: context admitted");
        char source[256];snprintf(source,sizeof(source),
            "globalThis.base=document.createElement('base');base.href='https://base-%c.test/folder/';document.head.appendChild(base)",'a'+i);
        realm_script(ce_realms[i],source,"navigation: independent document bases");
    }
    return true;
}
static void dom_navigation_authority_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;DBrowsingContext *c[5];
        if(!navigation_open(&f,policy,c)){ce_close(&f);continue;}
        NavigationFixture n={.fixture=&f,.permit=true,.loaded=true,.retire_snapshot=-1,.retire_allow=-1,.retire_dispatch=-1};
        DNavigationPolicy host={&n,navigation_snapshot,navigation_allow};os64_js_outcome_t out;
        realm_script(ce_realms[0],"let conversions=0;denied(()=>B.loc.href={toString(){conversions++;return 'next'}});assert(conversions===0)",
            "navigation: absent policy refuses before conversion");
        size_t before=f.cluster->bytes;
        dom_fail_at=dom_attempts+1;
        check(d_navigation_policy(f.cluster,&host,&out)==OS64_JS_ADMISSION_REFUSED&&f.cluster->bytes==before,
            "navigation: policy allocation refusal preserves charge");dom_fail_at=0;
        check(d_navigation_policy(f.cluster,&host,&out)==OS64_JS_OK,"navigation: host policy installed");
        realm_script(ce_realms[0],"B.loc.href={toString(){base.href='https://converted.test/base/';return 'next'}}",
            "navigation: conversion updates entry base before resolution");
        check(n.seen.source==f.bindings[0]&&n.seen.entry==f.bindings[0]&&n.seen.target==f.bindings[1]&&
              !strcmp(n.seen.url,"https://converted.test/base/next")&&n.seen.snapshot.restrictions==0x1234,
              "navigation: source/base/target and snapshot recorded independently");
        os64_dom_navigation_t old;
        d_navigate(f.bindings[1],OS64_DOM_NAVIGATE_URL,NULL,NULL,"https://unattributed.test/",0);
        check(!os64_dom_take_navigation(f.bindings[1],&old),"navigation: attributed request cannot escape legacy consumer");
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.deliveries==1,
              "navigation: admitted request dispatches once");
        check(!d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.deliveries==1,
              "navigation: successful request is consumed");
        check(os64_js_realm_task_begin(ce_realms[0],OS64_JS_ABI_ID,"navigation roles",&out)==OS64_JS_OK,
              "navigation: outer accounting task A");
        JSValue result=JS_UNDEFINED;
        check(os64_js_realm_with_settings(ce_realms[2],ce_realms[0],OS64_JS_ABI_ID,
              navigation_scoped_body,NULL,&result,&out)==OS64_JS_OK&&!JS_IsException(result),
              "navigation: independent checked entry C and incumbent A");
        JS_FreeValue(ce_contexts[2],result);
        check(os64_js_task_end(ce_runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"navigation: role task completes");
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.seen.entry==f.bindings[2]&&
              n.seen.source==f.bindings[0]&&n.seen.target==f.bindings[1]&&
              !strcmp(n.seen.url,"https://base-c.test/folder/next")&&n.seen.kind==OS64_DOM_NAVIGATE_REPLACE,
              "navigation: entry controls URL base while incumbent controls source");
        n.loaded=false;n.activation=false;
        realm_script(ce_realms[0],"B.loc.href='https://destination.test/loading'","navigation: unloaded target without activation");
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.seen.kind==OS64_DOM_NAVIGATE_REPLACE,
              "navigation: loading without activation selects replacement history");
        n.activation=true;
        realm_script(ce_realms[0],"B.loc.href='https://destination.test/active'","navigation: trusted activation snapshot");
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.seen.kind==OS64_DOM_NAVIGATE_URL&&n.seen.snapshot.transient_activation,
              "navigation: activation preserves automatic history behavior");
        n.loaded=true;
        realm_script(ce_realms[0],"C.loc.hash='hello world'","navigation: hash uses target URL");
        check(d_navigation_dispatch(c[2],navigation_consume,&n,&out)&&
              !strcmp(n.seen.url,"https://authority-b.test/document#hello%20world"),
              "navigation: hash is queued with encoded target fragment");
        realm_script(ce_realms[0],"C.loc.reload()","navigation: reload routed through policy");
        check(d_navigation_dispatch(c[2],navigation_consume,&n,&out)&&n.seen.source==f.bindings[2]&&
              n.seen.kind==OS64_DOM_NAVIGATE_RELOAD,"navigation: reload source is its relevant document");
        realm_script(ce_realms[0],"B.loc.href='https://destination.test/kept'","navigation: preserve admitted predecessor");
        n.permit=false;
        realm_script(ce_realms[0],"denied(()=>B.loc.href='https://destination.test/denied')","navigation: denied replacement refuses");
        n.permit=true;
        check(d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&!strcmp(n.seen.url,"https://destination.test/kept"),
              "navigation: denial leaves previous admitted ask intact");
        realm_script(ce_realms[0],"B.loc.href='https://destination.test/revoked'","navigation: pending before revocation");
        check(d_navigation_policy(f.cluster,NULL,&out)==OS64_JS_OK&&
              !d_navigation_dispatch(c[1],navigation_consume,&n,&out),"navigation: policy revocation invalidates queued request");
        check(d_navigation_policy(f.cluster,&host,&out)==OS64_JS_OK,"navigation: policy restored with new generation");
        realm_script(ce_realms[0],"B.loc.href='https://destination.test/stale'","navigation: pending before target replacement");
        os64_dom_child_t child=context_candidate_prepare(&f,0,&out);
        check(child.state==OS64_DOM_CHILD_ACTIVE&&d_browsing_context_replace(c[1],1,child.binding,&out)==OS64_JS_OK,
              "navigation: replace target after enqueue");
        unsigned deliveries=n.deliveries;
        check(!d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&n.deliveries==deliveries,
              "navigation: old generation never dispatches to replacement");
        size_t charge=f.cluster->bytes;
        realm_script(ce_realms[0],"E.loc.href='https://destination.test/one'","navigation: first slot on independent target");
        check(f.cluster->bytes==charge+sizeof(DNavigationRequest),"navigation: slot is charged to aggregate budget");
        charge=f.cluster->bytes;
        realm_script(ce_realms[0],"E.loc.href='https://destination.test/two';E.loc.href='https://destination.test/three'",
              "navigation: bounded slot reuse");
        check(f.cluster->bytes==charge&&d_navigation_dispatch(c[4],navigation_consume,&n,&out)&&
              !strcmp(n.seen.url,"https://destination.test/three"),"navigation: bounded last-successful request wins");
        ce_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<9;mode++) {
        CeFixture f;DBrowsingContext *c[5];if(!navigation_open(&f,policy,c)){ce_close(&f);continue;}
        NavigationFixture n={.fixture=&f,.permit=true,.loaded=true,.retire_snapshot=-1,.retire_allow=-1,.retire_dispatch=-1};
        DNavigationPolicy host={&n,navigation_snapshot,navigation_allow};os64_js_outcome_t out;
        check(d_navigation_policy(f.cluster,&host,&out)==OS64_JS_OK,"navigation: reentry fixture policy");
        if(mode==0)realm_script(ce_realms[0],"denied(()=>B.loc.href={toString(){retireA();return 'next'}})",
              "navigation: source retirement during conversion refuses");
        if(mode==1)realm_script(ce_realms[0],"denied(()=>B.loc.href={toString(){retireB();return 'next'}})",
              "navigation: target retirement during conversion refuses");
        if(mode==2){n.retire_snapshot=1;realm_script(ce_realms[0],"denied(()=>B.loc.href='next')","navigation: snapshot retirement rechecked");}
        if(mode==3){n.retire_allow=0;realm_script(ce_realms[0],"denied(()=>B.loc.href='next')","navigation: authorization retirement rechecked");}
        if(mode==7) {
            size_t before=f.cluster->bytes;dom_fail_at=dom_attempts+2;
            realm_script(ce_realms[0],"let e;try{B.loc.href='next'}catch(x){e=x}assert(e&&e.name==='QuotaExceededError')",
                "navigation: request allocation refusal");dom_fail_at=0;
            check(!c[1]->navigation&&f.cluster->bytes==before,"navigation: failed request leaves no slot or charge");
        }
        if(mode==8) {
            size_t cap=f.cluster->limits.dom_bytes;f.cluster->limits.dom_bytes=f.cluster->bytes+sizeof(DNavigationRequest)-1;
            realm_script(ce_realms[0],"let e;try{B.loc.href='next'}catch(x){e=x}assert(e&&e.name==='QuotaExceededError')",
                "navigation: request aggregate cap refusal");
            check(!c[1]->navigation,"navigation: cap refusal leaves no request");f.cluster->limits.dom_bytes=cap;
        }
        if(mode>=4&&mode<=6) {
            realm_script(ce_realms[0],"B.loc.href='next'","navigation: queue for dispatch revalidation");
            if(mode==4)n.retire_dispatch=1;
            if(mode==5)n.permit=false;
            if(mode==6)check(d_browsing_context_discard(c[0],1,&out)==OS64_JS_OK,"navigation: source discarded before dispatch");
        }
        if(mode<=1)check(!n.snapshots&&!n.authorizations,"navigation: conversion retirement never calls a detached policy provider");
        if(mode==2)check(!n.authorizations,"navigation: snapshot retirement prevents authorization callback");
        unsigned authorizations=n.authorizations;
        check(!d_navigation_dispatch(c[1],navigation_consume,&n,&out)&&!n.deliveries,
              "navigation: no stale or unauthorized request reaches consumer");
        if(mode==6)check(n.authorizations==authorizations,"navigation: discarded source refuses before dispatch policy");
        ce_close(&f);
    }
}
