/* ELEMENT.ATTRIBUTES, NAMEDNODEMAP AND ATTR (libdom/attr.c; LIBDOM.md
 * § Attributes as nodes): a case per sentence of the contract. The shape
 * and the switch; identity across the map and the element's methods;
 * liveness, and detachment by every removal verb with the last value
 * kept; createAttribute and createAttributeNS; setAttributeNode moving an
 * Attr, replacing one, refusing one in use, and an on<type> Attr's
 * handler; every Node member through an Attr, and the shadow covering
 * Node.prototype whole; an Attr handed to the tree verbs; the map's
 * indexed and named properties, and page code running in the middle of a
 * walk; the namespace forms on an SVG element's xlink:href; the quota met
 * by detaching in a loop; the backstop's line, reached by a host verb
 * beneath the binding; every allocation failed in turn. */

#define ATTR_ASSERT STORAGE_ASSERT

uint64_t d_attr_steps(void);

static void attributes_shape_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "var b=document.getElementById('box'),m=b.attributes;"
        "assert(m===b.attributes,'[SameObject]');"
        "assert(m instanceof NamedNodeMap&&Object.prototype.toString.call(m)==='[object NamedNodeMap]');"
        "assert(m.length===2&&m[0].name==='id'&&m[1].name==='class','in the element\\'s order');"
        "var a=m[0];"
        "assert(a===m[0]&&a===m.item(0)&&a===m.id&&a===m['id']&&a===m.getNamedItem('id')&&"
        "a===m.getNamedItem('ID')&&a===b.getAttributeNode('id')&&a===b.getAttributeNode('Id'),"
        "'one Attr, however it is reached');"
        "assert(a instanceof Attr&&a instanceof Node&&a.nodeType===2&&Node.ATTRIBUTE_NODE===2);"
        "assert(Object.prototype.toString.call(a)==='[object Attr]'&&Object.getPrototypeOf(Attr)===Node);"
        "assert(a.ownerElement===b&&a.specified===true&&a.namespaceURI===null&&a.prefix===null&&"
        "a.localName==='id'&&a.name==='id'&&a.nodeName==='id'&&a.value==='box');"
        "assert(m.ID===undefined&&!('ID' in m)&&'id' in m,'an HTML element lists no uppercase name');"
        "assert(b.getAttributeNode('absent')===null&&m.getNamedItem('absent')===null&&m.absent===undefined);"
        "assert(m.item(2)===null&&m[2]===undefined&&m.item(-1)===null,'past the end');"
        "throws('TypeError',()=>new Attr());throws('TypeError',()=>new NamedNodeMap());"
        "throws('TypeError',()=>m.item());throws('TypeError',()=>b.getAttributeNode());"
        "throws('TypeError',()=>document.getAttributeNode('id'));"
        "throws('TypeError',()=>b.createAttribute('x'));"
        "var v=Object.getOwnPropertyDescriptor(Attr.prototype,'value');"
        "throws('TypeError',()=>v.get.call(b));throws('TypeError',()=>v.set.call(b,'x'));"
        "var l=Object.getOwnPropertyDescriptor(NamedNodeMap.prototype,'length').get;"
        "throws('TypeError',()=>l.call(b));"
        "var at=Object.getOwnPropertyDescriptor(Element.prototype,'attributes').get;"
        "throws('TypeError',()=>at.call(document));throws('TypeError',()=>at.call(a));",
        "attributes: the map, the Attr and their interfaces");
    fixture_free(&f);

    /* With the switch off the surface is the page's all the same (it is
     * DOM Level 1's, jQuery 1's floor); only the interface objects go. */
    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "assert(typeof Attr==='undefined'&&typeof NamedNodeMap==='undefined');"
        "var b=document.getElementById('box'),a=b.attributes.id;"
        "assert(a===b.getAttributeNode('id')&&a.nodeType===2&&a.value==='box'&&b.attributes.length===2);"
        "var c=document.createAttribute('t');c.value='v';b.setAttributeNode(c);assert(b.getAttribute('t')==='v');"
        /* the shadow, reached through the chain with no interface to name it */
        "var A=Object.getPrototypeOf(a),N=Object.getPrototypeOf(A);"
        "assert(N===Object.getPrototypeOf(Object.getPrototypeOf(document.createTextNode(''))));"
        "for(const k of Object.getOwnPropertyNames(N))"
        "if(k!=='constructor'&&!/^[A-Z_]+$/.test(k))assert(Object.prototype.hasOwnProperty.call(A,k),'shadows '+k);",
        "attributes: with the switch off, the classic surface and no interface objects");
    fixture_free(&f);
}

