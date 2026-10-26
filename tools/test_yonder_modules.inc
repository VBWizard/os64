/* ES MODULES, END TO END (MODERN_WEB.md § Slice 3; modules.h): a page's
 * module scripts through yonder's window, their graphs fetched as a
 * script's requests are (the real fetch job against the fake libfetch,
 * landed by request_land), compiled as they land, and run in HTML's order.
 * Classic scripts' sources land by script_land, as they always have. */

#define MODULE_JS "Content-Type: text/javascript\r\n"

/* The text an element holds, "" for none. */
static const char *element_text(const char *id) {
    os64_html_node_t *n=probe_id(id);
    return n!=NULL && n->first_child!=NULL && n->first_child->kind==OS64_HTML_TEXT ? n->first_child->text : "";
}

/* A page whose scripts write what ran, in order, into #out. */
static void module_page(const char *rest) {
    char page[4096];
    snprintf(page,sizeof(page),"<p id=out></p><script>var order=[];function note(n){order.push(n);"
        "document.getElementById('out').textContent=order.join('|')}</script>%s",rest);
    loop_page(page);
}

/* A graph of three, fetched beside the parse: each module once, its
 * imports first, the page waiting for it as for a deferred script. */
static void module_graph_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/a.js",200,MODULE_JS,"import {b} from './b.js';import './c.js';note('a'+b);");
    fake_answer("GET","http://fixture.test/m/b.js",200,MODULE_JS,"import './c.js';export const b='B';note('b');");
    fake_answer("GET","http://fixture.test/m/c.js",200,MODULE_JS,"note('c');");
    module_page("<script type=module src=/m/a.js></script>");
    check(loop_settle() && nrequest_jobs==1 && g.page.tree==NULL,
          "modules: the entry is fetched, and the page waits for it as for a deferred script");
    check(request_land("/m/a.js") && nrequest_jobs==2,"modules: its imports are fetched as it lands");
    check(request_land("/m/b.js") && request_land("/m/c.js") && nrequest_jobs==0,
          "modules: c, imported twice, is fetched once");
    check(loop_settle() && g.page.tree!=NULL && script_rebuild() && probe_text_is("out","c|b|aB"),
          "modules: the graph runs once it is all in, each module once, its imports first");
    check(strstr(request_record_text(),"MISSING script module")==NULL,"modules: a module that ran is not missing");
    request_drop();
}

/* HTML's order: a parser-inserted module is deferred, among the defer
 * scripts in document order; an async one runs when its graph is ready; a
 * nomodule classic script does not run where modules do. */
static void module_order_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/async.js",200,MODULE_JS,"note('async module')");
    module_page("<script>document.addEventListener('DOMContentLoaded',()=>note('DOMContentLoaded'))</script>"
        "<script type=module>note('inline module')</script><script defer src=/d.js></script>"
        "<script type=module async src=/m/async.js></script><script nomodule>note('nomodule')</script>"
        "<script>note('inline classic')</script>");
    /* The page arrives only once its deferred scripts have run, so what
     * ran is read at the end; the order it was written in is the proof. */
    check(loop_settle() && g.page.tree==NULL,"modules: after the parse the page waits for its defer script");
    check(request_land("/m/async.js") && loop_settle() && g.page.tree==NULL,
          "modules: the async module lands, and the page still waits");
    check(script_land("/d.js","note('defer')") && loop_settle() && g.page.tree!=NULL && script_rebuild() &&
          probe_text_is("out","inline classic|inline module|async module|defer|DOMContentLoaded"),
          "modules: the inline module first of the deferred, the async one as it landed, the defer script, "
          "then DOMContentLoaded; the nomodule script never");
    request_drop();
}

/* import.meta.url is the page's for an inline module; a dynamic import()
 * is fetched on the window thread inside its task; a module a script
 * connects runs when its graph is ready, compiled between tasks. */
static void module_dynamic_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/dyn.js",200,MODULE_JS,"export default 'dyn';");
    module_page("<script type=module>note(import.meta.url);import('/m/dyn.js').then(m=>note(m.default))</script>"
        "<script>var s=document.createElement('script');s.type='module';"
        "s.textContent=\"note('connected ' + (this === undefined ? 'module' : 'classic'))\";"
        "document.body.appendChild(s)</script>");
    check(loop_settle() && g.page.tree!=NULL && script_rebuild(),"modules: the page with a dynamic import arrives");
    const char *out=element_text("out");
    /* A module's `this` is undefined, a classic script's the window: the
     * connected script ran as the module its `type` property made it. */
    check(strstr(out,"http://fixture.test/final")!=NULL && strstr(out,"dyn")!=NULL &&
          strstr(out,"connected module")!=NULL,
          "modules: import.meta.url, the dynamic import's default, and the connected module, run as one");
    check(strstr(request_record_text(),"request GET http://fixture.test/m/dyn.js: 200")!=NULL &&
          strstr(request_record_text(),"synchronous")!=NULL,
          "modules: the record says the dynamic import stopped the page to fetch");
    request_drop();
}

