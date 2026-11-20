/* innerText and outerText through the binding (WHEEL_FILELIST_INNERTEXT.md
 * § 3): textContent with no provider, the walk's answer with one, and the
 * setters. The walk's own rules are tools/test_inner_text_host.sh's; these
 * prove the door to it. The fake provider reads an element's `data-t`
 * (n: display none, b: block, h: hidden) and gives div, p and body blocks
 * and head, script and style none, inherited down the tree as CSS does. */

#define INNER_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;assert(e.name===n,'wanted '+n+' got '+e.name)}assert(caught,'no '+n);}"

typedef struct {
    size_t calls;
    bool refuse;
} InnerFake;

static char inner_kind(const os64_html_node_t *n)
{
    const os64_html_attr_t *t = os64_html_attr(n, "data-t");
    if (t != NULL) return t->value[0];
    if (n->ns != OS64_HTML_NS_HTML || n->name == NULL) return 'i';
    static const char *const none[] = {"head", "script", "style", "title", "template"};
    static const char *const block[] = {"html", "body", "div", "p"};
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++) if (strcmp(n->name, none[i]) == 0) return 'n';
    for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); i++) if (strcmp(n->name, block[i]) == 0) return 'b';
    return 'i';
}

static bool inner_fake(void *opaque, const os64_html_node_t *element, os64_dom_text_style_t *out)
{
    InnerFake *f = opaque;
    f->calls++;
    if (f->refuse) return false;
    memset(out, 0, sizeof(*out));
    out->rendered = true;
    out->visible = true;
    for (const os64_html_node_t *n = element; n != NULL && n->kind == OS64_HTML_ELEMENT; n = n->parent) {
        char k = inner_kind(n);
        if (k == 'n') out->rendered = false;
        if (k == 'h') out->visible = false;
    }
    out->display = inner_kind(element) == 'b' ? OS64_DOM_TEXT_BLOCK : OS64_DOM_TEXT_INLINE;
    return true;
}

static void dom_inner_text_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, INNER_ASSERT
        "var host=document.createElement('div');document.body.append(host);globalThis.host=host;"
        "host.innerHTML='a<span>b</span><br><p>c</p><span data-t=n>gone</span><script>s()</script>';"
        "assert(host.innerText===host.textContent&&host.innerText==='abcgones()','no provider: textContent: '+host.innerText);"
        "assert(host.outerText===host.textContent,'outerText reads as innerText');"
        "var d=Object.getOwnPropertyDescriptor(HTMLElement.prototype,'innerText');"
        "assert(d&&d.get&&d.set&&d.configurable&&d.enumerable,'an accessor on HTMLElement.prototype');"
        "assert(Object.getOwnPropertyDescriptor(HTMLElement.prototype,'outerText').set,'outerText too');"
        "assert(!('innerText' in document.createElementNS('http://www.w3.org/2000/svg','svg')),'not an SVG element\\'s');"
        "throws('TypeError',()=>d.get.call(new Text('t')));throws('TypeError',()=>d.get.call({}));"
        "throws('TypeError',()=>d.get.call(document.createElementNS('http://www.w3.org/2000/svg','g')));",
        "innerText: textContent without a provider");
    InnerFake fake = {0};
    os64_dom_set_text_style(f.binding, inner_fake, &fake);
    dom_script(&f, INNER_ASSERT
        /* The br writes "\n" as a string and the p a count of 2: three line feeds. */
        "assert(host.innerText==='ab\\n\\n\\nc','with a provider, as rendered: '+JSON.stringify(host.innerText));"
        "host.innerHTML='x <b data-t=h>hid<i>den</i></b> y<div>z</div>';"
        "assert(host.innerText==='x y\\nz','hidden text left out, its spaces collapsed: '+JSON.stringify(host.innerText));"
        "var lone=document.createElement('div');lone.innerHTML='<p>p</p>q';"
        "assert(lone.innerText==='pq'&&lone.innerText===lone.textContent,'a detached element: textContent');"
        "var sc=document.createElement('script');sc.type='text/x-inert';sc.textContent='source()';host.append(sc);"
        "assert(sc.innerText==='source()','read ON a script, not rendered: its source');"
        "var none=document.createElement('div');none.dataset.t='n';none.textContent='t';host.append(none);"
        "assert(none.innerText==='t','an element not rendered: its textContent');",
        "innerText: the provider's facts");
    check(fake.calls > 0, "innerText: the provider is asked");
    fake.refuse = true;
    dom_script(&f, INNER_ASSERT
        "host.innerHTML='a<div>b</div>';assert(host.innerText==='ab','a provider with no answer: textContent');",
        "innerText: a refusing provider");
    fake.refuse = false;
    dom_script(&f, INNER_ASSERT
        "var o=new MutationObserver(()=>{});o.observe(host,{childList:true});"
        "host.innerText='one\\ntwo\\r\\nthree\\rfour';o.takeRecords();"
        "assert(host.innerHTML==='one<br>two<br>three<br>four','line breaks become br: '+host.innerHTML);"
        "host.innerText='a\\n\\nb';var r=o.takeRecords();assert(host.innerHTML==='a<br><br>b',host.innerHTML);"
        "host.innerHTML='1<b>2</b>3<b>4</b>5<b>6</b>7';o.takeRecords();host.textContent='z';var t=o.takeRecords();"
        /* The records textContent's replace all makes, whatever that is
         * (DEBTS.md: a replace all is several records, not one). */
        "assert(r.length===t.length&&r.length>0,'the records of a replace all: '+r.length+' and '+t.length);"
        "host.innerText='';assert(host.childNodes.length===0,'empty: no children');"
        "host.innerText=null;assert(host.childNodes.length===0,'null is the empty string');"
        "host.innerText=42;assert(host.innerHTML==='42','anything else is a string');"
        "host.innerText='<b>&amp;</b>';assert(host.firstChild.data==='<b>&amp;</b>'&&host.childNodes.length===1,'text, never markup');"
        "o.disconnect();",
        "innerText: the setter");
    dom_script(&f, INNER_ASSERT
        "host.innerHTML='x<span id=o>old</span>y';var sp=host.querySelector('#o');"
        "sp.outerText='new';"
        "assert(host.childNodes.length===1&&host.firstChild.data==='xnewy','merged with both neighbours: '+host.innerHTML);"
        "assert(sp.parentNode===null,'the element is gone');"
        "host.innerHTML='x<span>old</span>y';host.firstElementChild.outerText='1\\n2';"
        "assert(host.innerHTML==='x1<br>2y'&&host.childNodes.length===3,'a break between: '+host.innerHTML);"
        "host.innerHTML='<i>a</i><span>old</span><i>b</i>';host.children[1].outerText='';"
        "assert(host.childNodes.length===3&&host.childNodes[1].nodeType===3&&host.childNodes[1].data==='',"
        "'an empty value leaves an empty Text node');"
        "host.innerHTML='x<span>old</span>';host.lastChild.outerText='';"
        "assert(host.childNodes.length===1&&host.firstChild.data==='x','and merges it away beside text');"
        "throws('NoModificationAllowedError',()=>{document.createElement('b').outerText='x'});",
        "outerText: the setter");
    fixture_free(&f);
}

#undef INNER_ASSERT
