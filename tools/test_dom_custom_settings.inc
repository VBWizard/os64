/* Registry D and activity E are independent of capturing A, associated B
 * and executable C. Native constructors expose the backup incumbent that a
 * script-bearing class constructor would intentionally override. */
static os64_js_runtime_t *ce_runtime;
static os64_js_realm_t *ce_realms[5];
static JSContext *ce_contexts[5];
static unsigned ce_entry, ce_incumbent, ce_constructors, ce_script_calls, ce_calls[4];
static unsigned ce_order[64], ce_order_count;
static bool ce_nested, ce_throw, ce_engine_cut, ce_bound;
static size_t ce_cut, ce_started;

static void ce_settings_check(JSContext *ctx, bool data_callback)
{
    os64_js_outcome_t out;
    check(os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==ce_realms[ce_entry],
          "custom settings: associated entry independent of registry");
    check(os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==ce_realms[ce_incumbent],
          "custom settings: captured incumbent independent of activity");
    check(os64_js_settings_realm(ce_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==ce_realms[2],
          "custom settings: native executable current is C");
    /* Generic C constructors receive their defining context; CFunctionData
     * callbacks receive the incoming context. Neither fact chooses settings. */
    check(ctx==ce_contexts[data_callback?ce_entry:2],"custom settings: native ABI context remains independent of settings");
}

static JSValue ce_inspect(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;(void)argc;(void)argv;ce_settings_check(ctx,false);ce_script_calls++;
    return JS_UNDEFINED;
}

static JSValue ce_method(JSContext *ctx, JSValueConst self, const char *name, int argc, JSValueConst *argv)
{
    JSValue method=JS_GetPropertyStr(ctx,self,name);
    JSValue result=JS_IsException(method)?JS_EXCEPTION:JS_Call(ctx,method,self,argc,argv);
    JS_FreeValue(ctx,method);return result;
}

static JSValue ce_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv)
{
    (void)argc;(void)argv;ce_settings_check(ctx,false);ce_constructors++;
    if(ce_order_count<64)ce_order[ce_order_count++]=4;
    JSValue global=JS_GetGlobalObject(ce_contexts[3]);
    JSValue base=JS_GetPropertyStr(ctx,global,"HTMLElement");JS_FreeValue(ctx,global);
    JSValue result=JS_CallConstructor2(ctx,base,new_target,0,NULL);JS_FreeValue(ctx,base);
    return result;
}

static JSValue ce_callback(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                           int magic, JSValue *data)
{
    (void)data;ce_settings_check(ctx,true);ce_calls[magic]++;
    if(ce_order_count<64)ce_order[ce_order_count++]=(unsigned)magic;
    check(JS_IsObject(self),"custom settings: lifecycle receiver is the element");
    JSValue name=JS_GetPropertyStr(ctx,self,"localName");const char *text=JS_ToCString(ctx,name);
    check(text&&!strcmp(text,"x-probe"),"custom settings: wrapper identity survives adoption");
    JS_FreeCString(ctx,text);JS_FreeValue(ctx,name);
    check(argc==(magic==2?2:magic==3?4:0),"custom settings: lifecycle argument shape");
    if(magic==3&&ce_bound) {
        JSValue id=JS_GetPropertyStr(ctx,self,"id");text=JS_ToCString(ctx,id);
        check(text&&!strcmp(text,"bound"),"custom settings: bound callback retains its explicit receiver");
        JS_FreeCString(ctx,text);JS_FreeValue(ctx,id);
    }
    if(magic==3) {
        text=JS_ToCString(ctx,argv[0]);
        check(text&&!strcmp(text,"data-x")&&JS_IsNull(argv[3]),"custom settings: observed attribute payload preserved");
        JS_FreeCString(ctx,text);
    }
    if(magic==2) {
        JSValue old=JS_GetGlobalObject(ce_contexts[3]),now=JS_GetGlobalObject(ce_contexts[4]);
        JSValue old_doc=JS_GetPropertyStr(ctx,old,"document"),new_doc=JS_GetPropertyStr(ctx,now,"document");
        check(JS_VALUE_GET_PTR(argv[0])==JS_VALUE_GET_PTR(old_doc)&&JS_VALUE_GET_PTR(argv[1])==JS_VALUE_GET_PTR(new_doc),
              "custom settings: adopted callback receives old and new documents");
        JS_FreeValue(ctx,old_doc);JS_FreeValue(ctx,new_doc);JS_FreeValue(ctx,old);JS_FreeValue(ctx,now);
    }
    if(magic==0&&ce_nested) {
        ce_nested=false;
        JSValue args[2]={JS_NewString(ctx,"data-x"),JS_NewString(ctx,"nested")};
        JSValue result=ce_method(ctx,self,"setAttribute",2,args);
        JS_FreeValue(ctx,args[0]);JS_FreeValue(ctx,args[1]);
        if(JS_IsException(result))return result;
        JS_FreeValue(ctx,result);ce_settings_check(ctx,true);
        check(ce_calls[3]>0,"custom settings: nested CEReactions return before native caller resumes");
    }
    if(magic==3&&ce_throw){ce_throw=false;return JS_ThrowTypeError(ctx,"lifecycle callback failure");}
    return JS_UNDEFINED;
}

