/* THE MODERN WEB'S SWITCH (libdom/modern.c, MODERN_WEB.md rule 4): with
 * options.modern clear, no name of the table's exists on window; set, each
 * row puts on window exactly the names it declares. Held against the table
 * itself, so a row a slice adds is held to its names with no case of its
 * own: a name installed outside the table is on window both ways, and one a
 * row installs without listing it is on window only one way, and either
 * fails here. */

#define MODERN_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}"

/* libdom's internal.h: the harness links the binding's own objects. */
const char *d_modern_name(size_t index);

static int modern_name_order(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* window's own string-named properties, sorted, malloc'd (each name and
 * the array); *n of them. */
static char **modern_global_names(DomFixture *f, size_t *n)
{
    *n = 0;
    os64_js_outcome_t outcome;
    JSContext *ctx = os64_js_context(f->runtime, OS64_JS_ABI_ID, &outcome);
    if (ctx == NULL) return NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSPropertyEnum *props = NULL;
    uint32_t count = 0;
    char **names = NULL;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, global, JS_GPN_STRING_MASK) == 0) {
        names = malloc(sizeof(*names) * (count ? count : 1));
        if (!names) abort();
        for (uint32_t i = 0; i < count; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            names[(*n)++] = strdup(name ? name : "");
            JS_FreeCString(ctx, name);
        }
        JS_FreePropertyEnum(ctx, props, count);
        qsort(names, *n, sizeof(*names), modern_name_order);
    }
    JS_FreeValue(ctx, global);
    return names;
}

static bool modern_has(char **names, size_t n, const char *name)
{
    return bsearch(&name, names, n, sizeof(*names), modern_name_order) != NULL;
}

static void modern_names_free(char **names, size_t n)
{
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* The shape of the binding's prototype chains, for nodes of every kind it
 * tells apart: for each, each level down to Object.prototype, as the names
 * the binding put there, leaving out what the table's rows add to them
 * (each prototype's `constructor` and Node's constants, the interface
 * row's; onscroll, onresize and the document's scrollingElement, the
 * view's; its defaultView, getComputedStyle's; the document's onerror, the
 * error row's; P1-Da's members of a document's prototype, which the
 * document row puts there and window never sees, left out of a document's
 * levels alone, since an element's hidden, dir and title are its own and
 * every page's; Symbol.toStringTag is a symbol, which no list of names
 * holds).
 * malloc'd; NULL when the script did not run. */
static char *modern_chain_shape(DomFixture *f)
{
    if (!dom_script(f, MODERN_ASSERT
        "var holder=document.createElement('div');holder.innerHTML='<svg><circle/></svg>';"
        "var nodes=[document,document.body,document.createElement('div'),document.createElement('input'),"
        "document.createElement('a'),document.createElement('script'),holder.firstChild,"
        "document.createTextNode('t'),document.createComment('c'),document.createDocumentFragment(),"
        "document.doctype,document.implementation,document.implementation.createHTMLDocument('')];"
        "var row=/(^constructor$|_NODE$|^DOCUMENT_POSITION_|^onscroll$|^onresize$|^scrollingElement$|^defaultView$"
        "|^onerror$|^getRootNode$|^wholeText$|^splitText$|^selectionStart$|^selectionEnd$|^selectionDirection$"
        "|^setSelectionRange$|^select$|^content$)/;"
        "var documentRow=/^(readyState|onreadystatechange|visibilityState|hidden|onvisibilitychange|referrer|"
        "activeElement|hasFocus|currentScript|title|prerendering|onprerenderingchange|domain|URL|documentURI|"
        "wasDiscarded|compatMode|dir|characterSet|charset|inputEncoding|contentType|lastModified|rootElement|"
        "clear|getSelection|createElementNS)$/;"
        "globalThis.chainShape=nodes.map(function(n){var levels=[];"
        "for(var p=Object.getPrototypeOf(n);p&&p!==Object.prototype;p=Object.getPrototypeOf(p))"
        "levels.push(Object.getOwnPropertyNames(p).filter(function(k){"
        "return !row.test(k)&&!(n.nodeType===9&&documentRow.test(k))}).sort().join(','));"
        "return levels.join(' / ')}).join('\\n');", "modern-chain-shape"))
        return NULL;
    os64_js_outcome_t outcome;
    JSContext *ctx = os64_js_context(f->runtime, OS64_JS_ABI_ID, &outcome);
    if (ctx == NULL) return NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue shape = JS_GetPropertyStr(ctx, global, "chainShape");
    const char *text = JS_ToCString(ctx, shape);
    char *copy = text != NULL ? strdup(text) : NULL;
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, shape);
    JS_FreeValue(ctx, global);
    return copy;
}

