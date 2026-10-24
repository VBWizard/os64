/* P1-Da, THE DOCUMENT'S STATE (libdom/document.c, CustomEvent and
 * createEvent in libdom/event.c; WINDOW_DOCUMENT.md § P1-Da): a case per
 * sentence of document.c's contract, for the window's document and for a
 * document of its own; document.body's setter, switch on and off;
 * CustomEvent and createEvent's table; and every allocation the new
 * members make failed in turn. */

#define DOCUMENT_URL "https://www.example.test/dir/page.html"

/* The host's focus, faked: what a case says is focused, and whether the
 * document has the window's focus. */
static const os64_html_node_t *document_focused;
static bool document_has_focus;
static unsigned document_focus_asks;
static bool document_focus(void *opaque, const os64_html_node_t **focused)
{
    check(opaque == NULL, "focus: the binding's host context is the options'");
    document_focus_asks++;
    *focused = document_focused;
    return document_has_focus;
}

static os64_dom_options_t document_options(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.url = DOCUMENT_URL;
    options.focus = document_focus;
    return options;
}

/* A fixture over `html`, parsed with `charset`, as fixture_new makes one
 * over its own page. */
static DomFixture document_fixture(const char *html, const char *charset, const os64_dom_options_t *options)
{
    DomFixture fixture = {0};
    os64_html_options_t html_options = os64_html_options_default();
    html_options.charset = charset;
    os64_html_parser_t *parser = os64_html_parser_new(&html_options);
    if (!parser) return fixture;
    os64_html_parser_feed(parser, html, strlen(html));
    fixture.document = os64_html_parser_finish(parser);
    fixture.state = os64_page_state_create(fixture.document, 0);
    os64_js_config_t config = fixture_config();
    fixture.runtime = create_fixture(&config);
    if (fixture.runtime && fixture.state) {
        os64_js_outcome_t outcome;
        fixture.binding = os64_dom_create(fixture.runtime, fixture.document, fixture.state, options, &outcome);
    }
    return fixture;
}

