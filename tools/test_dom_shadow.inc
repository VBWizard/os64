/* SHADOW_DOM.md § 7: shadow trees' script surface (libdom/shadow.c), behind
 * options.shadow_trees. The trees themselves are libhtml's and held by its
 * own suite; these cases hold what a page reaches. */

/* libdom's internal.h: the harness links the binding's own objects. */
const char *d_shadow_name(size_t index);

#define SHADOW_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throwsName(f,n){try{f()}catch(e){return e.name===n||(n==='TypeError'&&e instanceof TypeError)}return false;}"

static void dom_shadow_switch_cases(void)
{
    /* Off by default: no name of the surface exists, so a page that tests
     * for one takes its fallback. */
    check(!os64_dom_default_options().shadow_trees, "shadow: the switch is off unless a host turns it on");
    DomFixture f = fixture_new(NULL);
    if (fixture_ready(&f)) {
        dom_script(&f, SHADOW_ASSERT
            "assert(typeof ShadowRoot==='undefined'&&typeof HTMLSlotElement==='undefined','interfaces');"
            "var e=Element.prototype;assert(!('attachShadow' in e)&&!('shadowRoot' in e)&&!('assignedSlot' in e)"
            "&&!('slot' in e),'Element members');assert(!('assignedSlot' in Text.prototype),'Text member');"
            "assert(Object.getPrototypeOf(document.createElement('slot'))===HTMLElement.prototype,"
            "'a slot keeps HTMLElement.prototype');assert(!('composed' in Event.prototype),'Event.composed');"
            "assert(!('onslotchange' in e)&&!('onslotchange' in window)&&!('onslotchange' in document),'onslotchange');"
            "globalThis.ran=0;var se=document.createElement('div');se.setAttribute('onslotchange','globalThis.ran++');"
            "document.body.appendChild(se);se.dispatchEvent(new Event('slotchange'));"
            "assert(ran===0,'an onslotchange attribute is no handler');",
            "shadow-off");
        fixture_free(&f);
    }
    os64_dom_options_t options = os64_dom_default_options();
    options.shadow_trees = true;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    for (size_t i = 0; d_shadow_name(i) != NULL; i++) {
        char script[384];
        snprintf(script, sizeof(script), SHADOW_ASSERT "assert(typeof %s==='function');", d_shadow_name(i));
        dom_script(&f, script, d_shadow_name(i));
    }
    dom_script(&f, SHADOW_ASSERT
        "assert(throwsName(function(){new ShadowRoot()},'TypeError')&&throwsName(function(){new HTMLSlotElement()},'TypeError'),"
        "'Illegal constructors');"
        "assert(Object.getPrototypeOf(ShadowRoot.prototype)===DocumentFragment.prototype,'ShadowRoot inherits');"
        "assert(Object.getPrototypeOf(HTMLSlotElement.prototype)===HTMLElement.prototype,'HTMLSlotElement inherits');",
        "shadow-interfaces");
    fixture_free(&f);
    /* The modern switch off takes this surface too. Without modern there
     * are no interface objects to look on, so an element is asked. */
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, SHADOW_ASSERT "var e=document.createElement('div');"
               "assert(typeof ShadowRoot==='undefined'&&!('attachShadow' in e)&&!('shadowRoot' in e));",
               "shadow-modern-off");
    fixture_free(&f);
}

