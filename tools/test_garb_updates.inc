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
// The caller's clock for the measure's cases: each read a tick later, so
// every part of a build is at least a tick.
static uint64_t measure_clock(void)
{
    static uint64_t ticks;
    return ++ticks;
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
    /* Anchors filtered by the compound around them (select.h § AN ANCHORED
     * :has()): inside :not(), :is() and :where(); a descendant, a child and
     * a sibling argument; a state inside the argument, which libgarb
     * answers false (P_USER_ACTION), so the :not() always holds. Each case
     * tracks, and each turn restyles what the anchor reaches. */
    has_open(&h,".c:not(:has(.x)) > .y{color:red}"
                ".d:is(.e, :has(> .x)) .y{color:blue}"
                "section:where(:has(.x)) ~ .z{opacity:.5}"
                ".f:not(:hover, :has(:focus-visible)) .y{background:green}",
             "<!doctype html><div id=c class=c><p id=cy class=y>y</p><p id=cw>w</p></div>"
             "<div id=d class=d><p id=dy class=y>y</p></div>"
             "<div><section id=wd><i id=wi></i></section><p id=z class=z>z</p></div>"
             "<div id=f class=f><p id=fy class=y>y</p></div>");
    {
        int32_t anchors=0,bare=-1;
        garb_cascade_anchors(h.c,&anchors,&bare);
        update_check(anchors==4 && bare==0,"enclosing filters: four anchors, none bare");
    }
    os64_html_set_attr(h.doc,has_el(&h,"cw"),"class","x",1);
    was=has_update(&h,"inside :not(): the argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"cy"),was),"inside :not(): what the anchor reaches is restyled");
    update_check(!garb_style_updated_since(h.c,has_el(&h,"dy"),was),"inside :not(): another anchor's subtree is not");
    os64_html_node_t *dx=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"b",NULL);
    os64_html_set_attr(h.doc,dx,"class","x",1);
    os64_html_insert(h.doc,has_el(&h,"d"),dx,NULL);
    was=has_update(&h,"inside :is() with >: a child argument inserted updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"dy"),was),"inside :is() with >: what it reaches is restyled");
    os64_html_set_attr(h.doc,has_el(&h,"wi"),"class","x",1);
    was=has_update(&h,"inside :where() with ~: the argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"z"),was),"inside :where() with ~: the sibling is restyled");
    os64_html_set_attr(h.doc,has_el(&h,"fy"),"title","t",1);
    has_update(&h,"a state inside :not(:has()): a change updates incrementally");
    os64_html_set_attr(h.doc,has_el(&h,"c"),"class","",0);
    was=has_update(&h,"an enclosing filter going updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"cy"),was),"an enclosing filter going restyles what it reached");
    has_close(&h);
    /* A compound left of a combinator in the anchor's own complex names
     * another element, a pseudo-class is no filter, and a sibling branch of
     * a list is none either: each is bare, and the cascade still tracks. */
    {
        const char *bare_css[]={":is(.a :has(.x)) .z{color:red}",":not(.a):has(.x) i{color:red}",
                                ":is(.x, :has(p)) i{color:red}"};
        for(size_t i=0;i<3;++i) {
            os64_html_document_t *doc=update_doc("<!doctype html><div class=a><p>t</p></div>");
            garb_parsed_t sheet;garb_parse_sheet_text(bare_css[i],strlen(bare_css[i]),&sheet);
            garb_sheet_in_t in={.sheet=&sheet};
            garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
            int32_t anchors=0,bare=0;
            garb_cascade_anchors(c,&anchors,&bare);
            update_check(anchors==1 && bare==1 && garb_cascade_tracks_changes(c),
                         "an ancestor's compound, a :not() or a sibling branch is no filter: bare, and tracked");
            garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
        }
    }
    /* A bare anchor pays for itself: asked again where a change could turn
     * it, its reach restyled where it turned and nowhere else, and what it
     * cost counted. A descendant, a child and a sibling argument, and one
     * inside :not(). */
    has_open(&h,":has(> .x) > .y{color:red}:has(.deep) .z{color:blue}:has(+ .s) ~ .w{opacity:.5}"
                ":not(:has(.n)) > .m{background:green}",
             "<!doctype html><div id=c><p id=cy class=y>y</p><p id=cw>w</p></div>"
             "<div id=d><section><p id=dp>p</p></section><i id=dz class=z>z</i></div>"
             "<div><p id=sp>p</p><p id=sq>q</p><p id=sw class=w>w</p></div>"
             "<div id=n><p id=nm class=m>m</p><p id=nn>n</p></div>"
             "<div id=far><p id=fy class=y>y</p><b class=deep></b><i class=n></i></div>");
    /* (The page holds a .deep and a .n from the start, so the root's
     * ancestors already answer the descendant arguments yes: one arriving
     * turns the anchors between it and them, not the root, whose reach is
     * most of the page and would be a fresh cascade, rightly.) */
    {
        int32_t anchors=0,bare=0;
        garb_cascade_anchors(h.c,&anchors,&bare);
        update_check(anchors==4 && bare==4,"bare anchors: four, all bare, and the cascade tracks");
    }
    os64_html_set_attr(h.doc,has_el(&h,"cw"),"class","x",1);
    was=has_update(&h,"bare :has(> …): a child argument arriving updates incrementally");
    garb_bare_work_t work=garb_cascade_bare_work(h.c);
    update_check(garb_style_updated_since(h.c,has_el(&h,"cy"),was) && !garb_style_updated_since(h.c,has_el(&h,"fy"),was),
                 "bare :has(> …): the turned anchor's reach is restyled, another subtree's is not");
    update_check(work.tested>0 && work.turned>=1 && work.marked>=1,"bare :has(> …): what it cost is counted");
    os64_html_set_attr(h.doc,has_el(&h,"dp"),"class","deep",4);
    was=has_update(&h,"bare :has(…): a descendant argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"dz"),was),"bare :has(…): what it reaches is restyled");
    os64_html_set_attr(h.doc,has_el(&h,"sq"),"class","s",1);
    was=has_update(&h,"bare :has(+ …): a sibling argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"sw"),was),"bare :has(+ …): the sibling after is restyled");
    os64_html_set_attr(h.doc,has_el(&h,"nn"),"class","n",1);
    was=has_update(&h,"bare :not(:has(…)): the argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"nm"),was),"bare :not(:has(…)): what it reached is restyled");
    /* Fifty changes in one batch under one deep chain: each element is
     * visited once an update, not once a change, so the asks are the
     * elements visited (fifty leaves, the chain above them and, since a
     * `+` argument is in the sheet, the chain's previous siblings: about
     * seventy), not fifty times the chain (thousands). */
    {
        os64_html_node_t *at=has_el(&h,"far");
        for(int i=0;i<10;++i) {
            os64_html_node_t *d=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"div",NULL);
            os64_html_insert(h.doc,at,d,NULL);
            at=d;
        }
        os64_html_node_t *leaves[50];
        for(int i=0;i<50;++i) {
            leaves[i]=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"u",NULL);
            os64_html_insert(h.doc,at,leaves[i],NULL);
        }
        has_update(&h,"bare anchors: a deep chain added updates incrementally");
        for(int i=0;i<50;++i) os64_html_set_attr(h.doc,leaves[i],"title","t",1);
        has_update(&h,"bare anchors: fifty changes under one chain update incrementally");
        work=garb_cascade_bare_work(h.c);
        update_check(work.tested>=4u*50 && work.tested<=4u*80,
                     "bare anchors: each element visited once an update, not once a change");
        if(work.tested>4u*80) fprintf(stderr,"  asked %llu times\n",(unsigned long long)work.tested);
    }
    os64_html_set_attr(h.doc,has_el(&h,"fy"),"title","t",1);
    was=has_update(&h,"bare anchors: a change that turns none updates incrementally");
    work=garb_cascade_bare_work(h.c);
    update_check(work.tested>0 && work.turned==0 && work.marked==0 &&
                 !garb_style_updated_since(h.c,has_el(&h,"cy"),was),
                 "bare anchors: asked again, nothing turned, nothing marked for them");
    has_close(&h);
    /* More anchors than one word of bits: the 70th (bit 5 of the second
     * word) turns at an element where the 6th (bit 5 of the first) holds,
     * and its reach is restyled; the cascade tracks. */
    {
        char css[8192]; size_t len=0;
        for(int i=0;i<70;++i) len+=(size_t)sprintf(css+len,".g%d:has(.x%d) .y{z-index:%d}",i,i,i);
        has_open(&h,css,"<!doctype html><div id=g class='g5 g69'><i class=x5></i><p id=y class=y>y</p><p id=w>w</p></div>"
                        "<div id=o class=g3><p id=oy class=y>y</p></div>");
        int32_t anchors=0,bare=-1;
        garb_cascade_anchors(h.c,&anchors,&bare);
        update_check(anchors==70 && bare==0,"seventy anchors, none bare");
        os64_html_set_attr(h.doc,has_el(&h,"w"),"class","x69",3);
        was=has_update(&h,"seventy anchors: the seventieth turning updates incrementally");
        update_check(garb_style_updated_since(h.c,has_el(&h,"y"),was) && !garb_style_updated_since(h.c,has_el(&h,"oy"),was),
                     "seventy anchors: its reach is restyled, another anchor's is not");
        has_close(&h);
    }
    /* An anchor's bits survive the record table growing: the table is
     * sized at the build, a few additions past half of it grow it, and the
     * anchor's answer turning back afterwards is still seen. */
    {
        char html[16384]="<!doctype html><aside id=aside>";
        for(int i=0;i<493;++i) strcat(html,"<i></i>");
        strcat(html,"</aside><div id=g class=g><p id=y class=y>y</p><i id=x class=x></i></div>");
        HasCase big;
        big.doc=update_doc(html);
        const char *css=".g:has(.x) .y{color:red}";
        garb_parse_sheet_text(css,strlen(css),&big.sheet);
        big.in=(garb_sheet_in_t){.sheet=&big.sheet};
        big.c=garb_cascade(&big.in,1,big.doc,(garb_env_t){.width=800,.height=600});
        for(int i=0;i<40;++i)
            os64_html_insert(big.doc,has_el(&big,"aside"),os64_html_create_element(big.doc,OS64_HTML_NS_HTML,"b",NULL),NULL);
        has_update(&big,"growing the record table updates incrementally");
        os64_html_remove(big.doc,has_el(&big,"x"));
        was=has_update(&big,"an anchor's argument removed after the table grew updates incrementally");
        update_check(garb_style_updated_since(big.c,has_el(&big,"y"),was),"and the anchor's reach is restyled");
        has_close(&big);
    }
    /* An anchor's bits survive removals around it: many records removed
     * at once shift others back in the table, and thirty anchors whose
     * answers then turn are each seen. */
    {
        char html[16384]="<!doctype html><aside id=aside>";
        for(int i=0;i<100;++i) strcat(html,"<b></b>");
        strcat(html,"</aside>");
        for(int i=0;i<30;++i) {
            char one[160];
            sprintf(one,"<div class=g><p id=y%d class=y>y</p><i id=x%d class=x></i></div>",i,i);
            strcat(html,one);
        }
        has_open(&h,".g:has(.x) .y{color:red}",html);
        while(has_el(&h,"aside")->first_child) os64_html_remove(h.doc,has_el(&h,"aside")->first_child);
        has_update(&h,"a hundred records removed updates incrementally");
        for(int i=0;i<30;++i) { char id[16]; sprintf(id,"x%d",i); os64_html_remove(h.doc,has_el(&h,id)); }
        was=has_update(&h,"thirty anchors turning after the removals updates incrementally");
        bool all=true;
        for(int i=0;i<30;++i) { char id[16]; sprintf(id,"y%d",i); all&=garb_style_updated_since(h.c,has_el(&h,id),was); }
        update_check(all,"and each anchor's reach is restyled");
        has_close(&h);
    }
    /* An anchor left of a combinator inside an :is(): the compound around
     * the :is() names another element, so it is no filter of the anchor's,
     * and the anchor turning where that compound does not match is seen. */
    has_open(&h,".k:is(.f:has(.x) > .d) .z{color:red}",
             "<!doctype html><div id=f class=f><p class='d k'><i id=z class=z>z</i></p><b id=b></b></div>");
    os64_html_set_attr(h.doc,has_el(&h,"b"),"class","x",1);
    was=has_update(&h,"an anchor inside :is(), not its subject: the argument arriving updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"z"),was),"and what it reaches is restyled");
    has_close(&h);
    /* An anchor moved with its argument inside: the move drops its record
     * and makes a new one, whose bits are first asked when it is styled, and
     * the next change under it is the first to compare them, so what was
     * stored then must be its answer. (Building it detached would not do:
     * each insert under it is logged, and the update visits it.) */
    has_open(&h,".g:has(.x) .y{color:red}",
             "<!doctype html><div id=host></div><div id=g class=g><p id=y class=y>y</p><i id=x class=x></i></div>");
    os64_html_insert(h.doc,has_el(&h,"host"),has_el(&h,"g"),NULL);
    has_update(&h,"an anchor moved with its argument updates incrementally");
    os64_html_remove(h.doc,has_el(&h,"x"));
    was=has_update(&h,"its argument then removed updates incrementally");
    update_check(garb_style_updated_since(h.c,has_el(&h,"y"),was),"and what the anchor reached is restyled back");
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
    /* A turn that changes a custom property every element under it reads
     * recomputes them (THE RECOMPUTE): no matching, nothing counted toward
     * the half, so no fresh cascade, and each one's winner changed. */
    {
        char html[8192]="<!doctype html><div id=a class=a>";
        for(int i=0;i<260;++i) strcat(html,"<p>x</p>");
        strcat(html,"<i id=i></i></div>");
        has_open(&h,".a:has(.b){--ink:red}p{color:var(--ink)}",html);
        os64_html_set_attr(h.doc,has_el(&h,"i"),"class","b",1);
        bool ok=garb_cascade_update(h.c);
        garb_update_marks_t mk=garb_cascade_update_marks(h.c);
        update_check(ok && mk.recomputed==260 && mk.recompute_changed==260 && mk.marked<=2 && mk.inherited==0,
                     "a custom property a :has() turn changed is recomputed below it, no fresh cascade");
        update_equal(h.c,h.doc,&h.in);
        has_close(&h);
    }
    /* A turn whose marks pass half the elements asks for a fresh cascade,
     * says why, and the fresh one is right. */
    {
        char html[8192]="<!doctype html><div id=a class=a>";
        for(int i=0;i<260;++i) strcat(html,"<p>x</p>");
        strcat(html,"<i id=i></i></div>");
        has_open(&h,".a:has(.b) p{color:red}",html);
        os64_html_set_attr(h.doc,has_el(&h,"i"),"class","b",1);
        update_check(!garb_cascade_update(h.c) && garb_cascade_update_refusal(h.c)==GARB_UPDATE_BROAD,
                     "a broad :has() turn asks for a fresh cascade, and says it was broad");
        update_check(update_or_rebuild(&h.c,h.doc,&h.in),"and the fresh cascade builds");
        update_equal(h.c,h.doc,&h.in);
        has_close(&h);
    }
    /* THE CALLER'S MEASURE (garb_cascade_limits_t.clock): a cascade made
     * with a clock says what its fresh build cost by part, and a broad
     * refusal names the change that marked the most, by which mark; one
     * made without a clock says nothing of either. */
    {
        char html[8192]="<!doctype html><div id=a class=a>";
        for(int i=0;i<260;++i) strcat(html,"<p>x</p>");
        strcat(html,"<i id=i></i></div><b id=b></b>");
        const char *css=".a:has(.b) p{color:red}";
        garb_cascade_limits_t limits={GARB_ARENA_MAX,GARB_CASCADE_READ_MAX,GARB_ARENA_MAX,SIZE_MAX,measure_clock,NULL,false};
        for(int clocked=1;clocked>=0;--clocked){
            HasCase m={0};
            m.doc=update_doc(html);
            garb_parse_sheet_text(css,strlen(css),&m.sheet);
            m.in=(garb_sheet_in_t){.sheet=&m.sheet};
            m.c=garb_cascade_scoped(&m.in,1,NULL,0,m.doc,(garb_env_t){.width=800,.height=600},clocked?&limits:NULL);
            update_check(m.c && garb_cascade_tracks_changes(m.c),"measure: the cascade tracks changes");
            garb_build_cost_t k=garb_cascade_build_cost(m.c);
            garb_broad_trigger_t t;
            if(clocked)
                update_check(k.rules==1 && k.elements>=263 && k.sheets>0 && k.table>0 && k.matching>0,
                             "measure: a clocked build says its parts, its rules and its elements");
            else
                update_check(k.rules==0 && k.elements==0 && k.sheets==0 && k.matching==0,
                             "measure: a build with no clock says nothing");
            os64_html_set_attr(m.doc,has_el(&m,"b"),"title","t",1);
            update_check(garb_cascade_update(m.c) && !garb_cascade_broad_trigger(m.c,&t),
                         "measure: an update that went ahead names no trigger");
            os64_html_set_attr(m.doc,has_el(&m,"i"),"class","b",1);
            bool refused=!garb_cascade_update(m.c) && garb_cascade_update_refusal(m.c)==GARB_UPDATE_BROAD;
            bool named=garb_cascade_broad_trigger(m.c,&t);
            if(clocked)
                // The :has() turned at #a, anchored: what it reaches, its
                // descendants, more than half the page; the batch's change
                // that began it is the class at i#i.
                update_check(refused && named && t.kind==OS64_HTML_CHANGE_ATTR && !strcmp(t.attr,"class") &&
                             !strcmp(t.target,"i#i.b") && t.depth==4 && t.marked>0 &&
                             t.by[GARB_MARK_CLASS]+t.by[GARB_MARK_HAS]==t.marked && t.changes==1 &&
                             t.marked_changes==1 && t.dirty>t.slots/2,
                             "measure: a broad refusal names its change and the marks");
            else
                update_check(refused && !named,"measure: a broad refusal with no clock names nothing");
            // The trigger is the latest update's alone: the next ask, refused
            // before it marks anything, names none.
            update_check(!garb_cascade_update(m.c) && !garb_cascade_broad_trigger(m.c,&t),
                         "measure: an update refused after a broad one names no trigger");
            has_close(&m);
        }
        update_check(!strcmp(garb_mark_name(GARB_MARK_HAS),":has()") && !strcmp(garb_mark_name(GARB_MARKS),"?"),
                     "measure: the marks' names");
    }
    update_check(live==baseline,":has() updates release what they made");
}

/* WHAT AN INLINE STYLE WRITE CHANGED (garb_cascade_inline_changes), asked
 * before the update, and whether a selector reads the `style` attribute. */
static bool inline_listed(const garb_inline_change_t *out, int32_t n, garb_prop_t prop, garb_inline_change_t *got)
{
    for(int32_t i=0;i<n;i++) if(out[i].prop==prop){ if(got) *got=out[i]; return true; }
    return false;
}
static void inline_set(HasCase *h, os64_html_node_t *el, const char *style)
{
    os64_html_set_attr(h->doc,el,"style",style,strlen(style));
}
static void update_inline_changes(void)
{
    size_t baseline=live;
    HasCase h;
    has_open(&h,"p{margin:0}","<!doctype html><p id=p style='color:red;transform:translateX(5px)'>x</p><p id=q>y</p>");
    os64_html_node_t *p=has_el(&h,"p"),*q=has_el(&h,"q");
    garb_inline_change_t out[64],got;
    int32_t n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==0,"inline: the attribute as parsed lists nothing");
    os64_html_set_attr(h.doc,p,"style","color:blue;transform:translateX(5px)",36);
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_COLOR,&got) && got.declared && !strcmp(got.keyword,"blue") && !got.var,
                 "inline: a colour changed is listed alone");
    os64_html_set_attr(h.doc,p,"style","color:red;transform:none",24);
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_TRANSFORM,&got) && got.declared && !strcmp(got.keyword,"none"),
                 "inline: a transform to none says none");
    inline_set(&h,p,"color:red;transform:translateX(5px);visibility:COLLAPSE");
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_VISIBILITY,&got) && !strcmp(got.keyword,"collapse"),
                 "inline: a keyword is given lowercased");
    os64_html_set_attr(h.doc,p,"style","color:red",9);
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_TRANSFORM,&got) && !got.declared,
                 "inline: a declaration removed is listed as no longer declared");
    os64_html_set_attr(h.doc,p,"style","color:red!important;transform:translateX(5px)",45);
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_COLOR,NULL),"inline: importance is a change");
    inline_set(&h,p,"color:red;transform:translateX(6px)");
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_TRANSFORM,NULL),"inline: a change inside a function's arguments is listed");
    os64_html_set_attr(h.doc,p,"style","color:red;transform:var(--t)",28);
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_TRANSFORM,&got) && got.var,"inline: var() says so");
    os64_html_set_attr(h.doc,p,"style","color:red;transform:translateX(5px);--t:1px",43);
    update_check(garb_cascade_inline_changes(h.c,p,out,64)==-1,"inline: a custom property cannot be said");
    os64_html_set_attr(h.doc,p,"style","color:red;transform:translateX(5px);background:blue",51);
    n=garb_cascade_inline_changes(h.c,p,out,64);
    update_check(n>1 && inline_listed(out,n,GARB_BACKGROUND_COLOR,NULL) && !inline_listed(out,n,GARB_COLOR,NULL),
                 "inline: a shorthand lists each longhand it sets");
    update_check(garb_cascade_inline_changes(h.c,p,out,1)==-1,"inline: more than the cap cannot be said");
    os64_html_set_attr(h.doc,p,"style","color:red;transform:translateX(5px)",35);
    update_check(garb_cascade_inline_changes(h.c,p,out,64)==0,"inline: written back as parsed lists nothing");
    os64_html_set_attr(h.doc,q,"style","width:10px",10);
    n=garb_cascade_inline_changes(h.c,q,out,64);
    update_check(n==1 && inline_listed(out,n,GARB_WIDTH,&got) && got.declared,"inline: a style where there was none");
    has_update(&h,"inline: the update for it");
    update_check(garb_cascade_inline_changes(h.c,q,out,64)==0,"inline: after the update, its parse is the baseline");
    int64_t why=0;
    os64_html_node_t *loose=os64_html_create_element(h.doc,OS64_HTML_NS_HTML,"p",&why);
    update_check(loose && garb_cascade_inline_changes(h.c,loose,out,64)==-1,"inline: an element never styled cannot be said");
    update_check(!garb_cascade_reads_style_attribute(h.c),"inline: no selector reads the style attribute");
    has_close(&h);
    const char *readers[]={"[style*=x]{color:red}","p:is([STYLE]){color:red}","div:has(> [style]){color:red}",
                           "p::after{content:attr(style)}","p::before{content:\"(\" ATTR( style ) \")\"}"};
    for(size_t i=0;i<sizeof(readers)/sizeof(*readers);i++){
        has_open(&h,readers[i],"<!doctype html><div><p style=color:red>x</p></div>");
        update_check(garb_cascade_reads_style_attribute(h.c),"inline: a selector that reads the style attribute is known");
        has_close(&h);
    }
    update_check(live==baseline,"inline: nothing retained");
}

