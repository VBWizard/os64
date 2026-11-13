/* Shared internal slots must preserve genuine brands across retained realms. */
static void dom_stream_realm_cases(void)
{
    for (int policy=OS64_JS_TEARDOWN_FATAL; policy<=OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned retired=0; retired<2; ++retired) {
        os64_js_config_t config=fixture_config(); config.limits.memory_bytes=64u<<20;
        os64_js_runtime_t *runtime=NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "stream realms: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_dom_cluster_t *cluster=os64_dom_cluster_create(runtime,&limits,&out);
        if (!cluster) { check(false,"stream realms: cluster created"); os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[2]={os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out),NULL};
        check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&realms[1],&out)==OS64_JS_OK,"stream realms: child created");
        os64_dom_t *bindings[2]={0};
        for (unsigned i=0; i<2 && realms[i]; ++i) {
            DomFixture f=fixture_configured(0,0,&config); os64_js_destroy(f.runtime);
            os64_dom_options_t options=p2b_options("https://streams.example/");
            os64_dom_admission_t a=os64_dom_create_in_realm(cluster,realms[i],f.document,f.state,&options,&out);
            bindings[i]=a.binding; check(a.binding&&a.consumed,"stream realms: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (bindings[0]&&bindings[1]) {
            JSContext *ctx=os64_js_realm_context(realms[0],OS64_JS_ABI_ID,&out);
            JSContext *child=os64_js_realm_context(realms[1],OS64_JS_ABI_ID,&out);
            JSValue global=JS_GetGlobalObject(ctx);
            check(JS_SetPropertyStr(ctx,global,"B",JS_GetGlobalObject(child))>=0,"stream realms: export child");
            JS_FreeValue(ctx,global);
            realm_script(realms[0],STORAGE_ASSERT
                "globalThis.sourceB=new B.ReadableStream({start(c){c.enqueue(7);c.close()}});"
                "globalThis.sourceA=new ReadableStream({start(c){c.enqueue(11);c.close()}});"
                "globalThis.writes=[];globalThis.sinkA=new WritableStream({write(v){writes.push(v)}});"
                "globalThis.sinkB=new B.WritableStream({write(v){writes.push(v)}});"
                "globalThis.sameB=new B.ReadableStream({start(c){c.enqueue(13);c.close()}});"
                "globalThis.sameSinkB=new B.WritableStream({write(v){writes.push(v)}});",
                "stream realms: create sources and sinks with live author callbacks");
            if (retired) {
                check(os64_dom_depart(bindings[1],&out)==OS64_JS_OK,"stream realms: mark child");
                check(os64_dom_depart_finish(bindings[1],&out)==OS64_JS_OK,"stream realms: finish child");
            }
            realm_script(realms[0],
                "globalThis.piped=0;globalThis.pipeErrors=[];"
                "globalThis.pipeB=sourceB.pipeTo(sinkA);assert(pipeB instanceof B.Promise);"
                "pipeB.then(()=>piped++,e=>pipeErrors.push(String(e)));"
                "globalThis.pipeA=sourceA.pipeTo(sinkB);assert(pipeA instanceof Promise);"
                "pipeA.then(()=>piped++,e=>pipeErrors.push(String(e)));",
                "stream realms: pipes start in both directions");
            realm_script(realms[0],"assert(pipeErrors.length===0,pipeErrors.join(';'));"
                "assert(piped===2&&writes.includes(7)&&writes.includes(11),'piped='+piped+' writes='+writes.join(','));",
                "stream realms: mixed-realm pipes settle without brand errors");
            realm_script(realms[0],"sameB.pipeTo(sameSinkB).then(()=>piped++,e=>pipeErrors.push(String(e)));",
                         "stream realms: retained same-realm pipe runs in its own bounded task");
            realm_script(realms[0],"assert(piped===3&&writes.includes(13)&&pipeErrors.length===0);",
                         "stream realms: retained same-realm pipe also settles");
            realm_script(realms[0],
                "function getter(P,k,o){return Object.getOwnPropertyDescriptor(P,k).get.call(o)}"
                "var rs=new B.ReadableStream({start(c){globalThis.rc=c;c.enqueue(23);c.close()}});"
                "assert(getter(ReadableStream.prototype,'locked',rs)===false);"
                "globalThis.reader=ReadableStream.prototype.getReader.call(rs);"
                "assert(reader instanceof ReadableStreamDefaultReader);globalThis.readValue=0;"
                "B.ReadableStreamDefaultReader.prototype.read.call(reader).then(v=>readValue=v.value);"
                "globalThis.ws=new B.WritableStream({write(v){globalThis.wrote=v},start(c){globalThis.wc=c}});"
                "assert(getter(WritableStream.prototype,'locked',ws)===false);"
                "globalThis.writer=WritableStream.prototype.getWriter.call(ws);"
                "B.WritableStreamDefaultWriter.prototype.write.call(writer,29);"
                "assert(getter(B.WritableStreamDefaultWriter.prototype,'desiredSize',writer)!==undefined);"
                "assert(getter(ReadableStreamDefaultController.prototype,'desiredSize',rc)===0);"
                "assert(getter(WritableStreamDefaultController.prototype,'signal',wc));",
                "stream realms: borrowed readers writers and controller getters");
            realm_script(realms[0],"assert(readValue===23&&wrote===29);"
                "B.ReadableStreamDefaultReader.prototype.releaseLock.call(reader);"
                "B.WritableStreamDefaultWriter.prototype.releaseLock.call(writer);",
                "stream realms: borrowed reads writes and lock release settle");
            realm_script(realms[0],
                "globalThis.transform=new B.TransformStream();"
                "assert(getter(TransformStream.prototype,'readable',transform) instanceof B.ReadableStream);"
                "assert(getter(TransformStream.prototype,'writable',transform) instanceof B.WritableStream);"
                "var enc=new B.TextEncoderStream(),dec=new B.TextDecoderStream();"
                "assert(getter(TextEncoderStream.prototype,'encoding',enc)==='utf-8');"
                "assert(getter(TextDecoderStream.prototype,'encoding',dec)==='utf-8');"
                "assert(getter(TextEncoderStream.prototype,'readable',enc) instanceof B.ReadableStream);"
                "assert(getter(TextDecoderStream.prototype,'writable',dec) instanceof B.WritableStream);"
                "for(let h of [0,2,NaN]){let a=new B.ByteLengthQueuingStrategy({highWaterMark:h});"
                "let b=new B.CountQueuingStrategy({highWaterMark:h});"
                "assert(Object.is(getter(ByteLengthQueuingStrategy.prototype,'highWaterMark',a),h));"
                "assert(Object.is(getter(CountQueuingStrategy.prototype,'highWaterMark',b),h));"
                "assert(getter(ByteLengthQueuingStrategy.prototype,'size',a)({byteLength:3})===3);"
                "assert(getter(CountQueuingStrategy.prototype,'size',b)()===1)}",
                "stream realms: transform encoding and queuing strategy brands");
            realm_script(realms[0],
                "var r=new B.ReadableStream({type:'bytes',start(c){globalThis.bc=c;c.enqueue(new Uint8Array([31]));c.close()}});"
                "assert(getter(ReadableByteStreamController.prototype,'desiredSize',bc)===-1);"
                "globalThis.byob=ReadableStream.prototype.getReader.call(r,{mode:'byob'});globalThis.byte=0;"
                "B.ReadableStreamBYOBReader.prototype.read.call(byob,new Uint8Array(1)).then(v=>byte=v.value[0]);"
                "globalThis.it=B.ReadableStream.prototype.values.call(new ReadableStream({start(c){c.enqueue(37);c.close()}}));"
                "globalThis.iterValue=0;Object.getPrototypeOf(new ReadableStream().values()).next.call(it).then(v=>iterValue=v.value);",
                "stream realms: byte readers and async iterator brands");
            realm_script(realms[0],"assert(byte===31&&iterValue===37);"
                "B.ReadableStreamBYOBReader.prototype.releaseLock.call(byob);",
                "stream realms: byte and iterator reads settle");
            realm_script(realms[0],
                "globalThis.requestByte=0;var bs=new B.ReadableStream({type:'bytes',pull(c){"
                "let q=getter(ReadableByteStreamController.prototype,'byobRequest',c);"
                "getter(ReadableStreamBYOBRequest.prototype,'view',q)[0]=41;"
                "ReadableStreamBYOBRequest.prototype.respond.call(q,1);c.close()}});"
                "bs.getReader({mode:'byob'}).read(new Uint8Array(1)).then(v=>requestByte=v.value[0]);"
                "var t=new B.TransformStream({start(c){globalThis.tc=c;c.enqueue(43)}});"
                "assert(getter(TransformStreamDefaultController.prototype,'desiredSize',tc)===-1);"
                "TransformStreamDefaultController.prototype.enqueue.call(tc,44);"
                "globalThis.transformed=0;t.readable.getReader().read().then(v=>transformed=v.value);",
                "stream realms: BYOB request and transform controller brands");
            realm_script(realms[0],"assert(requestByte===41&&transformed===43);",
                         "stream realms: borrowed controller algorithms settle");
            realm_script(realms[0],
                "globalThis.savedThen=B.Promise.prototype.then;globalThis.thenTouches=0;"
                "B.Promise.prototype.then=function(){thenTouches++;throw Error('public then')};"
                "globalThis.patchedDone=0;globalThis.patchedError='';globalThis.patchedWrite=0;"
                "var r=new B.ReadableStream({start(c){c.enqueue(47);c.close()}});"
                "var w=new WritableStream({write(v){patchedWrite=v}});"
                "Promise.prototype.then.call(r.pipeTo(w),()=>patchedDone++,e=>patchedError=String(e));",
                "stream realms: internal adoption ignores patched public promise then");
            realm_script(realms[0],"B.Promise.prototype.then=savedThen;"
                "assert(patchedDone===1&&patchedWrite===47&&thenTouches===0&&patchedError==='',patchedError);",
                "stream realms: patched public then never receives internal continuation");
            realm_script(realms[0],
                "globalThis.bodyTexts=[];"
                "var rb=new B.ReadableStream({start(c){c.enqueue(new Uint8Array([120]));c.close()}});"
                "var ra=new ReadableStream({start(c){c.enqueue(new Uint8Array([121]));c.close()}});"
                "new Response(rb).text().then(t=>bodyTexts.push(t));"
                "new B.Response(ra).text().then(t=>bodyTexts.push(t));",
                "stream realms: native Response accepts foreign readable bodies");
            realm_script(realms[0],"assert(bodyTexts.length===2&&bodyTexts.includes('x')&&bodyTexts.includes('y'));",
                         "stream realms: foreign body conversion retains its owners");
            realm_script(realms[0],
                "globalThis.traps=0;var proxy=new Proxy(new B.ReadableStream(),{get(){traps++;throw Error('trap')},"
                "getPrototypeOf(){traps++;throw Error('trap')}});"
                "throws('TypeError',()=>getter(ReadableStream.prototype,'locked',proxy));"
                "throws('TypeError',()=>getter(ReadableStream.prototype,'locked',Object.create(B.ReadableStream.prototype)));"
                "var revoked=Proxy.revocable(new B.ReadableStream(),{});revoked.revoke();"
                "throws('TypeError',()=>getter(ReadableStream.prototype,'locked',revoked.proxy));assert(traps===0);",
                "stream realms: proxies and forged prototypes do not acquire private brands");
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live==0&&dom_live==0&&!dom_allocations,"stream realms: retained records reclaimed at root teardown");
    }
}
