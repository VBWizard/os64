#include <os64/js_failure.h>

/* Native cluster policy: the providers own real fixture storage until the
 * recovery coordinator has returned OK, or root teardown has completed. */
typedef struct DomRecoveryFixture DomRecoveryFixture;
typedef struct {
    DomRecoveryFixture *fixture;
    FakeStore store;
    FakeNet net;
    unsigned member, prepares;
    bool armed;
} DomRecoveryProvider;
struct DomRecoveryFixture {
    os64_js_runtime_t *runtime;
    os64_dom_cluster_t *cluster;
    os64_js_realm_t *realm[3];
    os64_dom_t *binding[3];
    JSContext *ctx[3];
    DomRecoveryProvider *provider[3];
    os64_js_outcome_t *active_outcome;
    unsigned cleanup_action;
};
static unsigned dom_recovery_misses;
static unsigned dom_recovery_job_hits;
static JSValue dom_recovery_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    dom_recovery_job_hits++;
    return JS_UNDEFINED;
}
static void dom_recovery_probe(DomRecoveryProvider *p)
{
    if (!p->armed) return;
    DomRecoveryFixture *f = p->fixture;
    JSExecutionState engine;
    os64_js_recovery_info_t info;
    check(JS_GetExecutionState(JS_GetRuntime(f->ctx[0]), &engine) && engine.blocked &&
          os64_js_recovery_inspect(f->runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
          info.state == OS64_JS_RECOVERY_PREPARING && info.generation == f->cluster->recovery_generation,
          "DOM recovery: native cleanup owns the exact fenced generation");
    check(f->cluster->recovering && d_provider_busy(f->binding[0]) &&
          !d_delivery_active(f->binding[0]) && !d_delivery_active(f->binding[2]) &&
          os64_js_realm_is_failed(f->realm[1]),
          "DOM recovery: failure and cluster pause publish before native provider reentry");
    os64_js_outcome_t out;
    check(os64_dom_cluster_recover(f->cluster, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY,
          "DOM recovery: native provider cannot recursively acknowledge cleanup");
    if (f->active_outcome) {
        os64_js_outcome_t saved = *f->active_outcome;
        check(os64_dom_cluster_recover(f->cluster, OS64_JS_ABI_ID, f->active_outcome) == OS64_JS_BUSY &&
              !memcmp(&saved, f->active_outcome, sizeof(saved)),
              "DOM recovery: provider cannot overwrite its enclosing recovery outcome");
    }
    JSValue value = JS_Eval(f->ctx[0], "shared.escaped=1", 16, "native-reentry", JS_EVAL_TYPE_GLOBAL);
    check(JS_IsException(value) && !JS_HasException(f->ctx[0]),
          "DOM recovery: native cleanup cannot enter healthy author code or replace its exception");
    JS_FreeValue(f->ctx[0], value);
    p->prepares++;
}
static uint64_t dom_recovery_identity(void *opaque, const char *origin)
{ return fake_identity(&((DomRecoveryProvider *)opaque)->store, origin); }
static int64_t dom_recovery_load(void *opaque, const char *origin, void *bytes, size_t cap)
{ return fake_load(&((DomRecoveryProvider *)opaque)->store, origin, bytes, cap); }
static os64_dom_storage_status_t dom_recovery_commit(void *opaque, const char *origin,
    const void *bytes, size_t length)
{
    DomRecoveryProvider *p = opaque;
    dom_recovery_probe(p);
    return fake_commit(&p->store, origin, bytes, length);
}
static uint64_t dom_recovery_start(void *opaque, const os64_dom_request_t *request, uint32_t token)
{ return net_start(&((DomRecoveryProvider *)opaque)->net, request, token); }
static void dom_recovery_cancel(void *opaque, uint64_t id)
{
    DomRecoveryProvider *p = opaque;
    dom_recovery_probe(p);
    check(!p->armed || !os64_dom_fetches_pending(p->fixture->binding[p->member]),
          "DOM recovery: native cancellation cannot rediscover its unlinked request");
    net_cancel(&p->net, id);
    if (p->armed && p->member == 1) {
        DomRecoveryFixture *f = p->fixture;
        unsigned action = f->cleanup_action;
        f->cleanup_action = 0;
        if (action == 1) os64_dom_effects_mark(f->binding[2]);
        if (action == 2) os64_js_cancel(f->runtime);
        if (action == 3) dom_clock = -1;
        if (action == 4) {
            fail_at = attempts + 1;
            void *allocation = js_malloc_rt(JS_GetRuntime(f->ctx[0]), 64u << 10);
            fail_at = 0;
            check(!allocation, "DOM recovery: cleanup observes actual allocator OOM");
            js_free_rt(JS_GetRuntime(f->ctx[0]), allocation);
        }
        if (action == 5) {
            void *allocation = js_malloc_rt(JS_GetRuntime(f->ctx[0]), 128u << 20);
            check(!allocation, "DOM recovery: cleanup observes actual engine memory ceiling");
            js_free_rt(JS_GetRuntime(f->ctx[0]), allocation);
        }
        if (action == 6) check(JS_EnqueueJob(f->ctx[2], dom_recovery_job, 0, NULL) == 0,
                              "DOM recovery: native cleanup can expose new pending bookkeeping");
        if (action == 7) JS_Throw(f->ctx[0], JS_NewInt32(f->ctx[0], 71));
    }
}
static void dom_recovery_miss(void *opaque, const char *name)
{
    (void)name;
    DomRecoveryProvider *p = opaque;
    check(p->fixture != NULL, "DOM recovery: global-miss opaque storage is live when called");
    dom_recovery_misses++;
}
static bool dom_recovery_open(DomRecoveryFixture *f, unsigned policy, bool modern)
{
    memset(f, 0, sizeof(*f));
    os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
    config.limits.execution_ms = 10;
    os64_dom_cluster_limits_t limits = {64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
    os64_js_outcome_t out;
    f->cluster = os64_dom_cluster_create_with_recovery(&config, policy, OS64_JS_ABI_ID,
                                                     &limits, &f->runtime, &out);
    check(f->cluster && f->runtime && out.status == OS64_JS_OK,
          "DOM recovery: paired constructor publishes a configured runtime and cluster");
    if (!f->cluster) return false;
    f->realm[0] = os64_js_primary_realm(f->runtime, OS64_JS_ABI_ID, &out);
    for (unsigned i = 1; i < 3; ++i)
        check(os64_js_realm_create(f->runtime, OS64_JS_ABI_ID, &f->realm[i], &out) == OS64_JS_OK,
              "DOM recovery: independent child slot admitted");
    p2b_ms = 1000; p2b_us = 1000000;
    for (unsigned i = 0; i < 3; ++i) {
        f->provider[i] = os64_calloc(1, sizeof(*f->provider[i]));
        check(f->provider[i] != NULL, "DOM recovery: independent host provider allocated");
        if (!f->provider[i]) abort();
        f->provider[i]->fixture = f; f->provider[i]->member = i;
        DomFixture doc = child_install_document();
        os64_dom_options_t options = p2b_options("https://recover.example/"); options.modern = modern;
        options.host_opaque = f->provider[i];
        os64_dom_admission_t admission = os64_dom_create_in_realm(f->cluster, f->realm[i],
            doc.document, doc.state, &options, &out);
        check(admission.binding && admission.consumed, "DOM recovery: binding installed in an owned realm");
        if (!admission.binding) abort();
        f->binding[i] = admission.binding;
        f->ctx[i] = os64_js_realm_context(f->realm[i], OS64_JS_ABI_ID, &out);
        os64_dom_set_global_miss(f->binding[i], dom_recovery_miss, f->provider[i]);
        if (modern) {
            os64_dom_storage_t store = {dom_recovery_identity,dom_recovery_load,dom_recovery_commit,f->provider[i]};
            os64_dom_network_t net = {.start=dom_recovery_start,.cancel=dom_recovery_cancel,.opaque=f->provider[i]};
            os64_dom_set_storage(f->binding[i], OS64_DOM_STORAGE_LOCAL, &store);
            os64_dom_set_network(f->binding[i], &net);
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        JSValue global = JS_GetGlobalObject(f->ctx[i]);
        for (unsigned j = 0; j < 3; ++j) {
            char name[] = {(char)('A'+j), 0};
            check(JS_SetPropertyStr(f->ctx[i], global, name, JS_GetGlobalObject(f->ctx[j])) >= 0,
                  "DOM recovery: native fixture exports independent globals");
        }
        JS_FreeValue(f->ctx[i], global);
    }
    realm_script(f->realm[0], "globalThis.shared={value:0,order:'',events:''};setTimeout(()=>shared.order+='A',0)",
                 "DOM recovery: primary data and independent native task");
    realm_script(f->realm[1], "globalThis.shared=A.shared;globalThis.node=document.createElement('div');"
        "globalThis.badCallback=function(){shared.forbidden=1};"
        "node.addEventListener('ping',()=>{shared.events+='B'});document.body.appendChild(node);"
        "setTimeout(()=>shared.order+='B',0);"
        "globalThis.bad=function(probe){if(probe){shared.forbidden=1;return 7}shared.value=42;"
        "try{while(true){}}finally{shared.value=99}};", "DOM recovery: child activity and abort function");
    realm_script(f->realm[2], "globalThis.shared=A.shared;document.body.appendChild(B.node);"
        "B.node.addEventListener('ping',()=>{shared.events+='C'});"
        "setInterval(B.badCallback,0);setTimeout(()=>shared.order+='1',0);setTimeout(()=>shared.order+='2',0);",
        "DOM recovery: adoption and healthy sibling native task order");
    if (modern) {
        realm_script(f->realm[1], "localStorage.setItem('key','kept');globalThis.pending=fetch('/pending');"
            "globalThis.canvas=document.createElement('canvas');canvas.width=2;canvas.height=2;"
            "globalThis.paint=canvas.getContext('2d');paint.fillStyle='red';paint.fillRect(0,0,2,2);",
            "DOM recovery: completed native mutations and pending transport");
        realm_script(f->realm[2], "globalThis.pending=fetch('/healthy')", "DOM recovery: independent sibling transport");
    }
    check(!f->cluster->streams_used, "DOM recovery: interface installation alone is not Streams use");
    return true;
}
static void dom_recovery_close(DomRecoveryFixture *f)
{
    dom_clock_step = 0; dom_clock = 10000000; fail_at = 0; dom_fail_at = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (f->provider[i]) f->provider[i]->armed = false;
        os64_dom_effects_release(f->binding[i], UINT64_MAX);
    }
    os64_dom_cluster_drain(f->cluster);
    os64_js_teardown_report_t report;
    os64_js_destroy_report(f->runtime, &report);
    os64_dom_cluster_free(f->cluster);
    for (unsigned i = 0; i < 3; ++i) if (f->provider[i]) {
        fake_store_free(&f->provider[i]->store);
        os64_free(f->provider[i]);
    }
    check(!report.leaked && live == 0 && dom_live == 0 && !dom_allocations,
          "DOM recovery: host, native registries and engine roots release under selected teardown");
}
static os64_js_outcome_t dom_recovery_overrun_realm(DomRecoveryFixture *f, unsigned child)
{
    os64_js_outcome_t out;
    check(os64_js_realm_task_begin(f->realm[child], OS64_JS_ABI_ID, "DOM child timeout", &out) == OS64_JS_OK,
          "DOM recovery: original child task opens");
    JSValue global = JS_GetGlobalObject(f->ctx[child]);
    JSValue fn = JS_GetPropertyStr(f->ctx[child], global, "bad"); JS_FreeValue(f->ctx[child], global);
    dom_clock_step = 1000;
    check(os64_js_realm_call(f->realm[0], OS64_JS_ABI_ID, fn, JS_UNDEFINED, 0, NULL, NULL, &out) == OS64_JS_LIMIT &&
          out.limit == OS64_JS_LIMIT_EXECUTION, "DOM recovery: actual child bytecode exhausts original deadline");
    dom_clock_step = 0; JS_FreeValue(f->ctx[child], fn);
    os64_js_outcome_t closed;
    check(os64_js_task_end(f->runtime, OS64_JS_ABI_ID, &closed) == OS64_JS_LIMIT &&
          closed.limit == out.limit && closed.jobs_executed == out.jobs_executed,
          "DOM recovery: abandoned task closes without resuming or renewing its work");
    return out;
}
static os64_js_outcome_t dom_recovery_overrun(DomRecoveryFixture *f)
{ return dom_recovery_overrun_realm(f, 1); }
static void dom_recovery_success_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned modern = 0; modern < 2; ++modern) {
        DomRecoveryFixture f;
        if (!dom_recovery_open(&f, policy, modern)) continue;
        if (modern) realm_script(f.realm[2], "globalThis.healthyCallback=function(){shared.native+='F'}",
            "DOM recovery: healthy callback may be stored in another realm's custom definition");
        if (modern) realm_script(f.realm[1],
            "new MutationObserver(()=>{shared.forbidden=2}).observe(document.body,{attributes:true});"
            "new ResizeObserver(()=>{shared.forbidden=3}).observe(document.body);"
            "new IntersectionObserver(()=>{shared.forbidden=4}).observe(document.body);"
            "globalThis.motion=document.body.animate({opacity:[0,1]},1000);"
            "customElements.define('x-bad',class extends HTMLElement{"
            "static get observedAttributes(){return ['data-probe']}"
            "attributeChangedCallback(){shared.forbidden=5}});"
            "globalThis.badElement=document.createElement('x-bad');document.body.appendChild(badElement);"
            "class ForeignData extends HTMLElement{static get observedAttributes(){return ['data-probe']}}"
            "ForeignData.prototype.attributeChangedCallback=C.healthyCallback;customElements.define('x-foreign',ForeignData);"
            "globalThis.foreignElement=document.createElement('x-foreign');document.body.appendChild(foreignElement);"
            "document.body.appendChild(document.createElement('x-bad'));",
            "DOM recovery: native observer holds and retained animation precede interruption");
        if (modern) realm_script(f.realm[2],
            "document.body.appendChild(B.foreignElement);document.body.appendChild(B.badElement);"
            "shared.native='';customElements.define('x-good',class extends HTMLElement{"
            "static get observedAttributes(){return ['data-probe']}"
            "attributeChangedCallback(){shared.native+='C'}});"
            "globalThis.good=document.createElement('x-good');document.body.appendChild(good);"
            "new MutationObserver(()=>{shared.native+='M'}).observe(good,{attributes:true});",
            "DOM recovery: healthy custom element and observer share native reaction queues");
        os64_js_outcome_t original = dom_recovery_overrun(&f), out;
        if (modern) {
            for (unsigned i = 1; i < 3; ++i)
                check(os64_html_set_attr(f.binding[i]->document, f.binding[i]->document->body->last_child,
                      "data-probe", "changed", 7) == OS64_HTML_OK,
                      "DOM recovery: parser mutation between closed tasks queues native reactions");
            check(os64_html_set_attr(f.binding[2]->document, f.binding[2]->document->body->last_child->prev,
                  "data-probe", "adopted", 7) == OS64_HTML_OK,
                  "DOM recovery: failed definition's adopted element shares the healthy native queue");
            check(os64_html_set_attr(f.binding[2]->document, f.binding[2]->document->body->last_child->prev->prev,
                  "data-probe", "foreign", 7) == OS64_HTML_OK,
                  "DOM recovery: retained failed registry carries a healthy callback on an adopted element");
            check(os64_dom_reactions_owed(f.binding[2]) && !JS_IsJobPending(JS_GetRuntime(f.ctx[0])),
                  "DOM recovery: native owed reactions are distinct from forbidden pending engine jobs");
        }
        for (unsigned i = 0; i < 3; ++i) f.provider[i]->armed = true;
        f.active_outcome = &out;
        check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
              !f.cluster->recovering && f.binding[1]->departure_finished,
              "DOM recovery: native preparation and exact-generation commit enable independent siblings");
        f.active_outcome = NULL;
        if (modern) check(f.provider[1]->prepares == 2 && f.provider[1]->store.commits == 1 &&
                          f.provider[1]->net.cancels == 1 && !f.provider[2]->net.cancels,
                          "DOM recovery: failed provider settles and cancels once while sibling transport survives");
        os64_js_recovery_info_t info;
        check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
              info.state == OS64_JS_RECOVERY_COMMITTED && info.failure.original.limit == original.limit &&
              info.jobs_executed == original.jobs_executed,
              "DOM recovery: commit preserves the original timeout and abandoned-task accounting");
        check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
              out.jobs_executed == info.jobs_executed && out.jobs_discarded == info.jobs_discarded,
              "DOM recovery: repeated acknowledgment preserves final task accounting");
        if (modern) {
            check(os64_dom_reactions_run(f.binding[2], &out) && out.status == OS64_JS_OK,
                  "DOM recovery: independent custom and observer deliveries resume after commit");
            realm_script(f.realm[2], STORAGE_ASSERT "assert(shared.native==='CFM'&&shared.forbidden===undefined,shared.native)",
                         "DOM recovery: failed payloads drop while healthy native reactions retain order");
        }
        DomRecoveryProvider *failed = f.provider[1];
        fake_store_free(&failed->store); os64_free(failed); f.provider[1] = NULL;
        unsigned misses = dom_recovery_misses;
        JSValue global = JS_GetGlobalObject(f.ctx[1]);
        JSValue missing = JS_GetPropertyStr(f.ctx[1], global, "a_missing_failed_global");
        JS_FreeValue(f.ctx[1], missing); JS_FreeValue(f.ctx[1], global);
        check(dom_recovery_misses == misses && !f.binding[1]->global_miss && !f.binding[1]->options.host_opaque,
              "DOM recovery: released provider opaque is unreachable through retained global-miss hooks");
        p2b_advance(1000);
        check(os64_dom_timer_fire(f.binding[2], p2b_ms, &out) && out.status == OS64_JS_OK && f.binding[2]->timer_count == 2,
              "DOM recovery: failed cross-realm interval is suppressed and removed from the healthy queue");
        check(os64_dom_timer_fire(f.binding[2], p2b_ms, &out) && out.status == OS64_JS_OK &&
              os64_dom_timer_fire(f.binding[2], p2b_ms, &out) && out.status == OS64_JS_OK &&
              os64_dom_timer_fire(f.binding[0], p2b_ms, &out) && out.status == OS64_JS_OK,
              "DOM recovery: independent sibling and primary timers retain their native order");
        realm_script(f.realm[2], STORAGE_ASSERT
            "assert(shared.value===42&&shared.order==='12A'&&shared.forbidden===undefined&&shared.escaped===undefined);"
            "document.body.dispatchEvent.call(B.node,new Event('ping'));assert(shared.events==='C');"
            "let proto=document.body,descriptor;while(proto&&!(descriptor=Object.getOwnPropertyDescriptor(proto,'nodeType')))proto=Object.getPrototypeOf(proto);assert(descriptor.get.call(B.node)===1);"
            "let caught=false;try{B.bad(true)}catch(e){caught=true}assert(caught&&shared.forbidden===undefined);",
            "DOM recovery: retained adopted data and healthy listeners survive while failed code stays sealed");
        if (modern) realm_script(f.realm[2], STORAGE_ASSERT
            "let px=CanvasRenderingContext2D.prototype.getImageData.call(B.paint,0,0,1,1).data;"
            "assert(px[0]===255&&px[3]===255)", "DOM recovery: retained Canvas pixels use a healthy realm's native API");
        check(os64_dom_timer_next(f.binding[1]) == UINT64_MAX && !os64_dom_fetches_pending(f.binding[1]),
              "DOM recovery: failed native activity remains stopped after provider storage release");
        dom_recovery_close(&f);
    }
}

