/* P1-W, WINDOW'S LOW-HANGING FRUIT (libdom/browsing.c, libdom/crypto.c;
 * WINDOW_DOCUMENT.md § P1-W): a case per sentence of browsing.c's and
 * crypto.c's contracts, the classic fixes (window and document
 * unforgeable, history read-only, status kept), and every allocation the
 * new members make failed in turn. */

#include <time.h>

/* The host's entropy, faked: byte n of all the bytes ever asked for is
 * (n * 37 + 11) & 255, so a case can say exactly what a fill holds.
 * window_entropy_fail refuses every ask after writing a byte, as a source
 * that ran short does; the binding must hand none of it to the page. */
static uint64_t window_entropy_next;
static size_t window_entropy_asks, window_entropy_most;
static bool window_entropy_fail;
static bool window_entropy(void *opaque, uint8_t *out, size_t length)
{
    check(opaque == NULL, "entropy: the binding's host context is the options'");
    window_entropy_asks++;
    if (length > window_entropy_most) window_entropy_most = length;
    for (size_t i = 0; i < length; i++) out[i] = (uint8_t)((window_entropy_next++ * 37 + 11) & 255);
    return !window_entropy_fail;
}

#define WINDOW_URL "https://example.test/dir/page.html"

static os64_dom_options_t window_options(const char *url)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.url = url;
    options.entropy = window_entropy;
    return options;
}

/* What the page's fill bytes should be, from the fake's next byte on. */
#define WINDOW_JS "function fake(at,n){let a=[];for(let i=0;i<n;i++)a.push((at+i)*37+11&255);return a}" \
    "function same(a,b){return a.length===b.length&&a.every((x,i)=>x===b[i])}"

