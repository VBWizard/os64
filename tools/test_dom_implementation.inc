/* DOMIMPLEMENTATION AND THE DOCUMENTS IT MAKES (libdom/implementation.c;
 * LIBDOM.md § Documents of their own): a case per sentence of the
 * contract. document.implementation and its interface; createHTMLDocument's
 * tree, titled and not; jQuery's feature test, verbatim; a node made in a
 * new document adopted into the page and found there; which document a
 * node belongs to through every way it can be made, moved, taken out and
 * copied; what a document with no window cannot do (location, cookies,
 * defaultView, write, navigation, submission, focus, its scripts and its
 * handler attributes); createDocument and createDocumentType with their
 * refusals; hasFeature; the receivers; every allocation the binding
 * makes failed in turn; and the two jQuerys the kept corpus carries,
 * whole and unedited, booting. */

#define IMPL_ASSERT STORAGE_ASSERT

/* A file's bytes, the harness's malloc; NULL when it cannot be read. */
static char *implementation_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    char *text = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n + 1 >= cap) {
            cap = cap ? cap * 2 : 65536;
            text = realloc(text, cap);
            if (text == NULL) abort();
        }
        size_t got = fread(text + n, 1, cap - n - 1, file);
        if (got == 0) break;
        n += got;
    }
    bool failed = ferror(file) != 0;
    fclose(file);
    if (failed) { free(text); return NULL; }
    text[n] = '\0';
    *length = n;
    return text;
}

/* A library run whole in a page of the classic fixture, then `after`,
 * which holds it to what booting means. The page's engine takes a source
 * of a megabyte, as a page's does in yonder. `moved` is the message it
 * died with before, said by name when it dies with it again. */
static void implementation_library(const char *path, const char *after, const char *what,
                                   const char *moved)
{
    size_t length = 0;
    char *source = implementation_file(path, &length);
    char line[256];
    snprintf(line, sizeof(line), "%s: the file reads", what);
    check(source != NULL, line);
    if (source == NULL) return;
    DomFixture f = fixture_base(0, 0, 0);
    os64_js_destroy(f.runtime);
    os64_js_config_t config = fixture_config();
    config.limits.source_bytes = 1024 * 1024;
    f.runtime = create_fixture(&config);
    os64_js_outcome_t outcome;
    f.binding = os64_dom_create(f.runtime, f.document, f.state, NULL, &outcome);
    if (!fixture_ready(&f)) { free(source); return; }
    os64_js_status_t result = os64_js_run(f.runtime, source, length, path, &outcome);
    if (moved != NULL) {
        snprintf(line, sizeof(line), "%s: no longer stops at \"%s\"", what, moved);
        check(result == OS64_JS_OK || strstr(outcome.message, moved) == NULL, line);
    }
    if (result != OS64_JS_OK)
        fprintf(stderr, "%s: status=%d message=%s\n%s\n", what, result, outcome.message, outcome.stack_trace);
    snprintf(line, sizeof(line), "%s: the library runs to its end", what);
    check(result == OS64_JS_OK, line);
    snprintf(line, sizeof(line), "%s: it works", what);
    if (result == OS64_JS_OK) dom_script(&f, after, line);
    free(source);
    fixture_free(&f);
}

/* What booting means for either jQuery: it is on window, it selects, it
 * parses markup into nodes the page then holds, and an event bound through
 * it is heard. */
#define JQUERY_WORKS IMPL_ASSERT \
    "assert(typeof jQuery==='function'&&window.$===jQuery,'on window');" \
    "assert($('#box p').length===2&&$('#one').text()==='one','selects');" \
    "var made=$('<p class=made>new</p>');made.appendTo('#box');" \
    "assert(document.querySelectorAll('#box .made').length===1,'parses and appends');" \
    "var parsed=$.parseHTML('<b id=parsed>x</b><script>window.ran=1</'+'script>');" \
    "assert(parsed.length===1&&parsed[0].nodeName==='B','parseHTML leaves its scripts out');" \
    "$('#box').append(parsed);assert(document.getElementById('parsed')===parsed[0],'the parsed node is the page\\'s');" \
    "assert(window.ran===undefined,'no script ran');" \
    "var heard=0;$('#two').on('click',function(){heard++});$('#two').trigger('click');" \
    "assert(heard===1,'an event is heard');" \
    "assert($('#field').val()==='default','a value');"