/* An attached Attr reads and writes its element's attribute by name; one a
 * removal took keeps the value it left with, and takes a value of its own. */
static void attributes_live_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "var b=document.getElementById('box'),a=b.getAttributeNode('class');"
        "b.setAttribute('class','new');assert(a.value==='new'&&a.nodeValue==='new'&&a.textContent==='new','read live');"
        "a.value='set';assert(b.getAttribute('class')==='set'&&b.className==='set','written through');"
        "a.nodeValue=null;assert(b.getAttribute('class')==='','nodeValue: null is empty');"
        "a.textContent='t';a.textContent=undefined;assert(b.getAttribute('class')==='','textContent: undefined is empty');"
        "a.value=null;assert(b.getAttribute('class')==='null','value: a DOMString');"
        "a.value={toString(){return 'once'}};assert(b.getAttribute('class')==='once');"
        "a.value='last';b.removeAttribute('class');"
        "assert(a.ownerElement===null&&a.value==='last'&&b.getAttributeNode('class')===null,'detached with its last value');"
        "assert(b.attributes.length===1&&b.attributes[0].name==='id');"
        "a.value='mine';assert(!b.hasAttribute('class')&&a.value==='mine'&&a.nodeValue==='mine','its own value now');"
        "b.setAttribute('class','again');var n=b.getAttributeNode('class');"
        "assert(n!==a&&n.value==='again'&&a.value==='mine'&&a.ownerElement===null,'set again: a new Attr');"
        /* a value converted from page code that moves the Attr is set where
         * the Attr is once the conversion is over */
        "var x=document.createElement('i');x.setAttribute('k','1');var k=x.getAttributeNode('k');"
        "k.value={toString(){x.removeAttribute('k');return 'late'}};"
        "assert(!x.hasAttribute('k')&&k.ownerElement===null&&k.value==='late','set on the detached Attr');",
        "attributes: read and written live, detached with the last value");
    dom_script(&f, ATTR_ASSERT
        "function gone(make,remove,label){var e=document.createElement('div');document.body.appendChild(e);make(e);"
        "var x=e.attributes[0],name=x.name,value=x.value;remove(e,x);"
        "assert(x.ownerElement===null&&x.value===value,label+': detached with its value');"
        "assert(!e.hasAttribute(name),label+': removed');}"
        "gone(e=>e.setAttribute('t','v'),e=>e.removeAttribute('T'),'removeAttribute');"
        "gone(e=>e.setAttribute('t','v'),e=>assert(e.toggleAttribute('t')===false),'toggleAttribute');"
        "gone(e=>e.setAttribute('t','v'),e=>assert(e.toggleAttribute('t',false)===false),'toggleAttribute, forced');"
        "gone(e=>{e.dataset.fooBar='v'},e=>{delete e.dataset.fooBar},'a dataset delete');"
        "gone(e=>e.setAttribute('t','v'),(e,x)=>assert(e.removeAttributeNode(x)===x),'removeAttributeNode');"
        "gone(e=>e.setAttribute('t','v'),(e,x)=>assert(e.attributes.removeNamedItem('t')===x),'removeNamedItem');"
        "gone(e=>e.setAttribute('t','v'),(e,x)=>assert(e.attributes.removeNamedItemNS(null,'t')===x),"
        "'removeNamedItemNS');"
        "var e=document.createElement('p');e.setAttribute('only','1');"
        "var r=e.attributes.removeNamedItem('only');"
        "assert(r instanceof Attr&&r.value==='1'&&r.ownerElement===null,'removeNamedItem makes the Attr it answers');"
        "throws('NotFoundError',()=>document.body.attributes.removeNamedItem('nope'));"
        "throws('NotFoundError',()=>document.body.attributes.removeNamedItemNS(null,'nope'));"
        "var b=document.getElementById('box');b.className='old';var k=b.getAttributeNode('class');b.classList.remove('old');"
        "assert(k.ownerElement===b&&k.value==='','classList empties the attribute and leaves it');",
        "attributes: every removal verb detaches the Attr");
    fixture_free(&f);
}