static void dom_shadow_surface_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.shadow_trees = true;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, SHADOW_ASSERT
        "var h=document.createElement('div');document.body.appendChild(h);"
        "var r=h.attachShadow({mode:'open'});"
        "assert(r instanceof ShadowRoot&&r instanceof DocumentFragment&&r.host===h&&r.mode==='open'&&h.shadowRoot===r,'open root');"
        "assert(r.delegatesFocus===false&&r.clonable===false&&r.serializable===false&&r.slotAssignment==='named','defaults');"
        "assert(r.nodeType===11&&r.parentNode===null,'a fragment with no parent');"
        "assert(throwsName(function(){h.attachShadow({mode:'open'})},'NotSupportedError'),'a second root');"
        "assert(throwsName(function(){document.createElement('input').attachShadow({mode:'open'})},'NotSupportedError'),'no host');"
        "assert(throwsName(function(){document.createElement('p').attachShadow({})},'TypeError'),'mode required');"
        "assert(throwsName(function(){document.createElement('p').attachShadow({mode:'sideways'})},'TypeError'),'mode enum');"
        "assert(throwsName(function(){document.createElement('p').attachShadow({mode:'open',slotAssignment:'manual'})},"
        "'NotSupportedError'),'manual slots');"
        "var o=document.createElement('span');var od=o.attachShadow({mode:'open',delegatesFocus:true,clonable:true,serializable:true});"
        "assert(od.delegatesFocus&&od.clonable&&od.serializable,'options kept');"
        "var c=document.createElement('span');var cr=c.attachShadow({mode:'closed'});"
        "assert(c.shadowRoot===null&&cr.mode==='closed','a closed root is not handed out');"
        /* innerHTML, parsed with the host as context */
        "r.innerHTML='<p>x<slot name=a></slot><slot></slot></p>';"
        "assert(r.firstChild.tagName==='P'&&r.innerHTML==='<p>x<slot name=\"a\"></slot><slot></slot></p>','innerHTML');"
        "var s=r.querySelectorAll('slot')[0],d=r.querySelectorAll('slot')[1];"
        "assert(s instanceof HTMLSlotElement&&s.name==='a'&&d.name==='','slots');"
        "var k=document.createElement('b');k.slot='a';h.appendChild(k);var t=document.createTextNode('t');h.appendChild(t);"
        "assert(k.slot==='a'&&k.getAttribute('slot')==='a','slot reflects');"
        "assert(k.assignedSlot===s&&t.assignedSlot===d,'assignedSlot');"
        "assert(s.assignedNodes().length===1&&s.assignedNodes()[0]===k&&d.assignedNodes()[0]===t,'assignedNodes');"
        "assert(d.assignedElements().length===0&&s.assignedElements()[0]===k,'assignedElements');"
        "assert(throwsName(function(){s.assign()},'NotSupportedError'),'assign');"
        /* getRootNode, connectedness and the document */
        "assert(s.getRootNode()===r&&s.getRootNode({composed:true})===document&&k.getRootNode()===document,'getRootNode');"
        "assert(s.isConnected&&s.ownerDocument===document,'connected through the host');"
        /* flatten: a slot assigned to a slot gives its own; fallback */
        "var inner=document.createElement('div');r.appendChild(inner);var ir=inner.attachShadow({mode:'open'});"
        "ir.innerHTML='<slot name=b>fb</slot>';var is=ir.firstChild;"
        "assert(is.assignedNodes({flatten:true}).length===1&&is.assignedNodes({flatten:true})[0].nodeType===3,'fallback flattened');"
        "var fwd=document.createElement('slot');fwd.name='a';fwd.slot='b';inner.appendChild(fwd);s.remove();"
        "assert(is.assignedNodes()[0]===fwd&&is.assignedNodes({flatten:true})[0]===k,'a forwarded slot flattens to its nodes');"
        /* a closed tree's slot is not handed out */
        "var ck=document.createElement('i');c.appendChild(ck);cr.innerHTML='<slot></slot>';"
        "assert(ck.assignedSlot===null&&cr.firstChild.assignedNodes()[0]===ck,'closed assignedSlot');"
        /* clone */
        "assert(throwsName(function(){r.cloneNode()},'NotSupportedError'),'a root is not cloned');"
        "var cl=document.createElement('div');cl.attachShadow({mode:'open',clonable:true}).innerHTML='<u></u>';"
        "var copy=cl.cloneNode(false);assert(copy.shadowRoot&&copy.shadowRoot.firstChild.tagName==='U'&&copy.shadowRoot.clonable,'clonable');"
        "assert(h.cloneNode(true).shadowRoot===null,'not clonable');",
        "shadow-surface");
    /* Custom elements: disabledFeatures, a constructor that attaches, and
     * an instance inside another tree upgraded by define (D2a's crossing). */
    dom_script(&f, SHADOW_ASSERT
        "class N extends HTMLElement{static get disabledFeatures(){return ['shadow']}}customElements.define('x-noshadow',N);"
        "assert(throwsName(function(){document.createElement('x-noshadow').attachShadow({mode:'open'})},'NotSupportedError'),"
        "'disabledFeatures');"
        "class Y extends HTMLElement{constructor(){super();this.attachShadow({mode:'open'}).innerHTML='<i>y</i>'}}"
        "customElements.define('x-yes',Y);var y=document.createElement('x-yes');"
        "assert(y.shadowRoot&&y.shadowRoot.firstChild.textContent==='y','a constructor attaches');"
        "var host=document.createElement('div');document.body.appendChild(host);var hr=host.attachShadow({mode:'open'});"
        "hr.appendChild(document.createElement('x-later'));globalThis.later=0;"
        "customElements.define('x-later',class extends HTMLElement{connectedCallback(){globalThis.later++}});"
        "assert(globalThis.later===1,'define upgrades an instance inside a shadow tree');"
        "var moved=document.createElement('x-later');var h2=document.createElement('div');h2.attachShadow({mode:'open'}).appendChild(moved);"
        "assert(globalThis.later===1,'not connected yet');document.body.appendChild(h2);"
        "assert(globalThis.later===2,'connected with its host');"
        /* A definition that disables shadow, defined after its element got a root: the upgrade fails. */
        "var errs=[];addEventListener('error',function(e){errs.push(e.error&&e.error.name)});"
        "var early=document.createElement('x-late-shut');early.attachShadow({mode:'open'});document.body.appendChild(early);"
        "var built=0;class Shut extends HTMLElement{static get disabledFeatures(){return ['shadow']}"
        "constructor(){super();built++}}customElements.define('x-late-shut',Shut);"
        "assert(built===0&&!(early instanceof Shut)&&errs.join()==='NotSupportedError','a late definition: '+built+' '+errs.join());"
        /* The surface's [CEReactions] setters run their reactions before they return. */
        "var obs=[];customElements.define('x-watch',class extends HTMLElement{static get observedAttributes(){return ['slot']}"
        "attributeChangedCallback(n,o,v){obs.push(n+':'+v)}});var xw=document.createElement('x-watch');xw.slot='a';"
        "assert(obs.join()==='slot:a','Element.slot is [CEReactions]: '+obs.join());"
        "var cbs=0;customElements.define('x-cb',class extends HTMLElement{connectedCallback(){cbs++}});"
        "var ih=document.createElement('div');document.body.appendChild(ih);var ihr=ih.attachShadow({mode:'open'});"
        "ihr.innerHTML='<x-cb></x-cb>';assert(cbs===1,'ShadowRoot.innerHTML is [CEReactions]: '+cbs);",
        "shadow-custom");
    fixture_free(&f);
}

