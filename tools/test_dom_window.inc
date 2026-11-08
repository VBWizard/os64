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
        {"file:///home/page.html", true, "null"},
        {"about:srcdoc", true, "null"},
        {"about:blank?x", false, "null"},
        {NULL, true, "null"},
    };
    for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
        options = window_options(kPages[i].url);
        f = fixture_new(&options);
        if (!fixture_ready(&f)) return;
        char source[1024];
        snprintf(source, sizeof(source), STORAGE_ASSERT
                 "assert(isSecureContext===%s,'isSecureContext '+isSecureContext);"
                 "assert(origin==='%s','origin '+origin);"
                 "assert(('randomUUID' in crypto)===%s,'randomUUID is [SecureContext]');"
                 "assert(('subtle' in crypto)===%s&&('SubtleCrypto' in window)===%s&&('CryptoKey' in window)===%s,"
                 "'subtle and its interfaces are [SecureContext]');"
                 "assert(typeof crypto.getRandomValues==='function');",
                 kPages[i].secure ? "true" : "false", kPages[i].origin, kPages[i].secure ? "true" : "false",
                 kPages[i].secure ? "true" : "false", kPages[i].secure ? "true" : "false",
                 kPages[i].secure ? "true" : "false");
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
            "assert(top===window&&parent===window&&length===0&&frames===window,'the root context, either way');"
            "for(const n of ['top','parent','length'])assert(typeof Object.getOwnPropertyDescriptor(window,n).get==='function',n);"
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
        "assert(crypto.subtle instanceof SubtleCrypto,'subtle is here on a secure page (WEB_CRYPTO.md)');"
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

/* crypto.subtle (WEB_CRYPTO.md, tier 1): the interfaces whole, digest
 * over every buffer source, and every failure a rejection, never a throw.
 * The digests' values are FIPS 180-2's (tools/test_subtle_core_host.sh
 * holds the core to more). */
#define SUBTLE_JS "function hex(b){return Array.from(new Uint8Array(b),x=>x.toString(16).padStart(2,'0')).join('')}" \
    "function rejects(p,n,tag){assert(p instanceof Promise,tag+': a promise');" \
    "p.then(()=>log.push(tag+': resolved'),e=>log.push(tag+': '+(e.name===n?'ok':e.name+' '+e.message)))}"

/* crypto.subtle settles its promises from a task (a binding timer of no
 * delay; subtle.c's settle_later): every timer due fired, and the tasks
 * they queue in turn, before a script reads what the promises said. */
static void subtle_settle(DomFixture *f)
{
    os64_js_outcome_t outcome;
    for (int i = 0; i < 100000 && os64_dom_timer_fire(f->binding, UINT64_MAX - 1, &outcome); i++) {
        if (outcome.status != OS64_JS_OK)
            fprintf(stderr, "subtle task: status=%d limit=%d message=%s\n", outcome.status, outcome.limit, outcome.message);
        check(outcome.status == OS64_JS_OK, "subtle: a task that settles a promise runs cleanly");
        if (outcome.status != OS64_JS_OK) return;
    }
}

