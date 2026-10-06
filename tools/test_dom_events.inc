/* D7a: events, handler attributes, timers, the asks a script makes, and the
 * script-element flags (DOM_D7.md § The cut). Each rule has its own case. */
#define DOM_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;assert(e.name===n,'wanted '+n+' got '+e.name)}assert(caught,'no '+n);}"
static uint64_t event_clock;
static uint64_t event_now(void *opaque) { (void)opaque; return event_clock; }
static const os64_html_node_t *connected_scripts[16];
static size_t connected_count;
static const os64_html_node_t *activated_node;
static os64_dom_activation_t activated_what;
static size_t activations;
static void event_connected(void *opaque, const os64_html_node_t *script)
{
    check(opaque == &event_clock, "script notice receives the host's context");
    if (connected_count < 16) connected_scripts[connected_count] = script;
    connected_count++;
}
static void event_activate(void *opaque, const os64_html_node_t *node, os64_dom_activation_t what)
{
    check(opaque == &event_clock, "activation receives the host's context");
    activated_node = node;
    activated_what = what;
    activations++;
}
static const char event_html[] =
    "<!doctype html><html><head><title>events</title></head>"
    "<body onload=\"log.push('body onload '+(event.target===document)+' '+(this===window))\">"
    "<div id=outer><p id=inner><a id=link href=\"next.html\">link</a></p></div>"
    "<div id=scoped onclick=\"log.push(id+' '+(typeof getElementById)+' '+(this===event.currentTarget))\"></div>"
    "<form id=form action=/send><input id=field onclick=\"log.push(typeof requestSubmit)\">"
    "<input id=tick type=checkbox><button id=go>go</button><button id=undo type=reset>undo</button></form>"
    "<div id=broken onclick=\"log.push('broken'\"></div>"
    "<div id=escape onclick=\"}),(function(){log.push('escaped')\"></div>"
    "<div id=falsy onclick=\"return false\"></div>"
    "<div id=first onclick=\"log.push('attribute first')\"></div>"
    "<span id=wrapless onmousedown=\"log.push('ancestor attribute')\"><b id=deep>deep</b></span>"
    "</body></html>";
static DomFixture event_fixture(const char *html, const char *url)
{
    DomFixture fixture = {0};
    os64_html_options_t html_options = os64_html_options_default();
    html_options.charset = "utf-8";
    os64_html_parser_t *parser = os64_html_parser_new(&html_options);
    if (!parser) return fixture;
    os64_html_parser_feed(parser, html, strlen(html));
    fixture.document = os64_html_parser_finish(parser);
    fixture.state = os64_page_state_create(fixture.document, 0);
    os64_js_config_t config = fixture_config();
    config.limits.jobs_per_turn = 100000;
    fixture.runtime = create_fixture(&config);
    os64_dom_options_t options = os64_dom_default_options();
    options.url = url;
    options.now_ms = event_now;
    options.script_connected = event_connected;
    options.activate = event_activate;
    options.host_opaque = &event_clock;
    os64_js_outcome_t outcome;
    if (fixture.runtime && fixture.state)
        fixture.binding = os64_dom_create(fixture.runtime, fixture.document, fixture.state, &options, &outcome);
    connected_count = activations = 0;
    activated_node = NULL;
    event_clock = 1000;
    return fixture;
}
static os64_dom_event_t plain_event(const char *type, bool bubbles, bool cancelable)
{
    os64_dom_event_t event = {0};
    event.type = type;
    event.bubbles = bubbles;
    event.cancelable = cancelable;
    return event;
}
static os64_js_status_t host_dispatch(DomFixture *f, const char *id, const char *type, bool *prevented)
{
    os64_dom_event_t event = plain_event(type, true, true);
    os64_js_outcome_t outcome;
    os64_html_node_t *node = id ? native_id(f->document->document, id) : NULL;
    return os64_dom_dispatch(f->binding, node, &event, prevented, &outcome);
}
#define LOG_IS(expected) "if(log.join('|')!==" expected ")throw Error('log: '+log.join('|'));log.length=0;"

