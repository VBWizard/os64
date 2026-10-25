/* P2b, CLONE, MESSAGE, IDLE (libdom/clone.c, libdom/message.c,
 * libdom/idle.c, MessageEvent in event.c; WINDOW_DOCUMENT.md § P2b): a case
 * per sentence of each file's contract, the bounds a page can size, what a
 * page cannot intercept, the switch off, and every binding and engine
 * allocation of a clone, a message and an idle callback failed in turn. */

#define P2B_URL "https://example.test/dir/page.html"

static uint64_t p2b_ms, p2b_us;
static uint64_t p2b_now_ms(void *opaque) { (void)opaque; return p2b_ms; }
static uint64_t p2b_now_us(void *opaque) { (void)opaque; return p2b_us; }

static os64_dom_options_t p2b_options(const char *url)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.url = url;
    options.now_ms = p2b_now_ms;
    options.now_us = p2b_now_us;
    return options;
}

/* A script of assertions in a block of its own, so each case's names are
 * its own. */
static int p2b_script(DomFixture *f, const char *body, const char *name)
{
    size_t length = strlen(STORAGE_ASSERT) + strlen(body) + 3;
    char *source = malloc(length);
    if (source == NULL) abort();
    snprintf(source, length, "%s{%s}", STORAGE_ASSERT, body);
    int result = dom_script(f, source, name);
    free(source);
    return result;
}

/* Both clocks moved on together. */
static void p2b_advance(uint64_t ms)
{
    p2b_ms += ms;
    p2b_us += ms * 1000;
}

