/* CUSTOM ELEMENTS (libdom/custom.c; docs/design/pending/CUSTOM_ELEMENTS.md):
 * the registry's members and their checks in the standard's order, the
 * HTMLElement constructor, upgrades on define and on insertion, synchronous
 * construction in createElement, the lifecycle callbacks, clone, upgrade(),
 * a second document's adoption, and the reactions a change between tasks
 * leaves owed, which the host runs. `L` is each script's log.
 *
 * Until [CEReactions] wraps the DOM's own members (cut b2), a callback a
 * plain DOM call causes runs at the microtask checkpoint after the script,
 * which is the end of dom_script's run: such a callback is looked for in
 * the next script. define, upgrade, createElement and cloneNode push their
 * own element queue and run what they queued before they return. */

#define CE_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(f,name){try{f()}catch(e){return e.name===name}return false}"

/* A script as yonder runs one: the run, then its end settled, which takes
 * what the binding reported meanwhile (a constructor's failure). */
static void custom_script(DomFixture *f, const char *source, const char *name)
{
    dom_script(f, source, name);
    os64_js_outcome_t outcome;
    os64_dom_script_ended(f->binding, name, &outcome);
}

/* The window's document, changed as the parser would change it: by a verb,
 * between tasks. */
static void custom_parser_insert(DomFixture *f, const char *name, const char *id)
{
    int64_t status;
    os64_html_node_t *element = os64_html_create_element(f->document, OS64_HTML_NS_HTML, name, &status);
    check(element != NULL && os64_html_set_attr(f->document, element, "id", id, strlen(id)) == OS64_HTML_OK &&
              os64_html_insert(f->document, f->document->body, element, NULL) == OS64_HTML_OK,
          "custom: an element inserted between tasks");
}

