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
static int cascade_updates(void)
{
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
        os64_html_changes_limit(doc,0);
        os64_html_set_attr(doc,a,"class","lost",4);
        update_check(!garb_cascade_update(c),"lost history refuses reuse");
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
        update_check(live==baseline,"updates release allocations and node holds");
    }
    const char *broad[]={"div:has(p){color:red}","div:is(:not(:has(p))){color:blue}","p:dir(rtl){color:green}"};
    for(size_t i=0;i<sizeof(broad)/sizeof(*broad);++i) {
        os64_html_document_t *doc=update_doc("<!doctype html><div id=a><p>text</p></div>");
        garb_parsed_t sheet;garb_parse_sheet_text(broad[i],strlen(broad[i]),&sheet);garb_sheet_in_t in={.sheet=&sheet};
        garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"a"),"class","x",1);
        update_check(!garb_cascade_update(c),"nested broad dependency requests full cascade");
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
    }
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
    for(size_t failure=0;failure<=points;++failure) {
        os64_html_document_t *doc=update_doc("<!doctype html><div id=a><p>text</p></div>");
        garb_parsed_t sheet;const char *text="div{--x:red}p{color:var(--x)}";
        garb_parse_sheet_text(text,strlen(text),&sheet);garb_sheet_in_t in={.sheet=&sheet};
        garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
        os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"a"),"style","--x:blue",8);
        size_t before=allocations;fail_single=true;fail_at=failure?before+failure:0;
        bool updated=garb_cascade_update(c);fail_at=0;fail_single=false;
        if(!failure){points=allocations-before;update_check(updated,"unfailed update");}
        if(updated)update_equal(c,doc,&in);
        garb_cascade_free(c);garb_free(&sheet);os64_html_document_free(doc);
        update_check(live==baseline,"allocation refusal releases ownership");
    }
    printf("libgarb updates: %u checks, %u failed, %zu allocation positions\n",update_checks,update_failures,points);
    return update_failures?1:0;
}