static void dom_document_readiness_cases(void)
{
    /* The rule, pure. */
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    const os64_html_node_t *root = f.document->document, *body = native_id(f.document->document, "box");
    check(os64_dom_readiness_after(OS64_DOM_LOADING, f.document, root, "DOMContentLoaded") == OS64_DOM_INTERACTIVE,
          "readiness: DOMContentLoaded at the document makes it interactive");
    check(os64_dom_readiness_after(OS64_DOM_LOADING, f.document, NULL, "load") == OS64_DOM_COMPLETE &&
              os64_dom_readiness_after(OS64_DOM_INTERACTIVE, f.document, NULL, "load") == OS64_DOM_COMPLETE,
          "readiness: load at window makes it complete, from either");
    check(os64_dom_readiness_after(OS64_DOM_LOADING, f.document, body, "DOMContentLoaded") == OS64_DOM_LOADING &&
              os64_dom_readiness_after(OS64_DOM_LOADING, f.document, NULL, "DOMContentLoaded") == OS64_DOM_LOADING &&
              os64_dom_readiness_after(OS64_DOM_LOADING, f.document, body, "load") == OS64_DOM_LOADING &&
              os64_dom_readiness_after(OS64_DOM_LOADING, f.document, root, "click") == OS64_DOM_LOADING &&
              os64_dom_readiness_after(OS64_DOM_LOADING, f.document, root, NULL) == OS64_DOM_LOADING,
          "readiness: nothing else moves it");
    check(os64_dom_readiness_after(OS64_DOM_COMPLETE, f.document, root, "DOMContentLoaded") == OS64_DOM_COMPLETE &&
              os64_dom_readiness_after(OS64_DOM_INTERACTIVE, f.document, root, "DOMContentLoaded") ==
                  OS64_DOM_INTERACTIVE,
          "readiness: it never moves back");
    /* The binding's: loading by default, moved by the host's dispatches,
     * readystatechange just before each, heard through the window too. */
    dom_script(&f, STORAGE_ASSERT
        "assert(document.readyState==='loading');globalThis.seen=[document.readyState];"
        "document.onreadystatechange=e=>seen.push('rsc:'+document.readyState+':'+e.isTrusted+':'+e.bubbles+"
        "':'+e.cancelable+':'+(e.target===document));"
        "addEventListener('readystatechange',()=>seen.push('window-capture'),true);"
        "addEventListener('readystatechange',()=>seen.push('window-bubble'));"
        "document.addEventListener('DOMContentLoaded',()=>seen.push('dcl:'+document.readyState));"
        "addEventListener('load',()=>seen.push('load:'+document.readyState));"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(Object.getPrototypeOf(document),'readyState').get.call(document.body));"
        "document.readyState='complete';assert(document.readyState==='loading','read-only');",
        "readiness: listeners set");
    os64_js_outcome_t outcome;
    os64_dom_event_t loaded = {.type = "DOMContentLoaded", .bubbles = true};
    check(os64_dom_dispatch(f.binding, root, &loaded, NULL, &outcome) == OS64_JS_OK,
          "readiness: DOMContentLoaded dispatched");
    os64_dom_event_t load = {.type = "load"};
    check(os64_dom_dispatch(f.binding, NULL, &load, NULL, &outcome) == OS64_JS_OK, "readiness: load dispatched");
    check(os64_dom_dispatch(f.binding, NULL, &load, NULL, &outcome) == OS64_JS_OK, "readiness: load again");
    dom_script(&f, STORAGE_ASSERT
        "assert(seen.join()==='loading,window-capture,rsc:interactive:true:false:false:true,dcl:interactive,"
        "window-capture,rsc:complete:true:false:false:true,load:complete,load:complete',seen.join());"
        "assert(document.readyState==='complete');",
        "readiness: loading, interactive, complete, readystatechange just before each and never again");
    fixture_free(&f);

    /* A dispatch nobody listens to still moves it; a binding made late is
     * made where the host says the document stands. */
    f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    check(os64_dom_listens(f.binding, "readystatechange") == false && os64_dom_listens(f.binding, "DOMContentLoaded") == false,
          "readiness: nothing listens");
    check(os64_dom_dispatch(f.binding, f.document->document, &loaded, NULL, &outcome) == OS64_JS_OK,
          "readiness: an unheard DOMContentLoaded");
    dom_script(&f, STORAGE_ASSERT "assert(document.readyState==='interactive')", "readiness: moved unheard");
    fixture_free(&f);
    os64_dom_options_t options = os64_dom_default_options();
    options.readiness = OS64_DOM_COMPLETE;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(document.readyState==='complete');let n=0;document.onreadystatechange=()=>n++;"
        "globalThis.count=()=>n;",
        "readiness: a binding made after the load is complete");
    check(os64_dom_dispatch(f.binding, NULL, &load, NULL, &outcome) == OS64_JS_OK, "readiness: load once more");
    dom_script(&f, STORAGE_ASSERT "assert(count()===0,'no move, no readystatechange')", "readiness: complete stays");
    fixture_free(&f);

    /* A readystatechange that throws is the dispatch's outcome, and the
     * event after it still runs; the handler is the document's alone. */
    f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "document.onreadystatechange=()=>{throw new Error('rsc threw')};globalThis.dcl=0;"
        "document.addEventListener('DOMContentLoaded',()=>dcl++);"
        "assert(!('onreadystatechange' in window)&&!('onreadystatechange' in document.body),'the document\\'s alone');"
        "document.body.setAttribute('onreadystatechange','dcl+=100');"
        "document.documentElement.setAttribute('onvisibilitychange','dcl+=100');",
        "readiness: a throwing handler");
    check(os64_dom_dispatch(f.binding, f.document->document, &loaded, NULL, &outcome) == OS64_JS_EXCEPTION &&
              strstr(outcome.message, "rsc threw") != NULL,
          "readiness: readystatechange's throw is the dispatch's outcome");
    dom_script(&f, STORAGE_ASSERT "assert(dcl===1,'DOMContentLoaded still ran, and no content attribute: '+dcl)",
               "readiness: the event after it ran");
    check(os64_dom_handler_bit("readystatechange") == 0 && os64_dom_handler_bit("visibilitychange") == 0 &&
              os64_dom_handler_bit("prerenderingchange") == 0,
          "readiness: no content attribute names the document's own handlers");
    check(os64_dom_handler_attributes(f.document) == 0, "readiness: the attribute walk counts neither attribute");
    fixture_free(&f);

    /* With the switch off there is no readyState and no readystatechange. */
    options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(!('readyState' in document)&&!('onreadystatechange' in document)&&typeof CustomEvent==='undefined');"
        "globalThis.heard=0;document.addEventListener('readystatechange',()=>heard++);",
        "readiness: switch off");
    check(os64_dom_dispatch(f.binding, f.document->document, &loaded, NULL, &outcome) == OS64_JS_OK &&
              os64_dom_dispatch(f.binding, NULL, &load, NULL, &outcome) == OS64_JS_OK,
          "readiness: switch off, dispatched");
    dom_script(&f, STORAGE_ASSERT "assert(heard===0,'nothing fires it')", "readiness: switch off, nothing fired");
    fixture_free(&f);
}