static void attributes_create_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "var c=document.createAttribute('Data-X');"
        "assert(c instanceof Attr&&c.name==='data-x'&&c.localName==='data-x'&&c.value===''&&c.ownerElement===null&&"
        "c.ownerDocument===document&&c.namespaceURI===null&&c.prefix===null,'folded, born detached');"
        "var colon=document.createAttribute('a:b');"
        "assert(colon.localName==='a:b'&&colon.prefix===null&&colon.name==='a:b','in no namespace, no prefix');"
        "throws('InvalidCharacterError',()=>document.createAttribute('a b'));"
        "throws('InvalidCharacterError',()=>document.createAttribute(''));"
        "throws('InvalidCharacterError',()=>document.createAttribute('a>'));"
        "throws('TypeError',()=>document.createAttribute());"
        "var d=document.implementation.createHTMLDocument('');var o=d.createAttribute('o');"
        "assert(o.ownerDocument===d,'made in its document');"
        "assert(document.implementation.createDocument(null,'').createAttribute('KeepCase').name==='KeepCase',"
        "'an XML document keeps the case');"
        "var b=document.body;o.value='v';b.setAttributeNode(o);"
        "assert(o.ownerDocument===document&&o.ownerElement===b,'adopted with its element');"
        "b.removeAttributeNode(o);assert(o.ownerDocument===document,'the document it left');"
        "var e=d.createElement('p');e.setAttribute('q','1');var q=e.getAttributeNode('q');"
        "assert(q.ownerDocument===d);e.removeAttribute('q');assert(q.ownerDocument===d);",
        "createAttribute: folded, detached, in its document");
    dom_script(&f, ATTR_ASSERT
        "var X='http://www.w3.org/1999/xlink',XML='http://www.w3.org/XML/1998/namespace',"
        "XMLNS='http://www.w3.org/2000/xmlns/';"
        "var a=document.createAttributeNS(X,'xlink:href');"
        "assert(a.namespaceURI===X&&a.prefix==='xlink'&&a.localName==='href'&&a.name==='xlink:href'&&a.value==='');"
        "var n=document.createAttributeNS(null,'Plain');"
        "assert(n.namespaceURI===null&&n.prefix===null&&n.localName==='Plain'&&n.name==='Plain','no folding');"
        "assert(document.createAttributeNS('','e').namespaceURI===null,'the empty namespace is none');"
        "assert(document.createAttributeNS(X,'f:o:o').localName==='o:o','split at the first colon');"
        "throws('NamespaceError',()=>document.createAttributeNS(null,'a:b'));"
        "throws('NamespaceError',()=>document.createAttributeNS(X,'xml:lang'));"
        "assert(document.createAttributeNS(XML,'xml:lang').prefix==='xml');"
        "throws('NamespaceError',()=>document.createAttributeNS(X,'xmlns'));"
        "throws('NamespaceError',()=>document.createAttributeNS(XMLNS,'a'));"
        "assert(document.createAttributeNS(XMLNS,'xmlns:a').localName==='a');"
        "throws('InvalidCharacterError',()=>document.createAttributeNS(X,':a'));"
        "throws('InvalidCharacterError',()=>document.createAttributeNS(X,'a:'));"
        "throws('InvalidCharacterError',()=>document.createAttributeNS(X,'a b'));"
        "throws('TypeError',()=>document.createAttributeNS(X));",
        "createAttributeNS: validate and extract, for an attribute");
    fixture_free(&f);
}

static void attributes_move_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "var p=document.getElementById('one'),q=document.getElementById('two');"
        "var a=document.createAttribute('title');a.value='t1';"
        "assert(p.setAttributeNode(a)===null&&p.getAttribute('title')==='t1'&&a.ownerElement===p,'taken on');"
        "assert(p.attributes[p.attributes.length-1]===a&&p.getAttributeNode('title')===a,'last, and the same object');"
        "assert(p.setAttributeNode(a)===a,'its own element answers it');"
        "throws('InUseAttributeError',()=>q.setAttributeNode(a));"
        "throws('InUseAttributeError',()=>q.attributes.setNamedItem(a));"
        "assert(!q.hasAttribute('title'),'refused before anything changed');"
        "assert(p.removeAttributeNode(a)===a&&a.ownerElement===null&&!p.hasAttribute('title'));"
        "assert(q.setAttributeNodeNS(a)===null&&a.ownerElement===q&&q.getAttribute('title')==='t1',"
        "'moved to another element');"
        "throws('NotFoundError',()=>p.removeAttributeNode(a));"
        "throws('TypeError',()=>p.setAttributeNode({}));throws('TypeError',()=>p.setAttributeNode(p));"
        "throws('TypeError',()=>p.removeAttributeNode(null));throws('TypeError',()=>p.setAttributeNode());"
        "var b=document.createAttribute('title');b.value='t2';var at=[...q.attributes].indexOf(a);"
        "var old=q.attributes.setNamedItemNS(b);"
        "assert(old===a&&a.ownerElement===null&&a.value==='t1'&&b.ownerElement===q&&q.getAttribute('title')==='t2',"
        "'the one it replaced, detached with its value');"
        "assert(q.attributes[at]===b,'in its place');"
        "a.value='free';assert(q.getAttribute('title')==='t2','the replaced Attr is no longer the element\\'s');"
        /* an on<type> Attr is a handler like the attribute setAttribute sets */
        "var h=document.createAttribute('onclick');h.value='window.heard=(window.heard||0)+1';"
        "var t=document.getElementById('two');t.setAttributeNode(h);t.click();assert(window.heard===1,'a handler');"
        "h.value='window.heard+=10';t.click();assert(window.heard===11,'changed through the Attr');"
        "t.removeAttributeNode(h);t.click();assert(window.heard===11,'removed, no handler');",
        "setAttributeNode: taken on, moved, replacing, refusing one in use");
    fixture_free(&f);
}