static void dom_event_dispatch_cases(void)
{
    DomFixture f = event_fixture(event_html, "http://example.test/dir/page.html?q=1#top");
    if (!fixture_ready(&f)) return;
    dom_script(&f, DOM_ASSERT "globalThis.log=[];"
        "const outer=document.getElementById('outer'),inner=document.getElementById('inner'),link=document.getElementById('link');"
        "function mark(n){return e=>log.push(n+':'+e.eventPhase+':'+(e.currentTarget===window?'window':e.currentTarget.id||e.currentTarget.nodeName))}"
        "window.addEventListener('click',mark('wc'),true);document.addEventListener('click',mark('dc'),{capture:true});"
        "outer.addEventListener('click',mark('oc'),true);link.addEventListener('click',mark('tb'));"
        "link.addEventListener('click',mark('tc'),true);outer.addEventListener('click',mark('ob'));"
        "document.addEventListener('click',mark('db'));window.addEventListener('click',mark('wb'));"
        "const dup=mark('dup');link.addEventListener('click',dup);link.addEventListener('click',dup);"
        "link.addEventListener('click',{handleEvent(e){log.push('object '+(this!==e.currentTarget))}});",
        "event-order-setup.js");
    bool prevented = true;
    os64_js_outcome_t outcome;
    check(host_dispatch(&f, "link", "click", &prevented) == OS64_JS_OK && !prevented,
          "a host click runs as one task");
    dom_script(&f, LOG_IS("'wc:1:window|dc:1:#document|oc:1:outer|tc:2:link|tb:2:link|dup:2:link|object true|"
        "ob:3:outer|db:3:#document|wb:3:window'"), "event-order.js");
    dom_script(&f, DOM_ASSERT
        "const p=document.getElementById('inner');let n=0;"
        "const once=()=>{n++};p.addEventListener('ping',once,{once:true});"
        "p.dispatchEvent(new Event('ping'));p.dispatchEvent(new Event('ping'));assert(n===1,'once ran '+n);"
        "const late=()=>log.push('late');const gone=()=>log.push('gone');"
        "p.addEventListener('pong',()=>{log.push('first');p.addEventListener('pong',late);p.removeEventListener('pong',gone)});"
        "p.addEventListener('pong',gone);p.dispatchEvent(new Event('pong'));"
        LOG_IS("'first'")
        "p.dispatchEvent(new Event('pong'));" LOG_IS("'first|late'")
        "p.addEventListener('stop',e=>{log.push('a');e.stopImmediatePropagation()});p.addEventListener('stop',()=>log.push('b'));"
        "document.addEventListener('stop',()=>log.push('doc'));p.dispatchEvent(new Event('stop',{bubbles:true}));"
        LOG_IS("'a'")
        "document.addEventListener('still',()=>log.push('doc capture'),true);document.addEventListener('still',()=>log.push('doc bubble'));"
        "p.addEventListener('still',()=>log.push('target'));p.dispatchEvent(new Event('still'));"
        LOG_IS("'doc capture|target'")
        "p.addEventListener('half',e=>{e.cancelBubble=true;log.push('p')});document.body.addEventListener('half',()=>log.push('body'));"
        "p.dispatchEvent(new Event('half',{bubbles:true}));" LOG_IS("'p'")
        "const plain=new Event('quiet',{cancelable:false});p.addEventListener('quiet',e=>{e.preventDefault();e.returnValue=false});"
        "assert(p.dispatchEvent(plain)===true&&!plain.defaultPrevented,'uncancelable ignores preventDefault');"
        "const loud=new Event('loud',{cancelable:true});p.addEventListener('loud',e=>{e.returnValue=false});"
        "assert(p.dispatchEvent(loud)===false&&loud.defaultPrevented&&loud.returnValue===false,'returnValue=false cancels');"
        "assert(loud.target===p&&loud.srcElement===p&&loud.currentTarget===null&&loud.eventPhase===0,'event after dispatch');"
        "assert(Event.AT_TARGET===2&&loud.BUBBLING_PHASE===3&&loud instanceof Event,'event constants');"
        "const old=document.createEvent('HTMLEvents');throws('InvalidStateError',()=>p.dispatchEvent(old));"
        "old.initEvent('legacy',true,true);p.addEventListener('legacy',e=>log.push('legacy '+e.bubbles+' '+e.cancelable));"
        "p.dispatchEvent(old);" LOG_IS("'legacy true true'")
        "p.addEventListener('nest',e=>{log.push('outer');Promise.resolve().then(()=>log.push('outer job'));"
        "p.dispatchEvent(new Event('inner'));log.push('outer done')});"
        "p.addEventListener('inner',()=>{log.push('inner');Promise.resolve().then(()=>log.push('inner job'))});",
        "event-rules.js");
    check(host_dispatch(&f, "inner", "nest", &prevented) == OS64_JS_OK, "a nested dispatchEvent is part of one task");
    dom_script(&f, LOG_IS("'outer|inner|outer done|outer job|inner job'"), "event-nested.js");
    dom_script(&f, "const t=document.getElementById('inner');"
        "t.addEventListener('two',()=>{log.push('l1');Promise.resolve().then(()=>log.push('l1 job'))});"
        "t.addEventListener('two',()=>log.push('l2'));"
        "t.addEventListener('throws',()=>{throw Error('listener threw')});t.addEventListener('throws',()=>log.push('next ran'));",
        "event-checkpoint-setup.js");
    check(host_dispatch(&f, "inner", "two", &prevented) == OS64_JS_OK, "two host listeners");
    dom_script(&f, LOG_IS("'l1|l1 job|l2'"), "event-checkpoint.js");
    os64_dom_event_t throwing = plain_event("throws", true, true);
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "inner"), &throwing, &prevented, &outcome) ==
          OS64_JS_EXCEPTION && strstr(outcome.message, "listener threw"),
          "a throwing listener is the dispatch's reported exception");
    dom_script(&f, LOG_IS("'next ran'"), "event-throw-next.js");
    dom_script(&f, "document.getElementById('outer').addEventListener('deep',()=>{throw Error('deep listener')});"
        "document.getElementById('inner').addEventListener('nested-throw',()=>{"
        "document.getElementById('outer').dispatchEvent(new Event('deep'));log.push('still running')})", "event-report-setup.js");
    os64_dom_event_t nested = plain_event("nested-throw", true, true);
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "inner"), &nested, &prevented, &outcome) ==
          OS64_JS_EXCEPTION && strstr(outcome.message, "deep listener"),
          "a nested listener's error is folded into the host dispatch");
    dom_script(&f, LOG_IS("'still running'"), "event-report.js");
    dom_script(&f, "document.getElementById('outer').dispatchEvent(new Event('deep'))", "event-report-script.js");
    check(os64_dom_take_report(f.binding, &outcome) && strstr(outcome.message, "deep listener") &&
          !os64_dom_take_report(f.binding, &outcome), "a script task's nested error is taken once");
    fixture_free(&f);
}