static void dom_document_member_cases(void)
{
    os64_dom_options_t options = document_options();
    char referrer[64];
    snprintf(referrer, sizeof(referrer), "%s", "https://from.example.test/kept");
    options.referrer = referrer;
    options.last_modified = 1700000000;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    snprintf(referrer, sizeof(referrer), "%s", "https://changed.example.test/");
    document_focused = NULL;
    document_has_focus = true;
    setenv("TZ", "UTC0", 1);
    dom_script(&f, STORAGE_ASSERT
        "const P=Object.getPrototypeOf(document);"
        "function ro(n){const d=Object.getOwnPropertyDescriptor(P,n);return d&&typeof d.get==='function'&&d.set===undefined}"
        "for(const n of ['readyState','visibilityState','hidden','referrer','activeElement','currentScript','prerendering',"
        "'URL','documentURI','wasDiscarded','compatMode','characterSet','charset','inputEncoding','contentType',"
        "'lastModified','rootElement'])assert(ro(n),n+' is read-only');"
        "for(const n of ['title','domain','dir']){const d=Object.getOwnPropertyDescriptor(P,n);"
        "assert(typeof d.get==='function'&&typeof d.set==='function',n)}"
        "assert(document.visibilityState==='visible'&&document.hidden===false);"
        "assert(document.prerendering===false&&document.wasDiscarded===false);"
        "assert(document.referrer==='https://from.example.test/kept','the binding\\'s copy: '+document.referrer);"
        "assert(document.URL==='" DOCUMENT_URL "'&&document.documentURI===document.URL);"
        "assert(document.compatMode==='CSS1Compat'&&document.characterSet==='UTF-8'&&document.charset==='UTF-8'&&"
        "document.inputEncoding==='UTF-8'&&document.contentType==='text/html');"
        "assert(document.lastModified==='11/14/2023 22:13:20',document.lastModified);"
        "assert(document.rootElement===null&&document.clear()===undefined&&document.clear.length===0);"
        "assert(document.hasFocus()===true&&document.hasFocus.length===0&&document.activeElement===document.body);"
        "assert(document.currentScript===null);"
        "for(const t of ['visibilitychange','prerenderingchange'])assert(('on'+t) in document&&document['on'+t]===null,t);"
        "let heard=[];document.onvisibilitychange=e=>heard.push(e.type);document.onprerenderingchange=e=>heard.push(e.type);"
        "document.dispatchEvent(new Event('visibilitychange'));document.dispatchEvent(new Event('prerenderingchange'));"
        "assert(heard.join()==='visibilitychange,prerenderingchange',heard.join());",
        "document: P1-Da's members of the window's document");
    setenv("TZ", "EST5EDT", 1);
    dom_script(&f, STORAGE_ASSERT "assert(document.lastModified==='11/14/2023 17:13:20',document.lastModified)",
               "document: lastModified in the local time zone");
    unsetenv("TZ");
    /* A document of its own has no browsing context. */
    dom_script(&f, STORAGE_ASSERT
        "const d=document.implementation.createHTMLDocument('own');"
        "assert(d.readyState==='complete'&&d.visibilityState==='hidden'&&d.hidden===true);"
        "assert(d.referrer===''&&d.currentScript===null&&d.hasFocus()===false&&d.activeElement===d.body);"
        "assert(d.URL==='about:blank'&&d.documentURI==='about:blank'&&d.compatMode==='CSS1Compat');"
        "assert(d.characterSet==='UTF-8'&&d.contentType==='text/html'&&d.title==='own');"
        "assert(d.domain==='www.example.test','its maker\\'s origin');throws('SecurityError',()=>{d.domain='www.example.test'});"
        "assert(/^\\d\\d\\/\\d\\d\\/\\d{4} \\d\\d:\\d\\d:\\d\\d$/.test(d.lastModified),d.lastModified);"
        "const x=document.implementation.createDocument('http://www.w3.org/1999/xhtml','html',null);"
        "assert(x.contentType==='application/xhtml+xml'&&x.readyState==='complete');"
        "const s=document.implementation.createDocument('http://www.w3.org/2000/svg','svg',null);"
        "assert(s.contentType==='image/svg+xml'&&s.rootElement===s.documentElement);"
        "throws('NotSupportedError',()=>s.createElement('g'));"
        "const m=document.implementation.createDocument(null,'',null);"
        "assert(m.contentType==='application/xml'&&m.activeElement===null&&m.rootElement===null&&m.title==='');"
        "m.title='nothing';assert(m.title===''&&m.firstChild===null,'no element: nothing is made');",
        "document: a document of its own");
    fixture_free(&f);
    /* With no referrer, none; with no address, about:blank; with no
     * Last-Modified, the time it is asked; with no focus hook, no focus. */
    options = os64_dom_default_options();
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    setenv("TZ", "UTC0", 1);
    dom_script(&f, STORAGE_ASSERT
        "assert(document.referrer===''&&document.URL==='about:blank'&&document.domain==='');"
        "assert(document.lastModified==='11/14/2023 22:13:20','now, on the host\\'s wall clock: '+document.lastModified);"
        "assert(document.hasFocus()===false&&document.activeElement===document.body);"
        "throws('SecurityError',()=>{document.domain=''});",
        "document: a binding given none of it");
    unsetenv("TZ");
    fixture_free(&f);
}

