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
    for (unsigned i = 0; i < 3; ++i) {
        if (i) require(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[i], &out) == OS64_JS_OK,
                       "realmtest: child");
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
        os64_dom_admission_t admission = os64_dom_create_in_realm(cluster, realms[i], document, state, &options, &out);
        require(admission.binding && admission.consumed, "realmtest: binding admission");
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
    os64_dom_cluster_drain(cluster);
    os64_js_destroy(runtime);
    os64_dom_cluster_free(cluster);
    require(os64_heap_verify() == 0, "realmtest: heap after cluster teardown");
}

int main(void)
{
    lifetime(OS64_JS_TEARDOWN_FATAL);
    lifetime(OS64_JS_TEARDOWN_RECLAIM);
    os64_serial_log("realmtest: PASS (three realms, two teardown policies)");
    return REALMS_OK;
}