static void dom_event_handler_cases(void)
{
    DomFixture f = event_fixture(event_html, "http://example.test/dir/page.html");
    if (!fixture_ready(&f)) return;
    bool prevented;
    os64_js_outcome_t outcome;
    dom_script(&f, "globalThis.log=[];", "handler-setup.js");
    check(host_dispatch(&f, "deep", "mousedown", &prevented) == OS64_JS_OK,
          "a handler attribute on an unwrapped ancestor");
    dom_script(&f, LOG_IS("'ancestor attribute'")
        "document.getElementById('first').addEventListener('click',()=>log.push('listener'))", "handler-ancestor.js");
    check(host_dispatch(&f, "first", "click", &prevented) == OS64_JS_OK, "parser attribute and a later listener");
    dom_script(&f, LOG_IS("'attribute first|listener'"), "handler-parser-first.js");
    check(host_dispatch(&f, "scoped", "click", &prevented) == OS64_JS_OK, "attribute handler on an unwrapped node");
    dom_script(&f, LOG_IS("'scoped function true'"), "handler-scope.js");
    check(host_dispatch(&f, "field", "click", &prevented) == OS64_JS_OK, "form-owned control's handler");
    dom_script(&f, LOG_IS("'function'"), "handler-form-scope.js");
    check(host_dispatch(&f, "broken", "click", &prevented) == OS64_JS_EXCEPTION, "a handler that does not compile is reported");
    check(host_dispatch(&f, "broken", "click", &prevented) == OS64_JS_OK, "a failed handler stays absent until it changes");
    check(host_dispatch(&f, "escape", "click", &prevented) == OS64_JS_EXCEPTION, "a body that closes the wrapper is refused");
    check(host_dispatch(&f, "falsy", "click", &prevented) == OS64_JS_OK && prevented, "return false cancels");
    dom_script(&f, DOM_ASSERT LOG_IS("''")
        "{const b=document.getElementById('broken');assert(b.onclick===null,'failed handler reads null');"
        "b.setAttribute('onclick',\"log.push('fixed')\");}",
        "handler-fix.js");
    check(host_dispatch(&f, "broken", "click", &prevented) == OS64_JS_OK, "a changed attribute compiles again");
    dom_script(&f, DOM_ASSERT LOG_IS("'fixed'")
        "{const b=document.getElementById('broken');b.removeAttribute('onclick');}",
        "handler-removed-setup.js");
    check(host_dispatch(&f, "broken", "click", &prevented) == OS64_JS_OK, "removed attribute dispatch");
    dom_script(&f, DOM_ASSERT LOG_IS("''")
        "{const s=document.getElementById('scoped');assert(typeof s.onclick==='function','compiled handler readable');"
        "const order=document.getElementById('outer');order.addEventListener('click',()=>log.push('before'));"
        "order.setAttribute('onclick',\"log.push('attribute')\");order.addEventListener('click',()=>log.push('after'));"
        "order.onclick=function(){log.push('property '+(this===order));return false};}",
        "handler-order-setup.js");
    check(host_dispatch(&f, "outer", "click", &prevented) == OS64_JS_OK && prevented,
          "a property handler's false cancels");
    dom_script(&f, DOM_ASSERT LOG_IS("'before|property true|after'")
        "{const order=document.getElementById('outer');order.addEventListener('click',()=>false);order.onclick=null;"
        "assert(order.onclick===null,'null removes the handler');order.setAttribute('onclick','log.push(1)');"
        "assert(typeof order.onclick==='function','a later attribute replaces a property');}",
        "handler-order.js");
    check(host_dispatch(&f, "outer", "click", &prevented) == OS64_JS_OK && !prevented,
          "a listener's false cancels nothing");
    dom_script(&f, DOM_ASSERT LOG_IS("'before|1|after'")
        "assert(typeof window.onload==='function'&&document.body.onload===window.onload,'body onload is window\\'s');",
        "handler-body.js");
    check(os64_dom_listens(f.binding, "load"), "body onload attribute counts as a load listener");
    os64_dom_event_t load = plain_event("load", false, false);
    check(os64_dom_dispatch(f.binding, NULL, &load, &prevented, &outcome) == OS64_JS_OK, "load at window");
    dom_script(&f, DOM_ASSERT LOG_IS("'body onload true true'")
        "{let fired=0;window.addEventListener('load',()=>fired++);"
        "document.body.onload=()=>log.push('replaced');assert(window.onload===document.body.onload);"
        "window.dispatchEvent(new Event('load'));assert(fired===1,'load listener on window');}",
        "handler-body-replace.js");
    dom_script(&f, LOG_IS("'replaced'"), "handler-body-replaced.js");
    fixture_free(&f);
}

