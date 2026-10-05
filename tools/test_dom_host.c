/* Reuse the established target heap/syscall fixtures, not DOM expectations. */
#define main js_port_fixture_main
#include "test_js_port_host.c"
#undef main
#include <os64/js_engine.h>
#include "quickjs.h"
#include <dom/dom.h>
#include <html/html.h>
#include <page/page.h>

int64_t os64_open(const char *path, const char *mode)
{ (void)path; (void)mode; return -1; }
int64_t os64_read(int32_t handle, void *buffer, size_t size)
{ (void)handle; (void)buffer; (void)size; return -1; }
int64_t os64_close(int32_t handle)
{ (void)handle; return 0; }
/* A clock that stands still unless a case makes it step, so a deadline is
 * met only where a case means it to be. */
static int64_t dom_clock = 10000000, dom_clock_step;
int64_t os64_micros(void)
{ return dom_clock += dom_clock_step; }
static os64_js_config_t fixture_config(void)
{
    return (os64_js_config_t){.limits={16*1024*1024,256*1024,64*1024,1000,100}};
}
static os64_js_runtime_t *create_fixture(const os64_js_config_t *config)
{
    os64_js_runtime_t *runtime=NULL;
    os64_js_outcome_t outcome;
    check(os64_js_create(config,OS64_JS_ABI_ID,&runtime,&outcome)==OS64_JS_OK&&runtime,
          "runtime creation");
    return runtime;
}
static size_t dom_attempts, dom_fail_at, dom_live, dom_sweeps;
typedef struct DomAllocation {
    void *pointer;
    size_t size;
    struct DomAllocation *next;
} DomAllocation;
static DomAllocation *dom_allocations;
void *dom_host_malloc(size_t size)
{
    dom_attempts++;
    if (dom_fail_at && dom_attempts == dom_fail_at)
        return NULL;
    void *pointer = os64_malloc(size);
    if (!pointer)
        return NULL;
    DomAllocation *record = malloc(sizeof(*record));
    if (!record)
        abort();
    *record = (DomAllocation){pointer, os64_malloc_size(pointer), dom_allocations};
    dom_allocations = record;
    dom_live++;
    return pointer;
}
void *dom_host_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size)
        return NULL;
    void *pointer = dom_host_malloc(count * size);
    if (pointer)
        memset(pointer, 0, count * size);
    return pointer;
}
void dom_host_free(void *pointer)
{
    if (!pointer)
        return;
    DomAllocation **link = &dom_allocations;
    while (*link && (*link)->pointer != pointer)
        link = &(*link)->next;
    if (!*link) {
        fputs("binding freed storage outside its native allocation ledger\n", stderr);
        abort();
    }
    DomAllocation *record = *link;
    *link = record->next;
    dom_live--;
    os64_free(pointer);
    free(record);
}
size_t dom_host_malloc_size(const void *pointer)
{
    return pointer ? os64_malloc_size(pointer) : 0;
}
void *dom_host_realloc(void *pointer, size_t size)
{
    if (!pointer)
        return dom_host_malloc(size);
    if (!size) {
        dom_host_free(pointer);
        return NULL;
    }
    void *replacement = dom_host_malloc(size);
    if (replacement) {
        size_t old = dom_host_malloc_size(pointer);
        memcpy(replacement, pointer, old < size ? old : size);
        dom_host_free(pointer);
    }
    return replacement;
}
void *os64_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size)
        return NULL;
    void *pointer = os64_malloc(count * size);
    if (pointer)
        memset(pointer, 0, count * size);
    return pointer;
}
static char alert_text[512];
static size_t alert_length, alerts;
static void alert_capture(void *opaque, const char *text, size_t length)
{
    check(opaque == alert_text, "alert receives its borrowed host context");
    check(length < sizeof(alert_text), "alert bounded fixture message");
    if (length >= sizeof(alert_text))
        length = sizeof(alert_text) - 1;
    memcpy(alert_text, text, length);
    alert_text[length] = 0;
    alert_length = length;
    alerts++;
}
typedef struct {
    os64_js_runtime_t *runtime;
    os64_html_document_t *document;
    os64_page_state_t *state;
    os64_dom_t *binding;
} DomFixture;
static const char initial_html[] =
    "<!doctype html><html><head><title>fixture</title></head><body>"
    "<div id=box class=old><p id=one>one</p><p id=two>two</p></div>"
    "<form id=form><input id=field value=default><input id=check type=checkbox>"
    "<input id=r1 type=radio name=g checked><input id=r2 type=radio name=g>"
    "<select id=select><option id=o1>A</option><option id=o2>B</option></select></form>"
    "<template id=template><b>inside</b></template></body></html>";