static void dom_clone_value_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "assert(typeof structuredClone==='function'&&structuredClone.length===1&&structuredClone.name==='structuredClone');"
        "throws('TypeError',()=>structuredClone());"
        "for(const v of [undefined,null,true,false,0,-0,NaN,Infinity,1.5,'','\\uD800','a\\u0000b',2n**80n,-3n])"
        "assert(Object.is(structuredClone(v),v),'primitive '+String(v));"
        "const w=[new Boolean(false),new Number(-0),new String('\\uDC00x'),Object(5n)];"
        "const c=structuredClone(w);"
        "assert(c[0] instanceof Boolean&&c[0].valueOf()===false,'Boolean');"
        "assert(c[1] instanceof Number&&Object.is(c[1].valueOf(),-0),'Number');"
        "assert(c[2] instanceof String&&c[2].valueOf()==='\\uDC00x'&&c[2]!==w[2],'String');"
        "assert(typeof c[3]==='object'&&c[3].valueOf()===5n,'BigInt');"
        "const d=new Date(12345),bad=new Date(NaN);"
        "assert(structuredClone(d).getTime()===12345&&structuredClone(d)!==d&&isNaN(structuredClone(bad).getTime()));"
        "const r=/a\\/b/dgimsy;r.lastIndex=3;const rc=structuredClone(r);"
        "assert(rc.source===r.source&&rc.flags===r.flags&&rc.lastIndex===0&&rc!==r,'RegExp '+rc.flags);"
        "assert(structuredClone(/x/v).flags==='v'&&structuredClone(/x/u).unicode===true);",
        "clone: primitives, wrappers, Date and RegExp");
    p2b_script(&f,
        "const b=new ArrayBuffer(8);const u8=new Uint8Array(b);u8.set([1,2,3,4,5,6,7,8]);"
        "const v={a:new Uint8Array(b,2,3),b:new Uint16Array(b,4,2),d:new DataView(b,1,4),buf:b,"
        "f:new Float64Array([1.5]),g:new BigInt64Array([-2n])};"
        "const c=structuredClone(v);"
        "assert(c.buf!==b&&c.buf.byteLength===8&&c.a.buffer===c.buf&&c.b.buffer===c.buf&&c.d.buffer===c.buf,'one buffer');"
        "assert(c.a.byteOffset===2&&c.a.length===3&&c.a[0]===3&&c.b.length===2&&c.b.byteOffset===4,'views');"
        "assert(c.d instanceof DataView&&c.d.byteOffset===1&&c.d.byteLength===4&&c.d.getUint8(0)===2,'DataView');"
        "assert(c.f[0]===1.5&&c.g[0]===-2n,'kinds');"
        "c.a[0]=99;assert(u8[2]===3,'the copy is its own bytes');"
        "const rb=new ArrayBuffer(4,{maxByteLength:16});const rc=structuredClone(rb);"
        "assert(rc.resizable&&rc.maxByteLength===16&&rc.byteLength===4,'resizable');"
        "const e=structuredClone(new ArrayBuffer(0));assert(e.byteLength===0&&!e.resizable);",
        "clone: buffers and views share as they shared");
    p2b_script(&f,
        "const k={k:1},m=new Map([[k,'v'],['x',k],[NaN,-0]]);const s=new Set([3,k,'z']);"
        "const c=structuredClone({m,s});"
        "const keys=[...c.m.keys()];"
        "assert(c.m instanceof Map&&keys.length===3&&keys[0].k===1&&keys[1]==='x'&&Number.isNaN(keys[2]),'Map in order');"
        "assert(c.m.get(keys[0])==='v'&&c.m.get('x')===keys[0],'a key met again is the same copy');"
        "assert(Object.is(c.m.get(NaN),-0),'-0 kept as a value');"
        "const vs=[...c.s];assert(c.s instanceof Set&&vs[0]===3&&vs[1]===keys[0]&&vs[2]==='z','Set in order');",
        "clone: Map and Set");
    p2b_script(&f,
        "for(const E of [Error,EvalError,RangeError,ReferenceError,SyntaxError,TypeError,URIError]){"
        "const e=new E('m',{cause:{c:1}});e.extra=1;const c=structuredClone(e);"
        "assert(c instanceof E&&c.constructor===E&&c.name===E.name&&c.message==='m'&&c!==e,E.name);"
        "assert(c.cause.c===1&&c.extra===undefined,'cause kept, own extras dropped');"
        "assert(!Object.getOwnPropertyDescriptor(c,'message').enumerable&&!Object.getOwnPropertyDescriptor(c,'cause').enumerable);}"
        "const plain=structuredClone(new Error());assert(!plain.hasOwnProperty('message')&&!('cause' in plain));"
        "class Mine extends TypeError{};const m=structuredClone(new Mine('x'));"
        "assert(m.constructor===TypeError&&m.message==='x','a subclass is its native kind');"
        "const odd=new Error('o');odd.name='Custom';assert(structuredClone(odd).name==='Error','another name is Error');"
        "const ag=new AggregateError([],'a');assert(structuredClone(ag).constructor===Error);"
        "let read=0;const g=new Error();Object.defineProperty(g,'name',{get(){read++;return 'RangeError'}});"
        "assert(structuredClone(g) instanceof RangeError&&read===1,'name is read with Get');"
        "const t=new Error();t.message={toString(){return 'made'}};assert(structuredClone(t).message==='made','message is converted');"
        "const acc=new Error('a');Object.defineProperty(acc,'message',{get(){throw 1}});"
        "assert(!structuredClone(acc).hasOwnProperty('message'),'a message that is no data property is none');"
        "const st=new Error('s');st.stack='kept';assert(structuredClone(st).stack==='kept','an own stack is kept');",
        "clone: Error and its kinds");
    p2b_script(&f,
        "const a=[1,,3];a.extra='x';const big=new Array(1e9);"
        "const c=structuredClone(a);assert(Array.isArray(c)&&c.length===3&&!(1 in c)&&c[2]===3&&c.extra==='x','holes and extras');"
        "const bc=structuredClone(big);assert(bc.length===1e9&&Object.keys(bc).length===0,'a long sparse array is cheap');"
        "const o={'0':'z',b:1,length:1};o[Symbol()]=2;Object.defineProperty(o,'hidden',{value:1,enumerable:false});"
        "const oc=structuredClone(o);assert(JSON.stringify(Object.keys(oc))==='[\"0\",\"b\",\"length\"]','own enumerable string keys in order');"
        "assert(!('hidden' in oc)&&Object.getOwnPropertySymbols(oc).length===0);"
        "class P{constructor(){this.x=1}get y(){return 2}};const pc=structuredClone(new P());"
        "assert(pc.constructor===Object&&pc.x===1&&pc.y===undefined,'a class instance is a plain object of its own');"
        "const n=Object.create(null);n.q=1;assert(Object.getPrototypeOf(structuredClone(n))===Object.prototype);"
        "const op=structuredClone(Object.prototype);Object.setPrototypeOf(op,{});assert(op!==Object.prototype);",
        "clone: arrays and objects");
    /* A Map or Set is its entries alone, whether or not it has any; an
     * Array is its every own property (HTML's serialization of each), so
     * an empty one keeps its expandos (Codex #347 round 1). */
    p2b_script(&f,
        "const em=structuredClone({m:Object.assign(new Map(),{extra:42}),after:1});"
        "assert(em.m instanceof Map&&em.m.size===0&&!('extra' in em.m)&&em.after===1,'an empty Map');"
        "const es=structuredClone({s:Object.assign(new Set(),{extra:42}),after:1});"
        "assert(es.s instanceof Set&&es.s.size===0&&!es.s.has(42)&&!('extra' in es.s)&&es.after===1,'an empty Set');",
        "clone: an empty Map or Set with an expando is empty");
    p2b_script(&f,
        "const fm=structuredClone(Object.assign(new Map([[1,2]]),{extra:42}));"
        "assert(fm.size===1&&fm.get(1)===2&&!('extra' in fm),'a Map');"
        "const fs=structuredClone(Object.assign(new Set([7]),{extra:42}));"
        "assert(fs.size===1&&fs.has(7)&&!('extra' in fs),'a Set');",
        "clone: a Map or Set with entries and an expando is its entries");
    p2b_script(&f,
        "const ea=structuredClone(Object.assign([],{extra:42}));"
        "assert(Array.isArray(ea)&&ea.length===0&&ea.extra===42,'an empty Array');",
        "clone: an empty Array keeps its expando");
    p2b_script(&f,
        "const ee=structuredClone(Object.assign(new Error('m',{cause:1}),{extra:42}));"
        "assert(ee.message==='m'&&ee.cause===1&&!('extra' in ee),'an Error');",
        "clone: an Error with an expando is its name, message, stack and cause");
    p2b_script(&f,
        "const order=[];const o={get a(){order.push('a');delete this.c;return 1},b:2,c:3,get d(){order.push('d');return 4}};"
        "const c=structuredClone(o);"
        "assert(order.join()==='a,d'&&c.a===1&&c.b===2&&!('c' in c)&&c.d===4,'getters run in order; a key deleted first is skipped');"
        "const boom=new Error('getter');let caught;try{structuredClone({get x(){throw boom}})}catch(e){caught=e}"
        "assert(caught===boom,'a getter\\'s exception is thrown as it is');"
        "const self={};self.self=self;self.list=[self,self];const sc=structuredClone(self);"
        "assert(sc.self===sc&&sc.list[0]===sc&&sc.list[1]===sc&&sc!==self,'cycles and shared references');"
        "const shared={s:1};const pair=structuredClone([shared,{x:shared}]);assert(pair[0]===pair[1].x);",
        "clone: getters, cycles and shared references");
    p2b_script(&f,
        "function bad(v,what){let e;try{structuredClone(v)}catch(x){e=x}"
        "assert(e instanceof DOMException&&e.name==='DataCloneError'&&e.code===25,what+': '+(e&&e.name));}"
        "bad(Symbol(),'Symbol');bad(()=>1,'function');bad({f(){}},'a function in an object');"
        "bad(new Proxy({},{}),'Proxy');bad(Promise.resolve(),'Promise');bad(new WeakMap(),'WeakMap');"
        "bad(Object(Symbol()),'Symbol object');bad((function(){return arguments})(),'arguments');"
        "bad(document,'document');bad(document.body,'a node');bad(window,'window');bad(location,'location');"
        "bad(history,'history');bad(navigator,'navigator');bad(new Event('x'),'an Event');"
        "bad(new Response(),'a Response');bad(document.body.style,'a declaration');"
        "const d=new ArrayBuffer(4),over=new Uint8Array(d),view=new DataView(d);structuredClone(d,{transfer:[d]});"
        "bad(d,'a detached buffer');bad(over,'a typed array of a detached buffer');bad(view,'a DataView of one');"
        "const rb=new ArrayBuffer(16,{maxByteLength:32});const oob=new Uint8Array(rb,8);const dv=new DataView(rb,8);"
        "rb.resize(0);bad(oob,'a typed array out of bounds');bad(dv,'a DataView out of bounds');"
        "if(typeof SharedArrayBuffer==='function')bad(new SharedArrayBuffer(4),'SharedArrayBuffer');",
        "clone: DataCloneError for the rest");
    p2b_script(&f,
        "const blob=new Blob(['ab\\u0000c'],{type:'Text/Plain'});const bc=structuredClone(blob);"
        "assert(bc instanceof Blob&&!(bc instanceof File)&&bc!==blob&&bc.size===4&&bc.type==='text/plain','Blob');"
        "const file=new File(['xyz'],'n\\u00e9.txt',{type:'a/b',lastModified:42});const fc=structuredClone(file);"
        "assert(fc instanceof File&&fc.name==='n\\u00e9.txt'&&fc.lastModified===42&&fc.size===3&&fc.type==='a/b','File');"
        "class Sub extends File{};assert(Object.getPrototypeOf(structuredClone(new Sub([],'s')))===File.prototype);"
        "const BlobI=Blob;delete globalThis.Blob;const after=structuredClone(blob);globalThis.Blob=BlobI;"
        "assert(after instanceof BlobI,'an interface deleted from the global still deserializes');"
        "const ex=new DOMException('msg','NotFoundError');const xc=structuredClone(ex);"
        "assert(xc instanceof DOMException&&xc.name==='NotFoundError'&&xc.message==='msg'&&xc.code===8&&xc!==ex,'DOMException');",
        "clone: Blob, File and DOMException");
    dom_script(&f, "globalThis.blobText=null;structuredClone(new Blob(['hey'])).text().then(t=>{blobText=t});",
               "clone: a cloned Blob's bytes, read");
    p2b_script(&f, "assert(blobText==='hey',String(blobText));", "clone: are the original's");
    p2b_script(&f,
        "const b=new ArrayBuffer(4);new Uint8Array(b).set([9,8,7,6]);const v=new Uint8Array(b,1,2);"
        "const c=structuredClone({v,b},{transfer:[b]});"
        "assert(b.byteLength===0&&v.length===0,'the buffer is detached');"
        "assert(c.b.byteLength===4&&c.v.buffer===c.b&&c.v[0]===8,'the copy has its bytes');"
        "const lone=new ArrayBuffer(2);const lc=structuredClone(1,{transfer:[lone]});assert(lc===1&&lone.byteLength===0,'a buffer named only in the list is detached');"
        "const rb=new ArrayBuffer(2,{maxByteLength:8});const rc=structuredClone(rb,{transfer:[rb]});"
        "assert(rc.resizable&&rc.maxByteLength===8&&rb.detached,'a resizable buffer is transferred resizable');"
        "function bad(f,what){let e;try{f()}catch(x){e=x}assert(e&&e.name==='DataCloneError',what+': '+(e&&e.name));}"
        "const twice=new ArrayBuffer(1);bad(()=>structuredClone(0,{transfer:[twice,twice]}),'twice');"
        "assert(twice.byteLength===1,'a refused list detaches nothing');"
        "bad(()=>structuredClone(0,{transfer:[new Uint8Array(1)]}),'a view');bad(()=>structuredClone(0,{transfer:[{}]}),'an object');"
        "const gone=new ArrayBuffer(1);structuredClone(0,{transfer:[gone]});"
        "const kept=new ArrayBuffer(3);bad(()=>structuredClone(0,{transfer:[kept,gone]}),'a detached buffer');"
        "assert(kept.byteLength===3,'nothing is detached when one of them refuses');"
        "throws('TypeError',()=>structuredClone(0,{transfer:[1]}));throws('TypeError',()=>structuredClone(0,{transfer:1}));"
        "throws('TypeError',()=>structuredClone(0,7));"
        "assert(structuredClone(2,null)===2&&structuredClone(2,{transfer:undefined})===2);"
        "let sideEffect=0;const fresh=new ArrayBuffer(1);bad(()=>structuredClone({get x(){sideEffect++;return Symbol()}},{transfer:[fresh]}),'a refused value');"
        "assert(sideEffect===1&&fresh.byteLength===1,'a value that refuses detaches nothing');",
        "clone: transfer");
    fixture_free(&f);
}