/* SHADOW_DOM.md § 8: the path through a slot and out of a root to its
 * host, retargeting, at-target hosts, clearTargets, closed trees hidden
 * from composedPath, relatedTarget, and what is composed. */
#define SHADOW_TREE SHADOW_ASSERT \
    "var h=document.createElement('div');h.id='sh';document.body.appendChild(h);" \
    "var r=h.attachShadow({mode:'open'});r.innerHTML='<div id=w><slot></slot></div>';" \
    "var w=r.firstChild,s=w.firstChild,k=document.createElement('b');h.appendChild(k);var log=[];" \
    "function nm(x){return x===k?'k':x===h?'h':x===w?'w':x===s?'s':x===null?'null':'?'}" \
    "function L(n){return function(e){log.push(n+':'+nm(e.target)+':'+e.eventPhase)}}" \
    "[[s,'slot'],[w,'w'],[r,'root'],[h,'host'],[document.body,'body']].forEach(function(p){" \
    "p[0].addEventListener('ping',L(p[1]))});"

static void dom_shadow_event_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.shadow_trees = true;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, SHADOW_TREE
        "assert(new Event('x').composed===false&&new Event('x',{composed:true}).composed===true,'Event.composed');"
        "assert(new CustomEvent('x',{composed:true}).composed&&new MouseEvent('x',{composed:true}).composed,"
        "'composed through every EventInit');"
        /* A slotted node's parent is its slot; the whole path sees the light child. */
        "k.dispatchEvent(new Event('ping',{bubbles:true}));"
        "assert(log.join()==='slot:k:3,w:k:3,root:k:3,host:k:3,body:k:3','through the slot: '+log.join());"
        /* Not composed: the path ends at the root, and the target is cleared after. */
        "log=[];var e=new Event('ping',{bubbles:true});w.dispatchEvent(e);"
        "assert(log.join()==='w:w:2,root:w:3','not composed: '+log.join());"
        "assert(e.target===null&&e.composedPath().length===0,'clearTargets');"
        /* Composed: the host is at its target, retargeted. */
        "log=[];e=new Event('ping',{bubbles:true,composed:true});w.dispatchEvent(e);"
        "assert(log.join()==='w:w:2,root:w:3,host:h:2,body:h:3','composed: '+log.join());"
        "assert(e.target===h,'left showing the host');"
        /* A composed event that does not bubble still reaches the host, at its target. */
        "log=[];w.dispatchEvent(new Event('ping',{composed:true}));"
        "assert(log.join()==='w:w:2,host:h:2','non-bubbling: '+log.join());"
        /* Capture runs outside in, the host at its target. */
        "log=[];h.addEventListener('ping',function(e){log.push('cap:'+e.eventPhase)},true);"
        "w.addEventListener('ping',function(e){log.push('wcap:'+e.eventPhase)},true);"
        "w.dispatchEvent(new Event('ping',{composed:true}));"
        "assert(log.join()==='cap:2,wcap:2,w:w:2,host:h:2','capture: '+log.join());",
        "shadow-path");
    dom_script(&f, SHADOW_ASSERT
        "var o=document.createElement('div');document.body.appendChild(o);var or=o.attachShadow({mode:'open'});"
        "or.innerHTML='<i></i>';var oi=or.firstChild;var seen;"
        "document.body.addEventListener('p',function(e){seen=e.composedPath()});"
        "oi.dispatchEvent(new Event('p',{bubbles:true,composed:true}));"
        "assert(seen.length===7&&seen[0]===oi&&seen[1]===or&&seen[2]===o&&seen[6]===window,'an open tree shows: '+seen.length);"
        "var c=document.createElement('div');document.body.appendChild(c);var cr=c.attachShadow({mode:'closed'});"
        "cr.innerHTML='<i></i>';var ci=cr.firstChild,inner;"
        "ci.addEventListener('p',function(e){inner=e.composedPath()});"
        "ci.dispatchEvent(new Event('p',{bubbles:true,composed:true}));"
        "assert(seen.length===5&&seen[0]===c&&seen[4]===window,'a closed tree hides from outside: '+seen.length);"
        "assert(inner.length===7&&inner[0]===ci&&inner[1]===cr,'and not from inside');"
        /* A light child slotted into a closed tree: outside, the slot and root are hidden. */
        "cr.appendChild(document.createElement('slot'));var lk=document.createElement('b');c.appendChild(lk);"
        "lk.dispatchEvent(new Event('p',{bubbles:true}));"
        "assert(seen.length===6&&seen[0]===lk&&seen[1]===c,'a closed slot hides: '+seen.length);"
        /* A descendant of a slotted node: the slot's level is undone, so the light nodes show. */
        "var lb=document.createElement('button');lk.appendChild(lb);lb.dispatchEvent(new Event('p',{bubbles:true}));"
        "assert(seen.length===7&&seen[0]===lb&&seen[1]===lk&&seen[2]===c,'below a slotted node: '+seen.length);"
        /* Forwarded through two closed trees: each slot's level is undone. */
        "var outer=document.createElement('div');document.body.appendChild(outer);"
        "var orr=outer.attachShadow({mode:'closed'});var mid=document.createElement('div');orr.appendChild(mid);"
        "orr.appendChild(document.createElement('slot'));var mr=mid.attachShadow({mode:'closed'});"
        "mr.appendChild(document.createElement('slot'));var fs=document.createElement('slot');mid.appendChild(fs);"
        "var deep=document.createElement('em');outer.appendChild(deep);deep.dispatchEvent(new Event('p',{bubbles:true}));"
        "assert(seen.length===6&&seen[0]===deep&&seen[1]===outer,'through two closed trees: '+seen.length);",
        "shadow-composed-path");
    dom_script(&f, SHADOW_TREE
        /* relatedTarget retargets per step and keeps the outside's answer. */
        "var rel=[];function R(n){return function(e){rel.push(n+':'+nm(e.relatedTarget))}}"
        "s.addEventListener('mouseover',R('slot'));document.body.addEventListener('mouseover',R('body'));"
        "var m=new MouseEvent('mouseover',{bubbles:true,composed:true,relatedTarget:w});k.dispatchEvent(m);"
        "assert(rel.join()==='slot:w,body:h','relatedTarget: '+rel.join());"
        "assert(m.relatedTarget===h&&m.target===k,'after');"
        /* An event whose relatedTarget retargets to its target is not dispatched. */
        "rel=[];h.addEventListener('mouseover',R('host'));"
        "h.dispatchEvent(new MouseEvent('mouseover',{bubbles:true,composed:true,relatedTarget:w}));"
        "assert(rel.length===0,'to itself: '+rel.join());"
        /* ...and ends as any dispatch does: canceled answers false, the flags are reset. */
        "var nd=new MouseEvent('mouseover',{cancelable:true,composed:true,relatedTarget:w});"
        "nd.preventDefault();nd.stopPropagation();"
        "assert(h.dispatchEvent(nd)===false&&nd.cancelBubble===false,'no path still ends the dispatch');"
        /* A checkbox's activation input is composed, its change not. */
        "var heard=[];document.addEventListener('input',function(e){heard.push('input:'+e.composed)});"
        "document.addEventListener('change',function(e){heard.push('change')});"
        "var cbox=document.createElement('input');cbox.type='checkbox';r.appendChild(cbox);cbox.click();"
        "assert(cbox.checked&&heard.join()==='input:true','checkbox input composed: '+heard.join());"
        /* click() is composed, and its activation is found on the path: a
         * submit button around a host is activated by a click inside it. */
        "var cs;r.addEventListener('click',function(e){cs=e.composed});w.click();assert(cs===true,'click() is composed');"
        "var fm=document.createElement('form'),bt=document.createElement('button'),bh=document.createElement('span');"
        "document.body.appendChild(fm);fm.appendChild(bt);bt.appendChild(bh);"
        "bh.attachShadow({mode:'open'}).innerHTML='<i></i>';var sub=0;"
        "fm.addEventListener('submit',function(e){sub++;e.preventDefault()});bh.shadowRoot.firstChild.click();"
        "assert(sub===1,'activation through the host');",
        "shadow-related");
    fixture_free(&f);

    /* activeElement: the document's retargets to the host, the root's names
     * the element inside. */
    options = document_options();
    options.shadow_trees = true;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, SHADOW_ASSERT
        "var h=document.createElement('div');h.id='fh';document.body.appendChild(h);"
        "var r=h.attachShadow({mode:'open'});r.innerHTML='<input>';",
        "shadow-focus-setup");
    os64_html_node_t *host = native_id(f.document->document, "fh");
    document_focused = host != NULL && host->shadow_root != NULL ? host->shadow_root->first_child : NULL;
    document_has_focus = true;
    check(document_focused != NULL, "shadow: the focus fixture");
    dom_script(&f, SHADOW_ASSERT
        "var h=document.getElementById('fh'),r=h.shadowRoot;"
        "assert(document.activeElement===h,'the document sees the host');"
        "assert(r.activeElement===r.firstChild,'the root sees its element');"
        "var o=document.createElement('div');document.body.appendChild(o);"
        "assert(o.attachShadow({mode:'open'}).activeElement===null,'another root sees nothing');",
        "shadow-active-element");
    document_focused = NULL;
    document_has_focus = false;
    fixture_free(&f);
}