/* A classic script's dynamic import(), as a bundler's lazy chunk is loaded:
 * it resolves against the script's own address, not the page's. */
static void module_classic_import_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/s/chunk.js",200,MODULE_JS,"export default 'chunk';");
    module_page("<script src=/s/loader.js></script>");
    check(loop_settle() && script_land("/s/loader.js","import('./chunk.js').then(m=>note(m.default))") &&
          loop_settle() && g.page.tree!=NULL && script_rebuild() && probe_text_is("out","chunk"),
          "modules: a classic script's import() resolves against the script's address");
    request_drop();
}

/* Each way a module fails: its line is on the record; one that could not
 * be fetched or is not JavaScript runs nothing, and a syntax error, an
 * import naming no module and a throw each fire the window's error (HTML's
 * error to rethrow). */
static void module_failure_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/a.js",200,MODULE_JS,"import './missing.js';note('never')");
    fake_answer("GET","http://fixture.test/m/missing.js",404,"","");
    fake_answer("GET","http://fixture.test/m/plain.js",200,"Content-Type: text/plain\r\n","note('plain')");
    fake_answer("GET","http://fixture.test/m/bad.js",200,MODULE_JS,"note(;");
    module_page("<script>addEventListener('error',e=>note(e.message))</script>"
        "<script type=module src=/m/a.js></script><script type=module src=/m/plain.js></script>"
        "<script type=module src=/m/bad.js></script><script type=module>import 'lodash';note('bare')</script>"
        "<script type=module>throw Error('boom')</script>");
    check(loop_settle() && request_land("/m/a.js") && request_land("/m/plain.js") && request_land("/m/bad.js") &&
          request_land("/m/missing.js") && loop_settle() && g.page.tree!=NULL && script_rebuild(),
          "modules: a page of failing modules arrives");
    /* In the defer order: the two that could not be fetched or were not
     * JavaScript run nothing; the syntax error, the bare import and the
     * throw each reach the window as a script's error. */
    check(probe_text_is("out","Uncaught SyntaxError: unexpected token in expression: ';'|"
                              "Uncaught TypeError: cannot resolve module specifier 'lodash'|Uncaught Error: boom"),
          "modules: none ran; a syntax error, a bare import and a throw each fired the window's error");
    const char *record=request_record_text();
    check(strstr(record,"FAILED script http://fixture.test/m/missing.js: the module's server did not answer it")!=NULL &&
          strstr(record,"FAILED script http://fixture.test/m/plain.js: the module is not typed as JavaScript")!=NULL &&
          strstr(record,"FAILED script http://fixture.test/m/bad.js: SyntaxError")!=NULL &&
          strstr(record,"names no module")!=NULL,
          "modules: each failure is on the record by its module's address");
    request_drop();
}

/* How often `needle` is in `text`. */
static int count_in(const char *text, const char *needle) {
    int n=0;
    for(const char *at=strstr(text,needle);at!=NULL;at=strstr(at+1,needle)) n++;
    return n;
}

/* A Vite page: its entry first, then the preloads of what it imports. A
 * preload begins its module's fetch before the entry lands, and the entry's
 * import finds it fetched, once; a preload nothing imports is fetched and
 * never run, and one whose `as` is a style's is not a module's. The entry's
 * first question is Vite's: whether the browser preloads modules itself. */
