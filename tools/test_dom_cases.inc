#define DOM_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;assert(e.name===n,'wanted '+n+' got '+e.name)}assert(caught,'no '+n);}"
static bool fixture_ready(DomFixture *fixture)
{
    check(fixture->binding != NULL, "DOM binding construction");
    if (!fixture->binding) {
        fixture_free(fixture);
        return false;
    }
    check(os64_dom_registry_count(fixture->binding) > 0, "binding retains an explicit registry");
    return true;
}
static void dom_surface_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.alert = alert_capture;
    options.alert_opaque = alert_text;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    check(os64_js_install_output(f.runtime, 5, OS64_JS_OUTPUT_CONSOLE, &outcome) == OS64_JS_OK,
          "console capability installed separately");
    output_len = 0; alerts = 0;
    dom_script(&f, DOM_ASSERT
        "assert(window===globalThis&&window.document===document);"
        "assert(document.nodeType===9&&document.nodeName==='#document');"
        "assert(document.documentElement.tagName==='HTML'&&document.head.tagName==='HEAD'&&document.body.tagName==='BODY');"
        "assert(document.getElementById('absent')===null);"
        "const box=document.getElementById('box');assert(box.id==='box'&&box.className==='old');"
        "box.setAttribute('data-x','yes');assert(box.hasAttribute('data-x')&&box.getAttribute('data-x')==='yes');"
        "box.removeAttribute('data-x');assert(!box.hasAttribute('data-x')&&box.getAttribute('data-x')===null);"
        "box.id='renamed';box.className='new';assert(document.getElementById('renamed')===box);"
        "const el=document.createElement('SPAN'),text=document.createTextNode('hello'),comment=document.createComment('note'),frag=document.createDocumentFragment();"
        "assert(el.tagName==='SPAN'&&el.nodeName==='SPAN'&&el.nodeType===1);"
        "assert(text.nodeType===3&&text.nodeName==='#text'&&text.nodeValue==='hello'&&text.data==='hello');"
        "assert(comment.nodeType===8&&comment.nodeName==='#comment'&&comment.data==='note');"
        "assert(frag.nodeType===11&&frag.nodeName==='#document-fragment');"
        "assert(el.appendChild(text)===text&&el.insertBefore(comment,text)===comment);"
        "assert(el.firstChild===comment&&el.lastChild===text&&comment.nextSibling===text&&text.previousSibling===comment&&text.parentNode===el);"
        "text.nodeValue='changed';assert(text.data==='changed');text.data='again';assert(text.textContent==='again');"
        "assert(el.textContent==='again');"
        "assert(el.removeChild(comment)===comment&&comment.parentNode===null&&comment.nextSibling===null&&comment.previousSibling===null);"
        "const replacement=document.createTextNode('replacement');assert(el.replaceChild(replacement,text)===text);"
        "assert(text.parentNode===null&&el.textContent==='replacement');"
        "const shallow=el.cloneNode(false),deep=el.cloneNode(true);assert(shallow.childNodes.length===0&&deep.textContent==='replacement');"
        "assert(deep!==el&&deep.firstChild!==el.firstChild);"
        "assert(frag.appendChild(el)===el);assert(box.appendChild(frag)===frag&&frag.childNodes.length===0&&el.parentNode===box);"
        "el.textContent='sole';assert(el.childNodes.length===1&&el.firstChild.data==='sole');"
        "el.innerHTML='<b a=\"&amp;\">bold</b><i>italic</i>';"
        "assert(el.innerHTML==='<b a=\"&amp;\">bold</b><i>italic</i>');assert(el.outerHTML==='<span><b a=\"&amp;\">bold</b><i>italic</i></span>');"
        "const t=document.getElementById('template');assert(t.childNodes.length===0&&t.innerHTML==='<b>inside</b>');"
        "assert(t.cloneNode(true).innerHTML==='<b>inside</b>');"
        "console.log('dom',42);alert('notice');assert(confirm('question')===false&&prompt('question')===null);",
        "surface.js");
    check(output_len == 7 && !memcmp(output, "dom 42\n", 7), "console bytes captured");
    check(alerts == 1 && alert_length == 6 && !strcmp(alert_text, "notice"), "alert is nonmodal captured host message");
    os64_html_node_t *box = native_id(f.document->document, "renamed");
    check(box && os64_html_attr(box,"class") && !strcmp(os64_html_attr(box,"class")->value,"new"),
          "native tree agrees with script attribute effects");
    if (box) {
        char *native = native_serialized(box->last_child);
        check(!strcmp(native,"<span><b a=\"&amp;\">bold</b><i>italic</i></span>"),
              "native tree agrees with script content effects");
        free(native);
    }
    fixture_free(&f);
}
static void dom_identity_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, DOM_ASSERT
        "globalThis.saved=document.body;globalThis.held=document.getElementById('one');"
        "held.expando={owner:held};held.self=held;globalThis.kids=held.childNodes;kids.owner=held;globalThis.savedFind=document.getElementById;"
        "assert(document.body===saved);document.getElementById('two').textContent='unrelated';",
        "identity-before-gc.js");
    os64_js_outcome_t outcome;
    JSContext *context = os64_js_context(f.runtime, OS64_JS_ABI_ID, &outcome);
    check(context != NULL, "GC fixture borrows checked context");
    if (context) {
        void *runtime_opaque = JS_GetRuntimeOpaque(JS_GetRuntime(context));
        void *context_opaque = JS_GetContextOpaque(context);
        JS_RunGC(JS_GetRuntime(context));
        check(runtime_opaque == JS_GetRuntimeOpaque(JS_GetRuntime(context)) &&
                  context_opaque == JS_GetContextOpaque(context), "binding leaves runtime/context opaque slots owned by libjs");
    }
    os64_page_t *old = os64_page_build(f.document,"https://fixture.test/",NULL,f.state);
    check(old != NULL,"identity fixture model");
    dom_script(&f, DOM_ASSERT
        "assert(saved===document.body&&held===document.getElementById('one'));"
        "assert(held.expando.owner===held&&held.self===held&&held.childNodes===kids&&kids.owner===held);"
        "held.parentNode.removeChild(held);assert(held.parentNode===null);"
        "document.getElementById('box').appendChild(held);assert(document.getElementById('one')===held);",
        "identity-after-gc.js");
    os64_page_t *fresh = old ? os64_page_rebuild(old) : NULL;
    check(!old || fresh != NULL,"identity survives native model rebuild");
    dom_script(&f,DOM_ASSERT "assert(saved===document.body&&held.expando.owner===held);","identity-after-rebuild.js");
    os64_page_free(fresh);os64_page_free(old);
    os64_dom_drain(f.binding);
    check(os64_dom_registry_count(f.binding)==0,"registry drains strong identity and cyclic expandos");
    dom_script(&f,DOM_ASSERT
        "throws('InvalidStateError',()=>document.getElementById('one'));throws('InvalidStateError',()=>savedFind.call(document,'one'));"
        "throws('InvalidStateError',()=>held.textContent);"
        "throws('InvalidStateError',()=>kids.length);",
        "retired-binding.js");
    fixture_free(&f);
}
static void dom_collection_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "const box=document.getElementById('box');globalThis.list=box.childNodes;globalThis.tags=document.getElementsByTagName('p');"
        "assert(list===box.childNodes&&list.length===2&&tags.length===2);"
        "assert(list[0]===document.getElementById('one')&&list.item(1)===document.getElementById('two'));"
        "assert(list.item(99)===null&&list[99]===undefined);assert(Object.keys(list).filter(k=>/^\\d+$/.test(k)).join(',')==='0,1');assert(Object.getOwnPropertyDescriptor(list,'0').value===list[0]);"
        "const p=document.createElement('p');p.id='third';box.appendChild(p);"
        "assert(list.length===3&&list[2]===p&&tags.length===3&&tags[2]===p);"
        "box.insertBefore(p,box.firstChild);assert(list[0]===p&&tags[0]===p);"
        "box.removeChild(p);assert(list.length===2&&tags.length===2);"
        "const detached=document.createElement('div');const held=detached.childNodes;"
        "detached.appendChild(p);assert(held.length===1&&held[0]===p);"
        "assert(document.getElementsByTagName('P').length===2);assert(document.getElementsByTagName('b').length===0);"
        "assert(document.getElementsByTagName('*').length>tags.length);throws('TypeError',()=>Object.defineProperty(list,'0',{value:'shadow'}));throws('TypeError',()=>Object.defineProperty(list,'7',{value:'future'}));assert(!Reflect.defineProperty(list,'0',{value:'shadow'})&&!Reflect.defineProperty(list,'7',{value:'future'}));assert(!Reflect.deleteProperty(list,'0')&&Reflect.deleteProperty(list,'7'));assert(!Reflect.preventExtensions(list));throws('TypeError',()=>Object.freeze(list));list['-1']='negative';list['01']='leading';list['4294967295']='nonindex';const key=Symbol('expando');list[key]='symbol';assert(list['-1']==='negative'&&list['01']==='leading'&&list['4294967295']==='nonindex'&&list[key]==='symbol');const numericSymbol=Symbol('0');list[numericSymbol]='symbol-zero';list['0\\0x']='nul-expando';assert(list[numericSymbol]==='symbol-zero'&&list['0\\0x']==='nul-expando'&&list[0]===list.item(0));const grow=document.createElement('em');box.appendChild(grow);assert(list.length===3&&list[2]===grow&&list.item(2)===grow);",
        "live-collections.js");
    dom_script(&f,DOM_ASSERT
        "const originalIndex=list[0];list[0]='sloppy-shadow';list[7]='sloppy-future';"
        "assert(list[0]===originalIndex&&list[7]===undefined&&!Object.hasOwn(list,'7'),'sloppy index assignments ignored');"
        "throws('TypeError',()=>{ 'use strict';list[0]='strict-shadow'; });"
        "throws('TypeError',()=>{ 'use strict';list[7]='strict-future'; });"
        "throws('TypeError',()=>{ 'use strict';delete list[0]; });"
        "assert((()=>{ 'use strict';return delete list[7]; })()===true,'unsupported strict delete succeeds');"
        "assert(Reflect.defineProperty(list,'ordinary',{value:'expando',writable:true,configurable:true}));"
        "assert(list.ordinary==='expando'&&delete list.ordinary&&!Object.hasOwn(list,'ordinary'),'ordinary expando define/delete');"
        "let futureNode;for(let i=3;i<8;i++){futureNode=document.createElement('span');box.appendChild(futureNode);}"
        "assert(list.length===8&&list[7]===futureNode&&list[7]===list.item(7),'future index grows without shadow');"
        "assert(list[0]===originalIndex,'supported index remains native');",
        "live-index-assignment-modes.js");
    fixture_free(&f);
}
/* Selectors API: first match or a static NodeList, in tree order, from the
 * receiver's descendants, with :scope the receiver; collections iterate. */
