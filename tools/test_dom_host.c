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
#include "test_dom_classic.inc"
#define GEOMETRY_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let ok=false;try{f()}catch(e){ok=e.name===n}assert(ok,n)}"

typedef struct {
    DomFixture *fixture;
    unsigned calls;
    bool refuse, slow;
} GeometryFixture;

static bool fake_geometry(void *opaque, const os64_html_node_t *node, os64_dom_geometry_t *out)
{
    GeometryFixture *fixture=opaque;
    check(node->kind==OS64_HTML_ELEMENT, "geometry provider receives a native element");
    fixture->calls++;
    *out=(os64_dom_geometry_t){.x=1.25,.y=-3.5,.width=20.5,.height=30.25,
        .offset_left=2,.offset_top=3,.offset_width=21,.offset_height=30,
        .client_left=1,.client_top=2,.client_width=19,.client_height=27,
        .offset_parent=fixture->fixture->document->body,.layouts=1,.elapsed_us=250};
    if(fixture->slow) dom_clock+=2000000;
    return !fixture->refuse;
}

static void dom_geometry_cases(void)
{
    DomFixture f=fixture_new(NULL);
    if(!fixture_ready(&f)) return;
    dom_script(&f,GEOMETRY_ASSERT
        "throws('InvalidStateError',()=>document.body.offsetWidth);"
        "throws('InvalidStateError',()=>document.body.getBoundingClientRect());", "geometry-without-provider");
    dom_script(&f,GEOMETRY_ASSERT
        "var holder=document.createElement('div');holder.innerHTML='<svg><rect/></svg>';"
        "for(const foreign of [holder.firstChild,holder.firstChild.firstChild]){"
        "for(const name of ['offsetLeft','offsetTop','offsetWidth','offsetHeight',"
        "'clientLeft','clientTop','clientWidth','clientHeight'])assert(foreign[name]===0);"
        "assert(foreign.offsetParent===null);"
        "for(const value of Object.values(foreign.getBoundingClientRect()))assert(value===0);}",
        "foreign-geometry-without-provider");
    GeometryFixture provider={.fixture=&f};
    os64_dom_set_geometry(f.binding,fake_geometry,&provider);
    dom_script(&f,GEOMETRY_ASSERT
        "var node=document.getElementById('one');var rect=node.getBoundingClientRect();"
        "assert(rect.x===1.25&&rect.y===-3.5&&rect.width===20.5&&rect.height===30.25);"
        "assert(rect.left===rect.x&&rect.top===rect.y&&rect.right===21.75&&rect.bottom===26.75);"
        "assert(node.offsetLeft===2&&node.offsetTop===3&&node.offsetWidth===21&&node.offsetHeight===30);"
        "assert(node.clientLeft===1&&node.clientTop===2&&node.clientWidth===19&&node.clientHeight===27);"
        "assert(node.offsetParent===document.body);"
        "Object.defineProperty(Object.prototype,'x',{set(){throw Error('setter ran')},configurable:true});"
        "assert(node.getBoundingClientRect().x===1.25);delete Object.prototype.x;"
        "const getter=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(node),'offsetWidth').get;"
        "throws('TypeError',()=>getter.call(document));throws('TypeError',()=>getter.call(node.firstChild));"
        "throws('TypeError',()=>node.getBoundingClientRect.call({}));", "geometry-values-and-receivers");
    os64_dom_geometry_stats_t stats=os64_dom_geometry_stats(f.binding,true);
    check(stats.layouts==11&&stats.elapsed_us==2750&&os64_dom_geometry_stats(f.binding,false).layouts==0,
          "geometry telemetry counts provider work and resets independently");
    unsigned before_foreign=provider.calls;
    dom_script(&f,GEOMETRY_ASSERT
        "assert(holder.firstChild.clientWidth===0);assert(holder.firstChild.firstChild.getBoundingClientRect().width===0);",
        "foreign-geometry-with-provider");
    check(provider.calls==before_foreign,"foreign geometry does not ask the HTML layout provider");
    provider.refuse=true;
    dom_script(&f,GEOMETRY_ASSERT "throws('InvalidStateError',()=>document.body.offsetHeight);", "geometry-refusal");
    check(os64_dom_geometry_stats(f.binding,false).layouts==1,
          "failed geometry retains attempted-layout telemetry");
    provider.refuse=false;provider.slow=true;provider.calls=0;
    os64_js_outcome_t outcome;
    const char *source="try{document.body.offsetWidth}catch(e){};try{document.body.offsetWidth}catch(e){}";
    check(os64_js_run(f.runtime,source,strlen(source),"geometry-overrun",&outcome)==OS64_JS_LIMIT&&
          outcome.limit==OS64_JS_LIMIT_EXECUTION&&provider.calls==1,
          "caught native geometry overrun remains sticky and stops later provider work");
    dom_clock=10000000;
    fixture_free(&f);
}