/* SHADOW_DOM.md § 4: slotchange at the mutation observers' notify, once a
 * slot's assigned nodes change, bubbling and not composed. */
static void dom_shadow_slotchange_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.shadow_trees = true;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, SHADOW_ASSERT
        "var h=document.createElement('div');document.body.appendChild(h);var r=h.attachShadow({mode:'open'});"
        "r.innerHTML='<slot name=a></slot><slot></slot>';var a=r.firstChild,d=a.nextSibling;"
        "globalThis.sc=[];globalThis.order=[];"
        "r.addEventListener('slotchange',function(e){sc.push((e.target.name||'default')+':'+e.bubbles+':'+e.composed+"
        "':'+e.isTrusted)});"
        "r.onslotchange=function(){order.push('handler')};"
        "h.addEventListener('slotchange',function(){sc.push('host')});"
        "new MutationObserver(function(){order.push('observer')}).observe(h,{childList:true});"
        "var x=document.createElement('b');x.slot='a';h.appendChild(x);"
        "Promise.resolve().then(function(){order.push('after')});"
        "assert(sc.length===0,'not synchronously');",
        "shadow-slotchange-setup");
    dom_script(&f, SHADOW_ASSERT
        "assert(sc.join()==='a:true:false:true','the slot that changed, once: '+sc.join());"
        "assert(order.join()==='observer,handler,after','after the observers, at their notify: '+order.join());"
        "sc=[];h.firstChild.slot='';",
        "shadow-slotchange-move");
    dom_script(&f, SHADOW_ASSERT
        "assert(sc.join()==='a:true:false:true,default:true:false:true','both slots, in tree order: '+sc.join());"
        "sc=[];h.appendChild(document.createElement('i'));h.lastChild.remove();",
        "shadow-slotchange-undone");
    /* A clone is made by no change the dispatcher sees, so its own script
     * holds that the clone alone queues the notify. */
    dom_script(&f, SHADOW_ASSERT
        "assert(sc.length===0,'an assignment back where it was is no change: '+sc.join());"
        "globalThis.cl=document.createElement('div');cl.appendChild(document.createElement('u'));"
        "cl.attachShadow({mode:'open',clonable:true}).innerHTML='<slot></slot>';globalThis.cc=[];",
        "shadow-slotchange-clonable");
    dom_script(&f, SHADOW_ASSERT
        "var copy=cl.cloneNode(true);copy.shadowRoot.addEventListener('slotchange',function(){cc.push('copy')});",
        "shadow-slotchange-clone");
    dom_script(&f, SHADOW_ASSERT "assert(cc.join()==='copy','a clone\\'s first assignment: '+cc.join());",
               "shadow-slotchange-cloned");
    /* onslotchange is GlobalEventHandlers': a slot's own property and its
     * content attribute both fire. */
    dom_script(&f, SHADOW_ASSERT
        "assert('onslotchange' in window&&'onslotchange' in document&&'onslotchange' in document.body,'GlobalEventHandlers');"
        "var g=document.createElement('div');document.body.appendChild(g);var gr=g.attachShadow({mode:'open'});"
        "gr.innerHTML='<slot name=p></slot><slot onslotchange=\"globalThis.byAttr++\"></slot>';"
        "globalThis.byAttr=0;globalThis.byProp=0;gr.firstChild.onslotchange=function(){byProp++};"
        "var gp=document.createElement('i');gp.slot='p';g.appendChild(gp);g.appendChild(document.createElement('u'));",
        "shadow-onslotchange");
    dom_script(&f, SHADOW_ASSERT "assert(byProp===1&&byAttr===1,'property '+byProp+', attribute '+byAttr);",
               "shadow-onslotchange-fired");
    fixture_free(&f);
}