static void dom_window_subtle_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "{const s=crypto.subtle;assert(s===crypto.subtle&&s instanceof SubtleCrypto,'one SubtleCrypto');"
        "assert(Object.prototype.toString.call(s)==='[object SubtleCrypto]');"
        "const d=Object.getOwnPropertyDescriptor(Crypto.prototype,'subtle');"
        "assert(d&&typeof d.get==='function'&&d.set===undefined&&!Object.getOwnPropertyNames(crypto).includes('subtle'),"
        "'subtle is an accessor on Crypto.prototype');"
        "throws('TypeError',()=>d.get.call({}));"
        "throws('TypeError',()=>new SubtleCrypto());throws('TypeError',()=>new CryptoKey());"
        "const want={encrypt:3,decrypt:3,sign:3,verify:4,digest:2,generateKey:3,deriveKey:5,deriveBits:2,"
        "importKey:5,exportKey:2,wrapKey:4,unwrapKey:7};"
        "for(const m in want){const f=SubtleCrypto.prototype[m];"
        "assert(typeof f==='function'&&f.length===want[m]&&f.name===m,m+'\\'s shape')}"
        "for(const a of ['type','extractable','algorithm','usages']){"
        "const k=Object.getOwnPropertyDescriptor(CryptoKey.prototype,a);"
        "assert(k&&typeof k.get==='function'&&k.set===undefined,a);throws('TypeError',()=>k.get.call({}))}}",
        "subtle: the interfaces");
    dom_script(&f, STORAGE_ASSERT SUBTLE_JS
        "globalThis.log=[];{const s=crypto.subtle,abc=new TextEncoder().encode('abc');"
        "s.digest('SHA-256',abc).then(b=>log.push('256:'+(b instanceof ArrayBuffer)+hex(b)));"
        "s.digest({name:'sha-1'},new Uint8Array(0)).then(b=>log.push('1:'+hex(b)));"
        "s.digest('Sha-384',abc.buffer).then(b=>log.push('384:'+hex(b).slice(0,16)));"
        "s.digest({name:'SHA-512'},new DataView(abc.buffer)).then(b=>log.push('512:'+hex(b).slice(0,16)));"
        "const wide=new TextEncoder().encode('xabcx');"
        "s.digest('SHA-256',wide.subarray(1,4)).then(b=>log.push('view:'+hex(b).slice(0,8)));"
        "s.digest('SHA-256',new Int32Array(0)).then(b=>log.push('int32:'+hex(b).slice(0,8)));"
        /* failures: rejections, each with its name */
        "rejects(s.digest('AES-GCM',abc),'NotSupportedError','an algorithm that is no hash');"
        "rejects(s.digest('\\u017fha-256',abc),'NotSupportedError','only ASCII folds');"
        "rejects(s.digest({},abc),'TypeError','a dictionary with no name');"
        "rejects(s.digest('SHA-256'),'TypeError','too few arguments');"
        "rejects(s.digest('SHA-256','abc'),'TypeError','a string is no BufferSource');"
        "rejects(s.digest('SHA-256',new ArrayBuffer(4,{maxByteLength:8})),'TypeError','a resizable buffer');"
        "rejects(SubtleCrypto.prototype.digest.call({},'SHA-256',abc),'TypeError','the receiver');"
        "rejects(s.digest({get name(){throw new RangeError('x')}},abc),'RangeError','a getter that throws');"
        /* the bytes are read after the name: a getter that restores the
         * buffer is seen, and one that detaches it leaves none */
        "const m=new Uint8Array(abc);m[0]=0;"
        "s.digest({get name(){m[0]=97;return 'SHA-256'}},m).then(b=>log.push('restored:'+hex(b).slice(0,8)));"
        "const t=new Uint8Array(abc);let reads=0;"
        "s.digest({get name(){reads++;if(!t.buffer.detached)t.buffer.transfer();return 'SHA-256'}},t).then(b=>log.push('detached:'+hex(b).slice(0,8)),"
        "e=>log.push('detached: '+e.name+' '+e.message));"
        "assert(reads===2,'the name is read twice, as the Algorithm dictionary and as its own: '+reads);"
        "const late=new Uint8Array(abc);s.digest('SHA-256',late).then(b=>log.push('late:'+hex(b).slice(0,8)));late[0]=0;"
        /* the other eleven: what the IDL refuses is a TypeError, and an
         * algorithm nothing registers NotSupportedError */
        "rejects(s.generateKey({name:'NOT-AN-ALGORITHM',length:128},true,['encrypt']),'NotSupportedError','generateKey');"
        "rejects(s.generateKey('AES-GCM',true,['bogus']),'TypeError','an unknown usage');"
        "rejects(s.importKey('raw',new Uint8Array(16),'NOT-AN-ALGORITHM',true,['encrypt']),'NotSupportedError','importKey');"
        "rejects(s.importKey('jwk',{kty:'oct'},'NOT-AN-ALGORITHM',true,['sign']),'NotSupportedError','importKey of a JWK');"
        "rejects(s.importKey('bogus',new Uint8Array(16),'AES-GCM',true,['encrypt']),'TypeError','an unknown format');"
        "rejects(s.importKey('raw',5,'AES-GCM',true,['encrypt']),'TypeError','a number is no key data');"
        "rejects(s.encrypt('AES-GCM',{},abc),'TypeError','an object that is no CryptoKey');"
        "rejects(s.exportKey('raw',{}),'TypeError','exportKey wants a key');"
        "rejects(s.deriveBits('HKDF',{},8),'TypeError','deriveBits wants a key');"
        "rejects(s.unwrapKey('raw',abc,{},'AES-KW','AES-GCM',true,[]),'TypeError','unwrapKey wants a key');}",
        "subtle: digest and the rejections");
    subtle_settle(&f);
    dom_script(&f, STORAGE_ASSERT
        "{const want=['256:trueba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',"
        "'1:da39a3ee5e6b4b0d3255bfef95601890afd80709','384:cb00753f45a35e8b','512:ddaf35a193617aba',"
        "'view:ba7816bf','int32:e3b0c442',"
        "'an algorithm that is no hash: ok','only ASCII folds: ok','a dictionary with no name: ok',"
        "'too few arguments: ok','a string is no BufferSource: ok','a resizable buffer: ok','the receiver: ok',"
        "'a getter that throws: ok','restored:ba7816bf','detached:e3b0c442','late:ba7816bf',"
        "'generateKey: ok','an unknown usage: ok','importKey: ok','importKey of a JWK: ok','an unknown format: ok',"
        "'a number is no key data: ok','an object that is no CryptoKey: ok','exportKey wants a key: ok',"
        "'deriveBits wants a key: ok','unwrapKey wants a key: ok'];"
        "for(let i=0;i<Math.max(log.length,want.length);i++)"
        "assert(log[i]===want[i],'entry '+i+': got '+log[i]+', want '+want[i]);}",
        "subtle: answered in order");
    /* WebIDL's shapes and conversions: members enumerable and named, a name
     * converted with ToString, a usage sequence item by item, a JsonWebKey
     * converted with the arguments (before the algorithm's name is read),
     * and Level 2's enumerations. */
    dom_script(&f, STORAGE_ASSERT SUBTLE_JS
        "globalThis.log=[];{const s=crypto.subtle,abc=new TextEncoder().encode('abc');"
        "for(const [P,m] of [[SubtleCrypto.prototype,'digest'],[SubtleCrypto.prototype,'unwrapKey'],"
        "[Crypto.prototype,'getRandomValues'],[Crypto.prototype,'randomUUID']])"
        "assert(Object.getOwnPropertyDescriptor(P,m).enumerable,m+' is enumerable');"
        "for(const [P,a] of [[Crypto.prototype,'subtle'],[CryptoKey.prototype,'type'],[CryptoKey.prototype,'usages']]){"
        "const d=Object.getOwnPropertyDescriptor(P,a);assert(d.enumerable&&d.get.name==='get '+a,a+'\\'s getter')}"
        "assert(!Object.getOwnPropertyDescriptor(SubtleCrypto.prototype,'constructor').enumerable,'constructor is not');"
        "s.digest({name:{toString(){return 'SHA-256'}}},abc).then(b=>log.push('tostring:'+hex(b).slice(0,8)));"
        "function*g(){yield 'nope';log.push('pulled');yield 'sign'}"
        "rejects(s.generateKey('AES-GCM',true,g()),'TypeError','a bad usage ends the iteration');"
        "rejects(s.importKey('jwk',{get kty(){throw new RangeError('k')}},'AES-GCM',true,[]),'RangeError','a JWK getter');"
        "rejects(s.importKey('raw',{key_ops:5},'AES-GCM',true,[]),'TypeError','a JWK member of the wrong type');"
        "rejects(s.importKey('jwk',{oth:[5]},'AES-GCM',true,[]),'TypeError','oth holds dictionaries');"
        "const order=[];"
        "rejects(s.importKey('jwk',{get alg(){order.push('alg')},get kty(){order.push('kty')}},"
        "{get name(){order.push('name');return 'AES-GCM'}},true,{*[Symbol.iterator](){order.push('usages')}}),"
        "'DataError','the JWK is converted with the arguments');"
        "assert(order.join()==='alg,kty,usages,name,name','conversion order '+order.join());"
        "rejects(s.importKey('raw-secret',abc,{name:'HMAC',hash:'SHA-256'},true,['sign']),'NotSupportedError',"
        "'a newer draft\\'s format, which HMAC does not import');"
        "rejects(s.generateKey({name:'AES-GCM',length:128},true,['encapsulateKey']),'SyntaxError',"
        "'a newer draft\\'s usage, which AES does not have');}",
        "subtle: WebIDL's shapes and conversions");
    subtle_settle(&f);
    dom_script(&f, STORAGE_ASSERT
        "{const want=['tostring:ba7816bf','a bad usage ends the iteration: ok','a JWK getter: ok',"
        "'a JWK member of the wrong type: ok','oth holds dictionaries: ok','the JWK is converted with the arguments: ok',"
        "'a newer draft\\'s format, which HMAC does not import: ok','a newer draft\\'s usage, which AES does not have: ok'];"
        "for(let i=0;i<Math.max(log.length,want.length);i++)"
        "assert(log[i]===want[i],'entry '+i+': got '+log[i]+', want '+want[i]);}",
        "subtle: the conversions answered, and nothing pulled past a bad usage");
    fixture_free(&f);
}

