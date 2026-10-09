/* Consumer-shaped regressions for the classic page surface. The snippets
 * are independent fixtures for the API patterns, not copies of site code. */
#define CLASSIC_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}"
static void dom_classic_cases(void)
{
    DomFixture f=fixture_new(NULL);
    if(!fixture_ready(&f)) return;
    dom_script(&f,CLASSIC_ASSERT
        "var b=document.body,i=document.getElementById('field'),frag=document.createDocumentFragment();"
        "assert(b instanceof Element&&b instanceof HTMLElement&&b instanceof Node&&document instanceof Document);"
        "assert(i instanceof HTMLInputElement&&i instanceof Element&&!(b instanceof HTMLInputElement));"
        "assert(frag instanceof DocumentFragment&&document.createTextNode('t') instanceof CharacterData);"
        "assert(new Image() instanceof HTMLImageElement&&Image.prototype===HTMLImageElement.prototype);"
        "assert(Object.getPrototypeOf(b)===HTMLElement.prototype||b instanceof HTMLElement);"
        "var bad=false;try{new Element()}catch(e){bad=e instanceof TypeError&&e.message==='Illegal constructor'}assert(bad);"
        "bad=false;try{Element()}catch(e){bad=e instanceof TypeError}assert(bad);"
        "assert(!Object.keys(window).includes('Element')&&typeof HTMLAnchorElement==='undefined');"
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