static void module_preload_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/main.js",200,MODULE_JS,
        "import {v} from './vendor.js';const r=document.createElement('link').relList;"
        "note(r.supports('modulepreload')?'native':'polyfill');note(v);");
    fake_answer("GET","http://fixture.test/m/vendor.js",200,MODULE_JS,"export const v='V';note('vendor');");
    fake_answer("GET","http://fixture.test/m/extra.js",200,MODULE_JS,"note('extra');");
    module_page("<script type=module src=/m/main.js></script><link rel=modulepreload href=/m/vendor.js>"
        "<link rel='MODULEPRELOAD other' href=/m/extra.js><link rel=modulepreload as=style href=/m/look.js>");
    check(loop_settle() && nrequest_jobs==3,"modules: the entry and two preloads are fetched beside the parse");
    check(request_land("/m/main.js") && nrequest_jobs==2,"modules: the entry's import is the preload already out");
    check(request_land("/m/vendor.js") && request_land("/m/extra.js") && loop_settle() && g.page.tree!=NULL &&
          script_rebuild() && probe_text_is("out","vendor|native|V"),
          "modules: the graph runs with the preloaded module, and the unimported preload runs nothing");
    const char *record=request_record_text();
    check(count_in(record,"request GET http://fixture.test/m/vendor.js")==1 && strstr(record,"/m/look.js")==NULL,
          "modules: the preloaded module was fetched once, and the style one never");
    request_drop();
}

/* A classic script's import() of a module a deferred graph is still
 * fetching takes the fetch over, inline, and its answer never comes: the
 * graph is walked again between tasks, so the deferred module runs and the
 * page arrives, rather than waiting for an answer nobody will give. */
static void module_takeover_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/main.js",200,MODULE_JS,"import {u} from './util.js';note('main '+u);");
    fake_answer("GET","http://fixture.test/m/util.js",200,MODULE_JS,"export const u='U';note('util');");
    module_page("<script type=module src=/m/main.js></script><script src=/s/late.js></script>");
    check(loop_settle() && request_land("/m/main.js") && nrequest_jobs==1,
          "modules: the deferred entry lands, its import is out, the classic script waits");
    check(script_land("/s/late.js","import('/m/util.js').then(m=>note('dyn '+m.u))") && loop_settle() &&
          g.page.tree!=NULL && script_rebuild() && probe_text_is("out","util|dyn U|main U"),
          "modules: the import() took the fetch over, and the deferred graph still ran");
    request_drop();
}

/* A module answered 204 (no body) is an empty module: its importer runs. */
static void module_no_content_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/main.js",200,MODULE_JS,"import './empty.js';note('after empty');");
    fake_answer("GET","http://fixture.test/m/empty.js",204,MODULE_JS,"");
    module_page("<script type=module src=/m/main.js></script>");
    check(loop_settle() && request_land("/m/main.js") && request_land("/m/empty.js") && loop_settle() &&
          g.page.tree!=NULL && script_rebuild() && probe_text_is("out","after empty"),
          "modules: a 204 module is an empty one, and its importer runs");
    request_drop();
}

/* With the Modern Web surface off, a module is what the page asked for and
 * did not get, and nothing of it runs; an import map is too. */
static void module_modern_off_case(void) {
    fake_fetch_reset();
    loop_modern_off=true;
    module_page("<script type=importmap>{\"imports\":{\"a\":\"/a.js\"}}</script><script type=module>note('module')</script>");
    check(loop_settle() && g.page.tree!=NULL && script_rebuild() && element_text("out")[0]=='\0',
          "modules: with modern off a module does not run");
    check(strstr(request_record_text(),"MISSING script module (1)")!=NULL &&
          strstr(request_record_text(),"MISSING script importmap (1)")!=NULL,
          "modules: and it and the import map are on the record as missing");
    loop_modern_off=false;
    request_drop();
}

/* IMPORT MAPS (MODERN_WEB.md § Import maps, as built): a parser-inserted map
 * registers before the parse goes on, so the modules after it import bare
 * names through it: an exact rule, a prefix rule, and a scope over the
 * modules beneath a path, each fetched once at the address it maps to. */
static void import_map_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/lib/react.js",200,MODULE_JS,"export default 'react 19';");
    fake_answer("GET","http://fixture.test/lib/react-17.js",200,MODULE_JS,"export default 'react 17';");
    fake_answer("GET","http://fixture.test/lib/util/fmt.js",200,MODULE_JS,"export const fmt=s=>'<'+s+'>';");
    fake_answer("GET","http://fixture.test/m/legacy/old.js",200,MODULE_JS,"import r from 'react';note('old '+r);");
    module_page("<script type=importmap>{\"imports\":{\"react\":\"/lib/react.js\",\"util/\":\"/lib/util/\"},"
        "\"scopes\":{\"/m/legacy/\":{\"react\":\"/lib/react-17.js\"}}}</script>"
        "<script type=module>import r from 'react';import {fmt} from 'util/fmt.js';note(fmt(r));"
        "note(HTMLScriptElement.supports('importmap'))</script>"
        "<script type=module src=/m/legacy/old.js></script>");
    check(loop_settle() && request_land("/lib/react.js") && request_land("/lib/util/fmt.js") &&
          request_land("/m/legacy/old.js") && request_land("/lib/react-17.js") && loop_settle() &&
          g.page.tree!=NULL && script_rebuild() && probe_text_is("out","<react 19>|true|old react 17"),
          "importmap: a bare name, a prefix and a scope each import what the map says");
    const char *record=request_record_text();
    check(count_in(record,"request GET http://fixture.test/lib/react.js")==1 && strstr(record,"names no module")==NULL &&
          strstr(record,"bare name")==NULL && strstr(record,"MISSING script importmap")==NULL,
          "importmap: each module was fetched once, and nothing is missing or failed");
    request_drop();
}