static JSValue ce_retire(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,int magic)
{
    (void)ctx;(void)self;(void)argc;(void)argv;
    os64_js_outcome_t out;os64_js_realm_retire(ce_realms[magic],OS64_JS_ABI_ID,&out);return JS_UNDEFINED;
}

static JSValue ce_arm(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx;(void)self;(void)argc;(void)argv;
    ce_started=ce_engine_cut?attempts:dom_attempts;
    if(ce_cut){if(ce_engine_cut)fail_at=attempts+ce_cut;else dom_fail_at=dom_attempts+ce_cut;}
    return JS_UNDEFINED;
}

typedef struct {
    os64_dom_cluster_t *cluster;
    os64_dom_t *bindings[5];
    os64_html_document_t *documents[5];
} CeFixture;

static bool ce_open(CeFixture *f,int policy,unsigned tracked)
{
    *f=(CeFixture){0};ce_entry=1;ce_incumbent=0;ce_constructors=ce_order_count=ce_script_calls=0;ce_nested=ce_throw=ce_bound=false;
    memset(ce_calls,0,sizeof(ce_calls));p2b_ms=1000;p2b_us=1000000;
    os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
    os64_js_outcome_t out;
    if(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&ce_runtime,&out)!=OS64_JS_OK)return false;
    os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
    f->cluster=tracked==7?os64_dom_cluster_create_with_stream_callback_settings(ce_runtime,&limits,&out)
                      :tracked==6?os64_dom_cluster_create_with_synchronous_settings(ce_runtime,&limits,&out)
                      :tracked==5?os64_dom_cluster_create_with_neutral_promise_settings(ce_runtime,&limits,&out)
                      :tracked==4?os64_dom_cluster_create_with_import_settings(ce_runtime,&limits,&out)
                      :tracked==3?os64_dom_cluster_create_with_finalization_settings(ce_runtime,&limits,&out)
                      :tracked==2?os64_dom_cluster_create_with_promise_settings(ce_runtime,&limits,&out)
                      :tracked?os64_dom_cluster_create_with_custom_settings(ce_runtime,&limits,&out)
                      :os64_dom_cluster_create_with_observer_settings(ce_runtime,&limits,&out);
    check(f->cluster!=NULL,"custom settings: selected cluster created");if(!f->cluster)return false;
    ce_realms[0]=os64_js_primary_realm(ce_runtime,OS64_JS_ABI_ID,&out);
    bool ready=true;
    for(int i=0;i<5;i++) {
        if(i)check(os64_js_realm_create(ce_runtime,OS64_JS_ABI_ID,&ce_realms[i],&out)==OS64_JS_OK,"custom settings: child realm created");
        ce_contexts[i]=os64_js_realm_context(ce_realms[i],OS64_JS_ABI_ID,&out);
        DomFixture source=fixture_configured(0,0,&config);os64_js_destroy(source.runtime);
        os64_dom_options_t options=p2b_options("https://custom-settings.example/");
        os64_dom_admission_t a=os64_dom_create_in_realm(f->cluster,ce_realms[i],source.document,source.state,&options,&out);
        f->bindings[i]=a.binding;f->documents[i]=source.document;ready &= a.binding!=NULL;
        check(a.binding&&a.consumed,"custom settings: binding installed");
        if(!a.consumed){os64_page_state_free(source.state);os64_html_document_free(source.document);}
    }
    if(!ready)return false;
    for(int i=0;i<5;i++) {
        JSContext *ctx=ce_contexts[i];JSValue global=JS_GetGlobalObject(ctx);
        for(int j=0;j<5;j++) {
            char name[]={(char)('A'+j),0},retire[]="retireA";retire[6]+=j;
            check(JS_SetPropertyStr(ctx,global,name,JS_GetGlobalObject(ce_contexts[j]))>=0,"custom settings: trusted realm exchange");
            check(JS_SetPropertyStr(ctx,global,retire,JS_NewCFunctionMagic(ctx,ce_retire,retire,0,JS_CFUNC_generic_magic,j))>=0,"custom settings: retirement seam installed");
        }
        if(i==2) {
            check(JS_SetPropertyStr(ctx,global,"ctor",JS_NewCFunction2(ctx,ce_constructor,"ctor",0,JS_CFUNC_constructor,0))>=0,"custom settings: native constructor installed");
            check(JS_SetPropertyStr(ctx,global,"inspect",JS_NewCFunction(ctx,ce_inspect,"inspect",0))>=0,"custom settings: script inspection seam installed");
            check(JS_SetPropertyStr(ctx,global,"arm",JS_NewCFunction(ctx,ce_arm,"arm",0))>=0,"custom settings: allocation seam installed");
            for(int j=0;j<4;j++){char name[]="cb0";name[2]+=j;
                check(JS_SetPropertyStr(ctx,global,name,JS_NewCFunctionData(ctx,ce_callback,0,j,0,NULL))>=0,"custom settings: native lifecycle callback installed");}
        }
        JS_FreeValue(ctx,global);
        realm_script(ce_realms[i],STORAGE_ASSERT "globalThis.errors=[];addEventListener('error',e=>{errors.push(e.error);e.preventDefault()});","custom settings: bootstrap");
    }
    realm_script(ce_realms[1],
        "globalThis.ctor=new Proxy(C.ctor,{});Object.setPrototypeOf(ctor,A.Function.prototype);"
        "for(let i=0;i<4;i++)globalThis['p'+i]=new Proxy(C['cb'+i],{});"
        "C.ctor.prototype=Object.create(D.HTMLElement.prototype);"
        "Object.assign(C.ctor.prototype,{connectedCallback:p0,disconnectedCallback:p1,adoptedCallback:p2,attributeChangedCallback:p3});"
        "C.ctor.observedAttributes=['data-x'];","custom settings: associated proxies and definition prototype");
    return true;
}

