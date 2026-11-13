#include <dom/dom.h>
#include <os64/os64.h>
#include <os64/js_engine.h>
#include "quickjs.h"
#include "realm_cases.h"

#define REALMS_OK ((int32_t)0x446f0100)
static void require(bool pass, const char *what)
{
    if (pass) return;
    os64_serial_log(what);
    os64_exit(REALMS_OK + 1);
}

static void run(os64_js_realm_t *realm, const char *source)
{
    os64_js_outcome_t out;
    os64_js_status_t status = os64_js_realm_run(realm, source, os64_strlen(source), "realmtest", &out);
    if (status != OS64_JS_OK) os64_serial_log(out.message);
    require(status == OS64_JS_OK, "realmtest: script failed");
}

static void lifetime(os64_js_teardown_policy_t policy)
{
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t out;
    os64_js_config_t config = {.limits = os64_js_default_limits()};
    config.limits.memory_bytes = 64u << 20;
    require(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
            "realmtest: runtime");
    os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
    os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
    require(cluster != NULL, "realmtest: cluster");
    os64_js_realm_t *realms[3] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out)};
    os64_dom_t *bindings[3] = {0};
    os64_dom_installation_t installation = {.engine_headroom = OS64_DOM_CHILD_ENGINE_HEADROOM};
    for (unsigned i = 0; i < OS64_DOM_TIMING_COUNT; ++i)
        installation.timing.at[i] = OS64_DOM_TIMING_NOT_YET;
    for (unsigned i = 0; i < 3; ++i) {
        const char markup[] = "<!doctype html><body><p>independent arena</p>";
        os64_html_parser_t *parser = os64_html_parser_new(NULL);
        require(parser != NULL, "realmtest: parser");
        require(os64_html_parser_feed(parser, markup, sizeof(markup)-1) == OS64_HTML_OK, "realmtest: feed");
        os64_html_document_t *document = os64_html_parser_finish(parser);
        require(document != NULL, "realmtest: document");
        os64_page_state_t *state = os64_page_state_create(document, 0);
        os64_dom_options_t options = os64_dom_default_options();
        options.modern = true;
        options.ranges = options.shadow_trees = true;
        options.url = i ? "https://child.example/" : "https://root.example/";
        if (i == 0) {
            os64_dom_admission_t admission = os64_dom_create_in_realm(cluster, realms[i], document, state, &options, &out);
            require(admission.binding && admission.consumed, "realmtest: binding admission");
            bindings[i] = admission.binding;
        } else {
            os64_dom_child_t child = os64_dom_create_child(cluster, document, state, &options, &installation, &out);
            require(child.state == OS64_DOM_CHILD_ACTIVE && child.consumed, "realmtest: staged child commit");
            bindings[i] = child.binding;
            realms[i] = child.realm;
        }
        run(realms[i], "function assert(x){if(!x)throw Error('realm assertion')}assert(document instanceof Document)");
    }
    require(os64_js_realm_task_begin(realms[0], OS64_JS_ABI_ID, "trusted exchange", &out) == OS64_JS_OK,
            "realmtest: task");
    for (unsigned i = 0; i < 3; ++i) {
        JSContext *ctx = os64_js_realm_context(realms[i], OS64_JS_ABI_ID, &out);
        JSValue global = JS_GetGlobalObject(ctx);
        for (unsigned j = 0; j < 3; ++j) {
            JSContext *peer = os64_js_realm_context(realms[j], OS64_JS_ABI_ID, &out);
            require(JS_SetPropertyStr(ctx, global, j == 0 ? "A" : j == 1 ? "B" : "C", JS_GetGlobalObject(peer)) >= 0,
                    "realmtest: peer exchange");
        }
        JS_FreeValue(ctx, global);
    }
    require(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK, "realmtest: task end");
    run(realms[0], dom_realm_adoption_oracle);
    run(realms[2], "assert(A.container===document.querySelector('section'));assert(A.container.firstChild instanceof B.HTMLElement)");
    run(realms[1], "globalThis.pure=n=>n+7;globalThis.callbacks=0;setTimeout(()=>callbacks++,0);");
    require(os64_dom_depart(bindings[1], &out) == OS64_JS_OK, "realmtest: child departure mark");
    require(os64_dom_depart_finish(bindings[1], &out) == OS64_JS_OK, "realmtest: child departure finish");
    require(!os64_dom_timer_fire(bindings[1], UINT64_MAX, &out), "realmtest: departed timer suppressed");
    run(realms[0], "assert(B.pure(5)===12&&B.callbacks===0);assert(container.firstChild instanceof B.HTMLElement);"
                   "container.firstChild.textContent='after departure';assert(container.firstChild.ownerDocument===C.document);");
    run(realms[2], "assert(document.querySelector('section').firstChild.textContent==='after departure');"
                   "assert(A.container===document.querySelector('section'));");
    os64_html_parser_t *parser = os64_html_parser_new(NULL);
    require(parser != NULL, "realmtest: refused candidate parser");
    const char markup[] = "<!doctype html><body>refused child";
    require(os64_html_parser_feed(parser, markup, sizeof(markup)-1) == OS64_HTML_OK, "realmtest: refused candidate feed");
    os64_html_document_t *refused_document = os64_html_parser_finish(parser);
    require(refused_document != NULL, "realmtest: refused candidate document");
    os64_page_state_t *refused_state = os64_page_state_create(refused_document, 0);
    require(refused_state != NULL, "realmtest: refused candidate state");
    installation.engine_headroom = SIZE_MAX;
    os64_dom_child_t refused = os64_dom_create_child(cluster, refused_document, refused_state, NULL, &installation, &out);
    require(refused.state == OS64_DOM_CHILD_REFUSED && !refused.consumed && !refused.binding &&
            out.status == OS64_JS_ADMISSION_REFUSED, "realmtest: local admission refusal");
    os64_page_state_free(refused_state);
    os64_html_document_free(refused_document);
    run(realms[0], "assert(B.pure(1)===8);document.body.setAttribute('data-live','yes');");
    run(realms[2], "assert(A.document.body.getAttribute('data-live')==='yes');");
    os64_dom_cluster_drain(cluster);
    os64_js_destroy(runtime);
    os64_dom_cluster_free(cluster);
    require(os64_heap_verify() == 0, "realmtest: heap after cluster teardown");
}

int main(void)
{
    lifetime(OS64_JS_TEARDOWN_FATAL);
    lifetime(OS64_JS_TEARDOWN_RECLAIM);
    os64_serial_log("realmtest: PASS (staged children, departure, retained adoption, refusal, two teardown policies)");
    return REALMS_OK;
}