static void dom_window_member_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "function own(n){return Object.getOwnPropertyDescriptor(window,n)}"
        /* opener: null, and HTML's setter */
        "assert(opener===null&&typeof own('opener').set==='function');"
        "opener=null;assert(opener===null&&typeof own('opener').get==='function','null keeps it');"
        "opener=5;assert(opener===5&&own('opener').value===5&&own('opener').writable,'anything else is own data');"
        /* closed and close() */
        "assert(closed===false&&close()===undefined&&closed===false&&typeof own('closed').set==='undefined');"
        /* clientInformation, [Replaceable] */
        "assert(clientInformation===navigator);clientInformation=1;assert(own('clientInformation').value===1);"
        /* the moves and sizes: no-ops, their arguments converted */
        "let asked=0,n={valueOf(){asked++;return 3}};"
        "assert(moveTo(n,n)===undefined&&moveBy(1,2)===undefined&&resizeTo(1,n)===undefined&&resizeBy(0,0)===undefined);"
        "assert(asked===3,'each argument converted: '+asked);"
        "throws('TypeError',()=>moveTo(1));throws('TypeError',()=>resizeBy());throws('TypeError',()=>moveBy(Symbol(),1));"
        "assert(moveTo.length===2&&resizeBy.length===2&&moveTo.name==='moveTo');"
        "assert(captureEvents()===undefined&&releaseEvents()===undefined&&captureEvents.length===0);"
        /* the constants */
        "assert(frameElement===null&&crossOriginIsolated===false&&originAgentCluster===true);"
        "assert(isSecureContext===true&&origin==='https://example.test'&&self.origin===origin);"
        "origin='mine';assert(origin==='mine'&&own('origin').value==='mine','origin is [Replaceable]');"
        "isSecureContext=false;assert(isSecureContext===true,'read-only');",
        "window: P1-W's members on an https page");
    dom_script(&f, STORAGE_ASSERT
        "const bars=['locationbar','menubar','personalbar','scrollbars','statusbar','toolbar'];"
        "const objects=bars.map(n=>window[n]);"
        "assert(new Set(objects).size===6,'six objects');"
        "for(const b of objects)assert(b instanceof BarProp&&b.visible===true&&Object.prototype.toString.call(b)==='[object BarProp]');"
        "assert(bars.every((n,i)=>window[n]===objects[i]),'the same object each read');"
        "throws('TypeError',()=>new BarProp());"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(BarProp.prototype,'visible').get.call({}));"
        "toolbar=7;assert(toolbar===7,'[Replaceable]');"
        "assert(external instanceof External&&external===window.external);"
        "assert(external.AddSearchProvider()===undefined&&external.IsSearchProviderInstalled()===undefined);"
        "throws('TypeError',()=>External.prototype.AddSearchProvider.call({}));throws('TypeError',()=>new External());",
        "window: the BarProps and external");
    dom_script(&f, STORAGE_ASSERT
        "for(const t of ['online','offline','languagechange'])assert(('on'+t) in window&&window['on'+t]===null,t);"
        "let heard=[];onoffline=e=>heard.push(e.type);"
        "addEventListener('online',e=>heard.push('listener:'+e.type));ononline=e=>heard.push('handler:'+e.type);"
        "dispatchEvent(new Event('offline'));dispatchEvent(new Event('online'));"
        "assert(heard.join()==='offline,listener:online,handler:online',heard.join());"
        "onoffline=null;assert(onoffline===null);"
        "document.body.setAttribute('onlanguagechange','heard.push(\"body\")');"
        "assert(typeof onlanguagechange==='function','the body content attribute is window\\'s');"
        "dispatchEvent(new Event('languagechange'));assert(heard.pop()==='body');",
        "window: ononline, onoffline and onlanguagechange are handler slots");
    fixture_free(&f);

    /* What a page's address decides: isSecureContext, origin, randomUUID. */
    static const struct { const char *url; bool secure; const char *origin; } kPages[] = {
        {"https://example.test/", true, "https://example.test"},
        {"http://example.test:8080/x", false, "http://example.test:8080"},
        {"http://127.0.0.1/x", true, "http://127.0.0.1"},
        {"http://localhost/x", false, "http://localhost"},
        {"file:///home/page.html", false, "null"},
        {"about:srcdoc", true, "null"},
        {"about:blank?x", false, "null"},
        {NULL, true, "null"},
    };
    for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
        options = window_options(kPages[i].url);
        f = fixture_new(&options);
        if (!fixture_ready(&f)) return;
        char source[512];
        snprintf(source, sizeof(source), STORAGE_ASSERT
                 "assert(isSecureContext===%s,'isSecureContext '+isSecureContext);"
                 "assert(origin==='%s','origin '+origin);"
                 "assert(('randomUUID' in crypto)===%s,'randomUUID is [SecureContext]');"
                 "assert(typeof crypto.getRandomValues==='function');",
                 kPages[i].secure ? "true" : "false", kPages[i].origin, kPages[i].secure ? "true" : "false");
        dom_script(&f, source, kPages[i].url != NULL ? kPages[i].url : "window: a page with no address");
        fixture_free(&f);
    }
}

/* The classic fixes, with the switch on and off: window and document
 * unforgeable, history a read-only accessor, status kept. */
static void dom_window_classic_cases(void)
{
    for (int modern = 1; modern >= 0; modern--) {
        os64_dom_options_t options = window_options(WINDOW_URL);
        options.modern = modern;
        DomFixture f = fixture_new(&options);
        if (!fixture_ready(&f)) return;
        dom_script(&f, STORAGE_ASSERT
            "const doc=document,win=window,hist=history;"
            "for(const n of ['window','document']){const d=Object.getOwnPropertyDescriptor(window,n);"
            "assert(typeof d.get==='function'&&d.set===undefined&&!d.configurable&&d.enumerable,n);}"
            "assert(window===globalThis&&window.window===window&&document.nodeType===9);"
            "window=1;document=2;window.document=3;self.window=4;"
            "assert(window===win&&document===doc&&globalThis.window===win,'an assignment is ignored');"
            "throws('TypeError',()=>{'use strict';window.document=1});"
            "assert(delete window.document===false&&document===doc);"
            "throws('TypeError',()=>Object.defineProperty(window,'document',{value:1}));"
            "throws('TypeError',()=>Object.defineProperty(window,'window',{get(){return 1}}));"
            "const h=Object.getOwnPropertyDescriptor(window,'history');"
            "assert(typeof h.get==='function'&&h.set===undefined&&h.configurable&&h.enumerable);"
            "history=1;assert(history===hist&&history.length===1,'history ignores an assignment');"
            "throws('TypeError',()=>{'use strict';history=1});"
            "status='set';assert(status==='set'&&window.status==='set');"
            "status=Symbol.iterator.description;assert(status==='Symbol.iterator');"
            "throws('TypeError',()=>{status=Symbol()});assert(status==='Symbol.iterator','a failed set keeps the last');",
            modern ? "window: the classic fixes, switch on" : "window: the classic fixes, switch off");
        /* `let document` at a script's top level meets the non-configurable
         * property, as in a browser. */
        os64_js_outcome_t outcome;
        os64_js_status_t r = os64_js_run(f.runtime, "let document=1", 14, "let-document.js", &outcome);
        check(r == OS64_JS_EXCEPTION && strstr(outcome.message, "SyntaxError") != NULL,
              "window: `let document` at the top level is a SyntaxError");
        dom_script(&f, STORAGE_ASSERT "assert(document.nodeType===9)", "window: the document survives it");
        fixture_free(&f);
    }
}