/* jQuery 1.12's support probes set an input's type and then its checked
 * (`c.type="checkbox",c.checked=!0`): with no `type` the input stayed a
 * text input and libpage refused its checkedness. Past that, the library
 * reads `e.attributes[name].expando` (its focusin probe), which died while
 * the binding had no `attributes` (attr.c builds it). The library is held
 * to booting whole, which is past both. */
#define JQUERY_1_ATTRIBUTES "cannot read property of undefined"
#define JQUERY_1_WORKS "assert(jQuery.support.appendChecked===true&&jQuery.support.checkClone===true," \
    "'support.appendChecked');assert(jQuery.support.focusin===false,'the focusin probe read attributes');"

static void implementation_jquery_cases(void)
{
    implementation_library("tools/fixtures/jquery-3.5.1.js", JQUERY_WORKS
        "assert(jQuery.fn.jquery==='3.5.1');"
        /* the feature test the library ran at load */
        "assert(jQuery.support.createHTMLDocument===true,'support.createHTMLDocument');",
        "jQuery 3.5.1", "createHTMLDocument");
    implementation_library("tools/fixtures/jquery-1.12.4.js", JQUERY_WORKS
        "assert(jQuery.fn.jquery==='1.12.4');" JQUERY_1_WORKS, "jQuery 1.12.4", JQUERY_1_ATTRIBUTES);
    /* The corpus's own build is not in the tree (it carries the vendor's
     * additions to the library); it is run when the harness is told where
     * a copy is, and said to be skipped when not. */
    const char *aem = getenv("OS64_JQUERY_AEM");
    if (aem != NULL && *aem != '\0')
        implementation_library(aem, JQUERY_WORKS "assert(jQuery.fn.jquery==='1.12.4-aem');" JQUERY_1_WORKS,
                               "jQuery 1.12.4-aem", JQUERY_1_ATTRIBUTES);
    else
        printf("DOM implementation: jQuery 1.12.4-aem skipped: OS64_JQUERY_AEM names no copy of it\n");
}

static void implementation_shape_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "var i=document.implementation;assert(i===document.implementation,'[SameObject]');"
        "assert(i instanceof DOMImplementation&&Object.getPrototypeOf(i)===DOMImplementation.prototype);"
        "throws('TypeError',()=>new DOMImplementation());"
        "assert(Object.prototype.toString.call(i)==='[object DOMImplementation]');"
        "var d=i.createHTMLDocument('t');assert(d.implementation!==i&&d.implementation===d.implementation,"
        "'one per document');"
        "assert(i.hasFeature()===true&&i.hasFeature('Core','9')===true,'hasFeature answers true');"
        "var p=DOMImplementation.prototype;"
        "for(const k of ['createHTMLDocument','createDocument','createDocumentType','hasFeature'])"
        "throws('TypeError',()=>p[k].call({}));"
        "throws('TypeError',()=>p.createHTMLDocument.call(document));"
        "var g=Object.getOwnPropertyDescriptor(Document.prototype,'implementation').get;"
        "throws('TypeError',()=>g.call(document.body));"
        "assert(p.createDocumentType.length===3&&p.createDocument.length===2&&p.createHTMLDocument.length===0);",
        "implementation: the object and its interface");
    dom_script(&f, IMPL_ASSERT
        "var t=document.doctype;assert(t===document.firstChild&&t instanceof DocumentType&&t.nodeType===10);"
        "assert(t.name==='html'&&t.publicId===''&&t.systemId===''&&t.nodeName==='html'&&t.nodeValue===null);"
        "assert(typeof t.remove==='function'&&typeof t.before==='function'&&t.nextElementSibling===undefined);"
        "var n=Object.getOwnPropertyDescriptor(DocumentType.prototype,'name').get;"
        "throws('TypeError',()=>n.call(document.body));"
        "assert(Object.prototype.toString.call(t)==='[object DocumentType]');",
        "implementation: the window's doctype and DocumentType");
    fixture_free(&f);

    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "assert(typeof DOMImplementation==='undefined'&&typeof DocumentType==='undefined');"
        "var d=document.implementation.createHTMLDocument('t');assert(d.body.nodeName==='BODY');"
        "assert(document.doctype.name==='html');",
        "implementation: with the switch off, the classic surface and no interface objects");
    fixture_free(&f);
}

