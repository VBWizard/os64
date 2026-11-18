/* The host exchanges raw globals only to exercise the private IDL boundary.
 * This is not a WindowProxy property-access or constructor escape proof. */
static void dom_window_authority_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;
        if(!authority_open(&f,policy,0)){check(false,"Window authority: fixture");ce_close(&f);continue;}
        realm_script(ce_realms[0],
            "globalThis.desc=(w,n)=>Object.getOwnPropertyDescriptor(w,n);"
            "globalThis.badType=f=>{let e;try{f()}catch(x){e=x}assert(e&&e.name==='TypeError')};"
            "globalThis.n=0;globalThis.value={toString(){n++;return 'hello'},valueOf(){n++;return 1}};"
            "denied(()=>Reflect.apply(A.alert,B,[value]));assert(n===0);"
            "Reflect.apply(B.alert,C,[value]);assert(n===1);"
            "Reflect.apply(A.alert,null,[value]);Reflect.apply(A.alert,undefined,[value]);assert(n===3);"
            "denied(()=>Reflect.apply(B.alert,null,[value]));assert(n===3);"
            "badType(()=>Reflect.apply(A.alert,{},[value]));"
            "badType(()=>Reflect.apply(A.alert,1,[value]));"
            "let traps=0;const p=new Proxy(C,{get(){traps++;throw 1},getPrototypeOf(){traps++;throw 2}});"
            "badType(()=>Reflect.apply(A.alert,p,[value]));assert(n===3&&traps===0);",
            "Window authority: receiver, defining owner and source are independent; no conversion on refusal");
        realm_script(ce_realms[0],
            "const s=desc(B,'status');Reflect.apply(s.set,C,['receiver']);"
            "assert(C.status==='receiver'&&A.status==='');assert(Reflect.apply(s.get,C,[])==='receiver');"
            "n=0;denied(()=>Reflect.apply(desc(A,'status').set,B,[value]));assert(n===0);"
            "for(const name of ['document','history','customElements','navigator','performance','screen','crypto','getSelection']){"
            "const d=desc(B,name);const fn=d.get||d.value;"
            "assert(Reflect.apply(fn,C,[])===(name==='getSelection'?C.getSelection():C[name]),name);"
            "denied(()=>Reflect.apply(fn,B,[]));}"
            "for(const name of ['window','self','frames','top','parent']){"
            "assert(Reflect.apply(desc(A,name).get,B,[])===B,name);}"
            "for(const name of ['closed','length','opener','location']){"
            "assert(Reflect.apply(desc(A,name).get,B,[])===B[name],name);}"
            "for(const name of ['self','frames','parent','length','opener']){"
            "denied(()=>Reflect.apply(desc(A,name).set,B,[value]));}assert(n===0);",
            "Window authority: held values and cross-origin getter/setter policies follow actual receiver");
        realm_script(ce_realms[0],
            "n=0;for(const name of ['alert','confirm','prompt','setTimeout','setInterval','clearTimeout','clearInterval',"
            "'requestAnimationFrame','cancelAnimationFrame','requestIdleCallback','cancelIdleCallback','queueMicrotask',"
            "'reportError','structuredClone','atob','btoa','matchMedia','getComputedStyle','scroll','scrollTo','scrollBy',"
            "'moveTo','moveBy','resizeTo','resizeBy','captureEvents','releaseEvents']){"
            "assert(typeof B[name]==='function',name);denied(()=>Reflect.apply(B[name],B,[value,value]));}assert(n===0);"
            "for(const name of ['localStorage','sessionStorage','innerWidth','outerWidth','devicePixelRatio','origin',"
            "'isSecureContext','originAgentCluster','frameElement','external','clientInformation','menubar','onload']){"
            "denied(()=>B[name]);}"
            "globalThis.fetchError=null;let synchronous=false;try{B.fetch(value).catch(e=>fetchError=e.name)}catch(e){synchronous=true}"
            "assert(!synchronous&&n===0);",
            "Window authority: method and attribute families refuse before conversion; fetch rejects asynchronously");
        realm_script(ce_realms[0],"assert(fetchError==='SecurityError'&&n===0)",
            "Window authority: fetch rejection retains the authority error");
        realm_script(ce_realms[0],
            "const proto=A.EventTarget.prototype;let calls=0;const callback=()=>calls++;"
            "n=0;denied(()=>Reflect.apply(proto.addEventListener,B,[value,callback,value]));"
            "denied(()=>Reflect.apply(proto.removeEventListener,B,[value,callback,value]));"
            "denied(()=>Reflect.apply(proto.dispatchEvent,B,[value]));assert(n===0);"
            "Reflect.apply(proto.addEventListener,C,['test',callback]);C.dispatchEvent(new Event('test'));assert(calls===1);"
            "Reflect.apply(proto.removeEventListener,C,['test',callback]);C.dispatchEvent(new Event('test'));assert(calls===1);"
            "denied(()=>Reflect.apply(desc(A,'onload').set,B,[callback]));"
            "Reflect.apply(desc(B,'onload').set,C,[callback]);assert(C.onload===callback&&A.onload===null);"
            "const el=C.document.createElement('p');el.addEventListener('test',callback);el.dispatchEvent(new Event('test'));assert(calls===2);",
            "Window authority: inherited EventTarget operations and property handlers select the target");
        realm_script(ce_realms[1],
            "globalThis.received='';onmessage=e=>{received=e.data}",
            "Window authority: receiver's own message listener");
        realm_script(ce_realms[0],
            "globalThis.timerRan=0;Reflect.apply(B.setTimeout,C,[()=>timerRan++,0]);"
            "Reflect.apply(A.postMessage,B,['receiver-message','*']);",
            "Window authority: borrowed timer and exempt postMessage publish on receiver");
        check(os64_dom_timer_next(f.bindings[0])==UINT64_MAX&&
              os64_dom_timer_next(f.bindings[1])==UINT64_MAX&&
              os64_dom_timer_next(f.bindings[2])!=UINT64_MAX,
              "Window authority: timer belongs to receiver, not source or function owner");
        os64_js_outcome_t out;
        check(os64_dom_timer_fire(f.bindings[2],UINT64_MAX,&out)&&out.status==OS64_JS_OK,
              "Window authority: receiver timer delivers");
        check(os64_dom_message_next(f.bindings[0])==UINT64_MAX&&
              os64_dom_message_fire(f.bindings[1],&out)&&out.status==OS64_JS_OK,
              "Window authority: permitted message belongs to receiver");
        realm_script(ce_realms[0],"assert(timerRan===1&&B.received==='receiver-message')",
            "Window authority: actual queued work retains receiver routing");
        realm_script(ce_realms[1],
            "globalThis.callA=()=>Reflect.apply(A.alert,A,[]);"
            "globalThis.proxyA=new Proxy(A.alert,{apply(t,s,a){return Reflect.apply(t,s,a)}});",
            "Window authority: prepare foreign script and callable proxy");
        realm_script(ce_realms[0],
            "denied(B.callA);denied(()=>Reflect.apply(B.proxyA,A,[]));"
            "denied(()=>B.Reflect.apply(A.alert,A,[]));"
            "denied(B.alert.bind(B));denied(()=>Reflect.apply(new Proxy(A.alert,{}),B,[]));"
            "B.renamedClose=B.alert;B.renamedAlert=B.close;denied(()=>B.renamedClose());B.renamedAlert();"
            "B.focus();B.blur();B.close();"
            "Reflect.apply(desc(A,'location').set,B,['https://destination.test/window']);",
            "Window authority: pre-native caller survives nesting and original policy survives renaming");
        os64_dom_navigation_t ask;
        check(os64_dom_take_navigation(f.bindings[1],&ask)&&!strcmp(ask.url,"https://destination.test/window"),
              "Window authority: permitted cross-origin location assignment reaches receiver navigation");
        realm_script(ce_realms[0],
            "for(const name of ['innerWidth','outerWidth','devicePixelRatio','performance','screen','parent','length','self','frames']){"
            "const d=desc(A,name),before=A[name];assert(typeof d.set==='function',name);"
            "Reflect.apply(d.set,C,[123]);assert(C[name]===123&&A[name]===before,name);}"
            "retireB();Reflect.apply(B.alert,C,[]);denied(()=>Reflect.apply(A.alert,B,[]));"
            "retireA();denied(()=>Reflect.apply(B.alert,C,[]));",
            "Window authority: Replaceable writes receiver; retired defining owner differs from retired target/source");
        ce_close(&f);
    }
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;
        if(!authority_open(&f,policy,2)){check(false,"Window authority: mixed fixture");ce_close(&f);continue;}
        realm_script(ce_realms[4],"denied(()=>A.alert());denied(()=>A.close())",
            "Window authority: legacy source has no origin authority");
        realm_script(ce_realms[0],"denied(()=>Reflect.apply(A.alert,E,[]));denied(()=>Reflect.apply(A.close,E,[]))",
            "Window authority: origin-bearing member refuses receiver without origin settings");
        JSContext *ctx=ce_contexts[0];JSValue global=JS_GetGlobalObject(ctx);
        JSValue fn=JS_GetPropertyStr(ctx,global,"alert"),result=JS_Call(ctx,fn,global,0,NULL);
        check(JS_IsException(result),"Window authority: unprepared host invocation refuses");
        if(JS_IsException(result))JS_FreeValue(ctx,JS_GetException(ctx));
        JS_FreeValue(ctx,result);JS_FreeValue(ctx,fn);JS_FreeValue(ctx,global);
        ce_close(&f);
    }
}