static void dom_selector_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "const box=document.getElementById('box'),one=document.getElementById('one'),two=document.getElementById('two');"
        "assert(document.querySelector('#box p')===one&&document.querySelector('p:last-child')===two,'first match');"
        "assert(document.querySelector('nope')===null&&document.querySelector('P')===one,'absent, and HTML names fold');"
        "const list=document.querySelectorAll('#two, #one');"
        "assert(list.length===2&&list[0]===one&&list[1]===two&&list.item(1)===two&&list.item(5)===null,'tree order, item');"
        "assert(Object.keys(list).join(',')==='0,1'&&[...list].length===2&&Array.from(list)[1]===two,'indices, spread');"
        "let seen=[];list.forEach((n,i)=>seen.push(i+n.id));assert(seen.join()==='0one,1two','forEach');"
        "assert([...list.keys()].join()==='0,1'&&[...list.values()][0]===one&&[...list.entries()][1][1]===two,'iterators');"
        "box.appendChild(document.createElement('p'));assert(list.length===2&&document.querySelectorAll('p').length===3,'static');"
        "assert(box.querySelector('div')===null&&box.querySelector('div p')===one,'descendants, ancestors outside count');"
        "assert(box.querySelectorAll(':scope > p').length===3&&document.body.querySelectorAll(':scope > p').length===0,':scope');"
        "assert(document.querySelectorAll(':scope').length===1&&document.querySelector(':scope')===document.documentElement,':scope of a document');"
        "assert(document.querySelectorAll('p::before').length===0&&document.querySelectorAll('input[type=radio]').length===2,'pseudo-elements, attributes');"
        "throws('SyntaxError',()=>document.querySelector('p['));throws('SyntaxError',()=>box.querySelector('> p'));throws('SyntaxError',()=>document.querySelectorAll(''));"
        "try{document.querySelector('p[')}catch(e){assert(e.message.includes(\"'p['\"),'the message names the selector')}"
        "throws('TypeError',()=>document.querySelector());"
        "throws('TypeError',()=>document.querySelector.call(document.createTextNode('x'),'p'));"
        "const fr=document.createDocumentFragment();fr.appendChild(document.createElement('i'));"
        "assert(fr.querySelector('i')!==null&&fr.querySelectorAll('*').length===1,'fragment');"
        "assert([...document.getElementsByTagName('p')].length===3,'a collection iterates');"
        "let n=0;for(const c of box.childNodes)n++;assert(n===3,'childNodes iterates');",
        "selectors.js");
    fixture_free(&f);
}
/* document.cookie: with no provider the document is cookie-averse; with
 * one, a read is what it writes and an assignment is handed to it whole. */