static void implementation_html_document_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "var d=document.implementation.createHTMLDocument('the title');"
        "assert(d instanceof Document&&d.nodeType===9&&d.childNodes.length===2&&d.ownerDocument===null);"
        "assert(d.doctype===d.firstChild&&d.doctype.name==='html'&&d.doctype.publicId===''&&d.doctype.systemId==='');"
        "var h=d.documentElement;assert(h===d.lastChild&&h.nodeName==='HTML'&&h.childNodes.length===2);"
        "assert(d.head===h.firstChild&&d.body===h.lastChild&&d.head.nodeName==='HEAD'&&d.body.nodeName==='BODY');"
        "assert(d.head.childNodes.length===1&&d.head.firstChild.nodeName==='TITLE');"
        "assert(d.head.firstChild.firstChild.data==='the title'&&d.body.childNodes.length===0);"
        "assert(d.body.ownerDocument===d&&d.doctype.ownerDocument===d,'its nodes are its own');"
        "assert(d!==document&&document.body!==d.body,'a new document, not the window\\'s');",
        "createHTMLDocument: the document with a title");
    dom_script(&f, IMPL_ASSERT
        "var d=document.implementation.createHTMLDocument();"
        "assert(d.head.childNodes.length===0,'no title asked for, no title element');"
        "d=document.implementation.createHTMLDocument(undefined);assert(d.head.childNodes.length===0);"
        "d=document.implementation.createHTMLDocument('');"
        "assert(d.head.childNodes.length===1&&d.head.firstChild.childNodes.length===1&&"
        "d.head.firstChild.firstChild.data==='','the empty title is a title');"
        "d=document.implementation.createHTMLDocument(null);assert(d.head.firstChild.textContent==='null');"
        "d=document.implementation.createHTMLDocument({toString(){return 'made'}});"
        "assert(d.head.firstChild.textContent==='made');",
        "createHTMLDocument: no title, an empty one, and converted ones");
    /* jQuery 3's support.createHTMLDocument, as the library writes it. */
    dom_script(&f, IMPL_ASSERT
        "var body=document.implementation.createHTMLDocument('').body;"
        "body.innerHTML='<form></form><form></form>';"
        "assert(body.childNodes.length===2,'jQuery\\'s feature test');",
        "createHTMLDocument: jQuery's feature test, verbatim");
    dom_script(&f, IMPL_ASSERT
        "var d=document.implementation.createHTMLDocument('');"
        "var made=d.createElement('DIV');assert(made.nodeName==='DIV'&&made.ownerDocument===d,'createElement folds');"
        "var text=d.createTextNode('words');made.appendChild(text);d.body.appendChild(made);"
        "made.id='there';made.expando=7;var heard=0;made.addEventListener('click',()=>heard++);"
        "assert(d.querySelector('#there')===made&&document.querySelector('#there')===null);"
        "d.body.innerHTML='<p class=k>a</p><p class=k>b</p>';"
        "assert(d.querySelectorAll('.k').length===2&&d.getElementsByTagName('p').length===2);"
        "assert(made.parentNode===null&&made.ownerDocument===d,'taken out by innerHTML, still the new document\\'s');"
        "document.body.appendChild(made);"
        "assert(made.ownerDocument===document&&text.ownerDocument===document,'adopted with its subtree');"
        "assert(document.querySelector('#there')===made&&document.getElementById('there')===made,"
        "'the page\\'s queries find it');"
        "assert(made.expando===7&&made.textContent==='words','the same object');"
        "made.click();assert(heard===1,'its listener moved with it');",
        "createHTMLDocument: a node made there, adopted into the page and read back");
    fixture_free(&f);
}

/* Which document a node belongs to (DOM § 4.4, "node document"), through
 * each way a node is made, moved, taken out and copied. */
