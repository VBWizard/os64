/* Raw globals, native context records and explicit DOM queries are independent
 * oracles for the private Window named-property/prototype integration. */
extern JSValue __real_d_window_named_prototype(os64_dom_t *, JSContext *, JSValueConst);
static bool window_names_install_armed, window_names_native;
static size_t window_names_cut, window_names_allocations;
JSValue __wrap_d_window_named_prototype(os64_dom_t *dom, JSContext *ctx, JSValueConst parent)
{
    if (!window_names_install_armed || !dom->binding_origin)
        return __real_d_window_named_prototype(dom, ctx, parent);
    size_t before = window_names_native ? dom_attempts : attempts;
    size_t *failure = window_names_native ? &dom_fail_at : &fail_at;
    *failure = window_names_cut ? before + window_names_cut : 0;
    JSValue result = __real_d_window_named_prototype(dom, ctx, parent);
    *failure = 0;
    window_names_allocations = (window_names_native ? dom_attempts : attempts) - before;
    return result;
}

static JSValue window_names_lookup_cut(JSContext *ctx, void *opaque)
{
    os64_dom_t *dom = opaque;
    size_t before = window_names_native ? dom_attempts : attempts;
    size_t *failure = window_names_native ? &dom_fail_at : &fail_at;
    *failure = window_names_cut ? before + window_names_cut : 0;
    JSValue result = JS_GetPropertyStr(ctx, dom->window->value, "many");
    *failure = 0;
    window_names_allocations = (window_names_native ? dom_attempts : attempts) - before;
    return result;
}

static void window_names_failure_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        for (unsigned mode = 0; mode < 4; mode++) {
            size_t count = 0;
            for (size_t cut = 0; cut <= count; cut++) {
                CeFixture f;
                if (!authority_open(&f, policy, 0)) { check(false, "Window name cuts: fixture"); ce_close(&f); continue; }
                os64_js_outcome_t out;
                window_names_cut = cut; window_names_native = mode & 1;
                if (mode < 2) {
                    window_names_install_armed = true;
                    os64_dom_child_t child = context_candidate_prepare(&f, 0, &out);
                    window_names_install_armed = false;
                    check(cut ? child.consumed && child.state != OS64_DOM_CHILD_ACTIVE
                              : child.state == OS64_DOM_CHILD_ACTIVE,
                        "Window name cuts: bootstrap refuses without partial binding publication");
                } else {
                    realm_script(ce_realms[0], "document.body.innerHTML='<div id=many></div><form name=many></form>'",
                        "Window name cuts: multiple-match fixture");
                    check(os64_js_realm_task_begin(ce_realms[0], OS64_JS_ABI_ID, "Window names cut", &out) == OS64_JS_OK,
                        "Window name cuts: checked lookup task");
                    JSValue result = JS_UNDEFINED;
                    os64_js_status_t status = os64_js_realm_with_settings(ce_realms[0], ce_realms[0], OS64_JS_ABI_ID,
                        window_names_lookup_cut, f.bindings[0], &result, &out);
                    check(cut ? status != OS64_JS_OK || JS_IsException(result)
                              : status == OS64_JS_OK && JS_IsObject(result),
                        "Window name cuts: collection refusal cannot return a partial value");
                    if (JS_IsException(result)) JS_FreeValue(ce_contexts[0], JS_GetException(ce_contexts[0]));
                    JS_FreeValue(ce_contexts[0], result);
                    check(os64_js_task_end(ce_runtime, OS64_JS_ABI_ID, &out) != OS64_JS_BUSY,
                        "Window name cuts: settings loans unwind");
                    if (cut && window_names_native)
                        realm_script(ce_realms[0], "assert(many.length===2)", "Window name cuts: native refusal retry");
                }
                if (!cut) { count = window_names_allocations; check(count > 0, "Window name cuts: allocation path measured"); }
                ce_close(&f);
                check(live == 0 && dom_live == 0 && !dom_allocations, "Window name cuts: balanced teardown");
            }
            printf("Window name allocation cuts policy %d mode %u: %zu\n", policy, mode, count);
        }
    }
}