/* Through an Attr every Node member answers as the standard says: a Node
 * with no place in the tree (attr.c's shadow). */
static void attributes_node_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "var a=document.getElementById('box').getAttributeNode('id');"
        "for(const k of ['parentNode','parentElement','firstChild','lastChild','previousSibling','nextSibling'])"
        "assert(a[k]===null,k);"
        "assert(a.childNodes.length===0&&a.childNodes===a.childNodes&&a.childNodes.item(0)===null&&"
        "[...a.childNodes].length===0,'childNodes: empty, [SameObject]');"
        "assert(a.hasChildNodes()===false&&a.isConnected===false&&a.ownerDocument===document&&a.nodeName==='id');"
        "assert(a.contains(a)===true&&a.contains(null)===false&&a.contains(document.body)===false&&"
        "a.contains(document.createAttribute('id'))===false,'contains: itself');"
        "throws('TypeError',()=>a.contains({}));"
        "var p=document.createElement('p');"
        "throws('HierarchyRequestError',()=>a.appendChild(p));throws('HierarchyRequestError',()=>a.insertBefore(p,null));"
        "throws('HierarchyRequestError',()=>a.replaceChild(p,p));throws('NotFoundError',()=>a.removeChild(p));"
        "throws('HierarchyRequestError',()=>a.appendChild(document.createAttribute('x')));"
        "throws('TypeError',()=>a.appendChild(5));throws('TypeError',()=>a.insertBefore(p,5));"
        "throws('TypeError',()=>a.removeChild(null));throws('TypeError',()=>a.replaceChild(p));"
        "assert(p.parentNode===null,'nothing moved');"
        "var c=a.cloneNode();assert(c!==a&&c instanceof Attr&&c.name==='id'&&c.value==='box'&&c.ownerElement===null&&"
        "c.ownerDocument===document&&a.cloneNode(true).value==='box','cloneNode: a detached copy');"
        "var X='http://www.w3.org/1999/xlink',n=document.createAttributeNS(X,'xlink:href');n.value='#v';"
        "var nc=n.cloneNode();assert(nc.namespaceURI===X&&nc.prefix==='xlink'&&nc.value==='#v');"
        "var heard=[];a.addEventListener('x',e=>heard.push(e.eventPhase,e.target===a,e.currentTarget===a));"
        "document.getElementById('box').addEventListener('x',()=>heard.push('element'));"
        "assert(a.dispatchEvent(new Event('x',{bubbles:true}))===true&&heard.join()==='2,true,true',"
        "'an EventTarget whose path is itself: '+heard);",
        "an Attr's Node members, each one");
    dom_script(&f, ATTR_ASSERT
        "var a=document.getElementById('box').getAttributeNode('id'),p=document.createElement('p');"
        "assert(Object.getPrototypeOf(Attr.prototype)===Node.prototype);"
        "for(const k of Object.getOwnPropertyNames(Node.prototype))"
        "if(k!=='constructor'&&!/^[A-Z_]+$/.test(k))"
        "assert(Object.prototype.hasOwnProperty.call(Attr.prototype,k),'Attr shadows '+k);"
        "try{Node.prototype.appendChild.call(a,p);assert(false,'no throw')}"
        "catch(e){assert(e.name==='TypeError'&&/an Attr is a Node with no place in the tree/i.test(e.message),e.message)}"
        "var g=Object.getOwnPropertyDescriptor(Node.prototype,'parentNode').get;throws('TypeError',()=>g.call(a));",
        "an Attr: the shadow covers Node.prototype, and Node's own member says why it refuses one");
    dom_script(&f, ATTR_ASSERT
        "var b=document.getElementById('box'),a=b.getAttributeNode('id'),one=document.getElementById('one');"
        "throws('HierarchyRequestError',()=>b.appendChild(a));"
        "throws('HierarchyRequestError',()=>b.insertBefore(a,null));throws('HierarchyRequestError',()=>b.insertBefore(a,one));"
        "throws('NotFoundError',()=>b.insertBefore(a,document.body));throws('NotFoundError',()=>b.insertBefore(one,a));"
        "throws('HierarchyRequestError',()=>b.replaceChild(a,one));throws('NotFoundError',()=>b.replaceChild(one,a));"
        "throws('NotFoundError',()=>b.removeChild(a));throws('TypeError',()=>b.insertBefore(5,a));"
        "throws('TypeError',()=>b.replaceChild(a,null));"
        "throws('HierarchyRequestError',()=>b.append(a));throws('HierarchyRequestError',()=>b.append('t',a));"
        "throws('HierarchyRequestError',()=>one.before(a));throws('HierarchyRequestError',()=>b.replaceChildren(a));"
        "var loose=document.createElement('i');loose.before(a);assert(loose.parentNode===null,'no parent, nothing to refuse');"
        "assert(b.contains(a)===false,'no tree node contains an Attr');"
        "assert(b.childNodes.length===2&&a.ownerElement===b&&b.textContent==='onetwo','nothing moved');",
        "an Attr handed to the tree verbs");
    fixture_free(&f);
}