/* THE KEPT READ (KEPT_READ.md): a build offered a read with an equal key
 * takes it, matches as one that read the sheets again, and the read
 * outlives the cascade it came from; each key member different reads again and
 * names itself; a sheet parsed again has a new serial; and a read taken is
 * never written, not even by an inline style naming a property the
 * sheets' own skipped records name (whose count the cascade keeps). The
 * quirks member is not reached here: it is compared only for one
 * document, whose mode its doctype fixed. */
static garb_cascade_t *kept_build(garb_sheet_in_t *in, os64_html_document_t *doc, garb_env_t env,
                                  const garb_scope_in_t *scopes, int32_t nscopes, const garb_cascade_t *from,
                                  size_t read_bytes)
{
    garb_read_t *held=garb_cascade_read_hold(from);
    garb_cascade_limits_t l={GARB_ARENA_MAX,read_bytes,GARB_ARENA_MAX,SIZE_MAX,NULL,held,true};
    garb_cascade_t *c=garb_cascade_scoped(in,1,scopes,nscopes,doc,env,&l);
    garb_read_release(held);
    return c;
}
static void update_kept_read(void)
{
    size_t baseline=live;
    const char *css=".on p{color:red}p{bogus-prop:1;margin:2px}@keyframes k{from{opacity:0}to{opacity:1}}";
    garb_parsed_t twice;
    garb_parse_sheet_text(css,strlen(css),&twice);
    uint64_t first=twice.serial;
    garb_free(&twice);
    garb_parse_sheet_text(css,strlen(css),&twice);
    update_check(first!=0 && twice.serial!=0 && twice.serial!=first,"kept read: a sheet parsed again into one struct has a new serial");
    garb_free(&twice);
    os64_html_document_t *doc=update_doc("<!doctype html><div id=box class=on><p id=a style='bogus-prop:2'>a</p><p>b</p></div>");
    os64_html_document_t *other=update_doc("<!doctype html><div id=box class=on><p>a</p></div>");
    garb_parsed_t sheet,same;
    garb_parse_sheet_text(css,strlen(css),&sheet);
    garb_parse_sheet_text(css,strlen(css),&same);
    // `moved` is `sheet` copied: the same parse, serial and all, at another
    // address (never freed: the arena is `sheet`'s).
    garb_parsed_t moved=sheet;
    garb_sheet_in_t in={.sheet=&sheet},in_same={.sheet=&same},in_moved={.sheet=&moved};
    garb_env_t env={.width=800,.height=600},wide={.width=900,.height=600};
    garb_cascade_t *a=kept_build(&in,doc,env,NULL,0,NULL,GARB_CASCADE_READ_MAX);
    update_check(a && garb_cascade_read_refusal(a)==GARB_READ_NOT_OFFERED,"kept read: a build offered none reads the sheets");
    garb_cascade_t *b=kept_build(&in,doc,env,NULL,0,a,GARB_CASCADE_READ_MAX);
    update_check(b && garb_cascade_read_refusal(b)==GARB_READ_KEPT && !garb_cascade_read_written(b),
                 "kept read: an equal key takes the read, unwritten though the giver matched an inline style");
    update_check(garb_cascade_read_content(a)==garb_cascade_read_content(b),"kept read: one read, one content");
    garb_read_t *outlives=garb_cascade_read_hold(a);
    garb_cascade_free(a);
    garb_cascade_limits_t held_limits={GARB_ARENA_MAX,GARB_CASCADE_READ_MAX,GARB_ARENA_MAX,SIZE_MAX,NULL,outlives,true};
    garb_cascade_t *after=garb_cascade_scoped(&in,1,NULL,0,doc,env,&held_limits);
    garb_read_release(outlives);
    update_check(after && garb_cascade_read_refusal(after)==GARB_READ_KEPT && !garb_cascade_read_written(after),
                 "kept read: a read held past its cascade is taken whole");
    garb_cascade_free(after);
    update_equal(b,doc,&in);
    // Each member of the key, different: read again, and named.
    struct { garb_cascade_t *c; garb_read_refusal_t want; const char *what; } cases[] = {
        {kept_build(&in,doc,wide,NULL,0,b,GARB_CASCADE_READ_MAX),GARB_READ_ENVIRONMENT,"kept read: another viewport reads again"},
        {kept_build(&in_same,doc,env,NULL,0,b,GARB_CASCADE_READ_MAX),GARB_READ_SHEETS,"kept read: the same text parsed again reads again"},
        {kept_build(&in_moved,doc,env,NULL,0,b,GARB_CASCADE_READ_MAX),GARB_READ_SHEETS,"kept read: the same parse at another address reads again"},
        {kept_build(&in,doc,env,NULL,0,b,GARB_CASCADE_READ_MAX/2),GARB_READ_LIMITS,"kept read: other limits read again"},
        {kept_build(&in,other,env,NULL,0,b,GARB_CASCADE_READ_MAX),GARB_READ_DOCUMENT,"kept read: another document reads again"},
        {kept_build(&in,doc,env,&(garb_scope_in_t){.root=by_id(doc->document,"box"),.sheets=(int32_t[]){0},.n=1},1,b,
                    GARB_CASCADE_READ_MAX),GARB_READ_SCOPES,"kept read: other scopes read again"},
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        update_check(cases[i].c && garb_cascade_read_refusal(cases[i].c)==cases[i].want,cases[i].what);
        garb_cascade_free(cases[i].c);
    }
    // A read that came up short is never taken.
    garb_cascade_t *shortread=kept_build(&in,doc,env,NULL,0,NULL,64);
    garb_cascade_t *offered=kept_build(&in,doc,env,NULL,0,shortread,64);
    update_check(offered && garb_cascade_read_refusal(offered)==GARB_READ_NOT_KEEPABLE,"kept read: a short read is not taken");
    garb_cascade_free(shortread); garb_cascade_free(offered);
    // Two cascades on one read, updated in turn: each its own marks.
    garb_cascade_t *c=kept_build(&in,doc,env,NULL,0,b,GARB_CASCADE_READ_MAX);
    update_check(c && garb_cascade_read_refusal(c)==GARB_READ_KEPT,"kept read: a third cascade takes it too");
    os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"a"),"style","bogus-prop:3",12);
    update_check(garb_cascade_update(b),"kept read: the first updates, on the shared read");
    update_equal(b,doc,&in);
    update_check(garb_cascade_update(c),"kept read: the second updates, on the shared read");
    update_equal(c,doc,&in);
    garb_cascade_t *d=kept_build(&in,doc,env,NULL,0,c,GARB_CASCADE_READ_MAX);
    update_check(d && garb_cascade_read_refusal(d)==GARB_READ_KEPT && !garb_cascade_read_written(d),
                 "kept read: an inline style's skipped property, counted after, leaves the read unwritten");
    garb_cascade_free(b); garb_cascade_free(c);
    // The same struct parsed again: one address, a new parse.
    garb_free(&sheet);
    garb_parse_sheet_text(css,strlen(css),&sheet);
    garb_cascade_t *e=kept_build(&in,doc,env,NULL,0,d,GARB_CASCADE_READ_MAX);
    update_check(e && garb_cascade_read_refusal(e)==GARB_READ_SHEETS,
                 "kept read: a sheet parsed again into the same struct reads again");
    garb_cascade_free(d); garb_cascade_free(e);
    garb_free(&sheet); garb_free(&same);
    os64_html_document_free(doc); os64_html_document_free(other);
    update_check(live==baseline,"kept read: nothing retained");
}

