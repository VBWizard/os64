/* A SCRIPT'S REQUESTS (libdom/fetch.c, xhr.c, abort.c, blob.c, exception.c;
 * dom.h § A SCRIPT'S REQUESTS): the origin the binding names is libway's,
 * byte for byte; Headers, Request and Response as a page makes them; fetch()
 * and XMLHttpRequest against a fake host network that records each request
 * and answers when a case says; aborting at each point a request can be;
 * the work a page or a server can size, held to its bound by count; a body
 * the runtime has no room for; every allocation failed in turn; and every
 * name gone with the modern switch off. */

#include <way/origin.h>

/* fetch.c's count of a header list's steps (libdom/internal.h). */
extern uint64_t d_request_steps;
/* abort.c's count of the steps a signal's links take (libdom/internal.h). */
extern uint64_t d_signal_steps;
/* abort.c's count of the dependents a signal's weak set links. */
size_t d_signal_dependents(JSValue signal);

#define FETCH_ASSERT STORAGE_ASSERT

/* What the fake network saw of one request. */
typedef struct {
    uint32_t token;
    uint64_t id;
    bool cancelled, preflight, has_body;
    char url[512], method[32], referrer[512];
    char headers[4096];
    size_t headers_length;
    char body[4096];
    size_t body_length;
    way_mode_t mode;
    way_credentials_t credentials;
    way_redirect_t redirect;
} NetSeen;

typedef struct {
    NetSeen seen[64];
    size_t count;
    unsigned cancels, runs, releases;
    bool refuse;
    /* What run answers: false is "could not be run". With run_stalls, the
     * run plays a server that never finishes: the clock moves a second at
     * each wait until the binding's stop says yes (run_stopped). */
    bool run_ok, run_stalls, run_stopped;
    os64_dom_response_t run_answer;
} FakeNet;

static void net_record(NetSeen *s, const os64_dom_request_t *q)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->url, sizeof(s->url), "%s", q->url != NULL ? q->url : "");
    snprintf(s->method, sizeof(s->method), "%s", q->method != NULL ? q->method : "");
    snprintf(s->referrer, sizeof(s->referrer), "%s", q->referrer != NULL ? q->referrer : "");
    s->headers_length = q->headers_length < sizeof(s->headers) - 1 ? q->headers_length : sizeof(s->headers) - 1;
    if (s->headers_length != 0) memcpy(s->headers, q->headers, s->headers_length);
    s->has_body = q->body != NULL;
    s->body_length = q->body_length < sizeof(s->body) - 1 ? q->body_length : sizeof(s->body) - 1;
    if (s->body_length != 0) memcpy(s->body, q->body, s->body_length);
    s->mode = q->mode;
    s->credentials = q->credentials;
    s->redirect = q->redirect;
    s->preflight = q->preflight;
}

static uint64_t net_start(void *opaque, const os64_dom_request_t *q, uint32_t token)
{
    FakeNet *n = opaque;
    if (n->refuse || n->count == sizeof(n->seen) / sizeof(n->seen[0])) return 0;
    NetSeen *s = &n->seen[n->count++];
    net_record(s, q);
    s->token = token;
    s->id = 1000 + n->count;
    return s->id;
}

static void net_cancel(void *opaque, uint64_t id)
{
    FakeNet *n = opaque;
    n->cancels++;
    for (size_t i = 0; i < n->count; i++)
        if (n->seen[i].id == id) n->seen[i].cancelled = true;
}

static bool net_run(void *opaque, const os64_dom_request_t *q, bool (*stop)(void *), void *stop_ctx,
                    os64_dom_response_t *out)
{
    FakeNet *n = opaque;
    n->runs++;
    if (n->count < sizeof(n->seen) / sizeof(n->seen[0])) net_record(&n->seen[n->count++], q);
    for (int wait = 0; n->run_stalls && wait < 100000; wait++) {
        dom_clock += 1000000;
        if (stop(stop_ctx)) {
            n->run_stopped = true;
            return false;
        }
    }
    if (!n->run_ok) return false;
    *out = n->run_answer;
    return true;
}

static void net_release(void *opaque, os64_dom_response_t *r)
{
    FakeNet *n = opaque;
    (void)r;
    n->releases++;
}

static DomFixture net_fixture_engine(const char *url, FakeNet *net, size_t engine_cap)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.url = url;
    options.now_ms = storage_now;
    DomFixture f = fixture_base(0, 0, engine_cap);
    if (f.runtime != NULL && f.state != NULL) {
        os64_js_outcome_t outcome;
        f.binding = os64_dom_create(f.runtime, f.document, f.state, &options, &outcome);
    }
    if (f.binding != NULL && net != NULL) {
        const os64_dom_network_t network = {net_start, net_cancel, net_run, net_release, net};
        os64_dom_set_network(f.binding, &network);
    }
    return f;
}

static DomFixture net_fixture(const char *url, FakeNet *net)
{
    return net_fixture_engine(url, net, 0);
}

static os64_dom_response_t net_answer_of(int32_t status, const char *url, const char *headers, const char *body)
{
    return (os64_dom_response_t){
        .ok = true, .type = WAY_TYPE_BASIC, .status = status, .status_text = "OK", .url = url,
        .headers = headers, .headers_length = headers != NULL ? strlen(headers) : 0,
        .has_body = body != NULL, .body = body, .body_length = body != NULL ? strlen(body) : 0,
    };
}

/* The host's answer to the request the net saw `which`th, as its task. */
static os64_js_status_t net_answer(DomFixture *f, FakeNet *n, size_t which, const os64_dom_response_t *r)
{
    os64_js_outcome_t outcome;
    os64_js_status_t status = os64_dom_fetched(f->binding, n->seen[which].token, r, &outcome);
    if (status != OS64_JS_OK)
        fprintf(stderr, "fetched: status=%d message=%s\n%s\n", status, outcome.message, outcome.stack_trace);
    return status;
}

/* A global's value as text (empty when it has none). */
static void global_text(DomFixture *f, const char *name, char *out, size_t cap)
{
    os64_js_outcome_t outcome;
    JSContext *ctx = os64_js_context(f->runtime, OS64_JS_ABI_ID, &outcome);
    out[0] = '\0';
    if (ctx == NULL) return;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue value = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    const char *text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (text == NULL) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    snprintf(out, cap, "%s", text);
    JS_FreeCString(ctx, text);
}

/* ── One origin, three spellings ─────────────────────────────────────── */

/* d_origin is libway's way_origin_serialize: what location.origin reads,
 * what the storage provider is told (and so what names a store's file),
 * and what the fetch job judges are one string for every kind of address:
 * default and explicit ports, upper case, IP literals, opaque origins. */
static void dom_fetch_origin_cases(void)
{
    static const char *const kUrls[] = {
        "http://example.test/", "http://example.test:80/a", "http://example.test:8080/p?q#f",
        "HTTP://EXAMPLE.TEST/", "https://example.test:443/", "https://example.test:444/",
        "http://127.0.0.1:8000/", "http://[::1]/", "http://[2001:db8::1]:81/x", "https://user:pw@example.test/",
        "ws://example.test/", "ftp://example.test:21/", "file:///home/page.html", "about:blank",
        "data:text/html,hi", "blob:http://example.test/uuid",
    };
    for (size_t i = 0; i < sizeof(kUrls) / sizeof(kUrls[0]); i++) {
        FakeStore store = {0};
        DomFixture f = storage_fixture(kUrls[i], &store);
        if (!fixture_ready(&f)) { fake_store_free(&store); return; }
        dom_script(&f, "globalThis.o=location.origin;try{localStorage.getItem('k')}catch(e){}", "origin: read");
        char page[256];
        global_text(&f, "o", page, sizeof(page));
        way_origin_t origin;
        way_origin_of(kUrls[i], 0, &origin);
        char way[WAY_ORIGIN_TEXT_MAX];
        check(way_origin_serialize(&origin, way, sizeof(way)), "origin: libway serializes it");
        if (strcmp(page, way) != 0) fprintf(stderr, "origin %s: location %s, libway %s\n", kUrls[i], page, way);
        check(strcmp(page, way) == 0, "origin: location.origin is libway's");
        /* An opaque origin has no storage; a tuple's is told the same. */
        if (store.identities != 0) {
            if (strcmp(store.origin, way) != 0) fprintf(stderr, "origin %s: store %s\n", kUrls[i], store.origin);
            check(strcmp(store.origin, way) == 0, "origin: the store is named by libway's origin");
        } else check(strcmp(way, "null") == 0, "origin: only an opaque origin has no store");
        fixture_free(&f);
        fake_store_free(&store);
    }
}

