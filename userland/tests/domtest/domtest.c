#include <dom/dom.h>
#include <os64/os64.h>

#define DOMTEST_OK ((int32_t)0x446f0000)
#define DOMTEST_FAIL ((int32_t)0x446f0001)

static void require(bool good, const char *message)
{
    if (good)
        return;
    os64_printf("domtest: FAIL: %s\n", message);
    os64_serial_log(message);
    os64_exit(DOMTEST_FAIL);
}

static void eval(os64_js_runtime_t *runtime, const char *source)
{
    os64_js_outcome_t outcome;
    os64_js_status_t status = os64_js_run(runtime, source, os64_strlen(source),
                                         "domtest:script", &outcome);
    if (status != OS64_JS_OK)
        os64_printf("domtest: script status=%d %s\n", status, outcome.message);
    require(status == OS64_JS_OK, "script evaluation");
}

/* This fixture proves the shared-library seam on the target heap. Yonder's
 * script scheduling, repaint and navigation acceptance belong to D5b. */
static void round_trip(void)
{
    static const char markup[] = "<title>DOM</title><body><h1 id=h>before</h1>"
                                 "<form><input id=q name=q value=default>"
                                 "<textarea id=a>area</textarea></form>";
    os64_html_parser_t *parser = os64_html_parser_new(NULL);
    require(parser != NULL, "parser creation");
    require(os64_html_parser_feed(parser, markup, sizeof(markup) - 1) == OS64_HTML_OK,
            "parse feed");
    os64_html_document_t *document = os64_html_parser_finish(parser);
    require(document != NULL && document->refusal == OS64_HTML_OK, "parse finish");
    os64_page_state_t *state = os64_page_state_create(document, 0);
    require(state != NULL, "control state creation");
    os64_page_t *model = os64_page_build(document, "https://fixture.test/dom", NULL, state);
    require(model != NULL && !os64_page_incomplete(model), "page model creation");
    require(os64_page_set_text(model, 0, "typed", 5) == 0, "user field edit");

    os64_js_config_t config = {.limits = os64_js_default_limits()};
    os64_js_runtime_t *runtime = NULL;
    os64_js_outcome_t outcome;
    require(os64_js_create(&config, OS64_JS_ABI_ID, &runtime, &outcome) == OS64_JS_OK,
            "runtime creation");
    os64_dom_t *dom = os64_dom_create(runtime, document, state, NULL, &outcome);
    require(dom != NULL && outcome.status == OS64_JS_OK, "binding creation");
    eval(runtime,
         "function check(x,what){if(!x)throw Error(what||'fixture assertion')};"
         "var held=document.getElementById('h');held.extra=42;"
         "var children=document.body.childNodes;var n=children.length;"
         "held.textContent='after';"
         "var p=document.createElement('p');p.innerHTML='<b>made</b>';"
         "check(document.body.appendChild(p)===p);"
         "check(children.length===n+1 && children.item(n)===p);"
         "check(held===document.getElementById('h') && held.extra===42);"
         "check(document.getElementById('q').value==='typed');");
    os64_page_t *next = os64_page_rebuild(model);
    require(next != NULL && !os64_page_incomplete(next), "transactional model rebuild");
    require(os64_streq(os64_page_control(next, 0)->value, "typed"), "edit survived rebuild");
    os64_page_free(model);
    eval(runtime,
         "check(held===document.getElementById('h') && held.extra===42);"
         "var q=document.getElementById('q');q.setAttribute('type','hidden');"
         "q.setAttribute('type','text');check(q.value==='typed');"
         "q.value='clone-typed';"
         "var qc=q.cloneNode();qc.setAttribute('value','changed-default');"
         "check(qc.value==='clone-typed' && q.value==='clone-typed','dirty input clone');"
         "var a=document.getElementById('a'),ac=a.cloneNode();"
         "check(ac.value==='area' && ac.textContent==='','clean shallow textarea clone');"
         "p.setAttribute('title','unrelated');check(ac.value==='area','unrelated clone mutation');"
         "var text=document.createTextNode('changed');ac.appendChild(text);"
         "ac.removeChild(text);check(ac.value==='','clean clone child history');"
         "var ad=a.cloneNode(true);ad.value='edited-area';"
         "var adc=ad.cloneNode(true);adc.firstChild.data='new-default';"
         "check(adc.value==='edited-area','dirty textarea clone');"
         "var tick=document.createElement('input');tick.setAttribute('type','checkbox');"
         "tick.checked=true;var tickc=tick.cloneNode();"
         "tickc.removeAttribute('checked');check(tickc.checked,'dirty checked clone');"
         "check(confirm('continue')===false && prompt('value')===null);");
    os64_dom_drain(dom);
    os64_dom_drain(dom);
    require(os64_dom_registry_count(dom) == 0, "registry drain");
    os64_js_destroy(runtime);
    os64_dom_free(dom);
    os64_page_free(next);
    os64_page_state_free(state);
    os64_html_document_free(document);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    for (unsigned i = 0; i < 4; i++)
        round_trip();
    require(os64_heap_verify() == 0, "heap after teardown");
    os64_printf("domtest: PASS (4 document/runtime lifetimes)\n");
    os64_serial_log("domtest: PASS");
    return DOMTEST_OK;
}