static void dom_event_listens_cases(void)
{
    DomFixture f = event_fixture("<!doctype html><body><p id=p>quiet</p></body>", "http://example.test/");
    if (!fixture_ready(&f)) return;
    bool prevented = true;
    os64_js_outcome_t outcome;
    check(!os64_dom_listens(f.binding, "mousemove") && !os64_dom_listens(f.binding, "click"),
          "a page with no handlers listens to nothing");
    os64_dom_event_t move = plain_event("mousemove", true, false);
    size_t engine = attempts, binding = dom_attempts;
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "p"), &move, &prevented, &outcome) == OS64_JS_OK &&
          !prevented && attempts == engine && dom_attempts == binding,
          "an event nobody hears allocates nothing and enters no engine");
    dom_script(&f, "globalThis.moves=0;const h=()=>moves++;document.addEventListener('mousemove',h);globalThis.h=h;",
               "listens-add.js");
    check(os64_dom_listens(f.binding, "mousemove"), "a listener is heard");
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "p"), &move, &prevented, &outcome) == OS64_JS_OK,
          "a heard mousemove dispatches");
    dom_script(&f, DOM_ASSERT "assert(moves===1);document.removeEventListener('mousemove',h);", "listens-remove.js");
    check(!os64_dom_listens(f.binding, "mousemove"), "a removed listener is no longer heard");
    dom_script(&f, "document.getElementById('p').setAttribute('onmouseover','1')", "listens-attribute.js");
    check(os64_dom_listens(f.binding, "mouseover"), "a handler attribute set by a verb is heard");
    dom_script(&f, "document.getElementById('p').removeAttribute('onmouseover');document.onkeydown=()=>{}",
               "listens-attribute-removed.js");
    check(!os64_dom_listens(f.binding, "mouseover") && os64_dom_listens(f.binding, "keydown"),
          "a removed attribute is not heard; a property is");
    os64_dom_event_t key = plain_event("keydown", true, true);
    key.kind = OS64_DOM_EVENT_KEY;
    key.key = "a";
    key.key_code = 65;
    key.shift = true;
    dom_script(&f, "globalThis.keys=[];document.onkeydown=e=>{keys.push(e.key,e.keyCode,e.which,e.shiftKey,e.isTrusted)}",
               "listens-key-setup.js");
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "p"), &key, &prevented, &outcome) == OS64_JS_OK,
          "a key event dispatches");
    dom_script(&f, DOM_ASSERT "assert(keys.join()==='a,65,65,true,true',keys.join())", "listens-key.js");
    os64_dom_event_t over = plain_event("mouseover", true, true);
    over.kind = OS64_DOM_EVENT_MOUSE;
    over.client_x = 7;
    over.client_y = 9;
    over.related = f.document->body;
    dom_script(&f, "globalThis.mouse=[];document.onmouseover=e=>{mouse.push(e.clientX,e.clientY,e.button,e.which,e.relatedTarget===document.body)}",
               "listens-mouse-setup.js");
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "p"), &over, &prevented, &outcome) == OS64_JS_OK,
          "a mouse event dispatches");
    dom_script(&f, DOM_ASSERT "assert(mouse.join()==='7,9,0,1,true',mouse.join())", "listens-mouse.js");
    fixture_free(&f);
}