// The skipped records copied out of a read hold exactly what the read held,
// so their growth stops at GARB_SKIPS_MAX, not at the first doubling past
// it: 150 names from the sheets, then a new one from a style attribute,
// kept, in the cascade that made the read and in one that took it.
static void update_kept_skips(void)
{
    size_t baseline=live;
    char css[8192]; size_t at=0;
    for(int i=0;i<150;i++) at+=(size_t)snprintf(css+at,sizeof(css)-at,"p{zzprop%d:1}",i);
    os64_html_document_t *doc=update_doc("<!doctype html><p style='zznew:1'>a</p>");
    garb_parsed_t sheet; garb_parse_sheet_text(css,at,&sheet);
    garb_sheet_in_t in={.sheet=&sheet};
    garb_env_t env={.width=800,.height=600};
    garb_cascade_t *maker=kept_build(&in,doc,env,NULL,0,NULL,GARB_CASCADE_READ_MAX);
    garb_cascade_t *taker=kept_build(&in,doc,env,NULL,0,maker,GARB_CASCADE_READ_MAX);
    garb_cascade_t *both[2]={maker,taker};
    for(int k=0;k<2;k++) {
        uint32_t lost=99;
        int32_t n=both[k] ? garb_cascade_skips(both[k],NULL,0,&lost) : -1;
        update_check(n==151 && lost==0,k==0 ? "kept skips: the maker keeps a 151st name"
                                             : "kept skips: the taker keeps a 151st name");
    }
    update_check(taker && garb_cascade_read_refusal(taker)==GARB_READ_KEPT,"kept skips: the taker took the read");
    garb_cascade_free(maker); garb_cascade_free(taker);
    garb_free(&sheet); os64_html_document_free(doc);
    update_check(live==baseline,"kept skips: nothing retained");
}

// :EMPTY, ASKED AGAIN: a change of children or of a text's data asks the
// parent's :empty again and marks for it only where the answer turned: the
// parent alone, and the siblings after it a `+`/`~` reads. An append to a
// parent that is not empty takes an update however much follows the parent
// (before, its subtree and every following sibling's were marked, and the
// 61 elements after #box crossed half the page). `garb_style_updated_since`
// is true of every element a subtree's mark restyles, so "not updated" is
// "not marked by a subtree's mark" here (an element marked alone whose
// winners come out the same reads not updated). Every step's winners equal
// a fresh cascade's.
static os64_html_node_t *ue_append(os64_html_document_t *doc, os64_html_node_t *parent, const char *tag)
{
    os64_html_node_t *x=os64_html_create_element(doc,OS64_HTML_NS_HTML,tag,NULL);
    if(x)os64_html_insert(doc,parent,x,NULL);
    return x;
}
static void update_empty_turns(void)
{
    size_t baseline=live;
    char html[8192]="<!doctype html><aside>";
    for(int i=0;i<50;++i)strcat(html,"<i></i>");
    strcat(html,"</aside><div id=box><p id=full><b id=kid>k</b></p><p id=blank></p>"
                "<p id=after><i></i></p><p id=far><i></i></p></div><section id=big>");
    for(int i=0;i<60;++i)strcat(html,"<i></i>");
    strcat(html,"</section>");
    const char *css="p:empty{color:red}p:empty + p{color:green}p:empty ~ p i{opacity:.5}section i{z-index:1}";
    os64_html_document_t *doc=update_doc(html);
    garb_parsed_t sheet;garb_parse_sheet_text(css,strlen(css),&sheet);
    garb_sheet_in_t in={.sheet=&sheet};
    garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    os64_html_node_t *box=(void *)by_id(doc->document,"box"),*full=(void *)by_id(doc->document,"full"),
        *kid=(void *)by_id(doc->document,"kid"),*blank=(void *)by_id(doc->document,"blank"),
        *after=(void *)by_id(doc->document,"after"),*far=(void *)by_id(doc->document,"far"),
        *big=(void *)by_id(doc->document,"big");
    uint64_t u=garb_cascade_updates(c);
    ue_append(doc,box,"em");
    update_check(garb_cascade_update(c),":empty: an append to a parent that is not empty updates");
    update_equal(c,doc,&in);
    update_check(!garb_style_updated_since(c,full,u) && !garb_style_updated_since(c,big->first_child,u),
                 ":empty: an append to a parent that is not empty marks neither its children nor what follows it");
    u=garb_cascade_updates(c);
    ue_append(doc,full,"em");
    update_check(garb_cascade_update(c) && !garb_style_updated_since(c,kid,u) &&
                 !garb_style_updated_since(c,after,u),":empty: an append to a paragraph that is not empty marks nothing for :empty");
    update_equal(c,doc,&in);
    // Turned, each way: the siblings after it are marked (`+` and `~`).
    u=garb_cascade_updates(c);
    os64_html_node_t *em=ue_append(doc,blank,"em");
    update_check(garb_cascade_update(c) && garb_style_updated_since(c,after,u) &&
                 garb_style_updated_since(c,far->first_child,u),":empty: a turn to not empty marks the siblings after it");
    update_equal(c,doc,&in);
    u=garb_cascade_updates(c);
    os64_html_remove(doc,em);
    update_check(garb_cascade_update(c) && garb_style_updated_since(c,far,u),":empty: a turn to empty marks the siblings after it");
    update_equal(c,doc,&in);
    // A text child: one with no characters turns nothing; its data turns it.
    u=garb_cascade_updates(c);
    os64_html_node_t *text=os64_html_create_text(doc,"",0,NULL);
    os64_html_insert(doc,blank,text,NULL);
    update_check(garb_cascade_update(c) && !garb_style_updated_since(c,after,u),":empty: a text child with no characters turns nothing");
    update_equal(c,doc,&in);
    u=garb_cascade_updates(c);
    os64_html_set_text(doc,text,"x",1);
    update_check(garb_cascade_update(c) && garb_style_updated_since(c,after,u),":empty: a text's data turns its parent");
    update_equal(c,doc,&in);
    u=garb_cascade_updates(c);
    os64_html_set_text(doc,text,"",0);
    update_check(garb_cascade_update(c) && garb_style_updated_since(c,far,u),":empty: a text emptied turns its parent back");
    update_equal(c,doc,&in);
    u=garb_cascade_updates(c);
    os64_html_set_text(doc,kid->first_child,"kk",2);
    update_check(garb_cascade_update(c) && !garb_style_updated_since(c,kid,u),":empty: a text whose parent stays filled marks nothing");
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);
    // Rules with no :empty: a text's data marks nothing at all.
    css="p{color:red}b{color:blue}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    u=garb_cascade_updates(c);
    os64_html_set_text(doc,kid->first_child,"zz",2);
    update_check(garb_cascade_update(c) && !garb_style_updated_since(c,kid,u),":empty: with no :empty in the rules a text's data marks nothing");
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // <details> and <fieldset> keep their children's whole mark, :empty or
    // not: libflow's first-summary and first-legend rules read child order,
    // and learn which children to compute again from these marks.
    doc=update_doc("<!doctype html><aside><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i></aside>"
                   "<details id=d><summary id=s1>a</summary><p>x</p></details>"
                   "<fieldset id=f><legend id=l1>a</legend><p>x</p></fieldset>");
    const char *sheets[]={"p{color:red}b{color:blue}","p:empty{color:red}b{color:blue}"};
    const char *parents[]={"d","f"},*tags[]={"summary","legend"};
    for(int r=0;r<2;r++) {
        garb_parse_sheet_text(sheets[r],strlen(sheets[r]),&sheet);
        c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        for(int k=0;k<2;k++) {
            os64_html_node_t *parent=(void *)by_id(doc->document,parents[k]),*first=parent->first_child;
            os64_html_node_t *ahead=os64_html_create_element(doc,OS64_HTML_NS_HTML,tags[k],NULL);
            u=garb_cascade_updates(c);
            os64_html_insert(doc,parent,ahead,first);
            update_check(garb_cascade_update(c) && garb_style_updated_since(c,first,u),
                         k==0 ? (r==0 ? "details: a summary put first marks the one that was first"
                                      : "details: with :empty in the rules, a summary put first marks the one that was first")
                              : (r==0 ? "fieldset: a legend put first marks the one that was first"
                                      : "fieldset: with :empty in the rules, a legend put first marks the one that was first"));
        }
        garb_cascade_free(c);garb_free(&sheet);
    }
    os64_html_document_free(doc);
    update_check(live==baseline,":empty: the fixture releases what it holds");
}

