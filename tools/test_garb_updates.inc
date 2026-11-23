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
    /* THE CALLER'S MEASURE (garb_cascade_limits_t.clock): a cascade made
     * with a clock says what its fresh build cost by part, and a broad
     * refusal names the change that marked the most, by which mark; one
     * made without a clock says nothing of either. */
    {
        char html[8192]="<!doctype html><div id=a class=a>";
        for(int i=0;i<260;++i) strcat(html,"<p>x</p>");
        strcat(html,"<i id=i></i></div><b id=b></b>");
        const char *css=".a:has(.b){--ink:red}p{color:var(--ink)}";
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
                update_check(k.rules==2 && k.elements>=263 && k.sheets>0 && k.table>0 && k.matching>0,
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
                // The :has() turned at #a: #a is restyled alone, its --ink
                // comes out different, and passes down to its children,
                // more than half the page; the batch's change that began
                // it is the class at i#i.
                update_check(refused && named && t.kind==OS64_HTML_CHANGE_ATTR && !strcmp(t.attr,"class") &&
                             !strcmp(t.target,"i#i.b") && t.depth==4 && t.marked>0 &&
                             t.by[GARB_MARK_SUBTREE]+t.by[GARB_MARK_HAS]==t.marked && t.changes==1 &&
                             t.marked_changes==1 && t.restyled && !strcmp(t.restyled_from,"div#a.a") &&
                             t.restyled_depth==3 && t.dirty>t.slots/2 && t.restyled_marked+t.marked==t.dirty,
                             "measure: a broad refusal names its change, what passed its style down, and the marks");
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

static int cascade_updates(void)
{
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