static void dom_timer_cases(void)
{
    DomFixture f = event_fixture("<!doctype html><body><p id=p>timers</p></body>", "http://example.test/t.html");
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    check(os64_dom_timer_next(f.binding) == UINT64_MAX && !os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &outcome),
          "no timers, nothing due");
    dom_script(&f, DOM_ASSERT "globalThis.log=[];"
        "const a=setTimeout(()=>log.push('a'),20),b=setTimeout((x,y)=>log.push('b'+x+y+(this===window)),10,'x','y');"
        "const c=setTimeout(\"log.push('c')\",10);const d=setTimeout(()=>log.push('d'),5);clearTimeout(d);"
        "assert(a===1&&b===2&&c===3&&d===4,'ids from 1');globalThis.n=0;"
        "globalThis.i=setInterval(()=>{log.push('i'+(++n));if(n===3)clearInterval(i)},7);"
        "setTimeout(()=>{throw Error('timer threw')},40);setTimeout(\"log.push('z')\",-5);",
        "timer-setup.js");
    check(os64_dom_timer_next(f.binding) == 1000, "a negative delay is zero");
    check(os64_dom_timer_fire(f.binding, 1000, &outcome) && outcome.status == OS64_JS_OK,
          "a string timer runs as a script");
    check(os64_dom_timer_next(f.binding) == 1007 && !os64_dom_timer_fire(f.binding, 1006, &outcome),
          "the earliest timer is next and nothing fires before it");
    uint64_t now = 1007;
    size_t fired = 0;
    while (os64_dom_timer_next(f.binding) <= 1060 && fired < 20) {
        now = os64_dom_timer_next(f.binding);
        event_clock = now;
        check(os64_dom_timer_fire(f.binding, now, &outcome), "a due timer fires");
        if (now == 1040) check(outcome.status == OS64_JS_EXCEPTION && strstr(outcome.message, "timer threw"),
                               "a timer that throws reports it");
        else check(outcome.status == OS64_JS_OK, "a timer runs as its own task");
        fired++;
    }
    check(os64_dom_timer_next(f.binding) == UINT64_MAX, "every timer has fired or been cleared");
    dom_script(&f, LOG_IS("'z|i1|bxytrue|c|i2|a|i3'"), "timer-order.js");
    dom_script(&f, DOM_ASSERT "let made=0;try{for(;;){setTimeout(()=>{},1000);made++}}catch(e){assert(e.name==='QuotaExceededError',e.name)}"
        "assert(made===4096,'cap '+made);", "timer-cap.js");
    check(os64_dom_registry_count(f.binding) >= 4096, "pending timers are registry values");
    fixture_free(&f);
}

