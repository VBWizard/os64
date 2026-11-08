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

/* crypto.subtle, tier 3a (WEB_CRYPTO.md): HKDF against RFC 5869 A.1 and
 * PBKDF2 against RFC 6070, a derivation key's import and export refusals,
 * deriveBits' lengths, deriveKey into AES and HMAC (the derived key is the
 * derivation's first bits) and each of its refusals in the spec's order,
 * AES-KW against RFC 3394 4.1 through wrapKey and back, a JWK padded to
 * AES-KW's 8 bytes, and the dictionary members copied when they are
 * converted (a salt, an iv), which a later getter must not reach, though
 * the hash's getter runs before the copy, as step 10 orders it; and 512
 * bytes wrapped under AES-KW, past a one-byte step counter. Every
 * expectation is node's WebCrypto's but four: three where node departs
 * from the spec (a derivation key's format is checked before its usages,
 * an HMAC length of 0 is get key length's TypeError, and a key's usages
 * read back in the recognized order, the spec's normalized value, where
 * node keeps the caller's), and PBKDF2's 65,536-byte cap, which is
 * yonder's (WEB_CRYPTO.md § 3). */
static void dom_window_subtle_tier3a_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
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
        "function unhex(t){return new Uint8Array(t.match(/../g).map(x=>parseInt(x,16)))}"
        "(async()=>{const s=crypto.subtle,enc=t=>new TextEncoder().encode(t);"
        "const refused=async(p,n,t)=>{try{await p;log.push(t+': resolved')}catch(e){log.push(t+': '+(e.name===n?'ok':e.name))}};"
        "const hk=await s.importKey('raw',unhex('0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b'),'HKDF',false,['deriveBits','deriveKey']);"
        "const hp={name:'HKDF',hash:'SHA-256',salt:unhex('000102030405060708090a0b0c'),info:unhex('f0f1f2f3f4f5f6f7f8f9')};"
        "log.push('hkdf:'+hex(await s.deriveBits(hp,hk,336)).slice(0,16));"
        "log.push('hkdf key:'+[hk.type,hk.extractable,hk.algorithm.name,hk.algorithm.length,hk.usages.join('/')].join());"
        "const pk=await s.importKey('raw',enc('password'),{name:'pbkdf2'},false,['deriveBits']);"
        "log.push('pbkdf2:'+hex(await s.deriveBits({name:'PBKDF2',hash:'SHA-1',salt:enc('salt'),iterations:2},pk,160)));"
        "log.push('empty:'+(await s.deriveBits(hp,hk,0)).byteLength);"
        "await refused(s.deriveBits(hp,hk,null),'OperationError','a null length');"
        "await refused(s.deriveBits(hp,hk),'OperationError','no length');"
        "await refused(s.deriveBits(hp,hk,230),'OperationError','a length within a byte');"
        "await refused(s.deriveBits({...hp,hash:'SHA-1'},hk,8*255*20+8),'OperationError','past 255 hashes');"
        "await refused(s.deriveBits(hp,pk,256),'InvalidAccessError','another algorithm\\'s key');"
        "await refused(s.deriveBits({name:'PBKDF2',hash:'SHA-1',salt:enc('salt'),iterations:0},pk,160),'OperationError','no iterations');"
        "await refused(s.deriveBits({name:'HKDF',hash:'SHA-256',info:new Uint8Array(0)},hk,256),'TypeError','a missing salt');"
        "await refused(s.deriveBits({...hp,hash:'AES-GCM'},hk,256),'NotSupportedError','a hash that is no digest');"
        "await refused(s.importKey('raw',new Uint8Array(4),'HKDF',true,['deriveBits']),'SyntaxError','an extractable derivation key');"
        "await refused(s.importKey('spki',new Uint8Array(4),'PBKDF2',false,['sign']),'NotSupportedError','the format before the usages');"
        "await refused(s.importKey('raw',new Uint8Array(4),'HKDF',false,['sign']),'SyntaxError','a usage it lacks');"
        "await refused(s.exportKey('raw',hk),'NotSupportedError','exporting a derivation key');"
        "await refused(s.generateKey({name:'HKDF'},false,['deriveBits']),'NotSupportedError','generating one');"
        "const bitsOnly=await s.importKey('raw',new Uint8Array(4),'HKDF',false,['deriveBits']);"
        "await refused(s.deriveKey(hp,bitsOnly,{name:'AES-GCM',length:128},true,['encrypt']),'InvalidAccessError','deriveKey wants deriveKey');"
        "const ak=await s.deriveKey(hp,hk,{name:'AES-GCM',length:256},true,['encrypt']);"
        "log.push('derived aes:'+(hex(await s.exportKey('raw',ak))===hex(await s.deriveBits(hp,hk,256)))+','+ak.algorithm.length);"
        "const mk=await s.deriveKey(hp,hk,{name:'HMAC',hash:{name:'sha-512'}},false,['sign']);"
        "log.push('derived hmac:'+mk.algorithm.hash.name+','+mk.algorithm.length);"
        "await refused(s.deriveKey(hp,hk,{name:'HMAC',hash:'SHA-256',length:0},true,['sign']),'TypeError','an HMAC length of 0');"
        "await refused(s.deriveKey(hp,hk,{name:'AES-GCM',length:512},true,['encrypt']),'OperationError','an AES length of 512');"
        "await refused(s.deriveKey(hp,hk,{name:'AES-GCM'},true,['encrypt']),'TypeError','an AES type with no length');"
        "await refused(s.deriveKey(hp,hk,'HKDF',false,['deriveBits']),'OperationError','a derivation key derived');"
        "await refused(s.deriveKey(hp,hk,{name:'AES-GCM',length:128},true,[]),'SyntaxError','no usages, after the derivation');"
        "const salt=unhex('000102030405060708090a0b0c');"
        "const seen=await s.deriveKey({name:'HKDF',hash:'SHA-256',salt,info:hp.info},hk,{get name(){salt.fill(0);return 'AES-GCM'},length:256},true,['encrypt']);"
        "log.push('salt copied:'+(hex(await s.exportKey('raw',seen))===hex(await s.exportKey('raw',ak))));"
        "const kek=await s.importKey('raw',unhex('000102030405060708090a0b0c0d0e0f'),'AES-KW',true,['wrapKey','unwrapKey']);"
        "const data=await s.importKey('raw',unhex('00112233445566778899aabbccddeeff'),'AES-CBC',true,['encrypt']);"
        "const wrapped=await s.wrapKey('raw',data,kek,'AES-KW');"
        "log.push('kw:'+hex(wrapped));"
        "const back=await s.unwrapKey('raw',wrapped,kek,{name:'AES-KW'},'AES-CBC',true,['encrypt']);"
        "log.push('kw back:'+hex(await s.exportKey('raw',back)));"
        "const bad=new Uint8Array(wrapped);bad[23]^=1;"
        "await refused(s.unwrapKey('raw',bad,kek,'AES-KW','AES-CBC',true,['encrypt']),'OperationError','an altered wrapped key');"
        "await refused(s.unwrapKey('raw',new Uint8Array(16),kek,'AES-KW','AES-CBC',true,['encrypt']),'OperationError','one block to unwrap');"
        "const short=await s.importKey('raw',new Uint8Array(8),{name:'HMAC',hash:'SHA-256'},true,['sign']);"
        "await refused(s.wrapKey('raw',short,kek,'AES-KW'),'OperationError','one block to wrap');"
        "const wj=await s.wrapKey('jwk',data,kek,'AES-KW');"
        "const uj=await s.unwrapKey('jwk',wj,kek,'AES-KW','AES-CBC',true,['encrypt']);"
        "log.push('kw jwk padded:'+(wj.byteLength%8)+','+hex(await s.exportKey('raw',uj)));"
        "await refused(s.encrypt({name:'AES-KW'},kek,new Uint8Array(16)),'NotSupportedError','AES-KW encrypting');"
        "await refused(s.wrapKey('raw',hk,kek,'AES-KW'),'NotSupportedError','wrapping a derivation key');"
        "const kwj=await s.exportKey('jwk',kek);"
        "log.push('kw jwk:'+kwj.alg+','+kwj.key_ops.join('/'));"
        "const gk=await s.generateKey({name:'AES-KW',length:192},true,['wrapKey']);"
        "log.push('kw generated:'+gk.algorithm.length+','+(await s.exportKey('raw',gk)).byteLength);"
        "await refused(s.generateKey({name:'AES-KW',length:128},true,['encrypt']),'SyntaxError','an AES-KW key that encrypts');"
        "const gw=await s.generateKey({name:'AES-GCM',length:128},false,['wrapKey','unwrapKey']);"
        "const iv=new Uint8Array(12);"
        "const gwrapped=await s.wrapKey('raw',data,gw,{name:'AES-GCM',iv});"
        "const un=await s.unwrapKey('raw',gwrapped,gw,{name:'AES-GCM',iv},{get name(){iv[0]=1;return 'AES-CBC'}},true,['encrypt']);"
        "log.push('iv copied:'+hex(await s.exportKey('raw',un)).slice(0,8));"
        "const big=await s.importKey('raw',new Uint8Array(512).map((_,i)=>i&255),{name:'HMAC',hash:'SHA-256'},true,['sign']);"
        "log.push('kw big:'+hex(await s.digest('SHA-256',await s.wrapKey('raw',big,kek,'AES-KW'))).slice(0,16));"
        "const hsalt=unhex('000102030405060708090a0b0c');"
        "const hb=await s.deriveBits({name:'HKDF',hash:{get name(){hsalt.fill(0);return 'SHA-256'}},salt:hsalt,info:hp.info},hk,256);"
        "log.push('hash before copy:'+hex(hb).slice(0,16));"
        "await refused(s.deriveBits({name:'PBKDF2',hash:'SHA-256',salt:enc('s'),iterations:1},pk,65537*8),'OperationError','past the PBKDF2 cap');"
        "})().catch(e=>log.push('threw: '+e.name+' '+e.message));",
        "subtle tier 3a: the calls");
    subtle_settle(&f);
    dom_script(&f, STORAGE_ASSERT
        "{const want=["
        "'hkdf:3cb25f25faacd57a','hkdf key:secret,false,HKDF,,deriveKey/deriveBits',"
        "'pbkdf2:ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957','empty:0','a null length: ok','no length: ok',"
        "'a length within a byte: ok','past 255 hashes: ok','another algorithm\\'s key: ok',"
        "'no iterations: ok','a missing salt: ok','a hash that is no digest: ok',"
        "'an extractable derivation key: ok','the format before the usages: ok','a usage it lacks: ok',"
        "'exporting a derivation key: ok','generating one: ok','deriveKey wants deriveKey: ok',"
        "'derived aes:true,256','derived hmac:SHA-512,1024','an HMAC length of 0: ok',"
        "'an AES length of 512: ok','an AES type with no length: ok','a derivation key derived: ok',"
        "'no usages, after the derivation: ok','salt copied:true',"
        "'kw:1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5','kw back:00112233445566778899aabbccddeeff',"
        "'an altered wrapped key: ok','one block to unwrap: ok','one block to wrap: ok',"
        "'kw jwk padded:0,00112233445566778899aabbccddeeff','AES-KW encrypting: ok',"
        "'wrapping a derivation key: ok','kw jwk:A128KW,wrapKey/unwrapKey','kw generated:192,24',"
        "'an AES-KW key that encrypts: ok','iv copied:00112233','kw big:fcfa100fa59ec685','hash before copy:abbafb13f5c1bc48','past the PBKDF2 cap: ok'"
        "];"
        "for(let i=0;i<Math.max(log.length,want.length);i++)"
        "assert(log[i]===want[i],'entry '+i+': got '+log[i]+', want '+want[i]);}",
        "subtle tier 3a: answered");
    fixture_free(&f);
}

