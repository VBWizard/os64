/* Real lazy inline handlers supply null provenance; baseURI and import use
 * the same document-base algorithm, independently of the queue's later work. */
static char dom_import_base[256];
static unsigned dom_import_realm,dom_import_loads;
static char *dom_import_resolve(void *opaque,const char *base,const char *specifier)
{
    dom_import_realm=(unsigned)(uintptr_t)opaque;
    snprintf(dom_import_base,sizeof(dom_import_base),"%s",base);
    size_t n=strlen(specifier)+1;char *result=os64_malloc(n);if(result)memcpy(result,specifier,n);
    return result;
}
static bool dom_import_load(void *opaque,const char *name,const char **bytes,size_t *length)
{
    (void)name;check(dom_import_realm==(unsigned)(uintptr_t)opaque,"import DOM: resolver and loader agree");
    dom_import_loads++;static const char source[]="export const value=await Promise.resolve(42)";
    *bytes=source;*length=sizeof(source)-1;return true;
}
static JSValue dom_import_arm(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv)
{
    (void)ctx;(void)self;(void)argc;(void)argv;
    dom_fail_at=dom_attempts+1;return JS_UNDEFINED;
}
static void dom_import_settings_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned mode=0;mode<7;mode++) {
        CeFixture f;if(!ce_open(&f,policy,4)){ce_close(&f);continue;}
        dom_import_base[0]=0;dom_import_loads=0;dom_import_realm=99;os64_js_outcome_t out;
        for(unsigned i=0;i<5;i++) {
            os64_js_module_loader_t loader={(void *)(uintptr_t)i,dom_import_resolve,dom_import_load};
            check(os64_js_realm_set_module_loader(ce_realms[i],OS64_JS_ABI_ID,&loader,&out)==OS64_JS_OK,
                  "import DOM: distinct realm loaders configured");
        }
        JSValue global=JS_GetGlobalObject(ce_contexts[2]);
        check(JS_SetPropertyStr(ce_contexts[2],global,"armImport",
              JS_NewCFunction(ce_contexts[2],dom_import_arm,"armImport",0))>=0,"import DOM: native refusal seam installed");
        JS_FreeValue(ce_contexts[2],global);
        realm_script(ce_realms[2],
            "globalThis.base=document.createElement('base');base.setAttribute('href','https://c.example/initial/');"
            "document.head.appendChild(base);globalThis.node=document.getElementById('one');",
            "import DOM: live base and target prepared");
        const char *bodies[]={
            "globalThis.promise=import('./module.js');base.setAttribute('href','https://c.example/late/');",
            "globalThis.promise=import({toString(){base.setAttribute('href','https://c.example/converted/');return './module.js'}});base.setAttribute('href','https://c.example/late/');",
            "globalThis.promise=import('./module.js',{get with(){base.setAttribute('href','https://c.example/converted/');return undefined}});base.setAttribute('href','https://c.example/late/');",
            "globalThis.promise=(0,eval)(\"import('./module.js')\");base.setAttribute('href','https://c.example/late/');",
            "globalThis.promise=import('./module.js');base.setAttribute('href','https://c.example/late/');",
            "globalThis.promise=import({toString(){base.remove();return './module.js'}});",
            "armImport();globalThis.promise=import('./module.js');promise.catch(e=>{globalThis.refused=e.name});"
        };
        char source[1024];
        /* Quote through JSON.stringify so the handler source reaches the real
         * attribute parser unchanged, including its nested eval string. */
        JSContext *ctx=ce_contexts[2];JSValue body=JS_NewString(ctx,bodies[mode]);
        JSValue quoted=JS_JSONStringify(ctx,body,JS_UNDEFINED,JS_UNDEFINED);const char *text=JS_ToCString(ctx,quoted);
        snprintf(source,sizeof(source),"node.setAttribute('onclick',%s)",text?text:"null");
        JS_FreeCString(ctx,text);JS_FreeValue(ctx,quoted);JS_FreeValue(ctx,body);
        realm_script(ce_realms[2],source,"import DOM: lazy inline source installed");
        if(mode==4) {
            realm_script(ce_realms[0],"C.node.onclick()","https://A.example/source.js");
        } else {
            os64_dom_event_t event={.type="click",.bubbles=true,.cancelable=true};bool prevented=false;
            check(os64_dom_dispatch(f.bindings[2],wa_by_id(f.documents[2]->document,"one"),&event,&prevented,&out)==OS64_JS_OK,
                  "import DOM: native event runs inline handler and queued import");
        }
        dom_fail_at=0;
        if(mode==6) {
            check(!dom_import_loads&&dom_import_realm==99,"import DOM: base allocation refusal never reaches loader");
            realm_script(ce_realms[2],"assert(refused==='QuotaExceededError');assert(document.baseURI==='https://c.example/initial/');",
                         "import DOM: failed base snapshot rejects and leaves document usable");
            ce_close(&f);continue;
        }
        const char *expected=mode==4?"https://A.example/source.js":mode==5?"https://custom-settings.example/":
            (mode==1||mode==2)?"https://c.example/converted/":"https://c.example/initial/";
        check(dom_import_loads==1&&dom_import_realm==(mode==4?0u:2u),"import DOM: source record or current document selects loader");
        check(!strcmp(dom_import_base,expected),"import DOM: live base snapshot precedes later mutation");
        realm_script(ce_realms[2],"assert(promise instanceof Promise);promise.then(ns=>{globalThis.answer=ns.value});",
                     "import DOM: executing document owns import Promise");
        realm_script(ce_realms[2],"assert(answer===42);assert(document.baseURI===(base.parentNode?'https://c.example/late/':'https://custom-settings.example/'));",
                     "import DOM: awaited module settles after base changes");
        check(os64_js_realm_retire(ce_realms[2],OS64_JS_ABI_ID,&out)==OS64_JS_OK&&
              os64_dom_depart_finish(f.bindings[2],&out)==OS64_JS_OK,
              "import DOM: departure detaches provider at quiescence");
        ce_close(&f);
    }
}
