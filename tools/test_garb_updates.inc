static unsigned update_checks, update_failures;
static void update_check(bool ok, const char *what)
{
    ++update_checks;
    if (!ok) { ++update_failures; fprintf(stderr,"FAIL cascade update: %s\n",what); }
}
static os64_html_document_t *update_doc(const char *html)
{
    os64_html_options_t opt=os64_html_options_default();
    os64_html_parser_t *p=os64_html_parser_new(&opt);
    os64_html_parser_feed(p,html,strlen(html));return os64_html_parser_finish(p);
}
static void update_equal(garb_cascade_t *c, os64_html_document_t *doc, garb_sheet_in_t *in)
{
    garb_cascade_t *whole=garb_cascade(in,1,doc,garb_cascade_env(c));
    char a[65536],b[65536];
    size_t na=garb_cascade_dump(c,doc,a,sizeof(a)),nb=garb_cascade_dump(whole,doc,b,sizeof(b));
    update_check(whole && !garb_cascade_incomplete(whole) && na<sizeof(a) && na==nb && !strcmp(a,b),
                 "updated winners equal a fresh cascade");
    garb_cascade_free(whole);
}
static bool update_or_rebuild(garb_cascade_t **c, os64_html_document_t *doc, garb_sheet_in_t *in)
{
    if (garb_cascade_update(*c)) return true;
    garb_cascade_t *fresh=garb_cascade(in,1,doc,garb_cascade_env(*c));
    if (!fresh || garb_cascade_incomplete(fresh)) { garb_cascade_free(fresh); return false; }
    garb_cascade_free(*c); *c=fresh;
    return true;
}
// Isolate a small changing subtree inside a larger document so these cases
// exercise incremental winners, rather than passing through dense fallback.
static void update_dependencies(void)
{
    const char *rules[]={
        "p:nth-child(2n){color:red}p:first-of-type{opacity:.5}",
        "p:last-child{color:red}p:nth-last-of-type(2){opacity:.5}",
        "p:empty{--ink:red}p:not(:empty){--ink:blue}span{color:var(--ink)}p:empty + p{color:green}",
        "p.on ~ p span{color:red}p:not(:is(.on + p)){opacity:.5}",
        "p:nth-child(2 of :is(.on, .on + p)){color:red}p:nth-last-child(1 of .on){opacity:.5}",
        "x-item:defined{color:red}x-item:not(:defined) + p{color:blue}",
        "i{--ink:black;color:var(--ink)}p{--ink:red}p::before,p::after{content:'x';color:var(--ink)}.on{--ink:blue}"
        "p.on::before{content:attr(title)}p:empty + p::after{color:green}"
    };
    for(size_t k=0;k<sizeof(rules)/sizeof(*rules);++k) {
        size_t baseline=live;
        char html[8192]="<!doctype html><aside>";
        for(int i=0;i<50;++i)strcat(html,"<i></i>");
        strcat(html,"</aside><div id=box><p id=a>A</p><p id=b class=on><span>B</span></p><p id=c>C</p><x-item id=x></x-item><p id=e></p></div><section id=other></section>");
        os64_html_document_t *doc=update_doc(html);
        garb_parsed_t sheet;garb_parse_sheet_text(rules[k],strlen(rules[k]),&sheet);
        garb_sheet_in_t in={.sheet=&sheet};
        garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        os64_html_node_t *box=(void *)by_id(doc->document,"box"),*a=(void *)by_id(doc->document,"a"),
            *b=(void *)by_id(doc->document,"b"),*last=(void *)by_id(doc->document,"c"),
            *x=(void *)by_id(doc->document,"x"),*empty=(void *)by_id(doc->document,"e"),
            *other=(void *)by_id(doc->document,"other");
        os64_html_set_text(doc,a->first_child,"",0);
        update_check(garb_cascade_update(c),"text/empty dependency updates incrementally");update_equal(c,doc,&in);
        if(k==0)update_check(!garb_style_updated_since(c,b,0),"positional selectors do not dirty siblings for text edits");
        os64_html_set_attr(doc,a,"class","on",2);
        update_check(garb_cascade_update(c),"attribute/filter/following update");update_equal(c,doc,&in);
        os64_html_set_ce(doc,x,OS64_HTML_CE_CUSTOM);
        update_check(garb_cascade_update(c),"custom-element STATE update");update_equal(c,doc,&in);
        // The historical previous sibling moves to a new position in the
        // same parent before the log is read.
        os64_html_insert(doc,box,empty,b);
        os64_html_insert(doc,box,a,NULL);
        os64_html_insert(doc,box,last,empty);
        update_check(garb_cascade_update(c),"batched moves preserve positional matches");update_equal(c,doc,&in);
        os64_html_node_t *span=b->first_child;
        os64_html_remove(doc,b);
        os64_html_insert(doc,other,span,NULL);
        os64_html_node_t *replacement=os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL);
        update_check(replacement && replacement!=b,"cascade pin prevents remove/create address alias");
        os64_html_insert(doc,box,replacement,NULL);
        update_check(garb_cascade_update(c),"removed ancestor and rescued descendant update");update_equal(c,doc,&in);
        os64_html_node_t *text=os64_html_create_text(doc,"filled",6,NULL);
        os64_html_insert(doc,empty,text,NULL);
        update_check(garb_cascade_update(c),"empty parent changes after insertion");update_equal(c,doc,&in);
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
        update_check(live==baseline,"dependency fixtures release node pins");
    }
    // Stream appends grow the persistent table, while removed slots exercise
    // collision backshifts. Compare each published version with a rebuild.
    size_t baseline=live;
    os64_html_options_t opt=os64_html_options_default();
    os64_html_parser_t *parser=os64_html_parser_new(&opt);
    const char *start="<!doctype html><body>";
    os64_html_parser_feed(parser,start,strlen(start));
    for(int i=0;i<20;++i)os64_html_parser_feed(parser,"<i></i>",7);
    os64_html_document_t *doc=os64_html_parser_document(parser);
    garb_parsed_t sheet;const char *css="p{color:red;opacity:.5}";
    garb_parse_sheet_text(css,strlen(css),&sheet);garb_sheet_in_t in={.sheet=&sheet};
    garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    for(int i=0;i<80;++i) {
        os64_html_parser_feed(parser,"<p>stream</p>",13);
        update_check(i<12 ? garb_cascade_update(c) : update_or_rebuild(&c,doc,&in),
                     "parser-fed cascade update including slot growth");update_equal(c,doc,&in);
    }
    doc=os64_html_parser_finish(parser);
    update_check(update_or_rebuild(&c,doc,&in),"parser finish update");update_equal(c,doc,&in);
    for(int i=0;i<60;++i) {
        os64_html_remove(doc,doc->body->first_child);
        update_check(update_or_rebuild(&c,doc,&in),"repeated slot removal");update_equal(c,doc,&in);
    }
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    update_check(live==baseline,"streaming fixture releases allocations");
}
// A flat stylesheet should not need one temporary heap allocation per
// styled element. Include a larger declaration set in the same update so
// the scratch buffer must grow without changing earlier winners.
static void update_scratch_cost(void)
{
    size_t points=0;
    for(size_t failure=0;failure<=points;++failure) {
        size_t baseline=live;
        char html[4096]="<!doctype html><main id=rows>";
        for(int i=0;i<256;++i)strcat(html,"<p></p>");
        strcat(html,"</main>");
        os64_html_document_t *doc=update_doc(html);
        char css[2048]="p{color:red}.on{color:blue;";
        for(int i=0;i<40;++i)strcat(css,"width:2px;");
        strcat(css,"}.large{");
        for(int i=0;i<60;++i)strcat(css,"height:3px;");
        strcat(css,"}");
        garb_parsed_t sheet;garb_parse_sheet_text(css,strlen(css),&sheet);
        garb_sheet_in_t in={.sheet=&sheet};
        size_t before=allocations;
        garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        size_t build_calls=allocations-before;
        update_check(c && !garb_cascade_incomplete(c) && build_calls<128,
                     "flat full cascade avoids per-element temporary allocations");
        os64_html_node_t *rows=(void *)by_id(doc->document,"rows"),*node=rows->first_child;
        for(int i=0;i<100;++i,node=node->next) {
            const char *classes=i%3==1?"on":i%3==2?"on large":"off";
            os64_html_set_attr(doc,node,"class",classes,strlen(classes));
        }
        before=allocations;fail_single=true;fail_at=failure?before+failure:0;
        bool updated=garb_cascade_update(c);
        size_t update_calls=allocations-before;
        fail_at=0;fail_single=false;
        if(!failure) {
            points=update_calls;
            update_check(updated && update_calls<50,"batch update shares temporary declaration storage");
            printf("libgarb scratch: 256-element build %zu allocations, 100-element update %zu\n",build_calls,update_calls);
        }
        if(updated) update_equal(c,doc,&in);
        else update_check(!garb_cascade_update(c),"failed scratch growth requires a fresh cascade");
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
        update_check(live==baseline,"temporary declaration storage released with its pass");
    }
    printf("libgarb scratch: %zu batch-update failure positions\n",points);
}