/* Tier 3b: ECDSA and ECDH on the three curves. RFC 6979's P-256 key and
 * its SHA-256 signature of "sample", RFC 5903's P-384 exchange, WPT's
 * shapes of key pair, export and failure. Every expectation is node's
 * WebCrypto's but these, where yonder follows the spec or WebIDL: the
 * signature equals RFC 6979's (BearSSL's nonce is deterministic, node's
 * random); a JWK's and a CryptoKeyPair's members come in WebIDL's
 * lexicographic order (node's do not); exporting the wrong half of a pair
 * in a format is InvalidAccessError (§ 23.7.5; node says
 * NotSupportedError); and a compressed point is refused (DataError, which
 * the spec allows; node takes it). */
static void dom_window_subtle_tier3b_cases(void)
{
    os64_dom_options_t options = window_options(WINDOW_URL);
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
        "function unhex(t){return new Uint8Array(t.match(/../g).map(x=>parseInt(x,16)))}"
        "(async()=>{const s=crypto.subtle,enc=t=>new TextEncoder().encode(t);"
        "const refused=async(p,n,t)=>{try{await p;log.push(t+': resolved')}catch(e){log.push(t+': '+(e.name===n?'ok':e.name))}};"
        "const E256={name:'ECDSA',namedCurve:'P-256'},D384={name:'ECDH',namedCurve:'P-384'};"
        "const rj={kty:'EC',crv:'P-256',x:'YP7UuiVanTHJYet0xjVtaMBJuJI7Yfps5mliLmDyn7Y',y:'eQP-EAi4vJmkGunpVii8ZPLxsgwtfp9Rd6PClNRGIpk',d:'ya-p2EW6dRZrXCFXZ7HWk05Qw9s26JsSe4piKxIPZyE'};"
        "const rpriv=await s.importKey('jwk',rj,E256,true,['sign']);"
        "const rpub=await s.importKey('jwk',{kty:'EC',crv:'P-256',x:rj.x,y:rj.y},E256,true,['verify']);"
        "const rsig=unhex('EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8');"
        "log.push('rfc6979 verifies:'+await s.verify({name:'ECDSA',hash:'SHA-256'},rpub,rsig,enc('sample')));"
        "const mine=await s.sign({name:'ECDSA',hash:'SHA-256'},rpriv,enc('sample'));"
        "log.push('deterministic:'+(hex(mine)===hex(rsig)));"
        "log.push('short:'+await s.verify({name:'ECDSA',hash:'SHA-256'},rpub,rsig.slice(1),enc('sample')));"
        "log.push('other hash:'+await s.verify({name:'ECDSA',hash:'SHA-384'},rpub,rsig,enc('sample')));"
        "await refused(s.sign({name:'ECDSA',hash:'SHA-256'},rpub,enc('x')),'InvalidAccessError','signing with a public key');"
        "await refused(s.sign({name:'ECDSA'},rpriv,enc('x')),'TypeError','ECDSA with no hash');"
        "log.push('pub jwk:'+Object.keys(await s.exportKey('jwk',rpub)).join());"
        "log.push('priv jwk:'+Object.keys(await s.exportKey('jwk',rpriv)).join());"
        "log.push('raw:'+hex(await s.exportKey('raw',rpub)).slice(0,18));"
        "log.push('spki:'+hex(await s.exportKey('spki',rpub)).slice(0,60));"
        "log.push('pkcs8:'+hex(await s.exportKey('pkcs8',rpriv)).slice(0,80));"
        "await refused(s.exportKey('pkcs8',rpub),'InvalidAccessError','pkcs8 of a public key');"
        "await refused(s.exportKey('spki',rpriv),'InvalidAccessError','spki of a private key');"
        "await refused(s.exportKey('raw',rpriv),'InvalidAccessError','raw of a private key');"
        "const p8=await s.exportKey('pkcs8',rpriv);"
        "const again=await s.importKey('pkcs8',p8,E256,true,['sign']);"
        "log.push('pkcs8 round trip:'+(hex(await s.exportKey('pkcs8',again))===hex(p8))+','+again.type);"
        "const sp=await s.importKey('spki',await s.exportKey('spki',rpub),{name:'ECDSA',namedCurve:'P-256'},false,['verify']);"
        "log.push('spki round trip:'+sp.type+','+sp.algorithm.namedCurve+','+sp.usages.join('/'));"
        "const raw=new Uint8Array(await s.exportKey('raw',rpub));"
        "const comp=raw.slice(0,33);comp[0]=2+(raw[64]&1);"
        "await refused(s.importKey('raw',comp,E256,true,['verify']),'DataError','a compressed point');"
        "const off=raw.slice();off[64]^=1;"
        "await refused(s.importKey('raw',off,E256,true,['verify']),'DataError','a point off the curve');"
        "await refused(s.importKey('raw',raw,{name:'ECDSA',namedCurve:'P-512'},true,['verify']),'NotSupportedError','a curve not here');"
        "await refused(s.importKey('raw',raw,{name:'ECDSA',namedCurve:'p-256'},true,['verify']),'NotSupportedError','a curve misspelled');"
        "await refused(s.importKey('raw',raw,{name:'ECDSA'},true,['verify']),'TypeError','no namedCurve');"
        "await refused(s.importKey('raw',raw,E256,true,['sign']),'SyntaxError','a public key that signs');"
        "await refused(s.importKey('raw',raw,{name:'ECDSA',namedCurve:'P-384'},true,['verify']),'DataError','another curve\\'s point');"
        "await refused(s.importKey('jwk',{...rj,crv:'P-384'},E256,true,['sign']),'DataError','a JWK of another crv');"
        "await refused(s.importKey('jwk',{...rj,alg:'ES384'},E256,true,['sign']),'DataError','a JWK alg of another curve');"
        "log.push('jwk ES256:'+(await s.importKey('jwk',{...rj,alg:'ES256'},E256,true,['sign'])).type);"
        "await refused(s.importKey('jwk',rj,E256,true,[]),'SyntaxError','a private key with no usages');"
        "log.push('public no usages:'+(await s.importKey('raw',raw,E256,true,[])).usages.length);"
        "await refused(s.importKey('jwk',{...rj,x:rj.x.slice(1)},E256,true,['sign']),'DataError','a JWK x a byte short');"
        "const g=await s.generateKey({name:'ECDSA',namedCurve:'P-521'},false,['sign','verify']);"
        "log.push('pair:'+Object.keys(g).join()+','+g.publicKey.type+','+g.publicKey.extractable+','+g.publicKey.usages.join('/')+','+g.privateKey.type+','+g.privateKey.extractable+','+g.privateKey.usages.join('/')+','+g.privateKey.algorithm.namedCurve);"
        "const gs=await s.sign({name:'ECDSA',hash:'SHA-512'},g.privateKey,enc('hello'));"
        "log.push('p521 sig:'+gs.byteLength+','+await s.verify({name:'ECDSA',hash:'SHA-512'},g.publicKey,gs,enc('hello')));"
        "log.push('mismatch:'+await (async()=>{const o=await s.generateKey(E256,true,['sign']);const j=await s.exportKey('jwk',o.privateKey);try{await s.importKey('jwk',{...j,x:rj.x,y:rj.y},E256,true,['sign']);return 'resolved'}catch(e){return e.name}})());"
        "await refused(s.generateKey(E256,true,['verify']),'SyntaxError','a pair whose private key has no usage');"
        "await refused(s.generateKey({name:'ECDSA',namedCurve:'P-512'},true,['sign']),'NotSupportedError','generating on a curve not here');"
        "await refused(s.generateKey(E256,true,['encrypt']),'SyntaxError','an ECDSA usage it lacks');"
        "const ij={kty:'EC',crv:'P-384',x:'ZnhC19GArCzeb3TzdVH1V1XHZFwg73PjFjT-crTFXubeOsgIrLS9tMiHMq7pX0Gq',y:'lILtH8Duucr8SYRiXM_CP2UDIUng4UStoCQYFTWg847rn8_zwslH2uabTGNFc6gc',d:'CZ88cDTUosaZiE1zo3Wmf3Yk73xrPA8WBke2dBTc5lXjW1OAQeZJ7j-u-JZ4OrGU'};"
        "const ipriv=await s.importKey('jwk',ij,D384,false,['deriveBits','deriveKey']);"
        "const rpubk=await s.importKey('raw',unhex('04E558DBEF53EECDE3D3FCCFC1AEA08A89A987475D12FD950D83CFA41732BC509D0D1AC43A0336DEF96FDA41D0774A3571DCFBEC7AACF3196472169E838430367F66EEBE3C6E70C416DD5F0C68759DD1FFF83FA40142209DFF5EAAD96DB9E6386C'),D384,true,[]);"
        "log.push('rfc5903:'+hex(await s.deriveBits({name:'ECDH',public:rpubk},ipriv,null)).slice(0,24));"
        "log.push('whole:'+(await s.deriveBits({name:'ECDH',public:rpubk},ipriv)).byteLength);"
        "log.push('230:'+hex(await s.deriveBits({name:'ECDH',public:rpubk},ipriv,230)).slice(-4));"
        "log.push('zero:'+(await s.deriveBits({name:'ECDH',public:rpubk},ipriv,0)).byteLength);"
        "await refused(s.deriveBits({name:'ECDH',public:rpubk},ipriv,392),'OperationError','longer than the secret');"
        "await refused(s.deriveBits({name:'ECDH',public:ipriv},ipriv,8),'InvalidAccessError','a private key as public');"
        "await refused(s.deriveBits({name:'ECDH',public:rpub},ipriv,8),'InvalidAccessError','an ECDSA key as public');"
        "const h256=await s.generateKey({name:'ECDH',namedCurve:'P-256'},true,['deriveBits']);"
        "await refused(s.deriveBits({name:'ECDH',public:h256.publicKey},ipriv,8),'InvalidAccessError','another curve\\'s public key');"
        "await refused(s.deriveBits({name:'ECDH',public:{}},ipriv,8),'TypeError','a public that is no key');"
        "await refused(s.deriveBits({name:'ECDH'},ipriv,8),'TypeError','no public');"
        "await refused(s.deriveBits({name:'ECDH',public:rpubk},rpubk,8),'InvalidAccessError','deriving from a public key');"
        "log.push('ecdh pair:'+h256.publicKey.usages.length+','+h256.privateKey.usages.join('/'));"
        "await refused(s.generateKey({name:'ECDH',namedCurve:'P-256'},true,[]),'SyntaxError','an ECDH pair with no usage');"
        "await refused(s.importKey('raw',unhex('04E558DBEF53EECDE3D3FCCFC1AEA08A89A987475D12FD950D83CFA41732BC509D0D1AC43A0336DEF96FDA41D0774A3571DCFBEC7AACF3196472169E838430367F66EEBE3C6E70C416DD5F0C68759DD1FFF83FA40142209DFF5EAAD96DB9E6386C'),D384,true,['deriveBits']),'SyntaxError','an ECDH public key that derives');"
        "log.push('ecdh jwk alg:'+(await s.importKey('jwk',{...ij,alg:'whatever'},D384,false,['deriveBits'])).type);"
        "const dk=await s.deriveKey({name:'ECDH',public:rpubk},ipriv,{name:'AES-GCM',length:256},true,['encrypt']);"
        "log.push('derived:'+(hex(await s.exportKey('raw',dk))===hex(await s.deriveBits({name:'ECDH',public:rpubk},ipriv,256))));"
        "await refused(s.deriveKey({name:'ECDH',public:rpubk},ipriv,{name:'HMAC',hash:'SHA-512'},true,['sign']),'OperationError','an HMAC key longer than the secret');"
        "const kek=await s.importKey('raw',new Uint8Array(16).fill(7),'AES-KW',false,['wrapKey','unwrapKey']);"
        "const wj=await s.wrapKey('jwk',rpriv,kek,'AES-KW');"
        "const uj=await s.unwrapKey('jwk',wj,kek,'AES-KW',E256,true,['sign']);"
        "log.push('wrapped jwk:'+(hex(await s.exportKey('pkcs8',uj))===hex(p8)));"
        "const gk=await s.importKey('raw',new Uint8Array(16).fill(9),'AES-GCM',false,['wrapKey','unwrapKey']);"
        "const iv=new Uint8Array(12);"
        "const wp=await s.wrapKey('pkcs8',rpriv,gk,{name:'AES-GCM',iv});"
        "const up=await s.unwrapKey('pkcs8',wp,gk,{name:'AES-GCM',iv},E256,true,['sign']);"
        "log.push('wrapped pkcs8:'+(hex(await s.exportKey('pkcs8',up))===hex(p8)));"
        "const ws=await s.wrapKey('spki',rpub,gk,{name:'AES-GCM',iv});"
        "log.push('wrapped spki:'+(await s.unwrapKey('spki',ws,gk,{name:'AES-GCM',iv},E256,true,['verify'])).type);"
        "await refused(s.wrapKey('raw',rpriv,gk,{name:'AES-GCM',iv}),'InvalidAccessError','wrapping a private key raw');"
        "})().catch(e=>log.push('threw: '+e.name+' '+e.message));",
        "subtle tier 3b: the calls");
    subtle_settle(&f);
    dom_script(&f, STORAGE_ASSERT
        "{const want=["
        "'rfc6979 verifies:true',"
        "'deterministic:true',"
        "'short:false',"
        "'other hash:false',"
        "'signing with a public key: ok',"
        "'ECDSA with no hash: ok',"
        "'pub jwk:crv,ext,key_ops,kty,x,y',"
        "'priv jwk:crv,d,ext,key_ops,kty,x,y',"
        "'raw:0460fed4ba255a9d31',"
        "'spki:3059301306072a8648ce3d020106082a8648ce3d0301070342000460fed4',"
        "'pkcs8:308187020100301306072a8648ce3d020106082a8648ce3d030107046d306b0201010420c9afa9d8',"
        "'pkcs8 of a public key: ok',"
        "'spki of a private key: ok',"
        "'raw of a private key: ok',"
        "'pkcs8 round trip:true,private',"
        "'spki round trip:public,P-256,verify',"
        "'a compressed point: ok',"
        "'a point off the curve: ok',"
        "'a curve not here: ok',"
        "'a curve misspelled: ok',"
        "'no namedCurve: ok',"
        "'a public key that signs: ok',"
        "'another curve\\'s point: ok',"
        "'a JWK of another crv: ok',"
        "'a JWK alg of another curve: ok',"
        "'jwk ES256:private',"
        "'a private key with no usages: ok',"
        "'public no usages:0',"
        "'a JWK x a byte short: ok',"
        "'pair:privateKey,publicKey,public,true,verify,private,false,sign,P-521',"
        "'p521 sig:132,true',"
        "'mismatch:DataError',"
        "'a pair whose private key has no usage: ok',"
        "'generating on a curve not here: ok',"
        "'an ECDSA usage it lacks: ok',"
        "'rfc5903:11187331c279962d93d60424',"
        "'whole:48',"
        "'230:7e70',"
        "'zero:0',"
        "'longer than the secret: ok',"
        "'a private key as public: ok',"
        "'an ECDSA key as public: ok',"
        "'another curve\\'s public key: ok',"
        "'a public that is no key: ok',"
        "'no public: ok',"
        "'deriving from a public key: ok',"
        "'ecdh pair:0,deriveBits',"
        "'an ECDH pair with no usage: ok',"
        "'an ECDH public key that derives: ok',"
        "'ecdh jwk alg:private',"
        "'derived:true',"
        "'an HMAC key longer than the secret: ok',"
        "'wrapped jwk:true',"
        "'wrapped pkcs8:true',"
        "'wrapped spki:public',"
        "'wrapping a private key raw: ok'"
        "];"
        "for(let i=0;i<Math.max(log.length,want.length);i++)"
        "assert(log[i]===want[i],'entry '+i+': got '+log[i]+', want '+want[i]);}",
        "subtle tier 3b: answered");
    fixture_free(&f);
}

static void dom_window_cases(void)
{
    dom_window_member_cases();
    dom_window_classic_cases();
    dom_window_crypto_cases();
    dom_window_subtle_cases();
    dom_window_subtle_tier2_cases();
    dom_window_subtle_tier3a_cases();
    dom_window_subtle_tier3b_cases();
    dom_window_microtask_cases();
    dom_window_sweep_cases();
}