/* ── Headers ─────────────────────────────────────────────────────────── */

static void dom_fetch_headers_cases(void)
{
    DomFixture f = net_fixture("http://example.test/page", NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, FETCH_ASSERT
        "var h=new Headers();assert(h instanceof Headers&&Object.prototype.toString.call(h)==='[object Headers]');"
        "h.append('B','2');h.append('a','1');h.append('b',' 3\\t');"
        "assert(h.get('b')==='2, 3'&&h.get('A')==='1'&&h.get('c')===null&&h.has('B')&&!h.has('c'));"
        "assert([...h].join('|')==='a,1|b,2, 3');"
        "assert([...h.keys()].join()==='a,b'&&[...h.values()].join('|')==='1|2, 3');"
        "h.set('B','x');assert(h.get('b')==='x');h.delete('A');assert(!h.has('a')&&[...h].length===1);"
        "throws('TypeError',()=>h.append('bad name','v'));throws('TypeError',()=>h.append('n','a\\nb'));"
        "throws('TypeError',()=>h.append('n','\\u0100'));throws('TypeError',()=>h.get('a b'));",
        "headers: append, get, set, delete, iteration sorted and combined");
    dom_script(&f, FETCH_ASSERT
        "var h=new Headers([['Set-Cookie','a=1'],['set-cookie','b=2'],['x','1']]);"
        "assert([...h].join('|')==='set-cookie,a=1|set-cookie,b=2|x,1');"
        "assert(h.getSetCookie().join()==='a=1,b=2'&&h.get('set-cookie')==='a=1, b=2');"
        "var r=new Headers({z:'1',y:'2'});assert([...r.keys()].join()==='y,z');"
        "throws('TypeError',()=>new Headers([['a']]));throws('TypeError',()=>new Headers([['a','b','c']]));"
        "throws('TypeError',()=>new Headers(1));"
        "var c=new Headers(r);assert(c.get('z')==='1'&&c!==r);"
        "var seen=[];new Headers({x:'1'}).forEach(function(v,k,o){seen.push(k+'='+v,o instanceof Headers)});"
        "assert(seen.join()==='x=1,true');",
        "headers: set-cookie apart, records, sequences, another Headers, forEach");
    /* A walk reads the sorted list as it stands at each step. */
    dom_script(&f, FETCH_ASSERT
        "var h=new Headers({b:'2',d:'4'});var out=[];"
        "for(const [k] of h){out.push(k);if(k==='b')h.append('c','3');}"
        "assert(out.join()==='b,c,d','a name added behind the walk is reached');"
        "var g=new Headers({a:'1',b:'2',c:'3'});out=[];for(const [k] of g){out.push(k);if(k==='a')g.delete('a');}"
        "assert(out.join()==='a,c','a name deleted under the walk moves it on');",
        "headers: iteration under change");
    /* Guards: what a Request's and a Response's headers refuse. */
    dom_script(&f, FETCH_ASSERT
        "var q=new Request('/x',{headers:{'Accept-Charset':'x','Cookie':'c','X-Fine':'1','Proxy-A':'p','Sec-B':'s'}});"
        "assert([...q.headers.keys()].join()==='x-fine','forbidden request headers go silently');"
        "q.headers.set('Host','h');assert(!q.headers.has('host'));"
        "q.headers.set('X-HTTP-Method-Override','CONNECT');assert(!q.headers.has('x-http-method-override'));"
        "var p=new Response('',{headers:{'Set-Cookie':'a','X':'1'}});assert(!p.headers.has('set-cookie')&&p.headers.has('x'));"
        "var e=Response.error();throws('TypeError',()=>e.headers.append('a','b'));"
        "var n=new Request('/x',{mode:'no-cors',headers:{'Accept':'text/html','X-Not':'1','Content-Type':'text/plain'}});"
        "assert([...n.headers.keys()].join()==='accept,content-type','no-cors keeps the safelisted');"
        "n.headers.append('Content-Type','application/json');assert(n.headers.get('content-type')==='text/plain');"
        "n.headers.append('Range','bytes=0-1');assert(!n.headers.has('range'));",
        "headers: request, response, immutable and no-cors guards");
    /* A header the binding writes itself meets the same guard: a no-cors
     * request's body type that is not safelisted is dropped, as the page's
     * own would be (a JSON CSRF otherwise: an opaque POST, no preflight,
     * Content-Type: application/json); a safelisted one stays. */
    dom_script(&f, FETCH_ASSERT
        "var j=new Request('http://other.test/api',{mode:'no-cors',method:'POST',"
        "body:new Blob(['{}'],{type:'application/json'})});assert(!j.headers.has('content-type'),j.headers.get('content-type'));"
        "var t=new Request('http://other.test/api',{mode:'no-cors',method:'POST',body:'x'});"
        "assert(t.headers.get('content-type')==='text/plain;charset=UTF-8');"
        "var c=new Request('/x',{method:'POST',body:new Blob(['{}'],{type:'application/json'})});"
        "assert(c.headers.get('content-type')==='application/json');",
        "headers: the binding's own headers through the guard");
    fixture_free(&f);
}

/* ── Request and Response, as a page makes them ──────────────────────── */