/* What a page cannot reach: the intrinsics the clone calls are the ones the
 * engine made, and its copies are filled by definition, not assignment. */
static void dom_clone_hostile_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "const value={x:1,a:[5],m:new Map([[1,2]]),s:new Set([3]),r:/q/g,n:new Number(4),d:new Date(7),"
        "e:new RangeError('r'),b:new ArrayBuffer(2,{maxByteLength:4})};"
        "const hits=[];const trap=n=>function(){hits.push(n);throw Error(n)};"
        "Map.prototype.set=trap('set');Map.prototype.forEach=trap('forEach');Map.prototype[Symbol.iterator]=trap('iter');"
        "Set.prototype.add=trap('add');Set.prototype.forEach=trap('sforEach');"
        "for(const n of ['flags','source','global'])Object.defineProperty(RegExp.prototype,n,{get:trap(n)});"
        "Object.defineProperty(ArrayBuffer.prototype,'maxByteLength',{get:trap('max')});"
        "Object.defineProperty(Object.prototype,'x',{set:trap('x'),configurable:true});"
        "Object.defineProperty(Object.prototype,'maxByteLength',{get:trap('options'),configurable:true});"
        "Object.defineProperty(Array.prototype,'0',{set:trap('0'),configurable:true});"
        "Number.prototype.valueOf=trap('valueOf');Date.prototype.getTime=trap('getTime');"
        "const MapI=Map,SetI=Set,RegExpI=RegExp,RangeI=RangeError;"
        "globalThis.Map=globalThis.Set=globalThis.RegExp=globalThis.RangeError=globalThis.Object=null;"
        "const c=structuredClone(value);"
        "if(hits.length)throw Error('the page was reached: '+hits.join());"
        "if(c.x!==1||c.a[0]!==5||!(c.m instanceof MapI)||!(c.s instanceof SetI)||!(c.r instanceof RegExpI))throw Error('the copy');"
        "if(!(c.e instanceof RangeI)||c.b.byteLength!==2)throw Error('the copy, again');",
        "clone: no page code it replaced runs");
    fixture_free(&f);

    /* A getter that re-enters the clone, posts a message, detaches a
     * buffer in the transfer list, or grows a buffer already written. */
    options = p2b_options(P2B_URL);
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "let n=0;const o={get a(){return n++<40?structuredClone(o):'end'}};"
        "let c=structuredClone(o),depth=0;while(typeof c.a==='object'){c=c.a;depth++}"
        "assert(depth===40&&c.a==='end','a clone inside a getter is a clone of its own: '+depth);"
        "let e;const forever={get a(){return structuredClone(forever)}};try{structuredClone(forever)}catch(x){e=x}"
        "assert(e instanceof RangeError||e instanceof InternalError,'endless re-entry ends as the engine ends recursion: '+e);"
        "globalThis.order=[];addEventListener('message',m=>order.push(m.data.tag));"
        "postMessage({tag:'outer',get x(){postMessage({tag:'inner'},'*');return 1}},'*');"
        "const buf=new ArrayBuffer(4);throws('DataCloneError',()=>structuredClone({get x(){structuredClone(buf,{transfer:[buf]});return 1}},{transfer:[buf]}));"
        /* A view past the bytes its buffer had when the buffer was written
         * is cloned as it was: out of bounds over the buffer as it was. */
        "const rb=new ArrayBuffer(8,{maxByteLength:64});new Uint8Array(rb).set([1,2,3,4,5,6,7,8]);"
        "const grown={v:new Uint8Array(rb,0,4),get g(){rb.resize(64);return 0},get late(){return new Uint8Array(rb,32,8)}};"
        "const gc=structuredClone(grown);"
        "assert(gc.v.buffer===gc.late.buffer&&gc.v.buffer.byteLength===8&&gc.v.buffer.maxByteLength===64,'one buffer, as it was written');"
        "assert(gc.v[3]===4&&gc.late.length===0&&gc.late.byteOffset===0,'the late view out of bounds');"
        "gc.v.buffer.resize(40);assert(gc.late.length===8&&gc.late.byteOffset===32,'and in bounds when its buffer grows');"
        /* The cold read's two: a view of a transferred buffer the getter
         * shrank, and one of a buffer it grew. */
        "const b1=new ArrayBuffer(8,{maxByteLength:16});"
        "const c1=structuredClone({a:new Uint8Array(b1,4,4),get g(){b1.resize(2);return 1}},{transfer:[b1]});"
        "assert(b1.detached&&c1.a.buffer.byteLength===2&&c1.a.length===0,'shrunk, then transferred');"
        "c1.a.buffer.resize(8);assert(c1.a.length===4);"
        "const b2=new ArrayBuffer(4,{maxByteLength:16});"
        "const c2=structuredClone({a:new Uint8Array(b2,0,4),get g(){b2.resize(16);return 0},get c(){return new Uint8Array(b2,8,8)}});"
        "assert(c2.a.length===4&&c2.c.length===0&&c2.a.buffer===c2.c.buffer&&c2.a.buffer.byteLength===4,'grown after it was written');"
        "const b3=new ArrayBuffer(4,{maxByteLength:16});"
        "const dv=structuredClone({a:new Uint8Array(b3,0,2),get g(){b3.resize(16);return 0},get d(){return new DataView(b3,12,4)}});"
        "assert(dv.d instanceof DataView&&dv.a.buffer===dv.d.buffer&&dv.a.buffer.byteLength===4,'a DataView too');"
        "throws('TypeError',()=>dv.d.getUint8(0));dv.a.buffer.resize(16);assert(dv.d.byteLength===4);"
        /* Nothing is detached unless the whole clone succeeds. */
        "const kept=new ArrayBuffer(4);throws('DataCloneError',()=>structuredClone([kept,()=>1],{transfer:[kept]}));"
        "assert(kept.byteLength===4,'a refusal after the transferred buffer was written detaches nothing');"
        "const kept2=new ArrayBuffer(4);throws('DataCloneError',()=>postMessage([kept2,Symbol()],'*',[kept2]));"
        "assert(kept2.byteLength===4,'nor does a post that refuses');",
        "clone: getters that reach back in");
    os64_js_outcome_t outcome;
    check(os64_dom_message_fire(f.binding, &outcome) && os64_dom_message_fire(f.binding, &outcome),
          "clone: both messages run");
    p2b_script(&f, "assert(order.join()==='inner,outer',order.join());",
               "clone: the one posted while the other was being cloned was queued first");
    fixture_free(&f);
}

