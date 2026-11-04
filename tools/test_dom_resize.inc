/* RESIZEOBSERVER (libdom/resize.c; INTERSECTION_OBSERVER.md § 11): the
 * interface, and the standard's loop against a fake batch that reads each
 * element's sizes off its own attributes (data-w and data-h the content
 * box, data-b the border on each side, data-p the padding), so a callback
 * that changes an attribute changes what the next round measures, as a
 * real layout would. No data-w, or not in the document: no box. */

#define RO_ASSERT STORAGE_ASSERT \
    "function sz(a){return a.length+':'+a[0].inlineSize+'x'+a[0].blockSize}"

static size_t ro_calls;
static bool ro_refuse;

static double ro_attr(const os64_html_node_t *node, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(node, name);
    return a != NULL ? strtod(a->value, NULL) : 0;
}

static bool ro_provider(void *opaque, os64_dom_resize_t *items, size_t count, os64_dom_geometry_stats_t *cost)
{
    DomFixture *f = opaque;
    ro_calls++;
    cost->layouts = 1;
    if (ro_refuse) return false;
    for (size_t i = 0; i < count; i++) {
        const os64_html_node_t *n = items[i].target, *root = n;
        while (root->parent != NULL) root = root->parent;
        items[i].boxed = root == f->document->document && os64_html_attr(n, "data-w") != NULL;
        if (!items[i].boxed) continue;
        double w = ro_attr(n, "data-w"), h = ro_attr(n, "data-h"), b = ro_attr(n, "data-b"), p = ro_attr(n, "data-p");
        items[i].content[0] = w;
        items[i].content[1] = h;
        items[i].border[0] = w + 2 * (b + p);
        items[i].border[1] = h + 2 * (b + p);
        items[i].device[0] = w * 2;
        items[i].device[1] = h * 2;
        items[i].padding[0] = items[i].padding[1] = p;
    }
    return true;
}

static int ro_script(DomFixture *f, const char *body, const char *name)
{
    size_t length = strlen(RO_ASSERT) + strlen(body) + 3;
    char *source = malloc(length);
    if (source == NULL) abort();
    snprintf(source, length, "%s{%s}", RO_ASSERT, body);
    int result = dom_script(f, source, name);
    free(source);
    return result;
}

static DomFixture ro_fixture(DomFixture *slot)
{
    ro_calls = 0;
    ro_refuse = false;
    p2b_ms = 1000;
    p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    *slot = fixture_new(&options);
    if (fixture_ready(slot)) os64_dom_set_resizes(slot->binding, ro_provider, slot);
    return *slot;
}

/* The step, and its task's outcome OK. */
static bool ro_step(DomFixture *f, const char *what)
{
    os64_js_outcome_t out;
    bool ran = os64_dom_resizes_step(f->binding, &out);
    if (ran && out.status != OS64_JS_OK) fprintf(stderr, "%s: %s\n", what, out.message);
    check(!ran || out.status == OS64_JS_OK, what);
    return ran;
}

/* Every allocation of the step failed in turn, the binding's own or (with
 * `engine`) the engine's: unless the step's task failed, each target is
 * delivered or owed after the failed step and delivered once after the
 * next. A task fails when a callback does, and when the engine refuses an
 * allocation, which fails the runtime: that page runs no more scripts,
 * and only the teardown's accounting is claimed for it. */