/* The map's own properties: indices live and read-only, names live,
 * unenumerable and behind the prototype's, nothing held between calls. */
static void attributes_map_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "var b=document.getElementById('box'),m=b.attributes;"
        "assert(Object.keys(m).join()==='0,1','indices enumerable, names not');"
        "assert(Object.getOwnPropertyNames(m).join()==='0,1,id,class',Object.getOwnPropertyNames(m).join());"
        "var names=[];for(var k in m)names.push(k);"
        "assert(names[0]==='0'&&names[1]==='1'&&names.indexOf('id')<0&&names.indexOf('class')<0,'for..in: '+names);"
        "var dI=Object.getOwnPropertyDescriptor(m,0);"
        "assert(dI.value===m[0]&&dI.enumerable&&!dI.writable&&dI.configurable,'an index');"
        "var dN=Object.getOwnPropertyDescriptor(m,'id');"
        "assert(dN.value===m[0]&&!dN.enumerable&&!dN.writable&&dN.configurable,'a name');"
        "assert(Array.from(m).length===2&&[...m][1]===m[1]&&m[Symbol.iterator]===Array.prototype.values,'iterable');"
        "(function(){'use strict';throws('TypeError',()=>{m[0]=1});throws('TypeError',()=>{m.id=1});"
        "throws('TypeError',()=>{delete m[0]});throws('TypeError',()=>{delete m.id});})();"
        "throws('TypeError',()=>Object.defineProperty(m,'5',{value:1}));"
        "throws('TypeError',()=>Object.defineProperty(m,'class',{value:1}));"
        "assert(delete m[7]===true&&delete m.nothing===true&&m[0].name==='id');"
        /* (the engine lists an exotic object's own ordinary properties
         * before the ones it is asked for, whatever kind) */
        "m.expando=3;assert(m.expando===3&&Object.keys(m).sort().join()==='0,1,expando','another name is ordinary');"
        "b.setAttribute('item','x');b.setAttribute('length','y');"
        "assert(typeof m.item==='function'&&typeof m.length==='number'&&m.getNamedItem('item').value==='x',"
        "'the prototype\\'s names win');"
        "assert(Object.getOwnPropertyNames(m).indexOf('item')<0,'and are not listed');"
        "b.removeAttribute('item');b.removeAttribute('length');"
        "b.setAttribute('0','zero');assert(m[0].name==='id'&&m.getNamedItem('0').value==='zero','an index is an index');"
        "assert(Object.getOwnPropertyNames(m).filter(k=>k==='0').length===1,'and listed once');"
        "b.removeAttribute('0');",
        "NamedNodeMap: indices, names and the rest");
    dom_script(&f, ATTR_ASSERT
        "var e=document.createElement('div');e.setAttribute('a','1');e.setAttribute('b','2');e.setAttribute('c','3');"
        "var m=e.attributes,seen=[];"
        "for(var i=0;i<m.length;i++){seen.push(m[i].name);if(i===0)e.removeAttribute('b');}"
        "assert(seen.join()==='a,c','the walk reads the list as it stands at each step');"
        "assert(m.item({valueOf(){e.removeAttribute('a');return 0}}).name==='c','item: the list after its argument');"
        "e.setAttribute('d','4');assert(m.length===2&&m[1].name==='d','a set is seen at once');"
        /* the visibility check runs a Proxy prototype's `has`: what it
         * removes is not answered, the list being read after it */
        "var x=document.createElement('div');x.setAttribute('gone','1');var xm=x.attributes;"
        "Object.setPrototypeOf(xm,new Proxy(NamedNodeMap.prototype,{has(t,k){if(k==='gone')x.removeAttribute('gone');"
        "return Reflect.has(t,k)}}));"
        "assert(xm.gone===undefined&&!x.hasAttribute('gone'),'the list read after page code ran');"
        "x.setAttribute('kept','1');assert(Object.getOwnPropertyNames(xm).join()==='0,kept');",
        "NamedNodeMap: page code running in the middle of a walk");
    /* The walk's cursor is asked again at every use: another element, a
     * change to the list, or a backward step each walk from the first. */
    dom_script(&f, ATTR_ASSERT
        "var p=document.createElement('p'),q=document.createElement('q');"
        "for(var i=0;i<6;i++){p.setAttribute('p'+i,i);q.setAttribute('q'+i,i)}"
        "var pm=p.attributes,qm=q.attributes;"
        "assert(pm[4].name==='p4'&&qm[4].name==='q4'&&pm[5].name==='p5'&&pm[1].name==='p1','two elements, either order');"
        "assert(pm.p3.name==='p3'&&qm.q5.name==='q5'&&pm.p0.name==='p0'&&pm.p5.name==='p5','names, either way round');"
        "pm[4];p.removeAttribute('p2');assert(pm[4].name==='p5'&&pm[2].name==='p3','a removal before the cursor');"
        "pm[1];p.setAttribute('p1','changed');assert(pm[1].value==='changed'&&pm[3].name==='p4');"
        "pm[4];p.removeAttribute('p0');assert(pm[0].name==='p1'&&pm.p0===undefined&&pm[4]===undefined,"
        "'the first attribute gone');",
        "NamedNodeMap: the walk's cursor never answers a list that moved");
    fixture_free(&f);

    /* A walk over all of an element's n attributes, in one call, takes a
     * number of steps linear in n (attr.c's cursor and table), however it
     * walks: n² would be a million here. */
    f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.w=document.createElement('div');for(var i=0;i<1000;i++)w.setAttribute('a'+i,i);"
                   "globalThis.wm=w.attributes;", "walks: an element with 1000 attributes");
    static const char *const kWalks[] = {
        "Array.from(wm).length===1000||fail()", "[...wm].length===1000||fail()",
        "Object.keys(wm).length===1000||fail()", "Object.getOwnPropertyNames(wm).forEach(k=>wm[k])",
        "for(var k in wm);", "for(var i=0;i<wm.length;i++)wm.item(i)",
        "for(var i=0;i<1000;i++)wm['a'+i]",
    };
    for (size_t i = 0; i < sizeof(kWalks) / sizeof(kWalks[0]); i++) {
        uint64_t before = d_attr_steps();
        dom_script(&f, kWalks[i], "walks: a walk over every attribute");
        uint64_t steps = d_attr_steps() - before;
        char line[160];
        snprintf(line, sizeof(line), "walks: %s takes %llu steps, linear in 1000", kWalks[i],
                 (unsigned long long)steps);
        check(steps <= 10 * 1000, line);
    }
    fixture_free(&f);
}