static void dom_window_names_cases(void)
{
    window_names_failure_cases();
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; policy++) {
        CeFixture f; DBrowsingContext *c[5]; os64_js_outcome_t out;
        if (!identity_open_mode(&f, policy, c, 3)) { check(false, "Window names: fixture"); ce_close(&f); continue; }
        realm_script(ce_realms[0],
            "globalThis.np=Object.getPrototypeOf(Window.prototype);"
            "assert(Object.getPrototypeOf(window)===Window.prototype);"
            "assert(Object.getPrototypeOf(np)===EventTarget.prototype);"
            "assert(Object.prototype.toString.call(np)==='[object WindowProperties]');"
            "assert(Reflect.ownKeys(np).length===1&&Reflect.ownKeys(np)[0]===Symbol.toStringTag);"
            "let tagDesc=Object.getOwnPropertyDescriptor(np,Symbol.toStringTag);"
            "assert(tagDesc.value==='WindowProperties'&&tagDesc.configurable&&!tagDesc.writable&&!tagDesc.enumerable);"
            "assert(!Reflect.deleteProperty(np,Symbol.toStringTag)&&!Reflect.deleteProperty(np,'absent'));"
            "assert(!Reflect.defineProperty(np,Symbol.toStringTag,tagDesc)&&!Reflect.defineProperty(np,'absent',{value:1}));"
            "typeError(()=>Object.defineProperty(np,'absent',{value:1}));"
            "assert(!Reflect.set(np,'absent',1));np.absent=1;assert(!Object.hasOwn(np,'absent'));"
            "typeError(()=>{'use strict';np.absent=1});typeError(()=>{'use strict';delete np[Symbol.toStringTag]});"
            "for(let p of [np,Window.prototype,EventTarget.prototype]){"
            "assert(Reflect.setPrototypeOf(p,Object.getPrototypeOf(p))&&!Reflect.setPrototypeOf(p,null));"
            "typeError(()=>Object.setPrototypeOf(p,{}));}"
            "assert(Object.isExtensible(np)&&!Reflect.preventExtensions(np));typeError(()=>Object.freeze(np));",
            "Window names: exact prototype chain, class string, immutable links and exotic mutation refusal");
        realm_script(ce_realms[0],
            "document.body.innerHTML='<div id=unique></div><form name=byname></form><img name=picture>'"
                "+'<embed name=plugin><object name=objectName></object><input name=notWindow><div name=notWindow></div>'"
                "+'<svg><g id=svgName></g><a name=notWindow></a></svg><div id=multi></div><form name=multi></form>';"
            "assert(window.unique===document.getElementById('unique')&&unique===window.unique);"
            "for(let n of ['byname','picture','plugin','objectName'])assert(window[n]===document.querySelector('[name='+n+']'),n);"
            "assert(window.svgName===document.getElementById('svgName')&&window.notWindow===undefined);"
            "assert(window['']===undefined&&window['unique\\0suffix']===undefined&&window[Symbol('unique')]===undefined);"
            "assert('unique' in window&&!Object.hasOwn(window,'unique')&&!Object.hasOwn(A,'unique'));"
            "let d=Object.getOwnPropertyDescriptor(np,'unique');"
            "assert(d.value===unique&&d.writable&&d.enumerable&&d.configurable);"
            "assert(!Reflect.ownKeys(window).includes('unique')&&!Object.keys(np).length);"
            "assert(!Object.getOwnPropertyNames(np).length);let seen=false;for(let n in window)if(n==='unique')seen=true;assert(!seen);"
            "assert(Reflect.get(np,'unique',{})===document.getElementById('unique'));"
            "globalThis.list=window.multi;assert(list instanceof HTMLCollection&&list.length===2);"
            "assert(list[0]===document.querySelector('div[id=multi]')&&list[1]===document.querySelector('form[name=multi]'));"
            "let extra=document.createElement('img');extra.name='multi';document.body.prepend(extra);"
            "assert(list.length===3&&list[0]===extra);extra.remove();assert(list.length===2);"
            "list[1].remove();assert(window.multi===list[0]&&list.length===1);"
            "list[0].id='changed';assert(list.length===0&&window.multi===undefined&&window.changed!==undefined);"
            "let host=document.createElement('div');document.body.append(host);"
            "host.attachShadow({mode:'open'}).innerHTML='<i id=shadowOnly></i>';assert(window.shadowOnly===undefined);"
            "let detached=document.createElement('i');detached.id='detachedOnly';assert(window.detachedOnly===undefined);"
            "let unicode=document.createElement('div');unicode.id='caf\xc3\xa9';document.body.append(unicode);assert(window['caf\xc3\xa9']===unicode);",
            "Window names: supported elements, namespaces, inherited descriptors, nonenumeration and live tree membership");
        realm_script(ce_realms[0],
            "let el=document.getElementById('unique');window.unique=17;assert(window.unique===17&&Object.hasOwn(A,'unique'));"
            "assert(Object.getOwnPropertyDescriptor(np,'unique')===undefined);assert(delete window.unique&&window.unique===el);"
            "assert(delete window.unique&&window.unique===el);"
            "let reads=0;Object.defineProperty(Window.prototype,'unique',{get(){reads++;return 23},configurable:true});"
            "assert(!Object.getOwnPropertyDescriptor(np,'unique')&&reads===0&&window.unique===23&&reads===1);"
            "delete Window.prototype.unique;assert(window.unique===el);"
            "EventTarget.prototype.unique=42;assert(window.unique===42&&!Object.getOwnPropertyDescriptor(np,'unique'));"
            "delete EventTarget.prototype.unique;Object.prototype.unique=45;assert(window.unique===45);delete Object.prototype.unique;"
            "assert(window.unique===el);let receiver={};assert(Reflect.set(window,'unique',9,receiver)&&receiver.unique===9&&window.unique===el);"
            "for(let name of ['location','window','document','top','addEventListener','constructor','toString']){"
            "let collision=document.createElement('div');collision.id=name;document.body.append(collision);"
            "assert(window[name]!==collision&&!Object.getOwnPropertyDescriptor(np,name),name);}"
            "let numeric=document.createElement('div');numeric.id='40';document.body.append(numeric);"
            "assert(window[40]===numeric&&!Object.hasOwn(window,'40'));numeric.id='0';assert(window[0]===PB);",
            "Window names: own/prototype collisions, getter-free visibility, writes and indexed precedence");
        realm_script(ce_realms[0],
            "assert(window.identity===undefined);"
            "let marker=document.createElement('i');marker.id='identity';document.body.append(marker);"
            "assert(window.identity===PB);marker.remove();assert(window.identity===undefined);"
            "denied(()=>PB.unique);assert(Object.getPrototypeOf(PB)===null);"
            "globalThis.oldNames=Object.getPrototypeOf(PC.Window.prototype);"
            "PC.document.body.innerHTML='<div id=childItem></div><div id=retainedList></div><img name=retainedList>';"
            "globalThis.oldItem=PC.childItem;globalThis.oldList=PC.retainedList;"
            "assert(oldNames.childItem===oldItem&&oldList.length===2);",
            "Window names: first foreign duplicate, DOM-name support permits child precedence, and retained owner setup");
        check(d_browsing_context_discard(c[1], c[1]->generation, &out) == OS64_JS_OK,
            "Window names: discard first foreign duplicate");
        realm_script(ce_realms[0], "assert(window.identity===PC&&window[0]===PC)",
            "Window names: live child ordering after discard");
        os64_dom_child_t child = location_candidate(&f, 0, "https://replacement.test/names");
        JSContext *ctx = os64_js_realm_context(child.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(ctx, child.binding, c[2]) == 0 &&
              d_browsing_context_replace(c[2], c[2]->generation, child.binding, &out) == OS64_JS_OK,
              "Window names: same-origin document publication");
        realm_script(child.binding->realm,
            "document.body.innerHTML='<form id=childItem></form>';", "Window names: new associated document");
        realm_script(ce_realms[0],
            "assert(window.identity===PC&&PC.childItem!==oldItem&&PC.childItem===PC.document.getElementById('childItem'));"
            "assert(oldNames.childItem===oldItem&&oldList.length===2);"
            "document.adoptNode(oldItem);assert(oldNames.childItem===undefined);document.body.append(oldItem);assert(window.childItem===oldItem);",
            "Window names: current target routes new names; retained objects keep old document and track adoption");
        realm_script(ce_realms[0],
            "for(let name of ['window','document','location','top']){let d=Object.getOwnPropertyDescriptor(window,name);"
            "assert(d.enumerable&&!d.configurable&&typeof d.get==='function',name);"
            "assert(!Reflect.deleteProperty(window,name)&&!Reflect.defineProperty(window,name,{value:1}),name);"
            "typeError(()=>Object.defineProperty(window,name,{value:1}));}"
            "let ld=Object.getOwnPropertyDescriptor(window,'location');assert(typeof ld.set==='function');"
            "assert(Reflect.apply(ld.get,PC,[])===PC.location);typeError(()=>Reflect.apply(ld.get,{},[]));"
            "let calls=0,fake=new Proxy(PC,{get(){calls++;throw 1},getPrototypeOf(){calls++;throw 2}});"
            "typeError(()=>Reflect.apply(ld.set,fake,[{toString(){calls++;return '#bad'}}]));assert(calls===0);",
            "Window descriptors: unforgeable attributes, retained PutForwards setter and borrowed receiver validation");
        os64_dom_child_t foreign = location_candidate(&f, 1, "https://replacement.test/foreign-names");
        ctx = os64_js_realm_context(foreign.binding->realm, OS64_JS_ABI_ID, &out);
        check(d_window_identity_prepare(ctx, foreign.binding, c[2]) == 0 &&
              d_browsing_context_replace(c[2], c[2]->generation, foreign.binding, &out) == OS64_JS_OK,
              "Window names: cross-origin document publication");
        realm_script(ce_realms[0],
            "assert(window.identity===undefined&&oldList.length===2);denied(()=>PC.childItem);"
            "assert(oldNames.childItem===undefined&&Object.getPrototypeOf(oldNames)===C.EventTarget.prototype);"
            "let permit=document.createElement('div');permit.id='identity';document.body.append(permit);"
            "assert(window.identity===PC);denied(()=>window.identity.document);permit.remove();"
            "for(let i=0;i<100;i++){let e=document.createElement('div');e.id='scan'+i;document.body.append(e)}"
            "assert(window.scan99===document.getElementById('scan99')&&window.unknownName===undefined);",
            "Window names: origin recheck, retained document isolation and budgeted long tree walk");
        ce_close(&f);
        check(live == 0 && dom_live == 0 && !dom_allocations, "Window names: balanced grouped teardown");
    }
}