static void ro_sweep_step(bool engine)
{
    size_t *tried = engine ? &attempts : &dom_attempts, *fail = engine ? &fail_at : &dom_fail_at;
    size_t count = 0;
    for (size_t cut = 0; cut == 0 || cut <= count; cut++) {
        DomFixture f;
        ro_fixture(&f);
        if (!fixture_ready(&f)) return;
        ro_script(&f,
            "globalThis.L=[];const two=new ResizeObserver(es=>L.push(es.length));"
            "for(const id of ['box','one','two']){let e=document.getElementById(id);e.setAttribute('data-w','3');"
            "if(id==='two')new ResizeObserver(es=>L.push(es.length)).observe(e);else two.observe(e)}",
            "resize sweep: setup");
        size_t start = *tried;
        if (cut != 0) *fail = start + cut;
        os64_js_outcome_t out;
        (void)os64_dom_resizes_step(f.binding, &out);
        *fail = 0;
        if (cut == 0) {
            count = *tried - start;
            printf("resize step %sallocation points: %zu\n", engine ? "engine " : "", count);
        } else {
            dom_sweeps++;
        }
        if (out.status != OS64_JS_OK) {
            fixture_free(&f);
            continue;
        }
        char script[192];
        snprintf(script, sizeof(script), "const n=L.reduce((a,b)=>a+b,0);assert(n===3||%d,'first step delivered '+n)",
                 os64_dom_resizes_owed(f.binding));
        ro_script(&f, script, "resize sweep: what a failed step did not deliver is owed");
        (void)os64_dom_resizes_step(f.binding, &out);
        ro_script(&f, "const n=L.reduce((a,b)=>a+b,0);assert(n===3,'two steps delivered '+n)",
                  "resize sweep: and the next step delivers it, once");
        fixture_free(&f);
    }
}