// THE BACK INDEXES, COUNTED AGAIN (cascade.c index_walk): a change of
// children marks only the children whose count from the end, saturated at
// the rules' bound, turned. Each case's #box holds 40 paragraphs, each with
// a span; p20 sits in the middle, where no bounded back form reaches, so
// it is never marked (before, every child of #box was). Every step's
// winners equal a fresh cascade's.
static os64_html_document_t *bi_doc(void)
{
    char html[16384]="<!doctype html><aside>";
    for(int i=0;i<100;++i)strcat(html,"<i></i>");
    strcat(html,"</aside><div id=box>");
    for(int i=0;i<40;++i){char p[64];snprintf(p,sizeof(p),"<p id=p%d><span></span></p>",i);strcat(html,p);}
    strcat(html,"</div><div id=solo><p id=only><span></span></p></div>");
    return update_doc(html);
}
static os64_html_node_t *bi_id(os64_html_document_t *doc, const char *id) { return (void *)by_id(doc->document,id); }
static void bi_step(garb_cascade_t *c, os64_html_document_t *doc, garb_sheet_in_t *in, uint64_t u,
                    const char *marked, const char *unmarked, const char *what)
{
    char said[200];
    bool ok=garb_cascade_update(c);
    if(ok && marked) ok=garb_style_updated_since(c,bi_id(doc,marked),u);
    if(ok && unmarked) ok=!garb_style_updated_since(c,bi_id(doc,unmarked),u);
    snprintf(said,sizeof(said),"back index: %s",what);
    update_check(ok,said);
    update_equal(c,doc,in);
}
static void update_back_index(void)
{
    size_t baseline=live;
    // :last-child, with a descendant reading it.
    os64_html_document_t *doc=bi_doc();
    const char *css="p:last-child{color:red}p:last-child span{opacity:.5}";
    garb_parsed_t sheet;garb_parse_sheet_text(css,strlen(css),&sheet);
    garb_sheet_in_t in={.sheet=&sheet};
    garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    os64_html_node_t *box=bi_id(doc,"box");
    uint64_t u=garb_cascade_updates(c);
    ue_append(doc,box,"p");
    bi_step(c,doc,&in,u,"p39","p20","an append marks the child that was last, not the middle");
    update_check(garb_cascade_update_marks(c).by[GARB_MARK_BACK]==2,
                 "back index: the turned child and its span are counted as a back index's subtree");
    u=garb_cascade_updates(c);
    os64_html_insert(doc,box,os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL),box->first_child);
    bi_step(c,doc,&in,u,NULL,"p39","a prepend leaves the child restyled last time unmarked");
    u=garb_cascade_updates(c);
    os64_html_insert(doc,box,os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL),bi_id(doc,"p20"));
    bi_step(c,doc,&in,u,NULL,"p30","an insert in the middle marks no child after it");
    u=garb_cascade_updates(c);
    os64_html_remove(doc,box->last_child);
    bi_step(c,doc,&in,u,"p39","p20","removing the last marks the new last");
    u=garb_cascade_updates(c);
    os64_html_insert(doc,box,bi_id(doc,"p5"),NULL);
    bi_step(c,doc,&in,u,"p39","p20","a move to the end marks the child that was last");
    u=garb_cascade_updates(c);
    os64_html_node_t *appended=NULL;
    for(int i=0;i<3;++i)appended=ue_append(doc,box,"p");
    os64_html_set_attr(doc,appended,"id","zl",2);
    bi_step(c,doc,&in,u,"p5","p20","three appends in one batch mark the child that was last");
    u=garb_cascade_updates(c);
    os64_html_insert(doc,box,os64_html_create_text(doc,"x",1,NULL),NULL);
    bi_step(c,doc,&in,u,NULL,"zl","a text node put last turns nothing");
    // What came in with an added root is counted as it comes, so a later
    // append to it marks only its last. The root is filled while out of the
    // document and an update passes over that, so the batch that brings it
    // in names no change of its children.
    os64_html_node_t *fresh=os64_html_create_element(doc,OS64_HTML_NS_HTML,"div",NULL);
    for(int i=0;i<5;++i){
        os64_html_node_t *p=os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL);
        char id[16];snprintf(id,sizeof(id),"f%d",i);
        os64_html_set_attr(doc,p,"id",id,strlen(id));
        os64_html_insert(doc,fresh,p,NULL);
    }
    update_check(garb_cascade_update(c),"back index: changes out of the document update");
    os64_html_insert(doc,bi_id(doc,"solo"),fresh,NULL);
    u=garb_cascade_updates(c);
    bi_step(c,doc,&in,u,"f4",NULL,"a subtree comes in");
    u=garb_cascade_updates(c);
    ue_append(doc,fresh,"p");
    bi_step(c,doc,&in,u,"f4","f1","an append to what came in with a root marks its last, not its middle");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // :only-child at both ends: it reads the front too (front_child).
    doc=bi_doc();
    css="p:only-child span{opacity:.5}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    os64_html_node_t *solo=bi_id(doc,"solo");
    u=garb_cascade_updates(c);
    os64_html_node_t *extra=ue_append(doc,solo,"p");
    bi_step(c,doc,&in,u,"only",NULL,":only-child turns at an append");
    os64_html_remove(doc,extra);
    u=garb_cascade_updates(c);
    bi_step(c,doc,&in,u,"only",NULL,":only-child turns back at the removal");
    u=garb_cascade_updates(c);
    os64_html_insert(doc,solo,os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL),solo->first_child);
    bi_step(c,doc,&in,u,"only",NULL,":only-child turns at a prepend");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // A bounded :nth-last-child at its edge: the bound is 4.
    doc=bi_doc();
    css="p:nth-last-child(3){color:red}p:nth-last-child(-n+2) span{opacity:.5}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    box=bi_id(doc,"box");
    u=garb_cascade_updates(c);
    ue_append(doc,box,"p");
    bi_step(c,doc,&in,u,"p37","p20","a bounded :nth-last-child marks the third from the end that moved");
    u=garb_cascade_updates(c);
    os64_html_remove(doc,box->last_child);
    bi_step(c,doc,&in,u,"p37","p20","a bounded :nth-last-child marks what moved back at a removal");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // An unbounded one keeps marking every child.
    doc=bi_doc();
    css="p:nth-last-child(2n+1){color:red}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    u=garb_cascade_updates(c);
    ue_append(doc,bi_id(doc,"box"),"p");
    bi_step(c,doc,&in,u,"p20",NULL,"an unbounded :nth-last-child marks every child");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // -of-type, and a parent past the type table: u's count was never taken
    // at the build, so when a batch brings the walk back under the cap it
    // counts as turned (and it did turn: it is the last p now).
    char html[4096]="<!doctype html><aside>";
    for(int i=0;i<60;++i)strcat(html,"<i></i>");
    strcat(html,"</aside><div id=types><p id=u><span></span></p>");
    for(int i=0;i<32;++i){char x[64];snprintf(x,sizeof(x),"<x-t%d></x-t%d>",i,i);strcat(html,x);}
    strcat(html,"<p id=l></p></div><div id=kin><em id=e1></em><b id=b1></b><em id=e2></em></div>");
    doc=update_doc(html);
    css="p:last-of-type span{opacity:.5}em:last-of-type{color:red}b:only-of-type{color:blue}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    os64_html_node_t *kin=bi_id(doc,"kin");
    u=garb_cascade_updates(c);
    ue_append(doc,kin,"b");
    bi_step(c,doc,&in,u,"b1","e1",":only-of-type turns at an append of its type, its neighbour of another type does not");
    u=garb_cascade_updates(c);
    ue_append(doc,kin,"em");
    bi_step(c,doc,&in,u,"e2","b1",":last-of-type turns at an append of its type");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // The never-counted case alone, with no front form to mark it too.
    doc=update_doc(html);
    css="p:last-of-type span{opacity:.5}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    u=garb_cascade_updates(c);
    os64_html_remove(doc,bi_id(doc,"l"));
    os64_html_remove(doc,bi_id(doc,"types")->last_child);
    bi_step(c,doc,&in,u,"u",NULL,"a count the type table never took counts as turned");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // Past the type table at a change: the children the walk did not reach
    // are left uncounted, so a later batch that brings the parent back
    // under it counts e as turned (it was restyled not red at the first,
    // and is the last p again at the second).
    char wide[4096]="<!doctype html><aside>";
    for(int i=0;i<100;++i)strcat(wide,"<i></i>");
    strcat(wide,"</aside><div id=t><p id=e></p>");
    for(int i=0;i<31;++i){char x[64];snprintf(x,sizeof(x),"<x-t%d></x-t%d>",i,i);strcat(wide,x);}
    strcat(wide,"</div>");
    doc=update_doc(wide);
    css="p:last-of-type{color:red}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    os64_html_node_t *t=bi_id(doc,"t"),*f=ue_append(doc,t,"p");
    os64_html_node_t *x31=ue_append(doc,t,"x-t31");
    u=garb_cascade_updates(c);
    bi_step(c,doc,&in,u,"e",NULL,"a change past the type table marks every child");
    os64_html_remove(doc,f);
    os64_html_remove(doc,x31);
    u=garb_cascade_updates(c);
    bi_step(c,doc,&in,u,"e",NULL,"what a change past the type table did not count counts as turned after");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // A turn moves the siblings after it that a `~` reads: the span after
    // the p that was last of its type, whose own count stays, before the p
    // appended after it.
    doc=bi_doc();
    os64_html_node_t *row=bi_id(doc,"solo");
    os64_html_node_t *s1=ue_append(doc,row,"span");
    os64_html_set_attr(doc,s1,"id","s1",2);
    css="p:last-of-type ~ span{color:red}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    u=garb_cascade_updates(c);
    ue_append(doc,row,"p");
    bi_step(c,doc,&in,u,"s1",NULL,"a turn marks the siblings after it that a ~ reads");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    update_check(live==baseline,"back index: the fixtures release what they hold");
}