static void dom_navigation_cases(void)
{
    DomFixture f = event_fixture(event_html, "http://example.test:8080/dir/page.html?q=1#top");
    if (!fixture_ready(&f)) return;
    os64_dom_navigation_t ask;
    check(!os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_NONE, "no ask yet");
    dom_script(&f, DOM_ASSERT
        "assert(location.href==='http://example.test:8080/dir/page.html?q=1#top'&&String(location)===location.href);"
        "assert(location.protocol==='http:'&&location.host==='example.test:8080'&&location.hostname==='example.test');"
        "assert(location.port==='8080'&&location.pathname==='/dir/page.html'&&location.search==='?q=1');"
        "assert(location.hash==='#top'&&location.origin==='http://example.test:8080'&&document.location===location);"
        "window.status='ignored';assert(window.status==='');"
        "throws('InvalidStateError',()=>document.write('x'));"
        "try{document.writeln('x')}catch(e){assert(e.message==='document.write is not supported yet',e.message)}"
        "location.href='first.html';location.assign('../second.html');",
        "navigation-location.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_URL &&
          !strcmp(ask.url, "http://example.test:8080/second.html") && !os64_dom_take_navigation(f.binding, &ask),
          "the last ask wins, resolved against the page, and is taken once");
    dom_script(&f, "location.replace('http://other.test/x')", "navigation-replace.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_REPLACE &&
          !strcmp(ask.url, "http://other.test/x"), "replace keeps an absolute address");
    dom_script(&f, "location.hash='next'", "navigation-hash.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_URL &&
          !strcmp(ask.url, "http://example.test:8080/dir/page.html?q=1#next"), "hash names a place in this page");
    dom_script(&f, "window.location='/root'", "navigation-assign-location.js");
    check(os64_dom_take_navigation(f.binding, &ask) && !strcmp(ask.url, "http://example.test:8080/root"),
          "assigning location navigates");
    dom_script(&f, "history.back()", "navigation-back.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_HISTORY && ask.delta == -1,
          "history.back is a delta");
    dom_script(&f, "history.go(0)", "navigation-go.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_RELOAD, "go(0) reloads");
    dom_script(&f, "globalThis.log=[];document.getElementById('link').addEventListener('click',e=>log.push('link'));"
        "document.getElementById('link').click()", "navigation-link.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_FOLLOW &&
          ask.node == native_id(f.document->document, "link"), "a link's click asks to follow it");
    if (ask.node) os64_html_release(f.document, ask.node);
    dom_script(&f, "document.getElementById('link').onclick=()=>false;document.getElementById('link').click()",
               "navigation-link-cancelled.js");
    check(!os64_dom_take_navigation(f.binding, &ask), "a cancelled click follows nothing");
    dom_script(&f, "document.getElementById('form').onsubmit=e=>{log.push('submit '+e.cancelable)};"
        "document.getElementById('go').click()", "navigation-submit-button.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_SUBMIT &&
          ask.node == native_id(f.document->document, "form") && ask.submitter == native_id(f.document->document, "go"),
          "a submit button's click fires submit and asks to send the form");
    if (ask.node) os64_html_release(f.document, ask.node);
    if (ask.submitter) os64_html_release(f.document, ask.submitter);
    dom_script(&f, "document.getElementById('form').onsubmit=()=>false;document.getElementById('go').click()",
               "navigation-submit-cancelled.js");
    check(!os64_dom_take_navigation(f.binding, &ask), "a cancelled submit sends nothing");
    dom_script(&f, "document.getElementById('form').submit()", "navigation-form-submit.js");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_SUBMIT && !ask.submitter,
          "form.submit() asks without a submit event");
    if (ask.node) os64_html_release(f.document, ask.node);
    dom_script(&f, DOM_ASSERT LOG_IS("'link|link|submit true'")
        "document.getElementById('form').onreset=()=>log.push('reset');document.getElementById('undo').click();"
        "document.getElementById('field').focus();",
        "navigation-reset.js");
    check(activations == 2 && activated_what == OS64_DOM_ACTIVATE_FOCUS &&
          activated_node == native_id(f.document->document, "field"), "reset and focus reach the host");
    dom_script(&f, DOM_ASSERT LOG_IS("'reset'")
        "{const t=document.getElementById('tick');t.addEventListener('change',()=>log.push('change '+t.checked));"
        "t.click();assert(t.checked,'click checks');t.onclick=()=>false;t.click();assert(t.checked,'a cancelled click puts it back');}"
        LOG_IS("'change true'"), "navigation-checkbox.js");
    dom_script(&f, "location.href='a.html'", "navigation-unclaimed.js");
    fixture_free(&f);
}