/* The namespace forms, on an SVG element's xlink:href (DOM § 4.9's "get an
 * attribute by namespace and local name"), and the one thing the tree
 * cannot do: give a new attribute a namespace. */
static void attributes_namespace_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, ATTR_ASSERT
        "var X='http://www.w3.org/1999/xlink',box=document.getElementById('box');"
        "box.innerHTML='<svg><a xlink:href=\"#t\"></a></svg>';var svg=box.firstChild,s=svg.firstChild,h=s.attributes[0];"
        "assert(h.name==='xlink:href'&&h.namespaceURI===X&&h.prefix==='xlink'&&h.localName==='href'&&h.value==='#t');"
        "assert(s.getAttributeNodeNS(X,'href')===h&&s.attributes.getNamedItemNS(X,'href')===h&&"
        "s.getAttributeNode('xlink:href')===h&&s.attributes['xlink:href']===h,'by namespace, by name');"
        "assert(s.getAttributeNodeNS(null,'href')===null&&s.getAttributeNodeNS(X,'xlink:href')===null&&"
        "s.getAttributeNodeNS('','href')===null);"
        "throws('TypeError',()=>s.getAttributeNodeNS(X));"
        "svg.setAttribute('viewBox','0 0 1 1');"
        "assert(svg.attributes.viewBox.value==='0 0 1 1'&&svg.getAttributeNode('viewBox')===svg.attributes.viewBox,"
        "'an SVG element\\'s names are listed as they are');"
        "var gone=s.attributes.removeNamedItemNS(X,'href');"
        "assert(gone===h&&h.ownerElement===null&&h.namespaceURI===X&&h.value==='#t'&&!s.hasAttribute('xlink:href'));"
        "throws('NotSupportedError',()=>s.setAttributeNode(h));"
        "assert(!s.hasAttribute('xlink:href')&&h.ownerElement===null,'refused before the tree changed');"
        "var twin=document.createElement('div');twin.innerHTML='<svg><a xlink:href=\"#u\"></a></svg>';"
        "var t=twin.firstChild.firstChild,before=t.getAttributeNodeNS(X,'href');"
        "assert(t.setAttributeNodeNS(h)===before&&h.ownerElement===t&&h.value==='#t'&&before.value==='#u'&&"
        "before.ownerElement===null,'where its element holds the name in that namespace');"
        "assert(t.attributes[0]===h&&t.getAttributeNodeNS(X,'href')===h&&t.getAttribute('xlink:href')==='#t');"
        "throws('NotSupportedError',()=>t.setAttributeNode(document.createAttribute('xlink:href')));"
        "throws('NotSupportedError',()=>document.body.setAttributeNode(document.createAttributeNS(X,'xlink:role')));"
        "assert(!document.body.hasAttribute('xlink:role'));",
        "the namespace forms, on an SVG element's xlink:href");
    fixture_free(&f);
}