static void dom_window_crypto_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    window_entropy_next = 0;
    window_entropy_asks = 0;
    window_entropy_most = 0;
    window_entropy_fail = false;
    dom_script(&f, STORAGE_ASSERT WINDOW_JS
        "assert(crypto instanceof Crypto&&crypto===window.crypto&&Object.prototype.toString.call(crypto)==='[object Crypto]');"
        "assert(!('subtle' in crypto),'subtle is not here (ruled)');"
        "throws('TypeError',()=>new Crypto());crypto=1;assert(crypto instanceof Crypto,'read-only');"
        "assert(Crypto.prototype.getRandomValues.length===1&&Crypto.prototype.randomUUID.length===0);"
        /* each integer kind: the same array, filled from the fake's bytes */
        "let at=0;"
        "for(const C of [Int8Array,Uint8Array,Uint8ClampedArray,Int16Array,Uint16Array,Int32Array,Uint32Array,"
        "BigInt64Array,BigUint64Array]){const a=new C(3);"
        "assert(crypto.getRandomValues(a)===a,C.name+' answers the same array');"
        "const bytes=new Uint8Array(a.buffer);assert(same(Array.from(bytes),fake(at,bytes.length)),C.name+' filled');"
        "at+=bytes.length}"
        "const z=new Uint8Array(0);assert(crypto.getRandomValues(z)===z);"
        "class Mine extends Uint16Array{};const m=new Mine(2);assert(crypto.getRandomValues(m)===m);at+=4;"
        /* a view's own window of its buffer, and nothing else */
        "const whole=new Uint8Array(8);crypto.getRandomValues(whole.subarray(2,5));"
        "assert(same(Array.from(whole),[0,0].concat(fake(at,3),[0,0,0])),'only the view');at+=3;"
        /* the wrong views and the wrong values */
        "for(const v of [new Float32Array(2),new Float64Array(2),new Float16Array(2),new DataView(new ArrayBuffer(4)),"
        "new Float32Array(20000)]){let e;try{crypto.getRandomValues(v)}catch(x){e=x}"
        "assert(e instanceof DOMException&&e.name==='TypeMismatchError'&&e.code===17,String(v))}"
        "for(const v of [{},new ArrayBuffer(4),[1,2],undefined,null,1])throws('TypeError',()=>crypto.getRandomValues(v));"
        "throws('TypeError',()=>crypto.getRandomValues());"
        "throws('TypeError',()=>Crypto.prototype.getRandomValues.call({},new Uint8Array(1)));"
        /* the quota: 65,536 bytes, and one more refused untouched */
        "const big=new Uint8Array(65536);assert(crypto.getRandomValues(big)===big);at+=65536;"
        "const over=new Uint32Array(16385);let q;try{crypto.getRandomValues(over)}catch(x){q=x}"
        "assert(q&&q.name==='QuotaExceededError'&&q.code===22&&over.every(x=>x===0),'past the quota');"
        /* A view of a resizable buffer is refused whatever the buffer did
         * since the view was made: grown past the quota, shrunk, or not
         * resized at all; and a float view of one is a TypeError before it
         * is the wrong type. A page replacing the getters changes nothing. */
        "const grown=new ArrayBuffer(16,{maxByteLength:200000}),gv=new Uint8Array(grown);grown.resize(100000);"
        "throws('TypeError',()=>crypto.getRandomValues(gv));assert(gv.every(x=>x===0),'grown: untouched');"
        "const shrunk=new ArrayBuffer(64,{maxByteLength:64}),sv=new Uint8Array(shrunk);shrunk.resize(8);"
        "throws('TypeError',()=>crypto.getRandomValues(sv));"
        "throws('TypeError',()=>crypto.getRandomValues(new Uint8Array(new ArrayBuffer(4,{maxByteLength:8}))));"
        "throws('TypeError',()=>crypto.getRandomValues(new Float32Array(new ArrayBuffer(4,{maxByteLength:8}))));"
        "throws('TypeError',()=>crypto.getRandomValues(new DataView(new ArrayBuffer(4,{maxByteLength:8}))));"
        "Object.defineProperty(ArrayBuffer.prototype,'resizable',{get(){return false}});"
        "Object.defineProperty(Object.getPrototypeOf(Uint8Array.prototype),'buffer',{get(){return new ArrayBuffer(1)}});"
        "throws('TypeError',()=>crypto.getRandomValues(new Uint8Array(new ArrayBuffer(4,{maxByteLength:8}))));"
        /* A fixed-length buffer is filled as before. */
        "const fixed=new Uint8Array(new ArrayBuffer(6));assert(crypto.getRandomValues(fixed)===fixed);"
        "assert(same(Array.from(fixed),fake(at,6)),'a fixed buffer is filled');at+=6;"
        "globalThis.at=at;",
        "crypto: getRandomValues");
    check(window_entropy_most == 65536, "crypto: the host is never asked for more than 65,536 bytes");
    dom_script(&f, STORAGE_ASSERT WINDOW_JS
        "const u=crypto.randomUUID();"
        "assert(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u),u);"
        "const b=fake(at,16);b[6]=b[6]&15|64;b[8]=b[8]&63|128;"
        "const hex=b.map(x=>x.toString(16).padStart(2,'0')).join('');"
        "assert(u===[hex.slice(0,8),hex.slice(8,12),hex.slice(12,16),hex.slice(16,20),hex.slice(20)].join('-'),u+' '+hex);"
        "throws('TypeError',()=>Crypto.prototype.randomUUID.call({}));",
        "crypto: randomUUID is version 4 over the host's bytes");
    window_entropy_fail = true;
    dom_script(&f, STORAGE_ASSERT
        "const a=new Uint8Array(4).fill(170);let e;try{crypto.getRandomValues(a)}catch(x){e=x}"
        "assert(e&&e.name==='OperationError'&&e instanceof DOMException,'a refusal throws');"
        "assert(a.every(x=>x===170),'and the array keeps what it held');"
        "let refused;try{crypto.randomUUID()}catch(x){refused=x}assert(refused&&refused.name==='OperationError');",
        "crypto: a hook that fails throws, and fills nothing");
    window_entropy_fail = false;
    fixture_free(&f);
    /* With no hook, every ask is refused. */
    options.entropy = NULL;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "throws('OperationError',()=>crypto.getRandomValues(new Uint8Array(1)));"
        "throws('OperationError',()=>crypto.randomUUID());"
        "assert(crypto.getRandomValues(new Uint8Array(0)).length===0,'nothing to fill asks nothing');",
        "crypto: a host with no entropy refuses");
    fixture_free(&f);
}