static char cookie_heard[128];
static size_t cookie_get(void *opaque, char *out, size_t cap)
{
    (void)opaque;
    return (size_t)snprintf(out, cap, "a=1; b=2");
}
static void cookie_set(void *opaque, const char *text, size_t length)
{
    (void)opaque;
    snprintf(cookie_heard, sizeof(cookie_heard), "%.*s", (int)length, text);
}
static void dom_cookie_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT "assert(document.cookie===''&&'cookie' in document);document.cookie='x=1';"
        "assert(document.cookie==='','a cookie-averse document');"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(Object.getPrototypeOf(document),'cookie').get.call(document.body))",
        "cookie-averse.js");
    os64_dom_cookies_t cookies={cookie_get,cookie_set,NULL};
    os64_dom_set_cookies(f.binding,&cookies);
    cookie_heard[0]=0;
    dom_script(&f,DOM_ASSERT "assert(document.cookie==='a=1; b=2','read');document.cookie='c=3; path=/';",
        "cookie-provider.js");
    check(strcmp(cookie_heard,"c=3; path=/")==0,"document.cookie: an assignment reaches the provider whole");
    fixture_free(&f);
}
/* The classic methods pages assume (LIBDOM.md § The classic methods). */
static void dom_classic_method_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "const box=document.getElementById('box'),one=document.getElementById('one'),two=document.getElementById('two');"
        /* class and name queries: live */
        "one.className='a  b';two.className='b';const bs=document.getElementsByClassName('b'),ab=box.getElementsByClassName(' b\\ta ');"
        "assert(bs.length===2&&ab.length===1&&ab[0]===one,'class query');two.className='a b';assert(ab.length===2,'class query is live');"
        "assert(document.getElementsByClassName('').length===0&&document.getElementsByClassName('B').length===0,'empty and case');"
        "const named=document.getElementsByName('g');assert(named.length===2&&named[0].id==='r1','name query');"
        "throws('TypeError',()=>box.getElementsByName('g'));"
        /* matches / closest */
        "assert(one.matches('#box > p.a')&&!one.matches('div')&&one.matches(':scope'),'matches');"
        "assert(one.closest('div')===box&&one.closest('p')===one&&one.closest('form')===null,'closest');"
        "throws('SyntaxError',()=>one.matches('p['));"
        /* contains, isConnected */
        "assert(box.contains(one)&&box.contains(box)&&document.contains(one)&&document.body.contains(one)&&!one.contains(box)&&!box.contains(null),'contains');"
        "const loose=document.createElement('i');assert(one.isConnected&&!loose.isConnected,'isConnected');"
        /* append / prepend / before / after / replaceWith / remove / replaceChildren */
        "const x=document.createElement('x'),y=document.createElement('y');"
        "box.append(x,'tail');assert(box.lastChild.nodeType===3&&box.lastChild.data==='tail'&&box.lastChild.previousSibling===x,'append');"
        "box.prepend('head',y);assert(box.firstChild.data==='head'&&box.firstChild.nextSibling===y,'prepend');"
        "one.before(x);assert(one.previousSibling===x,'before');one.after('after');assert(one.nextSibling.data==='after','after');"
        "one.before(one.previousSibling,one);assert(one.previousSibling===x,'before skips the nodes it moves');"
        "y.replaceWith('was y',loose);assert(box.childNodes[1].data==='was y'&&box.childNodes[2]===loose&&!y.isConnected,'replaceWith');"
        "loose.remove();assert(!loose.isConnected&&loose.parentNode===null,'remove');loose.remove();"
        "const kids=document.createElement('div');kids.append('a','b');kids.replaceChildren(x);assert(kids.childNodes.length===1&&kids.firstChild===x,'replaceChildren');"
        "kids.replaceChildren();assert(!kids.hasChildNodes(),'replaceChildren empties');box.append(x);kids.replaceChildren(x,'t');assert(kids.firstChild===x&&kids.childNodes.length===2,'replaceChildren moves');const doc2=document.createDocumentFragment();doc2.append('q');doc2.replaceChildren(y);assert(doc2.firstChild===y,'fragment replaceChildren');"
        "throws('HierarchyRequestError',()=>one.append(box));"
        /* insertAdjacent* */
        "const host=document.createElement('section');box.append(host);host.innerHTML='<b>in</b>';"
        "host.insertAdjacentHTML('afterbegin','<i>first</i>');host.insertAdjacentHTML('beforeEnd','<u>last</u>');"
        "host.insertAdjacentHTML('beforebegin','<s>before</s>');host.insertAdjacentHTML('afterend','<em>after</em>');"
        "assert(host.innerHTML==='<i>first</i><b>in</b><u>last</u>'&&host.previousSibling.tagName==='S'&&host.nextSibling.tagName==='EM','insertAdjacentHTML');"
        "assert(host.insertAdjacentElement('afterbegin',x)===x&&host.firstChild===x,'insertAdjacentElement');"
        "host.insertAdjacentText('beforeend','!');assert(host.lastChild.data==='!','insertAdjacentText');"
        "assert(loose.insertAdjacentElement('afterend',x)===null,'nowhere beside a loose node');"
        "throws('NoModificationAllowedError',()=>loose.insertAdjacentHTML('afterend','<p>'));"
        "throws('SyntaxError',()=>host.insertAdjacentHTML('middle','x'));"
        "host.insertAdjacentHTML('beforeend','<script>globalThis.ranAdjacent=1</scr'+'ipt>');assert(globalThis.ranAdjacent===undefined,'its scripts never run');"
        /* attribute conveniences */
        "assert(one.getAttributeNames().join()==='id,class'&&one.hasAttributes()&&!document.createElement('p').hasAttributes(),'names');"
        "assert(one.toggleAttribute('data-on')===true&&one.hasAttribute('data-on')&&one.toggleAttribute('data-on')===false&&!one.hasAttribute('data-on'),'toggle');"
        "assert(one.toggleAttribute('data-on',false)===false&&one.toggleAttribute('data-on',true)===true&&one.toggleAttribute('data-on',true)===true,'toggle force');"
        "throws('InvalidCharacterError',()=>one.toggleAttribute('a b'));"
        /* reflections */
        "assert(one.hidden===false);one.hidden=true;assert(one.getAttribute('hidden')==='');one.hidden=false;assert(!one.hasAttribute('hidden'),'hidden');"
        "one.lang='fr';assert(one.lang==='fr'&&one.getAttribute('lang')==='fr','lang');"
        "one.setAttribute('dir','RTL');assert(one.dir==='rtl');one.dir='sideways';assert(one.dir===''&&one.getAttribute('dir')==='sideways','dir');"
        "assert(one.tabIndex===-1&&document.getElementById('field').tabIndex===0,'tabIndex defaults');one.tabIndex=3;assert(one.tabIndex===3&&one.getAttribute('tabindex')==='3','tabIndex');"
        "const s=document.createElement('script');s.src='/js/app.js';assert(s.getAttribute('src')==='/js/app.js'&&s.src.indexOf('/js/app.js')>=0,'script.src');"
        "const a=document.createElement('a');assert(a.href==='');a.href='page.html';assert(/page\\.html$/.test(a.href)&&a.getAttribute('href')==='page.html','a.href');"
        "assert(!('src' in one)&&!('href' in one)&&!('href' in s),'reflections only where they belong');"
        /* classList */
        "const c=one.classList;assert(c===one.classList&&c.length===2&&c[0]==='a'&&c.item(1)==='b'&&c.item(5)===null,'classList reads');"
        "c.add('c','a');assert(one.className==='a b c','add');c.remove('a','zz');assert(one.className==='b c','remove');"
        "assert(c.toggle('d')===true&&c.contains('d')&&c.toggle('d')===false&&c.toggle('b',true)===true,'toggle');"
        "assert(c.replace('b','z')===true&&one.className==='z c'&&c.replace('nope','q')===false,'replace');"
        "assert([...c].join()==='z,c'&&String(c)==='z c'&&c.value==='z c','iterate, value');"
        "throws('SyntaxError',()=>c.add(''));throws('InvalidCharacterError',()=>c.add('a b'));"
        "one.classList='p q';assert(one.className==='p q','PutForwards');"
        "const twice=document.createElement('p');twice.className='a a b';assert(twice.classList.length===2&&twice.classList[1]==='b','a token counts once');"
        "const bare=document.createElement('p');bare.classList.remove('x');bare.classList.add();assert(!bare.hasAttribute('class'),'no attribute for an empty set');"
        /* dataset */
        "one.setAttribute('data-foo-bar','1');const d=one.dataset;assert(d===one.dataset&&d.fooBar==='1','dataset reads');"
        "d.someName='v';assert(one.getAttribute('data-some-name')==='v','dataset sets');"
        "assert(Object.keys(d).join()==='on,fooBar,someName'&&'fooBar' in d,'dataset keys');"
        "delete d.fooBar;assert(!one.hasAttribute('data-foo-bar')&&d.fooBar===undefined,'dataset deletes');"
        "throws('SyntaxError',()=>{d['a-b']='x'});d[0]='zero';assert(one.getAttribute('data-0')==='zero','numeric key');",
        "classic-methods.js");
    /* Quinn #243: arguments are converted before the tree is read, a lone
     * replacement is judged before it moves, and insertAdjacentText's data
     * is text whatever it is. */
    dom_script(&f,DOM_ASSERT
        "const holder=document.createElement('div');document.body.append(holder);holder.innerHTML='<i></i><b id=victim></b>';"
        "const victim=document.getElementById('victim');"
        "victim.before({toString(){holder.textContent='';return 'text'}});"
        "assert(victim.parentNode===null&&holder.childNodes.length===0,'a conversion that empties the parent leaves nothing to insert beside');"
        "const kept=document.createElement('em');holder.append(kept);"
        "try{document.body.append(kept,{toString(){throw Error('coercion')}})}catch(e){assert(e.message==='coercion')}"
        "assert(kept.parentNode===holder,'a failed conversion moves nothing');"
        "const outer=document.createElement('div'),inner=document.createElement('p');outer.append(inner);document.body.append(outer);"
        "throws('HierarchyRequestError',()=>inner.replaceChildren(outer));"
        "assert(outer.parentNode===document.body&&inner.parentNode===outer,'a refused replacement leaves its node in place');"
        "const source=document.createElement('div'),moved=document.createElement('span');source.append(moved);"
        "moved.toString=()=>'literal text';holder.insertAdjacentText('beforeend',moved);"
        "assert(moved.parentNode===source&&holder.lastChild.nodeType===3&&holder.lastChild.data==='literal text','insertAdjacentText stringifies');"
        "holder.insertAdjacentHTML({toString(){holder.textContent='';return 'beforeend'}},'<u>late</u>');"
        "assert(holder.firstChild.tagName==='U','positions are read after the arguments are converted');"
        "const before=document.childNodes.length;throws('HierarchyRequestError',()=>document.replaceChildren(document.createElement('html')));"
        "assert(document.childNodes.length===before&&document.documentElement!==null,'a document keeps its children');",
        "conversion-order.js");
    fixture_free(&f);
}
static void dom_unicode_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "const e=document.createElement('div');e.id='unicode';document.body.appendChild(e);"
        "e.textContent='A\\0B\\ud800C\\udfffD\\ud83d\\ude00';"
        "assert(e.textContent==='A\\ufffdB\\ufffdC\\ufffdD\\ud83d\\ude00');"
        "e.setAttribute('data-x','\\0\\ud800');assert(e.getAttribute('data-x')==='\\ufffd\\ufffd');"
        "const t=document.createTextNode('\\ud800');assert(t.data==='\\ufffd');"
        "const c=document.createComment('\\udfff');assert(c.data==='\\ufffd');"
        "e.innerHTML='<b>\\0\\ud800</b>';assert(e.firstChild.textContent==='\\ufffd\\ufffd');"
        "throws('TypeError',()=>e.textContent=Symbol('x'));"
        "let called=0;throws('Error',()=>e.setAttribute('x',{toString(){called++;throw Error('conversion')}}));assert(called===1&&!e.hasAttribute('x'));",
        "converted-unicode.js");
    os64_html_node_t *e=native_id(f.document->document,"unicode");
    check(e&&e->first_child&&e->first_child->first_child&&
        !strcmp(e->first_child->first_child->text,"\xef\xbf\xbd\xef\xbf\xbd"),"native content stores one replacement per NUL/lone surrogate");
    fixture_free(&f);
}
static void dom_control_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    os64_page_t *old=os64_page_build(f.document,"https://fixture.test/",NULL,f.state);
    check(old!=NULL,"control fixture model");
    uint64_t tree=os64_html_version(f.document),state=os64_page_state_version(f.state);
    dom_script(&f,DOM_ASSERT
        "globalThis.field=document.getElementById('field');field.value='typed';"
        "assert(field.value==='typed');document.getElementById('check').checked=true;"
        "document.getElementById('select').selectedIndex=1;assert(document.getElementById('select').selectedIndex===1);"
        "const never=document.createElement('input');never.value='detached';assert(never.value==='detached');"
        "document.getElementById('r2').checked=true;assert(!document.getElementById('r1').checked&&document.getElementById('r2').checked);",
        "control-properties.js");
    check(os64_html_version(f.document)==tree,"dirty control properties leave native tree version alone");
    check(os64_page_state_version(f.state)>state,"dirty control properties move state revision");
    const char *value=NULL;size_t length=0;
    os64_html_node_t *field=native_id(f.document->document,"field");
    check(!os64_page_node_value(f.state,field,&value,&length)&&length==5&&!memcmp(value,"typed",5),"native state sees script value");
    dom_script(&f,DOM_ASSERT
        "field.setAttribute('type','hidden');field.setAttribute('type','text');assert(field.value==='typed'&&field.getAttribute('value')==='typed','dirty text hidden transfer');"
        "field.value='secret';field.setAttribute('type','file');assert(field.value==='','entering file clears current value');throws('InvalidStateError',()=>field.value='forbidden');field.setAttribute('type','text');assert(field.value==='typed','file exit restores value content attribute');"
        "field.value='kept';const parent=field.parentNode;parent.removeChild(field);parent.appendChild(field);assert(field.value==='kept','detach preserves dirty value');"
        "field.setAttribute('readonly','');field.setAttribute('disabled','');field.value='script';assert(field.value==='script','disabled readonly script setter');"
        "field.removeAttribute('readonly');field.removeAttribute('disabled');",
        "control-attribute-transitions.js");
    dom_script(&f,DOM_ASSERT
        "const r2=document.getElementById('r2'),form=document.getElementById('form');r2.checked=true;"
        "const other=document.createElement('form'),moved=document.createElement('input');"
        "moved.setAttribute('type','radio');moved.setAttribute('name','g');other.appendChild(moved);moved.checked=true;"
        "form.appendChild(moved);assert(moved.checked&&!r2.checked);"
        "other.appendChild(moved);r2.checked=true;form.appendChild(moved);other.appendChild(moved);"
        "assert(!r2.checked&&moved.checked);"
        "const sel=document.getElementById('select'),option=document.getElementById('o2');"
        "sel.selectedIndex=1;sel.setAttribute('multiple','');sel.removeAttribute('multiple');assert(sel.selectedIndex===1);"
        "sel.removeChild(option);assert(sel.selectedIndex===0);sel.appendChild(option);assert(sel.selectedIndex===1);",
        "control-structure-history.js");
    os64_page_t *fresh=old?os64_page_rebuild(old):NULL;
    check(!old||fresh!=NULL,"native rebuild accepts script control changes");
    check(!os64_page_node_value(f.state,field,&value,&length)&&length==6&&!memcmp(value,"script",6),"control value survives native rebuild");
    const os64_page_control_t *old_field=old?os64_page_control(old,os64_page_control_for(old,field)):NULL;
    const os64_page_control_t *new_field=fresh?os64_page_control(fresh,os64_page_control_for(fresh,field)):NULL;
    check(old_field&&new_field&&old_field->value_len==6&&new_field->value_len==6&&
              !memcmp(old_field->value,"script",6)&&!memcmp(new_field->value,"script",6),
          "simultaneous models safely publish current script values");
    os64_page_free(fresh);os64_page_free(old);fixture_free(&f);
}
static void dom_refusal_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    char *before=native_serialized(f.document->document);
    uint64_t version=os64_html_version(f.document),state=os64_page_state_version(f.state);
    dom_script(&f,DOM_ASSERT
        "const box=document.getElementById('box'),one=document.getElementById('one');"
        "throws('HierarchyRequestError',()=>one.appendChild(box));"
        "throws('NotFoundError',()=>box.removeChild(document.body));"
        "throws('NotFoundError',()=>box.insertBefore(document.createElement('b'),document.body));"
        "throws('InvalidCharacterError',()=>document.createElement('bad name'));"
        "throws('InvalidCharacterError',()=>box.setAttribute('bad name','x'));throws('TypeError',()=>box.setAttribute('bad name',Symbol('x')));throws('Error',()=>box.setAttribute('bad name',{toString(){throw Error('converted')}}));let conversions=0;throws('InvalidCharacterError',()=>box.setAttribute('bad name',{toString(){conversions++;return 'x'}}));assert(conversions===1);"
        "throws('TypeError',()=>box.appendChild({}));"
        "throws('TypeError',()=>box.appendChild.call({},one));"
        "throws('TypeError',()=>box.getAttribute.call({},'id'));"
        "assert(box.getAttribute('')===null&&!box.hasAttribute(''));box.removeAttribute('');box.removeAttribute('bad name');assert(box.parentNode===document.body&&one.parentNode===box);",
        "refusals.js");
    char *after=native_serialized(f.document->document);
    check(!strcmp(before,after)&&os64_html_version(f.document)==version&&os64_page_state_version(f.state)==state,
          "refused calls preserve native tree and both versions");
    free(before);free(after);fixture_free(&f);
}