static void dom_fetch_object_cases(void)
{
    DomFixture f = net_fixture("http://example.test/dir/page", NULL);
    if (!fixture_ready(&f)) return;
    /* DOMException's code is WebIDL's names table: a name with a legacy
     * code has it, and the three codes the table names no error for
     * (2, 6, 16) have their constants and no name. */
    dom_script(&f, FETCH_ASSERT
        "assert(new DOMException('','NotFoundError').code===8&&new DOMException('','DataCloneError').code===25);"
        "assert(new DOMException('','QuotaExceededError').code===22&&new DOMException('','EncodingError').code===0);"
        "for(const n of ['DOMStringSizeError','NoDataAllowedError','ValidationError','Bogus'])"
        "assert(new DOMException('',n).code===0,n);"
        "assert(DOMException.DOMSTRING_SIZE_ERR===2&&DOMException.NO_DATA_ALLOWED_ERR===6&&DOMException.VALIDATION_ERR===16);"
        "assert(new DOMException().VALIDATION_ERR===16);",
        "DOMException: codes by WebIDL's names table");
    /* A Blob's parts are converted once, before its options: a generator
     * among them is iterated once, and "native" endings apply to what it
     * gave; the options' getters run after the parts' toString. */
    dom_script(&f, FETCH_ASSERT
        "var g=function*(){yield 'a\\r\\nb\\rc';yield new Uint8Array([13,10]);};"
        "assert(new Blob(g(),{endings:'native'}).size===7,'native '+new Blob(g(),{endings:'native'}).size);"
        "assert(new Blob(g()).size===8&&new Blob(g(),{endings:'transparent'}).size===8);"
        "var order=[];new Blob([{toString(){order.push('part');return 'x'}}],{get endings(){order.push('endings');return 'native'}});"
        "assert(order.join()==='part,endings',order.join());"
        "assert(new File(g(),'n',{endings:'native'}).size===7);",
        "Blob: parts converted once, endings applied to them");
    dom_script(&f, FETCH_ASSERT
        "var q=new Request('a?b#c');assert(q.url==='http://example.test/dir/a?b#c'&&q.method==='GET');"
        "assert(q.mode==='cors'&&q.credentials==='same-origin'&&q.redirect==='follow'&&q.cache==='default');"
        "assert(q.referrer==='about:client'&&q.referrerPolicy===''&&q.integrity===''&&!q.keepalive&&q.destination==='');"
        "assert(q.signal instanceof AbortSignal&&!q.signal.aborted&&q.body===null&&!q.bodyUsed&&q.duplex==='half');"
        "assert(new Request('/x',{method:'post'}).method==='POST'&&new Request('/x',{method:'Patch'}).method==='Patch');"
        "throws('TypeError',()=>new Request('/x',{method:'CONNECT'}));throws('TypeError',()=>new Request('/x',{method:'a b'}));"
        "throws('TypeError',()=>new Request('/x',{mode:'navigate'}));"
        "throws('TypeError',()=>new Request('/x',{mode:'no-cors',method:'PUT'}));"
        "throws('TypeError',()=>new Request('/x',{body:'b'}));throws('TypeError',()=>new Request('/x',{method:'HEAD',body:'b'}));"
        "throws('TypeError',()=>new Request('http://a b/'));throws('TypeError',()=>new Request('/x',{window:{}}));"
        "throws('TypeError',()=>new Request('/x',{cache:'only-if-cached'}));"
        "throws('TypeError',()=>new Request('/x',{credentials:'bogus'}));"
        "var r=new Request('/x',{referrer:''});assert(r.referrer==='');"
        "r=new Request('/x',{referrer:'/elsewhere'});assert(r.referrer==='http://example.test/elsewhere');"
        "r=new Request('/x',{referrer:'http://other.test/'});assert(r.referrer==='about:client');"
        "r=new Request('/x',{referrerPolicy:'origin'});assert(r.referrerPolicy==='origin');"
        "r=new Request('/x',{referrer:'about:client'});assert(r.referrer==='about:client');",
        "request: the constructor's defaults and refusals");
    /* WebIDL reads a dictionary's members in name order, each once. */
    dom_script(&f, FETCH_ASSERT
        "var order=[];var init={};"
        "for(const k of ['window','signal','referrerPolicy','referrer','redirect','priority','mode','method',"
        "'keepalive','integrity','headers','duplex','credentials','cache','body'])"
        "Object.defineProperty(init,k,{get(){order.push(k);return undefined}});"
        "new Request('/x',init);"
        "assert(order.join()==='body,cache,credentials,duplex,headers,integrity,keepalive,method,mode,priority,"
        "redirect,referrer,referrerPolicy,signal,window',order.join());",
        "request: RequestInit read in name order");
    dom_script(&f, FETCH_ASSERT
        "globalThis.log=[];"
        "var q=new Request('/x',{method:'POST',body:'hi \\u00e9'});"
        "assert(q.headers.get('content-type')==='text/plain;charset=UTF-8'&&Object.prototype.toString.call(q.body)==='[object ReadableStream]');"
        "var c=q.clone();assert(c.url===q.url&&c.headers.get('content-type')===q.headers.get('content-type'));"
        "q.text().then(t=>log.push('q:'+t));c.arrayBuffer().then(b=>log.push('c:'+b.byteLength));"
        "assert(q.bodyUsed);throws('TypeError',()=>q.clone());"
        "new Request('/x',{method:'POST',body:new URLSearchParams('a=1&b=2')}).headers.get('content-type')==="
        "'application/x-www-form-urlencoded;charset=UTF-8'||log.push('bad params type');"
        "var b=new Request('/x',{method:'POST',body:new Blob(['x'],{type:'Text/Plain'})});"
        "assert(b.headers.get('content-type')==='text/plain');"
        "assert(!new Request('/x',{method:'POST',body:new Uint8Array([1,2])}).headers.has('content-type'));"
        "var fd=new FormData();fd.append('k','v');"
        "assert(/^multipart\\/form-data; boundary=/.test(new Request('/x',{method:'POST',body:fd}).headers.get('content-type')));"
        "var from=new Request('/x',{method:'POST',body:'once'});var to=new Request(from);"
        "assert(from.bodyUsed&&!to.bodyUsed);throws('TypeError',()=>new Request(from));",
        "request: bodies, their types, clone and taking an input's body");
    dom_script(&f, FETCH_ASSERT "assert(log.join()==='q:hi \\u00e9,c:5',log.join());", "request: bodies read");
    dom_script(&f, FETCH_ASSERT
        "globalThis.log=[];"
        "var r=new Response('{\"a\":1}',{status:201,statusText:'Made',headers:{'X':'1'}});"
        "assert(r.status===201&&r.ok&&r.statusText==='Made'&&r.type==='default'&&r.url===''&&!r.redirected);"
        "assert(r.headers.get('content-type')==='text/plain;charset=UTF-8');"
        "r.json().then(j=>log.push('json:'+j.a));"
        "throws('RangeError',()=>new Response('',{status:199}));throws('RangeError',()=>new Response('',{status:600}));"
        "throws('TypeError',()=>new Response('x',{status:204}));throws('TypeError',()=>new Response('',{statusText:'a\\nb'}));"
        "assert(new Response(null,{status:204}).body===null);"
        "var e=Response.error();assert(e.type==='error'&&e.status===0&&e.statusText===''&&e.body===null);"
        "var d=Response.redirect('/to',301);assert(d.status===301&&d.headers.get('location')==='http://example.test/to');"
        "throws('RangeError',()=>Response.redirect('/to',200));throws('TypeError',()=>Response.redirect('http://a b/'));"
        "var j=Response.json({b:[1]},{status:202});assert(j.status===202&&j.headers.get('content-type')==='application/json');"
        "j.text().then(t=>log.push('text:'+t));"
        "throws('TypeError',()=>Response.json(undefined));"
        "new Response(new Uint8Array([104,105])).text().then(t=>log.push('bytes:'+t));"
        "new Response('abc').bytes().then(b=>log.push('u8:'+(b instanceof Uint8Array)+b.length));"
        "new Response('abc',{headers:{'content-type':'Text/Weird; charset=x'}}).blob().then(b=>log.push('blob:'+b.type+b.size));"
        "new Response('a=1&b=2',{headers:{'content-type':'application/x-www-form-urlencoded'}}).formData()"
        ".then(f=>log.push('form:'+f.get('b')));"
        "new Response('x').formData().catch(e=>log.push('form-refused:'+e.name));"
        "new Response('{').json().catch(e=>log.push('json-bad:'+e.name));",
        "response: the constructor, error, redirect, json and every body reader");
    dom_script(&f, FETCH_ASSERT
        "var want='json:1,text:{\"b\":[1]},bytes:hi,u8:true3,blob:text/weird; charset=x3,form:2,"
        "form-refused:TypeError,json-bad:SyntaxError';assert(log.join()===want,log.join());",
        "response: bodies read");
    /* The stream a body is: one chunk, then done; a reader locks it. */
    dom_script(&f, FETCH_ASSERT
        "globalThis.log=[];var r=new Response('chunk');var s=r.body;assert(s===r.body&&!s.locked);"
        "var rd=s.getReader();assert(s.locked&&!r.bodyUsed);throws('TypeError',()=>s.getReader());"
        "r.text().catch(e=>log.push('locked:'+e.name));throws('TypeError',()=>r.clone());"
        "rd.read().then(a=>{log.push(a.done+':'+new TextDecoder().decode(a.value));return rd.read()})"
        ".then(a=>{log.push(a.done+':'+a.value);rd.releaseLock();log.push(s.locked)});"
        "assert(typeof ReadableStream==='undefined'||true);"
        "throws('RangeError',()=>new Response('x').body.getReader({mode:'byob'}));",
        "stream: one chunk, then done");
    /* A body method read it whole with a reader it keeps; a FormData body
     * reads back as one, and an empty one is no bytes. */
    dom_script(&f, FETCH_ASSERT
        "var r=new Response('x');r.text();assert(r.body.locked);throws('TypeError',()=>r.body.getReader());"
        "var fd=new FormData();fd.append('k','v, w');fd.append('f',new File(['bytes'],'n.txt',{type:'text/plain'}));"
        "new Request('/x',{method:'POST',body:fd}).formData().then(g=>{log.push('form:'+g.get('k')+'/'+g.get('f').name)});"
        "new Request('/x',{method:'POST',body:new FormData()}).text().then(t=>log.push('empty:'+t.length));",
        "stream: a body method locks it; FormData both ways");
    dom_script(&f, FETCH_ASSERT "assert(log.join()==='locked:TypeError,false:chunk,true:undefined,false,form:v, w/n.txt,empty:0',"
               "log.join());",
               "stream: read");
    /* A part's parameters are read without regard to case, by one parse:
     * Filename= makes a File, filename="" a File named "", name="" an
     * entry named "". */
    dom_script(&f,
        "globalThis.mlog=[];var m='--b\\r\\nContent-Disposition: form-data; Name=\"a\"; Filename=\"a.txt\"\\r\\n"
        "Content-Type: text/plain\\r\\n\\r\\nA\\r\\n--b\\r\\nContent-Disposition: form-data; name=\"e\"; filename=\"\"\\r\\n"
        "\\r\\nE\\r\\n--b\\r\\nContent-Disposition: form-data; name=\"\"\\r\\n\\r\\nN\\r\\n--b--\\r\\n';"
        "new Response(m,{headers:{'content-type':'multipart/form-data; boundary=b'}}).formData().then(g=>{"
        "var a=g.get('a'),e=g.get('e');mlog.push((a instanceof File)+':'+a.name+':'+(e instanceof File)+':'+e.name+':'+g.get(''))})"
        ".catch(x=>mlog.push('refused:'+x.message));",
        "formData: a part's parameters");
    dom_script(&f, FETCH_ASSERT "assert(mlog.join()==='true:a.txt:true::N',mlog.join());", "formData: parameters read by one parse");
    fixture_free(&f);
}

