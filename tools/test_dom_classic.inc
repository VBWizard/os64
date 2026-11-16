/* Consumer-shaped regressions for the classic page surface. The snippets
 * are independent fixtures for the API patterns, not copies of site code. */
#define CLASSIC_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}"
static void dom_classic_cases(void)
{
    DomFixture f=fixture_new(NULL);
    if(!fixture_ready(&f)) return;
    dom_script(&f,CLASSIC_ASSERT
        "assert(self===window&&frames===window&&parent===window&&top===window&&globalThis===window);"
        "assert(self.document===document&&top.location===location&&frames.length===0&&window.length===0);"
        "assert(top===self&&parent===self&&self===window.self);"
        "self.addEventListener('ping',function(){});"
        "var d=Object.getOwnPropertyDescriptor(window,'top');"
        "assert(typeof d.get==='function'&&d.set===undefined&&!d.configurable&&d.enumerable,'top: an unforgeable accessor');"
        "for(const n of ['parent','length']){const p=Object.getOwnPropertyDescriptor(window,n);"
        "assert(typeof p.get==='function'&&typeof p.set==='function'&&p.configurable&&p.enumerable,n+': a [Replaceable] accessor')}"
        "top=1;assert(top===window);"
        "self=7;assert(self===7&&window.self===7);parent='p';assert(parent==='p');"
        "length=3;assert(length===3&&Object.getOwnPropertyDescriptor(window,'length').value===3);"
        "frames=undefined;assert(frames===undefined&&window.window===window);",
        "classic-window-self-aliases");
    dom_script(&f,CLASSIC_ASSERT
        "var b=document.body,i=document.getElementById('field'),frag=document.createDocumentFragment();"
        "assert(b instanceof Element&&b instanceof HTMLElement&&b instanceof Node&&document instanceof Document);"
        "assert(i instanceof HTMLInputElement&&i instanceof Element&&!(b instanceof HTMLInputElement));"
        "assert(frag instanceof DocumentFragment&&document.createTextNode('t') instanceof CharacterData);"
        "assert(new Image() instanceof HTMLImageElement&&Image.prototype===HTMLImageElement.prototype);"
        "assert(Object.getPrototypeOf(b)===HTMLBodyElement.prototype&&b.constructor===HTMLBodyElement);"
        "assert(Object.getPrototypeOf(HTMLBodyElement.prototype)===HTMLElement.prototype);"
        "assert(Object.getPrototypeOf(HTMLElement.prototype)===Element.prototype);"
        "assert(Object.getPrototypeOf(HTMLInputElement.prototype)===HTMLElement.prototype);"
        "assert(Object.getPrototypeOf(Element.prototype)===Node.prototype);"
        "var a=document.createElement('a'),s=document.createElement('script');"
        "assert(a instanceof HTMLAnchorElement&&s instanceof HTMLScriptElement&&!(a instanceof HTMLScriptElement));"
        "var holder=document.createElement('div');holder.innerHTML='<svg><circle/></svg>';"
        "var svg=holder.firstChild;"
        "assert(svg instanceof Element&&!(svg instanceof HTMLElement)&&svg.constructor===SVGGraphicsElement);"
        "assert(svg instanceof SVGElement&&svg.firstChild instanceof SVGElement&&!(b instanceof SVGElement));"
        "assert(Object.getPrototypeOf(SVGElement)===Element&&Object.getPrototypeOf(SVGElement.prototype)===Element.prototype);"
        "holder.innerHTML='<math><mi>x</mi></math>';var mathml=holder.firstChild.firstChild;"
        "assert(mathml.constructor===Element&&!(mathml instanceof SVGElement));"
        "var bad=false;try{new Element()}catch(e){bad=e instanceof TypeError&&e.message==='Illegal constructor'}assert(bad);"
        "bad=false;try{Element()}catch(e){bad=e instanceof TypeError}assert(bad);"
        "assert(Element.length===0&&Element.name==='Element');"
        "var d=Object.getOwnPropertyDescriptor(window,'Element');"
        "assert(d.writable&&d.configurable&&!d.enumerable);"
        "d=Object.getOwnPropertyDescriptor(Element,'prototype');"
        "assert(!d.writable&&!d.configurable&&!d.enumerable);"
        "assert(typeof HTMLDialogElement==='undefined'&&typeof SVGSVGElement==='undefined');"
        /* Text and Comment: each its own prototype under CharacterData, and
         * a constructor (DOM § 4.11, § 4.14) */
        "var tn=document.createTextNode('t'),cn=document.createComment('c');"
        "assert(tn instanceof Text&&tn instanceof CharacterData&&!(tn instanceof Comment)&&tn.constructor===Text);"
        "assert(cn instanceof Comment&&cn instanceof CharacterData&&!(cn instanceof Text)&&cn.constructor===Comment);"
        "assert(Object.getPrototypeOf(Text)===CharacterData&&Object.getPrototypeOf(Comment.prototype)===CharacterData.prototype);"
        "var nt=new Text('hello'),nc=new Comment();"
        "assert(nt.data==='hello'&&nt.nodeType===3&&nt.ownerDocument===document&&nt.parentNode===null);"
        "assert(nc.data===''&&nc.nodeType===8&&new Text(undefined).data===''&&new Text(null).data==='null');"
        "assert(Text.length===0&&Comment.name==='Comment');"
        "bad=false;try{Text('x')}catch(e){bad=e instanceof TypeError}assert(bad);"
        "class T2 extends Text{shout(){return this.data.toUpperCase()}}var t2=new T2('hey');"
        "assert(t2 instanceof T2&&t2 instanceof Text&&t2.shout()==='HEY');"
        "b.appendChild(nt);assert(b.lastChild===nt&&b.textContent.slice(-5)==='hello');b.removeChild(nt);"
        /* the select's option and optgroup have prototypes of their own */
        "var op=document.createElement('option'),og=document.createElement('optgroup');"
        "assert(op instanceof HTMLOptionElement&&og instanceof HTMLOptGroupElement&&op instanceof HTMLElement);"
        "assert(Object.getPrototypeOf(HTMLOptionElement)===HTMLElement&&!(og instanceof HTMLOptionElement));"
        "assert(Node.ELEMENT_NODE===1&&Node.TEXT_NODE===3&&Node.DOCUMENT_FRAGMENT_NODE===11&&Node.NOTATION_NODE===12);"
        "assert(Node.DOCUMENT_POSITION_DISCONNECTED===1&&Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC===32);"
        "assert(b.ELEMENT_NODE===1&&b.nodeType===Node.ELEMENT_NODE&&document.createTextNode('t').TEXT_NODE===3);"
        "d=Object.getOwnPropertyDescriptor(Node,'COMMENT_NODE');assert(d.value===8&&!d.writable&&d.enumerable&&!d.configurable);"
        "d=Object.getOwnPropertyDescriptor(Node.prototype,'COMMENT_NODE');assert(d.value===8&&!d.writable&&!d.configurable);"
        "var made=new DocumentFragment();assert(made instanceof DocumentFragment&&made.ownerDocument===document);"
        "made.appendChild(document.createTextNode('x'));assert(made.firstChild.data==='x'&&made.nodeType===11);"
        "assert(DocumentFragment.name==='DocumentFragment'&&DocumentFragment.length===0);"
        "bad=false;try{DocumentFragment()}catch(e){bad=e instanceof TypeError}assert(bad);"
        "bad=false;try{DocumentFragment.call(function C(){})}catch(e){bad=e instanceof TypeError}assert(bad);"
        "bad=false;try{DocumentFragment.call(DocumentFragment)}catch(e){bad=e instanceof TypeError}assert(bad);"
        "bad=false;try{DocumentFragment.apply(Element,[])}catch(e){bad=e instanceof TypeError}assert(bad);"
        "assert(Reflect.construct(DocumentFragment,[]).nodeType===11);"
        "class F extends DocumentFragment{hello(){return 'hi'}}var ff=new F();"
        "assert(ff instanceof F&&ff instanceof DocumentFragment&&ff.hello()==='hi');"
        "assert(ff.nodeType===11&&ff.ownerDocument===document&&ff.constructor===F);"
        "function G(){}G.prototype=Object.create(DocumentFragment.prototype);G.prototype.greet=function(){return 'g'};"
        "var gg=Reflect.construct(DocumentFragment,[],G);"
        "assert(gg instanceof G&&gg.greet()==='g'&&Object.getPrototypeOf(gg)===G.prototype);"
        "gg.appendChild(document.createTextNode('z'));assert(gg.firstChild.data==='z');"
        "function H(){}H.prototype=7;var hh=Reflect.construct(DocumentFragment,[],H);"
        "assert(Object.getPrototypeOf(hh)===DocumentFragment.prototype&&hh.nodeType===11);"
        "assert(DocumentFragment.prototype.constructor===DocumentFragment&&made.constructor===DocumentFragment);"
        "assert(Object.getPrototypeOf(Node)===EventTarget&&Object.getPrototypeOf(Element)===Node);"
        "assert(Object.getPrototypeOf(HTMLElement)===Element&&Object.getPrototypeOf(HTMLInputElement)===HTMLElement);"
        "assert(Object.getPrototypeOf(HTMLLinkElement)===HTMLElement&&Object.getPrototypeOf(Document)===Node);"
        "assert(Object.getPrototypeOf(HTMLIFrameElement)===HTMLElement&&document.createElement('iframe') instanceof HTMLIFrameElement);"
        "assert(Object.getPrototypeOf(CharacterData)===Node&&Object.getPrototypeOf(DocumentFragment)===Node);"
        "assert(HTMLElement.ELEMENT_NODE===1);"
        "d=Object.getOwnPropertyDescriptor(HTMLElement,'length');"
        "assert(d.value===0&&!d.writable&&!d.enumerable&&d.configurable);"
        "bad=false;try{new DocumentType()}catch(e){bad=e instanceof TypeError&&e.message==='Illegal constructor'}assert(bad);"
        "Element.prototype.patched=function(){return 'yes'};assert(b.patched()==='yes'&&i.patched()==='yes');"
        "delete Element.prototype.patched;",
        "classic-interface-objects");
    dom_script(&f,CLASSIC_ASSERT
        "assert(navigator===window.navigator);assert(navigator.appName==='Netscape');"
        "assert(navigator.userAgent==='yonder/1.0 (os64)');"
        "assert(navigator.appVersion==='1.0 (os64)');"
        "var a=new Image(25,30);assert(a.tagName==='IMG'&&a.width===25&&a.height===30);"
        "assert(a.parentNode===null&&a instanceof Image);"
        "var getter=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(a),'src').get;"
        "var wrong=false;try{getter.call(document.body)}catch(e){wrong=e instanceof TypeError}assert(wrong);"
        "a.name='roll';a.src='before.gif';document.body.appendChild(a);"
        "assert(document.images.length===1&&document.images===document.images);"
        "assert(document.roll===a&&document.images.roll===a&&document.images.namedItem('roll')===a);"
        "var before=a.src;document.roll.src='after.gif';assert(a.src.indexOf('after.gif')>=0);"
        "a.src=before;a.name='renamed';assert(document.roll===undefined&&document.renamed===a);"
        "document.body.removeChild(a);assert(document.images.length===0&&document.renamed===undefined);"
        "document.captureEvents(Event.MOUSEMOVE);document.releaseEvents(Event.MOUSEMOVE);",
        "classic-image-rollovers");
    dom_script(&f,CLASSIC_ASSERT
        "var form=document.createElement('form');form.name='countdown';document.body.appendChild(form);"
        "var field=document.createElement('input');field.name='display';form.appendChild(field);"
        "assert(document.forms.length===2&&document.forms[1]===form&&document.countdown===form);"
        "assert(form.elements.length===1&&form.display===field&&form.elements.display===field);"
        "form.display.value='ready';assert(field.value==='ready');"
        "field.name='other';assert(form.display===undefined&&form.other===field);"
        "var detached=document.createElement('form');detached.id='externalOwner';"
        "var members=detached.elements;assert(members.length===0);"
        "var external=document.createElement('input');external.setAttribute('form','externalOwner');"
        "document.body.appendChild(external);document.body.appendChild(detached);"
        "assert(members===detached.elements&&members.length===1&&members[0]===external);"
        "document.body.removeChild(detached);assert(members.length===0);",
        "classic-named-form");
    dom_script(&f,CLASSIC_ASSERT
        "var box=document.createElement('div');document.body.appendChild(box);"
        "var style=box.style;assert(style===box.style);style.width='100px';style.display='none';"
        "assert(box.getAttribute('style').indexOf('width:100px')>=0&&style.width==='100px');"
        "for(var i=0;i<100;i++)style.width=(i+1)+'px';"
        "assert(style.width==='100px'&&box.getAttribute('style').length<80);"
        "style.width='bogus';assert(style.width==='100px');"
        "style.width='10px;height:99px';assert(style.width==='100px'&&style.height==='');"
        "style.display='';assert(style.display===''&&style.width==='100px');"
        "style.setProperty('height','12px','important');assert(style.getPropertyPriority('height')==='important');"
        "style.height='15px';assert(style.height==='15px'&&style.getPropertyPriority('height')==='');"
        "style.setProperty('--Case','one');assert(style.getPropertyValue('--Case')==='one'&&style.getPropertyValue('--case')==='');"
        "assert(style.removeProperty('height')==='15px'&&style.height==='');"
        "box.setAttribute('style','background-image:url(\"semi;colon.png\"); width:20px !important; width:30px');"
        "assert(style.width==='20px');style.width='40px';"
        "assert(style.width==='40px'&&style.backgroundImage.indexOf('semi;colon.png')>=0);"
        "document.body.removeChild(box);style.visibility='hidden';assert(box.style.visibility==='hidden');"
        "box.setAttribute('style','x'.repeat(65537));var capped=false;"
        "try{style.cssText}catch(e){capped=e.name==='QuotaExceededError'}assert(capped);"
        "style.cssText='';assert(style.cssText==='');",
        "classic-inline-style");
    /* CSSOM § 6.6.1: every property libgarb reads is a member camel-cased,
     * dashed as written, and webkit-cased when it is a -webkit- name, a
     * shorthand or an alias as much as a longhand; each sets and reads the
     * declaration of its own name. */
    dom_script(&f,CLASSIC_ASSERT
        "var box=document.createElement('div');document.body.appendChild(box);var style=box.style;"
        "style['margin-top']='5px';assert(style.marginTop==='5px'&&style['margin-top']==='5px');"
        "style.margin='1px 2px';assert(style.margin==='1px 2px'&&style['margin']==='1px 2px');"
        "assert(box.getAttribute('style').indexOf('margin:1px 2px')>=0);"
        "style['border-top']='bogus';assert(style.borderTop==='');"
        "assert('webkitTransform' in style&&'WebkitTransform' in style&&'-webkit-transform' in style);"
        "style.webkitTransform='none';assert(style['-webkit-transform']==='none'&&style.WebkitTransform==='none');"
        "assert('gridGap' in style&&'grid-gap' in style&&!('-webkit-margin' in style));"
        "assert(!('display-x' in style)&&!Object.prototype.hasOwnProperty.call(style,'margin'));",
        "classic-inline-style-names");
    /* CSSOM's list of an inline style's declarations: valid ones in order,
     * a shorthand as its longhands, a custom property by its name, each
     * once; length, item(), indices and an iterator over it. Setting or
     * removing a name takes away the declarations it covers, and a longhand
     * removed from inside a shorthand is left there, as DEBTS.md books
     * (the shorthand would have to be written back as text). The last
     * case is shaped like chatgpt.com's style bookkeeping, which spreads a
     * style to learn its names and removes each from another. */
    dom_script(&f,CLASSIC_ASSERT
        "var box=document.createElement('div');document.body.appendChild(box);var style=box.style;"
        "assert(style.length===0&&style.item(0)===''&&style[0]===undefined&&[...style].length===0);"
        "assert(style.parentRule===null);"
        "style.cssText='color:red; margin:0; --Brand:blue; color:green; bogus:1; width:';"
        "assert(style.length===6&&style[0]==='color'&&style.item(1)==='margin-top'&&style[4]==='margin-left');"
        "assert(style[5]==='--Brand'&&style.item(6)===''&&style[6]===undefined&&style.item(-1)==='');"
        "assert([...style].join()==='color,margin-top,margin-right,margin-bottom,margin-left,--Brand');"
        "assert(Object.keys(style).join()==='0,1,2,3,4,5'&&(5 in style)&&!(6 in style));"
        "style[0]='width';assert(style[0]==='color');var refused=false;"
        "try{(function(){'use strict';style[0]='width'})()}catch(e){refused=e instanceof TypeError}"
        "assert(refused&&style[0]==='color');"
        "assert(Array.prototype.indexOf.call(style,'--Brand')===5);"
        "style.removeProperty('margin');assert(style.length===2&&style[1]==='--Brand');"
        "style.cssText='margin-top:1px;color:red;margin-left:2px';style.margin='3px';"
        "assert(style.length===5&&style[0]==='color'&&box.getAttribute('style').indexOf('margin-top')<0);"
        "style.cssText='margin:4px;color:red';style.removeProperty('margin-top');"
        "assert(style.margin==='4px'&&style.length===5);"
        "style.cssFloat='left';assert(style.cssFloat==='left'&&style.getPropertyValue('float')==='left');"
        "style.cssText='';"
        "function yr(e,t){if(!e||!t)return e??void 0;let n=document.createElement('div').style;n.cssText=t;"
        "let a=[...n];n.cssText=e;for(let e of a)n.removeProperty(e);return n.cssText||void 0}"
        "var left=yr('display:none; width:5px; --gap:1px','width:9px;--gap:2px');"
        "assert(left.indexOf('display:none')===0&&left.indexOf('width')<0&&left.indexOf('--gap')<0);"
        "assert(yr('margin-top:4px;color:red','margin:0').indexOf('margin')<0);"
        "assert(yr('width:1px','width:2px')===undefined);",
        "classic-inline-style-list");
    dom_script(&f,CLASSIC_ASSERT
        "assert(window.event===undefined);var outer=new Event('outer'),inner=new Event('inner');"
        "document.body.addEventListener('inner',function(e){assert(event===e&&e===inner)});"
        "document.body.addEventListener('outer',function(e){assert(event===outer);"
        "document.body.dispatchEvent(inner);assert(event===outer)});"
        "document.body.dispatchEvent(outer);assert(window.event===undefined);",
        "classic-nested-window-event");
    dom_script(&f,CLASSIC_ASSERT
        "assert(!Object.getOwnPropertyDescriptor(Function.prototype,'arguments').enumerable);"
        "function legacy(a){var first=legacy.arguments;assert(first.length===2&&first[0]===7&&first[1]===9);"
        "first[0]=100;assert(a===7&&legacy.arguments[0]===7);return first;}"
        "var snapshot=legacy(7,9);assert(snapshot[1]===9&&legacy.arguments===null);"
        "function recurse(n){assert(recurse.arguments[0]===n);if(n)recurse(n-1);assert(recurse.arguments[0]===n)}recurse(5);"
        "function strictLegacy(){'use strict';return strictLegacy.arguments}"
        "var threw=false;try{strictLegacy()}catch(e){threw=e instanceof TypeError}assert(threw);"
        "var arrow=()=>{};threw=false;try{arrow.arguments}catch(e){threw=e instanceof TypeError}assert(threw);",
        "classic-legacy-function-arguments");
    fixture_free(&f);
}

#undef CLASSIC_ASSERT