static void implementation_owner_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "var d=document.implementation.createHTMLDocument('');"
        "assert(d.createComment('c').ownerDocument===d&&d.createTextNode('t').ownerDocument===d&&"
        "d.createDocumentFragment().ownerDocument===d,'made there');"
        "var p=document.getElementById('one');d.body.appendChild(p);"
        "assert(p.ownerDocument===d&&p.firstChild.ownerDocument===d,'the window\\'s node moved there');"
        "p.remove();assert(p.ownerDocument===d,'and taken out, still there');"
        "document.body.appendChild(p);assert(p.ownerDocument===document);"
        "document.body.removeChild(p);assert(p.ownerDocument===document,'taken out of the page, the page\\'s');"
        "var q=d.createElement('q');var r=d.createElement('r');q.appendChild(r);"
        "assert(r.ownerDocument===d,'under a node made there');"
        "var copy=q.cloneNode(true);assert(copy.ownerDocument===d&&copy.firstChild.ownerDocument===d,'a copy');"
        "d.body.appendChild(q);var old=d.body.replaceChild(d.createElement('x'),q);"
        "assert(old===q&&q.ownerDocument===d,'replaced out');"
        "var e=document.createElement('e');d.body.append(e);e.replaceWith(document.createElement('y'));"
        "assert(e.ownerDocument===d,'replaced with replaceWith');"
        "var kids=d.createElement('div');d.body.appendChild(kids);var kid=d.createElement('i');kids.appendChild(kid);"
        "kids.textContent='';assert(kid.ownerDocument===d,'taken out by textContent');"
        "var f2=d.createDocumentFragment();var g=d.createElement('g');f2.appendChild(g);document.body.appendChild(f2);"
        "assert(g.ownerDocument===document&&f2.ownerDocument===d,'a fragment\\'s children are adopted, not the fragment');"
        "assert(document.ownerDocument===null&&d.ownerDocument===null);"
        /* a root that never had a wrapper keeps no record, and reads as the
         * window's (LIBDOM.md § Documents of their own says so) */
        "d.body.innerHTML='<div><b id=deep>x</b></div>';var deep=d.getElementById('deep');"
        "assert(deep.ownerDocument===d);d.body.innerHTML='';"
        "assert(deep.parentNode.parentNode===null&&deep.ownerDocument===document,'an unwrapped root says nothing');",
        "ownerDocument through creation, moves, removals, replacements and copies");
    fixture_free(&f);
}

/* A document with no window: everything that would reach the host or the
 * page's parser does nothing, or says why not. */
static void implementation_windowless_cases(void)
{
    DomFixture f = event_fixture(event_html, "http://fixture.test/dir/page.html");
    if (!fixture_ready(&f)) return;
    os64_dom_cookies_t cookies = {cookie_get, cookie_set, NULL};
    os64_dom_set_cookies(f.binding, &cookies);
    cookie_heard[0] = 0;
    write_answer = OS64_HTML_OK;
    dom_script(&f, IMPL_ASSERT
        "var d=document.implementation.createHTMLDocument('');globalThis.d=d;"
        "assert(d.location===null&&document.location===location,'no location');"
        "throws('TypeError',()=>{d.location='http://fixture.test/'});"
        "assert(d.cookie===''&&document.cookie==='a=1; b=2','cookie-averse');d.cookie='z=9';"
        "throws('InvalidStateError',()=>d.write('<p>'));throws('InvalidStateError',()=>d.writeln('<p>'));"
        "throws('InvalidStateError',()=>d.open());assert(d.close()===undefined);"
        "assert(d.defaultView===null&&document.defaultView===window,'no window for defaultView');"
        "assert(d.scrollingElement===d.documentElement,'its root scrolls');",
        "windowless: location, cookies, the parser and its view");
    check(cookie_heard[0] == 0, "windowless: a cookie set reaches no provider");
    check(writes == 0, "windowless: a write reaches no parser");
    os64_dom_navigation_t ask;
    dom_script(&f, IMPL_ASSERT
        "d.body.innerHTML='<a id=far href=\"/far\">x</a><form id=f action=/send><input id=i>"
        "<button id=b>go</button></form>';"
        "var clicked=0;d.getElementById('far').addEventListener('click',()=>clicked++);"
        "d.getElementById('far').click();assert(clicked===1,'the click is an event');"
        "d.getElementById('b').click();d.getElementById('f').submit();d.getElementById('f').requestSubmit();"
        "d.getElementById('i').focus();d.getElementById('i').blur();",
        "windowless: a link, a form and focus");
    check(!os64_dom_take_navigation(f.binding, &ask), "windowless: no navigation or submission is asked for");
    check(activations == 0, "windowless: no focus or blur reaches the host");
    dom_script(&f, IMPL_ASSERT
        "d.body.innerHTML='<p id=h onclick=\"window.handled=1\">h</p>';var h=d.getElementById('h');"
        "assert(h.onclick===null,'a handler attribute is not compiled');h.click();assert(window.handled===undefined);"
        "document.body.appendChild(h);assert(typeof h.onclick==='function');h.click();assert(window.handled===1,"
        "'until the element is the window\\'s');",
        "windowless: handler attributes");
    connected_count = 0;
    dom_script(&f, IMPL_ASSERT
        "d.body.innerHTML='<script>window.parsedRan=1</'+'script>';"
        "var s=d.createElement('script');s.textContent='window.madeRan=1';d.body.appendChild(s);"
        "document.body.appendChild(d.body.firstChild);document.body.appendChild(s);"
        "var fresh=d.createElement('script');fresh.textContent='window.freshRan=1';document.body.appendChild(fresh);",
        "windowless: scripts set into it, connected to it, then moved into the page");
    check(connected_count == 1, "windowless: only the script first connected to the page is handed to the host");
    fixture_free(&f);
}