/* :has() under change tracking (BRIEF_HAS_CASCADE): each case mutates,
 * updates, and checks the update was incremental and its winners equal a
 * fresh cascade's; then what was and was not restyled. */
typedef struct {
    os64_html_document_t *doc;
    garb_parsed_t sheet;
    garb_sheet_in_t in;
    garb_cascade_t *c;
} HasCase;
// The document is padded with elements no rule touches, so a change and
// the ancestors it reaches stay a small part of it (more than half is a
// fresh cascade by design).
static void has_open(HasCase *h, const char *css, const char *html)
{
    char padded[16384]="<!doctype html><aside>";
    for(int i=0;i<200;++i) strcat(padded,"<i></i>");
    strcat(padded,"</aside>");
    strcat(padded,strncmp(html,"<!doctype html>",15) ? html : html+15);
    h->doc=update_doc(padded);
    garb_parse_sheet_text(css,strlen(css),&h->sheet);
    h->in=(garb_sheet_in_t){.sheet=&h->sheet};
    h->c=garb_cascade(&h->in,1,h->doc,(garb_env_t){.width=800,.height=600});
    update_check(h->c && garb_cascade_tracks_changes(h->c),"a :has() cascade tracks changes");
}
static os64_html_node_t *has_el(HasCase *h, const char *id) { return (os64_html_node_t *)by_id(h->doc->document,id); }
// One update: incremental, and its winners a fresh cascade's. Answers the
// update number the next checks compare against.
static uint64_t has_update(HasCase *h, const char *what)
{
    uint64_t before=garb_cascade_updates(h->c);
    bool ok=garb_cascade_update(h->c);
    update_check(ok && garb_cascade_update_refusal(h->c)==GARB_UPDATE_DONE,what);
    if(!ok) fprintf(stderr,"  refused: %s\n",garb_update_refusal_name(garb_cascade_update_refusal(h->c)));
    update_equal(h->c,h->doc,&h->in);
    return before;
}
static void has_close(HasCase *h)
{
    garb_cascade_free(h->c);garb_free(&h->sheet);os64_html_document_free(h->doc);
}
static void update_has(void)
{
    size_t baseline=live;
    HasCase h;
    /* Subject: the ancestor's own winners and, through a custom property,
     * its descendants'. A change that leaves the match as it was restyles
     * neither the ancestor nor what is under it. */
    has_open(&h,".a:has(.b){--ink:red}.a{--ink:blue}span{color:var(--ink)}",
             "<!doctype html><div id=a class=a><p id=p><span id=s>t</span></p><p id=q><i id=i></i></p></div>"
             "<div id=far><span id=fs>u</span></div>");
    uint64_t was=garb_cascade_updates(h.c);
    os64_html_set_attr(h.doc,has_el(&h,"i"),"class","b",1);
    was=has_update(&h,"subject: an argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"a"),was) && garb_style_updated_since(h.c,has_el(&h,"s"),was),
                 "subject: the ancestor and its descendants are restyled");
    update_check(!garb_style_updated_since(h.c,has_el(&h,"fs"),was),"subject: an unrelated subtree is not");
    os64_html_set_attr(h.doc,has_el(&h,"p"),"title","x",1);
    was=has_update(&h,"subject: a change that keeps the match updates incrementally");
    update_check(!garb_style_updated_since(h.c,has_el(&h,"a"),was) && !garb_style_updated_since(h.c,has_el(&h,"q"),was),
                 "subject: an ancestor whose match held keeps its record, and its other children theirs");
    os64_html_remove(h.doc,has_el(&h,"i"));
    was=has_update(&h,"subject: the argument removed updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"s"),was),"subject: and restyles what inherited from it");
    os64_html_node_t *b=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"b",NULL);
    os64_html_set_attr(h.doc,b,"class","b",1);
    os64_html_insert(h.doc,has_el(&h,"q"),b,NULL);
    has_update(&h,"subject: an argument inserted updates incrementally");
    has_close(&h);
    /* A sibling argument: the preceding sibling is the subject. */
    has_open(&h,".a:has(~ .b){color:red}.a:has(+ u){opacity:.5}",
             "<!doctype html><div><p id=a class=a>a</p><p id=m>m</p><p id=n>n</p></div>");
    os64_html_set_attr(h.doc,has_el(&h,"n"),"class","b",1);
    was=has_update(&h,":has(~): a following sibling turning updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"a"),was),":has(~): and restyles the subject before it");
    /* An element with no attributes: the change is the parent's children
     * alone, so the subject is found among them. */
    os64_html_node_t *cc=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"u",NULL);
    os64_html_insert(h.doc,has_el(&h,"a")->parent,cc,has_el(&h,"m"));
    was=has_update(&h,":has(+): a sibling inserted after the subject updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"a"),was),":has(+): and restyles the subject before it");
    has_close(&h);
    /* :dir() follows the dir attribute's inheritance: an update. */
    has_open(&h,"p:dir(rtl){color:green}","<!doctype html><div id=d><p id=p>x</p></div>");
    os64_html_set_attr(h.doc,has_el(&h,"d"),"dir","rtl",3);
    has_update(&h,":dir(): a dir attribute changing updates incrementally");
    has_close(&h);
    /* Anchored: the :has() is left of a combinator, and what changes is
     * below or beside the element it tests. */
    has_open(&h,".g:has(.x) .y{color:red}.h:has(c) > a{color:blue}.p:has(.x) ~ .z{opacity:.5}"
             ":is(.k:has(.x)) .y{background:green}",
             "<!doctype html><div id=g class=g><p id=y class=y>y</p><p id=w>w</p></div>"
             "<div id=g2 class=g><p id=y2 class=y>y</p></div>"
             "<div id=h class=h><a id=ha>a</a></div>"
             "<div><p id=pp class=p>p</p><p id=z class=z>z</p></div>"
             "<div id=k class=k><p id=ky class=y>y</p></div>");
    os64_html_set_attr(h.doc,has_el(&h,"w"),"class","x",1);
    was=has_update(&h,"anchored: an argument arriving under the anchor updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"y"),was),"anchored: what the anchor reaches is restyled");
    update_check(!garb_style_updated_since(h.c,has_el(&h,"y2"),was),"anchored: another anchor's subtree is not");
    os64_html_set_attr(h.doc,has_el(&h,"w"),"class","",0);
    has_update(&h,"anchored: the argument going updates incrementally");
    os64_html_insert(h.doc,has_el(&h,"h"),os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"c",NULL),NULL);
    has_update(&h,"anchored with >: an argument inserted updates incrementally");
    os64_html_node_t *px=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"i",NULL);
    os64_html_set_attr(h.doc,px,"class","x",1);
    os64_html_insert(h.doc,has_el(&h,"pp"),px,NULL);
    was=has_update(&h,"anchored with ~: an argument inserted updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"z"),was),"anchored with ~: the following sibling is restyled");
    os64_html_remove(h.doc,px);
    was=has_update(&h,"anchored with ~: the argument removed updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"z"),was) && !garb_style_updated_since(h.c,has_el(&h,"pp"),was),
                 "anchored with ~: the sibling is restyled back, the anchor itself is not");
    os64_html_node_t *kx=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"i",NULL);
    os64_html_set_attr(h.doc,kx,"class","x",1);
    os64_html_insert(h.doc,has_el(&h,"k"),kx,NULL);
    was=has_update(&h,"anchored through :is(): an argument inserted updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"ky"),was),"anchored through :is(): what it reaches is restyled");
    has_close(&h);
    /* An element both marked alone (a subject :has() in the sheets) and an
     * anchor whose answer turned owes its whole subtree. */
    has_open(&h,".g:has(.x) .y{color:red}.q:has(.r){margin:1px}",
             "<!doctype html><div id=g class=g><p id=y class=y>y</p><p id=w>w</p></div>");
    os64_html_set_attr(h.doc,has_el(&h,"w"),"class","x",1);
    was=has_update(&h,"subject and anchor: an argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"y"),was),"subject and anchor: what the anchor reaches is restyled");
    has_close(&h);
    /* The body as an anchor: an append elsewhere under it restyles only
     * what was appended; the argument turning restyles what it reaches. */
    {
        char html[8192]="<!doctype html><body><div id=panel class=panel>p</div><div id=feed>";
        for(int i=0;i<40;++i) strcat(html,"<p class=post>x</p>");
        strcat(html,"</div><t id=t></t>");
        has_open(&h,"body:has(t[data-open]) .panel{color:red}.post{color:blue}",html);
        os64_html_insert(h.doc,has_el(&h,"feed"),os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"p",NULL),NULL);
        was=has_update(&h,"body anchor: an append updates incrementally");
        update_check(!garb_style_updated_since(h.c,has_el(&h,"panel"),was) &&
                     !garb_style_updated_since(h.c,has_el(&h,"feed")->first_child,was),
                     "body anchor: an append whose match held restyles nothing it did not add");
        /* The body's whole subtree is what it reaches: more than half the
         * elements, so a fresh cascade, and the right one. */
        os64_html_set_attr(h.doc,has_el(&h,"t"),"data-open","",0);
        update_check(!garb_cascade_update(h.c) && garb_cascade_update_refusal(h.c)==GARB_UPDATE_BROAD,
                     "body anchor: the argument turning asks for a fresh cascade");
        update_check(update_or_rebuild(&h.c,h.doc,&h.in),"body anchor: which builds");
        update_equal(h.c,h.doc,&h.in);
        has_close(&h);
    }
    /* A container changed and then taken out in one batch: nothing in it
     * is visited, and the update goes ahead. */
    has_open(&h,".a:has(~ .b){color:red}.g:has(.x) .y{color:blue}",
             "<!doctype html><div id=out><div id=d><p id=dp class=a>a</p></div></div><p id=o>o</p>");
    os64_html_insert(h.doc,has_el(&h,"d"),os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"p",NULL),NULL);
    os64_html_remove(h.doc,has_el(&h,"d"));
    has_update(&h,"a container changed and taken out in one batch updates incrementally");
    has_close(&h);
    /* An ancestor whose winners are read through var(), and one with a
     * custom property in its style attribute: new copies each recompute,
     * the same rules matching, so neither is restyled when its match held. */
    has_open(&h,":root{--f:Arial, sans-serif}body{font-family:var(--f)}.a:has(.x){color:red}",
             "<!doctype html><div id=feed style='--g:1;font-family:serif'><p id=one>1</p><p id=two>2</p></div>");
    os64_html_set_attr(h.doc,has_el(&h,"one"),"title","t",1);
    was=has_update(&h,"var() and style-attribute ancestors: a change updates incrementally");
    update_check(!garb_style_updated_since(h.c,h.doc->body,was) && !garb_style_updated_since(h.c,has_el(&h,"feed"),was) &&
                 !garb_style_updated_since(h.c,has_el(&h,"two"),was),
                 "var() and style-attribute ancestors: neither they nor their other children are restyled");
    has_close(&h);
    /* A turn that restyles more than half the elements asks for a fresh
     * cascade, says why, and the fresh one is right. */
    {
        char html[8192]="<!doctype html><div id=a class=a>";
        for(int i=0;i<260;++i) strcat(html,"<p>x</p>");
        strcat(html,"<i id=i></i></div>");
        has_open(&h,".a:has(.b){--ink:red}p{color:var(--ink)}",html);
        os64_html_set_attr(h.doc,has_el(&h,"i"),"class","b",1);
        update_check(!garb_cascade_update(h.c) && garb_cascade_update_refusal(h.c)==GARB_UPDATE_BROAD,
                     "a broad :has() turn asks for a fresh cascade, and says it was broad");
        update_check(update_or_rebuild(&h.c,h.doc,&h.in),"and the fresh cascade builds");
        update_equal(h.c,h.doc,&h.in);
        has_close(&h);
    }
    update_check(live==baseline,":has() updates release what they made");
}

