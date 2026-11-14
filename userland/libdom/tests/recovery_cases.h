#ifndef DOM_RECOVERY_CASES_H
#define DOM_RECOVERY_CASES_H
#include <dom/dom.h>
#include <os64/os64.h>
#include <os64/js_engine.h>
#include <os64/js_failure.h>
#include "quickjs.h"

/* The same owner-thread host loop runs against fake and guest OS services.
 * Raw context exchange is trusted setup; dispatch and decisions use public APIs. */
typedef struct RecoveryConsumer RecoveryConsumer;
typedef struct { RecoveryConsumer *host; unsigned canceled, started; } RecoveryTransport;
struct RecoveryConsumer {
    os64_js_runtime_t *runtime;
    os64_dom_cluster_t *cluster;
    os64_js_realm_t *realm[3];
    os64_dom_t *dom[3];
    JSContext *ctx[3];
    RecoveryTransport transport[3];
    os64_js_outcome_t failed;
    uint64_t generation;
    unsigned custom_prepares;
    bool preparing;
    void (*require)(bool, const char *);
};
static unsigned recovery_consumer_jobs;
static os64_js_status_t recovery_consumer_custom_prepare(void *opaque,
    const os64_js_recovery_token_t *token, const os64_js_failure_record_t *failure, int64_t *error)
{
    (void)token; (void)failure; (void)error;
    ((RecoveryConsumer *)opaque)->custom_prepares++;
    return OS64_JS_OK;
}
static uint64_t recovery_consumer_now(void *opaque)
{ (void)opaque; return 1000; }
static uint64_t recovery_consumer_start(void *opaque, const os64_dom_request_t *request, uint32_t token)
{
    (void)request; (void)token;
    RecoveryTransport *p = opaque;
    return ++p->started;
}
static void recovery_consumer_cancel(void *opaque, uint64_t request)
{
    RecoveryTransport *p = opaque;
    RecoveryConsumer *h = p->host;
    h->require(request == 1, "recovery host: original producer request identity");
    p->canceled++;
    if (h->preparing) {
        os64_js_outcome_t saved = h->failed;
        os64_dom_recovery_result_t result = os64_dom_cluster_resolve_failure(h->cluster,
            OS64_JS_ABI_ID, h->generation, &h->failed);
        h->require(result.action == OS64_DOM_RECOVERY_WAIT && result.status == OS64_JS_BUSY,
                   "recovery host: provider reentry cannot publish a completed host decision");
        h->require(!os64_memcmp(&saved, &h->failed, sizeof(saved)),
                   "recovery host: provider reentry preserves original diagnostics");
    }
}
static JSValue recovery_consumer_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    recovery_consumer_jobs++;
    return JS_UNDEFINED;
}
static void recovery_consumer_run(RecoveryConsumer *h, unsigned realm, const char *source)
{
    os64_js_outcome_t out;
    h->require(os64_js_realm_run(h->realm[realm], source, os64_strlen(source),
        "recovery-consumer", &out) == OS64_JS_OK, "recovery host: independent script admission");
}

