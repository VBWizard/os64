/* A captures, B owns the callable proxy, C supplies native code, D owns
 * the observer and activity. Exercise each role under both teardown policies. */
static os64_js_runtime_t *observer_runtime;
static os64_js_realm_t *observer_realms[4];
static JSContext *observer_contexts[4];
static unsigned observer_calls, observer_family;
static bool observer_disconnect;
static size_t observer_cut, observer_started;

static JSValue observer_arm(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)ctx; (void)self; (void)argc; (void)argv;
    observer_started=attempts;
    if(observer_cut)fail_at=attempts+observer_cut;
    return JS_UNDEFINED;
}

static JSValue observer_probe(JSContext *ctx, JSValueConst self, int argc,
                               JSValueConst *argv, int magic, JSValue *data)
{
    (void)magic; (void)data;
    os64_js_outcome_t out;
    check(os64_js_settings_realm(observer_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_ENTRY,&out)==observer_realms[1],
          "observer settings: associated entry is B");
    check(os64_js_settings_realm(observer_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==observer_realms[0],
          "observer settings: captured incumbent is A");
    check(os64_js_settings_realm(observer_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_CURRENT,&out)==observer_realms[2],
          "observer settings: executable current is C");
    check(ctx==observer_contexts[1],"observer settings: native incoming context follows entry");
    check(argc==(observer_family==3?3:2)&&JS_IsObject(self)&&JS_VALUE_GET_PTR(self)==JS_VALUE_GET_PTR(argv[1]),
          "observer settings: observer receiver and arguments preserved");
    JSValue entries;
    if(observer_family==3) {
        JSValue method=JS_GetPropertyStr(ctx,argv[0],"getEntries");
        entries=JS_Call(ctx,method,argv[0],0,NULL);JS_FreeValue(ctx,method);
    } else entries=JS_DupValue(ctx,argv[0]);
    JSValue length=JS_GetPropertyStr(ctx,entries,"length");int32_t count=0;
    check(JS_ToInt32(ctx,&count,length)==0&&count==1,"observer settings: queued record delivered once");
    JS_FreeValue(ctx,length);JS_FreeValue(ctx,entries);
    observer_calls++;
    if(observer_disconnect) {
        JSValue global=JS_GetGlobalObject(observer_contexts[0]);
        check(JS_SetPropertyStr(ctx,global,"o",JS_NULL)>=0,"observer settings: author observer reference dropped during delivery");
        JS_FreeValue(ctx,global);
        JSValue method=JS_GetPropertyStr(ctx,self,"disconnect");
        JSValue result=JS_Call(ctx,method,self,0,NULL);JS_FreeValue(ctx,method);
        if(JS_IsException(result))return result;
        JS_FreeValue(ctx,result);
        JS_RunGC(JS_GetRuntime(ctx));
        check(os64_js_settings_realm(observer_runtime,OS64_JS_ABI_ID,OS64_JS_SETTINGS_INCUMBENT,&out)==observer_realms[0],
              "observer settings: disconnect and GC preserve active capture");
    }
    return JS_UNDEFINED;
}

static JSValue observer_retire(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic)
{
    (void)ctx;(void)self;(void)argc;(void)argv;
    os64_js_outcome_t out;os64_js_realm_retire(observer_realms[magic],OS64_JS_ABI_ID,&out);
    return JS_UNDEFINED;
}

static void dom_observer_settings_cases(void)
{
    static const char *const names[]={"MutationObserver","ResizeObserver","IntersectionObserver","PerformanceObserver"};
    static const char *const observe[]={"o.observe(D.target,{attributes:true})","o.observe(D.target)","o.observe(D.target)","o.observe({entryTypes:['mark']})"};
    size_t cuts[2][4]={{0}};
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(unsigned family=0;family<4;family++)
    for(size_t mode=0;mode<17+cuts[policy][family];mode++) {
        if((mode==11&&family==3)||(mode==15&&family!=2))continue;
        p2b_ms=1000;p2b_us=1000000;observer_calls=0;observer_family=family;observer_disconnect=mode==0;
        os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,"observer settings: runtime created");
        if(!runtime)continue;
        observer_runtime=runtime;
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_dom_cluster_t *cluster=mode==12?os64_dom_cluster_create_with_handler_settings(runtime,&limits,&out)
            :os64_dom_cluster_create_with_observer_settings(runtime,&limits,&out);
        check(cluster!=NULL,"observer settings: opt-in cluster created");
        if(!cluster){os64_js_destroy(runtime);continue;}
        os64_dom_t *bindings[4]={0};DomFixture target_fixture={0};bool ready=true;
        observer_realms[0]=os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out);
        for(int i=0;i<4;i++) {
            if(i)check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&observer_realms[i],&out)==OS64_JS_OK,"observer settings: child created");
            observer_contexts[i]=os64_js_realm_context(observer_realms[i],OS64_JS_ABI_ID,&out);
            DomFixture f=fixture_configured(0,0,&config);os64_js_destroy(f.runtime);
            os64_dom_options_t options=p2b_options("https://observers.example/");
            os64_dom_admission_t a=os64_dom_create_in_realm(cluster,observer_realms[i],f.document,f.state,&options,&out);
            bindings[i]=a.binding;ready &= a.binding!=NULL;
            if(i==3){target_fixture.document=f.document;target_fixture.binding=a.binding;}
            check(a.binding&&a.consumed,"observer settings: binding installed");
            if(!a.consumed){os64_page_state_free(f.state);os64_html_document_free(f.document);}
        }
        if(ready) {
            ro_refuse=false;io_reset();io_place("one",0,0,10,10);
            io_view=(ViewFake){.view={.width=800,.height=600}};
            os64_dom_view_provider_t view={fake_view_read,fake_view_scroll,&io_view,NULL};
            os64_dom_set_view(bindings[3],&view);
            os64_dom_set_resizes(bindings[3],ro_provider,&target_fixture);
            os64_dom_set_intersections(bindings[3],io_provider,NULL);
            for(int i=0;i<4;i++) {
                JSContext *ctx=observer_contexts[i];JSValue global=JS_GetGlobalObject(ctx);
                for(int j=0;j<4;j++) {
                    char name[]={(char)('A'+j),0},retire[]="retireA";retire[6]+=j;
                    check(JS_SetPropertyStr(ctx,global,name,JS_GetGlobalObject(observer_contexts[j]))>=0,"observer settings: trusted realm exchange");
                    check(JS_SetPropertyStr(ctx,global,retire,JS_NewCFunctionMagic(ctx,observer_retire,retire,0,JS_CFUNC_generic_magic,j))>=0,
                          "observer settings: retirement seam installed");
                }
                if(i==2) {
                    check(JS_SetPropertyStr(ctx,global,"probe",JS_NewCFunctionData(ctx,observer_probe,2,0,0,NULL))>=0,"observer settings: native callback installed");
                    check(JS_SetPropertyStr(ctx,global,"arm",JS_NewCFunction(ctx,observer_arm,"arm",0))>=0,"observer settings: allocation seam installed");
                }
                JS_FreeValue(ctx,global);
                realm_script(observer_realms[i],STORAGE_ASSERT "globalThis.errors=[];addEventListener('error',e=>{errors.push(e.error);e.preventDefault()});","observer settings: bootstrap");
            }
            realm_script(observer_realms[1],"globalThis.p=new Proxy(C.probe,{});Object.setPrototypeOf(p,A.Function.prototype);","observer settings: associated identity independent of prototype");
            realm_script(observer_realms[3],"globalThis.target=document.getElementById('one');target.setAttribute('data-w','12');","observer settings: target prepared");
            char registration[512];snprintf(registration,sizeof(registration),"globalThis.o=new D.%s(B.p);%s;",names[family],observe[family]);
            if(mode>=16) {
                char source[256];snprintf(source,sizeof(source),"globalThis.construct=()=>{C.arm();return new D.%s(B.p)};",names[family]);
                realm_script(observer_realms[0],source,"observer settings: isolated constructor prepared");
                JSContext *ctx=observer_contexts[0];JSValue global=JS_GetGlobalObject(ctx);
                JSValue construct=JS_GetPropertyStr(ctx,global,"construct");JS_FreeValue(ctx,global);
                check(os64_js_realm_task_begin(observer_realms[0],OS64_JS_ABI_ID,"observer-allocation",&out)==OS64_JS_OK,"observer settings: allocation task opened");
                observer_cut=mode-16;
                os64_js_realm_call(observer_realms[0],OS64_JS_ABI_ID,construct,JS_UNDEFINED,0,NULL,NULL,&out);
                size_t measured=attempts-observer_started;fail_at=0;
                if(observer_cut)check(measured>=observer_cut&&out.status!=OS64_JS_OK,"observer settings: engine allocation refusal reached");
                else {
                    check(out.status==OS64_JS_OK&&measured<128,"observer settings: constructor allocation measurement");
                    cuts[policy][family]=measured<128?measured:0;
                    fprintf(stderr,"observer constructor allocations: policy=%d family=%u cuts=%zu\n",policy,family,measured);
                }
                JS_FreeValue(ctx,construct);
                os64_js_task_end(runtime,OS64_JS_ABI_ID,&out);
            } else if(mode==12) {
                char source[128];snprintf(source,sizeof(source),"new D.%s(B.p)",names[family]);
                JSContext *ctx=observer_contexts[0];JSValue value=JS_Eval(ctx,source,strlen(source),"legacy-observer",JS_EVAL_TYPE_GLOBAL);
                check(!JS_IsException(value),"observer settings: previous opt-in preserves legacy constructors");
                if(JS_IsException(value))JS_FreeValue(ctx,JS_GetException(ctx));
                JS_FreeValue(ctx,value);
            } else if(mode==15) {
                realm_script(observer_realms[0],"try{new D.IntersectionObserver(B.p,{get delay(){retireA();return 0}});assert(false)}catch(e){assert(e.name==='InvalidStateError')}","observer settings: options retirement prevents publication");
            } else if(mode==5) {
                char raw[768];snprintf(raw,sizeof(raw),"globalThis.readOption=false;try{new D.%s(B.p,{get delay(){readOption=true;return 0}});throw Error('accepted')}catch(e){if(e.name!=='InvalidStateError')throw e}if(readOption)throw Error('late capture')",names[family]);
                JSContext *ctx=observer_contexts[0];JSValue value=JS_Eval(ctx,raw,strlen(raw),"unprepared-observer",JS_EVAL_TYPE_GLOBAL);
                check(!JS_IsException(value),"observer settings: unprepared constructor refuses before getters");
                if(JS_IsException(value))JS_FreeValue(ctx,JS_GetException(ctx));
                JS_FreeValue(ctx,value);
                char source[1024];snprintf(source,sizeof(source),"globalThis.refused=false;Promise.resolve().then(()=>{try{new D.%s(B.p)}catch(e){refused=e.name==='InvalidStateError'}});",names[family]);
                realm_script(observer_realms[0],source,"observer settings: unprepared Promise constructor");
                realm_script(observer_realms[0],"assert(refused);","observer settings: Promise does not borrow host scope");
            } else if(mode==6||mode==7||mode==13||mode==14) {
                char source[768];snprintf(source,sizeof(source),"let nt=new Proxy(function(){},{get(t,k){if(k==='prototype'){%s}return Reflect.get(t,k)}});try{Reflect.construct(D.%s,[B.p],nt);assert(false)}catch(e){assert(%s)}",mode==6?"throw 42;":mode==13?"retireA();return D.Object.prototype;":mode==14?"retireD();return D.Object.prototype;":"retireB();return D.Object.prototype;",names[family],mode==6?"e===42":"e.name==='InvalidStateError'");
                realm_script(observer_realms[0],source,"observer settings: prototype reentry refuses without publication");
            } else if(mode==10) {
                char source[768];snprintf(source,sizeof(source),"B.weak=new B.WeakRef(B.p);globalThis.o=new D.%s(B.p);B.p=null;o=null;",names[family]);
                realm_script(observer_realms[0],source,"observer settings: unregistered observer released");
                JS_RunGC(JS_GetRuntime(observer_contexts[0]));
                realm_script(observer_realms[0],"assert(B.weak.deref()===undefined);","observer settings: finalizer releases captured callback");
            } else if(mode==11) {
                dom_fail_at=dom_attempts+1;
                check(os64_js_realm_run(observer_realms[0],registration,strlen(registration),"observer-cut",&out)==OS64_JS_EXCEPTION,"observer settings: native record allocation refusal");
                dom_fail_at=0;os64_dom_script_ended(bindings[0],"observer-cut",&out);
            } else {
                if(mode==8)realm_script(observer_realms[1],"globalThis.pair=Proxy.revocable(C.probe,{});p=pair.proxy;","observer settings: revocable callback");
                if(mode==9)realm_script(observer_realms[1],"p=new Proxy(new C.Function('while(true){}'),{});","observer settings: interruptible callback");
                realm_script(observer_realms[0],registration,"observer settings: registration captures A");
                if(mode==8) {
                    realm_script(observer_realms[0],"B.pair.revoke();B.p=new B.Proxy(C.probe,{});globalThis.first=o;","observer settings: callback revoked after capture");
                    realm_script(observer_realms[0],registration,"observer settings: healthy successor registered");
                }
                char trigger[256];snprintf(trigger,sizeof(trigger),"%s%s",family==0?"target.setAttribute('data-x','1');":family==3?"performance.mark('sample');":"",mode>=1&&mode<=4?(const char *[]){"","retireA();","retireB();","retireC();","retireD();"}[mode]:"");
                if(mode==9&&family==0)dom_clock_step=10000;
                os64_js_status_t status=os64_js_realm_run(observer_realms[3],trigger,strlen(trigger),"observer-trigger",&out);
                dom_clock_step=0;
                os64_dom_script_ended(bindings[3],"observer-trigger",&out);
                if(family!=0) {
                    if(family==2)(void)os64_dom_intersections_update(bindings[3]);
                    if(mode==9)dom_clock_step=10000;
                    if(family==1)(void)os64_dom_resizes_step(bindings[3],&out);
                    if(family==2)(void)os64_dom_intersections_fire(bindings[3],&out);
                    if(family==3)(void)os64_dom_timer_fire(bindings[3],os64_dom_timer_next(bindings[3]),&out);
                    dom_clock_step=0;status=out.status;
                }
                if(mode==9)check(status==OS64_JS_LIMIT,"observer settings: terminal callback interruption preserved");
                else if(mode!=4) { if(status!=(mode==8&&family!=0?OS64_JS_EXCEPTION:OS64_JS_OK))fprintf(stderr,"observer outcome: family=%u mode=%zu status=%d %s\n",family,mode,status,out.message); check(status==(mode==8&&family!=0?OS64_JS_EXCEPTION:OS64_JS_OK),"observer settings: delivery outcome"); }
                check(observer_calls==((mode==0||mode==8)?1u:0u),"observer settings: retirement suppresses independently; errors allow successor");
                if(mode==8)realm_script(observer_realms[2],"assert(errors.length===1&&errors[0] instanceof B.TypeError);","observer settings: revoked error attributed to existing report owner");
            }
            check(JS_GetEntryContext(JS_GetRuntime(observer_contexts[0]))==NULL&&JS_GetIncumbentContext(JS_GetRuntime(observer_contexts[0]))==NULL,"observer settings: scopes restored");
            for(int i=0;i<4;i++)memory_balanced(bindings[i],"observer settings before drain");
        }
        os64_dom_cluster_drain(cluster);os64_js_destroy(runtime);os64_dom_cluster_free(cluster);
        check(live==0&&dom_live==0&&!dom_allocations,"observer settings: roots and records released");
    }
}
