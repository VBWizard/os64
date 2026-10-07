#include <os64/os64.h>
#include <os64/js_engine.h>
#include <dom/dom.h>
#include <garb/cascade.h>
#include "quickjs.h"
#include "../../apps/yonder/geometry.h"

static unsigned checks, failures;
static void check(bool passed, const char *name)
{
    checks++;
    if (!passed) { failures++; os64_printf("FAIL: %s\n", name); }
}

typedef struct {
    os64_html_document_t *doc;
    os64_page_state_t *state;
    os64_page_t *model;
    garb_parsed_t sheet;
    garb_cascade_t *cascade;
    flow_tree_t *tree;
    os64_js_runtime_t *runtime;
    os64_dom_t *dom;
    os64_text_context_t *text;
    os64_text_font_t *font;
    uint64_t version;
    uintptr_t bottom, top, lowest;
    unsigned depth, requested_depth, calls;
} GeometryPage;

/* Sequential fixture owner. Production bindings use retained function data;
 * this probe has one runtime at a time and does not start other threads. */
static GeometryPage *active_page;

static void sample(GeometryPage *page)
{
    volatile unsigned char frame;
    uintptr_t here = (uintptr_t)&frame;
    if (here < page->lowest) page->lowest = here;
}

static bool image_size(void *opaque, const os64_html_node_t *node, int32_t *w, int32_t *h)
{
    (void)node;
    GeometryPage *page = opaque;
    sample(page);
    page->calls++;
    if (page->requested_depth > page->depth) page->depth = page->requested_depth;
    *w = 20;
    *h = 10;
    return true;
}

static bool hex_address(const char **text, uintptr_t *value)
{
    const char *p = *text;
    *value = 0;
    if (p[0] == '0' && p[1] == 'x') p += 2;
    unsigned digits = 0;
    while (*p) {
        unsigned c = (unsigned char)*p, n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else break;
        if (++digits > 16) return false;
        *value = *value * 16 + n;
        p++;
    }
    *text = p;
    return digits != 0;
}

static bool stack_bounds(GeometryPage *page)
{
    int64_t handle = os64_open("/proc/self/maps", "r");
    if (handle < 0) return false;
    char line[512];
    uintptr_t current = (uintptr_t)&handle;
    bool found = false;
    while (os64_readline((int32_t)handle, line, sizeof(line)) == 1) {
        const char *p = line;
        uintptr_t low, high;
        if (!hex_address(&p, &low) || *p++ != '-' || !hex_address(&p, &high)) continue;
        const char *label = os64_strchr(p, '[');
        if (current >= low && current < high && label != NULL &&
            os64_strlen(label) >= 7 && os64_memcmp(label, "[stack:", 7) == 0) {
            page->bottom = low;
            page->top = high;
            page->lowest = high;
            found = true;
            break;
        }
    }
    os64_close((int32_t)handle);
    return found;
}

static void *text_allocate(void *opaque, size_t size)
{
    (void)opaque;
    return os64_malloc(size);
}
static void text_release(void *opaque, void *memory, size_t size)
{
    (void)opaque; (void)size;
    os64_free(memory);
}
static os64_font_status_t fonts(void *opaque, const flow_family_list_t *families,
    bool bold, bool italic, uint32_t px, os64_text_font_t *const **list, size_t *count,
    os64_font_face_info_t *primary)
{
    (void)families; (void)bold; (void)italic; (void)px;
    GeometryPage *page = opaque;
    sample(page);
    *list = &page->font;
    *count = 1;
    *primary = (os64_font_face_info_t){.ascent = 12 * 64, .descent = 4 * 64, .line_height = 16 * 64};
    return OS64_FONT_OK;
}

static bool geometry(void *opaque, const os64_html_node_t *node, os64_dom_geometry_t *out)
{
    GeometryPage *page = opaque;
    int64_t began = os64_micros();
    bool ready = true;
    if (page->tree == NULL || page->version != os64_html_version(page->doc)) {
        out->layouts = 1;
        os64_page_t *model = page->model != NULL ? os64_page_rebuild(page->model) :
            os64_page_build(page->doc, "https://geometry.test/", NULL, page->state);
        garb_sheet_in_t sheet = {.sheet = &page->sheet, .parent = -1};
        garb_cascade_t *cascade = model != NULL ? garb_cascade(&sheet, 1, page->doc,
                                                        (garb_env_t){800, 600}) : NULL;
        flow_env_t env = {.ctx = page, .replaced_size = image_size, .fonts = fonts,
                         .text = page->text, .cascade = cascade,
                         .viewport_height = 600, .viewport_font_px = 16, .zoom = 1000,
                         .scripting = true};
        flow_tree_t *tree = cascade != NULL ? flow_layout(page->doc, model, 800, &env) : NULL;
        if (tree == NULL || flow_incomplete(tree)) {
            flow_free(tree);
            garb_cascade_free(cascade);
            os64_page_free(model);
            ready = false;
        } else {
            flow_free(page->tree);
            garb_cascade_free(page->cascade);
            os64_page_free(page->model);
            page->tree = tree;
            page->cascade = cascade;
            page->model = model;
            page->version = os64_html_version(page->doc);
        }
    }
    uint64_t layouts = out->layouts;
    if (ready) ready = yonder_geometry_snapshot(page->doc, page->tree, node, 800, 600,
                                               1000, (flow_point_t){0, 0}, out);
    int64_t ended = os64_micros();
    out->layouts = layouts;
    out->elapsed_us = layouts != 0 && began >= 0 && ended >= began ? (uint64_t)(ended - began) : 0;
    return ready;
}