static void ce_close(CeFixture *f)
{
    if(f->cluster) {
        for(int i=0;i<5;i++)if(f->bindings[i])memory_balanced(f->bindings[i],"custom settings before drain");
        os64_dom_cluster_drain(f->cluster);
    }
    os64_js_destroy(ce_runtime);os64_dom_cluster_free(f->cluster);ce_runtime=NULL;
    check(live==0&&dom_live==0&&!dom_allocations,"custom settings: capture and definition roots released");
}

static void dom_custom_settings_cases(void)
{
    static const char define[]="D.customElements.define('x-probe',B.ctor);";
    size_t allocation_cuts[2][2]={{0}};
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned allocator=0;allocator<2;allocator++)
    for(size_t mode=allocator?25:0;mode<26+allocation_cuts[policy][allocator];mode++) {
        CeFixture f;if(!ce_open(&f,policy,mode!=19&&mode!=24)){ce_close(&f);continue;}
        os64_js_outcome_t out;
        if(mode==0) {
            realm_script(ce_realms[0],define,"custom settings: define captures A");
            ce_nested=true;
            realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');document.body.appendChild(target);target.remove();E.document.adoptNode(target);E.document.body.appendChild(target);target.setAttribute('data-x','adopted');target.dataset.x='backup';","custom settings: construction nested reactions and adoption");
            check(ce_constructors==1&&ce_calls[0]==2&&ce_calls[1]==1&&ce_calls[2]==1&&ce_calls[3]==3,"custom settings: all lifecycle families deliver in their queues");
            check(ce_order_count>=3&&ce_order[0]==4&&ce_order[1]==0&&ce_order[2]==3,"custom settings: constructor precedes reentrant connected/attribute callbacks");
        } else if(mode==1) {
            realm_script(ce_realms[3],"document.body.innerHTML='<x-probe data-x=initial></x-probe>';","custom settings: pending upgrade target");
            realm_script(ce_realms[0],define,"custom settings: define upgrades existing element");
            check(ce_constructors==1&&ce_calls[0]==1&&ce_calls[3]==1&&ce_order[0]==4&&ce_order[1]==3&&ce_order[2]==0,"custom settings: upgrade stack preserves constructor attribute connected ordering");
        } else if(mode==2) {
            realm_script(ce_realms[0],define,"custom settings: parser definition");
            DomFixture parser={.binding=f.bindings[3],.document=f.documents[3]};custom_parser_insert(&parser,"x-probe","parser");
            check(os64_dom_reactions_run(f.bindings[4],&out)&&out.status==OS64_JS_OK,"custom settings: owed reactions enter from independent host realm");
            check(ce_constructors==1&&ce_calls[0]==1,"custom settings: parser upgrade and connected callback delivered");
        } else if(mode>=4&&mode<=8) {
            realm_script(ce_realms[0],define,"custom settings: retained definition");
            realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');E.document.adoptNode(target);E.document.body.appendChild(target);","custom settings: element adopted out of registry document");
            memset(ce_calls,0,sizeof(ce_calls));
            os64_html_node_t *element=f.documents[4]->body->last_child;
            check(element!=NULL&&os64_html_set_attr(f.documents[4],element,"data-x","queued",6)==OS64_HTML_OK,"custom settings: native mutation queues adopted reaction");
            check(os64_js_realm_retire(ce_realms[mode-4],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"custom settings: independent realm role retired");
            (void)os64_dom_reactions_run(f.bindings[mode==8?3:4],&out);
            check(ce_calls[3]==(mode==7?1u:0u),"custom settings: registry retirement preserves healthy adopted callback; other roles suppress");
        } else if(mode==9) {
            realm_script(ce_realms[1],"globalThis.pair=Proxy.revocable(C.cb3,{});C.ctor.prototype.attributeChangedCallback=pair.proxy;","custom settings: revocable lifecycle callback");
            realm_script(ce_realms[0],define,"custom settings: revocable definition captured");
            realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');B.pair.revoke();target.setAttribute('data-x','bad');document.body.appendChild(target);","custom settings: revoked callback and healthy successor");
            realm_script(ce_realms[2],"assert(errors.length===1&&errors[0] instanceof B.TypeError);","custom settings: associated error realm and independent report owner");
            check(ce_calls[3]==0&&ce_calls[0]==1,"custom settings: callback failure does not discard later healthy reaction");
        } else if(mode==10) {
            realm_script(ce_realms[0],define,"custom settings: throwing callback definition");ce_throw=true;
            realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');target.setAttribute('data-x','bad');document.body.appendChild(target);","custom settings: throwing callback followed by healthy reaction");
            realm_script(ce_realms[2],"assert(errors.length===1&&errors[0] instanceof B.TypeError);","custom settings: prepared callback error and reporting");
            check(ce_calls[3]==1&&ce_calls[0]==1,"custom settings: callback exception preserves later reactions");
        } else if(mode==11||mode==12) {
            realm_script(ce_realms[2],"globalThis.bad=class extends D.HTMLElement{constructor(){super();while(true){}}};","custom settings: interruptible script constructor");
            realm_script(ce_realms[1],"ctor=new Proxy(C.bad,{});","custom settings: foreign constructor associated proxy");
            if(mode==11)realm_script(ce_realms[3],"document.body.innerHTML='<x-probe></x-probe>';","custom settings: terminal upgrade target");
            else realm_script(ce_realms[0],define,"custom settings: terminal synchronous definition");
            const char *source=mode==11?define:"D.document.createElement('x-probe');";
            dom_clock_step=10000;
            check(os64_js_realm_run(ce_realms[0],source,strlen(source),"custom-terminal",&out)==OS64_JS_LIMIT,"custom settings: terminal construction unwinds through original guard");
            dom_clock_step=0;os64_dom_script_ended(f.bindings[0],"custom-terminal",&out);
        } else if(mode==13) {
            realm_script(ce_realms[1],"globalThis.pair=Proxy.revocable(C.ctor,{});ctor=pair.proxy;","custom settings: revocable constructor");
            realm_script(ce_realms[0],define,"custom settings: constructor captured before revocation");
            realm_script(ce_realms[3],"B.pair.revoke();globalThis.target=document.createElement('x-probe');assert(target instanceof HTMLUnknownElement);","custom settings: revoked constructor produces failed element");
            realm_script(ce_realms[2],"assert(errors.length===1&&errors[0] instanceof B.TypeError);","custom settings: construction error uses associated realm");
        } else if(mode==14) {
            realm_script(ce_realms[0],define,"custom settings: direct new definition");ce_entry=ce_incumbent=4;
            realm_script(ce_realms[4],"globalThis.target=new B.ctor();assert(target.ownerDocument===D.document);","custom settings: author new keeps its own caller settings");
            check(ce_constructors==1,"custom settings: direct new uses HTML constructor without host preparation");
        } else if(mode==15||mode==19) {
            const char raw[]="globalThis.readOptions=false;try{D.customElements.define('x-probe',B.ctor,{get extends(){readOptions=true;return undefined}});globalThis.accepted=true}catch(e){if(e.name!=='InvalidStateError')throw e;globalThis.accepted=false}";
            JSContext *ctx=ce_contexts[0];JSValue result=JS_Eval(ctx,raw,sizeof(raw)-1,"custom-unprepared",JS_EVAL_TYPE_GLOBAL);
            check(!JS_IsException(result),"custom settings: unprepared definition has expected refusal");
            if(JS_IsException(result))JS_FreeValue(ctx,JS_GetException(ctx));
            JS_FreeValue(ctx,result);
            realm_script(ce_realms[0],mode==19?"assert(accepted&&readOptions);":"assert(!accepted&&!readOptions);","custom settings: new opt-in refuses before options; previous opt-in remains legacy");
            if(mode==15) {
                realm_script(ce_realms[0],"globalThis.refused=false;Promise.resolve().then(()=>{try{D.customElements.define('x-probe',B.ctor)}catch(e){refused=e.name==='InvalidStateError'}});","custom settings: unprepared Promise definition");
                realm_script(ce_realms[0],"assert(refused);","custom settings: Promise cannot borrow enclosing host scope");
            }
        } else if(mode==3||mode==16||mode==17||mode==18||mode==22) {
            size_t held=os64_dom_registry_count(f.bindings[3]);
            if(mode==16)realm_script(ce_realms[1],"Object.defineProperty(C.ctor.prototype,'connectedCallback',{get(){retireA();return p0}});Object.defineProperty(C.ctor.prototype,'disconnectedCallback',{get(){globalThis.readLater=true;return p1}});","custom settings: lifecycle getter retires incumbent");
            if(mode==17)realm_script(ce_realms[1],"Object.defineProperty(C.ctor,'disabledFeatures',{get(){retireB();return []}});","custom settings: late getter retires associated realm");
            if(mode==22)realm_script(ce_realms[1],"Object.defineProperty(C.ctor,'disabledFeatures',{get(){retireD();return []}});","custom settings: late getter retires registry");
            if(mode==18)realm_script(ce_realms[1],"Object.defineProperty(C.ctor,'observedAttributes',{get(){throw 42}});","custom settings: later getter throws");
            const char *source=mode==3?"try{D.customElements.define('x-probe',B.ctor,{get extends(){retireB();return undefined}});assert(false)}catch(e){assert(e.name==='InvalidStateError')}"
                :mode==18?"try{D.customElements.define('x-probe',B.ctor);assert(false)}catch(e){assert(e===42)}"
                :"try{D.customElements.define('x-probe',B.ctor);assert(false)}catch(e){assert(e.name==='InvalidStateError')}";
            realm_script(ce_realms[0],source,"custom settings: reentrant definition refuses publication");
            check(os64_dom_registry_count(f.bindings[3])==held,"custom settings: refused reading publishes no registry roots");
            if(mode==16)realm_script(ce_realms[1],"assert(!globalThis.readLater);","custom settings: capture occurs before next lifecycle getter");
            if(mode!=22)realm_script(ce_realms[3],"assert(customElements.get('x-probe')===undefined);","custom settings: failed definition name remains available");
            if(mode==18) {
                realm_script(ce_realms[1],"Object.defineProperty(C.ctor,'observedAttributes',{value:['data-x'],configurable:true,writable:true});","custom settings: throwing getter repaired");
                realm_script(ce_realms[0],define,"custom settings: definition flag resets after throwing getter");
                realm_script(ce_realms[3],"assert(customElements.get('x-probe')===B.ctor);","custom settings: refused name can be defined on retry");
            }
        } else if(mode==20) {
            realm_script(ce_realms[3],"globalThis.bound=document.createElement('x-probe');bound.id='bound';","custom settings: bound receiver prepared");
            realm_script(ce_realms[1],"C.ctor.prototype.attributeChangedCallback=Function.prototype.bind.call(C.cb3,D.bound);","custom settings: bound callback follows target associated realm");
            realm_script(ce_realms[0],define,"custom settings: bound callback definition");ce_bound=true;
            realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');","custom settings: bound callback target constructed");ce_entry=2;
            realm_script(ce_realms[3],"target.setAttribute('data-x','bound-call');","custom settings: bound lifecycle invocation");
            check(ce_calls[3]==1,"custom settings: bound callback delivered once");
        } else if(mode==21) {
            realm_script(ce_realms[2],"globalThis.scriptClass=class extends D.HTMLElement{constructor(){super();C.inspect()}static get observedAttributes(){return ['data-x']}attributeChangedCallback(){C.inspect()}};","custom settings: script-bearing constructor and lifecycle");
            realm_script(ce_realms[1],"ctor=new Proxy(C.scriptClass,{});","custom settings: script constructor associated proxy");
            realm_script(ce_realms[0],define,"custom settings: script definition captured");ce_incumbent=2;
            realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');","custom settings: script frame supersedes captured backup");ce_entry=2;
            realm_script(ce_realms[3],"target.setAttribute('data-x','script');","custom settings: script lifecycle uses its associated entry");
            check(ce_script_calls==2,"custom settings: script constructor and callback inspected");
        } else if(mode==23||mode==24) {
            realm_script(ce_realms[1],"globalThis.pair=Proxy.revocable(C.cb3,{});C.ctor.prototype.attributeChangedCallback=pair.proxy;pair.revoke();","custom settings: already revoked callable remains a valid callback value");
            realm_script(ce_realms[0],define,"custom settings: revoked callback metadata does not reject definition");
            realm_script(ce_realms[3],"assert(customElements.get('x-probe')===B.ctor);","custom settings: revoked callback definition published");
            if(mode==23) {
                realm_script(ce_realms[3],"globalThis.target=document.createElement('x-probe');target.setAttribute('data-x','revoked');assert(errors.length===1&&errors[0] instanceof B.TypeError);","custom settings: unavailable executable provenance uses registry report fallback");
                check(ce_calls[3]==0,"custom settings: already revoked callback cannot execute");
            }
        } else if(mode>=25) {
            realm_script(ce_realms[0],"globalThis.prior=class extends D.HTMLElement{};D.customElements.define('x-prior',prior);globalThis.register=()=>{C.arm();D.customElements.define('x-probe',B.ctor)};","custom settings: allocation-isolated definition");
            size_t held=os64_dom_registry_count(f.bindings[3]);ce_cut=mode-25;ce_engine_cut=allocator!=0;
            JSContext *ctx=ce_contexts[0];JSValue global=JS_GetGlobalObject(ctx),fn=JS_GetPropertyStr(ctx,global,"register");JS_FreeValue(ctx,global);
            check(os64_js_realm_task_begin(ce_realms[0],OS64_JS_ABI_ID,"custom-cut",&out)==OS64_JS_OK,"custom settings: allocation task opened");
            os64_js_realm_call(ce_realms[0],OS64_JS_ABI_ID,fn,JS_UNDEFINED,0,NULL,NULL,&out);
            size_t measured=(ce_engine_cut?attempts:dom_attempts)-ce_started;dom_fail_at=fail_at=0;JS_FreeValue(ctx,fn);
            bool healthy=out.status==OS64_JS_OK||out.status==OS64_JS_EXCEPTION;
            if(!ce_cut){check(out.status==OS64_JS_OK&&measured<128,"custom settings: definition allocation measurement");allocation_cuts[policy][allocator]=measured<128?measured:0;
                fprintf(stderr,"custom definition allocations: policy=%d engine=%u cuts=%zu\n",policy,allocator,measured);}
            else {
                check(measured>=ce_cut&&out.status!=OS64_JS_OK,"custom settings: definition allocation refusal reached");
                check(os64_dom_registry_count(f.bindings[3])==held,"custom settings: partial publication rolls back retained roots");
            }
            os64_js_task_end(ce_runtime,OS64_JS_ABI_ID,&out);
            if(ce_cut&&healthy) {
                realm_script(ce_realms[3],"assert(customElements.get('x-probe')===undefined);assert(customElements.get('x-prior')===A.prior);assert(document.createElement('x-prior') instanceof A.prior);","custom settings: rollback preserves older definitions and their roots");
                realm_script(ce_realms[0],define,"custom settings: allocation refusal permits healthy retry");
            }
        }
        check(JS_GetEntryContext(JS_GetRuntime(ce_contexts[0]))==NULL&&JS_GetIncumbentContext(JS_GetRuntime(ce_contexts[0]))==NULL,"custom settings: scope stack restored");
        ce_close(&f);
    }
}