static void dom_exception_ownership_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "let intercepted=0;const names=['name','message'];const saved=names.map(n=>Object.getOwnPropertyDescriptor(Error.prototype,n));"
        "try {for(const n of names)Object.defineProperty(Error.prototype,n,{configurable:true,set(){intercepted++;throw TypeError('intercepted')}});"
        "const box=document.getElementById('box'),one=document.getElementById('one');"
        "for(const [expected,action] of [['HierarchyRequestError',()=>one.appendChild(box)],['NotFoundError',()=>box.removeChild(document.body)]]){"
        "try{action();assert(false,'missing exception')}catch(e){assert(e.name===expected);assert(Object.hasOwn(e,'name')&&Object.hasOwn(e,'message'));assert(!Object.getOwnPropertyDescriptor(e,'name').enumerable);}}"
        "assert(intercepted===0);}finally{for(let i=0;i<names.length;i++){if(saved[i])Object.defineProperty(Error.prototype,names[i],saved[i]);else delete Error.prototype[names[i]];}}",
        "native-error-own-properties.js");fixture_free(&f);
}
static void dom_fragment_mode_cases(void)
{
    for(int mode=0;mode<2;mode++){
        os64_dom_options_t options=os64_dom_default_options();options.scripting=mode!=0;
        DomFixture f=fixture_new(&options);if(!fixture_ready(&f))continue;
        const char *source=mode?DOM_ASSERT "const e=document.getElementById('box');e.innerHTML='<noscript><b>x</b></noscript>';assert(e.firstChild.firstChild.nodeType===3);":
            DOM_ASSERT "const e=document.getElementById('box');e.innerHTML='<noscript><b>x</b></noscript>';assert(e.firstChild.firstChild.nodeType===1);";
        dom_script(&f,source,"binding-fragment-scripting-mode.js");
        dom_script(&f,DOM_ASSERT "globalThis.executed=false;document.getElementById('box').innerHTML='<script>executed=true</script>';assert(!executed);","inserted-script-inert.js");
        fixture_free(&f);
    }
}
static void dom_extra_surface_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "const box=document.getElementById('box'),one=document.getElementById('one'),two=document.getElementById('two');"
        "assert(one.ownerDocument===document&&one.parentElement===box&&document.ownerDocument===null);"
        "assert(box.children===box.children&&box.childElementCount===2&&box.firstElementChild===one&&box.lastElementChild===two);"
        "box.insertBefore(document.createTextNode('between'),two);assert(one.nextElementSibling===two&&two.previousElementSibling===one);"
        "assert(box.childNodes.length===3&&box.children.length===2&&box.children[1]===two);"
        "assert(document.firstChild.nodeType===10&&document.firstChild.textContent===null&&document.firstChild.nodeValue===null);"
        "assert(document.textContent===null&&box.nodeValue===null);box.nodeValue='ignored';assert(box.nodeValue===null);"
        "const unicode=document.createElement('é');assert(unicode.tagName==='é');unicode.setAttribute('é','yes');assert(unicode.getAttribute('é')==='yes');assert(document.createElement('a=').tagName==='A=');assert(document.createElement('a!').tagName==='A!');const astral='\\ud83d\\ude00';assert(document.createElement(astral).tagName===astral);throws('InvalidCharacterError',()=>document.createElement('1x'));throws('InvalidCharacterError',()=>document.createElement(':x!'));unicode.setAttribute('1x','v');unicode.setAttribute('a!','yes');assert(unicode.getAttribute('1x')==='v'&&unicode.getAttribute('a!')==='yes');for(const invalid of ['=','a/b','bad name'])throws('InvalidCharacterError',()=>unicode.setAttribute(invalid,'x'));const text=document.createTextNode('x');text.nodeValue=undefined;assert(text.data==='');text.textContent=undefined;assert(text.data==='');text.data=undefined;assert(text.data==='undefined');const nullable=document.createElement('div');nullable.textContent=undefined;assert(nullable.childNodes.length===0);nullable.innerHTML=undefined;assert(nullable.textContent==='undefined');"
        "let calls=0;prompt('x',{toString(){calls++;return 'default'}});assert(calls===1);"
        "const body=document.body;body.parentNode.removeChild(body);assert(document.body===null);document.documentElement.appendChild(body);assert(document.body===body);"
        "const frameset=document.createElement('frameset');document.documentElement.replaceChild(frameset,body);assert(document.body===frameset);"
        "assert(document.body===frameset);",
        "extra-surface.js");
    uint64_t tree=os64_html_version(f.document),state=os64_page_state_version(f.state);
    char *before=native_serialized(f.document->document);
    dom_script(&f,DOM_ASSERT "const div=document.createElement('div');const root=document.documentElement;throws('HierarchyRequestError',()=>document.replaceChild(div,root));assert(document.documentElement===root&&document.body===frameset);","document-root-refusal.js");
    char *after=native_serialized(f.document->document);
    check(!strcmp(before,after)&&tree==os64_html_version(f.document)&&state==os64_page_state_version(f.state),"document root refusal preserves tree and revisions");
    free(before);free(after);fixture_free(&f);
}