/* A native pointer packet hitting `hit`, which may be inside a shadow
 * tree, where no id reaches. */
static void shadow_pointer(DomFixture *f, const os64_html_node_t *hit, os64_dom_pointer_action_t action, int button,
                           unsigned buttons)
{
    os64_dom_pointer_sample_t sample = {.action = action, .hit = hit,
        .event = {.client_x = 4, .client_y = 4, .button = button, .buttons = buttons}};
    os64_js_outcome_t out;
    check(os64_dom_pointer_dispatch(f->binding, &sample, NULL, NULL, NULL, &out) == OS64_JS_OK,
          "shadow: a pointer packet completes");
}

/* SHADOW_DOM.md § 8, pointer.c: enter and leave are not composed, so a
 * host hears its own mouseenter once however deep the pointer goes; a
 * click whose press and release are in different trees goes to their
 * common element, never a shadow root. */
static void dom_shadow_pointer_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.shadow_trees = true;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, SHADOW_ASSERT
        "document.body.innerHTML='';var h=document.createElement('div');h.id='ph';document.body.appendChild(h);"
        "var r=h.attachShadow({mode:'open'});r.innerHTML='<i></i><div><span></span></div><slot></slot>';"
        "var k=document.createElement('b');k.id='pk';h.appendChild(k);"
        "globalThis.enters=0;globalThis.clicks=[];globalThis.rootClick=0;"
        "h.addEventListener('mouseenter',function(e){enters++;assert(!e.composed,'mouseenter is not composed')});"
        "r.addEventListener('click',function(e){if(e.target===r)rootClick++});"
        "document.addEventListener('click',function(e){clicks.push(e.target===h?'h':e.target&&e.target.nodeName)});",
        "shadow-pointer-setup");
    os64_html_node_t *host = native_id(f.document->document, "ph");
    const os64_html_node_t *root = host != NULL ? host->shadow_root : NULL;
    const os64_html_node_t *italic = root != NULL ? root->first_child : NULL;
    const os64_html_node_t *span = italic != NULL && italic->next != NULL ? italic->next->first_child : NULL;
    const os64_html_node_t *light = native_id(f.document->document, "pk");
    check(span != NULL && light != NULL, "shadow: the pointer fixture");
    if (span != NULL && light != NULL) {
        shadow_pointer(&f, span, OS64_DOM_POINTER_MOVE, -1, 0);
        shadow_pointer(&f, italic, OS64_DOM_POINTER_DOWN, 0, 1);
        shadow_pointer(&f, light, OS64_DOM_POINTER_UP, 0, 0);
        dom_script(&f, SHADOW_ASSERT
            "assert(enters===1,'the host hears its own mouseenter once: '+enters);"
            "assert(rootClick===0&&clicks.join()==='h','a click across trees goes to the host: '+clicks.join());"
            /* Pointer capture's events are composed: the document hears them, retargeted. */
            "globalThis.got=[];var sp=r.querySelector('span');"
            "sp.addEventListener('pointerdown',function(e){sp.setPointerCapture(e.pointerId)});"
            "document.addEventListener('gotpointercapture',function(e){got.push(e.composed+':'+(e.target===h))});"
            "document.addEventListener('lostpointercapture',function(e){got.push('lost:'+(e.target===h))});",
            "shadow-pointer");
        shadow_pointer(&f, span, OS64_DOM_POINTER_DOWN, 0, 1);
        shadow_pointer(&f, span, OS64_DOM_POINTER_UP, 0, 0);
        /* A pointer resting on a root's own child that is removed falls back
         * to the host, never the root. */
        shadow_pointer(&f, italic, OS64_DOM_POINTER_MOVE, -1, 0);
        dom_script(&f, SHADOW_ASSERT
            "assert(got.join()==='true:true,lost:true','capture events cross the root: '+got.join());"
            "var h=document.getElementById('ph'),r=h.shadowRoot;globalThis.rootOut=0;"
            "['pointerout','mouseout','pointerover','mouseover'].forEach(function(n){"
            "r.addEventListener(n,function(e){if(e.target===r||e.relatedTarget===r)rootOut++})});"
            "r.firstChild.remove();",
            "shadow-pointer-remove");
        shadow_pointer(&f, light, OS64_DOM_POINTER_MOVE, -1, 0);
        dom_script(&f, SHADOW_ASSERT "assert(rootOut===0,'no boundary event at the root: '+rootOut);",
                   "shadow-pointer-fallback");
    }
    fixture_free(&f);
}