/* A fixture whose engine has room for a large graph and its copy. */
static DomFixture p2b_large_fixture(const os64_dom_options_t *options)
{
    DomFixture f = fixture_base(0, 0, 128 * 1024 * 1024);
    os64_js_outcome_t outcome;
    if (f.runtime != NULL && f.state != NULL)
        f.binding = os64_dom_create(f.runtime, f.document, f.state, options, &outcome);
    return f;
}

/* NEVER SWALLOW A RUNTIME'S FAILURE (internal.h): a page that loops on a
 * binding call until its task's deadline is stopped there, however the
 * interrupt reaches the binding. Nearly every engine call such a loop
 * makes is the binding's (a DataView's getters, three a view), so the
 * interrupt lands in one of them; it must leave the binding uncatchable,
 * so the page's catch never runs. A catch that runs makes an 8 MiB buffer,
 * which the test port counts (large_asks): a failed runtime refuses every
 * binding member and all output, but not the engine's own allocation. */
static void dom_clone_interrupt_cases(void)
{
    static const struct { const char *loop, *what; } kLoops[] = {
        {"for(;;){try{structuredClone(views)}catch(e){globalThis.heard=new ArrayBuffer(8<<20);break}}",
         "clone: an interrupt in a DataView's getter ends the task, no catch running"},
        {"const dec=new TextDecoder();for(;;){try{for(const v of views)dec.decode(v)}"
         "catch(e){globalThis.heard=new ArrayBuffer(8<<20);break}}",
         "clone: an interrupt in a decoded DataView's getter ends the task, no catch running"},
    };
    for (size_t i = 0; i < sizeof(kLoops) / sizeof(kLoops[0]); i++) {
        os64_dom_options_t options = p2b_options(P2B_URL);
        DomFixture f = p2b_large_fixture(&options);
        if (!fixture_ready(&f)) return;
        dom_script(&f, "const b=new ArrayBuffer(1);globalThis.views=Array.from({length:20000},()=>new DataView(b));",
                   "clone: twenty thousand views of one buffer");
        /* The host's clock steps a tenth of a second at every read, so the
         * one-second deadline falls a few interrupt checks in. */
        size_t before = large_asks;
        dom_clock_step = 100000;
        os64_js_outcome_t outcome;
        os64_js_run(f.runtime, kLoops[i].loop, strlen(kLoops[i].loop), "interrupt.js", &outcome);
        dom_clock_step = 0;
        check(outcome.status == OS64_JS_LIMIT && outcome.limit == OS64_JS_LIMIT_EXECUTION && large_asks == before,
              kLoops[i].what);
        fixture_free(&f);
    }
}

/* What a page can size: a graph thirty thousand objects deep both ways
 * (never the C stack's: a recursive walk's frames would be megabytes), a
 * Map of thirty thousand entries and as wide an array, each a linear walk:
 * timed against half their count, the work doubles, never quadruples. The
 * sizes keep the sanitized profile well inside the fixture's one second a
 * script. */
static void dom_clone_bound_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    for (int scale = 1; scale <= 2; scale++) {
        DomFixture f = p2b_large_fixture(&options);
        if (!fixture_ready(&f)) return;
        char source[1024];
        int n = 15000 * scale;
        snprintf(source, sizeof(source), STORAGE_ASSERT
                 "const N=%d;let head={n:0};const first=head;for(let i=1;i<N;i++){head={n:i,next:head}};first.loop=head;"
                 "const m=new Map();for(let i=0;i<N;i++)m.set(i,{i});const arr=[];for(let i=0;i<N;i++)arr.push([i]);"
                 "const t=performance.now();"
                 "const c=structuredClone(head);let at=c,count=0;while(at.next){at=at.next;count++}"
                 "assert(count===N-1&&at.n===0&&at.loop===c,'deep: '+count);"
                 "const mc=structuredClone(m);assert(mc.size===N&&mc.get(N-1).i===N-1,'a large Map');"
                 "assert(structuredClone(arr)[N-1][0]===N-1,'a wide array');",
                 n);
        clock_t began = clock();
        dom_script(&f, source, "clone: deep and wide graphs");
        double seconds = (double)(clock() - began) / CLOCKS_PER_SEC;
        printf("clone bound: %d of each in %.3f s\n", n, seconds);
        static double first;
        if (scale == 1) first = seconds;
        else check(seconds < first * 4 + 0.05, "clone: twice the graph is about twice the work, not four times");
        fixture_free(&f);
    }
}

