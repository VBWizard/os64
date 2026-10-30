// Exercise the public incremental door against a wholly independent cascade.
// Destroying the source tree before CSSOM reads makes borrowed list storage
// a sanitizer failure rather than an accidentally correct comparison.
static void restyle_equal(flow_tree_t *a, flow_tree_t *b, const flow_env_t *ea,
                          const flow_env_t *eb, const os64_html_node_t *node)
{
    int64_t na = flow_dump(a, NULL, 0), nb = flow_dump(b, NULL, 0);
    char *sa = malloc((size_t)na + 1), *sb = malloc((size_t)nb + 1);
    flow_dump(a, sa, (size_t)na + 1); flow_dump(b, sb, (size_t)nb + 1);
    expect("restyle: layout equals whole layout", na == nb && !strcmp(sa, sb), NULL);
    free(sa); free(sb);
    for (; node; node = node->next) {
        if (node->kind == OS64_HTML_ELEMENT) {
            flow_computed_t *ca = flow_computed_new(a, ea, node, NULL);
            flow_computed_t *cb = flow_computed_new(b, eb, node, NULL);
            for (int32_t prop = 0; prop < GARB_NPROPS; ++prop) {
                char va[4096], vb[4096];
                size_t la = flow_computed_text(ca, prop, va, sizeof(va));
                size_t lb = flow_computed_text(cb, prop, vb, sizeof(vb));
                expect("restyle: computed property equals whole computation",
                       la == lb && !strcmp(va, vb), garb_prop_name(prop));
            }
            flow_computed_free(ca); flow_computed_free(cb);
        }
        if (node->first_child) restyle_equal(a, b, ea, eb, node->first_child);
    }
}
static void restyle_cases(void)
{
    const char *html = "<!doctype html><style>"
        "#changed.on{color:purple;font-size:21px}"
        "#kept{font-family:Missing,serif;background-image:linear-gradient(45deg,red,blue),url(a.png);"
        "background-position:2px 3px,4px 5px;background-repeat:no-repeat,repeat;"
        "box-shadow:1px 2px 3px red;text-shadow:2px 3px blue;transform:translate(2px,3px) rotate(5deg);"
        "display:grid;grid-template-columns:20px 1fr;grid-template-areas:'one two';}"
        "#kept span{grid-area:one}"
        "</style><div id=changed>change <b>me</b></div>"
        "<div id=kept><span>A</span><a href=x>B</a><span style='display:contents'>C</span></div>";
    Sheeted p; sheeted_open(&p, html, 800, 600);
    flow_tree_t *old = flow_layout(p.doc, p.page, 800, &p.env);
    os64_html_node_t *changed = (os64_html_node_t *)computed_find(p.doc->document, "changed");
    os64_html_set_attr(p.doc, changed, "class", "on", 2);
    expect("restyle: update cascade", garb_cascade_update(p.cascade), NULL);
    os64_page_t *model = os64_page_rebuild(p.page);
    flow_tree_t *next = flow_layout_restyled(p.doc, model, 800, &p.env, old, NULL, NULL, NULL);
    expect("restyle: unaffected styles reused", next && flow_styles_reused(next) >= 4, NULL);
    size_t held = live, points = 0;
    for (int mode = 0; mode < 2; ++mode) {
        for (size_t at = 0; at <= points; ++at) {
            size_t before = allocations;
            fail_single = mode != 0;
            fail_at = at ? before + at : 0;
            flow_tree_t *trial = flow_layout_restyled(p.doc, model, 800, &p.env, old, NULL, NULL, NULL);
            fail_at = 0; fail_single = false;
            if (!at) points = allocations - before;
            flow_free(trial);
            expect("restyle: allocation refusal releases new ownership", live == held, NULL);
        }
    }
    printf("libflow restyle: %zu allocation positions, single and persistent failures\n", points);
    flow_free(old);
    garb_sheet_in_t in[16];
    for (int32_t i=0;i<p.n;i++) in[i]=(garb_sheet_in_t){.sheet=&p.sheets[i]};
    garb_cascade_t *whole = garb_cascade(in,p.n,p.doc,garb_cascade_env(p.cascade));
    flow_env_t env = p.env; env.cascade=whole;
    flow_tree_t *oracle = flow_layout(p.doc,model,800,&env);
    restyle_equal(next,oracle,&p.env,&env,p.doc->document);
    flow_free(oracle); garb_cascade_free(whole); flow_free(next);
    os64_page_free(model); sheeted_close(&p);
}