static void dom_document_focus_cases(void)
{
    os64_dom_options_t options = document_options();
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    document_focused = native_id(f.document->document, "field");
    document_has_focus = false;
    document_focus_asks = 0;
    dom_script(&f, STORAGE_ASSERT
        "assert(document.activeElement===document.getElementById('field'),'the element the host says');"
        "assert(document.hasFocus()===false,'and the host\\'s word on the window');",
        "focus: the focused element");
    check(document_focus_asks == 2, "focus: asked at every read");
    /* The host's element out of the tree, or no element, is no focus. */
    dom_script(&f, "globalThis.field=document.getElementById('field');field.remove()", "focus: the element removed");
    dom_script(&f, STORAGE_ASSERT "assert(document.activeElement===document.body)", "focus: removed is not focused");
    document_focused = f.document->document;
    dom_script(&f, STORAGE_ASSERT "assert(document.activeElement===document.body)", "focus: the document is no element");
    document_focused = NULL;
    /* The window's document keeps its root (libhtml's rule); a document
     * of its own may lose it. */
    dom_script(&f, STORAGE_ASSERT
        "document.body.remove();assert(document.activeElement===document.documentElement,'no body: the root');"
        "const own=document.implementation.createHTMLDocument('');own.documentElement.remove();"
        "assert(own.activeElement===null,'no root: null')",
        "focus: no body, then no root");
    fixture_free(&f);
}