static JSValue depth(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    int32_t value = 0;
    if (argc == 0 || JS_ToInt32(ctx, &value, argv[0]) < 0) return JS_EXCEPTION;
    active_page->requested_depth = value > 0 ? (unsigned)value : 0;
    sample(active_page);
    return JS_UNDEFINED;
}

static bool attach(GeometryPage *page)
{
    os64_text_options_t text_options = {.memory = {NULL, text_allocate, text_release},
        .backend = os64_freetype_backend_v1(), .memory_cap = 8 * 1024 * 1024};
    if (os64_text_create(&text_options, &page->text) != OS64_FONT_OK ||
        os64_text_font_bitmap(page->text, &page->font) != OS64_FONT_OK) return false;
    page->state = os64_page_state_create(page->doc, 0);
    os64_js_config_t config = {.limits = os64_js_default_limits()};
    config.limits.stack_bytes = 128 * 1024;
    os64_js_outcome_t outcome;
    if (page->state == NULL || os64_js_create(&config, OS64_JS_ABI_ID, &page->runtime, &outcome) != OS64_JS_OK)
        return false;
    page->dom = os64_dom_create(page->runtime, page->doc, page->state, NULL, &outcome);
    if (page->dom == NULL || garb_parse_sheet_text("html,body{margin:0}", os64_strlen("html,body{margin:0}"), &page->sheet) != GARB_OK)
        return false;
    os64_dom_set_geometry(page->dom, geometry, page);
    JSContext *ctx = os64_js_context(page->runtime, OS64_JS_ABI_ID, &outcome);
    if (ctx == NULL) return false;
    JSValue global = JS_GetGlobalObject(ctx);
    int result = JS_DefinePropertyValueStr(ctx, global, "recordDepth",
                                         JS_NewCFunction(ctx, depth, "recordDepth", 1), JS_PROP_C_W_E);
    JS_FreeValue(ctx, global);
    return result >= 0;
}

static void drop(GeometryPage *page)
{
    os64_dom_drain(page->dom);
    os64_js_destroy(page->runtime);
    os64_dom_free(page->dom);
    flow_free(page->tree);
    os64_text_font_release(page->font);
    if (page->text != NULL) check(os64_text_destroy(page->text) == OS64_FONT_OK, "layout releases its text context");
    garb_cascade_free(page->cascade);
    garb_free(&page->sheet);
    os64_page_free(page->model);
    os64_page_state_free(page->state);
    os64_html_document_free(page->doc);
    active_page = NULL;
    check(os64_heap_verify() == 0, "geometry fixture teardown preserves heap integrity");
}

static bool run(GeometryPage *page, const char *source, os64_js_outcome_t *outcome)
{
    os64_js_status_t result = os64_js_run(page->runtime, source, os64_strlen(source), "geometrytest", outcome);
    if (result != OS64_JS_OK) os64_printf("geometry status=%u: %s\n", result, outcome->message);
    return result == OS64_JS_OK;
}

static void partial_document(void)
{
    os64_html_options_t options = os64_html_options_default();
    options.scripting = true;
    os64_html_parser_t *parser = os64_html_parser_new(&options);
    static const char markup[] = "<!doctype html><div id=box style='width:100px;height:40px;padding:3px;border:2px solid'>"
        "</div><script></script><div id=later style='height:50px'></div>";
    check(parser != NULL, "partial geometry parser creation");
    if (parser == NULL) return;
    int64_t parsed = os64_html_parser_feed(parser, markup, sizeof(markup) - 1);
    check(parsed == OS64_HTML_OK || parsed == OS64_HTML_SCRIPT, "partial geometry source feed");
    check(os64_html_parser_end(parser) == OS64_HTML_SCRIPT, "geometry parser stops at script");
    GeometryPage page = {.doc = os64_html_parser_document(parser)};
    active_page = &page;
    check(attach(&page), "partial tree geometry binding");
    os64_js_outcome_t outcome;
    check(run(&page, "var box=document.getElementById('box');"
        "if(box.offsetWidth!==110||box.clientWidth!==106)throw Error('partial dimensions');"
        "if(document.documentElement.clientHeight!==600)throw Error('partial viewport');"
        "if(document.getElementById('later')!==null)throw Error('parser advanced');"
        "box.setAttribute('style','width:140px;height:40px');"
        "if(box.offsetWidth!==140)throw Error('partial mutation');", &outcome),
        "script reads fresh geometry and mutates the stopped partial tree");
    check(os64_html_parser_resume(parser) == OS64_HTML_OK, "geometry parser resumes");
    os64_html_document_t *finished = os64_html_parser_finish(parser);
    check(finished == page.doc, "parser finish preserves geometry document identity");
    check(run(&page, "if(!document.getElementById('later'))throw Error('resume');"
        "if(document.getElementById('later').offsetHeight!==50)throw Error('resume geometry');", &outcome),
        "geometry refreshes after the parser resumes");
    drop(&page);
}