static void restyle_round(Sheeted *p, flow_tree_t **old)
{
    expect("restyle: mutation cascade update succeeds", garb_cascade_update(p->cascade), NULL);
    os64_page_t *model=os64_page_rebuild(p->page);
    flow_tree_t *next=flow_layout_restyled(p->doc,model,800,&p->env,*old,NULL,NULL,NULL);
    garb_sheet_in_t in[16];
    for(int32_t i=0;i<p->n;i++) in[i]=(garb_sheet_in_t){.sheet=&p->sheets[i]};
    garb_cascade_t *c=garb_cascade(in,p->n,p->doc,garb_cascade_env(p->cascade));
    flow_env_t env=p->env;env.cascade=c;
    flow_tree_t *whole=flow_layout(p->doc,model,800,&env);
    expect("restyle: mutation layouts complete",next && whole && !flow_incomplete(next) && !flow_incomplete(whole),NULL);
    flow_free(*old);
    restyle_equal(next,whole,&p->env,&env,p->doc->document);
    flow_free(whole);garb_cascade_free(c);
    os64_page_free(p->page);p->page=model;*old=next;
}
static void restyle_mutations(void)
{
    Sheeted p;
    sheeted_open(&p,"<!doctype html><style>html{font-size:16px}div{--ink:red}"
        "p{color:var(--ink);margin:1rem}p:first-child{padding:2px}"
        "p + p{border:1px solid blue}#stable{font-family:Missing,serif}"
        "</style><div id=parent><p id=a>A</p><p id=b>B</p></div>"
        "<aside id=stable><a id=link href=a>link</a><ruby id=ruby>X<rt>Y</rt></ruby></aside>"
        "<details id=details><summary id=summary>One</summary><summary>Two</summary><p>Content</p></details>",800,600);
    flow_tree_t *old=flow_layout(p.doc,p.page,800,&p.env);
    os64_html_node_t *a=(os64_html_node_t *)computed_find(p.doc->document,"a");
    os64_html_node_t *b=(os64_html_node_t *)computed_find(p.doc->document,"b");
    os64_html_node_t *parent=(os64_html_node_t *)computed_find(p.doc->document,"parent");
    os64_html_node_t *aside=(os64_html_node_t *)computed_find(p.doc->document,"stable");
    os64_html_node_t *summary=(os64_html_node_t *)computed_find(p.doc->document,"summary");
    os64_html_set_attr(p.doc,parent,"style","--ink:green",11);restyle_round(&p,&old);
    os64_html_set_text(p.doc,a->first_child,"longer text",11);restyle_round(&p,&old);
    os64_html_insert(p.doc,parent,b,a);restyle_round(&p,&old);
    os64_html_insert(p.doc,aside,a,NULL);restyle_round(&p,&old);
    os64_html_remove(p.doc,b);restyle_round(&p,&old);
    os64_html_set_attr(p.doc,p.doc->html,"style","font-size:23px",14);restyle_round(&p,&old);
    os64_html_set_attr(p.doc,p.doc->body,"link","green",5);restyle_round(&p,&old);
    os64_html_remove(p.doc,summary);restyle_round(&p,&old);
    os64_html_node_t *details=(os64_html_node_t *)computed_find(p.doc->document,"details");
    os64_html_set_attr(p.doc,details,"open","",0);restyle_round(&p,&old);
    os64_html_set_attr(p.doc,aside,"style","display:none",12);restyle_round(&p,&old);
    os64_html_set_attr(p.doc,aside,"style","display:contents",16);restyle_round(&p,&old);
    flow_free(old);sheeted_close(&p);
}