static void dom_recovery_busy_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned hold = 0; hold < 4; ++hold) {
        DomRecoveryFixture f;
        if (!dom_recovery_open(&f, policy, true)) continue;
        if (hold == 0) check(d_provider_enter(f.binding[2]), "DOM recovery: sibling provider loan acquired");
        if (hold == 1) os64_dom_effects_mark(f.binding[2]);
        if (hold == 2) check(d_reactions_push(f.binding[2], f.ctx[2]) == 0,
                             "DOM recovery: sibling native reaction frame acquired");
        dom_recovery_overrun(&f);
        f.provider[1]->armed = true;
        if (hold == 3) f.cleanup_action = 1;
        os64_js_outcome_t out;
        check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY &&
              f.cluster->recovering && !f.binding[1]->departure_finished &&
              f.binding[1]->options.host_opaque == f.provider[1] && !d_delivery_active(f.binding[2]),
              "DOM recovery: loan or native walk retains pause, providers and incomplete finish");
        os64_js_recovery_info_t waiting;
        check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &waiting) == OS64_JS_OK &&
              waiting.state == OS64_JS_RECOVERY_WAITING && os64_js_realm_is_failed(f.realm[1]),
              "DOM recovery: BUSY holds the same failed-code fence until native release");
        check(f.provider[1]->store.commits == (hold == 3) && f.provider[1]->net.cancels == (hold == 3),
              "DOM recovery: preexisting loans stop cleanup; a cleanup-created loan stops final detach");
        p2b_advance(1000);
        check(!os64_dom_timer_fire(f.binding[0], p2b_ms, &out) &&
              !os64_dom_timer_fire(f.binding[2], p2b_ms, &out),
              "DOM recovery: waiting native queues do not dispatch or lose healthy work");
        if (hold == 0) d_provider_leave(f.binding[2]);
        if (hold == 1 || hold == 3) os64_dom_effects_release(f.binding[2], UINT64_MAX);
        if (hold == 2) check(d_reactions_pop(f.binding[2], f.ctx[2]) == 0,
                             "DOM recovery: empty native frame releases without author execution");
        check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) == OS64_JS_OK &&
              f.provider[1]->store.commits == 1 && f.provider[1]->net.cancels == 1 &&
              !f.binding[1]->options.host_opaque && !f.cluster->recovering,
              "DOM recovery: retry releases providers after one final settlement and cancellation");
        os64_js_recovery_info_t committed;
        check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &committed) == OS64_JS_OK &&
              committed.generation == waiting.generation && committed.state == OS64_JS_RECOVERY_COMMITTED,
              "DOM recovery: native retry commits the waiting generation");
        dom_recovery_close(&f);
    }
}