/* Patch 0008: what a lookup on the global object did not find, heard
 * without changing the answer the page gets (YONDER_DIAGNOSTICS.md). */
typedef struct { char names[1024]; unsigned heard; } MissFixture;
static void heard_miss(void *opaque, const char *name)
{
    MissFixture *m=opaque;
    size_t at=strlen(m->names);
    snprintf(m->names+at,sizeof(m->names)-at,"%s|",name);
    m->heard++;
}
/* One form of lookup, and the names it should be heard to miss. */
static void miss_form(DomFixture *f, MissFixture *m, const char *source, const char *heard, const char *what)
{
    *m=(MissFixture){0};
    char script[1024];
    snprintf(script,sizeof(script),GEOMETRY_ASSERT "%s",source);
    dom_script(f,script,what);
    if(strcmp(m->names,heard)) fprintf(stderr,"%s: heard '%s', wanted '%s'\n",what,m->names,heard);
    check(!strcmp(m->names,heard),what);
}
static void dom_global_miss_cases(void)
{
    DomFixture f=fixture_new(NULL);
    if(!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    JSContext *ctx=os64_js_context(f.runtime,OS64_JS_ABI_ID,&outcome);
    MissFixture m={0};
    JS_SetGlobalMissHandler(ctx,heard_miss,&m);
    miss_form(&f,&m,"assert(typeof fetch==='undefined');","fetch|","global miss: typeof a bare name");
    miss_form(&f,&m,"assert(window.fetch===undefined);","fetch|","global miss: a property of window");
    miss_form(&f,&m,"assert(window['fetch']===undefined&&globalThis.fetch===undefined);","fetch|fetch|",
              "global miss: a computed property, and globalThis");
    miss_form(&f,&m,"assert(window.fetch?.('x')===undefined);","fetch|","global miss: an optional call");
    miss_form(&f,&m,"throws('ReferenceError',()=>fetch);","fetch|",
              "global miss: a bare name in a closure, which still throws");
    miss_form(&f,&m,"var r;try{fetch}catch(e){r=e.name}assert(r==='ReferenceError');","fetch|",
              "global miss: a bare name at top level, which still throws");
    miss_form(&f,&m,"(function(){assert(typeof requestAnimationFrame==='undefined')})();",
              "requestAnimationFrame|","global miss: typeof inside a function");
    miss_form(&f,&m,"assert(window[Symbol.iterator]===undefined&&window[7]===undefined);","",
              "global miss: a symbol or an index is no name");
    miss_form(&f,&m,"assert(typeof document==='object'&&window.document===document);"
              "var mine=1;assert(window.mine===1);let scoped=2;assert(scoped===2);","",
              "global miss: what is found is not heard");
    miss_form(&f,&m,"assert(!('fetch' in window)&&!window.hasOwnProperty('fetch'));"
              "assert(Object.getOwnPropertyDescriptor(window,'fetch')===undefined);","",
              "global miss: in, hasOwnProperty and a descriptor are feature tests, not lookups");
    JS_SetGlobalMissHandler(ctx,NULL,NULL);
    miss_form(&f,&m,"assert(typeof fetch==='undefined');","","global miss: nobody asked, nothing is heard");
    dom_script(&f,GEOMETRY_ASSERT
        "function msg(f){try{f()}catch(e){return e.message}return 'none'}"
        "var o={ok:function(){return 1},n:5},plain=5,k='bogus';"
        "assert(msg(()=>o.bogus())===\"'bogus' is not a function\",msg(()=>o.bogus()));"
        "assert(msg(()=>document.bogusMethod(1))===\"'bogusMethod' is not a function\");"
        "assert(msg(()=>o.n())===\"'n' is not a function\");"
        "assert(msg(()=>o.bogus(o.ok()))==='not a function','a call in the arguments proves nothing');"
        "assert(msg(()=>o.bogus(o.n,'x',7))===\"'bogus' is not a function\",'straight-line arguments');"
        "assert(msg(()=>plain())==='not a function');"
        /* Quinn's three (#239): a skipped optional call, a failing argument,
         * an abandoned call; none may lend its name to another call. */
        "assert(msg(()=>{o.optional?.();plain()})==='not a function','skipped optional call');"
        "assert(msg(()=>o.bogus(plain()))==='not a function','the argument failed, not bogus');"
        "assert(msg(()=>{try{o.abandoned((function(){throw 1})())}catch(_){};plain()})==='not a function');"
        "assert(msg(()=>{try{o.abandoned((function(){throw 1})())}catch(_){};o[k]()})==='not a function',"
        "'a computed callee is not a fetched one');"
        "assert(msg(()=>{o.optional?.();o.second()})===\"'second' is not a function\");"
        /* Quinn's round two (#239): a skipped optional call's slot refilled
         * by another callee holding the same undefined. */
        "var base=null;assert(msg(()=>{o.optional?.();(base?.inner)()})==='not a function','optional chain');"
        "assert(msg(new Function('o',\"o.optional?.();with({inner:undefined}){inner()}\").bind(null,o))==='not a function','with');"
        "class C{get #inner(){return undefined}run(){o.optional?.();this.#inner()}}"
        "assert(msg(()=>new C().run())==='not a function','a private getter');"
        /* Quinn's round three (#239): an update's fetch, consumed, its slot
         * refilled by a straight-line super or private call. */
        "class Pa{} class Ch extends Pa{run(){o.counter++;super.missing()}}"
        "assert(msg(()=>new Ch().run())==='not a function','an increment then super');"
        "class D{#missing;run(){o.other++;this.#missing()}}"
        "assert(msg(()=>new D().run())==='not a function','an increment then a private field');"
        "assert(msg(()=>{o.k2+=1;o.named()})===\"'named' is not a function\",'an update then a real fetch');"
        "assert(o.ok()===1);",
        "not-a-function names the method of the call that failed, or nothing");
    fixture_free(&f);
}

int main(int argc, char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--reclaim")){dom_reclaim_cases();printf("DOM reclaim probe: %u checks, %u failed\n",checks,failures);return failures?1:0;}
    if(argc==2&&!strcmp(argv[1],"--clone")){dom_clone_control_cases();dom_clone_native_cases();dom_clone_state_cap_cases();dom_clone_character_data_cases();dom_clone_allocation_cases();printf("DOM clone probe: %u checks, %u failed\n",checks,failures);return failures?1:0;}
    if(argc==2&&!strcmp(argv[1],"--events")){dom_event_cases();printf("DOM events probe: %u checks, %u failed\n",checks,failures);return failures?1:0;}
    bool mutation_mode=argc==2&&!strcmp(argv[1],"--mutants");
    dom_classic_cases();
    dom_geometry_cases();
    dom_global_miss_cases();
    dom_surface_cases();
    dom_identity_cases();
    dom_collection_cases();
    dom_selector_cases();
    dom_classic_method_cases();
    dom_cookie_cases();
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