static void dom_document_script_cases(void)
{
    os64_dom_options_t options = document_options();
    DomFixture f = document_fixture("<!doctype html><title>s</title><script id=s>1</script><p id=p>p</p>", "utf-8",
                                    &options);
    if (!fixture_ready(&f)) return;
    const os64_html_node_t *script = native_id(f.document->document, "s");
    const os64_html_node_t *p = native_id(f.document->document, "p");
    os64_dom_set_current_script(f.binding, script);
    dom_script(&f, STORAGE_ASSERT
        "const s=document.getElementById('s');assert(document.currentScript===s,'the running script');"
        "Promise.resolve().then(()=>{globalThis.inJob=document.currentScript});"
        "s.remove();assert(document.currentScript===s,'held, wherever it went');"
        "assert(document.implementation.createHTMLDocument('').currentScript===null,'not a document of its own\\'s');",
        "currentScript: set by the host");
    dom_script(&f, STORAGE_ASSERT "assert(inJob===document.currentScript,'its microtasks see it too')",
               "currentScript: a job of the script's");
    os64_dom_set_current_script(f.binding, p);
    os64_dom_set_current_script(f.binding, f.document->document);
    dom_script(&f, STORAGE_ASSERT "assert(document.currentScript===document.getElementById('p'),'a non-element is refused')",
               "currentScript: only an element");
    os64_dom_set_current_script(f.binding, NULL);
    dom_script(&f, STORAGE_ASSERT "assert(document.currentScript===null)", "currentScript: null between scripts");
    os64_dom_set_current_script(f.binding, script);
    os64_dom_set_current_script(NULL, script);
    fixture_free(&f);
    check(true, "currentScript: a script still set at teardown is let go (the fixture's own accounting)");
}

static void dom_document_title_cases(void)
{
    DomFixture f = document_fixture("<!doctype html><head><title>\t  a \n b  </title></head><body>"
                                    "<title>second</title></body>", "utf-8", NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(document.title==='a b','stripped and collapsed: '+JSON.stringify(document.title));"
        "const t=document.querySelector('title');t.appendChild(document.createElement('b')).textContent='x';"
        "t.appendChild(document.createTextNode(' \\u00a0c\\u000b'));"
        "assert(document.title==='a b \\u00a0c\\u000b','child text only, and only ASCII whitespace: '+JSON.stringify(document.title));"
        "document.title='  set  me ';assert(document.title==='set me'&&t.textContent==='  set  me '&&t.childNodes.length===1);"
        "document.title='';assert(document.title===''&&t.firstChild===null,'no Text node for empty');"
        "document.title={toString(){return 'obj'}};assert(document.title==='obj');"
        "const second=document.getElementsByTagName('title')[1];"
        "document.title={toString(){t.remove();return 'converted first'}};"
        "assert(t.textContent==='obj'&&second.textContent==='converted first','the title its conversion took away is not set');"
        "second.textContent='second';document.head.insertBefore(t,document.head.firstChild);"
        "t.remove();assert(document.title==='second','the next in tree order');"
        "document.querySelector('title').remove();assert(document.title==='');"
        "document.title='made';const n=document.head.lastChild;"
        "assert(n.tagName==='TITLE'&&n.textContent==='made'&&document.title==='made','made in head');"
        "n.remove();document.head.remove();document.title='nowhere';"
        "assert(document.title===''&&document.getElementsByTagName('title').length===0,'no head: nothing');",
        "title: the first title element, and its setter");
    dom_script(&f, STORAGE_ASSERT
        "const s=document.implementation.createDocument('http://www.w3.org/2000/svg','svg',null);"
        "assert(s.title==='');s.title='  svg  t ';const k=s.documentElement.firstChild;"
        "assert(s.title==='svg t'&&k.nodeName==='title','an SVG title, first');"
        "const h=document.implementation.createHTMLDocument();assert(h.title==='');h.title='x';"
        "assert(h.head.lastChild.tagName==='TITLE'&&h.title==='x');"
        /* a root in neither namespace: an HTML title inside it is read, never written */
        "const mm=document.implementation.createDocument('http://www.w3.org/1998/Math/MathML','math',null);"
        "const mt=document.createElement('title');mt.textContent='kept';mm.documentElement.appendChild(mt);"
        "assert(mm.title==='kept');mm.title='written';"
        "assert(mt.textContent==='kept'&&mm.title==='kept','a MathML-rooted document takes no title');",
        "title: an SVG document's, and a document of its own's");
    fixture_free(&f);
}