static void dom_custom_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;

    custom_script(&f, CE_ASSERT
        "assert(customElements instanceof CustomElementRegistry&&customElements===window.customElements);"
        "assert(throws(function(){new CustomElementRegistry()},'TypeError'),'the registry interface constructs nothing');"
        "assert(throws(function(){new HTMLElement()},'TypeError'),'HTMLElement itself is an Illegal constructor');"
        "assert(throws(function(){HTMLElement()},'TypeError'),'HTMLElement is not called without new');"
        "class Stray extends HTMLElement{};"
        "assert(throws(function(){new Stray()},'TypeError'),'a class no definition names constructs nothing');"
        "var d=Object.getOwnPropertyDescriptor(CustomElementRegistry.prototype,'define');"
        "assert(d&&d.writable&&d.configurable,'define is a writable, configurable operation, as the binding makes each');"
        "assert(d.value.length===2&&d.value.name==='define','define\\'s length and name: '+d.value.length+' '+d.value.name);",
        "custom-surface");

    /* define's checks, in the standard's order, and what each throws. */
    custom_script(&f, CE_ASSERT
        "class A extends HTMLElement{};"
        "assert(throws(function(){customElements.define('nohyphen',A)},'SyntaxError'),'an invalid name');"
        "assert(throws(function(){customElements.define('font-face',A)},'SyntaxError'),'a reserved name');"
        "assert(throws(function(){customElements.define('x-arrow',()=>{})},'TypeError'),'not a constructor');"
        "assert(throws(function(){customElements.define('x-thing',1)},'TypeError'),'not a function');"
        "assert(throws(function(){customElements.define('x-ext',A,{extends:'p'})},'NotSupportedError'),"
        "'customized built-ins are not supported');"
        "function NoProto(){};NoProto.prototype=1;"
        "assert(throws(function(){customElements.define('x-np',NoProto)},'TypeError'),'a prototype that is no object');"
        "function BadCb(){};BadCb.prototype={connectedCallback:5};"
        "assert(throws(function(){customElements.define('x-bad-cb',BadCb)},'TypeError'),'a callback that is no function');"
        "var order=[];function Read(){};"
        "Read.prototype=new Proxy({},{get:function(t,k){order.push(String(k));return undefined}});"
        "customElements.define('x-read',Read);"
        "assert(order.join()==='connectedCallback,disconnectedCallback,adoptedCallback,attributeChangedCallback',"
        "'the callbacks are read in order: '+order.join());"
        "var asked=0;function Obs(){};Obs.prototype={};"
        "Object.defineProperty(Obs,'observedAttributes',{get:function(){asked++;return['a']}});"
        "customElements.define('x-obs',Obs);assert(asked===0,'observedAttributes is read only for a callback');"
        "assert(throws(function(){customElements.define('x-read',A)},'NotSupportedError'),'a name defined twice');"
        "assert(throws(function(){customElements.define('x-again',Read)},'NotSupportedError'),'a constructor twice');"
        "var nested;function Outer(){};"
        "Outer.prototype={get connectedCallback(){try{customElements.define('x-inner',A)}catch(e){nested=e.name}}};"
        "customElements.define('x-outer',Outer);"
        "assert(nested==='NotSupportedError','define inside a define');"
        "assert(customElements.get('x-inner')===undefined);",
        "custom-define-checks");

    /* get, getName, whenDefined. */
    custom_script(&f, CE_ASSERT
        "class G extends HTMLElement{};"
        "var early=customElements.whenDefined('x-g');"
        "assert(early===customElements.whenDefined('x-g'),'one promise for one name');"
        "window.resolvedWith=null;early.then(function(c){resolvedWith=c});"
        "window.rejectedWith=null;customElements.whenDefined('nope').catch(function(e){rejectedWith=e.name});"
        "customElements.define('x-g',G);"
        "assert(customElements.get('x-g')===G&&customElements.get('x-none')===undefined);"
        "assert(customElements.getName(G)==='x-g'&&customElements.getName(function(){})===null);"
        "assert(throws(function(){customElements.getName(1)},'TypeError'));"
        "window.lateWith=null;customElements.whenDefined('x-g').then(function(c){lateWith=c});",
        "custom-registry-reads");
    custom_script(&f, CE_ASSERT
        "assert(resolvedWith===customElements.get('x-g'),'the promise resolved with the constructor');"
        "assert(lateWith===resolvedWith,'a defined name resolves at once');"
        "assert(rejectedWith==='SyntaxError','an invalid name rejects');",
        "custom-when-defined");

    /* Upgrade on define: the document's elements, in tree order, before
     * define returns, with their attributes and connection queued. */
    custom_script(&f, CE_ASSERT
        "window.L=[];var box=document.getElementById('box');"
        "box.innerHTML='<x-up id=u1 a=1 b=2></x-up><p><x-up id=u2></x-up></p>';"
        "var u1=document.getElementById('u1');"
        "assert(!u1.matches(':defined')&&document.querySelector('x-up:not(:defined)')===u1);"
        "class Up extends HTMLElement{"
        "constructor(){super();L.push('ctor '+this.id+' '+(this instanceof Up))}"
        "static get observedAttributes(){return['a']}"
        "attributeChangedCallback(n,o,v,ns){L.push('attr '+this.id+' '+n+' '+o+' '+v+' '+ns)}"
        "connectedCallback(){L.push('connected '+this.id)}"
        "disconnectedCallback(){L.push('disconnected '+this.id)}};"
        "customElements.define('x-up',Up);"
        "assert(L.join('|')==='ctor u1 true|attr u1 a null 1 null|connected u1|ctor u2 true|connected u2',L.join('|'));"
        "assert(u1 instanceof Up&&u1.matches(':defined')&&!document.querySelector(':not(:defined)'));"
        "L=[];u1.setAttribute('a','3');u1.setAttribute('b','4');u1.setAttribute('a','3');"
        "assert(L.length===0,'a plain DOM call queues for the checkpoint');",
        "custom-upgrade-on-define");
    custom_script(&f, CE_ASSERT
        "assert(L.join('|')==='attr u1 a 1 3 null|attr u1 a 3 3 null',L.join('|'));"
        "L=[];document.getElementById('u1').remove();",
        "custom-attribute-callbacks");
    custom_script(&f, CE_ASSERT
        "assert(L.join('|')==='disconnected u1',L.join('|'));L=[];"
        "var made=new (customElements.get('x-up'))();"
        "assert(made.localName==='x-up'&&made.matches(':defined')&&L.join()==='ctor  true');"
        "L=[];made.id='m';document.body.appendChild(made);",
        "custom-new");
    custom_script(&f, CE_ASSERT
        "assert(L.join('|')==='connected m',L.join('|'));L=[];",
        "custom-new-connected");

    /* Synchronous construction in createElement, and what it answers when
     * the constructor fails: reported, an HTMLUnknownElement, failed. */
    custom_script(&f, CE_ASSERT
        "window.errors=[];window.onerror=function(m){errors.push(m)};"
        "class Sync extends HTMLElement{constructor(){super();this.made=true}};"
        "customElements.define('x-sync',Sync);"
        "var s=document.createElement('x-sync');"
        "assert(s instanceof Sync&&s.made&&s.matches(':defined'));"
        "var ns=document.createElementNS('http://www.w3.org/1999/xhtml','x-sync');"
        "assert(ns instanceof Sync,'createElementNS in HTML\\'s namespace constructs too');"
        "class Bad extends HTMLElement{constructor(){super();throw new RangeError('no')}};"
        "customElements.define('x-bad',Bad);"
        "var b=document.createElement('x-bad');"
        "assert(b instanceof HTMLUnknownElement&&!(b instanceof Bad)&&b.localName==='x-bad');"
        "assert(!b.matches(':defined')&&errors.length===1&&/RangeError/.test(errors[0]),errors.join());"
        "class Other extends HTMLElement{constructor(){super();return document.createElement('p')}};"
        "customElements.define('x-other',Other);"
        "var o=document.createElement('x-other');"
        "assert(o instanceof HTMLUnknownElement&&errors.length===2&&/NotSupportedError/.test(errors[1]),errors.join());",
        "custom-create-element");
    char failure[256];
    check(os64_dom_custom_failure(f.binding, "x-bad", failure, sizeof(failure)) && strstr(failure, "RangeError"),
          "custom: the census reads a failed constructor's first failure by name");
    check(!os64_dom_custom_failure(f.binding, "x-sync", failure, sizeof(failure)) &&
              !os64_dom_custom_failure(f.binding, "x-undefined", failure, sizeof(failure)),
          "custom: a constructor that never failed, or no definition, has no failure");

    /* An upgrade that fails: the element is failed, with nothing it queued
     * run. Its prototype is what super() made it, if super() ran: the
     * standard does not take it back. And the construction stack's marker. */
    custom_script(&f, CE_ASSERT
        "L=[];errors=[];var box=document.getElementById('box');"
        "box.innerHTML='<x-throws id=t1 a=1></x-throws><x-early id=t0></x-early>"
        "<x-twice id=t2></x-twice><x-swap id=t3></x-swap>';"
        "class Throws extends HTMLElement{static get observedAttributes(){return['a']}"
        "constructor(){super();throw Error('upgrade fails')}"
        "attributeChangedCallback(){L.push('attr')}connectedCallback(){L.push('connected')}};"
        "customElements.define('x-throws',Throws);"
        "var t1=document.getElementById('t1');"
        "assert(t1 instanceof Throws&&!t1.matches(':defined'),'failed after super(): its prototype stays');"
        "assert(L.length===0&&errors.length===1,'its queued callbacks went with it: '+L.join());"
        "class Early extends HTMLElement{constructor(){throw Error('before super')}};"
        "customElements.define('x-early',Early);"
        "var t0=document.getElementById('t0');"
        "assert(!(t0 instanceof Early)&&t0 instanceof HTMLElement&&!t0.matches(':defined')&&errors.length===2,"
        "'failed before super(): still HTMLElement\\'s');"
        "errors=[];"
        "var inner;class Twice extends HTMLElement{constructor(){super();try{new Twice()}catch(e){inner=e.name}}};"
        "customElements.define('x-twice',Twice);"
        "assert(inner==='InvalidStateError','a second construction during an upgrade hits the marker');"
        "assert(document.getElementById('t2') instanceof Twice,'the marker\\'s element is upgraded');"
        "class Swap extends HTMLElement{constructor(){super();return {}}};"
        "customElements.define('x-swap',Swap);"
        "assert(!document.getElementById('t3').matches(':defined')&&errors.length===1&&/TypeError/.test(errors[0]),'a constructor returning another object: '+errors.join('; '));",
        "custom-upgrade-failures");
    custom_script(&f, CE_ASSERT
        "L=[];errors=[];var t1=document.getElementById('t1');"
        "t1.setAttribute('a','2');t1.remove();document.getElementById('box').appendChild(t1);",
        "custom-failed-again");
    custom_script(&f, CE_ASSERT
        "assert(L.length===0&&errors.length===0,'a failed element is not tried again, and observes nothing: '+"
        "L.join()+' '+errors.join());",
        "custom-failed-stays");
    /* A tree no document holds connects nothing and disconnects nothing. */
    custom_script(&f, CE_ASSERT
        "L=[];var holder=document.createElement('div');var made=new (customElements.get('x-up'))();"
        "holder.appendChild(made);made.remove();",
        "custom-detached");
    custom_script(&f, CE_ASSERT
        "assert(L.join('|')==='ctor  true','no connected or disconnected in a detached tree: '+L.join('|'));",
        "custom-detached-quiet");

    /* Clone and upgrade(): elements that are not connected. */
    custom_script(&f, CE_ASSERT
        "L=[];class Cl extends HTMLElement{constructor(){super();L.push('ctor')}connectedCallback(){L.push('conn')}};"
        "var holder=document.createElement('div');holder.innerHTML='<x-cl></x-cl>';"
        "assert(!(holder.firstChild instanceof Cl));"
        "customElements.define('x-cl',Cl);"
        "assert(!(holder.firstChild instanceof Cl)&&L.length===0,'define upgrades the document\\'s elements alone');"
        "var copy=holder.cloneNode(true);"
        "assert(copy.firstChild instanceof Cl&&L.join()==='ctor','a clone comes back upgraded, not connected');"
        "customElements.upgrade(holder);"
        "assert(holder.firstChild instanceof Cl&&L.join()==='ctor,ctor','upgrade() tries its inclusive descendants');"
        "assert(throws(function(){customElements.upgrade(1)},'TypeError'));",
        "custom-clone-upgrade");

    /* A second document has no registry; an element moved into one is
     * adopted, and its callbacks say so, adoption before connection. */
    custom_script(&f, CE_ASSERT
        "L=[];var other=document.implementation.createHTMLDocument('');"
        "var plain=other.createElement('x-up');"
        "assert(!(plain instanceof customElements.get('x-up')),'a second document constructs nothing');"
        "class Ad extends HTMLElement{connectedCallback(){L.push('conn '+(this.ownerDocument===document))}"
        "disconnectedCallback(){L.push('disc')}"
        "adoptedCallback(o,n){L.push('adopted '+(o===document)+' '+(n===window.otherDoc))}};"
        "customElements.define('x-ad',Ad);window.otherDoc=other;"
        "window.plainThere=other.createElement('x-up');other.body.appendChild(plainThere);"
        "var a=document.createElement('x-ad');document.body.appendChild(a);",
        "custom-adopt-setup");
    custom_script(&f, CE_ASSERT
        "assert(!(plainThere instanceof customElements.get('x-up')),'an insertion in a second document upgrades nothing');"
        "assert(L.join('|')==='conn true',L.join('|'));L=[];"
        "otherDoc.body.appendChild(document.querySelector('x-ad'));",
        "custom-adopt-move");
    custom_script(&f, CE_ASSERT
        "assert(L.join('|')==='disc|adopted true true|conn false',L.join('|'));",
        "custom-adopted");

    /* Between tasks: a change the parser would make queues an upgrade with
     * no checkpoint coming. It is owed, due now on the timer clock, and the
     * host's run upgrades it. */
    custom_script(&f, CE_ASSERT "class Late extends HTMLElement{};customElements.define('x-late',Late);", "custom-late");
    check(!os64_dom_reactions_owed(f.binding), "custom: nothing owed while the page waits");
    custom_parser_insert(&f, "x-late", "late");
    check(os64_dom_reactions_owed(f.binding) && os64_dom_timer_next(f.binding) == 0,
          "custom: an insertion between tasks leaves its upgrade owed, due now");
    os64_js_outcome_t outcome;
    bool ran = os64_dom_reactions_run(f.binding, &outcome);
    if (!ran || outcome.status != OS64_JS_OK)
        fprintf(stderr, "custom: reactions run %d status %d: %s\n", ran, outcome.status, outcome.message);
    check(ran && outcome.status == OS64_JS_OK && !os64_dom_reactions_owed(f.binding) &&
              !os64_dom_reactions_run(f.binding, &outcome),
          "custom: the host runs what is owed, once");
    custom_script(&f, CE_ASSERT "assert(document.getElementById('late') instanceof customElements.get('x-late'));",
               "custom-late-upgraded");
    custom_parser_insert(&f, "x-late", "later");
    check(os64_dom_timer_fire(f.binding, 0, &outcome) && outcome.status == OS64_JS_OK &&
              !os64_dom_reactions_owed(f.binding),
          "custom: a timer turn runs what is owed first, as the backstop");
    custom_script(&f, CE_ASSERT "assert(document.getElementById('later') instanceof customElements.get('x-late'));",
               "custom-late-backstop");
    check(os64_dom_custom_visits(f.binding) > 0, "custom: define's searches are counted for the page's record");

    /* Left owed at teardown: the record's hold goes with the binding. The
     * document outlives the binding, and a verb on it after the binding is
     * freed reaches no listener. */
    custom_parser_insert(&f, "x-late", "left");
    check(os64_dom_reactions_owed(f.binding), "custom: owed at teardown");
    os64_dom_drain(f.binding);
    os64_js_destroy(f.runtime);
    os64_dom_free(f.binding);
    f.binding = NULL;
    f.runtime = NULL;
    custom_parser_insert(&f, "x-late", "after");
    fixture_free(&f);

    /* With the switch off there is no registry and no listener. */
    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    custom_script(&f, CE_ASSERT "assert(typeof customElements==='undefined');", "custom-off");
    custom_parser_insert(&f, "x-late", "off");
    check(!os64_dom_reactions_owed(f.binding), "custom: nothing is owed with the switch off");
    fixture_free(&f);
}
#undef CE_ASSERT