/* ── postMessage ────────────────────────────────────────────────────── */

/* The window's next message as the host runs one. */
static bool p2b_message(DomFixture *f, os64_js_outcome_t *outcome)
{
    return os64_dom_message_fire(f->binding, outcome);
}

static void dom_message_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    p2b_ms = 500;
    check(os64_dom_message_next(f.binding) == UINT64_MAX && !p2b_message(&f, &outcome),
          "message: none queued, none run");
    p2b_script(&f,
        "assert(typeof postMessage==='function'&&postMessage.length===1&&onmessage===null&&onmessageerror===null);"
        "globalThis.got=[];const data={a:[1]};globalThis.sent=data;"
        "globalThis.listener=e=>got.push(e);addEventListener('message',listener);"
        "assert(postMessage(data)===undefined&&got.length===0,'nothing runs at once');",
        "message: posted");
    check(os64_dom_message_next(f.binding) == 500, "message: next is when it was posted");
    check(p2b_message(&f, &outcome) && outcome.status == OS64_JS_OK, "message: its task runs");
    check(os64_dom_message_next(f.binding) == UINT64_MAX, "message: and the queue is empty");
    p2b_script(&f,
        "assert(got.length===1);const e=got[0];"
        "assert(e instanceof MessageEvent&&e.type==='message'&&e.isTrusted&&!e.bubbles&&!e.cancelable,'the event');"
        "assert(e.data.a[0]===1&&e.data!==sent,'its data is a copy');"
        "assert(e.origin==='https://example.test'&&e.lastEventId===''&&e.source===window&&e.target===window,'its fields');"
        "assert(Array.isArray(e.ports)&&e.ports.length===0&&Object.isFrozen(e.ports)&&e.ports===e.ports,'its ports');"
        "removeEventListener('message',listener);",
        "message: a MessageEvent with a copy");
    /* A message posted in a message's task is a task of its own. */
    dom_script(&f, "got.length=0;onmessage=e=>{got.push('h'+e.data);if(e.data<3)postMessage(e.data+1)};postMessage(1);",
               "message: one that posts another");
    check(p2b_message(&f, &outcome), "message: the first task");
    p2b_script(&f, "assert(got.join()==='h1',got.join());", "message: ran only the first");
    check(p2b_message(&f, &outcome) && p2b_message(&f, &outcome) && !p2b_message(&f, &outcome),
          "message: each later one a task, then none");
    p2b_script(&f, "assert(got.join()==='h1,h2,h3',got.join());onmessage=null;got.length=0;",
               "message: in order");
    /* targetOrigin. */
    p2b_script(&f,
        "addEventListener('message',listener);postMessage('star','*');postMessage('slash','/');postMessage('same','https://example.test/other/x?y');"
        "postMessage('port','https://example.test:443/');postMessage('other','https://other.test/');"
        "postMessage('http','http://example.test/');postMessage('opaque','data:,x');"
        "postMessage('dict',{targetOrigin:'*'});postMessage('none',undefined);postMessage('null',null);"
        "let read=0;const g={get x(){read++;return 1}};"
        "throws('SyntaxError',()=>postMessage(g,'example.org'));throws('SyntaxError',()=>postMessage(g,'http://foo bar'));"
        "throws('SyntaxError',()=>postMessage(g,5));throws('SyntaxError',()=>postMessage(g,{targetOrigin:''}));"
        "assert(read===0,'a bad targetOrigin refuses before the message is read');"
        "throws('DataCloneError',()=>postMessage(()=>1,'*'));throws('TypeError',()=>postMessage());"
        "throws('TypeError',()=>postMessage(1,'*',null));throws('TypeError',()=>postMessage(1,{transfer:[2]}));",
        "message: targetOrigin");
    while (p2b_message(&f, &outcome)) {}
    p2b_script(&f,
        "const seen=got.map(e=>e.data).join();assert(seen==='star,slash,same,port,dict,none,null',seen);got.length=0;",
        "message: delivered to its origin only");
    /* Transfer: the buffer is detached as it is posted. */
    p2b_script(&f,
        "const b=new ArrayBuffer(3);new Uint8Array(b)[1]=7;postMessage({b},'*',[b]);assert(b.byteLength===0,'detached at once');"
        "const c=new ArrayBuffer(2);postMessage(c,{transfer:[c]});assert(c.byteLength===0);",
        "message: transfer");
    check(p2b_message(&f, &outcome) && p2b_message(&f, &outcome), "message: both delivered");
    p2b_script(&f,
        "assert(new Uint8Array(got[0].data.b)[1]===7&&got[1].data.byteLength===2,'the bytes arrived');got.length=0;"
        "removeEventListener('message',listener);",
        "message: with their bytes");
    /* The handler property, the body's attribute, and nothing listening. */
    dom_script(&f, "globalThis.heard=[];document.body.setAttribute('onmessage','heard.push(event.data)');"
                   "postMessage('attr','*');", "message: the body's attribute");
    check(p2b_message(&f, &outcome), "message: heard by the body's handler");
    p2b_script(&f,
        "assert(heard.join()==='attr'&&typeof onmessage==='function','the body\\'s onmessage is window\\'s');"
        "onmessage=null;document.body.removeAttribute('onmessage');",
        "message: the handler is window's");
    fixture_free(&f);

    options = p2b_options(P2B_URL);
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    size_t registry = os64_dom_registry_count(f.binding);
    dom_script(&f, "postMessage({big:new Array(100).fill('x')},'*');", "message: nobody listens");
    check(os64_dom_registry_count(f.binding) == registry + 1, "message: a queued message holds its poster");
    check(!p2b_message(&f, &outcome) && os64_dom_message_next(f.binding) == UINT64_MAX &&
          os64_dom_registry_count(f.binding) == registry,
          "message: one nothing could hear is dropped without a task");
    /* Two nobody hears yet, a timer between them that adds a listener: the
     * first is dropped, and the second waits for the timer. */
    p2b_ms = 100;
    dom_script(&f, "globalThis.logged=[];postMessage(1,'*');"
                   "setTimeout(()=>addEventListener('message',e=>logged.push(e.data)),5);", "message: a timer between");
    p2b_ms = 110;
    dom_script(&f, "postMessage(2,'*');", "message: posted after the timer fell due");
    check(!p2b_message(&f, &outcome) && os64_dom_message_next(f.binding) == 110,
          "message: the first dropped, the second left for the timer queued before it");
    check(os64_dom_timer_fire(f.binding, p2b_ms, &outcome) && p2b_message(&f, &outcome),
          "message: the timer, then the message, heard");
    p2b_script(&f, "assert(logged.join()==='2',logged.join());", "message: the listener the timer added heard it");
    dom_script(&f, "postMessage(1,'*');postMessage(2,'*');addEventListener('messageerror',()=>{});",
               "message: left queued");
    /* fixture_free drains the queue: the registry is checked empty there. */
    fixture_free(&f);

    /* A page with no address: its origin is opaque, and "/" is its own. */
    options = p2b_options(NULL);
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.got=[];onmessage=e=>got.push(e.origin+':'+e.data);"
                   "postMessage('a');postMessage('b','https://example.test');postMessage('c','*');",
               "message: from an opaque origin");
    while (p2b_message(&f, &outcome)) {}
    p2b_script(&f, "assert(got.join()==='null:a,null:c',got.join());", "message: its origin is null");
    fixture_free(&f);

    /* The switch off: none of it. */
    options = p2b_options(P2B_URL);
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "assert(!('postMessage' in window)&&!('onmessage' in window)&&!('MessageEvent' in window));"
        "assert(!('structuredClone' in window)&&!('requestIdleCallback' in window)&&!('IdleDeadline' in window));"
        "document.body.setAttribute('onmessage','1');assert(!('onmessage' in window));"
        "assert(Object.getPrototypeOf(document.createEvent('MessageEvent'))===Event.prototype);",
        "message: the switch off");
    check(os64_dom_message_next(f.binding) == UINT64_MAX && os64_dom_idle_next(f.binding) == UINT64_MAX,
          "message: the switch off queues nothing");
    fixture_free(&f);
}