static int cascade_updates(void)
{
    update_dependencies();
    update_has();
    update_scratch_cost();
    size_t baseline=live;
    const char *css[] = {
        "div{--ink:red}p{color:var(--ink)}.on{--ink:blue}span{opacity:.5}",
        "p:first-child{color:red}p:last-child{color:blue}p:nth-child(2n){opacity:.5}p + p span{color:green}",
        "p:empty{color:red}div:not(:is(.on + div)){opacity:.5}p:nth-child(2 of .on){color:blue}",
        "p{fill:red;stroke:blue}#a{color:green}p[title]{color:blue}"
    };
    for(size_t i=0;i<sizeof(css)/sizeof(*css);++i) {
        os64_html_document_t *doc=update_doc("<!doctype html><div id=box><p id=a>first<span>A</span></p><p id=b>second</p><p id=c></p></div><aside id=aside><span>unchanged</span></aside>");
        garb_parsed_t sheet;garb_parse_sheet_text(css[i],strlen(css[i]),&sheet);
        garb_sheet_in_t in={.sheet=&sheet};
        garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        os64_html_node_t *a=(os64_html_node_t *)by_id(doc->document,"a"),*b=(os64_html_node_t *)by_id(doc->document,"b"),
            *box=(os64_html_node_t *)by_id(doc->document,"box"),*aside=(os64_html_node_t *)by_id(doc->document,"aside");
        uint64_t generation=garb_cascade_generation(c);
        update_check(garb_cascade_tracks_changes(c),"local cascade advertises change tracking");
        update_check(!os64_html_set_attr(doc,a,"class","on",2) && garb_cascade_update(c),"class update succeeds");
        update_check(garb_cascade_generation(c)==generation && garb_cascade_updates(c)==1 &&
                     garb_style_updated_since(c,a,0) && !garb_style_updated_since(c,aside,0),"generation stable, unrelated branch untouched");
        update_equal(c,doc,&in);
        update_check(!os64_html_set_attr(doc,box,"class","on",2) && update_or_rebuild(&c,doc,&in),"inherited custom property update");update_equal(c,doc,&in);
        update_check(!os64_html_set_text(doc,a->first_child,"",0) && update_or_rebuild(&c,doc,&in),"text update");update_equal(c,doc,&in);
        update_check(!os64_html_insert(doc,box,b,a) && update_or_rebuild(&c,doc,&in),"move update");update_equal(c,doc,&in);
        update_check(!os64_html_remove(doc,b) && update_or_rebuild(&c,doc,&in),"removed slots discarded");update_equal(c,doc,&in);
        update_check(!os64_html_set_attr(doc,a,"style","color:purple;background:linear-gradient(red,blue)",49) &&
                     update_or_rebuild(&c,doc,&in),"inline parse update");update_equal(c,doc,&in);
        for(int k=0;k<150;k++) {
            update_check(!os64_html_set_attr(doc,a,"title",k%2?"x":"y",1),"repeated edit");
            if(!garb_cascade_update(c)) {
                garb_cascade_t *fresh=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
                garb_cascade_free(c);c=fresh;
            }
            update_equal(c,doc,&in);
        }
        garb_cascade_free(c);
        c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        os64_html_changes_limit(doc,0);
        os64_html_set_attr(doc,a,"class","lost",4);
        update_check(!garb_cascade_update(c) && garb_cascade_update_refusal(c)==GARB_UPDATE_HISTORY,
                     "lost history refuses reuse, and says so");
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
        update_check(live==baseline,"updates release allocations and node holds");
    }
    /* What still keeps a cascade from tracking changes: an anchored :has()
     * with nothing to test before it (a subtree walk on every element),
     * and more anchors than a record remembers. Each is named. */
    char many[8192]; size_t mlen=0;
    for(int i=0;i<=GARB_ANCHORS_MAX;++i) mlen+=(size_t)sprintf(many+mlen,".g%d:has(.x) i{color:red}",i);
    /* And a :has() inside :nth-child(… of S): its turn moves which sibling
     * the count picks, which nothing follows. */
    const char *broad[]={":has(p) i{color:red}",many,"p:nth-child(1 of .a:has(.x)){color:red}"};
    const garb_update_refusal_t why[]={GARB_UPDATE_BARE_ANCHOR,GARB_UPDATE_ANCHORS,GARB_UPDATE_COUNTED_HAS};
    for(size_t i=0;i<sizeof(broad)/sizeof(*broad);++i) {
        os64_html_document_t *doc=update_doc("<!doctype html><div id=a><p>text</p></div>");
        garb_parsed_t sheet;garb_parse_sheet_text(broad[i],strlen(broad[i]),&sheet);garb_sheet_in_t in={.sheet=&sheet};
        garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        update_check(!garb_cascade_tracks_changes(c),"untrackable cascade advertises no change tracking");
        os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"a"),"class","x",1);
        update_check(!os64_html_changes_bytes(doc),"untrackable cascade does not retain an unusable mutation log");
        update_check(!garb_cascade_update(c) && garb_cascade_update_refusal(c)==why[i],
                     "untrackable cascade requests a full cascade and says why");
        os64_html_reader_t *reader=os64_html_reader_new(doc,os64_html_version(doc));
        os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"a"),"title","kept",4);
        const os64_html_change_entry_t *entry=NULL;
        update_check(reader && os64_html_changes(reader,&entry)==1 && entry && entry->kind==OS64_HTML_CHANGE_ATTR,
                     "untrackable cascade leaves another native reader's invalidations intact");
        os64_html_reader_free(reader);
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    }
    update_check(!strcmp(garb_update_refusal_name(GARB_UPDATE_BROAD),"broad") &&
                 !strcmp(garb_update_refusal_name(GARB_UPDATE_REASONS),"?"),
                 "refusals have names, and past the enum is '?'");
    // Broad changes renew by bytes, while small changes keep a large
    // cascade reusable. These are allocator-pressure regressions, not clocks.
    for (int broad=0;broad<2;++broad) {
        char html[65536]; size_t len=(size_t)sprintf(html,"<!doctype html><div id=box>");
        for(int i=0;i<2000;++i) len+=(size_t)sprintf(html+len,"<p>%d</p>",i);
        strcpy(html+len,"</div>");
        os64_html_document_t *doc=update_doc(html);
        const char *css="p{color:red;background:blue;border:1px solid green;padding:2px}";
        garb_parsed_t sheet;garb_parse_sheet_text(css,strlen(css),&sheet);
        garb_sheet_in_t in={.sheet=&sheet};
        garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        os64_html_node_t *box=(os64_html_node_t *)by_id(doc->document,"box");
        os64_html_node_t *target=broad?box:box->first_child;
        unsigned renewals=0;
        for(int i=0;i<20;i++) {
            os64_html_set_attr(doc,target,"title",i%2?"a":"b",1);
            uint64_t was=garb_cascade_generation(c);
            update_check(update_or_rebuild(&c,doc,&in),"retention-pressure update completes");
            renewals+=garb_cascade_generation(c)!=was;
        }
        update_check(broad ? renewals>=4 : renewals==0,
                     broad?"large restyles renew before 64 updates":"small restyles retain incremental reuse");
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
        update_check(live==baseline,"retention-pressure ownership released");
    }
    size_t points=0;
    // 0: an inline style; 1: slots grown; 2: a :has() turned, its subject
    // and an anchor both, with the elements it reaches marked and restyled.
    static const char *const kSweepNames[]={"inline style","slot growth",":has() turn"};
    for(int growth=0;growth<3;++growth) {
        points=0;
        for(size_t failure=0;failure<=points;++failure) {
            char html[4096]="<!doctype html><body>";
            for(int i=0;i<27;++i)strcat(html,"<p></p>");
            if(growth==2) {
                strcpy(html,"<!doctype html><aside>");
                for(int i=0;i<60;++i)strcat(html,"<i></i>");
                strcat(html,"</aside><div id=a class=a><p>text</p><p id=w></p></div>");
            }
            os64_html_document_t *doc=update_doc(growth?html:"<!doctype html><div id=a><p>text</p></div>");
            garb_parsed_t sheet;
            const char *text=growth==2 ? "div{--x:red}.a:has(.b){--x:blue}.a:has(.b) p{opacity:.5}p{color:var(--x)}"
                                       : "div{--x:red}p{color:var(--x)}";
            garb_parse_sheet_text(text,strlen(text),&sheet);garb_sheet_in_t in={.sheet=&sheet};
            garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
            if(growth==2) {
                os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"w"),"class","b",1);
            } else if(growth) {
                for(int i=0;i<3;++i) {
                    os64_html_node_t *p=os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL);
                    os64_html_insert(doc,doc->body,p,NULL);
                }
            } else os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"a"),"style","--x:blue",8);
            size_t before=allocations;fail_single=true;fail_at=failure?before+failure:0;
            bool updated=garb_cascade_update(c);fail_at=0;fail_single=false;
            if(!failure){points=allocations-before;update_check(updated,"unfailed update");}
            if(updated)update_equal(c,doc,&in);
            garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
            update_check(live==baseline,"allocation refusal releases ownership");
        }
        printf("libgarb updates: %s sweep, %zu allocation positions\n",kSweepNames[growth],points);
    }
    printf("libgarb updates: %u checks, %u failed\n",update_checks,update_failures);
    return update_failures?1:0;
}