/* queueMicrotask: the engine's job queue, in order with promise jobs, and
 * a callback that throws reported as the task's failure while the jobs
 * after it run. */
static void dom_window_microtask_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "throws('TypeError',()=>queueMicrotask());throws('TypeError',()=>queueMicrotask({}));"
        "throws('TypeError',()=>queueMicrotask('code'));assert(queueMicrotask.length===1);"
        "globalThis.order=[];"
        "queueMicrotask(()=>order.push(1));Promise.resolve().then(()=>order.push(2));"
        "queueMicrotask(function(){'use strict';order.push(this===undefined?3:'this')});"
        "Promise.resolve().then(()=>queueMicrotask(()=>order.push(5)));queueMicrotask(()=>order.push(4));"
        "assert(order.length===0,'nothing runs inside the script');",
        "microtask: queued in order with promise jobs");
    dom_script(&f, STORAGE_ASSERT "assert(order.join()==='1,2,3,4,5',order.join());",
               "microtask: run at the checkpoint, FIFO");
    /* A throw in a timer's task: the task reports it, and the next runs. */
    dom_script(&f,
        "globalThis.after=[];setTimeout(()=>{queueMicrotask(()=>{throw new Error('boom')});"
        "queueMicrotask(()=>after.push('ran'));after.push('queued')},0)",
        "microtask: a timer queues a throwing callback");
    os64_js_outcome_t outcome;
    check(os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &outcome) && outcome.status == OS64_JS_EXCEPTION &&
              strstr(outcome.message, "boom") != NULL,
          "microtask: a callback that throws is reported as its task's failure");
    dom_script(&f, STORAGE_ASSERT "assert(after.join()==='queued,ran',after.join())",
               "microtask: the script that queued it finished, and the job after it ran");
    /* The same in a host dispatch: the listener's task reports it. */
    dom_script(&f,
        "globalThis.after=[];addEventListener('resize',()=>{queueMicrotask(()=>{throw new TypeError('bang')});"
        "queueMicrotask(()=>after.push('ran'))})",
        "microtask: a listener queues a throwing callback");
    os64_dom_event_t event = {.type = "resize"};
    bool prevented = false;
    check(os64_dom_dispatch(f.binding, NULL, &event, &prevented, &outcome) == OS64_JS_EXCEPTION &&
              strstr(outcome.message, "bang") != NULL,
          "microtask: a host dispatch reports the callback's throw");
    dom_script(&f, STORAGE_ASSERT "assert(after.join()==='ran',after.join())",
               "microtask: and the job after it ran");
    fixture_free(&f);
}