static void dom_message_event_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    p2b_script(&f,
        "const d=new MessageEvent('m');"
        "assert(d.data===null&&d.origin===''&&d.lastEventId===''&&d.source===null&&d.ports.length===0&&!d.isTrusted);"
        "assert(Object.isFrozen(d.ports)&&d.ports===d.ports&&MessageEvent.length===1&&d instanceof Event);"
        "const order=[];const init={};for(const n of ['bubbles','cancelable','data','lastEventId','origin','ports','source'])"
        "Object.defineProperty(init,n,{get(){order.push(n);return n==='ports'?[]:n==='source'?window:n==='bubbles'||n==='cancelable'?true:n}});"
        "const e=new MessageEvent('x',init);"
        "assert(order.join()==='bubbles,cancelable,data,lastEventId,origin,ports,source',order.join());"
        "assert(e.bubbles&&e.data==='data'&&e.lastEventId==='lastEventId'&&e.origin==='origin'&&e.source===window);"
        "assert(new MessageEvent('o',{origin:'\\uD800'}).origin==='\\uFFFD','origin is a USVString');"
        "throws('TypeError',()=>new MessageEvent('x',{ports:[1]}));throws('TypeError',()=>new MessageEvent('x',{source:{}}));"
        "throws('TypeError',()=>new MessageEvent('x',{ports:1}));throws('TypeError',()=>new MessageEvent());"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(MessageEvent.prototype,'data').get.call(new Event('x')));"
        "class Mine extends MessageEvent{};assert(new Mine('m') instanceof Mine);"
        "const c=document.createEvent('MessageEvent');assert(c instanceof MessageEvent&&c.data===null&&c.ports.length===0);"
        "c.initMessageEvent('t',true,false,{v:1},'o','id',window,[]);"
        "assert(c.type==='t'&&c.bubbles&&!c.cancelable&&c.data.v===1&&c.origin==='o'&&c.lastEventId==='id'&&c.source===window);"
        "throws('TypeError',()=>c.initMessageEvent('t',false,false,1,'','',null,[{}]));"
        "const fresh=document.createEvent('MessageEvent');"
        "throws('TypeError',()=>fresh.initMessageEvent('changed',true,true,2,'o','l',null,[{}]));"
        "assert(fresh.type===''&&!fresh.bubbles&&fresh.data===null,'a throwing initMessageEvent leaves the event untouched');"
        "throws('TypeError',()=>fresh.initMessageEvent('changed',true,true,2,'o','l',{}));"
        "assert(fresh.type==='','nor does a source that is no window');"
        "let during;addEventListener('t',ev=>{ev.initMessageEvent('changed');during=ev.type});dispatchEvent(c);"
        "assert(during==='t','initMessageEvent does nothing during a dispatch');",
        "message: MessageEvent's interface");
    fixture_free(&f);
}

/* ── requestIdleCallback ────────────────────────────────────────────── */

static bool p2b_idle(DomFixture *f, os64_js_outcome_t *outcome)
{
    return os64_dom_idle_run(f->binding, outcome);
}