static DomFixture fixture_base(size_t arena, size_t state_cap, size_t engine_cap)
{
    DomFixture fixture = {0};
    os64_html_options_t html_options = os64_html_options_default();
    html_options.charset = "utf-8";
    if (arena) html_options.max_arena_bytes=arena;
    os64_html_parser_t *parser = os64_html_parser_new(&html_options);
    if (!parser)
        return fixture;
    os64_html_parser_feed(parser, initial_html, sizeof(initial_html) - 1);
    fixture.document = os64_html_parser_finish(parser);
    fixture.state = os64_page_state_create(fixture.document, state_cap);
    os64_js_config_t config = fixture_config();
    if(engine_cap) config.limits.memory_bytes=engine_cap;
    fixture.runtime = create_fixture(&config);
    return fixture;
}
static DomFixture fixture_new(const os64_dom_options_t *options)
{
    DomFixture fixture=fixture_base(0,0,0);
    if(fixture.runtime&&fixture.state){
        os64_js_outcome_t outcome;
        fixture.binding=os64_dom_create(fixture.runtime,fixture.document,fixture.state,options,&outcome);
    }
    return fixture;
}

static void fixture_free(DomFixture *fixture)
{
    os64_dom_drain(fixture->binding);
    check(os64_dom_registry_count(fixture->binding) == 0, "drain releases retained engine values");
    os64_dom_drain(fixture->binding);
    os64_js_destroy(fixture->runtime);
    os64_dom_free(fixture->binding);
    os64_page_state_free(fixture->state);
    os64_html_document_free(fixture->document);
    memset(fixture, 0, sizeof(*fixture));
    check(live == 0 && dom_live == 0 && !dom_allocations, "ordered teardown releases all native and engine storage");
}
static int dom_script(DomFixture *fixture, const char *source, const char *name)
{
    os64_js_outcome_t outcome;
    os64_js_status_t result = os64_js_run(fixture->runtime, source, strlen(source), name, &outcome);
    if (result != OS64_JS_OK)
        fprintf(stderr, "DOM script %s: status=%d limit=%d message=%s\n", name, result, outcome.limit, outcome.message);
    check(result == OS64_JS_OK, name);
    return result == OS64_JS_OK;
}
static os64_html_node_t *native_id(os64_html_node_t *node, const char *id)
{
    for (; node; node = node->next) {
        const os64_html_attr_t *attr = os64_html_attr(node, "id");
        if (attr && !strcmp(attr->value, id))
            return node;
        os64_html_node_t *child = native_id(node->first_child, id);
        if (child)
            return child;
        if (node->template_contents) {
            child = native_id(node->template_contents->first_child, id);
            if (child)
                return child;
        }
    }
    return NULL;
}
static char *native_serialized(const os64_html_node_t *node)
{
    size_t length = os64_html_serialize(node, false, true, NULL, 0);
    char *text = malloc(length + 1);
    if (!text)
        abort();
    check(os64_html_serialize(node, false, true, text, length + 1) == length,
          "native serialization sized independently");
    return text;
}
#include "test_dom_cases.inc"
#include "test_libpage_clone.inc"
#include "test_dom_reclaim.inc"
#include "test_dom_events.inc"
int main(int argc, char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--reclaim")){dom_reclaim_cases();printf("DOM reclaim probe: %u checks, %u failed\n",checks,failures);return failures?1:0;}
    if(argc==2&&!strcmp(argv[1],"--clone")){dom_clone_control_cases();dom_clone_native_cases();dom_clone_state_cap_cases();dom_clone_character_data_cases();dom_clone_allocation_cases();printf("DOM clone probe: %u checks, %u failed\n",checks,failures);return failures?1:0;}
    if(argc==2&&!strcmp(argv[1],"--events")){dom_event_cases();printf("DOM events probe: %u checks, %u failed\n",checks,failures);return failures?1:0;}
    bool mutation_mode=argc==2&&!strcmp(argv[1],"--mutants");
    dom_surface_cases();
    dom_identity_cases();
    dom_collection_cases();
    dom_reclaim_cases();
    dom_unicode_cases();
    dom_control_cases();
    dom_clone_control_cases();
    dom_clone_native_cases();
    dom_clone_state_cap_cases();
    dom_clone_character_data_cases();
    dom_refusal_cases();
    dom_extra_surface_cases();
    dom_review_property_cases();
    dom_exception_ownership_cases();
    dom_fragment_mode_cases();
    dom_budget_cases();
    dom_event_cases();
    if(!mutation_mode){dom_allocation_cases();dom_clone_allocation_cases();}
    printf("DOM host: %u checks, %u failed; binding allocation sweeps=%zu\n", checks, failures, dom_sweeps);
    return failures ? 1 : 0;
}