static void dom_window_sweep_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
    static const char *const kOperations[] = {
        "crypto.getRandomValues(new Uint8Array(32))",
        "crypto.randomUUID()",
        "queueMicrotask(()=>{globalThis.q=1})",
        "status='a status';status",
        "[locationbar.visible,external.AddSearchProvider(),origin,isSecureContext,opener=null,moveTo(1,2)]",
        "ononline=()=>1;onoffline=null;document.body.setAttribute('onlanguagechange','1');onlanguagechange",
    };
    size_t n = sizeof(kOperations) / sizeof(kOperations[0]);
    binding_sweep("window", kOperations, n, &options);
    /* Not the last: reading onlanguagechange compiles the body's handler,
     * and a compile cut short still reaches `resolve_labels`, which writes
     * through a buffer that failed to grow (a SEGV in put_u32): the
     * unchecked parser path libjs patch 0011b owes (DEBTS.md), the
     * engine's to prove. */
    engine_sweep("window", kOperations, n - 1, &options);
}

/* `--window-reads`: what a million bare reads of a plain global, `window`,
 * `document` and `history` cost, on the host's monotonic clock. A
 * measurement printed, never a check: the clock is the machine's. */
static void dom_window_read_timing(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.plain=1", "reads: a plain global");
    static const char *const kReads[] = {"plain", "window", "document", "history"};
    for (size_t i = 0; i < sizeof(kReads) / sizeof(kReads[0]); i++) {
        char source[160];
        snprintf(source, sizeof(source), "for(let i=0,x;i<1000000;i++)x=%s", kReads[i]);
        struct timespec began, ended;
        clock_gettime(CLOCK_MONOTONIC, &began);
        dom_script(&f, source, "reads: a million");
        clock_gettime(CLOCK_MONOTONIC, &ended);
        double ms = (double)(ended.tv_sec - began.tv_sec) * 1000.0 + (double)(ended.tv_nsec - began.tv_nsec) / 1e6;
        printf("reads: 1e6 bare `%s` in %.1f ms\n", kReads[i], ms);
    }
    fixture_free(&f);
}

static void dom_window_cases(void)
{
    dom_window_member_cases();
    dom_window_classic_cases();
    dom_window_crypto_cases();
    dom_window_microtask_cases();
    dom_window_sweep_cases();
}