static void dom_script_flag_cases(void)
{
    DomFixture f = event_fixture("<!doctype html><body><div id=host></div>"
        "<script id=inline>1</script><script id=module type=module></script>"
        "<script id=vb type=text/vbscript></script><script id=lang language=JavaScript1.2></script>"
        "<script id=padded type=' text/javascript '></script></body>", "http://example.test/");
    if (!fixture_ready(&f)) return;
    const os64_html_node_t *root = f.document->document;
    check(os64_dom_script_kind(native_id((os64_html_node_t *)root, "inline")) == OS64_DOM_SCRIPT_CLASSIC &&
          os64_dom_script_kind(native_id((os64_html_node_t *)root, "module")) == OS64_DOM_SCRIPT_MODULE &&
          os64_dom_script_kind(native_id((os64_html_node_t *)root, "vb")) == OS64_DOM_SCRIPT_NONE &&
          os64_dom_script_kind(native_id((os64_html_node_t *)root, "lang")) == OS64_DOM_SCRIPT_CLASSIC &&
          os64_dom_script_kind(native_id((os64_html_node_t *)root, "padded")) == OS64_DOM_SCRIPT_CLASSIC &&
          os64_dom_script_kind(native_id((os64_html_node_t *)root, "host")) == OS64_DOM_SCRIPT_NONE,
          "script kinds follow HTML's type rules");
    const os64_html_node_t *inline_script = native_id((os64_html_node_t *)root, "inline");
    check(os64_dom_script_start(f.binding, inline_script) == 1 && os64_dom_script_start(f.binding, inline_script) == 0,
          "a script starts once");
    dom_script(&f, DOM_ASSERT
        "const host=document.getElementById('host');host.innerHTML='<script id=parsed>1<\\/script>';"
        "host.appendChild(document.getElementById('parsed'));"
        "const made=document.createElement('script');made.id='made';const other=document.createElement('script');"
        "other.setAttribute('type','module');const frag=document.createDocumentFragment();frag.appendChild(other);"
        "const detached=document.createElement('div');detached.appendChild(made);"
        "frag.appendChild(document.createElement('script'));host.appendChild(frag);host.appendChild(detached);"
        "host.appendChild(made);document.body.appendChild(document.getElementById('inline'));"
        "globalThis.copy=made.cloneNode(true);host.appendChild(copy);",
        "script-flags.js");
    check(connected_count == 2 && os64_html_attr(connected_scripts[0], "id") == NULL &&
          connected_scripts[1] == native_id((os64_html_node_t *)root, "made"),
          "connected scripts are reported once each: innerHTML, module and started scripts are not");
    check(os64_dom_script_start(f.binding, connected_scripts[1]) == 0, "a reported script was marked started");
    fixture_free(&f);
}

