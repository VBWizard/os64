/* Real Location members with host-selected origins deliberately unrelated to URL. */
#include "../userland/libdom/binding_authority.h"

static bool authority_bindings_open(CeFixture *f, int policy, unsigned mode)
{
    *f=(CeFixture){0};
    os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
    os64_js_outcome_t out;
    if(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&ce_runtime,&out)!=OS64_JS_OK)return false;
    os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
    f->cluster=os64_dom_cluster_create_with_browser_jobs(ce_runtime,&limits,&out);
    if(!f->cluster)return false;
    way_origin_t origins[5];
    way_origin_of("https://authority-a.test/",0,&origins[0]);
    way_origin_of("https://authority-b.test/",0,&origins[1]);
    origins[2]=origins[0];
    check(d_cluster_opaque_origin(f->cluster,&origins[3]),"authority: issue opaque identity");
    origins[4]=origins[3];
    if(mode==1)check(d_cluster_opaque_origin(f->cluster,&origins[4]),"authority: distinct opaque identity");
    ce_realms[0]=os64_js_primary_realm(ce_runtime,OS64_JS_ABI_ID,&out);
    for(unsigned i=0;i<5;i++) {
        if(i && os64_js_realm_create(ce_runtime,OS64_JS_ABI_ID,&ce_realms[i],&out)!=OS64_JS_OK)return false;
        ce_contexts[i]=os64_js_realm_context(ce_realms[i],OS64_JS_ABI_ID,&out);
        DomFixture source=fixture_configured(0,0,&config);os64_js_destroy(source.runtime);
        os64_dom_options_t options=p2b_options("https://authority-b.test/document");
        options.shadow_trees = mode == 3;
        os64_dom_admission_t a=mode==2&&i==4
            ?os64_dom_create_in_realm(f->cluster,ce_realms[i],source.document,source.state,&options,&out)
            :d_create_in_realm_with_origin(f->cluster,ce_realms[i],source.document,source.state,&options,&origins[i],&out);
        f->bindings[i]=a.binding;f->documents[i]=source.document;
        if(!a.consumed){os64_page_state_free(source.state);os64_html_document_free(source.document);}
        if(!a.binding)return false;
        memset(&origins[i],0,sizeof(origins[i])); /* Published binding must own its copy. */
    }
    return true;
}

static bool authority_open(CeFixture *f, int policy, unsigned mode)
{
    if (!authority_bindings_open(f, policy, mode)) return false;
    for(unsigned i=0;i<5;i++) {
        JSContext *ctx=ce_contexts[i];JSValue global=JS_GetGlobalObject(ctx);
        for(unsigned j=0;j<5;j++) {
            char name[]={(char)('A'+j),0},retire[]="retireA";retire[6]+=(char)j;
            check(JS_SetPropertyStr(ctx,global,name,JS_GetGlobalObject(ce_contexts[j]))>=0,"authority: trusted exchange");
            check(JS_SetPropertyStr(ctx,global,retire,JS_NewCFunctionMagic(ctx,ce_retire,retire,0,JS_CFUNC_generic_magic,(int)j))>=0,"authority: retirement seam");
        }
        JS_FreeValue(ctx,global);
        realm_script(ce_realms[i],STORAGE_ASSERT
            "function denied(f){let e;try{f()}catch(x){e=x}assert(e&&e.name==='SecurityError')}"
            "globalThis.loc=location;globalThis.getHref=Object.getOwnPropertyDescriptor(loc,'href').get;"
            "globalThis.setHash=Object.getOwnPropertyDescriptor(loc,'hash').set;"
            "globalThis.assign=loc.assign;globalThis.replace=loc.replace;",
            "authority: save actual members before cross-realm calls");
    }
    return true;
}

static void authority_admission_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned cut=0;cut<4;cut++) {
        os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
        os64_js_outcome_t out;os64_js_runtime_t *runtime=NULL;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "authority admission: runtime");
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_dom_cluster_t *cluster=os64_dom_cluster_create_with_browser_jobs(runtime,&limits,&out);
        check(cluster!=NULL,"authority admission: cluster");
        os64_js_realm_t *realm=os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out);
        DomFixture source=fixture_configured(0,0,&config);os64_js_destroy(source.runtime);
        os64_dom_options_t options=p2b_options("about:blank");way_origin_t valid;
        way_origin_of("https://authority.test/",0,&valid);
        size_t before=os64_dom_cluster_bytes(cluster).dom_bytes;
        if(!cut)for(unsigned bad=0;bad<5;bad++) {
            way_origin_t invalid=valid;
            if(bad==0)invalid=(way_origin_t){.opaque=true};
            if(bad==1)invalid=(way_origin_t){.opaque=true,.id=1};
            if(bad==2)memset(invalid.host,'x',sizeof(invalid.host));
            if(bad==3)invalid.scheme[0]='H';
            if(bad==4)invalid.id=1;
            os64_dom_admission_t a=d_create_in_realm_with_origin(cluster,realm,source.document,source.state,&options,&invalid,&out);
            check(!a.binding&&!a.consumed&&out.status==OS64_JS_BAD_ARGUMENT&&
                  os64_dom_cluster_bytes(cluster).dom_bytes==before,
                  "authority admission: invalid origin preserves ownership and charge");
        }
        dom_fail_at=cut?dom_attempts+cut:0;
        os64_dom_admission_t a=d_create_in_realm_with_origin(cluster,realm,source.document,source.state,&options,&valid,&out);
        dom_fail_at=0;
        if(cut<3&&cut) {
            check(!a.binding&&!a.consumed&&out.status==OS64_JS_HOST_FAILURE&&
                  os64_dom_cluster_bytes(cluster).dom_bytes==before,
                  "authority admission: shell/origin allocation refusal preserves ownership and charge");
            a=d_create_in_realm_with_origin(cluster,realm,source.document,source.state,&options,&valid,&out);
        }
        check(cut==3?(!a.binding&&a.consumed):(a.binding&&a.consumed),
              "authority admission: retry succeeds or post-transfer failure keeps grouped ownership");
        if(!a.consumed){os64_page_state_free(source.state);os64_html_document_free(source.document);}
        os64_dom_cluster_drain(cluster);os64_js_destroy(runtime);os64_dom_cluster_free(cluster);
        check(live==0&&dom_live==0&&!dom_allocations,"authority admission: allocation boundaries balance storage");
    }
}