static void dom_recovery_fallback_cases(void)
{
    static const char *streams[] = {"new ReadableStream()", "new WritableStream()", "new TransformStream()"};
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < 13; ++mode) {
        DomRecoveryFixture f;
        if (!dom_recovery_open(&f, policy, true)) continue;
        if (mode < 3) {
            realm_script(f.realm[2], streams[mode], "DOM recovery: healthy sibling creates self-hosted Streams state");
            check(f.cluster->streams_used, "DOM recovery: Streams use is sticky across the shared cluster");
        }
        if (mode == 3) realm_script(f.realm[2],
            "globalThis.watch=new MutationObserver(()=>{shared.forbidden=8});watch.observe(document.body,{attributes:true});",
            "DOM recovery: healthy observer registration before new queued work");
        dom_recovery_overrun(&f);
        f.provider[1]->armed = true;
        if (mode == 3) check(JS_EnqueueJob(f.ctx[2], dom_recovery_job, 0, NULL) == 0,
                             "DOM recovery: outstanding engine work remains unsupported");
        if (mode == 4) f.provider[1]->store.forced = OS64_DOM_STORAGE_FULL;
        if (mode == 5) f.provider[1]->store.forced = OS64_DOM_STORAGE_FAILED;
        if (mode == 6) dom_fail_at = dom_attempts + 1;
        if (mode >= 7) f.cleanup_action = mode - 5;
        unsigned jobs_before = dom_recovery_job_hits;
        os64_js_outcome_t out;
        os64_js_status_t status = os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out);
        dom_fail_at = 0; dom_clock = 10000000;
        os64_js_recovery_info_t info;
        check(status != OS64_JS_OK && status != OS64_JS_BUSY &&
              os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
              info.state == OS64_JS_RECOVERY_GLOBAL && info.failure.original.limit == OS64_JS_LIMIT_EXECUTION,
              "DOM recovery: unsupported native or late health state keeps original timeout and root fallback");
        if (mode >= 4 && mode <= 6) check(status == OS64_JS_HOST_FAILURE &&
              out.host_error == -(int64_t)(mode - 3) && !f.binding[1]->departure_finished &&
              f.binding[1]->options.host_opaque == f.provider[1],
              "DOM recovery: storage full, failed and serialization OOM cannot certify native cleanup");
        if (mode >= 7 && mode <= 10) check(info.failure.global.status ==
              (mode == 7 ? OS64_JS_CANCELLED : mode == 10 ? OS64_JS_LIMIT : OS64_JS_HOST_FAILURE) &&
              info.failure.global.limit == (mode == 10 ? OS64_JS_LIMIT_MEMORY : OS64_JS_LIMIT_NONE) &&
              info.failure.global.host_error == (mode == 8 ? -1 : 0),
              "DOM recovery: exact late cancellation, clock, allocator and memory causes remain visible");
        if (mode < 4) check(!f.provider[1]->store.commits && !f.provider[1]->net.cancels,
                            "DOM recovery: unsupported state does not start partial native cleanup");
        check(dom_recovery_job_hits == jobs_before &&
              os64_js_realm_task_begin(f.realm[2], OS64_JS_ABI_ID, "refused sibling", &out) != OS64_JS_OK,
              "DOM recovery: fallback neither drains work nor admits sibling script");
        dom_recovery_close(&f);
    }
}

