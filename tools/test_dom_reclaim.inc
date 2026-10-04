#define RECLAIM_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;assert(e.name===n)}assert(caught);} "
// These fixtures deliberately have no model pin: retention comes from the
// binding's wrappers and collection answers, rather than an older snapshot.
static void dom_reclaim_cases(void)
{
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
