/* A shared native request ledger, with independent document endpoints. These
 * tests call the native drain at a quiescent point; public departure finish
 * must also establish engine/dispatch/reaction barriers. */
void d_fetch_drain(os64_dom_t *dom);
enum { TR_IDLE, TR_START, TR_REFUSE, TR_RUN, TR_RELEASE, TR_MORE, TR_CLOCK,
       TR_REPLACE, TR_CLEAR, TR_ABORT, TR_FOREIGN_ABORT, TR_FOREIGN_RETIRED, TR_COUNT };
typedef struct TransportEndpoint TransportEndpoint;
typedef struct {
    FakeNet net;
    TransportEndpoint *owners[64];
} TransportLedger;
struct TransportEndpoint {
    TransportLedger *ledger;
    os64_dom_t *binding, *sibling;
    os64_js_realm_t *realm;
    unsigned mode, runs, releases, cancels, mores;
    bool armed, closed;
};

static void transport_mark(TransportEndpoint *p)
{
    os64_js_outcome_t out;
    p->armed = false;
    check(os64_js_realm_retire(p->realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "transport retirement: native mark");
}

static void transport_loan(TransportEndpoint *p)
{
    bool busy = d_provider_busy(p->binding) && d_provider_busy(p->sibling);
    check(busy,
          "transport retirement: provider loan visible across cluster");
    if (!busy) return; /* Keep a missing-loan failure from cascading into unsafe drain. */
    size_t pending = os64_dom_fetches_pending(p->binding);
    d_fetch_drain(p->binding);
    check(os64_dom_fetches_pending(p->binding) == pending,
          "transport retirement: reentrant drain cannot consume borrowed requests");
}

static void transport_late(TransportEndpoint *p, uint32_t token)
{
    os64_js_outcome_t out;
    os64_dom_response_t r = net_answer_of(200, "https://transport.example/late", "", "late");
    os64_js_status_t expected = p->closed ? OS64_JS_FAILED_RUNTIME : OS64_JS_OK;
    check(os64_dom_fetched(p->binding, token, &r, &out) == expected &&
          os64_dom_fetch_head(p->binding, token, &r, &out) == expected &&
          os64_dom_fetch_body(p->binding, token, "late", 4, &out) == expected &&
          os64_dom_fetch_end(p->binding, token, true, &out) == expected,
          "transport retirement: late borrowed payloads discarded without engine entry");
}

static uint64_t transport_start(void *opaque, const os64_dom_request_t *q, uint32_t token)
{
    TransportEndpoint *p = opaque;
    transport_loan(p);
    if (p->armed && (p->mode == TR_START || p->mode == TR_REFUSE)) {
        os64_dom_set_network(p->binding, NULL);
        transport_mark(p);
        if (p->mode == TR_REFUSE) return 0;
    }
    size_t index = p->ledger->net.count;
    uint64_t id = net_start(&p->ledger->net, q, token);
    if (id) p->ledger->owners[index] = p;
    return id;
}

static void transport_cancel(void *opaque, uint64_t id)
{
    TransportEndpoint *p = opaque;
    transport_loan(p);
    p->cancels++;
    for (size_t i = 0; i < p->ledger->net.count; ++i) {
        NetSeen *s = &p->ledger->net.seen[i];
        if (s->id != id) continue;
        check(p->ledger->owners[i] == p && !s->cancelled,
              "transport retirement: original producer cancels its own request once");
        check(!os64_dom_fetch_waiting(p->binding, s->token),
              "transport retirement: token unlinked before cancel reentry");
        transport_late(p, s->token);
        net_cancel(&p->ledger->net, id);
        return;
    }
    check(false, "transport retirement: cancel id belongs to shared ledger");
}

static void transport_more(void *opaque, uint64_t id)
{
    TransportEndpoint *p = opaque;
    transport_loan(p);
    p->mores++;
    for (size_t i = 0; i < p->ledger->net.count; ++i)
        if (p->ledger->net.seen[i].id == id)
            check(p->ledger->owners[i] == p,
                  "transport retirement: more uses the original request producer");
    net_more(&p->ledger->net, id);
    if (p->armed && p->mode == TR_MORE) transport_mark(p);
}

static bool transport_run(void *opaque, const os64_dom_request_t *q, bool (*stop)(void *),
                          void *stop_ctx, os64_dom_response_t *out)
{
    (void)q;
    TransportEndpoint *p = opaque;
    transport_loan(p);
    p->runs++;
    /* Clearing the installed table must not lose this loan's releaser. */
    os64_dom_set_network(p->binding, NULL);
    if (p->armed && p->mode == TR_RUN) {
        transport_mark(p);
        check(stop(stop_ctx), "transport retirement: synchronous stop observes retirement");
    }
    *out = net_answer_of(200, "https://transport.example/sync", "", "sync");
    return true;
}

static void transport_release(void *opaque, os64_dom_response_t *response)
{
    TransportEndpoint *p = opaque;
    transport_loan(p);
    p->releases++;
    check(response->body_length == 4, "transport retirement: original response release");
    if (p->armed && p->mode == TR_RELEASE) transport_mark(p);
}

static uint64_t transport_clock(void *opaque)
{
    TransportEndpoint *p = opaque;
    if (p->armed && p->mode == TR_CLOCK && os64_dom_fetches_pending(p->binding)) {
        transport_loan(p);
        transport_mark(p);
    }
    return 1000;
}

static os64_dom_network_t transport_network(TransportEndpoint *p)
{
    return (os64_dom_network_t){transport_start, transport_cancel, transport_run,
                               transport_release, p, transport_more};
}

static void dom_transport_retirement_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < TR_COUNT; ++mode) {
        fprintf(stderr, "transport scenario: policy=%d mode=%u\n", policy, mode);
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "transport retirement: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        if (!cluster) { check(false, "transport retirement: cluster created"); os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[2] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), NULL};
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK,
              "transport retirement: child realm created");
        TransportLedger ledger = {0};
        TransportEndpoint p[2] = {{.ledger=&ledger}, {.ledger=&ledger, .mode=mode}};
        for (unsigned i = 0; i < 2 && realms[i]; ++i) {
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://transport.example/");
            options.host_opaque = &p[i]; options.now_ms = transport_clock;
            os64_dom_admission_t a = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            p[i].binding = a.binding; p[i].realm = realms[i];
            check(a.binding && a.consumed, "transport retirement: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (p[0].binding && p[1].binding) {
            for (unsigned i = 0; i < 2; ++i) {
                p[i].sibling = p[1-i].binding;
                os64_dom_network_t network = transport_network(&p[i]);
                os64_dom_set_network(p[i].binding, &network);
                realm_script(realms[i], STORAGE_ASSERT
                    "globalThis.controller=new AbortController();globalThis.x=new XMLHttpRequest();"
                    "x.open('POST','/xhr');x.timeout=100;"
                    "globalThis.savedOpen=XMLHttpRequest.prototype.open;globalThis.savedSend=XMLHttpRequest.prototype.send;",
                    "transport retirement: objects prepared");
            }
            JSContext *ac = os64_js_realm_context(realms[0], OS64_JS_ABI_ID, &out);
            JSContext *bc = os64_js_realm_context(realms[1], OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(ac);
            check(JS_SetPropertyStr(ac, global, "B", JS_GetGlobalObject(bc)) >= 0,
                  "transport retirement: retained child global exported"); JS_FreeValue(ac, global);
            JSValue bg = JS_GetGlobalObject(bc);
            JSValue controller = JS_GetPropertyStr(bc, bg, "controller");
            JSValue signal = JS_GetPropertyStr(bc, controller, "signal");
            JS_FreeValue(bc, controller); JS_FreeValue(bc, bg);
            realm_script(realms[0],
                "globalThis.deliveries=0;globalThis.finished=0;globalThis.streamResponse=null;"
                "globalThis.refused=f=>{try{f();return false}catch(e){return e.name==='InvalidStateError'}};"
                "B.x.onload=B.x.onerror=B.x.onabort=B.x.ontimeout=()=>deliveries++;"
                "fetch('/sibling').then(r=>r.text()).then(t=>{assert(t==='alive');finished++});",
                "transport retirement: live sibling request and active callbacks");
            p[1].armed = true;
            if (mode == TR_FOREIGN_ABORT || mode == TR_FOREIGN_RETIRED) {
                realm_script(realms[0],
                    "globalThis.abortCount=0;globalThis.foreign=new AbortController();"
                    "B.fetch('/foreign',{signal:foreign.signal}).catch(e=>{assert(e==='foreign');abortCount++});",
                    "transport retirement: foreign signal with colliding binding tokens");
                check(ledger.net.seen[0].token == ledger.net.seen[1].token,
                      "transport retirement: sibling and child tokens intentionally collide");
                if (mode == TR_FOREIGN_RETIRED) transport_mark(&p[1]);
                realm_script(realms[0], "foreign.abort('foreign');",
                             "transport retirement: source realm aborts dependent request");
                check(p[0].cancels == 0 && p[1].cancels == 1 &&
                      !ledger.net.seen[0].cancelled && ledger.net.seen[1].cancelled,
                      "transport retirement: abort algorithm routes to dependent binding");
                realm_script(realms[0], mode == TR_FOREIGN_ABORT ? "assert(abortCount===1);" : "assert(abortCount===0);",
                             "transport retirement: foreign signal respects dependent activity");
                if (mode == TR_FOREIGN_ABORT) transport_mark(&p[1]);
            } else if (mode == TR_RUN || mode == TR_RELEASE) {
                realm_script(realms[0],
                    "B.x.timeout=0;B.x.open('GET','/sync',false);assert(refused(()=>B.x.send()));"
                    "assert(B.x.responseText===''&&deliveries===0);",
                    "transport retirement: synchronous retirement refuses completion");
                check(p[1].runs == 1 && p[1].releases == 1,
                      "transport retirement: synchronous loan released despite cleared provider");
            } else if (mode == TR_START || mode == TR_REFUSE || mode == TR_CLOCK) {
                realm_script(realms[0], "assert(refused(()=>B.x.send('body')));assert(deliveries===0);",
                             "transport retirement: send cannot publish work after callback retirement");
                check(p[1].cancels == (mode == TR_REFUSE ? 0u : 1u) &&
                      os64_dom_fetches_pending(p[1].binding) == 0,
                      "transport retirement: unpublished accepted request cancelled once");
            } else {
                realm_script(realms[0],
                    "globalThis.settledResponse=null;B.fetch('/settled').then(r=>settledResponse=r);"
                    "globalThis.done=new B.XMLHttpRequest();done.open('GET','/done');done.send();",
                    "transport retirement: completed data prepared");
                os64_dom_response_t saved = net_answer_of(200, "https://transport.example/saved", "", "saved");
                check(os64_dom_fetched(p[1].binding, ledger.net.seen[1].token, &saved, &out) == OS64_JS_OK &&
                      os64_dom_fetched(p[1].binding, ledger.net.seen[2].token, &saved, &out) == OS64_JS_OK,
                      "transport retirement: settled fetch and XHR data delivered");
                size_t child_start = ledger.net.count;
                realm_script(realms[0],
                    "B.fetch('/pending',{signal:B.controller.signal}).then(()=>deliveries++,()=>deliveries++);"
                    "B.x.send('body');"
                    "B.fetch('/stream',{signal:B.controller.signal}).then(r=>{streamResponse=r});",
                    "transport retirement: child pending fetch XHR and stream");
                os64_dom_response_t head = net_answer_of(200, "https://transport.example/stream", "", "");
                if (mode == TR_REPLACE) {
                    os64_dom_network_t replacement = transport_network(&p[0]);
                    os64_dom_set_network(p[1].binding, &replacement);
                }
                if (mode == TR_MORE) p[1].armed = false;
                check(os64_dom_fetch_head(p[1].binding, ledger.net.seen[child_start+1].token, &head, &out) == OS64_JS_OK &&
                      os64_dom_fetch_body(p[1].binding, ledger.net.seen[child_start+1].token, "part", 4, &out) == OS64_JS_OK,
                      "transport retirement: partial XHR bytes delivered");
                if (mode == TR_MORE) p[1].armed = true;
                check(os64_dom_fetch_head(p[1].binding, ledger.net.seen[child_start+2].token, &head, &out) == OS64_JS_OK,
                      "transport retirement: child stream head delivered");
                realm_script(realms[0], "assert(streamResponse!==null);globalThis.reader=streamResponse.body.getReader();",
                             "transport retirement: retained stream reader");
                if (mode == TR_MORE)
                    realm_script(realms[0], "reader.read().then(()=>deliveries++,()=>deliveries++);",
                                 "transport retirement: demand retires provider");
                if (mode != TR_MORE) {
                    check(os64_dom_fetch_body(p[1].binding, ledger.net.seen[child_start+2].token, "kept", 4, &out) == OS64_JS_OK,
                          "transport retirement: queued stream bytes");
                    transport_mark(&p[1]);
                    if (mode == TR_CLEAR) os64_dom_set_network(p[1].binding, NULL);
                }
                check(os64_js_realm_is_retired(realms[1]),
                      "transport retirement: child marked before cleanup");
                if (mode == TR_ABORT)
                    realm_script(realms[0], "B.controller.abort('late');", "transport retirement: explicit abort after mark");
                for (size_t i = child_start; i < ledger.net.count; ++i) transport_late(&p[1], ledger.net.seen[i].token);
                d_fetch_drain(p[1].binding);
                check(d_signal_dependents(signal) == 0,
                      "transport retirement: cleanup releases abort algorithm dependent holds");
                check(p[1].cancels == 3 && p[0].cancels == 0 && !ledger.net.seen[0].cancelled &&
                      os64_dom_fetches_pending(p[0].binding) == 1 && os64_dom_fetches_pending(p[1].binding) == 0,
                      "transport retirement: cleanup cancels child requests and preserves shared sibling");
                {
                    realm_script(realms[0],
                        "B.controller.abort('after');assert(deliveries===0);"
                        "assert(done.status===200&&done.responseText==='saved');"
                        "assert(B.x.status===200&&B.x.responseText==='part');"
                        "B.x.timeout=23;assert(B.x.timeout===23);"
                        "globalThis.savedText='';settledResponse.clone().text().then(t=>savedText=t);"
                        "assert(refused(()=>XMLHttpRequest.prototype.open.call(B.x,'GET','/late')));"
                        "let a=new XMLHttpRequest();B.savedOpen.call(a,'GET','/borrowed');B.savedSend.call(a);",
                        "transport retirement: no synthetic events and borrowed XHR uses receiver owner");
                    realm_script(realms[0], "assert(savedText==='saved');",
                                 "transport retirement: settled Response clone remains readable");
                    if (mode != TR_MORE && mode != TR_ABORT) {
                        realm_script(realms[0],
                            "globalThis.kept='';reader.read().then(v=>{kept=String.fromCharCode(...v.value)});",
                            "transport retirement: reading retained queued bytes");
                        realm_script(realms[0], "assert(kept==='kept');",
                                     "transport retirement: departure preserves stream storage");
                    }
                }
            }
            d_fetch_drain(p[1].binding);
            unsigned cancels = p[1].cancels;
            os64_dom_network_t retry = transport_network(&p[1]);
            os64_dom_set_network(p[1].binding, &retry);
            d_fetch_drain(p[1].binding);
            check(p[1].cancels == cancels && !d_provider_busy(p[0].binding),
                  "transport retirement: repeated cleanup is inert and loans balanced");
            os64_dom_response_t answer = net_answer_of(200, "https://transport.example/sibling", "", "alive");
            check(os64_dom_fetched(p[0].binding, ledger.net.seen[0].token, &answer, &out) == OS64_JS_OK,
                  "transport retirement: sibling response delivered");
            realm_script(realms[0], "assert(finished===1&&deliveries===0);",
                         "transport retirement: active sibling finishes with no retired delivery");
            JS_FreeValue(ac, signal);
        }
        p[0].closed = p[1].closed = true;
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations,
              "transport retirement: engine and native owners released");
    }
}