/* A page that makes or detaches Attrs in a loop meets the binding's
 * budget, and every byte is accounted for at teardown. */
static void attributes_quota_cases(void)
{
    static const char *const kLoops[] = {
        "var a=document.createAttribute('a');kept.push(a)",
        "kept.push(b.getAttributeNode('t'));b.removeAttribute('t');b.setAttribute('t',i)",
    };
    for (size_t l = 0; l < sizeof(kLoops) / sizeof(kLoops[0]); l++) {
        os64_dom_options_t options = os64_dom_default_options();
        options.max_bytes = 512 * 1024;
        DomFixture f = fixture_new(&options);
        if (!fixture_ready(&f)) return;
        char source[1024];
        snprintf(source, sizeof(source), ATTR_ASSERT
                 "var name='none',kept=[],b=document.getElementById('box');b.setAttribute('t','0');"
                 "for(var i=0;i<100000;i++){try{%s}catch(e){name=e.name;break}}"
                 "assert(name==='QuotaExceededError','the budget: '+name);", kLoops[l]);
        dom_script(&f, source, "quota: Attrs made or detached in a loop meet QuotaExceededError");
        check(os64_dom_bytes(f.binding) <= options.max_bytes, "quota: the binding stays within its budget");
        fixture_free(&f);
    }
    /* A removal or a replacement refused for want of memory changed
     * nothing: each of their allocations failed in turn, the Attrs and the
     * attribute read the same after as before. */
    static const char *const kRefused[] = {
        "b.removeAttribute('class')",
        "b.setAttributeNode(fresh)",
        "b.attributes.removeNamedItem('class')",
    };
    for (size_t r = 0; r < sizeof(kRefused) / sizeof(kRefused[0]); r++) {
        for (size_t cut = 1; cut <= 16; cut++) {
            DomFixture f = fixture_new(NULL);
            if (!fixture_ready(&f)) return;
            dom_script(&f, "globalThis.b=document.getElementById('box');globalThis.a=b.getAttributeNode('class');"
                           "globalThis.fresh=document.createAttribute('class');fresh.value='new';",
                       "refused: the Attrs held");
            char source[256];
            snprintf(source, sizeof(source), "try{%s;globalThis.done=true}catch(e){if(e.name!=='QuotaExceededError')"
                     "throw e;globalThis.done=false}", kRefused[r]);
            dom_fail_at = dom_attempts + cut;
            dom_script(&f, source, "refused: an allocation cut");
            dom_fail_at = 0;
            dom_script(&f, ATTR_ASSERT
                "if(done){assert(a.ownerElement===null&&a.value==='old','done: detached with its value');"
                "assert(fresh.ownerElement===(b.getAttributeNode('class')===fresh?b:null))}"
                "else{assert(a.ownerElement===b&&b.getAttribute('class')==='old'&&a.value==='old'&&"
                "b.getAttributeNode('class')===a&&fresh.ownerElement===null,'refused: as it was')}",
                "refused: a refusal changed nothing");
            fixture_free(&f);
        }
    }
}