/* A map that does not parse fires the window's error and registers
 * nothing, so a bare import after it throws; one with `src` is never
 * fetched; a bare name no map maps is on the record as such. */
static void import_map_failure_case(void) {
    fake_fetch_reset();
    module_page("<script>addEventListener('error',e=>note(e.error&&e.error.name))</script>"
        "<script type=importmap>{imports:{}}</script><script type=importmap src=/map.json></script>"
        "<script type=importmap>[]</script><script type=importmap>{\"imports\":{\"a\":\"/a.js\"}}</script>"
        "<script type=module>import 'lodash';note('never')</script>");
    check(loop_settle() && g.page.tree!=NULL && script_rebuild() &&
          probe_text_is("out","SyntaxError|TypeError|TypeError"),
          "importmap: a syntax error and a top-level array each fire the window's error, and the bare import throws");
    const char *record=request_record_text();
    check(strstr(record,"an import map is never fetched")!=NULL && strstr(record,"/map.json")==NULL &&
          strstr(record,"is a bare name no import map maps")!=NULL,
          "importmap: src is not fetched, and the bare name is on the record as one no map maps");
    request_drop();
}

/* Two maps merge, the first's rule winning where both name a key; a map a
 * script connects registers before the next task; and a rule for a name
 * already resolved is dropped, so a name means one thing for the page's
 * life. */
static void import_map_merge_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/m/a1.js",200,MODULE_JS,"export default 'a1';");
    fake_answer("GET","http://fixture.test/m/b.js",200,MODULE_JS,"export default 'b';");
    fake_answer("GET","http://fixture.test/m/c.js",200,MODULE_JS,"export default 'c';");
    module_page("<script type=importmap>{\"imports\":{\"a\":\"/m/a1.js\"}}</script>"
        "<script type=importmap>{\"imports\":{\"a\":\"/m/a2.js\",\"b\":\"/m/b.js\"}}</script>"
        "<script>var s=document.createElement('script');s.type='importmap';"
        "s.textContent='{\"imports\":{\"c\":\"/m/c.js\",\"b\":\"/m/b2.js\"}}';document.head.appendChild(s)</script>"
        "<script type=module>import a from 'a';import b from 'b';import c from 'c';note(a+b+c)</script>");
    check(loop_settle() && request_land("/m/a1.js") && request_land("/m/b.js") && request_land("/m/c.js") &&
          loop_settle() && g.page.tree!=NULL && script_rebuild() && probe_text_is("out","a1bc"),
          "importmap: the first map's a and b stand, the connected map's c joins them");
    request_drop();
}

/* An import map's addresses resolve against the document's base URL when
 * it was prepared, as an inline module's imports do; and that base URL is
 * the inline module's import.meta.url, whatever <base> becomes after. */
static void import_map_base_case(void) {
    fake_fetch_reset();
    fake_answer("GET","http://fixture.test/base/lib.js",200,MODULE_JS,"export default 'based';");
    module_page("<base href=/base/><script type=importmap>{\"imports\":{\"lib\":\"./lib.js\"}}</script>"
        "<script type=module>import l from 'lib';note(l+' '+import.meta.url)</script>"
        "<script>document.querySelector('base').href='/elsewhere/'</script>");
    check(loop_settle() && request_land("/base/lib.js") && loop_settle() && g.page.tree!=NULL && script_rebuild() &&
          probe_text_is("out","based http://fixture.test/base/"),
          "importmap: its addresses resolve against the page's <base>, which is the inline module's meta URL");
    request_drop();
}

static void module_cases(void) {
    module_graph_case();
    module_order_case();
    module_dynamic_case();
    module_classic_import_case();
    module_failure_case();
    module_preload_case();
    module_takeover_case();
    module_no_content_case();
    module_modern_off_case();
    import_map_case();
    import_map_failure_case();
    import_map_merge_case();
    import_map_base_case();
}