/* crypto.subtle, tier 2 (WEB_CRYPTO.md): AES-CTR, AES-CBC and AES-GCM
 * round trips, HMAC against RFC 4231's first case, the key object, JWK
 * export, wrapping under each mode, the usage and extractability
 * refusals, HMAC lengths within a byte and past what is generated, an
 * unwrapped JWK read past a polluted Object.prototype, results that
 * arrive in a task of their own, and more calls in flight than the page
 * has timers. WPT's WebCryptoAPI holds the rest. */
static void dom_window_subtle_tier2_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
    /* An await chain and a 5000-promise Promise.all spend far more than the
     * fixture's 100 jobs a turn; yonder's budget is 2^20 (scripts.c). */
    os64_js_config_t config = fixture_config();
    config.limits.jobs_per_turn = (uint64_t)1 << 20;
    DomFixture f = fixture_configured(0, 0, &config);
    if (f.runtime && f.state) {
        os64_js_outcome_t outcome;
        f.binding = os64_dom_create(f.runtime, f.document, f.state, &options, &outcome);
    }
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT SUBTLE_JS
        "globalThis.log=[];"
        "(async()=>{const s=crypto.subtle,data=new TextEncoder().encode('tier two'),enc=t=>new TextEncoder().encode(t);"
        "for(const name of ['AES-CTR','AES-CBC','AES-GCM']){"
        "const key=await s.generateKey({name,length:128},true,['encrypt','decrypt']);"
        "const p=name==='AES-CTR'?{name,counter:new Uint8Array(16),length:64}:{name,iv:new Uint8Array(name==='AES-GCM'?12:16)};"
        "const ct=await s.encrypt(p,key,data);const pt=await s.decrypt(p,key,ct);"
        "log.push(name+':'+new TextDecoder().decode(pt)+':'+ct.byteLength)}"
        /* RFC 4231 test case 1 */
        "const hk=await s.importKey('raw',new Uint8Array(20).fill(11),{name:'HMAC',hash:'SHA-256'},true,['sign','verify']);"
        "const mac=await s.sign({name:'HMAC',hash:'SHA-1'},hk,enc('Hi There'));"
        "log.push('hmac:'+hex(mac).slice(0,16));"
        "log.push('verify:'+(await s.verify('HMAC',hk,mac,enc('Hi There')))+','+(await s.verify('HMAC',hk,mac.slice(1),enc('Hi There')))"
        "+','+(await s.verify('HMAC',hk,mac,enc('Hi there'))));"
        "const k=await s.generateKey({name:'aes-gcm',length:256},false,['decrypt','encrypt','encrypt']);"
        "log.push('key:'+[k.type,k.extractable,k.algorithm.name,k.algorithm.length,k.algorithm===k.algorithm,"
        "k.usages===k.usages,k.usages.join('/'),Object.prototype.toString.call(k)].join());"
        "log.push('hmac key:'+[hk.algorithm.name,hk.algorithm.hash.name,hk.algorithm.length].join());"
        "const jwk=await s.exportKey('jwk',hk);"
        "const want=btoa(String.fromCharCode(...new Uint8Array(20).fill(11))).replace(/=+$/,'').replace(/\\+/g,'-').replace(/\\//g,'_');"
        "log.push('jwk:'+[Object.keys(jwk).join('/'),jwk.kty,jwk.alg,jwk.k===want,jwk.key_ops.join('/'),jwk.ext].join());"
        "const back=await s.importKey('jwk',jwk,{name:'HMAC',hash:'SHA-256'},false,['verify']);"
        "log.push('jwk back:'+(await s.verify('HMAC',back,mac,enc('Hi There'))));"
        "const wk=await s.generateKey({name:'AES-GCM',length:128},false,['wrapKey','unwrapKey']);"
        "const gp={name:'AES-GCM',iv:new Uint8Array(12)};"
        "for(const fmt of ['raw','jwk']){const w=await s.wrapKey(fmt,hk,wk,gp);"
        "const u=await s.unwrapKey(fmt,w,wk,gp,{name:'HMAC',hash:'SHA-256'},true,['sign']);"
        "log.push('wrap '+fmt+':'+hex(await s.exportKey('raw',u)).slice(0,8)+','+u.usages.join('/'))}"
        "const refused=async(p,n,t)=>{try{await p;log.push(t+': resolved')}catch(e){log.push(t+': '+(e.name===n?'ok':e.name))}};"
        "await refused(s.exportKey('raw',k),'InvalidAccessError','a key that is not extractable');"
        "await refused(s.encrypt(gp,hk,data),'InvalidAccessError','another algorithm\\'s key');"
        "const encOnly=await s.generateKey({name:'AES-GCM',length:128},true,['encrypt']);"
        "await refused(s.decrypt(gp,encOnly,new Uint8Array(32)),'InvalidAccessError','a usage the key lacks');"
        "await refused(s.decrypt(gp,k,new Uint8Array(32)),'OperationError','a tag that does not authenticate');"
        "await refused(s.generateKey({name:'AES-CBC',length:100},true,['encrypt']),'OperationError','an AES length');"
        "await refused(s.generateKey({name:'HMAC',hash:'SHA-256'},true,['encrypt']),'SyntaxError','an HMAC usage');"
        "await refused(s.importKey('raw',new Uint8Array(16),'AES-CTR',true,[]),'SyntaxError','no usages');"
        "await refused(s.importKey('jwk',{kty:'oct',k:'AAAA+A',alg:'A128GCM'},'AES-GCM',true,['encrypt']),'DataError','a k in the standard alphabet');"
        "await refused(s.encrypt({name:'AES-GCM',iv:new Uint8Array(0)},encOnly,data),'OperationError','an empty GCM iv');"
        "await refused(s.encrypt({name:'AES-CTR',counter:new Uint8Array(16),length:1},"
        "await s.importKey('raw',new Uint8Array(16),'AES-CTR',false,['encrypt']),new Uint8Array(33)),'OperationError','a counter that would repeat');"
        "const order=[];const p=s.digest('SHA-256',data).then(()=>order.push('crypto'));queueMicrotask(()=>order.push('micro'));await p;"
        "log.push('order:'+order.join());"
        /* An HMAC length within a byte: the key's last byte keeps that many
         * bits; one past what is generated is refused, never wrapped. */
        "const odd=await s.generateKey({name:'HMAC',hash:'SHA-256',length:161},true,['sign']);"
        "const oddRaw=new Uint8Array(await s.exportKey('raw',odd));"
        "const full=await s.importKey('raw',new Uint8Array(21).fill(255),{name:'HMAC',hash:'SHA-256',length:161},true,['sign']);"
        "log.push('odd:'+[odd.algorithm.length,oddRaw.length,oddRaw[20]&127,new Uint8Array(await s.exportKey('raw',full))[20]].join());"
        "await refused(s.generateKey({name:'HMAC',hash:'SHA-256',length:4294967295},true,['sign']),'OperationError','the longest HMAC length');"
        "await refused(s.generateKey({name:'HMAC',hash:'SHA-256',length:524289},true,['sign']),'OperationError','past the generated cap');"
        /* Wrapping under the other two modes, raw and JWK (CBC pads the
         * JSON, CTR does not). */
        "const others=[];"
        "for(const [name,wp] of [['AES-CBC',{name:'AES-CBC',iv:new Uint8Array(16)}],['AES-CTR',{name:'AES-CTR',counter:new Uint8Array(16),length:64}]]){"
        "const wk2=await s.generateKey({name,length:192},false,['wrapKey','unwrapKey']);"
        "for(const fmt of ['raw','jwk']){const w=await s.wrapKey(fmt,hk,wk2,wp);"
        "const u=await s.unwrapKey(fmt,w,wk2,wp,{name:'HMAC',hash:'SHA-256'},true,['sign']);"
        "others.push(name+' '+fmt+'='+hex(await s.exportKey('raw',u)).slice(0,8))}}"
        "log.push('wrap others:'+others.join());"
        /* An unwrapped JWK is read as a new global would read it: nothing a
         * page put on Object.prototype counts. */
        "const ek=await s.generateKey({name:'AES-GCM',length:128},false,['encrypt','unwrapKey']);"
        "const noKty=await s.encrypt(gp,ek,enc('{\"k\":\"CwsLCwsLCwsLCwsLCwsLCwsLCws\"}'));"
        "const wj=await s.wrapKey('jwk',hk,wk,gp);"
        "Object.defineProperty(Object.prototype,'alg',{get(){return 'x'},configurable:true});"
        "Object.prototype.ext=false;Object.prototype.kty='oct';"
        "try{const u=await s.unwrapKey('jwk',wj,wk,gp,{name:'HMAC',hash:'SHA-256'},true,['sign']);"
        "log.push('pristine:'+hex(await s.exportKey('raw',u)).slice(0,8)+','+u.extractable);"
        "await refused(s.unwrapKey('jwk',noKty,ek,gp,{name:'HMAC',hash:'SHA-256'},true,['sign']),'DataError','a JWK with no kty');"
        "}finally{delete Object.prototype.alg;delete Object.prototype.ext;delete Object.prototype.kty}"
        /* More calls in flight than the page has timers: they settle in the
         * order they were made, and the page's own setTimeout still works. */
        "const seen=[];const all=Array.from({length:5000},(_,i)=>s.digest('SHA-1',data).then(()=>seen.push(i)));"
        "let timer='ok';try{setTimeout(()=>{},0)}catch(e){timer=e.name}"
        "await Promise.all(all);"
        "log.push('queue:'+timer+','+seen.length+','+seen.every((v,i)=>v===i));"
        "})().catch(e=>log.push('threw: '+e.name+' '+e.message));",
        "subtle tier 2: the calls");
    subtle_settle(&f);
    dom_script(&f, STORAGE_ASSERT
        "{const want=['AES-CTR:tier two:8','AES-CBC:tier two:16','AES-GCM:tier two:24','hmac:b0344c61d8db3853',"
        "'verify:true,false,false','key:secret,false,AES-GCM,256,true,true,encrypt/decrypt,[object CryptoKey]',"
        "'hmac key:HMAC,SHA-256,160','jwk:alg/ext/k/key_ops/kty,oct,HS256,true,sign/verify,true','jwk back:true',"
        "'wrap raw:0b0b0b0b,sign','wrap jwk:0b0b0b0b,sign','a key that is not extractable: ok',"
        "'another algorithm\\'s key: ok','a usage the key lacks: ok','a tag that does not authenticate: ok',"
        "'an AES length: ok','an HMAC usage: ok','no usages: ok','a k in the standard alphabet: ok','an empty GCM iv: ok',"
        "'a counter that would repeat: ok','order:micro,crypto','odd:161,21,0,128','the longest HMAC length: ok',"
        "'past the generated cap: ok','wrap others:AES-CBC raw=0b0b0b0b,AES-CBC jwk=0b0b0b0b,AES-CTR raw=0b0b0b0b,"
        "AES-CTR jwk=0b0b0b0b','pristine:0b0b0b0b,true','a JWK with no kty: ok','queue:ok,5000,true'];"
        "for(let i=0;i<Math.max(log.length,want.length);i++)"
        "assert(log[i]===want[i],'entry '+i+': got '+log[i]+', want '+want[i]);}",
        "subtle tier 2: answered");
    fixture_free(&f);
}

static void dom_window_cases(void)
{
    dom_window_member_cases();
    dom_window_classic_cases();
    dom_window_crypto_cases();
    dom_window_subtle_cases();
    dom_window_subtle_tier2_cases();
    dom_window_microtask_cases();
    dom_window_sweep_cases();
}
