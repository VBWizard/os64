/* Native visitors borrow host state even when no script task is opened. */
enum { VR_MAP_IMPORT, VR_MAP_SCOPE, VR_MAP_LAST, VR_MAP_REFUSE, VR_MAP_REPLACE,
       VR_POINTER_QUIET, VR_POINTER_EVENT, VR_POINTER_LISTENER, VR_POINTER_UP, VR_POINTER_CAPTURE,
       VR_POINTER_PRE_DEFAULT, VR_COUNT };
typedef struct {
    os64_dom_t *binding, *sibling;
    os64_js_realm_t *realm;
    os64_dom_import_map_walk_t *installed;
    unsigned mode, calls, retire_at, redirected;
    bool sibling_inactive;
} VisitorRetirement;

static bool visitor_unexpected(void *opaque, const char *scope, const char *key, const char *value)
{
    (void)scope; (void)key; (void)value;
    ((VisitorRetirement *)opaque)->redirected++;
    return true;
}

static void visitor_touch(VisitorRetirement *p)
{
    p->calls++;
    bool busy = d_provider_busy(p->binding) && d_provider_busy(p->sibling);
    check(busy, "visitor retirement: native callback holds a cluster-visible loan");
    if (busy) {
        os64_js_outcome_t out;
        os64_dom_import_map_walk_t walk = {p, visitor_unexpected};
        check(os64_dom_import_map(p->sibling, "nested map", "{}", 2, &walk, &out) ==
              (p->sibling_inactive ? OS64_JS_REALM_RETIRED : OS64_JS_BUSY),
              "visitor retirement: provider reentry cannot start sibling import task");
        os64_dom_pointer_sample_t sample = {.action = OS64_DOM_POINTER_MOVE};
        bool ran = true;
        check(os64_dom_pointer_dispatch(p->sibling, &sample, NULL, NULL, &ran, &out) ==
              (p->sibling_inactive ? OS64_JS_OK : OS64_JS_BUSY) && !ran,
              "visitor retirement: provider reentry cannot dispatch sibling quiet hover");
    }
    if (p->retire_at && p->calls == p->retire_at) {
        os64_js_outcome_t out;
        check(os64_js_realm_retire(p->realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "visitor retirement: native mark succeeds during callback");
        check(d_provider_busy(p->binding) && !d_provider_enter(p->binding),
              "visitor retirement: old loan survives mark and new loan is refused");
    }
}

static bool visitor_map(void *opaque, const char *scope, const char *key, const char *value)
{
    VisitorRetirement *p = opaque;
    visitor_touch(p);
    check(key && key[0] && value && value[0] == '/' && (!scope || !strcmp(scope, "/scope")),
          "visitor retirement: callback's strings survive retirement");
    if (p->mode == VR_MAP_REPLACE) p->installed->entry = visitor_unexpected;
    return p->mode != VR_MAP_REFUSE;
}

static bool visitor_pointer(void *opaque, const os64_html_node_t *node,
                            const os64_dom_event_t *event, bool prevented)
{
    VisitorRetirement *p = opaque;
    (void)prevented;
    visitor_touch(p);
    check(node && node->kind == OS64_HTML_ELEMENT && event->type && event->kind == OS64_DOM_EVENT_MOUSE,
          "visitor retirement: borrowed pointer target and event survive retirement");
    return true;
}

static void dom_visitor_retirement_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < VR_COUNT; ++mode) {
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "visitor retirement: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        if (!cluster) { check(false, "visitor retirement: cluster created"); os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[2] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), NULL};
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK,
              "visitor retirement: child realm created");
        os64_dom_t *bindings[2] = {0}; os64_html_document_t *documents[2] = {0};
        for (unsigned i = 0; i < 2 && realms[i]; ++i) {
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://visitor.example/");
            os64_dom_admission_t a = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            bindings[i] = a.binding; documents[i] = f.document;
            check(a.binding && a.consumed, "visitor retirement: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (bindings[0] && bindings[1]) {
            for (unsigned i = 0; i < 2; ++i)
                realm_script(realms[i], STORAGE_ASSERT "document.body.innerHTML='<div id=target></div>';",
                             "visitor retirement: fixture tree");
            retirement_realms[1] = realms[1];
            JSContext *ac = os64_js_realm_context(realms[0], OS64_JS_ABI_ID, &out);
            JSContext *bc = os64_js_realm_context(realms[1], OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(ac);
            check(JS_SetPropertyStr(ac, global, "B", JS_GetGlobalObject(bc)) >= 0 &&
                  JS_SetPropertyStr(ac, global, "retireChild", JS_NewCFunctionMagic(ac, retirement_mark,
                      "retireChild", 0, JS_CFUNC_generic_magic, 1)) >= 0,
                  "visitor retirement: retained child and trusted mark exposed");
            JS_FreeValue(ac, global);
            realm_script(realms[0], "globalThis.events=[];", "visitor retirement: live callback ledger");
            VisitorRetirement p = {.binding=bindings[1], .sibling=bindings[0], .realm=realms[1],
                                   .mode=mode, .retire_at=1};
            os64_dom_import_map_walk_t walk = {&p, visitor_map}; p.installed = &walk;
            const char *json = "{\"imports\":{\"a\":\"/a\",\"b\":\"/b\"},\"scopes\":{\"/scope\":{\"c\":\"/c\",\"d\":\"/d\"}}}";
            if (mode <= VR_MAP_REPLACE) {
                p.retire_at = mode == VR_MAP_SCOPE ? 3 : mode == VR_MAP_LAST ? 4 : mode == VR_MAP_REPLACE ? 0 : 1;
                os64_js_status_t status = os64_dom_import_map(p.binding, "visitor map", json, strlen(json), &walk, &out);
                check(status == (mode == VR_MAP_REPLACE ? OS64_JS_OK : OS64_JS_REALM_RETIRED),
                      "visitor retirement: partial map cannot report successful registration");
                check(p.calls == (p.retire_at ? p.retire_at : 4) && p.redirected == 0,
                      "visitor retirement: map stops at mark and retains original visitor");
                if (mode == VR_MAP_REPLACE)
                    check(os64_js_realm_retire(p.realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                          "visitor retirement: mark after complete map");
            } else if (mode == VR_POINTER_CAPTURE) {
                p.retire_at = 0;
                os64_dom_pointer_sample_t sample = {.action=OS64_DOM_POINTER_DOWN,
                    .hit=native_id(documents[1]->document,"target"), .event={.button=0,.buttons=1}};
                check(os64_dom_pointer_dispatch(p.binding, &sample, visitor_pointer, &p, NULL, &out) == OS64_JS_OK,
                      "visitor retirement: capture pointer prepared");
                realm_script(realms[0],
                    "let capture=B.document.getElementById('target');capture.setPointerCapture(1);"
                    "assert(capture.hasPointerCapture(1));",
                    "visitor retirement: live capture before external mark");
                check(os64_js_realm_retire(p.realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
                      !os64_dom_pointer_active(p.binding),
                      "visitor retirement: mark hides pending native pointer activity before cleanup");
            } else {
                if (mode != VR_POINTER_QUIET)
                    realm_script(realms[0],
                        "for(let t of ['pointermove','mousemove','mouseup','click'])"
                        "B.document.addEventListener(t,e=>events.push(t));",
                        "visitor retirement: active listeners on child document");
                if (mode == VR_POINTER_LISTENER || mode == VR_POINTER_PRE_DEFAULT)
                    realm_script(realms[0], mode == VR_POINTER_LISTENER ?
                                 "B.document.addEventListener('pointermove',retireChild);" :
                                 "B.document.addEventListener('mousemove',retireChild);",
                                 "visitor retirement: author mark before native default");
                os64_dom_pointer_sample_t sample = {.action=OS64_DOM_POINTER_MOVE,
                    .hit=native_id(documents[1]->document,"target"), .event={.button=-1}};
                if (mode == VR_POINTER_UP) {
                    p.retire_at = 0; sample.action = OS64_DOM_POINTER_DOWN;
                    sample.event.button = 0; sample.event.buttons = 1;
                    check(os64_dom_pointer_dispatch(p.binding, &sample, visitor_pointer, &p, NULL, &out) == OS64_JS_OK,
                          "visitor retirement: pressed pointer prepared");
                    realm_script(realms[0], "events=[];", "visitor retirement: clear down events");
                    p.calls = 0; p.retire_at = 1;
                    sample.action = OS64_DOM_POINTER_UP; sample.event.buttons = 0;
                }
                bool ran = false;
                check(os64_dom_pointer_dispatch(p.binding, &sample, visitor_pointer, &p, &ran, &out) == OS64_JS_OK &&
                      ran == (mode != VR_POINTER_QUIET), "visitor retirement: packet stops cleanly after mark");
                check(p.calls == (mode == VR_POINTER_LISTENER || mode == VR_POINTER_PRE_DEFAULT ? 0u : 1u),
                      "visitor retirement: no native default after listener or provider mark");
                realm_script(realms[0], "assert(!events.includes('click'));",
                             "visitor retirement: retiring mouseup cannot synthesize later click");
            }
            unsigned calls = p.calls;
            check(os64_js_realm_is_retired(realms[1]) && !d_provider_busy(bindings[0]),
                  "visitor retirement: retirement leaves no borrowed host state");
            check(os64_dom_import_map(p.binding, "late map", json, strlen(json), &walk, &out) == OS64_JS_REALM_RETIRED,
                  "visitor retirement: retired map refused before task entry");
            os64_dom_pointer_sample_t late = {.action=OS64_DOM_POINTER_DOWN,
                .hit=native_id(documents[1]->document,"target"), .event={.button=0,.buttons=1}};
            bool ran = true;
            check(os64_dom_pointer_dispatch(p.binding, &late, visitor_pointer, &p, &ran, &out) == OS64_JS_OK && !ran &&
                  p.calls == calls && !os64_dom_pointer_active(p.binding),
                  "visitor retirement: late input neither runs tasks nor revives pointer state");
            os64_dom_pointer_cancel(p.binding);
            realm_script(realms[0],
                "let target=B.document.getElementById('target');assert(!target.hasPointerCapture(1));"
                "target.releasePointerCapture(1);let refused=false;try{target.setPointerCapture(1)}"
                "catch(e){refused=e.name==='NotFoundError'}assert(refused);"
                "target.textContent='retained';assert(target.textContent==='retained');"
                "assert(B.HTMLScriptElement.supports('importmap'));",
                "visitor retirement: retained node survives without pointer capture service");
            VisitorRetirement sibling = {.binding=bindings[0], .sibling=bindings[1], .realm=realms[0], .sibling_inactive=true};
            os64_dom_import_map_walk_t sibling_walk = {&sibling, visitor_map};
            check(os64_dom_import_map(bindings[0], "sibling map", json, strlen(json), &sibling_walk, &out) == OS64_JS_OK &&
                  sibling.calls == 4, "visitor retirement: active sibling map still completes");
            realm_script(realms[0], "assert(document.body!==null);", "visitor retirement: sibling script remains usable");
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "visitor retirement: native and engine holds released");
    }
}