/* THE BACKSTOP: an attribute removed beneath the binding (a host verb, the
 * kind of verb the door does not see) is read as detached and empty, and
 * the host is told once. */
static unsigned broken_heard;
static char broken_sentence[128];
static void broken_rule(void *opaque, const char *sentence)
{
    (void)opaque;
    broken_heard++;
    snprintf(broken_sentence, sizeof(broken_sentence), "%s", sentence);
}

static void attributes_backstop_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.broken = broken_rule;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    broken_heard = 0;
    dom_script(&f, ATTR_ASSERT
        "globalThis.k=document.getElementById('box').getAttributeNode('class');"
        "globalThis.one=document.getElementById('one');globalThis.j=one.getAttributeNode('id');assert(k.value==='old');",
        "backstop: Attrs held");
    os64_html_node_t *box = native_id(f.document->document, "box");
    os64_html_node_t *one = native_id(f.document->document, "one");
    check(box != NULL && os64_page_node_set_attr(f.state, box, "class", NULL, 0, true) == OS64_HTML_OK,
          "backstop: a host verb removes the attribute beneath the binding");
    check(one != NULL && os64_page_node_set_attr(f.state, one, "id", NULL, 0, true) == OS64_HTML_OK,
          "backstop: and another");
    dom_script(&f, ATTR_ASSERT
        "assert(k.value===''&&k.ownerElement===null&&k.ownerDocument===document,'read as detached and empty');"
        "assert(!document.getElementById('box').hasAttribute('class')&&k.name==='class');"
        "document.getElementById('box').setAttribute('class','x');"
        "assert(k.ownerElement===null&&document.getElementById('box').getAttributeNode('class')!==k);",
        "backstop: the Attr read after its attribute went");
    check(broken_heard == 1 && strcmp(broken_sentence, "an attribute left its element outside libdom") == 0,
          "backstop: the host is told the rule, once");
    dom_script(&f, ATTR_ASSERT
        "one.removeAttribute('id');assert(j.ownerElement===null&&j.value==='','the door finds it gone too');",
        "backstop: a removal at the door of an attribute already gone");
    check(broken_heard == 2, "backstop: and says so");
    fixture_free(&f);

    /* No one to tell: the Attr reads the same. */
    f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "globalThis.k=document.getElementById('box').getAttributeNode('class');", "backstop: held");
    box = native_id(f.document->document, "box");
    check(box != NULL && os64_page_node_set_attr(f.state, box, "class", NULL, 0, true) == OS64_HTML_OK,
          "backstop: removed beneath, with no host told");
    dom_script(&f, ATTR_ASSERT "assert(k.value===''&&k.ownerElement===null);", "backstop: read as detached");
    fixture_free(&f);
}

/* Every allocation each operation makes, the binding's own and the
 * engine's, failed in turn (test_dom_storage.inc's sweeps). */
static void attributes_allocation_cases(void)
{
    static const char *const kOperations[] = {
        "document.getElementById('box').attributes[0].value",
        "var b=document.getElementById('box');var a=b.getAttributeNode('class');b.removeAttribute('class');a.value",
        "var a=document.createAttribute('t');a.value='v';document.body.setAttributeNode(a)",
        "var b=document.getElementById('box');b.getAttributeNode('class');b.setAttributeNode(document.createAttribute('class'))",
        "document.createAttributeNS('http://www.w3.org/1999/xlink','xlink:href').cloneNode()",
        "var m=document.getElementById('box').attributes;Object.getOwnPropertyNames(m);m.id;[...m]",
        "document.getElementById('box').attributes.removeNamedItem('id')",
        "document.getElementById('box').getAttributeNode('id').childNodes",
        "var a=document.getElementById('box').getAttributeNode('id');a.addEventListener('x',()=>{});"
        "a.dispatchEvent(new Event('x'))",
    };
    binding_sweep("attributes", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), NULL);
    engine_sweep("attributes", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), NULL);
}

static void dom_attributes_cases(void)
{
    attributes_shape_cases();
    attributes_live_cases();
    attributes_create_cases();
    attributes_move_cases();
    attributes_node_cases();
    attributes_map_cases();
    attributes_namespace_cases();
    attributes_quota_cases();
    attributes_backstop_cases();
}
#undef ATTR_ASSERT