static void dom_review_property_cases(void)
{
    DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))return;
    dom_script(&f,DOM_ASSERT
        "const body=document.body,text=document.createTextNode('before'),comment=document.createComment('before');"
        "text.data=null;comment.data=null;assert(text.data===''&&comment.data==='','CharacterData null clears');"
        "text.data=undefined;comment.data=undefined;assert(text.data==='undefined'&&comment.data==='undefined','data undefined stringifies');"
        "assert(body.value===undefined&&body.checked===undefined&&body.data===undefined&&body.head===undefined);"
        "assert(text.tagName===undefined&&text.value===undefined&&text.innerHTML===undefined&&document.tagName===undefined);"
        "assert(!('value' in body)&&!('checked' in body)&&!('data' in body)&&!('head' in body)&&!('tagName' in text));"
        "assert('head' in document&&'tagName' in body&&'data' in text&&'nodeName' in text);"
        "const input=document.createElement('input'),area=document.createElement('textarea'),select=document.createElement('select'),button=document.createElement('button');"
        "assert('value' in input&&'value' in area&&'value' in select&&'value' in button);"
        "assert('checked' in input&&!('checked' in area)&&'selectedIndex' in select&&!('selectedIndex' in input));"
        "assert(Object.getPrototypeOf(Object.getPrototypeOf(input))===Object.getPrototypeOf(body));"
        "assert(Object.getPrototypeOf(text)===Object.getPrototypeOf(comment));"
        "assert(Object.getPrototypeOf(input.cloneNode())===Object.getPrototypeOf(input));"
        "const fragment=document.createDocumentFragment();assert('children' in fragment&&'getElementById' in fragment&&!('head' in fragment));"
        "const getter=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(document),'head').get;"
        "throws('TypeError',()=>getter.call(body));throws('TypeError',()=>body.getAttribute.call(text,'id'));"
        "body.value='expando';assert(body.value==='expando'&&document.body===body);",
        "review-property-prototypes.js");
    fixture_free(&f);
}
static void dom_budget_cases(void)
{
    check(os64_dom_bytes(NULL)==0&&os64_dom_registry_count(NULL)==0,"NULL statistics");
    os64_dom_drain(NULL);os64_dom_free(NULL);
    DomFixture mismatch=fixture_base(0,0,0),foreign=fixture_base(0,0,0);os64_js_outcome_t invalid;
    check(!os64_dom_create(mismatch.runtime,mismatch.document,foreign.state,NULL,&invalid)&&invalid.status==OS64_JS_BAD_ARGUMENT,
          "foreign control-state installation refused");
    check(!os64_dom_create(mismatch.runtime,mismatch.document,mismatch.state,NULL,NULL),"missing required outcome refused");
    /* Both fixtures must be retired before global allocator accounting is empty. */
    os64_js_destroy(foreign.runtime);os64_page_state_free(foreign.state);os64_html_document_free(foreign.document);memset(&foreign,0,sizeof(foreign));
    fixture_free(&mismatch);
    DomFixture zero=fixture_new(&(os64_dom_options_t){0});
    check(!zero.binding,"explicit zero native quota refuses installation");fixture_free(&zero);
    for(int mode=0;mode<3;mode++){
        DomFixture f=fixture_base(mode==1?65536:0,mode==2?4096:0,0);
        os64_dom_options_t options=os64_dom_default_options();
        if(mode==0) options.max_bytes=65536;
        os64_js_outcome_t outcome;
        f.binding=os64_dom_create(f.runtime,f.document,f.state,&options,&outcome);
        if(!fixture_ready(&f))continue;
        const char *source=mode==0?DOM_ASSERT "throws('QuotaExceededError',()=>document.body.textContent='x'.repeat(100000));assert(document.getElementById('one').textContent==='one');":
            mode==1?DOM_ASSERT "throws('QuotaExceededError',()=>document.body.innerHTML='<p>'+ 'x'.repeat(100000)+'</p>');assert(document.getElementById('one').textContent==='one');":
            DOM_ASSERT "throws('QuotaExceededError',()=>document.getElementById('field').value='x'.repeat(10000));assert(document.getElementById('field').value==='default');";
        char *before=native_serialized(f.document->document);uint64_t version=os64_html_version(f.document);
        dom_script(&f,DOM_ASSERT "globalThis.intercepted=0;for(const n of ['name','message'])Object.defineProperty(Error.prototype,n,{configurable:true,set(){intercepted++;throw TypeError('intercepted')}});","quota-error-prototype-setup.js");
        dom_script(&f,source,"native-budget-catch.js");
        dom_script(&f,DOM_ASSERT "assert(intercepted===0)","quota-error-prototype-uninvoked.js");
        char *after=native_serialized(f.document->document);
        check(!strcmp(before,after)&&version==os64_html_version(f.document),"native quota preserves visible tree/version");
        free(before);free(after);fixture_free(&f);
    }
    DomFixture f=fixture_base(0,0,1024*1024);os64_js_outcome_t outcome;
    f.binding=os64_dom_create(f.runtime,f.document,f.state,NULL,&outcome);
    if(!fixture_ready(&f))return;
    const char *source="try { let list=[];for(let i=0;i<100000;i++)list.push(document.createElement('div')); } catch(e) {}";
    check(os64_js_run(f.runtime,source,strlen(source),"engine-limit.js",&outcome)==OS64_JS_LIMIT&&outcome.limit==OS64_JS_LIMIT_MEMORY,
          "caught engine exhaustion remains sticky memory LIMIT");
    check(os64_js_run(f.runtime,"1",1,"after-limit.js",&outcome)==OS64_JS_FAILED_RUNTIME,"sticky failed runtime refuses subsequent evaluation");
    fixture_free(&f);
}
static void dom_allocation_cases(void)
{
    DomFixture baseline=fixture_base(0,0,0);os64_js_outcome_t outcome;
    size_t start=dom_attempts;
    baseline.binding=os64_dom_create(baseline.runtime,baseline.document,baseline.state,NULL,&outcome);
    size_t count=dom_attempts-start;
    check(baseline.binding&&count>0,"measure binding construction native allocations");printf("Native installer allocation points: %zu\n",count);fixture_free(&baseline);
    for(size_t cut=1;cut<=count;cut++){
        DomFixture f=fixture_base(0,0,0);size_t target=dom_attempts+cut;dom_fail_at=target;
        f.binding=os64_dom_create(f.runtime,f.document,f.state,NULL,&outcome);
        dom_fail_at=0;dom_sweeps++;check(dom_attempts>=target,"native installer injection point reached");
        check(!f.binding&&outcome.status!=OS64_JS_OK,"each native construction allocation refusal reports failure");
        os64_js_outcome_t access;JSContext *ctx=os64_js_context(f.runtime,OS64_JS_ABI_ID,&access);
        JSValue pending=JS_GetException(ctx);check(JS_IsNull(pending)||JS_VALUE_GET_TAG(pending)==JS_TAG_UNINITIALIZED,"failed installation consumes pending exception");JS_FreeValue(ctx,pending);
        fixture_free(&f);
    }
    baseline=fixture_base(0,0,0);start=attempts;
    baseline.binding=os64_dom_create(baseline.runtime,baseline.document,baseline.state,NULL,&outcome);
    count=attempts-start;check(baseline.binding&&count>0,"measure full installer allocation points");printf("Full installer engine/native allocation points: %zu\n",count);fixture_free(&baseline);
    for(size_t cut=1;cut<=count;cut++){
        DomFixture f=fixture_base(0,0,0);size_t target=attempts+cut;fail_at=target;
        f.binding=os64_dom_create(f.runtime,f.document,f.state,NULL,&outcome);fail_at=0;dom_sweeps++;check(attempts>=target,"full installer injection point reached");
        if(f.binding)fprintf(stderr,"Installer allocation cut %zu/%zu succeeded safely (attempts after cut: %zu)\n",cut,count,attempts);
        check(f.binding?outcome.status==OS64_JS_OK:outcome.status!=OS64_JS_OK,"every engine/native installer allocation failure has consistent outcome");
        os64_js_outcome_t access;JSContext *ctx=os64_js_context(f.runtime,OS64_JS_ABI_ID,&access);
        JSValue pending=JS_GetException(ctx);check(JS_IsNull(pending)||JS_VALUE_GET_TAG(pending)==JS_TAG_UNINITIALIZED,"engine installer failure consumes pending exception");JS_FreeValue(ctx,pending);
        fixture_free(&f);
    }
    const char *operations[]={"document.body.textContent='replacement'","document.getElementById('box').innerHTML='<b>replacement</b>'",
        "document.body.appendChild(document.createElement('aside'))","document.getElementsByTagName('p').item(1)","document.getElementById('box').setAttribute('data-test','replacement')",
        "document.getElementById('field').value='replacement'","document.getElementById('field').cloneNode(false)",
        "document.body.addEventListener('click',()=>{},{once:true})","setTimeout(()=>{},5,1,2)",
        "document.body.setAttribute('onclick','1')","document.body.dispatchEvent(new Event('x'))",
        "location.href='http://example.test/x'", "new Image()",
        "document.body.style.width='20px'", "document.forms.namedItem('missing')",
        "document.querySelectorAll('#box p').item(1)",
        "try{document.querySelector('p[')}catch(e){if(e.name!=='SyntaxError')throw e}",
        "document.getElementsByClassName('old').length", "document.getElementById('box').append('a',document.createElement('i'))",
        "document.getElementById('box').insertAdjacentHTML('beforeend','<b>x</b>')", "document.getElementById('box').classList.add('n')",
        "document.getElementById('box').dataset.someKey='v'", "document.getElementById('box').closest('body')"};
    for(size_t op=0;op<sizeof(operations)/sizeof(*operations);op++){
        DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))continue;
        start=dom_attempts;dom_script(&f,operations[op],"allocation-baseline.js");count=dom_attempts-start;printf("Native operation %zu allocation points: %zu\n",op,count);fixture_free(&f);
        for(size_t cut=1;cut<=count;cut++){
            f=fixture_new(NULL);if(!fixture_ready(&f))break;
            char source[1024];snprintf(source,sizeof(source),"try{%s;globalThis.refused=false}catch(e){if(e.name!=='QuotaExceededError')throw e;globalThis.refused=true}",operations[op]);
            char *before=native_serialized(f.document->document);uint64_t version=os64_html_version(f.document),state_version=os64_page_state_version(f.state);
            size_t target=dom_attempts+cut;dom_fail_at=target;int ok=dom_script(&f,source,"binding-allocation-cut.js");dom_fail_at=0;dom_sweeps++;check(dom_attempts>=target,"operation injection point reached");
            if(ok){dom_script(&f,DOM_ASSERT "assert(refused)","allocation-caught.js");}
            char *after=native_serialized(f.document->document);
            check(!strcmp(before,after)&&version==os64_html_version(f.document)&&state_version==os64_page_state_version(f.state),"binding failure preflights before visible mutation");
            free(before);free(after);fixture_free(&f);
        }
    }
}
static void dom_clone_control_cases(void)
{
    const char *sources[]={
        DOM_ASSERT "const f=document.getElementById('field');f.value='typed';const c=f.cloneNode(false);assert(c.value==='typed','input clone expected typed got '+JSON.stringify(c.value));",
        DOM_ASSERT "const f=document.getElementById('field');f.value='typed';const c=f.cloneNode(false);c.setAttribute('value','changed-default');assert(c.value==='typed','input clone dirty flag expected typed got '+JSON.stringify(c.value));",
        DOM_ASSERT "const f=document.getElementById('check');f.checked=true;const c=f.cloneNode(false);assert(c.checked===true,'checkbox clone expected true got '+c.checked);",
        DOM_ASSERT "const f=document.getElementById('check');f.setAttribute('checked','');f.checked=false;const c=f.cloneNode(false);c.removeAttribute('checked');c.setAttribute('checked','');assert(c.checked===false,'checkbox clone dirty flag expected false got '+c.checked);",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='default-area';f.value='typed-area';const c=f.cloneNode(false);assert(c.value==='typed-area','shallow textarea clone expected typed-area got '+JSON.stringify(c.value));",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='default-area';f.value='typed-area';const c=f.cloneNode(true);assert(c.value==='typed-area','deep textarea clone expected typed-area got '+JSON.stringify(c.value));",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='default-area';f.value='typed-area';const c=f.cloneNode(true);c.textContent='changed-default';assert(c.value==='typed-area','textarea clone dirty flag expected typed-area got '+JSON.stringify(c.value));",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='clean-area';const c=f.cloneNode(false);assert(c.childNodes.length===0&&c.value==='clean-area','clean shallow clone keeps raw value');c.textContent='';assert(c.value==='clean-area','empty replace-all no-op keeps clean override');document.getElementById('two').textContent='unrelated';c.setAttribute('title','unrelated');assert(c.value==='clean-area','unrelated mutations preserve clean raw value');",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='clean-area';const c=f.cloneNode(true);assert(c.value==='clean-area');c.firstChild.data='changed';assert(c.value==='changed','clean deep clone follows own CharacterData');",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='clean-area';const c=f.cloneNode(false);const child=document.createTextNode('temporary');c.appendChild(child);c.removeChild(child);assert(c.value==='','child append/remove history clears clean override before getter');",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='clean-area';const c=f.cloneNode(false);c.textContent='temporary';c.textContent='';assert(c.value==='','replace children history clears clean override before getter');",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='clean-area';const c=f.cloneNode(false);c.value='assigned';c.appendChild(document.createTextNode('markup'));c.firstChild.nodeValue='changed-markup';assert(c.value==='assigned','script assignment dirties clean clone');",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='clean-area';const c=f.cloneNode(false);const again=c.cloneNode(true);assert(again.childNodes.length===0&&again.value==='clean-area','clean raw override survives another deep clone');",
        DOM_ASSERT "const first=document.getElementById('r1'),last=document.getElementById('r2');last.setAttribute('checked','');assert(!first.checked&&last.checked);const c=first.cloneNode(false);assert(!c.checked&&c.hasAttribute('checked'),'clone copies effective false radio');c.removeAttribute('checked');c.setAttribute('checked','');assert(c.checked,'effective false radio clone remains clean');",
        DOM_ASSERT "const host=document.createElement('div'),input=document.createElement('input'),area=document.createElement('textarea'),tick=document.createElement('input');input.setAttribute('value','default');input.value='deep-typed';area.textContent='default-area';area.value='deep-area';tick.setAttribute('type','checkbox');tick.checked=true;host.appendChild(input);host.appendChild(area);host.appendChild(tick);const copy=host.cloneNode(true);assert(copy.childNodes.length===3&&copy.childNodes[0].value==='deep-typed'&&copy.childNodes[1].value==='deep-area'&&copy.childNodes[2].checked,'deep subtree copies descendant controls');",
        DOM_ASSERT "const f=document.getElementById('field');const c=f.cloneNode(false);assert(c.value==='default','clone works before source default getter');c.setAttribute('value','new-default');assert(c.value==='new-default','clean input clone stays clean');",
        DOM_ASSERT "const f=document.createElement('textarea');f.textContent='clean-area';const c=f.cloneNode(false);c.value='clean-area';c.appendChild(document.createTextNode('changed'));assert(c.value==='clean-area','equal assignment still dirties clean clone');",
        DOM_ASSERT "const f=document.createElement('textarea');f.appendChild(document.createTextNode('direct'));const nested=document.createElement('b');nested.textContent='nested';f.appendChild(nested);assert(f.value==='direct','textarea default includes direct Text children only');const c=f.cloneNode(true);assert(c.value==='direct');c.lastChild.firstChild.data='changed-nested';assert(c.value==='direct','nested CharacterData does not notify ancestor textarea');c.appendChild(document.createTextNode('-tail'));assert(c.value==='direct-tail','direct insertion updates clean textarea');",
        DOM_ASSERT "const f=document.createElement('textarea');f.appendChild(document.createTextNode('a\\r\\nb'));const c=f.cloneNode(true);assert(c.value==='a\\nb');c.firstChild.data='a\\nb';c.firstChild.data='a\\r\\nb';assert(c.value==='a\\nb','raw normalized-equal CharacterData change and restore remains coherent');",
        DOM_ASSERT "const f=document.getElementById('select');f.selectedIndex=1;const c=f.cloneNode(true);assert(c.selectedIndex===0,'select clone uses cloned option default selectedness');const option=document.getElementById('o2').cloneNode(true);const host=document.createElement('select');host.setAttribute('multiple','');host.appendChild(option);assert(host.selectedIndex===-1,'option clone does not copy live selectedness');",
        DOM_ASSERT "const f=document.getElementById('select'),option=document.getElementById('o1');option.setAttribute('selected','');f.selectedIndex=1;const copy=option.cloneNode(true),host=document.createElement('select');host.setAttribute('multiple','');host.appendChild(copy);assert(host.selectedIndex===0,'option clone initializes from selected attribute');copy.removeAttribute('selected');assert(host.selectedIndex===-1,'option clone starts nondirty');"
    };
    const char *names[]={"clone-input-current.js","clone-input-dirty.js","clone-checkbox-current.js","clone-checkbox-dirty.js","clone-textarea-shallow.js","clone-textarea-deep.js","clone-textarea-dirty.js","clone-clean-textarea-shallow.js","clone-clean-textarea-deep.js","clone-clean-child-history.js","clone-clean-replace-history.js","clone-clean-to-dirty.js","clone-clean-clone-chain.js","clone-clean-effective-radio.js","clone-deep-descendants.js","clone-clean-input-before-read.js","clone-equal-value-dirties.js","clone-textarea-direct-text.js","clone-raw-normalization-history.js","clone-option-defaults.js","clone-option-dirty-reset.js"};
    for(size_t i=0;i<sizeof(sources)/sizeof(*sources);i++){
        DomFixture f=fixture_new(NULL);if(!fixture_ready(&f))continue;
        dom_script(&f,sources[i],names[i]);fixture_free(&f);
    }
}
#undef DOM_ASSERT