static void dom_recovery_generation_cases(void)
{
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < 7; ++mode) {
        DomRecoveryFixture f;
        if (!dom_recovery_open(&f, policy, true)) continue;
        os64_js_outcome_t out;
        if (mode == 1) {
            realm_script(f.realm[2], "globalThis.spin=function(){while(true){}}", "DOM recovery: nested child abort function");
            realm_script(f.realm[1], "globalThis.bad=function(){shared.value=42;C.spin()}", "DOM recovery: two-child stack");
        }
        if (mode == 2) realm_script(f.realm[0], "globalThis.bad=function(){while(true){}}",
                                    "DOM recovery: primary author code remains global");
        if (mode == 3) {
            os64_js_realm_t *unbound;
            check(os64_js_realm_create(f.runtime, OS64_JS_ABI_ID, &unbound, &out) == OS64_JS_OK,
                  "DOM recovery: native policy does not silently adopt an unbound realm");
            realm_script(unbound, "globalThis.spin=function(){while(true){}}", "DOM recovery: unbound child abort");
            JSContext *ctx = os64_js_realm_context(unbound, OS64_JS_ABI_ID, &out);
            JSValue b = JS_GetGlobalObject(f.ctx[1]);
            check(JS_SetPropertyStr(f.ctx[1], b, "D", JS_GetGlobalObject(ctx)) >= 0,
                  "DOM recovery: unbound child exported into the bounded caller");
            JS_FreeValue(f.ctx[1], b);
            realm_script(f.realm[1], "globalThis.bad=function(){D.spin()}", "DOM recovery: unbound child enters actual author stack");
        }
        if (mode == 6) {
            check(os64_js_realm_task_begin(f.realm[2], OS64_JS_ABI_ID, "private Streams probe", &out) == OS64_JS_OK,
                  "DOM recovery: private bridge probe uses an ordinary bounded task");
            JSValue global = JS_GetGlobalObject(f.ctx[2]);
            check(d_stream_is(f.binding[2], f.ctx[2], global) == 0 && f.cluster->streams_used,
                  "DOM recovery: private Streams bridge use is sticky even without stream construction");
            JS_FreeValue(f.ctx[2], global);
            check(os64_js_task_end(f.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                  "DOM recovery: private bridge task closes before child overrun");
        }
        if (mode == 5) {
            realm_script(f.realm[1], "setTimeout(bad,0)", "DOM recovery: native timer schedules the failing child callable");
            p2b_advance(1000);
            check(os64_dom_timer_fire(f.binding[1], p2b_ms, &out) && out.status == OS64_JS_OK,
                  "DOM recovery: independent earlier child timer completes before failing delivery");
            dom_clock_step = 1000;
            bool fired = os64_dom_timer_fire(f.binding[1], p2b_ms, &out);
            dom_clock_step = 0;
            check(fired && out.status == OS64_JS_LIMIT && out.limit == OS64_JS_LIMIT_EXECUTION,
                  "DOM recovery: actual native timer delivery unwinds and closes the child timeout task");
        } else dom_recovery_overrun_realm(&f, mode == 2 ? 0 : 1);
        for (unsigned i = 0; i < 3; ++i) f.provider[i]->armed = true;
        if ((mode >= 2 && mode <= 3) || mode == 6) {
            check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) != OS64_JS_OK &&
                  !f.provider[1]->store.commits && !f.provider[1]->net.cancels,
                  "DOM recovery: primary or unbound affected realm prevents native certification");
            os64_js_recovery_info_t info;
            check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
                  info.state == OS64_JS_RECOVERY_GLOBAL, "DOM recovery: unsupported affected set requires root teardown");
        } else {
            if (mode == 4) {
                os64_js_recovery_token_t token;
                check(os64_js_recovery_prepare(f.runtime, OS64_JS_ABI_ID, &token, &out) == OS64_JS_OK &&
                      f.cluster->recovery_completed && f.cluster->recovering,
                      "DOM recovery: native preparation alone does not publish healthy DOM activity");
                os64_dom_effects_mark(f.binding[2]);
                check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) == OS64_JS_BUSY &&
                      !d_delivery_active(f.binding[2]),
                      "DOM recovery: a loan acquired after separate preparation still excludes commit");
                os64_dom_effects_release(f.binding[2], UINT64_MAX);
            }
            check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                  "DOM recovery: supported affected set commits after native safe point");
            os64_js_recovery_info_t first;
            check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &first) == OS64_JS_OK &&
                  first.failure.realm_count == (mode == 1 ? 2u : 1u) &&
                  os64_js_realm_is_failed(f.realm[1]) && os64_js_realm_is_failed(f.realm[2]) == (mode == 1),
                  "DOM recovery: every interrupted child and no independent sibling is sealed");
            check(f.provider[1]->store.commits == 1 && f.provider[1]->net.cancels == 1 &&
                  f.provider[2]->net.cancels == (mode == 1),
                  "DOM recovery: native cancellation follows the entire affected set");
            if (mode == 0) {
                realm_script(f.realm[2], "globalThis.bad=function(){shared.second=42;while(true){}}",
                             "DOM recovery: a fresh sibling task receives its own bounded episode");
                dom_recovery_overrun_realm(&f, 2);
                check(os64_dom_cluster_recover(f.cluster, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "DOM recovery: a second independent child can be contained without renewing its failed task");
                os64_js_recovery_info_t second;
                check(os64_js_recovery_inspect(f.runtime, OS64_JS_ABI_ID, &second) == OS64_JS_OK &&
                      second.generation > first.generation && second.failure.realm_count == 1 &&
                      second.failure.realms[0] == f.realm[2] && f.provider[2]->net.cancels == 1,
                      "DOM recovery: fresh generation cannot reuse the prior child's native acknowledgment");
            }
            realm_script(f.realm[0], STORAGE_ASSERT "assert(shared.forbidden===undefined&&shared.escaped===undefined)",
                         "DOM recovery: unaffected primary continues after complete child containment");
        }
        dom_recovery_close(&f);
    }
}

