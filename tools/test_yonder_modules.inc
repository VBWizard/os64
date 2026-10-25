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

/* With the Modern Web surface off, a module is what the page asked for and
 * did not get, and nothing of it runs. */
static void module_modern_off_case(void) {
    fake_fetch_reset();
    loop_modern_off=true;
    module_page("<script type=module>note('module')</script>");
    check(loop_settle() && g.page.tree!=NULL && script_rebuild() && element_text("out")[0]=='\0',
          "modules: with modern off a module does not run");
    check(strstr(request_record_text(),"MISSING script module (1)")!=NULL,"modules: and is on the record as missing");
    loop_modern_off=false;
    request_drop();
}

static void module_cases(void) {
    module_graph_case();
    module_order_case();
    module_dynamic_case();
    module_classic_import_case();
    module_failure_case();
    module_modern_off_case();
}