static void dom_modern_cases(void)
{
    check(os64_dom_default_options().modern, "modern: the switch is on unless a host turns it off");
    size_t rows = 0;
    while (d_modern_name(rows) != NULL) rows++;
    check(rows > 0, "modern: the table has names");

    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    size_t non = 0;
    char **on = modern_global_names(&f, &non);
    dom_script(&f, MODERN_ASSERT
        "var s=document.body.style;assert('animationName' in s);"
        /* slice 4's names: longhands, shorthands, aliases, flow-relative names */
        "assert('aspectRatio' in s&&'outline' in s&&'outline-offset' in s&&'wordWrap' in s);"
        "assert('marginInlineStart' in s&&'margin-inline' in s&&'borderBlockEnd' in s);"
        "s.marginInlineStart='3px';assert(s.getPropertyValue('margin-inline-start')==='3px'&&s.marginLeft==='');"
        "s.setProperty('animation-name','spin');assert(s.getPropertyValue('animation-name')==='spin');",
        "modern-on-style-member");
    dom_script(&f, MODERN_ASSERT
        "function tag(o){return Object.prototype.toString.call(o)}"
        "assert(tag(document.body)==='[object HTMLElement]'&&tag(document)==='[object Document]');"
        "assert(tag(document.createElement('input'))==='[object HTMLInputElement]');"
        "assert(tag(document.createTextNode('t'))==='[object Text]'&&tag(document.createComment('c'))==='[object Comment]');"
        "var holder=document.createElement('div');holder.innerHTML='<svg></svg>';"
        "assert(tag(holder.firstChild)==='[object SVGElement]');"
        "assert(tag(document.createElement('option'))==='[object HTMLOptionElement]');"
        "assert(tag(new DocumentFragment())==='[object DocumentFragment]');"
        "var d=Object.getOwnPropertyDescriptor(HTMLElement.prototype,Symbol.toStringTag);"
        "assert(d.value==='HTMLElement'&&d.configurable&&!d.writable&&!d.enumerable);"
        "assert(!Object.prototype.hasOwnProperty.call(HTMLElement,Symbol.toStringTag));",
        "modern-on-string-tag");
    /* 0f's P1a node row: getRootNode (Vue's v-model), Text's wholeText and
     * splitText. */
    dom_script(&f, MODERN_ASSERT
        "var p=document.createElement('p'),t=document.createTextNode('abc');p.appendChild(t);"
        "assert(t.getRootNode()===p&&p.getRootNode()===p&&document.body.getRootNode()===document);"
        "document.body.appendChild(p);assert(t.getRootNode({composed:true})===document);"
        "var asked=0;t.getRootNode({get composed(){asked++;return false}});assert(asked===1,'the options are converted');"
        "var bad=false;try{t.getRootNode(5)}catch(e){bad=e instanceof TypeError}assert(bad,'options must be an object');"
        "var at=document.createAttribute('x');assert(at.getRootNode()===at,'an Attr is its own root');"
        "assert(Object.prototype.hasOwnProperty.call(Attr.prototype,'getRootNode'));"
        "var f=new DocumentFragment(),ft=new Text('z');f.appendChild(ft);assert(ft.getRootNode()===f);"
        "p.appendChild(new Text('def'));p.appendChild(document.createElement('b'));p.appendChild(new Text('g'));"
        "assert(t.wholeText==='abcdef'&&p.childNodes[1].wholeText==='abcdef'&&p.lastChild.wholeText==='g');"
        "var tail=t.splitText(1);"
        "assert(t.data==='a'&&tail.data==='bc'&&t.nextSibling===tail&&tail.parentNode===p&&tail instanceof Text);"
        "assert(tail.ownerDocument===document&&p.textContent==='abcdefg');"
        "var lone=new Text('xy'),rest=lone.splitText(2);assert(lone.data==='xy'&&rest.data===''&&rest.parentNode===null);"
        "bad=false;try{lone.splitText(3)}catch(e){bad=e.name==='IndexSizeError'}assert(bad,'past the end');"
        "var wide=new Text('a\\u{1F600}bc');assert(wide.splitText(4).data==='c'&&wide.data==='a\\u{1F600}b','a pair is two code units');"
        "var acute=new Text('\\u00e9xb');assert(acute.splitText(2).data==='b'&&acute.data==='\\u00e9x','two bytes are one code unit');"
        "bad=false;try{Text.prototype.splitText.call(document.createComment('c'),0)}catch(e){bad=e instanceof TypeError}"
        "assert(bad,'a Text member on a comment');",
        "modern-on-node-row");
    /* 0f's P1b lists: NodeList apart from HTMLCollection, and the names of
     * the token and string lists (ES5 TodoMVC patches NodeList.prototype). */
    dom_script(&f, MODERN_ASSERT
        "var b=document.body,box=document.createElement('div');b.appendChild(box);"
        "box.innerHTML='<p class=k name=n>1</p><p class=k name=n>2</p>';"
        "var cn=box.childNodes,qs=box.querySelectorAll('p'),byName=document.getElementsByName('n');"
        "assert(cn instanceof NodeList&&!(cn instanceof HTMLCollection)&&qs instanceof NodeList&&byName instanceof NodeList,'NodeLists');"
        "assert(box.children instanceof HTMLCollection&&box.getElementsByClassName('k') instanceof HTMLCollection&&"
        "document.forms instanceof HTMLCollection&&!(box.children instanceof NodeList),'HTMLCollections');"
        "assert(!('namedItem' in cn)&&'namedItem' in box.children&&cn.length===2&&cn.item(1)===box.lastChild&&qs.item(0)===box.firstChild);"
        "assert(Object.getPrototypeOf(cn)===NodeList.prototype&&Object.getPrototypeOf(qs)===NodeList.prototype);"
        "var seen=[];NodeList.prototype.forEach=function(f){for(var i=0;i<this.length;i++)f(this[i],i);seen.push('patched')};"
        "qs.forEach(function(){});cn.forEach(function(){});assert(seen.join()==='patched,patched','a patch reaches both kinds');"
        "NodeList.prototype.forEach=Array.prototype.forEach;"
        "assert(Object.prototype.toString.call(qs)==='[object NodeList]'&&Object.prototype.toString.call(box.children)==='[object HTMLCollection]');"
        "var bad=false;try{new NodeList()}catch(e){bad=e instanceof TypeError}assert(bad,'no constructor');"
        "assert(b.classList instanceof DOMTokenList&&b.dataset instanceof DOMStringMap&&Object.getPrototypeOf(b.dataset)===DOMStringMap.prototype);"
        "assert(typeof b.dataset.hasOwnProperty==='function','a dataset inherits Object.prototype');"
        "var sel=document.createElement('select');"
        "assert(sel.options instanceof HTMLOptionsCollection&&sel.options instanceof HTMLCollection&&"
        "Object.getPrototypeOf(HTMLOptionsCollection)===HTMLCollection);",
        "modern-on-lists");
    /* 0f's P2 core: a text control's selection, and the window's Selection
     * (react-dom's two paths: the field's, and getSelection's) */
    dom_script(&f, MODERN_ASSERT
        "var t=document.createElement('input');t.value='hello';"
        "assert('selectionStart' in t&&t.selectionStart===5&&t.selectionEnd===5&&t.selectionDirection==='none','at the value end');"
        "t.setSelectionRange(1,3);assert(t.selectionStart===1&&t.selectionEnd===3&&t.selectionDirection==='none');"
        "t.setSelectionRange(1,3,'backward');assert(t.selectionDirection==='backward');"
        "t.selectionStart=4;assert(t.selectionStart===4&&t.selectionEnd===4,'a start past the end moves the end');"
        "t.selectionEnd=2;assert(t.selectionStart===2&&t.selectionEnd===2,'an end before the start moves the start');"
        "t.select();assert(t.selectionStart===0&&t.selectionEnd===5,'select() is all of it');"
        "t.setSelectionRange(0,99);assert(t.selectionEnd===5,'the end is clamped to the value');"
        "t.setSelectionRange(1,2);t.value='hi';assert(t.selectionStart===2&&t.selectionEnd===2,'a new value puts it at the end');"
        "var w=new Text('x');var u=document.createElement('input');u.value='a\\u{1F600}';assert(u.selectionEnd===3,'code units');"
        "var e=document.createElement('input');e.type='email';e.value='a@b';"
        "assert('selectionStart' in e&&e.selectionStart===null&&e.selectionDirection===null,'an email input has none');"
        "var bad=false;try{e.selectionStart=1}catch(x){bad=x.name==='InvalidStateError'}assert(bad,'and refuses one');"
        "bad=false;try{e.setSelectionRange(0,1)}catch(x){bad=x.name==='InvalidStateError'}assert(bad);e.select();"
        "var a=document.createElement('textarea');a.value='two\\nlines';a.setSelectionRange(4,9);"
        "assert(a.selectionStart===4&&a.selectionEnd===9,'a textarea has one');"
        "var s=window.getSelection();"
        "assert(s===document.getSelection()&&s instanceof Selection&&s.anchorNode===null&&s.focusNode===null&&"
        "s.anchorOffset===0&&s.rangeCount===0&&s.isCollapsed&&s.type==='None'&&String(s)==='','no page selection');"
        "bad=false;try{s.getRangeAt(0)}catch(x){bad=x.name==='IndexSizeError'}assert(bad);s.removeAllRanges();",
        "modern-on-selection");
    /* DOMParser, HTML only, as ES5 TodoMVC renders its list */
    dom_script(&f, MODERN_ASSERT
        "var p=new DOMParser();assert(p instanceof DOMParser&&Object.prototype.toString.call(p)==='[object DOMParser]');"
        "var d=p.parseFromString('<title>T</title><li data-id=1>one</li><li data-id=2>two</li><script>globalThis.ran=1<\\/script>','text/html');"
        "assert(d.nodeType===9&&d!==document&&d.head.firstChild.textContent==='T'&&d.body.children.length===3,'head and body as a document parse puts them');"
        "assert(typeof globalThis.ran==='undefined','its script never runs');"
        "var ul=document.createElement('ul');document.body.appendChild(ul);ul.appendChild(document.createElement('i'));"
        "ul.replaceChildren(...p.parseFromString('<li>a</li><li>b</li>','text/html').body.childNodes);"
        "assert(ul.children.length===2&&ul.lastChild.textContent==='b'&&ul.lastChild.ownerDocument===document,'ES5 TodoMVC renders so');"
        "var bad=false;try{p.parseFromString('<a/>','text/xml')}catch(e){bad=e.name==='NotSupportedError'}assert(bad,'XML is not parsed');"
        "bad=false;try{p.parseFromString('x','text/plain')}catch(e){bad=e instanceof TypeError}assert(bad,'not a parser type');"
        "bad=false;try{DOMParser()}catch(e){bad=e instanceof TypeError}assert(bad,'a constructor only');"
        "bad=false;try{DOMParser.prototype.parseFromString.call({},'x','text/html')}catch(e){bad=e instanceof TypeError}assert(bad,'its own');",
        "modern-on-dom-parser");
    /* HTMLElement's members are HTMLElement's: Svelte 5 looks for a member's
     * setter below Element.prototype and sets an attribute when it finds
     * none, so `hidden = false` on Element's became hidden="false" and hid
     * TodoMVC's list. Its walk, as it runs. */
    dom_script(&f, MODERN_ASSERT
        "function setters(el){var s=[],p=el;while(p!==Element.prototype){var d=Object.getOwnPropertyDescriptors(p);"
        "for(var k in d)if(d[k].set)s.push(k);p=Object.getPrototypeOf(p)}return s}"
        "var s=setters(document.createElement('main'));"
        "assert(['hidden','lang','dir','title','tabIndex'].every(k=>s.includes(k)),'Svelte finds them: '+s.join());"
        "var g=setters(document.createElementNS('http://www.w3.org/2000/svg','g'));"
        "assert(g.includes('tabIndex')&&!g.includes('hidden'),'an SVG element has tabIndex and not hidden');"
        "assert(!Object.getOwnPropertyDescriptor(Element.prototype,'hidden')&&'dataset' in document.createElementNS('http://www.w3.org/2000/svg','g'));"
        "var m=document.createElement('main');m.hidden=true;m.hidden=false;assert(!m.hasAttribute('hidden'),'false removes it');",
        "modern-on-html-element-members");
    /* A template's content, as Svelte builds its markup */
    dom_script(&f, MODERN_ASSERT
        "var t=document.createElement('template');assert(t instanceof HTMLTemplateElement&&t instanceof HTMLElement);"
        "t.innerHTML='<p>x</p><!---->';var c=t.content;"
        "assert(c instanceof DocumentFragment&&c===t.content&&c.firstChild.tagName==='P'&&t.childNodes.length===0,'its own fragment');"
        "var copy=c.cloneNode(true);assert(copy.childNodes.length===2&&copy.firstChild!==c.firstChild,'cloned whole');"
        "var h=document.createElement('div');h.innerHTML='<template id=tp><b>y</b></template>';"
        "assert(h.firstChild.content.firstChild.tagName==='B','a parsed one too');"
        "var bad=false;try{Object.getOwnPropertyDescriptor(HTMLTemplateElement.prototype,'content').get.call(h)}"
        "catch(e){bad=e instanceof TypeError}assert(bad,'a template only');",
        "modern-on-template-content");
    /* createElementNS: React's SVG and Svelte's every element */
    dom_script(&f, MODERN_ASSERT
        "var SVG='http://www.w3.org/2000/svg',X='http://www.w3.org/1999/xhtml';"
        "var c=document.createElementNS(SVG,'circle');"
        "assert(c instanceof SVGElement&&c.tagName==='circle'&&c.ownerDocument===document&&c.parentNode===null,'an SVG element');"
        "var g=document.createElementNS(SVG,'linearGradient');assert(g.tagName==='linearGradient','its case kept');"
        "var d=document.createElementNS(X,'div',undefined);assert(d instanceof HTMLElement&&d.tagName==='DIV','Svelte: an HTML one');"
        "var asked=0;document.createElementNS(X,'p',{get is(){asked++;return 'x-p'}});assert(asked===1,'options converted');"
        "var svg=document.createElementNS(SVG,'svg');svg.appendChild(c);document.body.appendChild(svg);"
        "assert(document.body.lastChild===svg&&svg.firstChild===c);"
        "function name(f){try{f();return 'none'}catch(e){return e.name}}"
        "assert(name(()=>document.createElementNS(SVG,'a:'))==='InvalidCharacterError','a bad name');"
        "assert(name(()=>document.createElementNS(null,'p:a'))==='NamespaceError','a prefix with no namespace');"
        "assert(name(()=>document.createElementNS(null,'a'))==='NotSupportedError','no namespace the tree holds');"
        "assert(name(()=>document.createElementNS(SVG))==='TypeError','two arguments');",
        "modern-on-create-element-ns");
    /* 0f's P1a select row: React's option updater first, as it runs. */
    dom_script(&f, MODERN_ASSERT
        "var h=document.createElement('div');document.body.appendChild(h);"
        "h.innerHTML='<form id=fm><select id=sort><option value=az>Name (A to Z)<option value=za>Name (Z to A)'+"
        "'<optgroup label=price><option value=lohi>Price (low to high)<option value=hilo disabled>Price (high to low)'+"
        "'</optgroup></select></form>';"
        "var s=document.getElementById('sort'),o=s.options;"
        "assert(o===s.options&&o.length===4&&s.length===4&&o[0].value==='az'&&o[2].value==='lohi','the list, optgroup included');"
        "assert(Object.prototype.toString.call(o)==='[object HTMLOptionsCollection]'&&o.item(1)===o[1]);"
        /* react-dom's updateOptions, single select: set the matching one, else the first enabled */
        "function update(node,value){var e=node.options,d=null;for(var n=0;n<e.length;n++){if(e[n].value===value){"
        "e[n].selected=true;return}d!==null||e[n].disabled||(d=e[n])}d!==null&&(d.selected=true)}"
        "update(s,'lohi');assert(s.selectedIndex===2&&o[2].selected&&!o[0].selected&&s.value==='lohi','React selects by value');"
        "update(s,'nothing');assert(s.selectedIndex===0&&o[0].selected,'and falls back to the first enabled');"
        "o[0].selected=false;assert(s.selectedIndex===0,'deselected, a one-line select resets to its first enabled');"
        "s.selectedIndex=-1;assert(s.selectedIndex===-1&&!o[0].selected&&o.selectedIndex===-1);"
        "o.selectedIndex=1;assert(s.value==='za');"
        "assert(o[3].disabled&&o[3].index===3&&o[1].index===1&&o[1].form===document.getElementById('fm'));"
        "var g=s.querySelector('optgroup');assert(g.label==='price'&&!g.disabled);g.disabled=true;assert(g.hasAttribute('disabled'));"
        /* text, value and label */
        "var t=document.createElement('option');t.innerHTML='  two \\n words <script>skip()<\\/script>here ';"
        "assert(t.text==='two words here'&&t.value==='two words here'&&t.label==='two words here'&&t.index===0&&t.form===null);"
        "t.label='L';t.value='V';assert(t.label==='L'&&t.value==='V'&&t.getAttribute('value')==='V');"
        "t.text='new';assert(t.textContent==='new'&&t.childNodes.length===1);"
        "t.defaultSelected=true;assert(t.hasAttribute('selected'));"
        /* the Option factory */
        "var n=new Option('Pink','pk',false,true);"
        "assert(n instanceof HTMLOptionElement&&n.text==='Pink'&&n.value==='pk'&&n.selected&&!n.defaultSelected);"
        "assert(Option.prototype===HTMLOptionElement.prototype&&HTMLOptionElement.prototype.constructor===HTMLOptionElement);"
        "assert(new Option().text===''&&new Option().childNodes.length===0&&Option.name==='Option'&&Option.length===0);"
        "var bad=false;try{Option('x')}catch(e){bad=e instanceof TypeError}assert(bad,'a constructor only');"
        /* add, remove, length: the old idioms */
        "var c=document.createElement('select');c.add(new Option('a'));c.add(new Option('c'));c.add(new Option('b'),1);"
        "assert([...c.options].map(x=>x.text).join()==='a,b,c','add before an index');"
        "c.add(new Option('z'),c.options[0]);assert(c.options[0].text==='z','add before an element');"
        "c.options.add(new Option('end'),null);assert(c.options[4].text==='end');"
        "bad=false;try{c.add(document.createElement('div'))}catch(e){bad=e instanceof TypeError}assert(bad,'only options and optgroups');"
        "bad=false;try{c.add(new Option('q'),document.body)}catch(e){bad=e.name==='NotFoundError'}assert(bad,'before must be in it');"
        "c.remove(0);assert(c.options[0].text==='a'&&c.length===4,'remove(index)');c.options.remove(9);assert(c.length===4);"
        "c.options.length=2;assert(c.length===2&&c.options[1].text==='b','length shortens');"
        "c.length=4;assert(c.options[3].text===''&&c.options[3].parentNode===c,'and lengthens with empty options');"
        "document.body.appendChild(c);c.remove();assert(c.parentNode===null&&c.length===4,'remove() is ChildNode remove');"
        /* the indexed setter: options[options.length] = new Option(...), the 2000s idiom */
        "var q=document.createElement('select');q.options[q.options.length]=new Option('one','1');"
        "q.options[2]=new Option('three');assert(q.length===3&&q.options[1].text===''&&q.options[2].text==='three','appends past the end');"
        "q.options[0]=new Option('uno');assert(q.options[0].text==='uno'&&q.length===3,'replaces');"
        "q.options[1]=null;assert(q.length===2&&q.options[1].text==='three','null removes');"
        "bad=false;try{q.options[0]=document.createElement('div')}catch(e){bad=e instanceof TypeError}assert(bad,'only an option');"
        "var dsc=Object.getOwnPropertyDescriptor(q.options,0);assert(dsc.writable&&dsc.enumerable&&dsc.value===q.options[0]);"
        "assert(Object.defineProperty(q.options,'5',{value:new Option('six')})&&q.length===6,'a data definition sets');"
        /* selectedOptions, live with control state */
        "var m=document.createElement('select');m.multiple=true;m.innerHTML='<option>1<option>2<option>3';"
        "var so=m.selectedOptions;assert(so===m.selectedOptions&&so.length===0);"
        "m.options[0].selected=true;m.options[2].selected=true;assert(so.length===2&&so[1]===m.options[2]);"
        "m.options[0].selected=false;assert(so.length===1&&so[0].text==='3');"
        "assert(m.size===0);m.size=3;assert(m.getAttribute('size')==='3'&&m.size===3);",
        "modern-on-select-row");
    check(on != NULL, "modern: window's names with the switch on");
    char *chain_on = modern_chain_shape(&f);
    fixture_free(&f);

    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) { modern_names_free(on, non); free(chain_on); return; }
    size_t noff = 0;
    char **off = modern_global_names(&f, &noff);
    check(off != NULL, "modern: window's names with the switch off");
    dom_script(&f, MODERN_ASSERT
        "assert(typeof Element==='undefined'&&!('Element' in window)&&!self.hasOwnProperty('HTMLElement'));"
        "var r;try{Node}catch(e){r=e.name}assert(r==='ReferenceError');"
        "var b=document.body;assert(b.tagName==='BODY'&&b.constructor===Object);"
        "assert(Object.getPrototypeOf(b)!==Object.prototype&&typeof b.getAttribute==='function');"
        "assert(b.ELEMENT_NODE===undefined&&!('ELEMENT_NODE' in b));"
        "assert(Object.prototype.toString.call(b)==='[object Object]'&&"
        "Object.getOwnPropertySymbols(Object.getPrototypeOf(b)).indexOf(Symbol.toStringTag)<0);"
        /* tools/test_dom_host.sh marks animation-name as the arc's */
        "assert(!('animationName' in b.style)&&!('animation-name' in b.style)&&'color' in b.style);"
        /* the shorthand is still read, for the longhands that are not marked */
        "assert('animation' in b.style);"
        "assert(!('aspectRatio' in b.style)&&!('outline' in b.style)&&!('wordWrap' in b.style)&&"
        "!('marginInlineStart' in b.style)&&!('margin-inline' in b.style)&&!('cursor' in b.style));"
        "b.style.setProperty('margin-inline-start','3px');assert(b.style.getPropertyValue('margin-inline-start')==='');"
        "b.style.setProperty('animation-name','spin');b.style.color='red';"
        "assert(b.style.getPropertyValue('animation-name')===''&&b.style.color==='red');"
        "assert(typeof Image==='function'&&new Image().tagName==='IMG');"
        /* the frames row's: an iframe has HTMLElement's members alone */
        "var fr=document.createElement('iframe');fr.src='/frame.html';"
        "assert(fr.getAttribute('src')===null&&fr.hasOwnProperty('src')&&!('HTMLIFrameElement' in window));",
        "modern-off-absent");

    bool exact = on != NULL && off != NULL;
    for (size_t i = 0; exact && i < noff; i++)
        if (!modern_has(on, non, off[i])) {
            fprintf(stderr, "modern: %s is on window only with the switch off\n", off[i]);
            exact = false;
        }
    for (size_t i = 0; exact && i < non; i++) {
        bool listed = false;
        for (size_t k = 0; k < rows && !listed; k++) listed = !strcmp(d_modern_name(k), on[i]);
        if (listed == modern_has(off, noff, on[i])) {
            fprintf(stderr, "modern: %s is %s\n", on[i], listed ? "on window with the switch off" :
                    "on window only with the switch on, and in no row of the table");
            exact = false;
        }
    }
    for (size_t k = 0; exact && k < rows; k++)
        if (!modern_has(on, non, d_modern_name(k))) {
            fprintf(stderr, "modern: the table lists %s, which is not on window\n", d_modern_name(k));
            exact = false;
        }
    check(exact, "modern: the switch adds exactly the table's names to window, and nothing else");
    /* The switch takes away the table's names, not the binding's own
     * structure: the prototype chains have the same levels, holding the
     * same members either way but the ones the rows put on them (the
     * shape's `row` and `documentRow` patterns, which name each). */
    char *chain_off = modern_chain_shape(&f);
    check(chain_on != NULL && chain_off != NULL && strcmp(chain_on, chain_off) == 0 &&
          strstr(chain_on, " / ") != NULL,
          "modern: the binding's prototype chains are the same with the switch on and off");
    free(chain_on);
    free(chain_off);
    modern_names_free(on, non);
    modern_names_free(off, noff);
    fixture_free(&f);
}
#undef MODERN_ASSERT