/* SHADOW_DOM.md § 12, D4b: constructed sheets and adoptedStyleSheets
 * (libdom/adopted.c) — the script's view, and the host's (os64_dom_adopted,
 * the version an adoption moves). */
static void dom_shadow_adopted_cases(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    DomFixture f = fixture_new(&options);
    if (fixture_ready(&f)) {
        dom_script(&f, SHADOW_ASSERT
            "assert(throwsName(function(){new CSSStyleSheet()},'TypeError'),'not constructible with the switch off');"
            "assert(!('adoptedStyleSheets' in document)&&!('replaceSync' in CSSStyleSheet.prototype),'no adoption');",
            "adopted-off");
        fixture_free(&f);
    }
    options.shadow_trees = true;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, SHADOW_ASSERT
        "var s=new CSSStyleSheet();assert(s instanceof CSSStyleSheet&&s instanceof StyleSheet,'constructs');"
        "assert(s.ownerNode===null&&s.href===null&&s.type==='text/css'&&s.disabled===false,'a constructed sheet');"
        "assert(s.replaceSync('p{color:red}')===undefined,'replaceSync');"
        "var st=document.createElement('style');document.head.appendChild(st);"
        "assert(throwsName(function(){st.sheet.replaceSync('x')},'NotAllowedError'),'a node\\'s sheet refuses');"
        "var a=document.adoptedStyleSheets;assert(Array.isArray(a)&&a.length===0,'an empty array');"
        "assert(document.adoptedStyleSheets===a,'the same array each read');"
        "document.adoptedStyleSheets=[s];assert(a.length===1&&a[0]===s,'assignment fills the array');"
        "assert(throwsName(function(){document.adoptedStyleSheets=[st.sheet]},'NotAllowedError'),'a node\\'s sheet');"
        "assert(throwsName(function(){document.adoptedStyleSheets=[{}]},'TypeError'),'not a sheet');"
        "assert(throwsName(function(){document.adoptedStyleSheets=7},'TypeError'),'not a sequence');"
        "assert(a.length===1&&a[0]===s,'a refused assignment changes nothing');"
        "assert(throwsName(function(){a.push({})},'TypeError'),'push checks');"
        "var t=new CSSStyleSheet();t.replaceSync('i{}');a.push(t);assert(a.length===2,'push adopts');"
        "var h=document.createElement('div');document.body.appendChild(h);var r=h.attachShadow({mode:'open'});"
        "r.adoptedStyleSheets=[t,s];assert(r.adoptedStyleSheets.length===2&&r.adoptedStyleSheets[0]===t,'a root\\'s own');"
        "globalThis.replaced=null;s.replace('b{}').then(function(v){globalThis.replaced=v});"
        /* replace does not throw: it rejects. */
        "globalThis.rejected=null;var pr=st.sheet.replace('x');assert(pr instanceof Promise,'a refused replace is a Promise');"
        "pr.catch(function(e){globalThis.rejected=e.name});"
        /* An observable array: no holes, no accessors, never frozen. */
        "var o=r.adoptedStyleSheets;'use strict';"
        "assert(!Reflect.set(o,5,s)&&o.length===2,'no index past the length');"
        "assert(!Reflect.set(o,'length',9)&&o.length===2,'length does not grow');"
        "assert(!Reflect.deleteProperty(o,0)&&o.length===2,'only the last index deletes');"
        "assert(Reflect.deleteProperty(o,1)&&o.length===1&&o[0]===t,'the last index deletes');"
        "assert(!Reflect.defineProperty(o,0,{get:function(){return s}}),'no accessor at an index');"
        "assert(throwsName(function(){Object.freeze(o)},'TypeError')&&Object.isExtensible(o),'never frozen');"
        "assert(Reflect.set(o,'length',0)&&o.length===0,'length shrinks');"
        /* Only the document a sheet was made for adopts it. */
        "var other=document.implementation.createHTMLDocument('');"
        "assert(throwsName(function(){other.adoptedStyleSheets=[s]},'NotAllowedError'),'another document refuses');"
        /* The array's intrinsics are its own: a page's replacements change nothing. */
        "var keep=Array.from;Array.from=function(){throw Error('page')};"
        "r.adoptedStyleSheets=[s];Array.from=keep;assert(r.adoptedStyleSheets[0]===s,'Array.from replaced');",
        "adopted-script");
    dom_script(&f, SHADOW_ASSERT "assert(globalThis.replaced!==null,'replace resolves with the sheet');"
               "assert(globalThis.rejected==='NotAllowedError','a refused replace rejects with NotAllowedError');",
               "adopted-replace");
    /* The host's view: each root's sheets in order, their text and
     * generation; an adoption and a replace each move the version. */
    const os64_html_node_t *doc = f.document->document;
    os64_dom_adopted_sheet_t got[4];
    size_t n = os64_dom_adopted(f.binding, doc, got, 4);
    check(n == 2 && got[0].length == 3 && memcmp(got[0].text, "b{}", 3) == 0 && got[0].generation == 2 &&
          got[1].length == 3 && memcmp(got[1].text, "i{}", 3) == 0 && got[0].key != got[1].key,
          "adopted: the document's sheets, their text and generation");
    const os64_html_node_t *host = f.document->body->last_child;
    size_t nr = host != NULL && host->shadow_root != NULL ? os64_dom_adopted(f.binding, host->shadow_root, got, 4) : 0;
    check(nr == 1 && got[0].length == 3 && memcmp(got[0].text, "b{}", 3) == 0,
          "adopted: a shadow root's sheets, as its array last left them");
    uint64_t before = os64_html_version(f.document);
    dom_script(&f, "document.adoptedStyleSheets[0].replaceSync('q{}');", "adopted-touch-text");
    uint64_t after_text = os64_html_version(f.document);
    dom_script(&f, "document.adoptedStyleSheets.pop();", "adopted-touch-list");
    check(after_text > before && os64_html_version(f.document) > after_text,
          "adopted: a replace and an adoption each move the version");
    check(os64_dom_adopted(f.binding, doc, got, 4) == 1, "adopted: pop takes a sheet out");
    fixture_free(&f);
}