static void dom_document_reflection_cases(void)
{
    DomFixture f = document_fixture("<html dir=RTL><p>quirks</p>", "windows-1252", NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(document.compatMode==='BackCompat','no doctype: quirks');"
        "assert(document.characterSet==='windows-1252'&&document.charset==='windows-1252');"
        "assert(document.dir==='rtl');document.dir='LTR';"
        "assert(document.documentElement.getAttribute('dir')==='LTR'&&document.dir==='ltr');"
        "document.dir='sideways';assert(document.dir===''&&document.documentElement.getAttribute('dir')==='sideways');"
        "const s=document.implementation.createDocument('http://www.w3.org/2000/svg','svg',null);"
        "assert(s.dir==='');s.dir='rtl';assert(!s.documentElement.hasAttribute('dir'),'no html element: nothing set');"
        /* the value is converted before the element is looked for */
        "const own=document.implementation.createHTMLDocument(''),root=own.documentElement;"
        "own.dir={toString(){root.remove();return 'rtl'}};"
        "assert(own.documentElement===null&&!root.hasAttribute('dir'),'the root its conversion took away is not set');",
        "dir, compatMode and characterSet");
    fixture_free(&f);
}

static void dom_document_domain_cases(void)
{
    static const struct { const char *url; const char *domain; const char *ok, *refused; } kPages[] = {
        {"https://www.example.test:8443/x", "www.example.test", "example.test", "test"},
        {"https://www.example.test/", "www.example.test", "WWW.Example.Test", "ample.test"},
        {"http://192.168.0.1/", "192.168.0.1", "192.168.0.1", "168.0.1"},
        {"file:///home/page.html", "", NULL, "home"},
    };
    for (size_t i = 0; i < sizeof(kPages) / sizeof(kPages[0]); i++) {
        os64_dom_options_t options = document_options();
        options.url = kPages[i].url;
        DomFixture f = fixture_new(&options);
        if (!fixture_ready(&f)) return;
        char source[640];
        snprintf(source, sizeof(source), STORAGE_ASSERT
                 "assert(document.domain==='%s','domain: '+document.domain);"
                 "if(%d){document.domain='%s';assert(document.domain==='%s','set, and nothing relaxed')}"
                 "throws('SecurityError',()=>{document.domain='%s'});"
                 "throws('SecurityError',()=>{document.domain='other.test'});",
                 kPages[i].domain, kPages[i].ok != NULL, kPages[i].ok != NULL ? kPages[i].ok : "",
                 kPages[i].domain, kPages[i].refused);
        dom_script(&f, source, kPages[i].url);
        fixture_free(&f);
    }
}