/* A body's window-reflecting handlers (onload, onfocus, onblur) are
 * window's only for the window's body (HTML § 8.1.8.1, "determining the
 * target of an event handler"); a new document's body has none. */
static void implementation_reflection_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "var mine=function(){};window.onload=mine;assert(document.body.onload===mine,'the window\\'s body reflects');"
        "var z=document.implementation.createHTMLDocument('');"
        "assert(z.body.onload===null,'a new document\\'s body reads null');"
        "z.body.onload=function(){};assert(window.onload===mine&&z.body.onload===null,'and an assignment changes nothing');"
        "z.body.setAttribute('onload','window.hijacked=1');z.body.setAttribute('onfocus','window.hijacked=1');"
        "assert(window.onload===mine&&window.onfocus===null,'nor does its attribute');"
        "var r=document.implementation.createHTMLDocument('').createElement('frameset');r.onblur=function(){};"
        "assert(r.onblur===null&&window.onblur===null,'a frameset of its own neither');"
        "z.body.onclick=function(){};assert(typeof z.body.onclick==='function','a handler that does not reflect is its own');",
        "reflected handlers: only the window's body reaches window");
    fixture_free(&f);
}

/* DOM's connected is any document's tree; running a script asks for the
 * window's (internal.h's two kinds of connected). */
static void implementation_connected_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "var d=document.implementation.createHTMLDocument('');"
        "assert(d.body.isConnected===true&&document.body.isConnected===true,'in a document, any document');"
        "assert(d.createElement('p').isConnected===false&&document.createElement('p').isConnected===false);"
        "d.body.innerHTML='<form id=f></form><input id=i form=f><input id=j form=nothing>';"
        "var form=d.getElementById('f');"
        "assert(form.elements.length===1&&form.elements[0]===d.getElementById('i'),"
        "'a form attribute names a form in its own document\\'s tree');"
        "document.body.appendChild(d.getElementById('i'));assert(form.elements.length===0,'and only there');",
        "connected: isConnected and a form attribute in a document of its own");
    /* Scripting is the page's (the fixture's options have it on), and off
     * for a node of a document with no window: noscript's contents are
     * markup there, and text in the page. */
    dom_script(&f, IMPL_ASSERT
        "var z=document.implementation.createHTMLDocument('');"
        "z.body.innerHTML='<noscript><b>x</b></noscript>';"
        "assert(z.body.firstChild.firstChild.nodeType===1,'innerHTML in a document of its own: markup');"
        "assert(z.body.innerHTML==='<noscript><b>x</b></noscript>','and serialized as markup');"
        "z.body.firstChild.insertAdjacentHTML('afterend','<noscript><i>y</i></noscript>');"
        "assert(z.body.lastChild.firstChild.nodeType===1,'insertAdjacentHTML there: markup');"
        "var e=document.getElementById('box');e.innerHTML='<noscript><b>x</b></noscript>';"
        "assert(e.firstChild.firstChild.nodeType===3,'in the page: text');",
        "scripting: a document with no window parses noscript as markup");
    fixture_free(&f);
}