static void recovery_consumer_case(os64_js_teardown_policy_t policy, unsigned mode,
    void (*require)(bool, const char *), void (*timeout_clock)(bool))
{
    RecoveryConsumer h = {.require=require};
    recovery_consumer_jobs = 0;
    os64_js_config_t config = {.limits=os64_js_default_limits()};
    config.limits.execution_ms = 5000;
    os64_dom_cluster_limits_t limits = {64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
    os64_js_outcome_t out;
    if (mode == 5) {
        os64_js_recovery_host_t custom = {.opaque=&h,.prepare=recovery_consumer_custom_prepare};
        require(os64_js_create_with_child_recovery(&config, policy, OS64_JS_ABI_ID,
            &custom, &h.runtime, &out) == OS64_JS_OK, "recovery host: custom non-DOM policy fixture");
        h.cluster = os64_dom_cluster_create(h.runtime, &limits, &out);
    } else h.cluster = os64_dom_cluster_create_with_recovery(&config, policy, OS64_JS_ABI_ID,
                                                            &limits, &h.runtime, &out);
    require(h.cluster && h.runtime, "recovery host: paired native policy construction");
    h.realm[0] = os64_js_primary_realm(h.runtime, OS64_JS_ABI_ID, &out);
    os64_dom_installation_t installation = {.engine_headroom=OS64_DOM_CHILD_ENGINE_HEADROOM};
    for (unsigned i = 0; i < OS64_DOM_TIMING_COUNT; ++i)
        installation.timing.at[i] = OS64_DOM_TIMING_NOT_YET;
    for (unsigned i = 0; i < 3; ++i) {
        const char markup[] = "<!doctype html><body><p>native cluster member</p>";
        os64_html_parser_t *parser = os64_html_parser_new(NULL);
        require(parser != NULL, "recovery host: document parser");
        require(os64_html_parser_feed(parser, markup, sizeof(markup)-1) == OS64_HTML_OK,
                "recovery host: document feed");
        os64_html_document_t *doc = os64_html_parser_finish(parser);
        os64_page_state_t *state = os64_page_state_create(doc, 0);
        require(doc && state, "recovery host: document and control state");
        os64_dom_options_t options = os64_dom_default_options();
        options.modern = true; options.url = "https://recovery.example/";
        options.now_ms = recovery_consumer_now;
        if (!i) {
            os64_dom_admission_t admission = os64_dom_create_in_realm(h.cluster, h.realm[i],
                doc, state, &options, &out);
            require(admission.binding && admission.consumed, "recovery host: primary binding admission");
            h.dom[i] = admission.binding;
        } else {
            os64_dom_child_t child = os64_dom_create_child(h.cluster, doc, state,
                                                          &options, &installation, &out);
            require(child.state == OS64_DOM_CHILD_ACTIVE && child.consumed,
                    "recovery host: private child installation and activation");
            h.dom[i] = child.binding; h.realm[i] = child.realm;
        }
        h.ctx[i] = os64_js_realm_context(h.realm[i], OS64_JS_ABI_ID, &out);
        h.transport[i].host = &h;
        os64_dom_network_t network = {.start=recovery_consumer_start,
            .cancel=recovery_consumer_cancel,.opaque=&h.transport[i]};
        os64_dom_set_network(h.dom[i], &network);
    }
    require(os64_js_realm_task_begin(h.realm[0], OS64_JS_ABI_ID, "trusted exchange", &out) == OS64_JS_OK,
            "recovery host: trusted setup task");
    for (unsigned i = 0; i < 3; ++i) {
        JSValue global = JS_GetGlobalObject(h.ctx[i]);
        for (unsigned j = 0; j < 3; ++j) {
            char name[] = {(char)('A'+j),0};
            require(JS_SetPropertyStr(h.ctx[i], global, name, JS_GetGlobalObject(h.ctx[j])) >= 0,
                    "recovery host: trusted peer exchange");
        }
        JS_FreeValue(h.ctx[i], global);
    }
    require(os64_js_task_end(h.runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
            "recovery host: setup task closed");
    recovery_consumer_run(&h, 0, "globalThis.shared={value:0,order:'',forbidden:0};"
        "globalThis.assert=function(x){if(!x)throw Error('host assertion')};setTimeout(()=>shared.order+='A',0)");
    recovery_consumer_run(&h, 1, "globalThis.shared=A.shared;globalThis.kept=document.createElement('div');"
        "kept.expando=42;document.body.appendChild(kept);globalThis.bad=function(){shared.value=42;"
        "try{while(true){}}finally{shared.value=99}};setTimeout(bad,0);globalThis.transport=fetch('/B')");
    recovery_consumer_run(&h, 2, "globalThis.shared=A.shared;globalThis.assert=A.assert;"
        "document.body.appendChild(B.kept);setTimeout(()=>shared.order+='1',0);setTimeout(()=>shared.order+='2',0);"
        "globalThis.transport=fetch('/C')");
    if (mode == 2) recovery_consumer_run(&h, 2, "globalThis.used=new ReadableStream()");
    if (mode == 4) os64_dom_effects_mark(h.dom[2]);
    require(os64_js_set_execution_ms(h.runtime, 100, &out) == OS64_JS_OK,
            "recovery host: bounded event allowance configured outside a task");
    timeout_clock(true);
    bool fired = os64_dom_timer_fire(h.dom[1], 1000, &h.failed);
    timeout_clock(false);
    require(fired && h.failed.status == OS64_JS_LIMIT && h.failed.limit == OS64_JS_LIMIT_EXECUTION,
            "recovery host: native event preserves terminal execution diagnostics");
    os64_js_outcome_t saved = h.failed;
    os64_js_recovery_info_t info;
    require(os64_js_recovery_inspect(h.runtime, OS64_JS_ABI_ID, &info) == OS64_JS_OK &&
        !info.task_open && !info.turn_open && info.failure.realm_count == 1 && info.failure.realms[0] == h.realm[1],
        "recovery host: complete child attribution after host/native unwind");
    h.generation = info.generation;
    if (mode == 1) os64_js_cancel(h.runtime);
    if (mode == 3) require(JS_EnqueueJob(h.ctx[2], recovery_consumer_job, 0, NULL) == 0,
                           "recovery host: pending independent engine work remains visible");
    os64_dom_recovery_result_t result = os64_dom_cluster_resolve_failure(h.cluster,
        OS64_JS_ABI_ID, h.generation+1, &h.failed);
    require(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED &&
        result.status == (mode == 1 ? OS64_JS_CANCELLED : OS64_JS_ADMISSION_REFUSED) &&
        !h.transport[1].canceled, "recovery host: stale generation cannot start containment or authorize dispatch");
    h.preparing = true;
    result = os64_dom_cluster_resolve_failure(h.cluster, OS64_JS_ABI_ID, h.generation, &h.failed);
    h.preparing = false;
    if (mode == 5) {
        require(os64_dom_cluster_recover(h.cluster, OS64_JS_ABI_ID, &out) != OS64_JS_OK &&
            !h.custom_prepares && !h.transport[1].canceled,
            "recovery host: custom libjs preparation cannot certify ordinary DOM providers");
    }
    if (mode == 4) {
        require(result.action == OS64_DOM_RECOVERY_WAIT && result.status == OS64_JS_BUSY &&
            !h.transport[1].canceled, "recovery host: wait retains native loan and producer storage");
        require(!os64_dom_timer_fire(h.dom[2], 1000, &out),
                "recovery host: waiting cluster does not dispatch a healthy queue");
        os64_dom_effects_release(h.dom[2], UINT64_MAX);
        h.preparing = true;
        result = os64_dom_cluster_resolve_failure(h.cluster, OS64_JS_ABI_ID, h.generation, &h.failed);
        h.preparing = false;
    }
    require(!os64_memcmp(&saved, &h.failed, sizeof(saved)),
            "recovery host: admission never replaces original task diagnostics or accounting");
    if (mode == 0 || mode == 4) {
        require(result.action == OS64_DOM_RECOVERY_READY && result.status == OS64_JS_OK &&
            result.generation == h.generation && os64_js_realm_is_failed(h.realm[1]) &&
            !os64_js_realm_is_failed(h.realm[0]) && !os64_js_realm_is_failed(h.realm[2]),
            "recovery host: explicit ready decision seals only affected child");
        require(h.transport[1].canceled == 1 && !h.transport[2].canceled,
                "recovery host: original producer canceled while sibling transport survives");
        require(!os64_dom_timer_fire(h.dom[1], 1000, &out), "recovery host: scheduler drops failed activity");
        require(os64_dom_timer_fire(h.dom[2], 1000, &out) && out.status == OS64_JS_OK &&
            os64_dom_timer_fire(h.dom[2], 1000, &out) && out.status == OS64_JS_OK &&
            os64_dom_timer_fire(h.dom[0], 1000, &out) && out.status == OS64_JS_OK,
            "recovery host: subsequent independent native tasks retain order");
        recovery_consumer_run(&h, 2, "assert(shared.value===42&&shared.order==='12A'&&!shared.forbidden);"
            "assert(B.kept.expando===42&&Object.getOwnPropertyDescriptor(Node.prototype,'ownerDocument').get.call(B.kept)===document);"
            "let blocked=false;try{B.bad()}catch(e){blocked=true}assert(blocked&&shared.value===42)");
        require(!os64_memcmp(&saved, &h.failed, sizeof(saved)),
                "recovery host: later healthy tasks cannot overwrite the abandoned verdict");
        result = os64_dom_cluster_resolve_failure(h.cluster, OS64_JS_ABI_ID, h.generation, &h.failed);
        require(result.action == OS64_DOM_RECOVERY_READY, "recovery host: same-generation acknowledgment is idempotent");
    } else {
        require(result.action == OS64_DOM_RECOVERY_ROOT_REQUIRED && result.status != OS64_JS_OK,
                "recovery host: unsupported or global state cannot become a ready decision");
        require(os64_js_realm_task_begin(h.realm[2], OS64_JS_ABI_ID, "refused sibling", &out) == OS64_JS_FAILED_RUNTIME,
                "recovery host: root decision prevents new sibling execution");
    }
    require(!recovery_consumer_jobs, "recovery host: no pending engine work receives a replacement budget");
    os64_dom_cluster_drain(h.cluster);
    os64_js_teardown_report_t report;
    os64_js_destroy_report(h.runtime, &report);
    os64_dom_cluster_free(h.cluster);
    require(!report.leaked, "recovery host: root cleanup has no leaked engine roots");
}
#endif