static void dom_idle_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    p2b_ms = 1000;
    p2b_us = 1000000;
    check(os64_dom_idle_next(f.binding) == UINT64_MAX && !p2b_idle(&f, &outcome), "idle: none asked for, none run");
    p2b_script(&f,
        "assert(requestIdleCallback.length===1&&cancelIdleCallback.length===1);"
        "globalThis.log=[];globalThis.kept=null;"
        "const one=requestIdleCallback(function(d){log.push('a:'+(this===window)+':'+arguments.length);kept=d});"
        "const two=requestIdleCallback(()=>log.push('b'));const three=requestIdleCallback(()=>log.push('c'));"
        "assert(typeof one==='number'&&two===one+1&&three===two+1&&log.length===0,'handles, and nothing at once');"
        "cancelIdleCallback(two);cancelIdleCallback(9999);assert(cancelIdleCallback(three+5)===undefined);"
        "throws('TypeError',()=>requestIdleCallback());throws('TypeError',()=>requestIdleCallback(1));"
        "throws('TypeError',()=>requestIdleCallback(()=>{},5));throws('TypeError',()=>cancelIdleCallback());",
        "idle: asked for");
    check(os64_dom_idle_next(f.binding) == 0, "idle: one may run now");
    p2b_us += 1000;
    check(p2b_idle(&f, &outcome) && outcome.status == OS64_JS_OK, "idle: the first runs as a task");
    p2b_script(&f,
        "assert(log.join()==='a:true:1',log.join());"
        "assert(kept instanceof IdleDeadline&&kept.didTimeout===false,'its IdleDeadline');"
        "const left=kept.timeRemaining();assert(left>0&&left<=50,'within the period: '+left);"
        "throws('TypeError',()=>new IdleDeadline());throws('TypeError',()=>IdleDeadline.prototype.timeRemaining.call({}));",
        "idle: one argument, this window");
    check(p2b_idle(&f, &outcome) && !p2b_idle(&f, &outcome), "idle: the cancelled one is skipped, the third runs");
    p2b_script(&f, "assert(log.join()==='a:true:1,c',log.join());log.length=0;", "idle: in order");
    check(os64_dom_idle_next(f.binding) == UINT64_MAX, "idle: none left");

    /* One a callback asks for waits for the next period, 50 ms on. */
    p2b_advance(100);
    dom_script(&f, "let n=0;function again(d){log.push(n++);if(n<3)requestIdleCallback(again)}requestIdleCallback(again);",
               "idle: one that asks for itself again");
    uint64_t began = p2b_ms;
    check(p2b_idle(&f, &outcome), "idle: it runs");
    check(os64_dom_idle_next(f.binding) == began + 50 && !p2b_idle(&f, &outcome),
          "idle: again only in the next period, 50 ms after this began");
    p2b_advance(49);
    check(!p2b_idle(&f, &outcome), "idle: not before");
    p2b_advance(1);
    check(p2b_idle(&f, &outcome), "idle: then");
    p2b_advance(50);
    check(p2b_idle(&f, &outcome) && os64_dom_idle_next(f.binding) == UINT64_MAX, "idle: three periods, three runs");
    p2b_script(&f, "assert(log.join()==='0,1,2',log.join());log.length=0;", "idle: each in its period");

    /* A request made outside a period's callback lets the next begin at once. */
    dom_script(&f, "requestIdleCallback(()=>{log.push('x');setTimeout(()=>{},0)});", "idle: asked by a script");
    check(os64_dom_idle_next(f.binding) == 0 && p2b_idle(&f, &outcome), "idle: runs at once");
    check(!p2b_idle(&f, &outcome) && os64_dom_timer_fire(f.binding, p2b_ms, &outcome), "idle: the timer it set first");

    /* The page's own work comes first: a due timer, a queued message. */
    dom_script(&f, "requestIdleCallback(()=>log.push('late'));setTimeout(()=>log.push('timer'),0);postMessage(1,'*');",
               "idle: with work queued");
    check(!p2b_idle(&f, &outcome), "idle: not while a timer is due");
    check(os64_dom_timer_fire(f.binding, p2b_ms, &outcome), "idle: the timer");
    check(!p2b_idle(&f, &outcome), "idle: not while a message is queued");
    check(!os64_dom_message_fire(f.binding, &outcome), "idle: the message (nobody hears it)");
    check(p2b_idle(&f, &outcome), "idle: then the idle callback");
    p2b_script(&f, "assert(log.join()==='x,timer,late',log.join());log.length=0;", "idle: after the page's tasks");

    /* The deadline: 50 ms, or the next timer, asked at each read. */
    dom_script(&f, "requestIdleCallback(d=>{const before=d.timeRemaining();setTimeout(()=>{},10);"
                   "log.push(before<=50&&d.timeRemaining()<=10?'shorter':'not '+before+' '+d.timeRemaining());"
                   "globalThis.kept=d})", "idle: a timer shortens the deadline");
    p2b_advance(100);
    check(p2b_idle(&f, &outcome), "idle: ran");
    p2b_advance(100);
    p2b_script(&f, "assert(log.join()==='shorter',log.join());assert(kept.timeRemaining()===0,'over');"
               "log.length=0;", "idle: and the deadline passes");
    check(os64_dom_timer_fire(f.binding, p2b_ms, &outcome), "idle: the timer it set");

    /* A period's deadline passed: what is left waits for the next. */
    dom_script(&f, "requestIdleCallback(()=>{log.push('1');globalThis.spend=true});requestIdleCallback(()=>log.push('2'));",
               "idle: two in one period");
    p2b_advance(100);
    check(p2b_idle(&f, &outcome), "idle: the first");
    p2b_us += 60000;
    check(os64_dom_idle_next(f.binding) == 0 && p2b_idle(&f, &outcome), "idle: past the deadline, a new period at once");
    p2b_script(&f, "assert(log.join()==='1,2',log.join());log.length=0;", "idle: both ran");

    /* timeout: a timer of the window's calls it when no period did. */
    dom_script(&f, "globalThis.timed=null;const h=requestIdleCallback(d=>{timed=[d.didTimeout,d.timeRemaining()]},{timeout:100});"
                   "requestIdleCallback(()=>log.push('ran'),{timeout:30});", "idle: with timeouts");
    check(os64_dom_timer_next(f.binding) == p2b_ms + 30, "idle: its timeout is a timer");
    check(p2b_idle(&f, &outcome) == true, "idle: the first runs in a period");
    p2b_script(&f, "assert(timed[0]===false,'not timed out');timed=null;", "idle: not timed out");
    check(os64_dom_timer_next(f.binding) == p2b_ms + 30, "idle: which cancelled its own timer");
    p2b_advance(30);
    check(os64_dom_timer_fire(f.binding, p2b_ms, &outcome) && outcome.status == OS64_JS_OK, "idle: the other's timeout");
    p2b_script(&f, "assert(log.join()==='ran',log.join());log.length=0;", "idle: called by its timeout");
    check(os64_dom_idle_next(f.binding) == UINT64_MAX && !p2b_idle(&f, &outcome), "idle: and no period calls it again");
    dom_script(&f, "requestIdleCallback(d=>{timed=[d.didTimeout,d.timeRemaining()]},{timeout:20});"
                   "requestIdleCallback(()=>{},{timeout:0});", "idle: timed out with a deadline");
    p2b_advance(20);
    check(os64_dom_timer_fire(f.binding, p2b_ms, &outcome), "idle: the timeout");
    p2b_script(&f, "assert(timed[0]===true&&timed[1]===0,String(timed));", "idle: didTimeout, 0 left");
    check(p2b_idle(&f, &outcome) && os64_dom_timer_next(f.binding) == UINT64_MAX,
          "idle: a timeout of 0 is no timeout");
    /* A page cannot cancel a timer the binding set for itself by guessing
     * its id, which is drawn from the page's count (the cold read's
     * idle2.html, and AbortSignal.timeout's the same way). */
    dom_script(&f, "globalThis.timed=null;requestIdleCallback(d=>{timed=d.didTimeout},{timeout:10});"
                   "globalThis.signal=AbortSignal.timeout(10);"
                   "const t=setTimeout(()=>{},1000);clearTimeout(t-1);clearTimeout(t-2);clearInterval(t-1);clearTimeout(t);",
               "idle: clearTimeout of the ids before a setTimeout's");
    p2b_advance(10);
    while (os64_dom_timer_fire(f.binding, p2b_ms, &outcome)) {}
    p2b_script(&f, "assert(timed===true,'the idle timeout fired: '+timed);assert(signal.aborted,'and the signal\\'s');",
               "idle: neither binding timer was cancelled");
    fixture_free(&f);
}