static void implementation_document_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "var I=document.implementation,HTML='http://www.w3.org/1999/xhtml',SVG='http://www.w3.org/2000/svg';"
        "var e=I.createDocument(null,'');assert(e instanceof Document&&e.childNodes.length===0&&e.documentElement===null);"
        "assert(I.createDocument('','',null).childNodes.length===0&&I.createDocument(null,null).childNodes.length===0);"
        "var x=I.createDocument(HTML,'html');assert(x.documentElement.nodeName==='HTML'&&x.childNodes.length===1);"
        "assert(x.documentElement.ownerDocument===x,'its root is its own');"
        "var t=I.createDocumentType('svg','-//W3C//DTD SVG 1.1//EN','http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd');"
        "var s=I.createDocument(SVG,'svg',t);"
        "assert(s.firstChild===t&&s.doctype===t&&s.documentElement.nodeName==='svg'&&t.ownerDocument===s);"
        "assert(s.head===null&&s.body===null&&s.location===null,'an SVG document has no head or body');"
        "assert(I.createDocument(HTML,'template').documentElement.nodeName==='TEMPLATE');",
        "createDocument: with no root, a root, and a doctype before it");
    dom_script(&f, IMPL_ASSERT
        "var I=document.implementation,HTML='http://www.w3.org/1999/xhtml',SVG='http://www.w3.org/2000/svg';"
        "var x=I.createDocument(HTML,'');var kept=x.createElement('DiV');"
        "assert(kept.outerHTML.indexOf('<DiV')===0,'an XML document keeps the case');"
        "throws('NotSupportedError',()=>I.createDocument(SVG,'').createElement('g'));"
        "throws('NotSupportedError',()=>I.createDocument(null,'').createElement('g'));"
        "throws('NotSupportedError',()=>I.createDocument(null,'root'));"
        "throws('NotSupportedError',()=>I.createDocument('urn:other','root'));"
        "throws('NamespaceError',()=>I.createDocument(null,'a:b'));"
        "throws('NamespaceError',()=>I.createDocument(HTML,'xml:b'));"
        "throws('NamespaceError',()=>I.createDocument(HTML,'xmlns'));"
        "throws('NamespaceError',()=>I.createDocument('http://www.w3.org/2000/xmlns/','a'));"
        "throws('InvalidCharacterError',()=>I.createDocument(HTML,'1a'));"
        "throws('InvalidCharacterError',()=>I.createDocument(HTML,':a'));"
        "throws('InvalidCharacterError',()=>I.createDocument(HTML,'a b'));"
        "throws('TypeError',()=>I.createDocument(HTML,'html',document.body));"
        "throws('TypeError',()=>I.createDocument(HTML));"
        "assert(I.createDocument(SVG,'svg:svg').documentElement.nodeName==='svg:svg','a prefix');"
        /* the standard's split at the first colon, as WPT's createElementNS
         * table at the pin has it: not XML's QName */
        "for(const ok of ['0:a','f:o:o','prefix::local','a:_','\u037E:a'])"
        "assert(I.createDocument(SVG,ok).documentElement.nodeName===ok,ok);"
        "for(const bad of ['a:0','foo:',':foo','namespaceURI:{','a: b'])"
        "throws('InvalidCharacterError',()=>I.createDocument(SVG,bad));",
        "createDocument: the XML document's createElement, and the refusals");
    dom_script(&f, IMPL_ASSERT
        "var I=document.implementation;var d=I.createHTMLDocument('');"
        "var t=d.implementation.createDocumentType('x.y','pub','sys');"
        "assert(t.name==='x.y'&&t.publicId==='pub'&&t.systemId==='sys'&&t.nodeType===10&&t.ownerDocument===d);"
        "assert(I.createDocumentType('','','').name===''&&I.createDocumentType('a','','').ownerDocument===document);"
        "throws('InvalidCharacterError',()=>I.createDocumentType('a b','',''));"
        "throws('InvalidCharacterError',()=>I.createDocumentType('a>','',''));"
        "throws('InvalidCharacterError',()=>I.createDocumentType('a\\0','',''));"
        "throws('InvalidCharacterError',()=>I.createDocumentType('\\t','',''));"
        "throws('TypeError',()=>I.createDocumentType('a',''));"
        "var o=I.createDocument(null,'',t);assert(o.doctype===t&&t.ownerDocument===o,'moved into another');"
        "t.remove();assert(o.doctype===null&&t.ownerDocument===o);"
        "var w=document.doctype;var moved=I.createDocument(null,'',w);"
        "assert(moved.doctype===w&&document.doctype===null&&w.ownerDocument===moved,'even the window\\'s');",
        "createDocumentType: its strings, its document, its refusals, and moving one");
    fixture_free(&f);
}