static void dom_event_failure_cases(void)
{
    DomFixture f = event_fixture(event_html, "http://example.test/");
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.kept=null;globalThis.log=[];"
        "document.getElementById('inner').addEventListener('click',e=>{kept=e;Promise.resolve().then(()=>{for(;;){}})});"
        "setTimeout(()=>log.push('never'),10);document.addEventListener('click',()=>log.push('never'));",
        "failure-setup.js");
    bool prevented;
    os64_js_outcome_t outcome;
    os64_dom_event_t click_event = plain_event("click", true, true);
    dom_clock_step = 1000;
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "inner"), &click_event, &prevented, &outcome) ==
          OS64_JS_LIMIT && outcome.limit == OS64_JS_LIMIT_EXECUTION, "a listener's checkpoint that overruns ends the task");
    dom_clock_step = 0;
    check(os64_dom_dispatch(f.binding, native_id(f.document->document, "inner"), &click_event, &prevented, &outcome) ==
          OS64_JS_FAILED_RUNTIME, "nothing dispatches after the runtime failed");
    event_clock = 2000;
    check(os64_dom_timer_fire(f.binding, 2000, &outcome) && outcome.status == OS64_JS_FAILED_RUNTIME,
          "a timer of a failed runtime does not run");
    check(os64_dom_registry_count(f.binding) > 0, "a listener, a timer and a kept event are registry values");
    fixture_free(&f);
}

/* Re-entry and retained state: a redispatch keeps its veto, conversions that
 * run script cannot slip a timer past the quota, and a click on a clicking
 * element returns while a click on another still runs. */
static void dom_event_reentry_cases(void)
{
    DomFixture f=event_fixture(event_html,"http://example.test/review");
    if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "var t=document.getElementById('inner'),log=[];"
        "t.addEventListener('click',e=>{log.push('capture');e.stopPropagation()},true);"
        "t.addEventListener('click',()=>log.push('bubble'));"
        "t.dispatchEvent(new Event('click',{bubbles:true}));"
        "assert(log.join('|')==='capture','review target propagation: '+log);",
        "review-target-propagation.js");
    fixture_free(&f);
    f=event_fixture(event_html,"http://example.test/review");
    if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "var e=new Event('review',{cancelable:true});e.preventDefault();"
        "assert(!document.body.dispatchEvent(e)&&e.defaultPrevented,'review cancellation lost');",
        "review-retained-cancellation.js");
    fixture_free(&f);
    f=event_fixture(event_html,"http://example.test/review");
    if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "for(var i=0;i<4095;i++)setTimeout(()=>{},1000);"
        "throws('QuotaExceededError',()=>setTimeout(()=>{},{valueOf(){setTimeout(()=>{},1000);return 1000}}));",
        "review-timer-reentrant-quota.js");
    fixture_free(&f);
    f=event_fixture(event_html,"http://example.test/review");
    if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "for(var i=0;i<4095;i++)setTimeout(()=>{},1000);"
        "throws('QuotaExceededError',()=>setTimeout({toString(){setTimeout(()=>{},1000);return 'void 0'}},1000));",
        "review-timer-reentrant-source-quota.js");
    fixture_free(&f);
    f=event_fixture(event_html,"http://example.test/review");
    if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "var t=document.getElementById('inner'),n=0;t.onclick=()=>{if(++n===1)t.click()};t.click();"
        "assert(n===1,'review recursive click: '+n);",
        "review-click-in-progress.js");
    fixture_free(&f);
    f=event_fixture(event_html,"http://example.test/review");
    if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "var t=document.getElementById('inner'),b=document.body,log=[];"
        "b.onclick=e=>{if(e.target===b)log.push('body')};t.onclick=()=>{log.push('inner');b.click()};t.click();"
        "assert(log.join('|')==='inner|body','review nested click elsewhere: '+log);",
        "review-click-other-element.js");
    fixture_free(&f);
}
static void dom_event_cases(void)
{
    dom_event_dispatch_cases();
    dom_event_handler_cases();
    dom_event_listens_cases();
    dom_event_reentry_cases();
    dom_timer_cases();
    dom_navigation_cases();
    dom_script_flag_cases();
    dom_event_failure_cases();
}
#undef LOG_IS
#undef DOM_ASSERT