// What an update restyled (garb_cascade_update_marks): the change marks
// the <b> it added and, alone, the div its :has() climb visits (body and
// html are turned away: the subject compound is filtered by `div`); the
// div's winners change and its custom properties do not, so nothing below
// it is marked or recomputed.
static void update_marks_counted(void)
{
    size_t baseline=live;
    char html[2048]="<!doctype html><aside>";
    for(int i=0;i<20;++i)strcat(html,"<i></i>");
    strcat(html,"</aside><div id=d><span></span><span></span></div>");
    os64_html_document_t *doc=update_doc(html);
    const char *css="div:has(> b){color:red}";
    garb_parsed_t sheet;garb_parse_sheet_text(css,strlen(css),&sheet);
    garb_sheet_in_t in={.sheet=&sheet};
    garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    garb_update_marks_t none=garb_cascade_update_marks(c);
    ue_append(doc,(void *)by_id(doc->document,"d"),"b");
    bool ok=garb_cascade_update(c);
    garb_update_marks_t m=garb_cascade_update_marks(c);
    update_check(none.marked==0 && none.inherited==0,"marks: zeros before any update");
    // The div's winners changed and its custom properties did not: nothing
    // under it is marked or recomputed (libflow carries what inherits).
    update_check(ok && m.marked==2 && m.inherited==0 && m.recomputed==0,
                 "marks: the change's two, and nothing below a changed winner whose map held");
    update_check(m.by[GARB_MARK_ADDED]==1 && m.by[GARB_MARK_HAS]==1 && m.by[GARB_MARK_INHERITED]==0,
                 "marks: by kind, one added, one by the :has() climb, none inherited");
    update_equal(c,doc,&in);
    // A class write on the div no rule reads marks the div alone, under its
    // own kind.
    os64_html_set_attr(doc,(void *)by_id(doc->document,"d"),"class","k",1);
    ok=garb_cascade_update(c);
    m=garb_cascade_update_marks(c);
    update_check(ok && m.by[GARB_MARK_CLASS]==1 && m.by[GARB_MARK_CHANGED]==0 && m.marked==1,
                 "marks: a class write is counted as class, its element alone where no reader reaches past it");
    update_equal(c,doc,&in);
    ok=garb_cascade_update(c);
    m=garb_cascade_update_marks(c);
    update_check(ok && m.marked==0 && m.inherited==0,"marks: an update that read no changes says zeros");
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // A p put first: the pair it made with #a is read by `p + p`, so #a is
    // marked alone, as a pair of neighbours; #z's neighbour did not move.
    doc=update_doc("<!doctype html><aside><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i></aside>"
                   "<div id=row><p id=a></p><p id=z></p></div>");
    css="p + p{color:red}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    os64_html_node_t *row=(void *)by_id(doc->document,"row");
    os64_html_insert(doc,row,os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL),row->first_child);
    ok=garb_cascade_update(c);
    m=garb_cascade_update_marks(c);
    update_check(ok && m.by[GARB_MARK_ADJACENT]==1 && m.by[GARB_MARK_SUFFIX]==0 && m.by[GARB_MARK_SIBLINGS]==0 &&
                 m.marked==2,"marks: an insert's `+` marks the one pair it made, as a pair of neighbours");
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // A p put first: #a's front index turned, marked alone (a subject form)
    // as a front index's reach; then one appended: #z's back index turned.
    doc=update_doc("<!doctype html><aside><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i></aside>"
                   "<div id=row><p id=a></p><p id=z></p></div>");
    css="p:first-child{color:red}p:last-child{opacity:.5}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    row=(void *)by_id(doc->document,"row");
    os64_html_insert(doc,row,os64_html_create_element(doc,OS64_HTML_NS_HTML,"p",NULL),row->first_child);
    ok=garb_cascade_update(c);
    m=garb_cascade_update_marks(c);
    update_check(ok && m.by[GARB_MARK_FRONT]==1 && m.by[GARB_MARK_BACK]==0 && m.by[GARB_MARK_SUFFIX]==0 &&
                 m.marked==2,"marks: a front index that turned, marked alone as its own kind");
    update_equal(c,doc,&in);
    ue_append(doc,row,"p");
    ok=garb_cascade_update(c);
    m=garb_cascade_update_marks(c);
    update_check(ok && m.by[GARB_MARK_BACK]==1 && m.by[GARB_MARK_FRONT]==0 && m.marked==2,
                 "marks: a back index that turned, marked alone as its own kind");
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // An unbounded back form's walk marks the children before the append
    // that its compound passes, as an unbounded position's walk.
    doc=update_doc("<!doctype html><aside><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i></aside>"
                   "<div id=row><p id=a></p><p id=z></p></div>");
    css="p:nth-last-child(2n+1){color:red}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    ue_append(doc,(void *)by_id(doc->document,"row"),"p");
    ok=garb_cascade_update(c);
    m=garb_cascade_update_marks(c);
    update_check(ok && m.by[GARB_MARK_POSITION]==2 && m.by[GARB_MARK_SUFFIX]==0,
                 "marks: an unbounded back form's walk is counted as its own kind");
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    // An `of S` marks every child at a change of children, as the
    // children's siblings.
    doc=update_doc("<!doctype html><aside><i></i><i></i><i></i><i></i><i></i><i></i><i></i><i></i></aside>"
                   "<div id=row><p id=a></p><p id=z></p></div>");
    css="p:nth-child(2 of p){color:red}";
    garb_parse_sheet_text(css,strlen(css),&sheet);
    c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    ue_append(doc,(void *)by_id(doc->document,"row"),"p");
    ok=garb_cascade_update(c);
    m=garb_cascade_update_marks(c);
    update_check(ok && m.by[GARB_MARK_SUFFIX]==2 && m.by[GARB_MARK_SIBLINGS]==0,
                 "marks: an `of S` marks every child, counted as the children's siblings");
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    update_check(live==baseline,"marks: the fixture releases what it holds");
}

// ONE ELEMENT RESTYLED TWICE IN AN UPDATE keeps every rule (cascade.c
// RuleSeen): the span's class change makes it a dirty root of its own (the
// :has() climb turns its parent p away: the subject is filtered by `div`),
// so it is restyled first; then the div, marked alone for its :has(), comes
// out changed and marks its subtree again, and the span's second match
// list must hold `.x` as the first did.
static void update_restyled_twice(void)
{
    size_t baseline=live;
    char html[2048]="<!doctype html><aside>";
    for(int i=0;i<20;++i)strcat(html,"<i></i>");
    strcat(html,"</aside><div id=d><p><span id=s class=y></span></p></div>");
    os64_html_document_t *doc=update_doc(html);
    const char *css="div:has(> b){color:red}.x{margin-top:2px}";
    garb_parsed_t sheet;garb_parse_sheet_text(css,strlen(css),&sheet);
    garb_sheet_in_t in={.sheet=&sheet};
    garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    os64_html_set_attr(doc,(void *)by_id(doc->document,"s"),"class","x",1);
    ue_append(doc,(void *)by_id(doc->document,"d"),"b");
    update_check(garb_cascade_update(c),"restyled twice: the batch updates");
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    update_check(live==baseline,"restyled twice: the fixture releases what it holds");
}

// THE SUBJECT :has() FILTER (select.h § A SUBJECT :has()): a <b> appended
// under #leaf visits #leaf and its five ancestors (p, div, section, body,
// html); each is marked alone only where some subject compound's filters
// pass it. `marked`/`away`: what garb_cascade_has_subject_work must say,
// -1 for "do not check". Every case's winners equal a fresh cascade's.
static int hs_change=-1;
static void hs_case(const char *css, bool on_div, bool before_p, int marked, int away, const char *what)
{
    char html[2048]="<!doctype html><aside>";
    for(int i=0;i<30;++i)strcat(html,"<i></i>");
    strcat(html,"</aside><section id=top data-q><div id=mid class=m><p id=low class=k></p>"
                "<p id=low2><span id=leaf></span></p></div></section>");
    os64_html_document_t *doc=update_doc(html);
    garb_parsed_t sheet;garb_parse_sheet_text(css,strlen(css),&sheet);
    garb_sheet_in_t in={.sheet=&sheet};
    garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
    if(on_div) os64_html_set_attr(doc,(void *)by_id(doc->document,"mid"),"class","m on",4);
    if(before_p) {
        os64_html_node_t *low2=(void *)by_id(doc->document,"low2");
        os64_html_insert(doc,low2->parent,os64_html_create_element(doc,OS64_HTML_NS_HTML,"b",NULL),low2);
    } else ue_append(doc,(void *)by_id(doc->document,"leaf"),"b");
    bool ok=garb_cascade_update(c);
    garb_has_subject_work_t w=garb_cascade_has_subject_work(c);
    if(marked>=0) ok=ok && (int)w.marked==marked;
    if(away>=0) ok=ok && (int)w.turned_away==away;
    if(hs_change>=0) ok=ok && (int)w.turned_away_change==hs_change;
    if(!ok) fprintf(stderr,"  %s: marked %llu, turned away %llu (by the change %llu), entries %llu, bare %llu\n",what,
                    (unsigned long long)w.marked,(unsigned long long)w.turned_away,
                    (unsigned long long)w.turned_away_change,(unsigned long long)w.entries,(unsigned long long)w.bare);
    hs_change=-1;
    char said[160];snprintf(said,sizeof(said),"subject :has(): %s",what);
    update_check(ok,said);
    update_equal(c,doc,&in);
    garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
}
static void update_has_subjects(void)
{
    size_t baseline=live;
    hs_case("div:has(b){color:red}",false,false,1,5,"a type passes the div alone");
    hs_case("#mid:has(b){color:red}",false,false,1,5,"an id passes its element alone");
    hs_case(".m:has(b){color:red}",false,false,1,5,"a class passes its element alone");
    hs_case("[data-q]:has(b){color:red}",false,false,1,5,"an attribute's presence passes its element alone");
    hs_case("fieldset:has(b){color:red}",false,false,0,6,"a type nothing on the climb has passes none");
    hs_change=5;
    hs_case("div:has(b){color:red}:has(i){opacity:.5}",false,false,1,0,
            "a bare holder whose argument the change brings nothing of turns nothing");
    hs_case("div:has(b){color:red}:has(> b){opacity:.5}",false,false,6,0,"a bare holder the change can turn marks every visit");
    hs_change=6;
    hs_case(".k :has(> .q){color:red}",false,false,0,0,"the measured bare form, a child argument with a class, turned by the change");
    hs_case(".k :has(> b){color:red}",false,false,6,0,"the same form whose argument the change brings");
    hs_case(":is(div:has(b), .zz:has(b)){color:red}",false,false,1,5,"each :is() alternative holding a :has() is its own entry");
    hs_case("section:not(.nope:has(b)){color:red}",false,false,0,6,
            "inside :not(), a failing own test means the :has() cannot move the :not()");
    hs_case("p.k:has(+ b){color:red}",false,true,1,-1,"the preceding siblings visited are filtered too");
    hs_case("div.on:has(b){color:red}",true,false,-1,-1,"a class added to the ancestor in the same batch");
    update_check(live==baseline,"subject :has(): the fixtures release what they hold");
}

