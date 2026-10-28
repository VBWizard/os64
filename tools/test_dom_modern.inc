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
 * the binding put there, leaving out what the table's rows add to them:
 * the `row` pattern names those, and `documentRow` the members the
 * document row puts on a document's prototype, left out of a document's
 * levels alone, since an element's hidden, dir and title are its own and
 * every page's (Symbol.toStringTag is a symbol, which no list of names
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
        "var row=/(^constructor$|_NODE$|^DOCUMENT_POSITION_|^onscroll$|^onresize$|^oninvalid$|^scrollingElement$"
        "|^defaultView$|^(willValidate|validity|validationMessage|checkValidity|reportValidity|setCustomValidity)$"
        "|^(valueAsNumber|valueAsDate|stepUp|stepDown)$"
        "|^(text|link|vLink|aLink|bgColor|background|onpopstate|onhashchange|ononline|onoffline|onlanguagechange"
        "|onmessage|onmessageerror|onunhandledrejection|onrejectionhandled)$"
        "|^onerror$|^getRootNode$|^wholeText$|^splitText$|^selectionStart$|^selectionEnd$|^selectionDirection$"
        "|^setSelectionRange$|^select$|^setRangeText$|^content$|^styleSheets$"
        "|^(accept|alt|autocomplete|defaultChecked|defaultValue|dirName|disabled|formAction|formEnctype|formMethod"
        "|formNoValidate|formTarget|height|indeterminate|max|maxLength|min|minLength|multiple|pattern|placeholder"
        "|readOnly|required|size|src|step|width|align|useMap|form|labels|localName|namespaceURI|prefix|baseURI)$)/;"
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
        "assert(tag(document.body)==='[object HTMLBodyElement]'&&tag(document)==='[object Document]');"
        "assert(tag(document.createElement('section'))==='[object HTMLElement]');"
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
        "var pw=document.createElement('input');pw.type='password';pw.value='abc';"
        "assert(pw.selectionStart===3,'a password input has one');"
        "var s=window.getSelection();"
        "assert(s===document.getSelection()&&s instanceof Selection&&s.anchorNode===null&&s.focusNode===null&&"
        "s.anchorOffset===0&&s.rangeCount===0&&s.isCollapsed&&s.type==='None'&&String(s)==='','no page selection');"
        "bad=false;try{s.getRangeAt(0)}catch(x){bad=x.name==='IndexSizeError'}assert(bad);s.removeAllRanges();",
        "modern-on-selection");
    /* 0f's P2: setRangeText, each mode, and where a selection begins. */
    dom_script(&f, MODERN_ASSERT
        "var fresh=document.createElement('input');assert(fresh.selectionStart===0&&fresh.selectionEnd===0,'a new control is at 0');"
        "var t=document.createElement('input');t.value='hello world';"
        "t.setSelectionRange(6,11);t.setRangeText('there');"
        "assert(t.value==='hello there'&&t.selectionStart===6&&t.selectionEnd===11,'one argument replaces the selection, preserved');"
        "t.setRangeText('X',0,5,'select');assert(t.value==='X there'&&t.selectionStart===0&&t.selectionEnd===1,'select');"
        "t.setRangeText('Y',1,1,'start');assert(t.value==='XY there'&&t.selectionStart===1&&t.selectionEnd===1,'start');"
        "t.setRangeText('ZZ',0,0,'end');assert(t.value==='ZZXY there'&&t.selectionStart===2&&t.selectionEnd===2,'end');"
        "t.setSelectionRange(5,8);t.setRangeText('--',0,2);"
        "assert(t.value==='--XY there'&&t.selectionStart===5&&t.selectionEnd===8,'a selection past the text moves with it');"
        "t.setSelectionRange(1,9);t.setRangeText('abc',0,4,'preserve');"
        "assert(t.value==='abc there'&&t.selectionStart===0&&t.selectionEnd===8,'one around it snaps and moves');"
        "t.setSelectionRange(2,3);t.setRangeText('xyz',1,4);"
        "assert(t.value==='axyzthere'&&t.selectionStart===1&&t.selectionEnd===4,'one inside it snaps to the new text');"
        "t.setRangeText('!',99,99);assert(t.value==='axyzthere!','a start past the end is the end');"
        "var bad=false;try{t.setRangeText('a',3,1)}catch(x){bad=x.name==='IndexSizeError'}assert(bad,'start past end');"
        "bad=false;try{t.setRangeText('a',1)}catch(x){bad=x instanceof TypeError}assert(bad,'two arguments');"
        "bad=false;try{t.setRangeText('a',1,2,'sideways')}catch(x){bad=x instanceof TypeError}assert(bad,'a mode');"
        "var o=document.createElement('input');o.value='abcd';var n=0;"
        "o.setRangeText({toString(){n=2;return 'X'}},{valueOf(){return n}},2);"
        "assert(o.value==='abXcd','the arguments converted left to right');"
        "var u=document.createElement('input');u.value='a\\u{1F600}b';u.setRangeText('c',1,3);"
        "assert(u.value==='acb','code units');"
        "var e=document.createElement('input');e.type='email';"
        "bad=false;try{e.setRangeText('a')}catch(x){bad=x.name==='InvalidStateError'}assert(bad,'an email input has none');"
        "var a=document.createElement('textarea');a.textContent='abc';a.setRangeText('Z',0,1);"
        "assert(a.value==='Zbc'&&a.textContent==='abc','a textarea, its children untouched');"
        "a.textContent='new';assert(a.value==='Zbc','and its value dirty: a new default does not reach it');",
        "modern-on-range-text");
    /* The node row: an element's names. */
    dom_script(&f, MODERN_ASSERT
        "var d=document.createElement('DIV');"
        "assert(d.localName==='div'&&d.namespaceURI==='http://www.w3.org/1999/xhtml'&&d.prefix===null,'html');"
        "var h=document.createElement('div');h.innerHTML='<svg><circle/></svg>';var c=h.firstChild.firstChild;"
        "assert(c.localName==='circle'&&c.namespaceURI==='http://www.w3.org/2000/svg','svg');"
        "h.innerHTML='<math><mi>x</mi></math>';var m=h.firstChild.firstChild;"
        "assert(m.localName==='mi'&&m.namespaceURI==='http://www.w3.org/1998/Math/MathML'&&m.prefix===null,'mathml');"
        "var s=document.createElementNS('http://www.w3.org/2000/svg','rect');"
        "assert(s.localName==='rect'&&s.namespaceURI==='http://www.w3.org/2000/svg','made in a namespace');"
        "var bad=false;try{Object.getOwnPropertyDescriptor(Element.prototype,'localName').get.call(document)}"
        "catch(x){bad=x instanceof TypeError}assert(bad,'an element only');",
        "modern-on-element-names");
    /* 0f's P3a: the controls' reflections, by each of HTML's rules, and the
     * members that are no plain reflection. */
    dom_script(&f, MODERN_ASSERT
        "var i=document.createElement('input'),b=document.createElement('button'),"
        "t=document.createElement('textarea'),s=document.createElement('select'),bad;"
        /* a string, a boolean, and a boolean whose attribute is another name */
        "assert(i.placeholder===''&&!i.required&&!i.readOnly,'defaults');"
        "i.placeholder=null;assert(i.getAttribute('placeholder')==='null','a string converts null');"
        "i.readOnly=1;assert(i.getAttribute('readonly')===''&&i.readOnly,'on');i.readOnly=0;"
        "assert(!i.hasAttribute('readonly'),'off');"
        "i.setAttribute('checked','');assert(i.defaultChecked&&i.checked,'defaultChecked reads checked');"
        "i.defaultValue='dv';assert(i.getAttribute('value')==='dv'&&i.value==='dv','defaultValue is the value attribute');"
        "assert(s.disabled===false&&(s.disabled=true,s.hasAttribute('disabled'))&&b.disabled===false,'every control');"
        /* enumerated */
        "assert(i.formMethod===''&&i.formEnctype==='','missing is empty');"
        "i.setAttribute('formmethod','POST');assert(i.formMethod==='post','canonical');"
        "i.setAttribute('formmethod','Dialog');assert(i.formMethod==='dialog','an input has dialog too');"
        "i.setAttribute('formmethod','put');assert(i.formMethod==='get','an unknown one is get');"
        "b.setAttribute('formmethod','DiAlOg');assert(b.formMethod==='dialog','a button has');"
        "i.formEnctype='nonsense';assert(i.getAttribute('formenctype')==='nonsense'&&"
        "i.formEnctype==='application/x-www-form-urlencoded','invalid');"
        "i.setAttribute('formenctype','text/PLAIN');assert(i.formEnctype==='text/plain');"
        /* URLs, and formAction's document URL */
        "assert(i.formAction===document.URL&&i.src==='','missing');"
        "i.setAttribute('formaction','');assert(i.formAction===document.URL,'empty');"
        "i.formAction='http://h.test/a/../go?x';assert(i.formAction==='http://h.test/go?x','parsed');"
        "i.src='/go';assert(i.src==='/go','as written where it does not resolve (about:blank)');"
        /* limited long */
        "assert(i.maxLength===-1&&t.minLength===-1,'default -1');"
        "i.setAttribute('maxlength',' 7x');assert(i.maxLength===7,'leading space, trailing junk');"
        "i.setAttribute('maxlength','-1');assert(i.maxLength===-1,'negative');"
        "i.setAttribute('maxlength','-0');assert(i.maxLength===0,'minus zero');"
        "i.setAttribute('maxlength','2147483648');assert(i.maxLength===-1,'past the range');"
        "i.setAttribute('maxlength','\\u000B7');assert(i.maxLength===-1,'a vertical tab is no space');"
        "bad=false;try{i.maxLength=-2}catch(x){bad=x.name==='IndexSizeError'}assert(bad,'a negative set throws');"
        "i.maxLength=12;assert(i.getAttribute('maxlength')==='12');"
        /* limited unsigned long, and the one with fallback */
        "assert(i.size===20,'size 20');i.setAttribute('size','0');assert(i.size===20,'zero is out of range');"
        "bad=false;try{i.size=0}catch(x){bad=x.name==='IndexSizeError'}assert(bad,'zero throws');"
        "i.size=2147483648;assert(i.getAttribute('size')==='20','past the range sets the default');"
        "assert(t.cols===20&&t.rows===2,'cols and rows');t.cols=0;assert(t.getAttribute('cols')==='20','fallback, no throw');"
        "t.rows=5;assert(t.rows===5);"
        /* a member of one interface is not another's */
        "assert(!('formAction' in t)&&!('cols' in i)&&!('maxLength' in s)&&'required' in s&&!('required' in b));"
        "bad=false;try{Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'accept').get.call(t)}"
        "catch(x){bad=x instanceof TypeError}assert(bad,'the brand is checked');"
        /* textarea's defaultValue and textLength */
        "t.appendChild(document.createTextNode('ab'));t.appendChild(document.createElement('b')).textContent='zz';"
        "t.appendChild(document.createTextNode('c'));"
        "assert(t.defaultValue==='abc','child text, not descendants');"
        "t.defaultValue='\\u{1F600}x';assert(t.childNodes.length===1&&t.value==='\\u{1F600}x'&&t.textLength===3,"
        "'replaced; textLength counts code units');"
        "t.defaultValue=null;assert(t.textContent==='null','DOMString, not nullable');"
        /* indeterminate: no attribute, and a click clears it */
        "var c=document.createElement('input');c.type='checkbox';assert(c.indeterminate===false);"
        "c.indeterminate=true;assert(c.indeterminate&&!c.hasAttribute('indeterminate'),'no attribute');"
        "c.click();assert(!c.indeterminate&&c.checked,'a click clears it');"
        "c.indeterminate=true;c.addEventListener('click',e=>e.preventDefault());c.click();"
        "assert(c.indeterminate&&c.checked,'a cancelled click puts it back');"
        "assert(c.cloneNode().indeterminate===true,'a clone copies it');"
        /* autocomplete: the grammar's tokens, lowercased */
        "assert(i.autocomplete==='','missing');i.autocomplete='OFF';assert(i.autocomplete==='off');"
        "i.setAttribute('autocomplete',' section-a  Shipping work EMAIL ');"
        "assert(i.autocomplete==='section-a shipping work email','the grammar');"
        "i.setAttribute('autocomplete','work name');assert(i.autocomplete==='','a contact kind for a field that is none');"
        "i.setAttribute('autocomplete','nonsense');assert(i.autocomplete==='');"
        "var h=document.createElement('input');h.type='hidden';h.autocomplete='on';assert(h.autocomplete==='','no on for a hidden one');"
        /* width and height: an image button's, else 0 */
        "i.setAttribute('width','40');assert(i.width===0,'a text input has no image');"
        "i.type='image';assert(i.width===40&&i.height===0);i.height=7;assert(i.getAttribute('height')==='7');"
        /* form, and labels */
        "var f=document.createElement('form');f.id='theform';document.body.appendChild(f);"
        "var x=document.createElement('input');f.appendChild(x);assert(x.form===f,'its ancestor form');"
        "var y=document.createElement('textarea');y.setAttribute('form','theform');document.body.appendChild(y);"
        "assert(y.form===f&&s.form===null,'by its form attribute; none');"
        "x.id='xid';var l1=document.createElement('label');l1.setAttribute('for','xid');"
        "var l2=document.createElement('label');l2.appendChild(document.createElement('span')).appendChild(x);"
        "f.appendChild(l2);f.appendChild(l1);var ls=x.labels;"
        "assert(ls===x.labels&&ls instanceof NodeList&&ls.length===2&&ls[0]===l2&&ls[1]===l1,'both, in tree order');"
        "l1.setAttribute('for','other');assert(ls.length===1,'live');"
        "var z=document.createElement('input');l2.insertBefore(z,l2.firstChild);assert(ls.length===0&&z.labels.length===1,"
        "'a label labels its first labelable descendant');"
        "z.type='hidden';assert(z.labels===null,'a hidden input has none');"
        "var lone=document.createElement('label'),inner=document.createElement('select');lone.appendChild(inner);"
        "assert(inner.labels.length===1&&inner.labels[0]===lone,'a tree no document holds, its root a label');"
        "var d=document.createElement('div');d.id='twice';var w=document.createElement('input');w.id='twice';"
        "var lf=document.createElement('label');lf.setAttribute('for','twice');document.body.append(lf,d,w);"
        "assert(w.labels.length===0,'for names the first element with the ID, which is no control');"
        "var ei=document.createElement('input');ei.setAttribute('id','');var el=document.createElement('label');"
        "el.setAttribute('for','');document.body.append(el,ei);assert(ei.labels.length===0,'an empty for names nothing');"
        /* the form owner's rules: no ID is empty, and a form attribute is read only in a document */
        "var ef=document.createElement('form');ef.id='';document.body.appendChild(ef);"
        "var eb=document.createElement('button');eb.setAttribute('form','');ef.appendChild(eb);"
        "assert(eb.form===null,'an empty form attribute names nothing');"
        "var df=document.createElement('form'),di=document.createElement('input');di.setAttribute('form','theform');"
        "df.appendChild(di);assert(di.form===df,'out of a document, its ancestor whatever the attribute says');",
        "modern-on-controls");
    /* 0f's P3b: the form's, label's, fieldset's, legend's and output's
     * members, and a form's controls collection and RadioNodeList. */
    dom_script(&f, MODERN_ASSERT
        "assert([HTMLLabelElement,HTMLFieldSetElement,HTMLLegendElement,HTMLOutputElement,"
        "HTMLFormControlsCollection,RadioNodeList].every(function(i){return typeof i==='function'}),'interfaces');"
        "var F=document.createElement('form');document.body.appendChild(F);"
        "assert(document.createElement('label') instanceof HTMLLabelElement&&"
        "Object.getPrototypeOf(HTMLOutputElement.prototype)===HTMLElement.prototype,'kinds of their own');"
        /* the form's reflections */
        "assert(F.action===document.URL&&F.method==='get'&&F.enctype==='application/x-www-form-urlencoded'&&"
        "F.autocomplete==='on'&&F.target===''&&!F.noValidate,'defaults');"
        "F.setAttribute('method','POST');F.setAttribute('enctype','TEXT/plain');F.autocomplete='nonsense';"
        "assert(F.method==='post'&&F.encoding==='text/plain'&&F.autocomplete==='on','enumerated');"
        "F.acceptCharset='utf-8';F.noValidate=true;"
        "assert(F.getAttribute('accept-charset')==='utf-8'&&F.hasAttribute('novalidate'),'set');"
        "F.rel='noopener nofollow';assert(F.relList.length===2&&F.relList.contains('noopener')&&"
        "F.relList.supports('noopener')===false,'rel and relList');"
        /* elements: listed elements, an image button left out */
        "F.innerHTML='<input name=a><fieldset name=fs><output name=o>x</output></fieldset>"
        "<input type=image name=img><input type=radio name=r value=1><input type=radio name=r value=2>"
        "<input type=checkbox name=r value=3>';"
        "var E=F.elements;assert(E instanceof HTMLFormControlsCollection&&E===F.elements,'the same collection');"
        "assert(E.length===6&&F.length===6&&E[1].localName==='fieldset'&&E[2].localName==='output','listed');"
        "assert(F[0]===E[0]&&F[5]===E[5]&&F[6]===undefined,'form[i]');"
        "var R=E.namedItem('r');assert(R instanceof RadioNodeList&&R instanceof NodeList&&R.length===3,'a RadioNodeList');"
        "assert(E.r instanceof RadioNodeList&&F.r instanceof RadioNodeList&&E.namedItem('a')===E[0],'and by name');"
        "assert(R.value==='','none checked');R[2].checked=true;assert(R.value==='','a checkbox is no radio');"
        "R[1].checked=true;assert(R.value==='2','the checked radio');"
        "R.value='1';assert(R[0].checked&&R.value==='1','set: the first of that value');"
        "R.value='2';assert(R[1].checked&&!R[0].checked,'set: the radio of that value');"
        "R.value='9';assert(R[1].checked,'set: none of that value changes nothing');"
        "assert(R.r===undefined&&R.namedItem===undefined,'a NodeList, with no names');"
        "assert(E.namedItem('r')===R&&F.r===R&&E.r===R,'one RadioNodeList a name');"
        "assert(HTMLCollection.prototype.namedItem.call(E,'r')===R[0],"
        "'HTMLCollection\\'s own namedItem, called on the controls, is the first');"
        /* the form is a legacy platform object: names over builtins, its own names, no redefinition */
        "var K=document.createElement('form');K.innerHTML='<input name=submit><input id=two><input name=submit id=x>';"
        "assert(K.submit instanceof RadioNodeList&&K.two.localName==='input','a control hides the method');"
        "assert(Object.getOwnPropertyNames(K).join()==='0,1,2,submit,two,x'&&Object.keys(K).join()==='0,1,2','own names');"
        "var refused=false;try{Object.defineProperty(K,'two',{value:5})}catch(x){refused=x instanceof TypeError}"
        "assert(refused&&K.two.localName==='input','a name is not redefined');"
        "refused=false;try{(function(){'use strict';delete K[0]})()}catch(x){refused=x instanceof TypeError}"
        "assert(refused&&K[0].localName==='input','an index is not deleted');"
        "K.expando=1;assert(K.expando===1&&delete K.expando&&K.expando===undefined,'an expando is as it was');"
        "var Z=document.createElement('form');refused=false;"
        "try{Object.defineProperty(Z,'0',{value:1})}catch(x){refused=x instanceof TypeError}"
        "Z.appendChild(document.createElement('input'));"
        "assert(refused&&Z[0]===Z.elements[0]&&Z[0].localName==='input','an index it has no control for is refused too');"
        "var N=document.createElement('form');N.innerHTML='<input name=0><input id=1 name=n>';"
        "assert(Object.getOwnPropertyNames(N).join()==='0,1,n'&&Object.keys(N).join()==='0,1',"
        "'a name an index spells is that index');"
        "var X=document.createElement('form');X.extra=1;var XI=document.createElement('input');XI.name='extra';"
        "X.appendChild(XI);var xn=Object.getOwnPropertyNames(X),xk=Object.keys(X);"
        "assert(xn.filter(function(k){return k==='extra'}).length===1&&xk.filter(function(k){return k==='extra'}).length===1&&"
        "xn.indexOf('0')>=0,'a name the form has as an expando is listed once');"
        "var FI=document.createElement('fieldset');FI.innerHTML='<input type=image><input>';"
        "assert(FI.elements.length===2,'a fieldset lists an image button, as a form does not');"
        /* baseURI and a lone webauthn */
        "assert(document.body.baseURI===document.baseURI&&typeof document.baseURI==='string','baseURI');"
        "var at=document.createAttribute('q');assert(at.baseURI===document.baseURI,'an Attr\\'s');"
        "var W=document.createElement('input');W.setAttribute('autocomplete',' WEBAUTHN ');"
        "assert(W.autocomplete==='webauthn','webauthn alone');"
        /* requestSubmit's submitter, and a form in no document */
        "var Q=document.createElement('form'),QB=document.createElement('button');Q.appendChild(QB);"
        "refused=false;try{Q.requestSubmit(document.body)}catch(x){refused=x instanceof TypeError}"
        "assert(refused,'no submit button is a TypeError');"
        "refused=false;try{Q.requestSubmit(document.createElement('button'))}catch(x){refused=x.name==='NotFoundError'}"
        "assert(refused,'another form\\'s is NotFoundError');"
        "var sent=0;Q.addEventListener('submit',function(e){sent++;e.preventDefault()});Q.requestSubmit(QB);"
        "assert(sent===0,'a form in no document submits nothing, not even the event');"
        /* requestSubmit's submitter, and a form in no document */
        "var Q=document.createElement('form'),QB=document.createElement('button');Q.appendChild(QB);"
        "refused=false;try{Q.requestSubmit(document.body)}catch(x){refused=x instanceof TypeError}"
        "assert(refused,'no submit button is a TypeError');"
        "refused=false;try{Q.requestSubmit(document.createElement('button'))}catch(x){refused=x.name==='NotFoundError'}"
        "assert(refused,'another form\\'s is NotFoundError');"
        "var sent=0;Q.addEventListener('submit',function(e){sent++;e.preventDefault()});Q.requestSubmit(QB);"
        "assert(sent===0,'a form in no document submits nothing, not even the event');"
        /* label */
        "var L=document.createElement('label');L.htmlFor='lx';var LX=document.createElement('input');LX.id='lx';"
        "F.append(L,LX);assert(L.getAttribute('for')==='lx'&&L.control===LX&&L.form===F,'label');"
        "var L2=document.createElement('label');assert(L2.control===null&&L2.form===null,'no control');"
        /* fieldset */
        "var S=F.querySelector('fieldset');"
        "assert(S.type==='fieldset'&&S.name==='fs'&&S.form===F&&!S.disabled,'fieldset');"
        "var SE=S.elements;assert(SE===S.elements&&SE.length===1&&SE[0].localName==='output','its listed descendants');"
        "S.appendChild(document.createElement('button'));assert(SE.length===2,'live');"
        /* legend */
        "var G=S.insertBefore(document.createElement('legend'),S.firstChild);G.align='left';"
        "assert(G.form===F&&G.getAttribute('align')==='left'&&document.createElement('legend').form===null,'legend');"
        /* output: value, defaultValue and their override */
        "var O=document.createElement('output');assert(O.type==='output'&&O.value===''&&O.defaultValue==='','empty');"
        "O.textContent='5';assert(O.value==='5'&&O.defaultValue==='5','default mode follows the text');"
        "O.defaultValue='10';assert(O.textContent==='10'&&O.value==='10','default mode: defaultValue sets the text');"
        "O.value='20';assert(O.defaultValue==='10'&&O.textContent==='20','value mode: the default is kept');"
        "O.defaultValue='15';assert(O.value==='20'&&O.defaultValue==='15','value mode: defaultValue alone');"
        "O.htmlFor='a b';assert(O.htmlFor.length===2&&O.getAttribute('for')==='a b','htmlFor, PutForwards');"
        "var bad=false;try{O.htmlFor.supports('a')}catch(x){bad=x instanceof TypeError}assert(bad,'no supported tokens');"
        "O.name='o';assert(O.getAttribute('name')==='o'&&O.form===null&&O.labels.length===0,'output');"
        "bad=false;try{Object.getOwnPropertyDescriptor(HTMLOutputElement.prototype,'value').get.call(L)}"
        "catch(x){bad=x instanceof TypeError}assert(bad,'the brand is checked');",
        "modern-on-forms");
    /* 0f's P4: constraint validation. */
    dom_script(&f, MODERN_ASSERT
        "function el(t,a){var e=document.createElement(t);for(var k in a)e.setAttribute(k,a[k]);return e}"
        "var i=el('input',{required:''});"
        "assert(typeof ValidityState==='function'&&i.validity instanceof ValidityState&&i.validity===i.validity,"
        "'one ValidityState, [SameObject]');"
        "assert(Object.prototype.toString.call(i.validity)==='[object ValidityState]','its class string');"
        "var bad=false;try{new ValidityState()}catch(x){bad=x instanceof TypeError}assert(bad,'no constructor');"
        "assert(i.validity.valueMissing&&!i.validity.valid&&i.willValidate,'required and empty');"
        "assert(i.validationMessage==='Please fill out this field.','its sentence');"
        "i.value='x';assert(i.validity.valid&&!i.validity.valueMissing&&i.validationMessage==='','filled: the flags follow');"
        /* the type mismatches */
        "var m=el('input',{type:'email'});m.value='a@b.c';assert(m.validity.valid,'an address');"
        "m.value='abc';assert(m.validity.typeMismatch,'not an address');"
        "m.value='a\\0b@c.d';assert(m.validity.typeMismatch,'a NUL is no address character');"
        "m.value='a@b.c,d@e.f';assert(m.validity.typeMismatch,'two need multiple');"
        "m.multiple=true;assert(m.validity.valid,'a list');m.value='a@b.c;d@e.f';"
        "assert(m.validity.typeMismatch,'not a list');m.value='a@-b.c';assert(m.validity.typeMismatch,'a label');"
        "var u=el('input',{type:'url'});u.value='http://x.example';assert(u.validity.valid,'a URL');"
        "u.value='abc';assert(u.validity.typeMismatch,'no scheme');u.value='mailto:a';"
        "assert(u.validity.valid,'a scheme without a host');u.value='http://';assert(u.validity.typeMismatch,'no host');"
        /* the pattern: the engine's, anchored, with v; one that does not compile is none */
        "var p=el('input',{pattern:'[a-z]+'});p.value='abc';assert(p.validity.valid,'matches');"
        "p.value='abc1';assert(p.validity.patternMismatch,'anchored at the end');"
        "p.value='';assert(p.validity.valid,'an empty value is no mismatch');"
        "p.setAttribute('pattern','http://a-b.example');p.value='http://a-b.example';"
        "assert(p.validity.valid,'a slash and a hyphen outside a class');p.value='http://a-b.examplf';"
        "assert(p.validity.patternMismatch,'and the pattern judged');"
        "p.setAttribute('pattern','a)(b');p.value='zzz';assert(p.validity.valid,'one that escapes its group is none');"
        "p.setAttribute('pattern','[');assert(p.validity.valid,'one that does not compile is none');"
        "var pm=el('input',{type:'email',multiple:'',pattern:'[a-z]+@[a-z]+\\\\.example'});"
        "pm.value='x@a.example,y@b.example';assert(pm.validity.valid,'an email list address by address');"
        "pm.value='x@a.example,Y@b.example';assert(pm.validity.patternMismatch,'one address that does not match');"
        "p.setAttribute('pattern','[\\\\p{L}--[a-z]]');p.value='A';"
        "assert(p.validity.valid,'the v flag: a set difference');p.value='a';"
        "assert(p.validity.patternMismatch,'and judged by it');"
        "var hp=el('input',{type:'hidden',pattern:'x'});hp.value='y';assert(hp.validity.valid,'no pattern on hidden');"
        /* the custom error, which barring does not clear */
        "var c=el('input');c.setCustomValidity('Nope');"
        "assert(c.validity.customError&&!c.validity.valid&&c.validationMessage==='Nope','a custom error');"
        "c.disabled=true;assert(c.validity.customError&&c.validationMessage===''&&!c.willValidate&&c.checkValidity(),"
        "'disabled: still its flag, but it counts for nothing');"
        "c.disabled=false;c.setCustomValidity('');assert(c.validity.valid,'cleared');"
        "var long='\\u00e9'.repeat(300);c.setCustomValidity(long);"
        "assert(c.validationMessage===long,'a long message whole');"
        "c.setCustomValidity('a\\0b');"
        "assert(c.validationMessage==='a\\ufffdb','a NUL, which the binding\\'s text never holds, is U+FFFD');"
        "c.setCustomValidity('');"
        "c.setCustomValidity('Nope');assert(!c.cloneNode().validity.customError,'a clone does not copy it');"
        "var fs=el('fieldset');fs.setCustomValidity('f');"
        "assert(fs.validity.customError&&!fs.willValidate&&fs.checkValidity()&&fs.validationMessage==='',"
        "'a fieldset has a custom error and never validates');"
        "var o=el('output');assert(!o.willValidate&&o.validity.valid,'an output');"
        /* willValidate */
        "assert(!el('input',{type:'hidden'}).willValidate&&!el('input',{type:'reset'}).willValidate&&"
        "!el('input',{type:'button'}).willValidate&&!el('button',{type:'button'}).willValidate&&"
        "el('button').willValidate,'the types that are not fields');"
        "assert(!el('input',{readonly:''}).willValidate&&!el('input',{type:'checkbox',readonly:''}).willValidate&&"
        "!el('textarea',{readonly:''}).willValidate&&el('select',{readonly:''}).willValidate,"
        "'readonly bars any input, applying or not, and a textarea; not a select');"
        "var dl=el('datalist');var di=el('input');dl.appendChild(di);assert(!di.willValidate,'in a datalist');"
        "var dfs=el('fieldset',{disabled:''});var lg=el('legend');var inl=el('input');var outl=el('input');"
        "lg.appendChild(inl);dfs.appendChild(lg);dfs.appendChild(outl);"
        "assert(inl.willValidate&&!outl.willValidate,'a disabled fieldset, but its first legend');"
        /* what a disabled control still suffers from */
        "var dc=el('input',{type:'checkbox',required:'',disabled:''});assert(dc.validity.valueMissing,'a tick');"
        "var dt=el('input',{required:'',disabled:''});assert(!dt.validity.valueMissing,'a field it cannot fill');"
        /* radios, the select's placeholder */
        "var rf=el('form');rf.innerHTML='<input type=radio name=r required><input type=radio name=r>"
        "<input type=radio required>';var rs=rf.querySelectorAll('input');"
        "assert(rs[0].validity.valueMissing&&rs[1].validity.valueMissing,'the group is required');"
        "assert(!rs[2].validity.valueMissing,'a radio with no name is in no group');"
        "rs[1].checked=true;assert(rs[0].validity.valid&&rs[1].validity.valid,'one ticked');"
        "var se=el('select',{required:''});se.innerHTML='<option value=\"\">Choose<option>A';"
        "assert(se.validity.valueMissing,'the placeholder');se.selectedIndex=1;assert(se.validity.valid,'a real one');"
        /* P4b: min, max and step, the step's grid drawn from the min */
        "var r=el('input',{type:'number',min:'2',max:'5',step:'2'});r.value='1';"
        "assert(r.validity.rangeUnderflow&&r.validationMessage==='Value must be greater than or equal to 2.','underflow');"
        "r.value='7';assert(r.validity.rangeOverflow&&!r.validity.rangeUnderflow,'overflow');"
        "r.value='3';assert(r.validity.stepMismatch,'off the grid');r.value='4';assert(r.validity.valid,'on it');"
        "var dd=el('input',{type:'date',min:'2000-01-02'});dd.value='2000-01-01';"
        "assert(dd.validity.rangeUnderflow&&dd.validationMessage==='Value must be 2000-01-02 or later.','a date');"
        /* a value a script sets is never bad input */
        "var n=el('input',{type:'number'});n.value='abc';assert(n.value===''&&!n.validity.badInput,'badInput');",
        "modern-on-validity");
    /* 0f's P5: the value as a number, libpage's conversions. The binding's
     * part: the JavaScript values, the errors and their order, and the
     * Date read with the getTime taken before any page ran. */
    dom_script(&f, MODERN_ASSERT
        "function el(t,a){var e=document.createElement(t);for(var k in a)e.setAttribute(k,a[k]);return e}"
        "function throws(f,name){try{f()}catch(x){return x.name===name}return false}"
        "var d=el('input',{type:'date'});d.value='2019-12-10';"
        "assert(d.valueAsNumber===1575936000000,'a date as a number');"
        "var v=d.valueAsDate;assert(v instanceof Date&&v.getTime()===1575936000000&&v!==d.valueAsDate,'a new Date each read');"
        "d.valueAsNumber=0;assert(d.value==='1970-01-01','set as a number');"
        "d.valueAsDate=new Date(86400000);assert(d.value==='1970-01-02','set as a Date');"
        "d.valueAsDate=null;assert(d.value===''&&d.valueAsDate===null&&Number.isNaN(d.valueAsNumber),'null empties it');"
        "d.value='2020-01-01';d.valueAsDate=undefined;assert(d.value==='','undefined is null');"
        "d.value='275760-12-31';assert(d.valueAsDate===null,'past a Date: null, not an invalid Date');"
        "assert(throws(function(){d.valueAsDate={}},'TypeError'),'no Date: TypeError');"
        "assert(throws(function(){d.valueAsDate=5},'TypeError'),'a number is no Date');"
        "Date.prototype.getTime=function(){return 0};d.valueAsDate=new Date(172800000);"
        "assert(d.value==='1970-01-03','the engine own getTime');"
        "var m=el('input',{type:'month'});m.valueAsNumber='599';assert(m.value==='2019-12','a string converted first');"
        "assert(m.valueAsDate.toISOString()==='2019-12-01T00:00:00.000Z','a month as its first midnight');"
        "var t=el('input',{type:'datetime-local'});assert(t.valueAsDate===null,'no Date for a local date and time');"
        "assert(throws(function(){t.valueAsDate=new Date(0)},'InvalidStateError'),'nor set as one');"
        "var c=el('input',{type:'checkbox'});assert(Number.isNaN(c.valueAsNumber)&&c.valueAsDate===null,'not a number');"
        "assert(throws(function(){c.valueAsNumber=5},'InvalidStateError'),'set where it does not apply');"
        "assert(throws(function(){c.valueAsNumber=Infinity},'TypeError'),'infinity before applying');"
        "assert(throws(function(){c.valueAsDate={}},'InvalidStateError'),'applying before the Date');"
        "assert(throws(function(){c.valueAsDate=5},'TypeError'),'no object: WebIDL first');"
        "var n=el('input',{type:'number'});n.valueAsNumber=NaN;assert(n.value==='','NaN empties it');"
        "assert(throws(function(){n.valueAsNumber=-Infinity},'TypeError'),'infinity refused');"
        "n.stepUp();assert(n.value==='1','stepUp');n.stepDown(3);assert(n.value==='-2','stepDown(n)');"
        "n.stepUp(4294967298);assert(n.value==='0','ToInt32 wraps n');"
        "n.stepUp(undefined);assert(n.value==='1','undefined is 1');"
        "n.setAttribute('step','any');assert(throws(function(){n.stepUp()},'InvalidStateError'),'step any');"
        "assert(throws(function(){c.stepDown()},'InvalidStateError'),'a checkbox steps not');"
        "var p=HTMLInputElement.prototype;"
        "assert(p.stepUp.name==='stepUp'&&p.stepUp.length===0&&p.stepDown.name==='stepDown','operations named');"
        "assert(throws(function(){p.stepUp.call(el('div'))},'TypeError'),'the brand is checked');"
        "assert(throws(function(){Object.getOwnPropertyDescriptor(p,'valueAsNumber').get.call(el('textarea'))},"
        "'TypeError'),'an input alone');",
        "modern-on-value-as");
    /* 0f's P6a: the element kinds whose members are reflections. */
    dom_script(&f, MODERN_ASSERT
        "function el(t,a){var e=document.createElement(t);for(var k in a)e.setAttribute(k,a[k]);return e}"
        "function throws(f,name){try{f()}catch(x){return x.name===name}return false}"
        "function kind(t,I){var e=el(t);return e instanceof I&&e.constructor===I}"
        "assert(kind('h1',HTMLHeadingElement)&&kind('h6',HTMLHeadingElement)&&kind('ins',HTMLModElement)"
        "&&kind('del',HTMLModElement)&&kind('q',HTMLQuoteElement)&&kind('blockquote',HTMLQuoteElement),'shared kinds');"
        "assert(kind('listing',HTMLPreElement)&&kind('xmp',HTMLPreElement)&&kind('thead',HTMLTableSectionElement)"
        "&&kind('th',HTMLTableCellElement)&&kind('colgroup',HTMLTableColElement)&&kind('dir',HTMLDirectoryElement),"
        "'HTML\\'s mapping');"
        "assert(kind('foo',HTMLUnknownElement)&&kind('applet',HTMLUnknownElement)&&kind('blink',HTMLUnknownElement),"
        "'unknown names');"
        "assert(el('x-foo').constructor===HTMLElement&&el('section').constructor===HTMLElement"
        "&&el('font-face').constructor===HTMLUnknownElement,'a custom element name is HTMLElement\\'s');"
        "assert(Object.getPrototypeOf(HTMLUnknownElement)===HTMLElement&&document.head instanceof HTMLHeadElement"
        "&&document.documentElement instanceof HTMLHtmlElement,'the tree\\'s own');"
        "var td=el('td',{colspan:'0',rowspan:'70000'});assert(td.colSpan===1&&td.rowSpan===65534,'clamped');"
        "td.setAttribute('colspan','2000');td.setAttribute('rowspan','0');assert(td.colSpan===1000&&td.rowSpan===0,'ends');"
        "td.setAttribute('colspan','x');assert(td.colSpan===1,'a default');td.colSpan=5;"
        "assert(td.getAttribute('colspan')==='5','set as unsigned');"
        "var ol=el('ol');assert(ol.start===1,'a long\\'s default');ol.setAttribute('start','-3');"
        "assert(ol.start===-3,'a negative long');ol.start=7;assert(ol.getAttribute('start')==='7','set');"
        "ol.setAttribute('start','99999999999');assert(ol.start===1,'past a long: the default');"
        "ol.setAttribute('start','2147483648');assert(ol.start===1,'one past a long');"
        "ol.setAttribute('start','-2147483648');assert(ol.start===-2147483648,'a long\\'s least');"
        "assert(el('canvas').width===300&&el('canvas').height===150,'a canvas\\'s defaults');"
        "var b=el('body');b.bgColor=null;assert(b.getAttribute('bgcolor')==='','null as empty');"
        "var hr=el('hr');hr.color=null;assert(hr.getAttribute('color')==='null','null as null');"
        "assert(el('td',{scope:'ROW'}).scope==='row'&&el('td',{scope:'x'}).scope==='','an enumeration');"
        "assert(el('area',{referrerpolicy:'Origin'}).referrerPolicy==='origin','a referrer policy');"
        "assert(el('area',{href:'HTTPS://example.test/a'}).href==='https://example.test/a','area\\'s href');"
        "assert(el('base').href===document.URL,'a base without href: the document\\'s URL');"
        "assert(el('meta',{'http-equiv':'refresh'}).httpEquiv==='refresh'&&el('font',{color:'red'}).color==='red',"
        "'renamed attributes');"
        "var t=el('title');t.text='a<b';assert(t.text==='a<b'&&t.firstChild.data==='a<b','a title\\'s text');"
        "var align=Object.getOwnPropertyDescriptor(HTMLDivElement.prototype,'align').get;"
        "assert(throws(function(){align.call(el('p'))},'TypeError'),'a row\\'s other interface is no brand');"
        "assert(throws(function(){align.call(el('span'))},'TypeError'),'nor an interface without it');"
        "var h=function(){};document.body.onpopstate=h;assert(window.onpopstate===h,'a body\\'s handler is window\\'s');"
        "window.onpopstate=null;assert(Object.prototype.hasOwnProperty.call(HTMLBodyElement.prototype,'onmessage'),"
        "'WindowEventHandlers on the body');",
        "modern-on-elements");
    dom_script(&f, MODERN_ASSERT
        "function el(t,a){var e=document.createElement(t);for(var k in a)e.setAttribute(k,a[k]);return e}"
        /* checkValidity and reportValidity fire invalid: cancelable, not bubbling, trusted */
        "var heard=[];var i=el('input',{required:''});document.body.appendChild(i);"
        "document.body.addEventListener('invalid',function(){heard.push('bubbled')});"
        "i.addEventListener('invalid',function(e){heard.push(e.type+e.cancelable+e.bubbles+e.isTrusted)});"
        "assert(i.checkValidity()===false&&heard.join()==='invalidtruefalsetrue','checkValidity');"
        "heard=[];assert(i.reportValidity()===false&&heard.length===1,'reportValidity');"
        "i.value='x';heard=[];assert(i.checkValidity()&&i.reportValidity()&&heard.length===0,'valid: no event');"
        /* oninvalid */
        "var h=el('input',{required:''});var got=0;h.oninvalid=function(){got++};h.checkValidity();"
        "assert(got===1&&'oninvalid' in window&&'oninvalid' in document,'the handler');"
        "h.setAttribute('oninvalid','window.attrHeard=1');h.checkValidity();assert(window.attrHeard===1,'its attribute');"
        /* a form: each invalid control it owns, in tree order, and only those */
        "var F=el('form',{id:'vf'});document.body.appendChild(F);"
        "F.innerHTML='<input name=a required><fieldset><input name=b required></fieldset>"
        "<input name=c><input name=d required disabled>';"
        "var outside=el('input',{form:'vf',required:''});document.body.appendChild(outside);"
        "var stray=el('input',{name:'stray',required:''});document.body.appendChild(stray);"
        "var order=[];document.addEventListener('invalid',function(e){order.push(e.target.name||'outside')},true);"
        "assert(F.checkValidity()===false&&order.join()==='a,b,outside','the form, its owned controls in tree order');"
        /* a listener that cancels invalid leaves nothing to report, and the form is still invalid */
        "order=[];F.addEventListener('invalid',function(e){e.preventDefault()},true);"
        "assert(F.reportValidity()===false&&order.length===3,'reportValidity of the form');"
        /* requestSubmit: invalid stops the submission before submit */
        "var sent=0;F.addEventListener('submit',function(e){sent++;e.preventDefault()});"
        "F.requestSubmit();assert(sent===0,'invalid: no submit event');"
        /* a submission an invalid listener asks of its own form does nothing */
        "var nested=0;var again=function(){nested++;F.requestSubmit()};F.addEventListener('invalid',again,true);"
        "order=[];F.requestSubmit();F.removeEventListener('invalid',again,true);"
        "assert(nested===2&&order.length===3&&sent===0,'no submission inside its own validation');"
        "F.noValidate=true;F.requestSubmit();assert(sent===1,'novalidate: submitted');F.noValidate=false;"
        "var fb=el('button',{formnovalidate:''});F.appendChild(fb);F.requestSubmit(fb);"
        "assert(sent===2,'formnovalidate: submitted');"
        "Array.prototype.forEach.call(F.querySelectorAll('input'),function(x){x.value='v'});outside.value='v';"
        "F.requestSubmit();assert(sent===3,'valid: submitted');"
        "sent=0;order=[];F.elements.a.value='';F.submit();"
        "assert(sent===0&&order.length===0,'submit() fires nothing, validates nothing');"
        /* the brand */
        "var bad=false;try{Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'validity').get.call(el('div'))}"
        "catch(x){bad=x instanceof TypeError}assert(bad,'the brand is checked');"
        "bad=false;try{Object.getOwnPropertyDescriptor(ValidityState.prototype,'valid').get.call({})}"
        "catch(x){bad=x instanceof TypeError}assert(bad,'a ValidityState is checked');",
        "modern-on-validity-events");
    /* The select event a set queues: a task of its own, which the host runs
     * (a binding timer due at once); trusted, bubbling, not cancelable; and
     * none for a set that moves nothing. The cases before this one queued
     * theirs, which run first. */
    os64_js_outcome_t select_out;
    while (os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &select_out)) {}
    dom_script(&f, MODERN_ASSERT
        "globalThis.selects=[];var sel=document.createElement('input');sel.value='abcdef';document.body.appendChild(sel);"
        "document.body.addEventListener('select',e=>selects.push(e.target===sel&&e.isTrusted&&e.bubbles&&!e.cancelable));"
        "sel.setSelectionRange(1,3);assert(selects.length===0,'queued, not fired inside the set');",
        "modern-on-select-event");
    check(os64_dom_timer_next(f.binding) != UINT64_MAX && os64_dom_timer_fire(f.binding, UINT64_MAX - 1, &select_out) &&
          select_out.status == OS64_JS_OK && os64_dom_timer_next(f.binding) == UINT64_MAX,
          "modern: the select event is one task the host runs");
    dom_script(&f, MODERN_ASSERT
        "assert(selects.length===1&&selects[0]===true,'trusted, bubbling, not cancelable: '+selects);"
        "sel.setSelectionRange(1,3);",
        "modern-on-select-event-heard");
    check(os64_dom_timer_next(f.binding) == UINT64_MAX, "modern: a set that moves nothing queues no select event");
    /* 0f's P1b: EventTarget, its constructor, and Window. The listener
     * methods are EventTarget.prototype's alone, which Node's, the plain
     * targets' and the global's chains reach; a bare call is the window's. */
    dom_script(&f, MODERN_ASSERT
        "assert(typeof EventTarget==='function'&&typeof Window==='function','both interfaces');"
        "var t=new EventTarget(),hits=0;t.addEventListener('ping',function(e){hits++;assert(e.target===t,'the target is the target')});"
        "assert(t.dispatchEvent(new Event('ping'))===true&&hits===1,'a plain target dispatches');"
        "class Bus extends EventTarget{fire(){return this.dispatchEvent(new Event('go'))}}"
        "var bus=new Bus(),got=0;bus.addEventListener('go',function(){got++});bus.fire();"
        "assert(got===1&&bus instanceof EventTarget&&bus instanceof Bus,'a subclass is a target');"
        "assert(Object.getPrototypeOf(Node.prototype)===EventTarget.prototype&&Object.getPrototypeOf(Node)===EventTarget,'Node inherits EventTarget');"
        "assert(!Object.prototype.hasOwnProperty.call(Node.prototype,'addEventListener')&&"
        "typeof Node.prototype.addEventListener==='function','Node inherits the methods');"
        "assert(Object.getPrototypeOf(window)===Window.prototype&&Object.getPrototypeOf(Window.prototype)===EventTarget.prototype,'the window chain');"
        "assert(window instanceof Window&&window instanceof EventTarget&&Object.getPrototypeOf(Window)===EventTarget,'Window inherits EventTarget');"
        "assert(!Object.prototype.hasOwnProperty.call(window,'addEventListener')&&"
        "window.addEventListener===EventTarget.prototype.addEventListener,'the window inherits them');"
        "var w=0;addEventListener('bare',function(){w++});dispatchEvent(new Event('bare'));assert(w===1,'a bare call is the window\\'s');"
        "var bad=false;try{new Window()}catch(e){bad=e instanceof TypeError}assert(bad,'Window is not constructed');"
        "assert(Object.getPrototypeOf(AbortSignal)===EventTarget&&Object.getPrototypeOf(AbortSignal.prototype)===EventTarget.prototype&&"
        "Object.getPrototypeOf(XMLHttpRequestEventTarget)===EventTarget,"
        "'a plain target\\'s interface inherits EventTarget');"
        "assert(document.createElement('p') instanceof EventTarget&&new AbortController().signal instanceof EventTarget,'nodes and signals are targets');"
        "assert(Object.prototype.toString.call(window)==='[object Window]'&&Object.prototype.toString.call(t)==='[object EventTarget]'&&"
        "Object.getOwnPropertySymbols(window).indexOf(Symbol.toStringTag)<0,'the string tags');",
        "modern-on-event-target");
    /* DOMParser, HTML only, as ES5 TodoMVC renders its list */
    dom_script(&f, MODERN_ASSERT
        "var p=new DOMParser();assert(p instanceof DOMParser&&Object.prototype.toString.call(p)==='[object DOMParser]');"
        "var d=p.parseFromString('<title>T</title><li data-id=1>one</li><li data-id=2>two</li><script>globalThis.ran=1<\\/script>','text/html');"
        "assert(d.nodeType===9&&d!==document&&d.head.firstChild.textContent==='T'&&d.body.children.length===3,'head and body as a document parse puts them');"
        "assert(typeof globalThis.ran==='undefined','its script never runs');"
        "var whole=p.parseFromString('<!doctype html><html lang=en class=theme><head><title>W</title><body><p>x','text/html');"
        "assert(whole.documentElement.getAttribute('lang')==='en'&&whole.documentElement.className==='theme'&&"
        "whole.documentElement.parentNode===whole&&whole.title==='W'&&whole.body.firstChild.tagName==='P',"
        "'a whole document keeps its root element\\'s attributes');"        "var ul=document.createElement('ul');document.body.appendChild(ul);ul.appendChild(document.createElement('i'));"
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
        "var other=document.implementation.createHTMLDocument('');"
        "assert(other.createElementNS(SVG,'g').ownerDocument===other,'made by the document it was asked of');"
        "function name(f){try{f();return 'none'}catch(e){return e.name}}"
        "assert(name(()=>document.createElementNS(SVG,'a:'))==='InvalidCharacterError','a bad name');"
        "assert(name(()=>document.createElementNS(null,'p:a'))==='NamespaceError','a prefix with no namespace');"
        "assert(name(()=>document.createElementNS('','p:a'))==='NamespaceError','the empty string is no namespace');"
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
        /* react-dom sets selected, then defaultSelected: the options the
         * first deselected stay clean, so the second's attribute is followed */
        "var r=document.createElement('select');r.innerHTML='<option>a<option>b<option selected>c';var ro=r.options;"
        "ro[0].selected=true;assert(r.selectedIndex===0&&!ro[2].selected,'a parsed selected sibling is deselected');"
        "ro[1].defaultSelected=true;assert(r.selectedIndex===1&&ro[1].selected&&!ro[0].selected,'a clean sibling follows its attribute');"
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
        "var fo=new Option('f');fo.defaultSelected=true;assert(fo.selected,'a factory option is clean: its attribute is followed');"
        "var fw=document.createElement('select');fw.innerHTML='<option>x';fw.add(fo);fw.options[0].selected=true;"
        "assert(fw.selectedIndex===0&&!fo.selected,'and deselected by a sibling once its select holds it');"
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

    /* baseURI is the document's base URL, its fragment kept: the page's
     * address's, and a base's own. */
    os64_dom_options_t options = os64_dom_default_options();
    options.url = "https://example.test/dir/page#top";
    f = fixture_new(&options);
    if (fixture_ready(&f))
        dom_script(&f, MODERN_ASSERT
            "assert(document.baseURI==='https://example.test/dir/page#top'&&"
            "document.body.baseURI===document.baseURI,'the page\\'s address, its fragment kept');"
            "var B=document.createElement('base');B.setAttribute('href','/other/x#frag');document.head.appendChild(B);"
            "assert(document.baseURI==='https://example.test/other/x#frag'&&"
            "document.createAttribute('q').baseURI===document.baseURI,'a base, its own fragment kept');"
            "B.remove();assert(document.baseURI==='https://example.test/dir/page#top','and without it again');"
            "var D=document.implementation.createHTMLDocument('x');"
            "assert(D.baseURI==='about:blank'&&D.body.baseURI==='about:blank','a document with no window: about:blank');"
            "var DB=D.createElement('base');DB.setAttribute('href','https://example.test/assets/#a');D.head.appendChild(DB);"
            "assert(D.baseURI==='https://example.test/assets/#a'&&D.body.baseURI===D.baseURI&&"
            "document.baseURI==='https://example.test/dir/page#top','its own base counts, and is not the page\\'s');"
            "DB.setAttribute('href','rel/');assert(D.baseURI==='about:blank','one that will not resolve: its fallback');",
            "modern-on-base-uri");
    /* The page's style sheets (sheets.c): document.styleSheets, live, by
     * libpage's rule; a sheet made once a node, and a node's `sheet`. */
    if (fixture_ready(&f))
        dom_script(&f, MODERN_ASSERT
            "assert(typeof StyleSheetList==='function'&&typeof StyleSheet==='function'&&"
            "Object.getPrototypeOf(CSSStyleSheet.prototype)===StyleSheet.prototype&&"
            "document.createElement('style') instanceof HTMLStyleElement,'interfaces');"
            "var L=document.styleSheets,n=L.length;"
            "assert(L instanceof StyleSheetList&&L===document.styleSheets,'one list');"
            "var s=document.createElement('style');s.textContent='p{}';s.title='T';s.id='sid';document.head.appendChild(s);"
            "assert(L.length===n+1&&L[n]===s.sheet&&L.item(n)===s.sheet&&L.item(n+1)===null&&L[n+1]===undefined,"
            "'live, the new one last');"
            "var S=s.sheet;assert(S instanceof CSSStyleSheet&&S===s.sheet&&S.type==='text/css'&&S.href===null&&"
            "S.ownerNode===s&&S.title==='T'&&S.disabled===false&&S.parentStyleSheet===null&&S.ownerRule===null,"
            "'a style element\\'s sheet');"
            "s.title='';assert(S.title===null,'an empty title is none');"
            "var k=document.createElement('link');k.rel='stylesheet';k.href='/x.css';document.head.appendChild(k);"
            "assert(L.length===n+2&&L[n+1]===k.sheet&&k.sheet.href==='https://example.test/x.css'&&"
            "k.sheet.title===null,'a linked sheet, its address resolved');"
            "var a=document.createElement('link');a.rel='alternate stylesheet';a.href='y.css';"
            "var t=document.createElement('style');t.setAttribute('type','text/plain');document.head.append(a,t);"
            "assert(L.length===n+2&&a.sheet===null&&t.sheet===null,'an alternate sheet and another language name none');"
            "var seen=[];for(var x of L)seen.push(x);"
            "assert(seen.length===n+2&&seen[n]===S&&L.namedItem===undefined&&L.sid===undefined&&!('sid' in L),'iterable, and no names');"
            "s.remove();assert(L.length===n+1&&s.sheet===null&&S.ownerNode===null,"
            "'a removed style has no sheet, and its sheet no owner');"
            "var bad=false;try{Object.getOwnPropertyDescriptor(StyleSheet.prototype,'type').get.call({})}"
            "catch(e){bad=e instanceof TypeError}assert(bad,'the brand is checked');"
            "k.remove();a.remove();t.remove();",
            "modern-on-sheets");
    fixture_free(&f);

    options = os64_dom_default_options();
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
        "assert(fr.getAttribute('src')===null&&fr.hasOwnProperty('src')&&!('HTMLIFrameElement' in window));"
        /* the node row's element names */
        "assert(!('localName' in b)&&!('namespaceURI' in b)&&!('prefix' in b));"
        /* 0f's P3b's members; a form's elements are a collection, and form[i] nothing */
        "var lf=document.createElement('form');lf.innerHTML='<input name=x><input name=x>';"
        "assert(!('control' in document.createElement('label'))&&!('length' in lf)&&lf[0]===undefined&&"
        "('namedItem' in lf.x)&&!('value' in lf.x));"
        /* the page's style sheets */
        "assert(!('styleSheets' in document)&&!('sheet' in document.createElement('style'))&&"
        "!('sheet' in document.createElement('link')));"
        /* 0f's P3a's controls' members; their name and value are every page's */
        "var ci=document.createElement('input');"
        "assert(!('formAction' in ci)&&!('labels' in ci)&&!('indeterminate' in ci)&&!('form' in ci)&&"
        "'name' in ci&&'value' in ci);"
        /* 0f's P6a's element kinds' members; an anchor's href is every page's */
        "assert(!('href' in document.createElement('area'))&&!('colSpan' in document.createElement('td'))&&"
        "!('text' in document.createElement('title'))&&!('onpopstate' in document.body)&&"
        "'href' in document.createElement('a'));",
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
