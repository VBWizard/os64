/* Reporting is document activity; callback provenance is independent of a
 * custom-element definition's retained registry storage. */
void d_report_drain(os64_dom_t *dom);
enum { RR_NOTICE_REENTRY, RR_NOTICE_RETIRE, RR_REJECTION_RETIRE, RR_CLOSE_AGGREGATE,
       RR_REPORT_RETIRED, RR_ERROR_LISTENER_RETIRE, RR_CALLBACK_LIVE, RR_CALLBACK_RETIRED,
       RR_CONSTRUCTOR, RR_UPGRADE, RR_CALLBACK_SCRIPT, RR_MODULE_RESOLVE, RR_MODULE_SOURCE, RR_COUNT };
typedef struct {
    os64_dom_t *binding, *sibling;
    os64_js_realm_t *realm;
    unsigned mode, calls, resolves, sources;
} ReportRetirement;

static void module_callback_retire(ReportRetirement *p)
{
    os64_js_outcome_t out;
    check(os64_js_realm_retire(p->realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
          "report retirement: loader marks realm during engine call");
    check(os64_js_realm_set_module_loader(p->realm, OS64_JS_ABI_ID, NULL, &out) == OS64_JS_BUSY &&
          os64_js_realm_set_script_host(p->realm, OS64_JS_ABI_ID, NULL, &out) == OS64_JS_BUSY,
          "report retirement: engine barrier retains loader and report host through callback");
}

static char *retiring_module_resolve(void *opaque, const char *base, const char *specifier)
{
    (void)base; (void)specifier;
    ReportRetirement *p = opaque;
    p->resolves++;
    if (p->mode == RR_MODULE_RESOLVE) module_callback_retire(p);
    char *name = os64_malloc(4);
    if (name) memcpy(name, "dep", 4);
    return name;
}

static bool retiring_module_source(void *opaque, const char *name, const char **bytes, size_t *length)
{
    (void)name;
    ReportRetirement *p = opaque;
    p->sources++;
    module_callback_retire(p);
    *bytes = "globalThis.sideEffect++;export const x=1;";
    *length = strlen(*bytes);
    return true;
}

static void retirement_failed_notice(void *opaque, const char *task, const char *message, const char *stack)
{
    ReportRetirement *p = opaque;
    p->calls++;
    bool busy = d_provider_busy(p->binding) && d_provider_busy(p->sibling);
    check(busy,
          "report retirement: diagnostic loan visible across cluster");
    char saved[OS64_JS_MESSAGE_CAP]; snprintf(saved, sizeof(saved), "%s", message);
    os64_js_outcome_t nested = {0};
    if (busy)
        check(!os64_dom_script_ended(p->binding, "nested close", &nested),
              "report retirement: native reentry cannot consume another report task");
    if (!strcmp(saved, "Error: debt")) {
        check(!os64_dom_take_report(p->binding, &nested),
              "report retirement: close detaches report before native callback");
        check(!strcmp(message, saved), "report retirement: borrowed diagnostic text survives reentry");
    }
    if (d_provider_busy(p->binding)) d_report_drain(p->binding);
    check(task != NULL && stack != NULL, "report retirement: diagnostic arguments remain borrowed");
    if (p->mode == RR_NOTICE_RETIRE || p->mode == RR_REJECTION_RETIRE ||
        (p->mode == RR_CLOSE_AGGREGATE && !strcmp(saved, "Error: debt"))) {
        check(os64_js_realm_retire(p->realm, OS64_JS_ABI_ID, &nested) == OS64_JS_OK,
              "report retirement: native notice marks document");
        check(d_provider_busy(p->binding) && !d_provider_enter(p->binding),
              "report retirement: mark keeps diagnostic loan but refuses new work");
    }
}

static void dom_report_retirement_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < RR_COUNT; ++mode) {
        os64_js_config_t config = fixture_config(); config.limits.memory_bytes = 64u << 20;
        os64_js_runtime_t *runtime = NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config, policy, OS64_JS_ABI_ID, &runtime, &out) == OS64_JS_OK,
              "report retirement: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits = {64u<<20, 16u<<20, 8u<<20, 256u<<20, 128u<<20};
        os64_dom_cluster_t *cluster = os64_dom_cluster_create(runtime, &limits, &out);
        if (!cluster) { check(false, "report retirement: cluster created"); os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[2] = {os64_js_primary_realm(runtime, OS64_JS_ABI_ID, &out), NULL};
        check(os64_js_realm_create(runtime, OS64_JS_ABI_ID, &realms[1], &out) == OS64_JS_OK,
              "report retirement: child realm created");
        ReportRetirement p[2] = {{.mode=mode}, {.mode=mode}};
        for (unsigned i = 0; i < 2 && realms[i]; ++i) {
            DomFixture f = fixture_configured(0, 0, &config); os64_js_destroy(f.runtime);
            os64_dom_options_t options = p2b_options("https://reports.example/");
            options.failed = retirement_failed_notice; options.host_opaque = &p[i];
            os64_dom_admission_t a = os64_dom_create_in_realm(cluster, realms[i], f.document, f.state, &options, &out);
            p[i].binding = a.binding; p[i].realm = realms[i];
            check(a.binding && a.consumed, "report retirement: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (p[0].binding && p[1].binding) {
            p[0].sibling = p[1].binding; p[1].sibling = p[0].binding;
            for (unsigned i = 0; i < 2; ++i)
                realm_script(realms[i], STORAGE_ASSERT
                    "globalThis.errors=[];addEventListener('error',e=>errors.push([e.error&&e.error.message,e.filename]));",
                    "report retirement: independent error ledgers");
            retirement_realms[1] = realms[1];
            JSContext *ac = os64_js_realm_context(realms[0], OS64_JS_ABI_ID, &out);
            JSContext *bc = os64_js_realm_context(realms[1], OS64_JS_ABI_ID, &out);
            JSValue global = JS_GetGlobalObject(ac);
            check(JS_SetPropertyStr(ac, global, "B", JS_GetGlobalObject(bc)) >= 0 &&
                  JS_SetPropertyStr(ac, global, "retireChild", JS_NewCFunctionMagic(ac, retirement_mark,
                      "retireChild", 0, JS_CFUNC_generic_magic, 1)) >= 0,
                  "report retirement: retained child and native mark exported");
            JS_FreeValue(ac, global);
            if (mode <= RR_CLOSE_AGGREGATE) {
                if (mode == RR_REJECTION_RETIRE || mode == RR_CLOSE_AGGREGATE)
                    realm_script(realms[1],
                        "for(let i=0;i<20;i++)Promise.reject('rejected '+i);",
                        "report retirement: queued rejection batch");
                if (mode != RR_REJECTION_RETIRE)
                    realm_script(realms[0], "B.reportError(new Error('debt'));",
                                 "report retirement: pending native report");
                out = (os64_js_outcome_t){.status = mode == RR_NOTICE_REENTRY || mode == RR_NOTICE_RETIRE
                                                       ? OS64_JS_EXCEPTION : OS64_JS_OK};
                check(os64_dom_script_ended(p[1].binding, "report turn", &out),
                      "report retirement: outer report task completes");
                unsigned expected = mode == RR_CLOSE_AGGREGATE ? 17 : 1;
                check(p[1].calls == expected && p[0].calls == 0 && !d_provider_busy(p[0].binding),
                      "report retirement: notices stop after mark without duplicate close");
                if (mode == RR_NOTICE_REENTRY)
                    check(os64_js_realm_retire(realms[1], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                          "report retirement: retire after reentry check");
                out = (os64_js_outcome_t){0};
                check(os64_dom_script_ended(p[1].binding, "late close", &out) && out.status == OS64_JS_OK &&
                      p[1].calls == expected, "report retirement: retired close has no stale diagnostic debt");
            } else if (mode <= RR_ERROR_LISTENER_RETIRE) {
                if (mode == RR_REPORT_RETIRED)
                    check(os64_js_realm_retire(realms[1], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                          "report retirement: mark before borrowed reportError");
                else
                    realm_script(realms[0], "B.addEventListener('error',retireChild);",
                                 "report retirement: error listener retires recipient");
                realm_script(realms[0], "B.reportError(new Error('first'));B.reportError(new Error('late'));",
                             "report retirement: reports stop with recipient activity");
                realm_script(realms[0], mode == RR_REPORT_RETIRED ? "assert(B.errors.length===0);" :
                             "assert(B.errors.length===1);", "report retirement: no late error event");
                if (mode == RR_REPORT_RETIRED)
                    check(!os64_dom_take_report(p[1].binding, &out),
                          "report retirement: inactive reportError creates no diagnostic debt");
                out = (os64_js_outcome_t){0};
                check(os64_dom_script_ended(p[1].binding, "retired errors", &out) && out.status == OS64_JS_OK &&
                      !os64_dom_take_report(p[1].binding, &out), "report retirement: obsolete report discarded");
            } else if (mode == RR_MODULE_RESOLVE || mode == RR_MODULE_SOURCE) {
                realm_script(realms[1], "globalThis.sideEffect=0;globalThis.loaded=0;",
                             "report retirement: module effects ledger");
                os64_js_module_loader_t loader = {&p[1], retiring_module_resolve, retiring_module_source};
                check(os64_js_realm_set_module_loader(realms[1], OS64_JS_ABI_ID, &loader, &out) == OS64_JS_OK,
                      "report retirement: native module provider installed");
                os64_js_module_t *dep = NULL, *entry = NULL;
                const char *source = "globalThis.sideEffect++;export const x=1;";
                if (mode == RR_MODULE_RESOLVE)
                    check(os64_js_realm_module_compile(realms[1], OS64_JS_ABI_ID, source, strlen(source),
                          "dep", NULL, &dep, &out) == OS64_JS_OK, "report retirement: dependency already compiled");
                source = "import {x} from 'dep';globalThis.loaded=x;";
                check(os64_js_realm_module_compile(realms[1], OS64_JS_ABI_ID, source, strlen(source),
                      "entry", NULL, &entry, &out) == OS64_JS_OK &&
                      os64_js_realm_task_begin(realms[1], OS64_JS_ABI_ID, "import", &out) == OS64_JS_OK,
                      "report retirement: module task prepared");
                check(os64_js_realm_module_run(realms[1], OS64_JS_ABI_ID, entry, &out) == OS64_JS_EXCEPTION &&
                      strstr(out.message, "module realm is not active") != NULL,
                      "report retirement: loader retirement refuses continued link or compile");
                check(os64_js_task_end(runtime, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                      "report retirement: rejected import ends without shared failure");
                check(p[1].resolves == 1 && p[1].sources == (mode == RR_MODULE_SOURCE ? 1u : 0u),
                      "report retirement: no later loader entry after mark");
                realm_script(realms[0], "assert(B.sideEffect===0&&B.loaded===0);",
                             "report retirement: callback retirement cannot execute a new module");
                check(os64_js_realm_module_release(realms[1], OS64_JS_ABI_ID, entry, &out) == OS64_JS_OK &&
                      os64_js_realm_module_release(realms[1], OS64_JS_ABI_ID, dep, &out) == OS64_JS_OK &&
                      os64_js_realm_set_module_loader(realms[1], OS64_JS_ABI_ID, NULL, &out) == OS64_JS_OK,
                      "report retirement: quiescent retired loader can detach and release handles");
            } else {
                char own_name[64], other_name[64];
                if (mode == RR_CALLBACK_SCRIPT)
                    check(os64_dom_script_name(p[0].binding, "https://reports.example/reserved.js",
                          own_name, sizeof(own_name)), "report retirement: offset callable script table");
                check(os64_dom_script_name(p[0].binding, "https://reports.example/live.js", own_name, sizeof(own_name)) &&
                      os64_dom_script_name(p[1].binding, "https://foreign.example/muted.js", other_name, sizeof(other_name)) &&
                      (mode == RR_CALLBACK_SCRIPT ? strcmp(own_name, other_name) != 0 : !strcmp(own_name, other_name)),
                      "report retirement: independent script tables include collisions and unequal ids");
                realm_script(realms[0],
                    "globalThis.reaction=function(){throw new Error('reaction')};"
                    "globalThis.Failing=class extends B.HTMLElement{constructor(){super();throw new Error('constructor')}};",
                    own_name);
                if (mode == RR_CALLBACK_LIVE || mode == RR_CALLBACK_RETIRED || mode == RR_CALLBACK_SCRIPT) {
                    realm_script(realms[0],
                        "B.reaction=reaction;B.eval(\"class Kept extends HTMLElement {static observedAttributes=['data-x']};"
                        "Kept.prototype.attributeChangedCallback=reaction;customElements.define('x-kept',Kept);"
                        "globalThis.kept=document.createElement('x-kept');\");document.body.appendChild(B.kept);",
                        "report retirement: definition retains live foreign callback");
                    if (mode == RR_CALLBACK_RETIRED)
                        check(os64_js_realm_retire(realms[1], OS64_JS_ABI_ID, &out) == OS64_JS_OK,
                              "report retirement: definition registry retires after adoption");
                    realm_script(realms[0], "B.kept.setAttribute('data-x','changed');",
                                 "report retirement: adopted retained definition callback throws");
                } else {
                    if (mode == RR_UPGRADE)
                        realm_script(realms[0], "B.document.body.appendChild(B.document.createElement('x-fail'));",
                                     "report retirement: upgrade candidate exists");
                    realm_script(realms[0], "B.customElements.define('x-fail',Failing);",
                                 "report retirement: foreign constructor registered");
                    if (mode == RR_CONSTRUCTOR)
                        realm_script(realms[0], "B.document.createElement('x-fail');",
                                     "report retirement: synchronous foreign constructor throws");
                }
                realm_script(realms[0],
                    "assert(errors.length===1&&B.errors.length===0);"
                    "assert(errors[0][1]==='https://reports.example/live.js');"
                    "assert(errors[0][0]==='reaction'||errors[0][0]==='constructor');",
                    "report retirement: failure uses callable realm and its unmuted script identity");
                check(os64_dom_take_report(p[0].binding, &out) && out.status == OS64_JS_EXCEPTION &&
                      !os64_dom_take_report(p[1].binding, &out),
                      "report retirement: report debt belongs to callable binding");
            }
            realm_script(realms[0], "errors=[];reportError(new Error('sibling'));assert(errors.length===1);",
                         "report retirement: sibling reporting remains live");
            check(p[0].calls == 0 && !d_provider_busy(p[0].binding),
                  "report retirement: no sibling diagnostic provider used accidentally");
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        check(live == 0 && dom_live == 0 && !dom_allocations, "report retirement: native and engine holds released");
    }
}