static void dom_recovery_constructor_cases(void)
{
    os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
    os64_dom_cluster_limits_t limits = {64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
    os64_js_outcome_t out;
    os64_js_runtime_t *runtime = (void *)(uintptr_t)1;
    check(!os64_dom_cluster_create_with_recovery(&config, OS64_JS_TEARDOWN_FATAL, "wrong ABI",
          &limits, &runtime, &out) && !runtime && out.status == OS64_JS_ABI_MISMATCH && !live && !dom_live,
          "DOM recovery: ABI refusal publishes neither handle and frees native groups");
    runtime = (void *)(uintptr_t)1;
    check(!os64_dom_cluster_create_with_recovery(&config, OS64_JS_TEARDOWN_FATAL, OS64_JS_ABI_ID,
          NULL, &runtime, &out) && !runtime && out.status == OS64_JS_BAD_ARGUMENT && !live && !dom_live,
          "DOM recovery: invalid limits cannot publish an unpaired runtime");
    check(!os64_dom_cluster_create_with_recovery(&config, OS64_JS_TEARDOWN_FATAL, OS64_JS_ABI_ID,
          &limits, NULL, &out) && out.status == OS64_JS_BAD_ARGUMENT && !live && !dom_live,
          "DOM recovery: missing runtime destination is refused before allocation");
    for (unsigned policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy) {
        size_t before = attempts;
        os64_dom_cluster_t *cluster = os64_dom_cluster_create_with_recovery(&config, policy,
            OS64_JS_ABI_ID, &limits, &runtime, &out);
        size_t allocations = attempts - before;
        check(cluster && runtime, "DOM recovery: constructor allocation baseline");
        if (!cluster) continue;
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(!live && !dom_live, "DOM recovery: empty paired cluster releases every allocation");
        for (size_t cut = 1; cut <= allocations; ++cut) {
            before = attempts; fail_at = before + cut;
            cluster = os64_dom_cluster_create_with_recovery(&config, policy, OS64_JS_ABI_ID,
                                                          &limits, &runtime, &out);
            fail_at = 0;
            check(attempts >= before + cut && (!!cluster == !!runtime),
                  "DOM recovery: each constructor allocation cut reaches a paired publication boundary");
            if (cluster) {
                os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
            }
            check(!live && !dom_live && !dom_allocations,
                  "DOM recovery: constructor failure releases runtime, native groups and guarded policy storage");
        }
        printf("DOM recovery constructor: %zu allocation cuts (policy %u)\n", allocations, policy);
    }
    /* Existing constructors leave the terminal runtime policy in force. */
    runtime = create_fixture(&config);
    os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
    check(cluster && !cluster->recovery_enabled &&
          os64_dom_cluster_recover(cluster, OS64_JS_ABI_ID, &out) == OS64_JS_ADMISSION_REFUSED,
          "DOM recovery: ordinary cluster construction remains terminal and does not opt in");
    os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
    check(!live && !dom_live, "DOM recovery: ordinary construction keeps its ownership order");
}

static void dom_recovery_cases(void)
{
    dom_recovery_success_cases();
    dom_recovery_busy_cases();
    dom_recovery_fallback_cases();
    dom_recovery_generation_cases();
    dom_recovery_constructor_cases();
}