// THE READERS (select.h § A READER; cascade.c § THE READERS, ASKED): a
// change of a row's children, each case's marks counted exactly (the
// change's own marks, added elements among them), the elements whose
// winners the change turned named in `yes` (an element marked alone whose
// winners come out the same is restored, so it reads as not restyled), and
// every case's winners equal to a fresh cascade's.
typedef struct {
    os64_html_document_t *doc;
    garb_parsed_t sheet;
    garb_sheet_in_t in;
    garb_cascade_t *c;
    uint64_t prior;
} RdCase;
static os64_html_node_t *rd_el(os64_html_document_t *doc, const char *tag, const char *cls)
{
    os64_html_node_t *e=os64_html_create_element(doc,OS64_HTML_NS_HTML,tag,NULL);
    if(e && cls) os64_html_set_attr(doc,e,"class",cls,strlen(cls));
    return e;
}
static os64_html_node_t *rd_id(RdCase *r, const char *id) { return (os64_html_node_t *)by_id(r->doc->document,id); }
static void rd_begin(RdCase *r, const char *css, const char *body)
{
    size_t cap=strlen(body)+512;
    char *html=malloc(cap);
    strcpy(html,"<!doctype html><aside>");
    for(int i=0;i<30;++i) strcat(html,"<i></i>");
    strcat(html,"</aside>");
    strcat(html,body);
    r->doc=update_doc(html);
    free(html);
    garb_parse_sheet_text(css,strlen(css),&r->sheet);
    r->in=(garb_sheet_in_t){.sheet=&r->sheet};
    r->c=garb_cascade(&r->in,1,r->doc,(garb_env_t){.width=800,.height=600});
    r->prior=garb_cascade_updates(r->c);
}
static void rd_end(RdCase *r, const char *what, int marked, const char *yes, int adjacent)
{
    bool ok=garb_cascade_update(r->c);
    garb_update_marks_t m=garb_cascade_update_marks(r->c);
    bool good=ok && (marked<0 || (int)m.marked==marked) && (adjacent<0 || (int)m.by[GARB_MARK_ADJACENT]==adjacent);
    char ids[128];snprintf(ids,sizeof(ids),"%s",yes?yes:"");
    for(char *id=strtok(ids," ");id;id=strtok(NULL," ")) {
        os64_html_node_t *x=rd_id(r,id);
        bool turned=x && garb_style_updated_since(r->c,x,r->prior);
        if(!turned) fprintf(stderr,"  %s: #%s not restyled\n",what,id);
        good=good && turned;
    }
    if(!good) fprintf(stderr,"  %s: updated %d, marked %llu (wanted %d), pairs %llu, refusal %s\n",what,ok,
                      (unsigned long long)m.marked,marked,(unsigned long long)m.by[GARB_MARK_ADJACENT],
                      garb_update_refusal_name(garb_cascade_update_refusal(r->c)));
    char said[200];snprintf(said,sizeof(said),"readers: %s",what);
    update_check(good,said);
    update_equal(r->c,r->doc,&r->in);
    garb_cascade_free(r->c);garb_free(&r->sheet);os64_html_document_free(r->doc);
}
#define RD_ROW "<div id=row><p id=a></p><p id=b></p><p id=c></p></div>"
#define RD_ROW4 "<div id=row><p id=a class=k></p><p id=b></p><p id=c class=k></p><p id=d class=k><span id=s></span></p></div>"
static void rd_insert(RdCase *r, const char *tag, const char *cls, const char *before)
{
    os64_html_insert(r->doc,rd_id(r,"row"),rd_el(r->doc,tag,cls),before?rd_id(r,before):NULL);
}
static void update_readers(void)
{
    size_t baseline=live;
    RdCase r;
    // The front index (index_walk from the start).
    rd_begin(&r,"p:first-child{color:red}",RD_ROW); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"a front index turned marks the child alone",2,"a",-1);
    rd_begin(&r,"p:first-child{color:red}",RD_ROW); rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"an insert past the bound turns no front index",1,NULL,-1);
    rd_begin(&r,"p:nth-child(2){color:red}",RD_ROW); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"a bounded nth-child turns the first two and stops at the bound",3,"a b",-1);
    rd_begin(&r,"p:first-child{color:red}",RD_ROW); os64_html_remove(r.doc,rd_id(&r,"a"));
    rd_end(&r,"a removal at the front turns the next child's index",1,"b",-1);
    rd_begin(&r,"p:only-child{color:red}","<div id=row><p id=a></p></div>"); rd_insert(&r,"p",NULL,NULL);
    rd_end(&r,":only-child reads the back too",2,"a",-1);
    rd_begin(&r,"p:only-child{color:red}","<div id=row><p id=a></p></div>"); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,":only-child reads the front too",2,"a",-1);
    rd_begin(&r,"p:nth-child(n+2){color:red}",RD_ROW); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"n+2 settles: only the first child can turn, and the walk stops",2,"a",-1);
    // The unbounded forms (position_walks).
    rd_begin(&r,".k:nth-child(3n){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"an unbounded front form marks the moved siblings its filters pass, each alone",4,"c",-1);
    rd_begin(&r,":nth-child(3n){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"an unbounded front form with no filter marks every moved sibling",5,"c",-1);
    rd_begin(&r,".k:nth-child(3n){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,"c");
    rd_end(&r,"an unbounded front walk starts where the change moved the indexes",3,"c",-1);
    rd_begin(&r,".k:nth-last-child(2n+1){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,NULL);
    rd_end(&r,"an unbounded back form marks the moved siblings its filters pass",4,"a c d",-1);
    rd_begin(&r,":nth-last-child(2n+1){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,NULL);
    rd_end(&r,"an unbounded back form with no filter marks every moved sibling",5,"a c d",-1);
    rd_begin(&r,".k:nth-last-child(2n+1){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"an unbounded back walk stops at the element after the change",2,"a",-1);
    rd_begin(&r,"p:not(:nth-child(2n)){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"a form inside :not() is untested: every moved sibling",5,"a b",-1);
    rd_begin(&r,".k:not(:nth-child(2n)){color:red}",RD_ROW4); rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"a form inside :not() passes even where the holder's filters fail",5,"a",-1);
    // `~` and `+` (mark_neighbours).
    rd_begin(&r,".q ~ p{color:red}",RD_ROW); rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"`~`: an element brought its filters fail marks nothing",1,NULL,-1);
    // The element after an insert moved later in the batch: where the
    // change happened is past finding, so every child is marked.
    rd_begin(&r,".q ~ p{color:red}",RD_ROW); rd_insert(&r,"p","q","b");
    os64_html_insert(r.doc,rd_id(&r,"row"),rd_id(&r,"b"),NULL);
    rd_end(&r,"`~`: the element after an insert moved away, every child",-1,"c b",-1);
    rd_begin(&r,".q ~ p{color:red}",RD_ROW); rd_insert(&r,"p","q","b");
    rd_end(&r,"`~`: an element brought its filters pass marks the siblings after it",3,"b c",-1);
    rd_begin(&r,".q + p{color:red}",RD_ROW); rd_insert(&r,"p","q","b");
    rd_end(&r,"`+` alone: the pair made reaches one sibling",2,"b",1);
    // A class set on an element before it is put in is that element's own
    // business: it is marked as added, and its neighbours are asked of it.
    rd_begin(&r,"p.q ~ p{color:red}",RD_ROW); rd_insert(&r,"p","z","b");
    rd_end(&r,"an attribute set on an element the batch brought marks nothing more",1,NULL,-1);
    rd_begin(&r,".q + p + p{color:red}",RD_ROW); rd_insert(&r,"p","q","b");
    rd_end(&r,"a `+` chain reaches every sibling after the change",3,"c",-1);
    rd_begin(&r,".q + p{color:red}","<div id=row><p id=a class=q></p><p id=b></p><p id=c></p></div>");
    rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"`+` alone: the pair broken, its left passing, reaches one sibling",2,"b",1);
    rd_begin(&r,".q + p{color:red}",RD_ROW); rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"`+` alone: a pair broken and made whose lefts fail marks nothing",1,NULL,0);
    rd_begin(&r,".q + p + p{color:red}","<div id=row><p id=a class=q></p><p id=b></p><p id=c></p></div>");
    rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"a `+` chain: the pair broken reaches every sibling after the change",3,"c",-1);
    // Two inserts either side of a text node: the second's left neighbour
    // before the batch is #a, not the element the first brought.
    rd_begin(&r,".q + p{color:red}","<div id=row><p id=a class=q></p> <p id=b></p></div>");
    os64_html_insert(r.doc,rd_id(&r,"row"),rd_el(r.doc,"p",NULL),rd_id(&r,"a")->next);
    rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"`+`: the pair broken skips what the batch brought",-1,"b",-1);
    rd_begin(&r,".q ~ p{color:red}","<div id=row><p id=a class=q></p><p id=b></p><p id=c></p></div>");
    rd_insert(&r,"p",NULL,"b");
    rd_end(&r,"`~`: a reader passing the element before an insert does nothing",1,NULL,0);
    rd_begin(&r,".q ~ p{color:red}","<div id=row><p id=a></p><p id=b class=q></p><p id=c></p><p id=d></p></div>");
    os64_html_remove(r.doc,rd_id(&r,"b"));
    rd_end(&r,"`~`: a removed element its filters pass marks the siblings after it",2,"c d",-1);
    rd_begin(&r,".q ~ p{color:red}","<div id=row><p id=a></p><p id=b></p><p id=c></p><p id=d></p></div>");
    os64_html_remove(r.doc,rd_id(&r,"b"));
    rd_end(&r,"`~`: a removed element its filters fail marks nothing",0,NULL,-1);
    rd_begin(&r,".q ~ p{color:red}","<div id=row><p id=a class=q></p><p id=b></p><p id=c></p><p id=d></p></div>");
    os64_html_node_t *gone=rd_id(&r,"b");
    os64_html_remove(r.doc,gone);
    os64_html_remove_attr(r.doc,rd_id(&r,"a"),"class");
    os64_html_set_attr(r.doc,gone,"class","x",1);
    rd_end(&r,"`~`: a removed element whose attributes the batch changed passes",3,"c d",-1);
    // The pair broken stands in for what its left was: a class taken off it
    // later in the batch passes it (stale), so the pair is marked as such.
    rd_begin(&r,".q + p{color:red}","<div id=row><p id=a class=q></p><p id=b></p><p id=c></p></div>");
    rd_insert(&r,"p",NULL,"b");
    os64_html_remove_attr(r.doc,rd_id(&r,"a"),"class");
    rd_end(&r,"`+`: the attribute-and-insert batch marks the pair broken",-1,"b",1);
    rd_begin(&r,".q + p{color:red}","<div id=row><p id=a class=q></p><p id=b></p></div>");
    os64_html_insert(r.doc,rd_id(&r,"row"),os64_html_create_text(r.doc,"t",1,NULL),rd_id(&r,"b"));
    rd_end(&r,"a change that moves only text moves no element's place",0,NULL,0);
    // An element taken and one put in at the next place, across a text
    // node: the second change's element stands between the text and #c, so
    // the first change's pair reaches #c, the element it did not bring.
    rd_begin(&r,".sel + li{color:red}","<ul id=row><li id=a></li> <li id=b class=sel></li> <li id=c></li></ul>");
    os64_html_remove(r.doc,rd_id(&r,"b"));
    rd_insert(&r,"li",NULL,"c");
    rd_end(&r,"`+`: the pair's reach skips an element brought after the change",-1,"c",-1);
    // An insert across a text node, then the element before it taken and
    // put back in place: the brought element left of the pair may not have
    // stood there, so every reader passes.
    rd_begin(&r,".q + p{color:red}","<div id=row><p id=a></p><p id=m class=q></p> <p id=n></p></div>");
    rd_insert(&r,"p",NULL,"n");
    os64_html_node_t *m=rd_id(&r,"m"),*after_m=m->next;
    os64_html_remove(r.doc,m);
    os64_html_insert(r.doc,rd_id(&r,"row"),m,after_m);
    rd_end(&r,"`+`: a left of the pair the batch brought is past finding",-1,"n",-1);
    rd_begin(&r,"p:empty + p{color:red}","<div id=row><p id=a></p><p id=b></p></div>");
    ue_append(r.doc,rd_id(&r,"a"),"span");
    rd_end(&r,":empty turned: its `+` reader reaches the next sibling",3,"b",-1);
    // A subject :has() and a change of children (garb_has_subject_children_may_turn):
    // the forms a change turns without bringing a match.
    rd_begin(&r,"div:has(> .c:last-child){color:red}","<div id=d><p class=c></p></div>");
    ue_append(r.doc,rd_id(&r,"d"),"span");
    rd_end(&r,":has() argument with a position form: every change of children passes",-1,"d",-1);
    rd_begin(&r,"div:has(> .c:empty){color:red}","<div id=d><p id=pc class=c></p></div>");
    ue_append(r.doc,rd_id(&r,"pc"),"span");
    rd_end(&r,":has() argument with :empty: every change of children passes",-1,"d",-1);
    rd_begin(&r,"div:has(> .a + .c){color:red}","<div id=d><p id=pa class=a></p><p class=c></p></div>");
    os64_html_insert(r.doc,rd_id(&r,"d"),rd_el(r.doc,"span",NULL),rd_id(&r,"pa")->next);
    rd_end(&r,":has() argument with a sibling combinator: every change of children passes",-1,"d",-1);
    rd_begin(&r,"div:has(> p:not(:first-child)){color:red}","<div id=d><p id=p1></p></div>");
    os64_html_insert(r.doc,rd_id(&r,"d"),rd_el(r.doc,"span",NULL),rd_id(&r,"p1"));
    rd_end(&r,":has() argument with :not(): every change of children passes",-1,"d",-1);
    rd_begin(&r,"div:has(input:disabled){color:red}",
             "<div id=d><fieldset id=f disabled><legend id=l1><input></legend></fieldset></div>");
    os64_html_insert(r.doc,rd_id(&r,"f"),rd_el(r.doc,"legend",NULL),rd_id(&r,"l1"));
    rd_end(&r,":has() argument reading a fieldset's children: every change of children passes",-1,"d",-1);
    rd_begin(&r,"p.b:has(+ .k){color:red}","<div id=row><p id=pb class=b></p><span id=mid></span><p class=k></p></div>");
    os64_html_remove(r.doc,rd_id(&r,"mid"));
    rd_end(&r,":has(+ ...) and a removal: the neighbours it makes pass",-1,"pb",-1);
    // A batch whose first change's climb visits #d with nothing it can
    // turn, and whose second change can: #d is visited again.
    rd_begin(&r,"div:has(> .q){color:red}","<div id=d></div>");
    ue_append(r.doc,rd_id(&r,"d"),"span");
    os64_html_insert(r.doc,rd_id(&r,"d"),rd_el(r.doc,"p","q"),NULL);
    rd_end(&r,"a compound a later change makes live is asked where an earlier climb went",-1,"d",-1);
    rd_begin(&r,"div:has(b){color:red}","<div id=d></div><section id=src><span id=sp><b></b></span></section>");
    os64_html_insert(r.doc,rd_id(&r,"d"),rd_id(&r,"sp"),NULL);
    rd_end(&r,"a descendant argument asks every element a moved subtree brings",-1,"d",-1);
    rd_begin(&r,"p.b:has(+ .k){color:red}","<div id=row><p id=pb class=b></p><p class=k></p></div>");
    os64_html_insert(r.doc,rd_id(&r,"row"),rd_el(r.doc,"span",NULL),rd_id(&r,"pb")->next);
    rd_end(&r,":has(+ ...) and an insert: the pair it parts passes",-1,"pb",-1);
    // A subtree taken, changed while out, and put back elsewhere: what it
    // held when taken is past finding though it ends in the document.
    rd_begin(&r,"section:has(.k){color:red}",
             "<section id=s1><div id=p><div id=rr><i id=k class=k></i></div></div></section><div id=q></div>");
    os64_html_node_t *rr=rd_id(&r,"rr"),*k=rd_id(&r,"k");
    os64_html_remove(r.doc,rr);
    os64_html_remove(r.doc,k);
    os64_html_insert(r.doc,rd_id(&r,"q"),rr,NULL);
    rd_end(&r,"a subtree changed while out of the document stands for nothing, put back or not",-1,"s1",-1);
    rd_begin(&r,"div:has(.q){color:red}","<div id=d><section id=s><p id=pq class=q></p></section></div>");
    os64_html_node_t *pq=rd_id(&r,"pq");
    os64_html_remove(r.doc,rd_id(&r,"s"));
    os64_html_remove(r.doc,pq);
    rd_end(&r,"a removed subtree changed after its removal stands for nothing",-1,"d",-1);
    // An nth B at the bottom of int32 classifies without overflow (the
    // host driver runs under UBSan).
    rd_begin(&r,"p:nth-child(n -2147483648){color:red}p:nth-last-child(n -2147483648){opacity:.5}"
             "p:nth-of-type(-n -2147483648){order:2}",RD_ROW);
    rd_insert(&r,"p",NULL,"a");
    rd_end(&r,"an nth B of INT32_MIN classifies without overflow",-1,NULL,-1);
    // An append moves no front index: the unbounded front walk walks nothing.
    rd_begin(&r,".k:nth-child(2n){color:red}",RD_ROW4);
    rd_insert(&r,"p","k",NULL);
    {
        bool ok=garb_cascade_update(r.c);
        garb_reader_work_t w=garb_cascade_reader_work(r.c);
        update_check(ok && w.walks==0 && w.walked==0,"readers: an append walks nothing for an unbounded front form");
        update_equal(r.c,r.doc,&r.in);
        garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
    }
    // What an update keeps on the heap is counted: the batch's arrays.
    rd_begin(&r,".q ~ p{color:red}",RD_ROW);
    {
        size_t before=garb_cascade_parts(r.c).tables;
        os64_html_remove(r.doc,rd_id(&r,"c"));
        rd_insert(&r,"p",NULL,"b");
        os64_html_set_attr(r.doc,rd_id(&r,"a"),"title","t",1);
        bool ok=garb_cascade_update(r.c);
        size_t after=garb_cascade_parts(r.c).tables;
        update_check(ok && after>=before+3*32*sizeof(void *),"readers: the batch's arrays are counted in the cascade's tables");
        garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
    }
    update_check(live==baseline,"readers: the fixtures release what they hold");
}