/* ── fetch() through the host ────────────────────────────────────────── */

static void dom_fetch_network_cases(void)
{
    FakeNet net = {0};
    DomFixture f = net_fixture("http://example.test/dir/page", &net);
    if (!fixture_ready(&f)) return;
    dom_script(&f, FETCH_ASSERT
        "globalThis.log=[];"
        "fetch('data.json?x',{method:'POST',body:'abc',headers:{'X-One':'1','x-one':'2'},credentials:'include'})"
        ".then(r=>{log.push(r.status,r.type,r.url,r.redirected,r.headers.get('content-type'));"
        "throws('TypeError',()=>r.headers.set('a','b'));return r.json()}).then(j=>log.push(j.v));",
        "fetch: a request goes to the host");
    check(net.count == 1, "fetch: the host starts one request");
    NetSeen *s = &net.seen[0];
    check(!strcmp(s->url, "http://example.test/dir/data.json?x") && !strcmp(s->method, "POST"),
          "fetch: the host gets the parsed URL and the method");
    check(s->has_body && s->body_length == 3 && !memcmp(s->body, "abc", 3), "fetch: the host gets the body");
    check(strstr(s->headers, "X-One: 1\r\n") != NULL && strstr(s->headers, "x-one: 2\r\n") != NULL &&
          strstr(s->headers, "Content-Type: text/plain;charset=UTF-8\r\n") != NULL,
          "fetch: the host gets the author headers as a block, the body's type among them");
    check(s->mode == WAY_MODE_CORS && s->credentials == WAY_CREDENTIALS_INCLUDE && s->redirect == WAY_REDIRECT_FOLLOW &&
          !s->preflight, "fetch: the modes reach the host");
    check(!strcmp(s->referrer, "http://example.test/dir/page"), "fetch: about:client is the page's address");
    check(os64_dom_fetches_pending(f.binding) == 1, "fetch: one request out");
    os64_dom_response_t answer = net_answer_of(200, "http://example.test/dir/data.json?x",
                                               "Content-Type: application/json\r\n", "{\"v\":42}");
    answer.type = WAY_TYPE_CORS;
    answer.redirected = true;
    check(net_answer(&f, &net, 0, &answer) == OS64_JS_OK, "fetch: the answer is a task");
    check(os64_dom_fetches_pending(f.binding) == 0, "fetch: an answered request is not out");
    dom_script(&f, FETCH_ASSERT
        "assert(log.join()==='200,cors,http://example.test/dir/data.json?x,true,application/json,42',log.join());",
        "fetch: the promise resolves with the host's response");
    check(net_answer(&f, &net, 0, &answer) == OS64_JS_OK, "fetch: a second answer for the token does nothing");

    /* A network error rejects with a TypeError; so does a host that would
     * not start the request, and a request this browser will not make. */
    dom_script(&f, FETCH_ASSERT
        "globalThis.log=[];fetch('/fail').catch(e=>log.push('net:'+e.name+'('+e.message+')'));"
        "fetch('/x',{integrity:'sha256-abc'}).catch(e=>log.push('sri:'+e.name));"
        "fetch('/x',{method:'POST',keepalive:true,body:'x'.repeat(65537)}).catch(e=>log.push('keep:'+e.name));"
        "fetch('http://a b/').catch(e=>log.push('url:'+e.name));fetch().catch(e=>log.push('none:'+e.name));",
        "fetch: refusals");
    check(net.count == 2, "fetch: only the network error's request reached the host");
    os64_dom_response_t error = {.ok = false};
    net_answer(&f, &net, 1, &error);
    net.refuse = true;
    dom_script(&f, "fetch('/r').catch(e=>log.push('refused:'+e.name));", "fetch: a host that will not start it");
    net.refuse = false;
    dom_script(&f, FETCH_ASSERT
        "assert(log.join()==='sri:TypeError,keep:TypeError,url:TypeError,none:TypeError,net:TypeError(Failed to fetch),refused:TypeError',"
        "log.join());", "fetch: each refusal rejects");

    /* data: goes to the host as written; the page's base resolves the
     * rest. A no-cors request carries its mode. */
    dom_script(&f, "fetch(' data:,hello ');fetch('/n',{mode:'no-cors',redirect:'manual'});", "fetch: data and no-cors");
    check(!strcmp(net.seen[2].url, "data:,hello"), "fetch: a data: address goes to the host trimmed and unparsed");
    check(net.seen[3].mode == WAY_MODE_NO_CORS && net.seen[3].redirect == WAY_REDIRECT_MANUAL,
          "fetch: no-cors and manual reach the host");
    /* The cold read's JSON CSRF: a credentialed no-cors POST with a body
     * typed application/json reaches the host with no such Content-Type. */
    dom_script(&f, "fetch('http://other.test/api',{mode:'no-cors',method:'POST',credentials:'include',"
               "body:new Blob(['{\"transfer\":1}'],{type:'application/json'})});", "fetch: no-cors JSON");
    check(net.seen[4].mode == WAY_MODE_NO_CORS && strstr(net.seen[4].headers, "application/json") == NULL &&
          strstr(net.seen[4].headers, "Content-Type") == NULL,
          "fetch: a no-cors body's type that is not safelisted never reaches the host");
    /* Drain cancels what is out. */
    unsigned cancels = net.cancels;
    fixture_free(&f);
    check(net.cancels == cancels + 3, "fetch: drain cancels each request out");

    /* No network at all: a TypeError. */
    f = net_fixture("http://example.test/", NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.log=[];fetch('/x').catch(e=>log.push(e.name));", "fetch: no network");
    dom_script(&f, FETCH_ASSERT "assert(log.join()==='TypeError');", "fetch: no network rejects");
    fixture_free(&f);
}

/* ── Aborting ────────────────────────────────────────────────────────── */

static void dom_fetch_abort_cases(void)
{
    FakeNet net = {0};
    DomFixture f = net_fixture("http://example.test/", &net);
    if (!fixture_ready(&f)) return;
    dom_script(&f, FETCH_ASSERT
        "globalThis.log=[];var c=new AbortController();var s=c.signal;"
        "assert(s instanceof AbortSignal&&s instanceof Object&&!s.aborted&&s.reason===undefined);"
        "s.onabort=e=>log.push('onabort:'+e.type+e.isTrusted);s.addEventListener('abort',()=>log.push('listener'));"
        "c.abort();c.abort('again');assert(s.aborted&&s.reason.name==='AbortError'&&s.reason instanceof DOMException);"
        "throws('AbortError',()=>s.throwIfAborted());"
        "assert(AbortSignal.abort('why').reason==='why');"
        "var any=AbortSignal.any([new AbortController().signal,AbortSignal.abort(7)]);assert(any.aborted&&any.reason===7);"
        "var t=AbortSignal.timeout(5);assert(!t.aborted);"
        "throws('TypeError',()=>AbortSignal.any([1]));throws('TypeError',()=>new AbortSignal());",
        "abort: a controller, its signal, abort, any and timeout");
    dom_script(&f, FETCH_ASSERT "assert(log.join()==='onabort:aborttrue,listener',log.join());", "abort: events");
    storage_clock += 10;
    os64_js_outcome_t outcome;
    check(os64_dom_timer_fire(f.binding, storage_clock, &outcome), "abort: the timeout's timer fires");
    dom_script(&f, FETCH_ASSERT "assert(t.aborted&&t.reason.name==='TimeoutError');", "abort: timeout aborts");

    /* Before: the fetch never starts. While out: the promise rejects and
     * the host cancels. After: the body's read rejects. */
    dom_script(&f, FETCH_ASSERT
        "globalThis.log=[];fetch('/x',{signal:AbortSignal.abort()}).catch(e=>log.push('before:'+e.name));"
        "globalThis.c2=new AbortController();fetch('/y',{signal:c2.signal}).catch(e=>log.push('during:'+e.name));"
        "globalThis.c3=new AbortController();fetch('/z',{signal:c3.signal}).then(r=>{globalThis.r3=r});"
        "c2.abort();",
        "abort: before and during");
    check(net.count == 2 && net.seen[0].cancelled && !net.seen[1].cancelled, "abort: during cancels with the host");
    os64_dom_response_t answer = net_answer_of(200, "http://example.test/z", "", "body");
    net_answer(&f, &net, 1, &answer);
    dom_script(&f, "c3.abort(new Error('late'));r3.text().catch(e=>log.push('after:'+e.message));", "abort: after");
    dom_script(&f, FETCH_ASSERT "assert(log.join()==='before:AbortError,during:AbortError,after:late',log.join());",
               "abort: each point");
    net_answer(&f, &net, 0, &answer);
    dom_script(&f, FETCH_ASSERT "assert(log.length===3,'an aborted request answers nothing');", "abort: late answer");

    /* A Request's signal follows its input's. */
    dom_script(&f, FETCH_ASSERT
        "var c=new AbortController();var q=new Request('/x',{signal:c.signal});var q2=new Request(q);"
        "assert(q.signal!==c.signal&&q2.signal!==q.signal);c.abort('r');"
        "assert(q.signal.aborted&&q.signal.reason==='r'&&q2.signal.aborted);",
        "abort: a request's signal follows");
    /* The source's event fires before its dependents', and each dependent
     * is already aborted when it does; a chain as deep as a page likes is
     * walked, not recursed. */
    dom_script(&f, FETCH_ASSERT
        "var order=[];var c=new AbortController();var q=new Request('/x',{signal:c.signal});"
        "c.signal.onabort=()=>order.push('source:'+q.signal.aborted);q.signal.onabort=()=>order.push('dependent');"
        "c.abort();assert(order.join()==='source:true,dependent',order.join());"
        "var root=new AbortController();var s=root.signal;for(let i=0;i<20000;i++)s=AbortSignal.any([s]);"
        "root.abort('deep');assert(s.aborted&&s.reason==='deep');",
        "abort: source first, then dependents, however deep");
    /* After an answer: the reason, every time; an empty body is closed
     * before an abort can error it. */
    dom_script(&f, "globalThis.c4=new AbortController();fetch('/w',{signal:c4.signal}).then(r=>{globalThis.r4=r});"
               "globalThis.c5=new AbortController();fetch('/v',{signal:c5.signal}).then(r=>{globalThis.r5=r});",
               "abort: two more requests");
    os64_dom_response_t full = net_answer_of(200, "http://example.test/w", "", "full");
    os64_dom_response_t none = net_answer_of(200, "http://example.test/v", "", "");
    net_answer(&f, &net, net.count - 2, &full);
    net_answer(&f, &net, net.count - 1, &none);
    dom_script(&f, "globalThis.log=[];c4.abort();c5.abort();"
               "r4.text().catch(e=>log.push('1:'+e.name));r4.text().catch(e=>log.push('2:'+e.name));"
               "r5.text().then(t=>log.push('empty:'+t.length));", "abort: after the answer");
    dom_script(&f, FETCH_ASSERT "assert(log.join()==='1:AbortError,2:AbortError,empty:0',log.join());",
               "abort: the reason twice, and an empty body read");
    fixture_free(&f);
}

/* The dependents a global signal's weak set links, after a collection. */
static size_t weak_dependents(DomFixture *f, const char *name)
{
    os64_js_outcome_t outcome;
    JSContext *ctx = os64_js_context(f->runtime, OS64_JS_ABI_ID, &outcome);
    JS_RunGC(JS_GetRuntime(ctx));
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue signal = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    size_t n = d_signal_dependents(signal);
    JS_FreeValue(ctx, signal);
    return n;
}

/* DOM'S WEAK SETS (abort.c): a source holds its dependents by C link, not
 * reference, so a dependent nobody can observe is collected however many
 * a page makes; DOM's GC clause holds one with an abort listener; and the
 * hold is released at exactly three points, each pinned here. */
static void dom_weak_set_cases(void)
{
    DomFixture f = net_fixture("http://example.test/", NULL);
    if (!fixture_ready(&f)) return;
    /* The bound: ten thousand dependents of one source, dropped, are
     * gone; a Request's signal per request likewise. */
    dom_script(&f, "globalThis.root=new AbortController();globalThis.rs=root.signal;"
               "for(let i=0;i<10000;i++)AbortSignal.any([rs]);for(let i=0;i<10000;i++)new Request('/x',{signal:rs});",
               "weak: ten thousand dependents, dropped");
    check(weak_dependents(&f, "rs") == 0, "weak: dropped dependents leave the source's set");
    /* A dependent the page keeps is linked, and an abort reaches it. */
    dom_script(&f, "globalThis.kept=AbortSignal.any([rs]);", "weak: a dependent kept");
    check(weak_dependents(&f, "rs") == 1, "weak: a kept dependent stays linked");
    /* Held for its listener: dropped by the page, still there, and heard. */
    dom_script(&f, "globalThis.heard=0;(function(){AbortSignal.any([rs]).onabort=function(){heard++}})();",
               "weak: a dependent only its listener wants");
    check(weak_dependents(&f, "rs") == 2, "weak: a dependent with an abort listener is held");
    /* Release point three, the source aborts (and with it point one, each
     * dependent is aborted): the held one is heard, then let go. */
    dom_script(&f, FETCH_ASSERT "root.abort();assert(heard===1&&kept.aborted,'heard '+heard);", "weak: the source aborts");
    dom_script(&f, "globalThis.kept=undefined;", "weak: the kept one dropped");
    check(weak_dependents(&f, "rs") == 0, "weak: a source's abort releases what it held");
    /* Release point two, the last abort listener goes. */
    dom_script(&f, "globalThis.r2=new AbortController();globalThis.s2=r2.signal;globalThis.f2=function(){};"
               "globalThis.d2=AbortSignal.any([s2]);d2.addEventListener('abort',f2);d2.addEventListener('abort',function(){},{once:false});",
               "weak: a dependent with two listeners");
    dom_script(&f, "d2.onabort=null;globalThis.d2held=d2;globalThis.d2=undefined;", "weak: still listened");
    dom_script(&f, "d2held=undefined;", "weak: dropped while listened");
    check(weak_dependents(&f, "s2") == 1, "weak: a listened dependent stays held");
    dom_script(&f, "globalThis.d3=AbortSignal.any([s2]);var h=function(){};d3.addEventListener('abort',h);"
               "d3.removeEventListener('abort',h);d3=undefined;", "weak: the last listener goes");
    check(weak_dependents(&f, "s2") == 1, "weak: a dependent whose last listener went is let go");
    /* Release point one, the dependent is aborted (by another source)
     * while this source lives on. */
    dom_script(&f, "globalThis.other=new AbortController();"
               "(function(){AbortSignal.any([s2,other.signal]).onabort=function(){}})();",
               "weak: a dependent of two sources");
    check(weak_dependents(&f, "s2") == 2, "weak: held for its listener");
    dom_script(&f, "other.abort();", "weak: aborted by its other source");
    check(weak_dependents(&f, "s2") == 1, "weak: a dependent aborted is let go by every source");
    /* DOM's set is ordered: the dependents' events fire in link order. */
    dom_script(&f, FETCH_ASSERT
        "var o=new AbortController(),seen=[];var a=AbortSignal.any([o.signal]),b=AbortSignal.any([o.signal]);"
        "a.onabort=()=>seen.push('a');b.onabort=()=>seen.push('b');o.abort();assert(seen.join()==='a,b',seen.join());",
        "weak: dependents abort in the order they were linked");
    fixture_free(&f);
}

/* The steps one script's links take (abort.c's d_signal_steps), after a
 * setup that is not counted. */
static uint64_t signal_steps_for(DomFixture *f, const char *setup, const char *measured)
{
    dom_script(f, setup, "bound: signals made");
    uint64_t before = d_signal_steps;
    dom_script(f, measured, "bound: signals linked");
    return d_signal_steps - before;
}

/* What a page can size in a signal's links, held to linear by count: an
 * AbortSignal.any of distinct sources, one of N copies of a dependent
 * with M sources (each flattened to the same M), and a Request following
 * such a dependent. Each grows its input sixteen to two hundred and fifty
 * six times and holds the steps to twice that, so a walk of a list per
 * link fails by count, not by a clock. */
static void dom_signal_bound_cases(void)
{
    /* Room for 65,536 controllers and their signals. */
    DomFixture f = net_fixture_engine("http://example.test/", NULL, (size_t)256 << 20);
    if (!fixture_ready(&f)) return;
    char setup[512], measured[256];
    uint64_t steps[2];
    /* AbortSignal.any of 256, then of 65,536 (D_SEQUENCE_MAX), distinct. */
    static const size_t kDistinct[2] = {256, 65536};
    for (int i = 0; i < 2; i++) {
        snprintf(setup, sizeof(setup),
                 "globalThis.cs=[];for(let i=0;i<%zu;i++)cs.push(new AbortController().signal);", kDistinct[i]);
        steps[i] = signal_steps_for(&f, setup, "globalThis.big=AbortSignal.any(cs);big=undefined;cs=undefined;");
    }
    fprintf(stderr, "bound: any of 256 distinct %llu steps, of 65536 %llu\n",
            (unsigned long long)steps[0], (unsigned long long)steps[1]);
    check(steps[0] > 0 && steps[1] <= steps[0] * 256 * 2, "bound: AbortSignal.any is linear in its sources");
    /* AbortSignal.any(Array(N).fill(d)), d a dependent of M sources: N and
     * M 64, then 1,024. */
    static const size_t kFill[2] = {64, 1024};
    for (int i = 0; i < 2; i++) {
        snprintf(setup, sizeof(setup),
                 "globalThis.cs=[];for(let i=0;i<%zu;i++)cs.push(new AbortController().signal);"
                 "globalThis.dd=AbortSignal.any(cs);globalThis.fill=Array(%zu).fill(dd);", kFill[i], kFill[i]);
        steps[i] = signal_steps_for(&f, setup, "globalThis.big=AbortSignal.any(fill);big=undefined;");
    }
    fprintf(stderr, "bound: any of 64 x a 64-source dependent %llu steps, of 1024 x 1024 %llu\n",
            (unsigned long long)steps[0], (unsigned long long)steps[1]);
    check(steps[0] > 0 && steps[1] <= steps[0] * 16 * 2,
          "bound: AbortSignal.any of copies of a dependent is linear in the copies and its sources");
    /* new Request following a dependent of M sources: 64, then 4,096. */
    static const size_t kRequest[2] = {64, 4096};
    for (int i = 0; i < 2; i++) {
        snprintf(setup, sizeof(setup),
                 "globalThis.cs=[];for(let i=0;i<%zu;i++)cs.push(new AbortController().signal);"
                 "globalThis.dd=AbortSignal.any(cs);", kRequest[i]);
        snprintf(measured, sizeof(measured), "new Request('/x',{signal:dd});");
        steps[i] = signal_steps_for(&f, setup, measured);
    }
    fprintf(stderr, "bound: a Request following a 64-source dependent %llu steps, a 4096-source one %llu\n",
            (unsigned long long)steps[0], (unsigned long long)steps[1]);
    check(steps[0] > 0 && steps[1] <= steps[0] * 64 * 2, "bound: a Request's signal is linear in its sources");
    dom_script(&f, "cs=undefined;dd=undefined;fill=undefined;", "bound: signals dropped");
    fixture_free(&f);
}

/* ── XMLHttpRequest ──────────────────────────────────────────────────── */

/* The event order WPT's xhr/resources/xmlhttprequest-event-order.js
 * records, which the xhr event-order tests hold a browser to. */
#define XHR_RECORDER "function rec(x){globalThis.ev=[];x.addEventListener('readystatechange',()=>ev.push(x.readyState));" \
    "for(const t of ['loadstart','progress','abort','timeout','error','load','loadend']){" \
    "x.addEventListener(t,e=>ev.push(e.type+'('+e.loaded+','+e.total+','+e.lengthComputable+')'));" \
    "x.upload.addEventListener(t,e=>ev.push('upload.'+e.type+'('+e.loaded+','+e.total+','+e.lengthComputable+')'));}}"

static void dom_xhr_cases(void)
{
    FakeNet net = {0};
    DomFixture f = net_fixture("http://example.test/dir/page", &net);
    if (!fixture_ready(&f)) return;
    dom_script(&f, FETCH_ASSERT
        "var x=new XMLHttpRequest();assert(x instanceof XMLHttpRequestEventTarget&&x.upload instanceof XMLHttpRequestUpload);"
        "assert(x.readyState===0&&XMLHttpRequest.DONE===4&&x.OPENED===1&&x.status===0&&x.statusText===''&&x.response==='');"
        "assert(x.responseType===''&&x.timeout===0&&!x.withCredentials&&x.responseURL===''&&x.getAllResponseHeaders()==='');"
        "throws('InvalidStateError',()=>x.send());throws('InvalidStateError',()=>x.setRequestHeader('a','b'));"
        "throws('SyntaxError',()=>x.open('a b','/x'));throws('SecurityError',()=>x.open('TRACE','/x'));"
        "throws('SyntaxError',()=>x.open('GET','http://a b/'));"
        "x.responseType='bogus';assert(x.responseType==='');x.responseType='json';"
        "throws('InvalidAccessError',()=>x.open('GET','/x',false));x.responseType='';"
        "x.open('get','/x');assert(x.readyState===1);"
        "x.setRequestHeader('X-A','1');x.setRequestHeader('x-a',' 2 ');x.setRequestHeader('Cookie','c');"
        "throws('SyntaxError',()=>x.setRequestHeader('a b','1'));throws('SyntaxError',()=>x.setRequestHeader('a','a\\nb'));"
        "throws('TypeError',()=>x.setRequestHeader('a','\\u0100'));"
        "var p=new ProgressEvent('p',{loaded:-1,total:3,lengthComputable:true,bubbles:true});"
        "assert(p instanceof Event&&p.loaded===-1&&p.total===3&&p.lengthComputable&&p.bubbles);assert(new ProgressEvent('p',{loaded:1.5}).loaded===1.5);throws('TypeError',()=>new ProgressEvent('p',{total:NaN}));"
        "throws('TypeError',()=>ProgressEvent.prototype.loaded);",
        "xhr: the surface and its refusals");

    /* Async, with an upload: WPT's send-response-event-order. */
    dom_script(&f, XHR_RECORDER
        "var x=new XMLHttpRequest();rec(x);x.open('POST','content.py');globalThis.xr=x;"
        "x.withCredentials=true;x.send('Test Message');",
        "xhr: an asynchronous POST");
    size_t last = net.count - 1;
    NetSeen *s = &net.seen[last];
    check(!strcmp(s->url, "http://example.test/dir/content.py") && !strcmp(s->method, "POST") && s->preflight &&
          s->credentials == WAY_CREDENTIALS_INCLUDE && strstr(s->headers, "Content-Type: text/plain;charset=UTF-8") != NULL,
          "xhr: the request reaches the host, an upload listener asking for a preflight");
    os64_dom_response_t answer = net_answer_of(200, "http://example.test/dir/content.py#frag",
                                               "Content-Length: 12\r\nX-B: 2\r\nx-a: 1\r\nX-A: 3\r\n", "Test Message");
    net_answer(&f, &net, last, &answer);
    dom_script(&f, FETCH_ASSERT
        "var want=[1,'loadstart(0,0,false)','upload.loadstart(0,12,true)','upload.progress(12,12,true)',"
        "'upload.load(12,12,true)','upload.loadend(12,12,true)',2,3,'progress(12,12,true)',4,'load(12,12,true)',"
        "'loadend(12,12,true)'].join();assert(ev.join()===want,ev.join());"
        "assert(xr.status===200&&xr.statusText==='OK'&&xr.responseText==='Test Message'&&xr.response==='Test Message');"
        "assert(xr.responseURL==='http://example.test/dir/content.py');"
        "assert(xr.getResponseHeader('x-a')==='1, 3'&&xr.getResponseHeader('nope')===null);"
        "assert(xr.getAllResponseHeaders()==='content-length: 12\\r\\nx-a: 1, 3\\r\\nx-b: 2\\r\\n',xr.getAllResponseHeaders());"
        "throws('InvalidStateError',()=>xr.responseType='text');throws('InvalidStateError',()=>xr.overrideMimeType('a/b'));",
        "xhr: the events in order, and the response");

    /* A network error: send-no-response's CORS failure. */
    dom_script(&f, XHR_RECORDER "var x=new XMLHttpRequest();rec(x);x.open('POST','/c');x.send('Test Message');",
               "xhr: a request that fails");
    os64_dom_response_t failure = {.ok = false};
    net_answer(&f, &net, net.count - 1, &failure);
    dom_script(&f, FETCH_ASSERT
        "var want=[1,'loadstart(0,0,false)','upload.loadstart(0,12,true)',4,'upload.error(0,0,false)',"
        "'upload.loadend(0,0,false)','error(0,0,false)','loadend(0,0,false)'].join();assert(ev.join()===want,ev.join());",
        "xhr: a network error's events");

    /* abort() inside loadstart: abort-event-order. */
    dom_script(&f, XHR_RECORDER
        "var x=new XMLHttpRequest();rec(x);x.open('POST','/a');"
        "x.addEventListener('loadstart',()=>x.abort());x.send('Test Message');"
        "var want=[1,'loadstart(0,0,false)',4,'upload.abort(0,0,false)','upload.loadend(0,0,false)',"
        "'abort(0,0,false)','loadend(0,0,false)'];assert(ev.join()===want.join(),ev.join());assert(x.readyState===0);",
        "xhr: abort inside loadstart");

    /* An empty answer: no LOADING. */
    dom_script(&f, XHR_RECORDER "var x=new XMLHttpRequest();rec(x);x.open('GET','/e');x.send();globalThis.xe=x;",
               "xhr: an empty answer");
    os64_dom_response_t empty = net_answer_of(204, "http://example.test/e", "", NULL);
    net_answer(&f, &net, net.count - 1, &empty);
    dom_script(&f, FETCH_ASSERT
        "assert(ev.join()===[1,'loadstart(0,0,false)',2,'progress(0,0,false)',4,'load(0,0,false)','loadend(0,0,false)'].join(),"
        "ev.join());assert(xe.status===204&&xe.response==='');",
        "xhr: an empty answer's events");

    /* A KNOWN zero is not computable (XHR § "fire a progress event": "if
     * length is not 0"): an empty upload's events and a Content-Length: 0
     * answer's say lengthComputable false, total 0, as browsers do. */
    dom_script(&f, XHR_RECORDER "var x=new XMLHttpRequest();rec(x);x.open('POST','/z');x.send('');globalThis.xz=x;",
               "xhr: an empty upload");
    os64_dom_response_t zero = net_answer_of(200, "http://example.test/z", "Content-Length: 0\r\n", "");
    net_answer(&f, &net, net.count - 1, &zero);
    dom_script(&f, FETCH_ASSERT
        "var want=[1,'loadstart(0,0,false)','upload.loadstart(0,0,false)','upload.progress(0,0,false)',"
        "'upload.load(0,0,false)','upload.loadend(0,0,false)',2,'progress(0,0,false)',4,'load(0,0,false)',"
        "'loadend(0,0,false)'].join();assert(ev.join()===want,ev.join());",
        "xhr: a known zero is not computable");

    /* Response types. */
    static const struct { const char *type, *holds; } kTypes[] = {
        {"arraybuffer", "xt.response instanceof ArrayBuffer&&xt.response.byteLength===9&&xt.response===xt.response"},
        {"blob", "xt.response instanceof Blob&&xt.response.size===9&&xt.response.type==='application/json;charset=utf-8'"},
        {"json", "xt.response.k[0]===1&&xt.response===xt.response"},
        {"text", "xt.response==='{\"k\":[1]}'&&xt.responseText===xt.response"},
        {"document", "xt.response===null&&xt.responseXML===null"},
    };
    for (size_t i = 0; i < sizeof(kTypes) / sizeof(kTypes[0]); i++) {
        char script[512];
        snprintf(script, sizeof(script),
                 "var x=new XMLHttpRequest();x.open('GET','/t');x.responseType='%s';x.send();globalThis.xt=x;"
                 "assert(x.response===(x.responseType==='text'?'':null));", kTypes[i].type);
        dom_script(&f, script, "xhr: a response type");
        os64_dom_response_t typed = net_answer_of(200, "http://example.test/t",
                                                  "Content-Type: application/json; charset=utf-8\r\n", "{\"k\":[1]}");
        net_answer(&f, &net, net.count - 1, &typed);
        snprintf(script, sizeof(script), FETCH_ASSERT "assert(%s,'%s');", kTypes[i].holds, kTypes[i].type);
        dom_script(&f, script, "xhr: a response type's response");
    }
    dom_script(&f, FETCH_ASSERT
        "assert(xt.responseXML===null);throws('InvalidStateError',()=>xt.responseText);"
        "var x=new XMLHttpRequest();x.open('GET','/j');x.responseType='json';x.send();globalThis.xj=x;",
        "xhr: document is null");
    os64_dom_response_t bad_json = net_answer_of(200, "http://example.test/j", "", "{");
    net_answer(&f, &net, net.count - 1, &bad_json);
    dom_script(&f, FETCH_ASSERT "assert(xj.response===null&&xj.response===null);", "xhr: JSON that does not parse is null");

    /* overrideMimeType decides the charset text is read in. */
    dom_script(&f, "var x=new XMLHttpRequest();x.open('GET','/l');x.overrideMimeType('text/plain;charset=windows-1252');"
               "x.send();globalThis.xl=x;", "xhr: an override");
    os64_dom_response_t latin = net_answer_of(200, "http://example.test/l", "Content-Type: text/plain;charset=utf-8\r\n",
                                              "caf\xe9");
    net_answer(&f, &net, net.count - 1, &latin);
    dom_script(&f, FETCH_ASSERT "assert(xl.responseText==='caf\\u00e9',escape(xl.responseText));", "xhr: the override's charset");

    /* Synchronous: run by the host, no events before, a NetworkError on
     * failure. */
    net.run_ok = true;
    net.run_answer = net_answer_of(200, "http://example.test/s", "Content-Length: 12\r\n", "Test Message");
    dom_script(&f, FETCH_ASSERT XHR_RECORDER
        "var x=new XMLHttpRequest();rec(x);x.open('POST','/s',false);"
        "throws('InvalidAccessError',()=>{x.timeout=5});throws('InvalidAccessError',()=>{x.responseType='text'});"
        "x.send('Test Message');assert(x.response==='Test Message');"
        "assert(ev.join()===[1,4,'load(12,12,true)','loadend(12,12,true)'].join(),ev.join());",
        "xhr: synchronous");
    check(net.runs == 1 && net.releases == 1, "xhr: a synchronous request is run and released");
    net.run_ok = false;
    dom_script(&f, FETCH_ASSERT
        "var x=new XMLHttpRequest();x.open('GET','/s',false);throws('NetworkError',()=>x.send());assert(x.readyState===4);",
        "xhr: a synchronous failure throws");

    /* A timeout, on the page's own timers. */
    dom_script(&f, XHR_RECORDER "var x=new XMLHttpRequest();rec(x);x.open('GET','/slow');x.timeout=50;x.send();",
               "xhr: a timeout");
    size_t slow = net.count - 1;
    storage_clock += 60;
    os64_js_outcome_t outcome;
    check(os64_dom_timer_fire(f.binding, storage_clock, &outcome), "xhr: the timeout's timer fires");
    check(net.seen[slow].cancelled, "xhr: a timeout cancels with the host");
    dom_script(&f, FETCH_ASSERT
        "assert(ev.join()===[1,'loadstart(0,0,false)',4,'timeout(0,0,false)','loadend(0,0,false)'].join(),ev.join());",
        "xhr: a timeout's events");
    /* open() again stops what was under way, with no events. */
    dom_script(&f, XHR_RECORDER "var x=new XMLHttpRequest();rec(x);x.open('GET','/one');x.send();x.open('GET','/two');"
               "assert(ev.join()===[1,'loadstart(0,0,false)'].join(),ev.join());", "xhr: open again");
    check(net.seen[net.count - 1].cancelled, "xhr: open again cancels the request out");
    /* A request out keeps its object alive with nothing else holding it. */
    dom_script(&f, "globalThis.done=0;(function(){var x=new XMLHttpRequest();x.onload=()=>done++;x.open('GET','/gc');x.send();})();",
               "xhr: unreferenced while out");
    size_t gc = net.count - 1;
    JSContext *ctx = os64_js_context(f.runtime, OS64_JS_ABI_ID, &outcome);
    JS_RunGC(JS_GetRuntime(ctx));
    os64_dom_response_t ok = net_answer_of(200, "http://example.test/gc", "", "x");
    net_answer(&f, &net, gc, &ok);
    dom_script(&f, FETCH_ASSERT "assert(done===1);", "xhr: an object with a request out outlives its references");
    fixture_free(&f);
}

/* A synchronous request whose server never finishes ends when the script
 * task that made it is out of time: the run asks the binding's stop, which
 * is the task's budget, and the task ends as any script out of time does. */
static void dom_xhr_sync_stall_case(void)
{
    FakeNet net = {0};
    DomFixture f = net_fixture("http://example.test/", &net);
    if (!fixture_ready(&f)) return;
    net.run_stalls = true;
    static const char source[] = "var x=new XMLHttpRequest();x.open('GET','/hang',false);x.send();";
    os64_js_outcome_t outcome;
    os64_js_status_t status = os64_js_run(f.runtime, source, sizeof(source) - 1, "stall.js", &outcome);
    check(net.run_stopped, "xhr: a stalled synchronous request is stopped by the task's budget");
    check(status == OS64_JS_LIMIT && outcome.limit == OS64_JS_LIMIT_EXECUTION,
          "xhr: and the task ends as a script out of time ends");
    fixture_free(&f);
}

/* ── What a page or a server can size, held to its bound by count ───── */

/* The steps a header list's work takes (fetch.c's d_request_steps): a
 * Headers walked whole, a server's header block read into a Response, and
 * an XMLHttpRequest's getAllResponseHeaders. Each doubles a size eight
 * times over and holds the work to n log n, so a quadratic step fails by
 * count, not by a clock. */
static uint64_t fetch_steps_for(DomFixture *f, FakeNet *net, int kind, size_t n)
{
    char script[512];
    if (kind == 0) {
        snprintf(script, sizeof(script),
                 "var a=[];for(let i=0;i<%zu;i++)a.push(['h'+((i*7919)%%%zu),'v']);globalThis.hh=new Headers(a);", n, n);
        dom_script(f, script, "bound: a Headers made");
        uint64_t before = d_request_steps;
        dom_script(f, "var k=0;for(const e of hh)k++;", "bound: a Headers walked");
        return d_request_steps - before;
    }
    /* A block of n lines, in reverse order. */
    size_t cap = n * 24 + 1;
    char *block = malloc(cap);
    if (block == NULL) abort();
    size_t at = 0;
    for (size_t i = n; i > 0; i--) at += (size_t)snprintf(block + at, cap - at, "H%zu: v\r\n", i);
    dom_script(f, kind == 1 ? "fetch('/b').then(r=>{globalThis.rb=r})"
                            : "var x=new XMLHttpRequest();x.open('GET','/b');x.send();globalThis.xb=x;",
               "bound: a request for a big block");
    os64_dom_response_t answer = net_answer_of(200, "http://example.test/b", block, "");
    uint64_t before = d_request_steps;
    net_answer(f, net, net->count - 1, &answer);
    if (kind == 2) dom_script(f, "xb.getAllResponseHeaders();", "bound: every header of a big block");
    uint64_t steps = d_request_steps - before;
    free(block);
    return steps;
}

static void dom_fetch_bound_cases(void)
{
    static const char *const kWhat[] = {"a Headers walked", "a server's block read", "getAllResponseHeaders"};
    for (int kind = 0; kind < 3; kind++) {
        FakeNet net = {0};
        DomFixture f = net_fixture("http://example.test/", &net);
        if (!fixture_ready(&f)) return;
        uint64_t small = fetch_steps_for(&f, &net, kind, 64);
        uint64_t large = fetch_steps_for(&f, &net, kind, 64 * 256);
        /* n log n from 64 to 16384: 256 times the entries, 256 * 14/6
         * the steps; a quadratic walk is 65536 times. */
        bool bounded = small > 0 && large <= small * 256 * 3;
        if (!bounded) fprintf(stderr, "bound %s: %llu steps at 64, %llu at 16384\n", kWhat[kind],
                              (unsigned long long)small, (unsigned long long)large);
        check(bounded, "bound: a header list's work is n log n in its length");
        fixture_free(&f);
    }
}

/* ── A body the runtime has no room for (requirement B) ──────────────── */

/* The answer's bytes are copied into the runtime's memory, so a body
 * larger than the runtime's limit is that limit, met as any allocation a
 * script makes meets it: libjs's sticky LIMIT, which ends the page's
 * runtime (its host retires it, as for a script that allocated too much).
 * Nothing the page sees half-made, and the teardown accounts for every
 * byte. The same body inside the limit resolves. */
static void dom_fetch_memory_cases(void)
{
    for (int fits = 0; fits < 2; fits++) {
        FakeNet net = {0};
        DomFixture f = net_fixture_engine("http://example.test/", &net, 4 * 1024 * 1024);
        if (!fixture_ready(&f)) return;
        size_t size = fits ? 512 * 1024 : 6 * 1024 * 1024;
        char *big = malloc(size + 1);
        if (big == NULL) abort();
        memset(big, 'x', size);
        big[size] = '\0';
        dom_script(&f, "globalThis.log=[];fetch('/big').then(r=>r.arrayBuffer()).then(b=>log.push(b.byteLength));",
                   "memory: a request for a big body");
        os64_dom_response_t answer = net_answer_of(200, "http://example.test/big", "", big);
        os64_js_status_t status = net_answer(&f, &net, 0, &answer);
        if (fits) {
            check(status == OS64_JS_OK, "memory: a body inside the limit is answered");
            dom_script(&f, FETCH_ASSERT "assert(log.join()==='524288',log.join());", "memory: and read");
        } else
            check(status == OS64_JS_LIMIT, "memory: a body past the limit is the runtime's limit");
        free(big);
        fixture_free(&f);
    }
}

/* ── Every allocation failed in turn ─────────────────────────────────── */

static void dom_fetch_sweep_cases(void)
{
    static const char *const kOperations[] = {
        "new Headers([['a','1'],['b','2']]).forEach(()=>{})",
        "new Request('/x',{method:'POST',body:'b',headers:{x:'1'}}).clone()",
        "new Response('a=1',{headers:{'content-type':'application/x-www-form-urlencoded'}}).formData()",
        "fetch('/x').catch(()=>{})",
        "var x=new XMLHttpRequest();x.open('GET','/x');x.setRequestHeader('a','b');x.send()",
        "AbortSignal.any([new AbortController().signal]);new AbortController().abort()",
        "var f=new FormData();f.append('a',new Blob(['x']),'n');new Response(f).text()",
    };
    size_t n = sizeof(kOperations) / sizeof(kOperations[0]);
    os64_dom_options_t options = os64_dom_default_options();
    options.url = "http://example.test/";
    binding_sweep("fetch", kOperations, n, &options);
    engine_sweep("fetch", kOperations, n, &options);
}

/* ── With the switch off, none of it ─────────────────────────────────── */

static void dom_fetch_modern_off_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, FETCH_ASSERT
        "for(const n of ['fetch','Headers','Request','Response','AbortController','AbortSignal','FormData','Blob',"
        "'File','XMLHttpRequest','XMLHttpRequestEventTarget','XMLHttpRequestUpload','ProgressEvent','DOMException',"
        "'ReadableStream'])assert(typeof window[n]==='undefined'&&!(n in window),n);",
        "modern off: no request surface");
    fixture_free(&f);
}

static void dom_fetch_cases(void)
{
    dom_fetch_origin_cases();
    dom_fetch_headers_cases();
    dom_fetch_object_cases();
    dom_fetch_network_cases();
    dom_fetch_abort_cases();
    dom_weak_set_cases();
    dom_signal_bound_cases();
    dom_xhr_cases();
    dom_xhr_sync_stall_case();
    dom_fetch_bound_cases();
    dom_fetch_memory_cases();
    dom_fetch_modern_off_cases();
    dom_fetch_sweep_cases();
}