/* document.body's setter: every page's, the switch on and off. */
static void dom_document_body_cases(void)
{
    for (int modern = 1; modern >= 0; modern--) {
        os64_dom_options_t options = os64_dom_default_options();
        options.modern = modern;
        DomFixture f = fixture_new(&options);
        if (!fixture_ready(&f)) return;
        dom_script(&f, STORAGE_ASSERT
            "const old=document.body,d=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(document),'body');"
            "assert(typeof d.set==='function');"
            "document.body=old;assert(document.body===old,'the same body: nothing');"
            "const b=document.createElement('body');b.id='new';document.body=b;"
            "assert(document.body===b&&old.parentNode===null&&b.parentNode===document.documentElement,'replaced');"
            "throws('HierarchyRequestError',()=>{document.body=document.createElement('div')});"
            "throws('HierarchyRequestError',()=>{document.body=null});"
            "throws('TypeError',()=>{document.body=document.createTextNode('t')});"
            "throws('TypeError',()=>{document.body={}});"
            "b.remove();const fs=document.createElement('frameset');document.body=fs;"
            "assert(document.body===fs&&document.documentElement.lastChild===fs,'appended to the root');"
            "const own=document.implementation.createHTMLDocument('o'),ob=own.createElement('body');own.body=ob;"
            "assert(own.body===ob&&ob.ownerDocument===own);"
            "own.documentElement.remove();throws('HierarchyRequestError',()=>{own.body=own.createElement('body')});",
            modern ? "body: its setter, switch on" : "body: its setter, switch off");
        fixture_free(&f);
    }
}

static void dom_document_custom_event_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "assert(typeof CustomEvent==='function'&&CustomEvent.length===1&&CustomEvent.name==='CustomEvent');"
        "assert(Object.getPrototypeOf(CustomEvent)===Event&&Object.getPrototypeOf(CustomEvent.prototype)===Event.prototype);"
        "throws('TypeError',()=>CustomEvent('x'));throws('TypeError',()=>new CustomEvent());"
        "const e=new CustomEvent('x');assert(e.detail===null&&e.type==='x'&&!e.bubbles&&e instanceof Event);"
        "assert(Object.prototype.toString.call(e)==='[object CustomEvent]');"
        "const o={a:1};const c=new CustomEvent('y',{detail:o,bubbles:true,cancelable:true});"
        "assert(c.detail===o&&c.bubbles&&c.cancelable);"
        "assert(new CustomEvent('z',{detail:undefined}).detail===null&&new CustomEvent('z',{detail:0}).detail===0);"
        "throws('TypeError',()=>new CustomEvent('z',1));"
        "const getter=Object.getOwnPropertyDescriptor(CustomEvent.prototype,'detail').get;"
        "throws('TypeError',()=>getter.call(new Event('x')));throws('TypeError',()=>getter.call({}));"
        "throws('TypeError',()=>CustomEvent.prototype.initCustomEvent.call(new Event('x'),'a'));"
        "class Mine extends CustomEvent{};const m=new Mine('m',{detail:2});"
        "assert(m instanceof Mine&&m.detail===2,'a subclass');"
        "c.self=c;const cyc=new CustomEvent('cycle',{detail:{back:null}});cyc.detail.back=cyc;",
        "CustomEvent: the constructor and detail");
    dom_script(&f, STORAGE_ASSERT
        /* The banking SDK's idiom, as it dies today. */
        "var el=document.getElementById('box'),got=[];"
        "el.addEventListener('x',function(e){got.push(e.detail.a,e.type,e.bubbles,e.cancelable,e.target===el)});"
        "document.body.addEventListener('x',function(e){got.push('bubbled')});"
        "var ev=document.createEvent('CustomEvent');"
        "assert(Object.getPrototypeOf(ev)===CustomEvent.prototype&&ev.type===''&&ev.detail===null);"
        "throws('InvalidStateError',()=>el.dispatchEvent(ev));"
        "ev.initCustomEvent('x',true,true,{a:1});assert(el.dispatchEvent(ev)===true);"
        "assert(got.join()==='1,x,true,true,true,bubbled',got.join());"
        /* during a dispatch it does nothing */
        "var d=new CustomEvent('during',{detail:'kept'});"
        "el.addEventListener('during',ev=>{ev.initCustomEvent('changed',false,false,'lost');got.push(ev.type,ev.detail)});"
        "el.dispatchEvent(d);assert(got.slice(-2).join()==='during,kept');"
        "d.initCustomEvent('again');assert(d.type==='again'&&d.detail===null&&!d.bubbles,'defaults');",
        "CustomEvent: createEvent('CustomEvent') and initCustomEvent, the banking SDK's idiom");
    fixture_free(&f);
}