// A CHANGE OF AN ATTRIBUTE BY ITS KEYS (cascade.c § A CHANGE OF AN
// ATTRIBUTE): the element marked alone, and the reach of the readers that
// read a key of the change and pass at the element with the changed tests
// left out; each case's marks counted exactly and its winners equal to a
// fresh cascade's.
#define AT_TREE "<div class=c id=top><div><div><div id=bar><div id=strip></div><p id=after></p></div></div></div></div>"
// THE RECOMPUTE (L4 part 2): what an element's changed custom property map
// owes what is under it. Each case is compared with a fresh cascade
// (rd_end); `recomputed` and `pruned` are the update's counts.
static void rc_case(const char *css, const char *body, const char *id, const char *style, const char *what,
                    int recomputed, int pruned, const char *yes)
{
    RdCase r;
    rd_begin(&r,css,body);
    os64_html_set_attr(r.doc,rd_id(&r,id),"style",style,strlen(style));
    bool ok=garb_cascade_update(r.c);
    garb_update_marks_t m=garb_cascade_update_marks(r.c);
    char said[200];
    snprintf(said,sizeof(said),"recompute: %s (recomputed %llu, pruned %llu, marked %llu)",what,
             (unsigned long long)m.recomputed,(unsigned long long)m.recompute_pruned,(unsigned long long)m.marked);
    bool good=ok && (recomputed<0 || (int)m.recomputed==recomputed) && (pruned<0 || (int)m.recompute_pruned==pruned);
    if(yes) {
        os64_html_node_t *x=rd_id(&r,yes);
        good=good && x && garb_style_updated_since(r.c,x,r.prior);
    }
    update_check(good,said);
    update_equal(r.c,r.doc,&r.in);
    garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
}
// Two updates in turn: `id1` gets `name1="v1"`, then `id2` `name2="v2"`;
// each compared with a fresh cascade.
static void rc_twice(const char *css, const char *body, const char *id1, const char *name1, const char *v1,
                     const char *id2, const char *name2, const char *v2, const char *what)
{
    // A page large enough that two style writes stay inside the cascade's
    // retained budget (one initial state's worth).
    static char padded[16384], sheet[1024];
    snprintf(padded,sizeof(padded),"%s",body);
    for(int i=0;i<300;i++) strncat(padded,"<b class=pad></b>",sizeof(padded)-strlen(padded)-1);
    snprintf(sheet,sizeof(sheet),"%s .pad{margin-left:var(--q,1px)}",css);
    RdCase r;
    rd_begin(&r,sheet,padded);
    os64_html_set_attr(r.doc,rd_id(&r,id1),name1,v1,strlen(v1));
    bool ok=garb_cascade_update(r.c);
    update_equal(r.c,r.doc,&r.in);
    os64_html_set_attr(r.doc,rd_id(&r,id2),name2,v2,strlen(v2));
    ok=ok && garb_cascade_update(r.c);
    char said[200];snprintf(said,sizeof(said),"recompute: %s",what);
    if(!ok) fprintf(stderr,"  %s: refused %s\n",what,garb_update_refusal_name(garb_cascade_update_refusal(r.c)));
    update_check(ok,said);
    update_equal(r.c,r.doc,&r.in);
    garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
}
// Whether `el`'s keyframes for `name` are the same in two cascades, value
// for value (update_equal compares winners only).
static bool rc_frames_same(const garb_cascade_t *a, const garb_cascade_t *b, const os64_html_node_t *el,
                           const char *name)
{
    const garb_keyframe_t *x=NULL,*y=NULL;
    int32_t nx=garb_keyframes(a,el,name,strlen(name),&x), ny=garb_keyframes(b,el,name,strlen(name),&y);
    if(nx!=ny) return false;
    for(int32_t i=0;i<nx;i++) {
        if(x[i].nsets!=y[i].nsets || x[i].offset!=y[i].offset) return false;
        for(int32_t k=0;k<x[i].nsets;k++) {
            char u[128],v[128];
            garb_val_dump(&x[i].sets[k].value,u,sizeof(u));garb_val_dump(&y[i].sets[k].value,v,sizeof(v));
            if(x[i].sets[k].prop!=y[i].sets[k].prop || strcmp(u,v)) return false;
        }
    }
    return true;
}
static void rc_frames(const char *css, const char *body, const char *id, const char *name, const char *value,
                      const char *el, const char *what)
{
    RdCase r;
    rd_begin(&r,css,body);
    os64_html_set_attr(r.doc,rd_id(&r,id),name,value,strlen(value));
    bool ok=garb_cascade_update(r.c);
    garb_cascade_t *fresh=garb_cascade(&r.in,1,r.doc,(garb_env_t){.width=800,.height=600});
    char said[200];snprintf(said,sizeof(said),"recompute: %s",what);
    update_check(ok && fresh && rc_frames_same(r.c,fresh,rd_id(&r,el),"k"),said);
    garb_cascade_free(fresh);
    update_equal(r.c,r.doc,&r.in);
    garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
}
static void update_recompute_cases(void)
{
    // The same-winners path keeps the old record for its winners and the
    // fresh one's derivation: a later recompute derives from the
    // declarations the element has now, not those it had (a var() winner, a
    // custom property's declaration, a pseudo-element's var()).
    rc_twice("div{}","<div id=g style='--a:red;--b:red'><p id=x style='color:var(--a)'></p></div>",
             "x","style","color:var(--b)","g","style","--a:blue;--b:red",
             "a var() winner switched to an equal one is recomputed from the new");
    rc_twice("#o{outline-color:var(--v)} p{color:var(--v)}",
             "<div id=g style='--a:red;--b:red'><div id=o style='--v:var(--a)'><p id=x></p></div></div>",
             "o","style","--v:var(--b)","g","style","--a:blue;--b:red",
             "a custom property switched to an equal one is made again from the new");
    rc_twice("p::before{content:'x';color:red} p.on::before{color:var(--c)}",
             "<div id=g style='--c:red'><p id=x></p></div>","x","class","on","g","style","--c:blue",
             "a pseudo-element's var() won with equal winners is recomputed whole");
    // Keyframes: a recompute that changes animation-name builds them
    // (whole); a changed animation-name reaches `animation-name: inherit`.
    rc_frames("@keyframes k{to{color:var(--c)}} p{animation-name:var(--n)}",
              "<div id=g style='--n:none;--c:red'><p id=x></p></div>","g","style","--n:k;--c:red","x",
              "an animation-name a recompute changed builds its keyframes");
    rc_frames("@keyframes k{to{color:var(--c)}} .d:has(.t){animation-name:k} p{animation-name:inherit}",
              "<div class=d style='--c:red'><span id=t></span><p id=x></p></div>","t","class","t","x",
              "a changed animation-name reaches what inherits it");
    // A custom property written on a container, read by var() three levels
    // down: each level recomputed, the reader's winner changed.
    rc_case("p{margin-left:var(--x,1px)}","<div id=c><div><span><p id=x></p></span></div></div>","c","--x:7px",
            "a --x write reaches a reader three levels down",3,0,"x");
    // A chain made from another (--w: var(--v)) under a container that set
    // --w itself: the value the reader takes is the new chain's.
    rc_case(".top{--w:calc(52px)} section{margin:var(--w,16px)}",
            "<div class=top><div id=d><span><p><section id=x></section></p></span></div></div>","d",
            "--v:3;--w:var(--v)","a chained write under a container that set the name",-1,0,"x");
    // An element between that declares the name itself: the PRUNE stops
    // there, and the reader under it keeps its record.
    rc_case(".r{--x:2px} p{margin-left:var(--x,1px)}","<div id=c><div class=r><span><p id=x></p></span></div></div>",
            "c","--x:7px","an element declaring the name itself prunes what is under it",1,1,NULL);
    // A write of an equal custom property beside another change: the map is
    // the same by value, so nothing under it is recomputed.
    {
        RdCase r;
        rd_begin(&r,"p{margin-left:var(--x,1px)}",
                 "<div id=c style='--x:3px;color:red'><div><p id=x></p></div></div>");
        os64_html_set_attr(r.doc,rd_id(&r,"c"),"style","--x:3px;color:blue",18);
        bool ok=garb_cascade_update(r.c);
        garb_update_marks_t m=garb_cascade_update_marks(r.c);
        update_check(ok && m.recomputed==0 && m.marked==1 && !garb_style_updated_since(r.c,rd_id(&r,"x"),r.prior),
                     "recompute: a write of an equal custom property marks and recomputes nothing below");
        update_equal(r.c,r.doc,&r.in);
        garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
    }
    // A plain transform write on a container of 2,000 elements marks the
    // container alone and recomputes nothing.
    {
        static char big[2000 * 10 + 64];
        strcpy(big,"<div id=c>");
        for(int i=0;i<2000;i++) strcat(big,"<p>x</p>");
        strcat(big,"</div>");
        RdCase r;
        rd_begin(&r,"p{color:var(--ink,black)}",big);
        os64_html_set_attr(r.doc,rd_id(&r,"c"),"style","transform:translateX(3px)",25);
        bool ok=garb_cascade_update(r.c);
        garb_update_marks_t m=garb_cascade_update_marks(r.c);
        update_check(ok && m.marked==1 && m.recomputed==0,
                     "recompute: a transform write on a 2,000-element container marks one, recomputes none");
        update_equal(r.c,r.doc,&r.in);
        garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
    }
    // A reader of the style attribute keeps the reader's path: its reach
    // is marked and matched again, whatever the map.
    rc_case("div[style*=x] p{color:red}","<div id=c><p id=x></p></div>","c","--x:7px",
            "a [style*=] reader marks its reach as before",0,0,"x");
    // A recomputed element whose var() reads another name: its winners come
    // out the same, and its record is kept (`changed` only where they differ).
    {
        RdCase r;
        rd_begin(&r,"p{margin-left:var(--y,1px)}","<div id=c><div><p id=x></p></div></div>");
        os64_html_set_attr(r.doc,rd_id(&r,"c"),"style","--x:7px",7);
        bool ok=garb_cascade_update(r.c);
        garb_update_marks_t m=garb_cascade_update_marks(r.c);
        update_check(ok && m.recomputed==2 && m.recompute_changed==0 &&
                     !garb_style_updated_since(r.c,rd_id(&r,"x"),r.prior),
                     "recompute: winners that come out the same keep their record");
        update_equal(r.c,r.doc,&r.in);
        garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
    }
    // A name made from the moved one (`--w: var(--v)`) moves with it: an
    // element below that declares --v alone is not pruned, and a reader of
    // --w under it takes the new value.
    rc_case(".m{--w:var(--v)} .e{--v:5px} p{margin-left:var(--w,1px)}",
            "<div id=c><div class=m><div class=e><p id=x></p></div></div></div>","c","--v:7px",
            "a name made from the moved one moves with it",-1,0,"x");
    // An element declaring the moved name as `inherit` takes the new value:
    // it is no PRUNE, and the reader under it is recomputed.
    rc_case(".m{--v:inherit} p{margin-left:var(--v,1px)}",
            "<div id=c><div class=m><span><p id=x></p></span></div></div>","c","--v:7px",
            "an element that declares the moved name as inherit is not pruned",-1,0,"x");
    // An element marked alone by another change of the batch, under the
    // container whose map moved: restyled whole, its map moves with what it
    // inherits, and what is under it is recomputed all the same.
    {
        RdCase r;
        rd_begin(&r,".m:has(.t){color:red} p{margin-left:var(--x,1px)}",
                 "<div id=c><div id=m class=m><span id=t></span><p id=x></p></div></div>");
        os64_html_set_attr(r.doc,rd_id(&r,"c"),"style","--x:7px",7);
        os64_html_set_attr(r.doc,rd_id(&r,"t"),"class","t",1);
        bool ok=garb_cascade_update(r.c);
        update_check(ok && garb_style_updated_since(r.c,rd_id(&r,"x"),r.prior),
                     "recompute: under an element restyled alone in the batch, a reader is recomputed");
        update_equal(r.c,r.doc,&r.in);
        garb_cascade_free(r.c);garb_free(&r.sheet);os64_html_document_free(r.doc);
    }
}
static void update_attribute_readers(void)
{
    size_t baseline=live;
    RdCase r;
    // The progress bar's shape: an inline transform written on a positioned
    // strip deep in a tree, under sheets that hold the bare subject :has().
    rd_begin(&r,".c :has(> .q){color:red}#strip{position:absolute}.x .y{opacity:.5}",AT_TREE);
    os64_html_set_attr(r.doc,rd_id(&r,"strip"),"style","transform:translateX(10px)",26);
    rd_end(&r,"an inline style write that no reader reads marks the element alone",1,NULL,-1);
    rd_begin(&r,".c :has(> .q){color:red}#strip{position:absolute}",AT_TREE);
    os64_html_set_attr(r.doc,rd_id(&r,"bar"),"aria-valuenow","42",2);
    rd_end(&r,"an attribute no reader names marks the element alone",1,NULL,-1);
    rd_begin(&r,"[aria-valuenow] + p{color:red}",AT_TREE);
    os64_html_set_attr(r.doc,rd_id(&r,"strip"),"aria-valuenow","42",2);
    rd_end(&r,"a reader of the attribute reaches its next sibling",2,"after",-1);
    // Classes by the old value: only the classes that differ are keys.
    rd_begin(&r,".a .x{color:red}","<div id=e class='a b'><p id=x class=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"class","a c",3);
    rd_end(&r,"a class change whose differing classes no reader names marks the element alone",1,NULL,-1);
    rd_begin(&r,".a .x{color:red}","<div id=e class=a><p id=x class=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"class","z",1);
    rd_end(&r,"a class a reader names reaches the subtree",2,"x",-1);
    rd_begin(&r,".p.a .x{color:red}","<div id=e><p id=x class=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"class","a",1);
    rd_end(&r,"a reader whose other tests fail at the element is turned away",1,NULL,-1);
    rd_begin(&r,"#m .x{color:red}","<div id=m><p id=x class=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"m"),"id","n",1);
    rd_end(&r,"an id change: the old id's readers reach the subtree",2,NULL,-1);
    rd_begin(&r,"p:lang(fr){color:red}","<div id=e><p id=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"lang","fr",2);
    rd_end(&r,"a key read at an ancestor too marks the subtree",-1,"x",-1);
    // Two changes at one element in a batch: neither may lean on the
    // other's test as the batch left it.
    rd_begin(&r,".a[data-b] .x{color:red}","<div id=e class=a data-b><p id=x class=x></p></div>");
    os64_html_remove_attr(r.doc,rd_id(&r,"e"),"data-b");
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"class","z",1);
    rd_end(&r,"two changes at one element pass every reader of their keys",-1,"x",-1);
    // A `class` written in a namespace is still the class the matcher reads.
    rd_begin(&r,".a > p{color:red}","<div id=e><p id=x></p></div>");
    {
        os64_html_attr_change_t ns={.name="class",.value="a",.value_len=1,.by_ns=true,.ns="urn:x"};
        os64_html_set_attrs(r.doc,rd_id(&r,"e"),&ns,1);
    }
    rd_end(&r,"a class written in a namespace is keyed as the class",2,"x",-1);
    // A key the holder reads: an :is() alternative reads `checked` from the
    // compound holding it.
    rd_begin(&r,"input:checked:is(.a, .b) + p{color:red}","<div><input id=box type=checkbox class=a><p id=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"box"),"checked","",0);
    rd_end(&r,"an :is() alternative reads its holder's keys",2,"x",-1);
    // The bare subject :has() judged by the change's key.
    rd_begin(&r,".c :has(> .q){color:red}","<div class=c><div id=par><p id=kid></p></div></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"kid"),"class","q",1);
    rd_end(&r,"a class the bare :has() argument reads makes it live: every ancestor up the climb",5,"par",-1);
    rd_begin(&r,".c :has(> .q){color:red}","<div class=c><div id=par><p id=kid></p></div></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"kid"),"class","z",1);
    rd_end(&r,"a class no :has() argument reads turns nothing above",1,NULL,-1);
    // A class change whose winners come out the same keeps the class it
    // was styled with up to date, so the next change compares with it.
    rd_begin(&r,".p.b .x{color:red}","<div id=e class=b><p id=x class=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"class","",0);
    update_check(garb_cascade_update(r.c),"attributes: the first class change updates");
    r.prior=garb_cascade_updates(r.c);
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"class","p",1);
    rd_end(&r,"a class change kept alone remembers the new class for the next",1,NULL,-1);
    // An element whose attributes changed counts as restyled even where its
    // winners come out the same.
    rd_begin(&r,"p{color:red}","<div id=e><p id=x></p></div>");
    os64_html_set_attr(r.doc,rd_id(&r,"e"),"width","10",2);
    rd_end(&r,"an element whose attribute changed counts as restyled",1,"e",-1);
    update_check(live==baseline,"attributes: the fixtures release what they hold");
}

static int cascade_updates(void)
{
    update_has_subjects();
    update_readers();
    update_attribute_readers();
    update_recompute_cases();
    update_restyled_twice();
    update_marks_counted();
    update_back_index();
    update_empty_turns();
    update_kept_read();
    update_kept_skips();
    update_dependencies();
    update_inline_changes();
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
    /* What still keeps a cascade from tracking changes: a :has() inside
     * :nth-child(… of S), whose turn moves which sibling the count picks,
     * which nothing follows. It is named. */
    const char *broad[]={"p:nth-child(1 of .a:has(.x)){color:red}"};
    const garb_update_refusal_t why[]={GARB_UPDATE_COUNTED_HAS};
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
        // `[title] p` reads the title the loop writes at the box, so its
        // subtree is marked: a large restyle.
        const char *css="p{color:red;background:blue;border:1px solid green;padding:2px}div[title] p{outline-color:red}";
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