/* The recursion that forces a layout at increasing JavaScript depth until the
 * engine's stack guard ends it; `dive` is defined by the caller's setup. */
static const char kDive[] =
    "var image=document.getElementById('probe');var layouts=0;var n=0;"
    "function dive(d){if(d){return dive(d-1)+1}"
    "image.setAttribute('width',String(20+layouts++));return image.offsetWidth}"
    "function descend(){for(n=0;n<4096;n+=32){try{recordDepth(n);dive(n)}catch(e){"
    "if(e.name!=='RangeError'&&e.name!=='InternalError')throw e;break}}}";

/* `dispatch`: the descent runs in a listener the host dispatches to, which
 * dispatches to a second listener (D7's path: os64_dom_dispatch, libdom's
 * invoke and run_record, JS_Call, and a nested dispatchEvent beneath it). */
static void stack_case(const char *name, const char *open, const char *close, unsigned count,
                       bool dispatch)
{
    size_t a = os64_strlen(open), b = os64_strlen(close);
    size_t capacity = (a + b) * count + 256;
    char *markup = os64_malloc(capacity);
    check(markup != NULL, "deep layout markup allocation");
    if (markup == NULL) return;
    size_t n = 0;
    const char *prefix = "<!doctype html>";
    os64_memcpy(markup + n, prefix, os64_strlen(prefix)); n += os64_strlen(prefix);
    for (unsigned i = 0; i < count; i++) { os64_memcpy(markup + n, open, a); n += a; }
    const char *image = "<img id=probe width=20 height=10>";
    os64_memcpy(markup + n, image, os64_strlen(image)); n += os64_strlen(image);
    for (unsigned i = 0; i < count; i++) { os64_memcpy(markup + n, close, b); n += b; }
    os64_html_options_t options = os64_html_options_default();
    options.max_depth = 4096;
    os64_html_parser_t *parser = os64_html_parser_new(&options);
    os64_html_parser_feed(parser, markup, n);
    GeometryPage page = {.doc = os64_html_parser_finish(parser)};
    os64_free(markup);
    active_page = &page;
    check(stack_bounds(&page), "combined layout probe reads mapped native stack");
    check(attach(&page), "combined layout probe attaches a budgeted runtime");
    os64_js_outcome_t outcome;
    check(run(&page, kDive, &outcome), "deep layout probe defines its descent");
    if (dispatch) {
        check(run(&page,
            "image.addEventListener('probe',function(){descend()});"
            "document.body.addEventListener('click',function(){image.dispatchEvent(new Event('probe'))});",
            &outcome), "listener chain installs");
        os64_dom_event_t click = {.type = "click", .bubbles = true};
        bool prevented = false;
        check(os64_dom_dispatch(page.dom, page.doc->body, &click, &prevented, &outcome) == OS64_JS_OK,
              "host dispatch reaches the nested listener's descent");
    } else {
        check(run(&page, "descend()", &outcome), "descent runs from a script");
    }
    check(run(&page, "if(n<64||n===4096)throw Error('stack guard not exercised');", &outcome),
        "native layout runs beneath increasing JavaScript frames and engine guard remains reusable");
    check(page.calls != 0 && page.lowest >= page.bottom + 128 * 1024,
          "sampled combined layout retains at least 128 KiB native headroom");
    os64_printf("geometry-stack,%s,%u,%u,%lu,%lu\n", name, count, page.depth,
        (unsigned long)(page.top - page.lowest), (unsigned long)(page.lowest - page.bottom));
    check(run(&page, "if(6*7!==42)throw Error('reuse')", &outcome), "runtime reuses after the recursion limit");
    drop(&page);
}

int main(void)
{
    partial_document();
    stack_case("blocks", "<div>", "</div>", 509, false);
    stack_case("tables", "<table><tr><td>", "</td></tr></table>", 126, false);
    stack_case("inline-blocks", "<span style='display:inline-block'>", "</span>", 254, false);
    stack_case("absolute", "<div style='position:absolute;width:100px;height:100px'>", "</div>", 254, false);
    stack_case("blocks-dispatch", "<div>", "</div>", 509, true);
    os64_printf("domgeometrytest: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0x47454F4D; /* GEOM */
}