/* Every allocation an adoption makes, failed in turn: whatever the page is
 * told, the array its script sees and the list the host reads stay one
 * list (Codex #488: the list's room is taken before the array changes). */
static void dom_shadow_adopted_sweep(void)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.shadow_trees = true;
    static const char kSetup[] =
        "globalThis.sheets=[];for(var i=0;i<9;i++){var s=new CSSStyleSheet();s.replaceSync('p{}');sheets.push(s)}"
        "document.adoptedStyleSheets=sheets.slice(0,8);";
    static const char kStep[] = "try{document.adoptedStyleSheets.push(sheets[8])}catch(e){}";
    static const char kRead[] = "document.body.setAttribute('data-n',String(document.adoptedStyleSheets.length));";
    os64_js_outcome_t outcome;
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, kSetup, "adopted-sweep-setup");
    size_t start = dom_attempts;
    dom_script(&f, kStep, "adopted-sweep-measure");
    size_t count = dom_attempts - start;
    fixture_free(&f);
    check(count > 0, "adopted sweep: a push allocates");
    for (size_t cut = 1; cut <= count; cut++) {
        f = fixture_new(&options);
        if (!fixture_ready(&f)) break;
        dom_script(&f, kSetup, "adopted-sweep-setup");
        dom_fail_at = dom_attempts + cut;
        (void)os64_js_run(f.runtime, kStep, sizeof(kStep) - 1, "adopted-sweep-step", &outcome);
        dom_fail_at = 0;
        dom_sweeps++;
        if (os64_js_run(f.runtime, kRead, sizeof(kRead) - 1, "adopted-sweep-read", &outcome) == OS64_JS_OK) {
            const os64_html_attr_t *seen = os64_html_attr(f.document->body, "data-n");
            size_t script = seen != NULL ? (size_t)strtoul(seen->value, NULL, 10) : 0;
            size_t host = os64_dom_adopted(f.binding, f.document->document, NULL, 0);
            check(seen != NULL && script == host, "adopted sweep: the script's array and the host's list agree");
        }
        fixture_free(&f);
    }
}

static void dom_shadow_cases(void)
{
    dom_shadow_adopted_cases();
    dom_shadow_adopted_sweep();
    dom_shadow_switch_cases();
    dom_shadow_surface_cases();
    dom_shadow_event_cases();
    dom_shadow_slotchange_cases();
    dom_shadow_pointer_cases();
}
