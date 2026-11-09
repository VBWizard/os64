#define RECLAIM_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;assert(e.name===n)}assert(caught);} "
/* MEMORY_CENSUS.md § 4: a wrapper is counted under its kind, one block a
 * node, each the same size, and blocks made only ever rise. */
static void dom_memory_kind_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    os64_dom_mem_t before = os64_dom_memory(f.binding, OS64_DOM_MEM_WRAPPER);
    dom_script(&f, RECLAIM_ASSERT
        "globalThis.kept=[];for(var i=0;i<1000;i++)kept.push(document.createElement('i'));",
        "memory-wrappers.js");
    os64_dom_mem_t after = os64_dom_memory(f.binding, OS64_DOM_MEM_WRAPPER);
    size_t made = after.blocks - before.blocks;
    check(made == 1000 && after.made - before.made == 1000,
          "memory census: a thousand nodes wrapped are a thousand wrapper blocks");
    check(made != 0 && (after.bytes - before.bytes) % made == 0 && (after.bytes - before.bytes) / made > 0,
          "memory census: every wrapper block costs the same");
    os64_dom_mem_t listeners = os64_dom_memory(f.binding, OS64_DOM_MEM_LISTENER);
    dom_script(&f, RECLAIM_ASSERT "for(var i=0;i<10;i++)kept[i].addEventListener('click',function(){});",
        "memory-listeners.js");
    check(os64_dom_memory(f.binding, OS64_DOM_MEM_LISTENER).blocks > listeners.blocks,
          "memory census: listeners are counted as listeners");
    check(!strcmp(os64_dom_mem_kind_name(OS64_DOM_MEM_WRAPPER), "wrapper") &&
          !strcmp(os64_dom_mem_kind_name(OS64_DOM_MEM_KINDS), "?"),
          "memory census: kinds have names, and past the enum is '?'");
    fixture_free(&f);
}

/* Each operation measured, then run again with each binding allocation it
 * makes refused in turn (dom_fail_at), and then once more, nothing refused,
 * in the same binding: the refused run throws QuotaExceededError or nothing,
 * and the run after succeeds, so a refusal left nothing half made that the
 * operation trips over or answers from (a [SameObject] getter's first
 * object, a stale cache). The teardown accounts for every byte. */
static void extra_sweep(const char *const *operations, size_t n, const os64_dom_options_t *options)
{
    for (size_t op = 0; op < n; op++) {
        DomFixture f = fixture_new(options);
        if (!fixture_ready(&f)) return;
        size_t start = dom_attempts;
        dom_script(&f, operations[op], "extra sweep: an operation measured");
        size_t count = dom_attempts - start;
        fixture_free(&f);
        check(count > 0, "extra sweep: the operation allocates");
        for (size_t cut = 1; cut <= count; cut++) {
            f = fixture_new(options);
            if (!fixture_ready(&f)) return;
            char source[1024];
            snprintf(source, sizeof(source),
                     "try{%s}catch(e){if(e.name!=='QuotaExceededError')throw e}", operations[op]);
            size_t target = dom_attempts + cut;
            dom_fail_at = target;
            dom_script(&f, source, "extra sweep: an allocation refused");
            dom_fail_at = 0;
            dom_sweeps++;
            check(dom_attempts >= target, "extra sweep: the cut was reached");
            char name[160];
            snprintf(name, sizeof(name), "extra sweep: operation %zu runs whole after cut %zu", op, cut);
            dom_script(&f, operations[op], name);
            fixture_free(&f);
        }
    }
}

/* A wrapper is its small record alone until something it rarely carries is
 * set (internal.h's DValueMore): a plain node costs no extra record, a view
 * asked for makes one, and asking again, or for another view of the same
 * element, makes none. */