static void dom_binding_authority_cases(void)
{
    authority_admission_cases();
    uint64_t serial=UINT64_MAX;way_origin_t token={.id=81};
    check(!d_next_opaque_origin(&serial,&token)&&serial==UINT64_MAX&&token.id==81,
          "authority: issuer exhaustion preserves counter and output");
    check(!d_next_opaque_origin(NULL,&token)&&!d_next_opaque_origin(&serial,NULL),
          "authority: issuer rejects missing storage");
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<3;mode++) {
        CeFixture f;if(!authority_open(&f,policy,mode)){check(false,"authority: binding fixture");ce_close(&f);continue;}
        realm_script(ce_realms[0],
            "assert(C.loc.href==='https://authority-b.test/document');"
            "denied(()=>B.loc.href);denied(()=>Reflect.apply(B.getHref,B.loc,[]));"
            "let n=0;const value={toString(){n++;return 'https://destination.test/'}};"
            "denied(()=>Reflect.apply(B.assign,B.loc,[value]));"
            "denied(()=>Reflect.apply(B.setHash,B.loc,[value]));assert(n===0);"
            "assert(Reflect.apply(C.getHref,C.loc,[])==='https://authority-b.test/document');"
            "let brand;try{Reflect.apply(B.assign,{},[value])}catch(e){brand=e.name}assert(brand==='TypeError'&&n===0);",
            "authority: independent origin, borrowed members and refusal before coercion");
        realm_script(ce_realms[1],
            "globalThis.readC=()=>C.loc.href;globalThis.proxyC=new Proxy(C.getHref,{apply(t,s,a){return Reflect.apply(t,s,a)}});",
            "authority: foreign script and proxy trap prepared");
        realm_script(ce_realms[0],
            "denied(B.readC);denied(()=>Reflect.apply(B.proxyC,C.loc,[]));"
            "const bound=B.getHref.bind(B.loc), proxy=new Proxy(B.getHref,{});"
            "denied(bound);denied(()=>Reflect.apply(proxy,B.loc,[]));"
            "denied(()=>B.Reflect.apply(C.getHref,C.loc,[]));"
            "assert(Reflect.apply(C.getHref,C.loc,[])==='https://authority-b.test/document');",
            "authority: current pre-member realm is independent of task entry, target and proxy ownership");
        realm_script(ce_realms[0],
            "B.loc.href='https://destination.test/href';Reflect.apply(B.replace,B.loc,['https://destination.test/replace']);",
            "authority: href setter and replace pass the IDL cross-origin exception");
        os64_dom_navigation_t ask;
        check(os64_dom_take_navigation(f.bindings[1],&ask)&&ask.kind==OS64_DOM_NAVIGATE_REPLACE&&
              !strcmp(ask.url,"https://destination.test/replace"),"authority: IDL exception reaches existing navigation ask");
        realm_script(ce_realms[0],
            "const thrown={};let caught;try{B.loc.href={toString(){throw thrown}}}catch(e){caught=e}assert(caught===thrown);",
            "authority: permitted conversion preserves thrown identity");
        check(!os64_dom_take_navigation(f.bindings[1],&ask),"authority: throwing conversion publishes no navigation");
        if(mode==0)realm_script(ce_realms[3],"assert(E.loc.href===loc.href);denied(()=>A.loc.href)",
            "authority: inherited opaque identity compares equal and differs from tuples");
        if(mode==1)realm_script(ce_realms[3],"denied(()=>E.loc.href)","authority: distinct opaque identities remain unequal");
        if(mode==2)realm_script(ce_realms[4],"denied(()=>A.loc.href);denied(()=>{A.loc.href='https://destination.test/'})",
            "authority: legacy source cannot invent origin-bearing settings");
        /* A raw native call has no prepared caller even when its ctx is target C. */
        JSValue global=JS_GetGlobalObject(ce_contexts[2]);JSValue loc=JS_GetPropertyStr(ce_contexts[2],global,"loc");
        JSValue getter=JS_GetPropertyStr(ce_contexts[2],global,"getHref");
        JSValue result=JS_Call(ce_contexts[2],getter,loc,0,NULL);
        check(JS_IsException(result),"authority: missing host settings refuse before member read");
        if(JS_IsException(result)){JSValue e=JS_GetException(ce_contexts[2]);JS_FreeValue(ce_contexts[2],e);}
        JS_FreeValue(ce_contexts[2],result);JS_FreeValue(ce_contexts[2],getter);JS_FreeValue(ce_contexts[2],loc);JS_FreeValue(ce_contexts[2],global);
        realm_script(ce_realms[0],"retireC();denied(()=>Reflect.apply(C.getHref,C.loc,[]))",
            "authority: retained member refuses departed target");
        realm_script(ce_realms[0],"retireA();denied(()=>Reflect.apply(B.replace,B.loc,['https://destination.test/']))",
            "authority: retired caller cannot use a cross-origin-exempt method");
        ce_close(&f);
    }
}