static void dom_resize_cases(void)
{
    DomFixture f;
    ro_fixture(&f);
    if (!fixture_ready(&f)) return;
    ro_script(&f,
        "assert(ResizeObserver.length===1);throws('TypeError',()=>new ResizeObserver());"
        "throws('TypeError',()=>new ResizeObserver(1));throws('TypeError',()=>ResizeObserver(()=>{}));"
        "throws('TypeError',()=>new ResizeObserverEntry());throws('TypeError',()=>new ResizeObserverSize());"
        "let o=new ResizeObserver(()=>{});let box=document.getElementById('box');"
        "throws('TypeError',()=>o.observe());throws('TypeError',()=>o.observe(document.createTextNode('t')));"
        "throws('TypeError',()=>o.observe(box,{box:'padding-box'}));throws('TypeError',()=>o.observe(box,1));"
        "throws('TypeError',()=>o.unobserve('x'));"
        "throws('TypeError',()=>ResizeObserver.prototype.observe.call({},box));"
        "o.observe(box,{box:'border-box'});o.unobserve(box);o.disconnect();",
        "resize: the interface");
    check(os64_dom_resizes_observed(f.binding) == 0 && !ro_step(&f, "resize: nothing observed"),
          "resize: no targets, no step");

    /* The first report, the entry's members, and no report while nothing
     * moves. */
    ro_script(&f,
        "globalThis.L=[];globalThis.box=document.getElementById('box');"
        "box.setAttribute('data-w','100');box.setAttribute('data-h','50');box.setAttribute('data-b','1');"
        "box.setAttribute('data-p','4');"
        "globalThis.ro=new ResizeObserver(function(es,obs){assert(this===ro&&obs===ro);L.push(...es)});ro.observe(box);",
        "resize: observe");
    check(os64_dom_resizes_owed(f.binding), "resize: a new target is owed its first report");
    check(ro_step(&f, "resize: the first step"), "resize: the first report");
    ro_script(&f,
        "assert(L.length===1,String(L.length));let e=L[0];"
        "assert(e instanceof ResizeObserverEntry&&e.target===box);"
        "assert(e.contentRect instanceof DOMRectReadOnly&&e.contentRect.x===4&&e.contentRect.y===4&&"
        "e.contentRect.width===100&&e.contentRect.height===50,'the content rect at the padding');"
        "assert(sz(e.contentBoxSize)==='1:100x50'&&sz(e.borderBoxSize)==='1:110x60'&&"
        "sz(e.devicePixelContentBoxSize)==='1:200x100');"
        "assert(Object.isFrozen(e.borderBoxSize)&&e.borderBoxSize===e.borderBoxSize);"
        "assert(e.borderBoxSize[0] instanceof ResizeObserverSize);",
        "resize: the entry");
    check(!os64_dom_resizes_owed(f.binding) && !ro_step(&f, "resize: at rest"), "resize: nothing moved, no task");

    /* A content-box observation hears the content box; a border-box one
     * only a border box that moved. */
    ro_script(&f,
        "L=[];globalThis.bb=new ResizeObserver(es=>L.push('border:'+sz(es[0].borderBoxSize)));"
        "bb.observe(box,{box:'border-box'});", "resize: a border-box observer");
    check(ro_step(&f, "resize: border first"), "resize: the border-box observer's first");
    ro_script(&f, "L=[];box.setAttribute('data-p','2');box.setAttribute('data-b','3');",
              "resize: padding traded for border");
    check(!ro_step(&f, "resize: the same border box"), "resize: neither box moved");
    ro_script(&f, "box.setAttribute('data-w','90');", "resize: narrower");
    check(ro_step(&f, "resize: narrower"), "resize: both observers report");
    ro_script(&f,
        "assert(L.length===2&&L[0] instanceof ResizeObserverEntry&&sz(L[0].contentBoxSize)==='1:90x50'&&"
        "L[1]==='border:1:100x60',L.join());L=[];bb.disconnect();",
        "resize: both, in the order made");

    /* No box is 0 by 0, and reports; a 0 by 0 target reports its first. */
    ro_script(&f, "L=[];box.removeAttribute('data-w');", "resize: no box");
    check(ro_step(&f, "resize: the box goes"), "resize: losing the box is a resize");
    ro_script(&f,
        "assert(L.length===1&&sz(L[0].contentBoxSize)==='1:0x0'&&L[0].contentRect.x===0);L=[];"
        "let z=document.getElementById('one');z.setAttribute('data-w','0');z.setAttribute('data-h','0');"
        "globalThis.zo=new ResizeObserver(es=>L.push(es.length));zo.observe(z);",
        "resize: a zero-size target");
    check(ro_step(&f, "resize: zero first"), "resize: a 0 by 0 target reports once");
    ro_script(&f, "assert(L.join()==='1');zo.disconnect();", "resize: its report");

    /* The depth loop: a shallow target's callback grows a deeper one, which
     * is reported in the same step; a callback that grows its own target,
     * at its own depth, is the loop error, and reported next step. */
    ro_script(&f,
        "L=[];ro.disconnect();globalThis.errors=[];"
        "window.onerror=m=>{document.body.style.color='red';errors.push(m);return true};"
        "let deep=document.getElementById('two');deep.setAttribute('data-w','1');deep.setAttribute('data-h','1');"
        "box.setAttribute('data-w','10');"
        "globalThis.outer=new ResizeObserver(es=>{L.push('outer');deep.setAttribute('data-w',String(+deep.getAttribute('data-w')+1))});"
        "globalThis.inner=new ResizeObserver(es=>L.push('inner:'+es[0].contentRect.width));"
        "outer.observe(box);inner.observe(deep);",
        "resize: the depth loop");
    check(ro_step(&f, "resize: two rounds"), "resize: one step");
    ro_script(&f,
        "assert(L.join()==='outer,inner:1,inner:2','both first, then the deeper change in the same step: '+L);L=[];",
        "resize: rounds");
    ro_script(&f,
        "globalThis.self=new ResizeObserver(es=>{L.push('self');let t=es[0].target;"
        "t.setAttribute('data-w',String(+t.getAttribute('data-w')+1))});"
        "outer.disconnect();inner.disconnect();self.observe(box);",
        "resize: a callback that resizes its own target");
    check(ro_step(&f, "resize: the loop error"), "resize: the loop error's step");
    ro_script(&f,
        "assert(L.join()==='self',L.join());assert(errors.length===1&&"
        "errors[0]==='ResizeObserver loop completed with undelivered notifications.',String(errors));"
        "assert(document.body.style.color==='red','its listener ran inside a budget');L=[];",
        "resize: the loop error");
    check(os64_dom_resizes_owed(f.binding), "resize: the skipped one is owed the next step, laid out or not");
    check(ro_step(&f, "resize: next step"), "resize: the skipped one reported next step");
    ro_script(&f, "assert(L.join()==='self');self.disconnect();", "resize: next step's report");

    /* Observing again starts over; a refused batch runs nothing. */
    ro_script(&f, "L=[];globalThis.again=new ResizeObserver(es=>L.push(es.length));again.observe(box);",
              "resize: again");
    check(ro_step(&f, "resize: again first"), "resize: again first");
    ro_script(&f, "again.observe(box);", "resize: observed again");
    ro_refuse = true;
    check(!ro_step(&f, "resize: refused") && os64_dom_resizes_owed(f.binding),
          "resize: a refused batch runs nothing and stays owed");
    ro_refuse = false;
    check(ro_step(&f, "resize: answered"), "resize: observing again reports again");
    ro_script(&f, "assert(L.join()==='1,1');again.disconnect();", "resize: twice");
    /* A refused step owes what it did not measure, a target it had already
     * reported included: the host may record the layout as measured. */
    ro_script(&f, "L=[];globalThis.kept=new ResizeObserver(es=>L.push(es[0].contentRect.width));kept.observe(box);",
              "resize: kept");
    check(ro_step(&f, "resize: kept first"), "resize: kept first");
    ro_script(&f, "box.setAttribute('data-w','42');", "resize: kept grows");
    ro_refuse = true;
    check(!ro_step(&f, "resize: kept refused") && os64_dom_resizes_owed(f.binding),
          "resize: a refused step owes a size it had reported before");
    ro_refuse = false;
    check(ro_step(&f, "resize: kept answered"), "resize: kept answered");
    ro_script(&f, "assert(L.length===2&&L[1]===42,L.join());kept.disconnect();", "resize: kept reported");
    check(!os64_dom_resizes_owed(f.binding), "resize: and nothing is owed after");
    fixture_free(&f);

    /* An observer nothing refers to is kept while it observes, and let go
     * when it does not; observers made and dropped are swept. */
    ro_fixture(&f);
    if (!fixture_ready(&f)) return;
    ro_script(&f, "globalThis.L=[];document.getElementById('box');document.getElementById('one');",
              "resize: wrappers");
    size_t base = os64_dom_registry_count(f.binding);
    ro_script(&f,
        "let b=document.getElementById('box');b.setAttribute('data-w','5');"
        "(function(){new ResizeObserver(es=>L.push(es[0].target.id)).observe(b)})();",
        "resize: unreferenced");
    check(os64_dom_registry_count(f.binding) == base + 1, "resize: observing, it is held");
    os64_js_outcome_t gc;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    check(ro_step(&f, "resize: unreferenced reports"), "resize: it reports");
    ro_script(&f, "assert(L.join()==='box');for(let i=0;i<100;i++)new ResizeObserver(()=>{});",
              "resize: a hundred dropped");
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    size_t before = os64_dom_bytes(f.binding);
    ro_script(&f, "new ResizeObserver(()=>{});", "resize: one more");
    check(os64_dom_bytes(f.binding) < before, "resize: the next observer sweeps the dropped");
    fixture_free(&f);

    ro_sweep_step(false);
    ro_sweep_step(true);
    static const char *const kOperations[] = {
        "let o=new ResizeObserver(()=>{});o.observe(document.getElementById('one'),{box:'border-box'});"
        "o.observe(document.getElementById('two'));o.unobserve(document.getElementById('one'));o.disconnect()",
    };
    os64_dom_options_t options = p2b_options(P2B_URL);
    binding_sweep("resize", kOperations, 1, &options);

    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "if(typeof ResizeObserver!=='undefined')throw Error('modern');", "resize: none with the switch off");
    fixture_free(&f);
}
#undef RO_ASSERT