static void dom_document_create_event_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "function proto(n){return Object.getPrototypeOf(document.createEvent(n))}"
        "for(const n of ['Event','events','HTMLEVENTS','SVGEvents','UIEvent','UIEvents','MouseEvent','MOUSEEVENTS',"
        "'KeyboardEvent','FocusEvent','CompositionEvent','DragEvent','MessageEvent','StorageEvent','TextEvent',"
        "'BeforeUnloadEvent','DeviceMotionEvent','DeviceOrientationEvent'])assert(proto(n)===Event.prototype,n);"
        "assert(proto('customevent')===CustomEvent.prototype&&proto('HashChangeEvent')===HashChangeEvent.prototype);"
        "const h=document.createEvent('hashchangeevent');assert(h.newURL===''&&h.oldURL===''&&h.type==='');"
        "for(const n of ['foo','TouchEvent','touchevent','PopStateEvent','ProgressEvent','MediaQueryListEvent',"
        "'CustomEvents','KeyboardEvents','U\\u0130Event','U\\u0131Event','Event\\u0000','','MouseEvent '])"
        "throws('NotSupportedError',()=>document.createEvent(n));"
        "throws('TypeError',()=>document.createEvent());"
        "const e=document.createEvent('MouseEvents');e.initEvent('click',true,true);"
        "let n=0;document.body.addEventListener('click',()=>n++);document.body.dispatchEvent(e);"
        "assert(n===1,'MouseEvents and initEvent still dispatch a click');",
        "createEvent: DOM's table");
    fixture_free(&f);
    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, STORAGE_ASSERT
        "for(const n of ['foo','CustomEvent','TouchEvent'])"
        "{const e=document.createEvent(n);assert(Object.getPrototypeOf(e)===Event.prototype&&!('detail' in e),n)}"
        "assert(document.createEvent().type==='');",
        "createEvent: switch off, every name a plain Event, as before the arc");
    fixture_free(&f);
}

static void dom_document_sweep_cases(void)
{
    os64_dom_options_t options = document_options();
    options.referrer = "https://from.example.test/";
    static const char *const kOperations[] = {
        "[document.readyState,document.referrer,document.visibilityState,document.URL,document.domain,"
        "document.compatMode,document.characterSet,document.contentType,document.lastModified]",
        "document.title='a new title';document.title",
        "document.querySelector('title').remove();document.title='made';document.title",
        "document.domain='example.test'",
        "document.dir='rtl';document.body=document.createElement('body')",
        "var e=document.createEvent('CustomEvent');e.initCustomEvent('x',true,true,{a:1});document.body.dispatchEvent(e)",
        "new CustomEvent('x',{detail:[1]}).detail",
        "[document.activeElement,document.hasFocus(),document.currentScript,"
        "document.implementation.createHTMLDocument('t').title]",
    };
    size_t n = sizeof(kOperations) / sizeof(kOperations[0]);
    binding_sweep("document", kOperations, n, &options);
    engine_sweep("document", kOperations, n, &options);
}

static void dom_document_cases(void)
{
    dom_document_readiness_cases();
    dom_document_member_cases();
    dom_document_focus_cases();
    dom_document_script_cases();
    dom_document_title_cases();
    dom_document_reflection_cases();
    dom_document_domain_cases();
    dom_document_body_cases();
    dom_document_custom_event_cases();
    dom_document_create_event_cases();
    dom_document_sweep_cases();
}