/* The page's arena running out under createHTMLDocument and
 * createDocument is refused by its name, whichever node's creation it
 * stops at: the status is read after the call that wrote it. */
static void implementation_arena_cases(void)
{
    static const char *const kMakers[] = {
        "document.implementation.createHTMLDocument('a title')",
        "document.implementation.createDocument('http://www.w3.org/1999/xhtml','html')",
    };
    for (size_t m = 0; m < sizeof(kMakers) / sizeof(kMakers[0]); m++) {
        DomFixture f = fixture_base(24 * 1024, 0, 0);
        if (f.runtime == NULL || f.state == NULL) return;
        os64_js_outcome_t outcome;
        f.binding = os64_dom_create(f.runtime, f.document, f.state, NULL, &outcome);
        if (!fixture_ready(&f)) return;
        char source[512];
        snprintf(source, sizeof(source), IMPL_ASSERT
                 "var name='none';for(var i=0;i<100000;i++){try{%s}catch(e){name=e.name;break}}"
                 "assert(name==='QuotaExceededError','the arena refusal: '+name);", kMakers[m]);
        dom_script(&f, source, "arena: a document made past the page's arena is QuotaExceededError");
        fixture_free(&f);
    }
}

/* Every allocation each operation makes, the binding's own and the
 * engine's, failed in turn (test_dom_storage.inc's sweeps). */
static void implementation_allocation_cases(void)
{
    static const char *const kOperations[] = {
        "document.implementation",
        "document.implementation.createHTMLDocument('t')",
        "document.implementation.createHTMLDocument()",
        "document.implementation.createDocument('http://www.w3.org/2000/svg','svg',"
        "document.implementation.createDocumentType('svg','p','s'))",
        "document.implementation.createHTMLDocument('').body.innerHTML='<form></form><form></form>'",
        "var d=document.implementation.createHTMLDocument('');document.body.appendChild(d.createElement('p'))",
        "var d=document.implementation.createHTMLDocument('');var p=d.createElement('p');d.body.appendChild(p);"
        "p.remove();p.ownerDocument;p.cloneNode(true)",
        "document.doctype.name",
    };
    binding_sweep("implementation", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), NULL);
    engine_sweep("implementation", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), NULL);
}

static void implementation_control_type_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, IMPL_ASSERT
        "var i=document.createElement('input');assert(i.type==='text','absent is text');"
        "i.checked=true;assert(i.checked===true,'a text input keeps its checkedness');i.checked=false;"
        "i.setAttribute('type','CheckBox');assert(i.type==='checkbox','its keyword in any case');"
        "i.setAttribute('type','nonsense');assert(i.type==='text','another word is text');"
        "i.type='RADIO';assert(i.getAttribute('type')==='RADIO'&&i.type==='radio','setting it sets the attribute');"
        "i.type='checkbox';i.checked=true;assert(i.checked===true,'a checkbox made by type is checkable');"
        "var b=document.createElement('button');assert(b.type==='submit');b.type='reset';assert(b.type==='reset');"
        "b.setAttribute('type','x');assert(b.type==='submit');"
        "var s=document.getElementById('select');assert(s.type==='select-one');s.setAttribute('multiple','');"
        "assert(s.type==='select-multiple');s.type='other';assert(s.type==='select-multiple'&&!s.hasAttribute('type'));"
        "var t=document.createElement('textarea');assert(t.type==='textarea');t.type='x';assert(t.type==='textarea');"
        "var g=Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'type').get;"
        "throws('TypeError',()=>g.call(document.body));assert(!('type' in document.body));",
        "a control's type: input, button, select and textarea");
    fixture_free(&f);
}

static void dom_implementation_cases(void)
{
    implementation_shape_cases();
    implementation_html_document_cases();
    implementation_owner_cases();
    implementation_windowless_cases();
    implementation_reflection_cases();
    implementation_connected_cases();
    implementation_document_cases();
    implementation_control_type_cases();
    implementation_arena_cases();
    implementation_jquery_cases();
}
#undef IMPL_ASSERT