static void dom_wrapper_extra_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    os64_dom_mem_t wrappers = os64_dom_memory(f.binding, OS64_DOM_MEM_WRAPPER);
    os64_dom_mem_t extra = os64_dom_memory(f.binding, OS64_DOM_MEM_EXTRA);
    dom_script(&f, RECLAIM_ASSERT
        "globalThis.held=[];for(var i=0;i<100;i++){var d=document.createElement('div');"
        "d.appendChild(document.createTextNode('x'));d.childNodes.length;held.push(d);}",
        "extra-plain.js");
    os64_dom_mem_t wrapped = os64_dom_memory(f.binding, OS64_DOM_MEM_WRAPPER);
    check(os64_dom_memory(f.binding, OS64_DOM_MEM_EXTRA).blocks == extra.blocks,
          "wrapper split: plain nodes and their childNodes make no extra record");
    check(wrapped.blocks > wrappers.blocks &&
          (wrapped.bytes - wrappers.bytes) / (wrapped.blocks - wrappers.blocks) <= 128,
          "wrapper split: a wrapper's record is 128 bytes at most");
    dom_script(&f, RECLAIM_ASSERT
        "for(var i=0;i<10;i++){var a=held[i].classList,b=held[i].style,c=held[i].dataset;"
        "assert(a===held[i].classList&&b===held[i].style&&c===held[i].dataset,'[SameObject] views');}",
        "extra-views.js");
    check(os64_dom_memory(f.binding, OS64_DOM_MEM_EXTRA).blocks == extra.blocks + 10,
          "wrapper split: an element's views share one extra record");
    fixture_free(&f);
    /* Each place that makes the extra record, as the operation's first
     * maker of it, through extra_sweep. A [SameObject] getter is asked twice,
     * so the run after a refusal proves it answers one object. */
    static const char *const kAsks[] = {
#define SAME(make, ask) make "if(" ask "!==" ask ")throw Error('same')"
        SAME("var e=document.createElement('div');", "e.classList"),
        SAME("var e=document.createElement('div');", "e.style"),
        SAME("var e=document.createElement('div');", "e.dataset"),
        SAME("var e=document.createElement('div');", "e.attributes"),
        SAME("var e=document.createElement('div');", "e.children"),
        SAME("var a=document.createElement('a');", "a.relList"),
        SAME("var o=document.createElement('output');", "o.htmlFor"),
        "new CSSStyleSheet().replaceSync('i{}')",
        SAME("", "document.adoptedStyleSheets"),
        "var c=document.createElement('canvas'),x=c.getContext('2d');if(x&&x!==c.getContext('2d'))throw Error('same')",
        "var x=document.createElement('canvas').getContext('2d');if(x)x.fillStyle=x.createLinearGradient(0,0,1,1)",
        "new Path2D('M0 0L1 1')",
        SAME("var s=document.createElement('select');", "s.options"),
        SAME("var s=document.createElement('select');", "s.selectedOptions"),
        SAME("var t=document.createElement('table');", "t.rows"),
        SAME("var t=document.createElement('table');", "t.tBodies"),
        SAME("var o=document.createElement('input');document.body.appendChild(o);", "o.labels"),
        SAME("var o=document.createElement('input');", "o.validity"),
        "var o=document.createElement('output');o.value='y';if(o.defaultValue!==''||o.value!=='y')throw Error('kept')",
        SAME("var fs=document.createElement('fieldset');", "fs.elements"),
        SAME("", "document.images"),
        SAME("", "document.forms"),
        SAME("", "document.styleSheets"),
        SAME("", "document.implementation"),
        SAME("var f=document.createElement('form');for(var i=0;i<2;i++){var r=document.createElement('input');"
             "r.type='radio';r.name='r';f.appendChild(r)}", "f.r"),
        SAME("var l=document.createElement('link');l.rel='stylesheet';document.head.appendChild(l);", "l.sheet"),
#undef SAME
    };
    os64_dom_options_t options = os64_dom_default_options();
    options.shadow_trees = true;        /* constructed and adopted sheets */
    extra_sweep(kAsks, sizeof(kAsks) / sizeof(kAsks[0]), &options);
}

// These fixtures deliberately have no model pin: retention comes from the
// binding's wrappers and collection answers, rather than an older snapshot.
static void dom_reclaim_cases(void)
{
    dom_memory_kind_cases();
    dom_wrapper_extra_cases();
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RECLAIM_ASSERT
        "var box=document.getElementById('box'),kept=document.getElementById('one');"
        "kept.expando='retained';var answer=box.childNodes;assert(answer.length===2);",
        "reclaim-holders.js");
    os64_html_node_t *one = native_id(f.document->document,"one");
    os64_html_node_t *two = native_id(f.document->document,"two");
    size_t nodes = f.document->node_count;
    check(os64_html_remove(f.document,two)==OS64_HTML_OK && f.document->node_count==nodes,
          "cached collection answer keeps an unwrapped detached subtree alive");
    dom_fail_at=dom_attempts+1;
    dom_script(&f, RECLAIM_ASSERT
        "throws('QuotaExceededError',()=>answer.length);",
        "reclaim-refresh-refusal.js");
    dom_fail_at=0;
    check(f.document->node_count==nodes,
          "refused collection refresh retains previous answer holds");
    dom_script(&f, RECLAIM_ASSERT "assert(answer.length===1&&answer.item(0)===kept);",
        "reclaim-refresh.js");
    check(f.document->node_count==nodes-2,
          "successful collection refresh reclaims unwrapped element and text");
    check(os64_html_remove(f.document,one)==OS64_HTML_OK,
          "detach wrapper-held subtree without a model");
    dom_script(&f, RECLAIM_ASSERT
        "assert(answer.length===0);assert(kept.parentNode===null&&kept.textContent==='one');"
        "assert(kept.expando==='retained');box.appendChild(kept);"
        "assert(document.getElementById('one')===kept);",
        "reclaim-wrapper-identity.js");
    os64_html_node_t *box=native_id(f.document->document,"box");
    nodes=f.document->node_count;
    check(os64_html_remove(f.document,box)==OS64_HTML_OK && f.document->node_count==nodes,
          "query root and wrapper holds retain a detached ancestor");
    os64_dom_drain(f.binding);
    os64_js_destroy(f.runtime);f.runtime=NULL;
    os64_dom_free(f.binding);f.binding=NULL;
    check(f.document->node_count==nodes-3,
          "binding teardown releases wrapper and query-root holds before document teardown");
    fixture_free(&f);

    f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    dom_script(&f, RECLAIM_ASSERT
        "var box=document.getElementById('box');"
        "box.innerHTML='<p>new</p>';assert(box.innerHTML==='<p>new</p>');",
        "reclaim-innerHTML-start.js");
    nodes=f.document->node_count;
    size_t arena=f.document->arena_bytes;
    dom_script(&f, RECLAIM_ASSERT
        "for(var i=0;i<1000;i++)box.innerHTML='<p>new</p>';"
        "assert(box.innerHTML==='<p>new</p>');",
        "reclaim-innerHTML-churn.js");
    check(f.document->node_count==nodes && f.document->arena_bytes==arena,
          "binding innerHTML replacements reclaim payloads and empty fragment containers");
    dom_script(&f, "box.innerHTML='';", "reclaim-empty-start.js");
    nodes=f.document->node_count;arena=f.document->arena_bytes;
    uint64_t version=os64_html_version(f.document);
    dom_script(&f, "for(var i=0;i<1000;i++)box.innerHTML='';", "reclaim-empty-churn.js");
    check(f.document->node_count==nodes && f.document->arena_bytes==arena &&
          os64_html_version(f.document)==version,
          "empty binding replacements reclaim result containers without a tree-version change");
    fixture_free(&f);
}

#undef RECLAIM_ASSERT