static void dom_idle_failure_cases(void)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    p2b_ms = 0;
    p2b_us = 0;
    dom_script(&f, "globalThis.errors=[];onerror=m=>{errors.push(m)};"
                   "requestIdleCallback(()=>{throw Error('idle')});requestIdleCallback(()=>errors.push('next'));"
                   "requestIdleCallback(()=>{throw Error('timed')},{timeout:5});", "idle: callbacks that throw");
    check(p2b_idle(&f, &outcome) && outcome.status == OS64_JS_EXCEPTION && strstr(outcome.message, "idle") != NULL,
          "idle: a callback's exception is its task's failure");
    check(strstr(outcome.source_name, "#idle-1") != NULL, "idle: its task is named for its handle");
    check(p2b_idle(&f, &outcome) && outcome.status == OS64_JS_OK, "idle: and the next still runs");
    p2b_advance(5);
    check(os64_dom_timer_fire(f.binding, p2b_ms, &outcome) && outcome.status != OS64_JS_OK,
          "idle: a timed-out callback's exception is its timer task's failure");
    p2b_script(&f,
        "assert(errors.join('|')==='Uncaught Error: idle|next|Uncaught Error: timed',errors.join('|'));",
        "idle: each heard by window.onerror");
    /* Cancelling the one running, and every hole closed up. */
    dom_script(&f, "globalThis.mine=requestIdleCallback(()=>cancelIdleCallback(mine));", "idle: cancels itself");
    check(p2b_idle(&f, &outcome) && outcome.status == OS64_JS_OK, "idle: cancelling the running one is nothing");
    size_t before = os64_dom_bytes(f.binding);
    dom_script(&f, "{const f=()=>{};for(let round=0;round<10;round++){const hs=[];"
                   "for(let i=0;i<10000;i++)hs.push(requestIdleCallback(f));for(const h of hs)cancelIdleCallback(h)}}",
               "idle: 100000 asked for and cancelled");
    check(os64_dom_bytes(f.binding) < before + 600 * 1024, "idle: the holes are closed up as they grow");
    check(os64_dom_idle_next(f.binding) == UINT64_MAX, "idle: none left");
    /* A page cannot size the binding's memory past its quota. */
    dom_script(&f, "{let refused=false;const f=()=>{};try{for(let i=0;i<2e6;i++)requestIdleCallback(f)}catch(e){refused=e.name}"
                   "if(refused!=='QuotaExceededError')throw Error('refused: '+refused);}", "idle: the quota");
    fixture_free(&f);
}

/* Every binding allocation (the queue's node, the idle entries, the
 * timeout's timer) and every engine allocation (the record, the copy, the
 * event) of each operation, refused in turn: nothing crashes, nothing
 * leaks, and the teardown balances (fixture_free). The value holds more
 * than 16 objects (o), so a cut also lands on the writer's memory table
 * growing past its first size with entries already in it. */
static size_t p2b_sweep_run(const char *operation, int drive, bool engine, size_t cut)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return 0;
    dom_script(&f, "globalThis.v={a:[1,'x',2n],m:new Map([[{},new Set([1])]]),d:new Date(1),r:/q/g,"
                   "e:new TypeError('t',{cause:1}),b:new Blob(['b']),x:new DOMException('m','AbortError'),"
                   "u:new Uint16Array([1,2]),w:new DataView(new ArrayBuffer(4)),o:Array.from({length:20},()=>({}))};v.self=v;"
                   "addEventListener('message',e=>{globalThis.last=e.data});addEventListener('messageerror',()=>{});",
               "sweep: setup");
    size_t *counter = engine ? &attempts : &dom_attempts;
    size_t start = *counter;
    if (cut != 0) {
        if (engine) fail_at = attempts + cut;
        else dom_fail_at = dom_attempts + cut;
    }
    os64_js_outcome_t outcome;
    os64_js_run(f.runtime, operation, strlen(operation), "sweep.js", &outcome);
    if (drive == 1) os64_dom_message_fire(f.binding, &outcome);
    if (drive == 2) {
        os64_dom_idle_run(f.binding, &outcome);
        p2b_advance(10);
        os64_dom_timer_fire(f.binding, p2b_ms, &outcome);
    }
    fail_at = 0;
    dom_fail_at = 0;
    size_t count = *counter - start;
    fixture_free(&f);
    return count;
}

/* NOTHING IS DETACHED UNLESS THE WHOLE CLONE SUCCEEDS: every engine
 * allocation of a structuredClone with a transfer list refused in turn, so
 * the refusal lands in the writer and in the reader. After each, a page
 * that finds its buffer detached by a clone that failed makes an 8 MiB
 * buffer the test port counts (large_asks); none may. */
static size_t p2b_detach_run(size_t cut)
{
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return 0;
    dom_script(&f, "globalThis.go=function(){const b=new ArrayBuffer(8);"
                   "const v={m:new Map([[1,{a:[1,2,3]}]]),s:new Set(['x']),d:new Date(1),r:/q/g,u:new Uint8Array(b,2,4)};"
                   "let ok=false;try{structuredClone(v,{transfer:[b]});ok=true}catch(e){}"
                   "if(b.byteLength===0&&!ok)globalThis.wrong=new ArrayBuffer(8<<20)}",
               "detach sweep: setup");
    size_t start = attempts, asks = large_asks;
    if (cut != 0) fail_at = attempts + cut;
    os64_js_outcome_t outcome;
    os64_js_run(f.runtime, "go()", 4, "detach.js", &outcome);
    fail_at = 0;
    size_t count = attempts - start;
    check(large_asks == asks, "clone: a clone refused anywhere leaves its transfer list attached");
    fixture_free(&f);
    return count;
}

static void dom_p2b_sweep_cases(void)
{
    size_t points = p2b_detach_run(0);
    printf("p2b detach allocation points: %zu\n", points);
    for (size_t cut = 1; cut <= points; cut++) {
        p2b_detach_run(cut);
        dom_sweeps++;
    }
    static const struct { const char *operation; int drive; } kOperations[] = {
        {"const b=new ArrayBuffer(8,{maxByteLength:16});structuredClone([v,b],{transfer:[b]})", 0},
        {"const b=new ArrayBuffer(2);postMessage(v,'*',[b]);postMessage(1,{targetOrigin:'https://example.test/'})", 1},
        {"requestIdleCallback(d=>d.timeRemaining());requestIdleCallback(()=>{},{timeout:5})", 2},
    };
    for (size_t op = 0; op < sizeof(kOperations) / sizeof(kOperations[0]); op++)
        for (int engine = 0; engine < 2; engine++) {
            p2b_ms = p2b_us = 0;
            size_t count = p2b_sweep_run(kOperations[op].operation, kOperations[op].drive, engine, 0);
            printf("p2b operation %zu %s allocation points: %zu\n", op, engine ? "engine" : "binding", count);
            for (size_t cut = 1; cut <= count; cut++) {
                p2b_ms = p2b_us = 0;
                p2b_sweep_run(kOperations[op].operation, kOperations[op].drive, engine, cut);
                dom_sweeps++;
            }
        }
}

static void dom_p2b_cases(void)
{
    dom_clone_value_cases();
    dom_clone_hostile_cases();
    dom_clone_bound_cases();
    dom_clone_interrupt_cases();
    dom_message_cases();
    dom_message_event_cases();
    dom_idle_cases();
    dom_idle_failure_cases();
}
