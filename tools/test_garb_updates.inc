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
        "x-item:defined{color:red}x-item:not(:defined) + p{color:blue}"
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
static int cascade_updates(void)
{
    update_dependencies();
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
        update_check(!os64_html_changes_bytes(doc),"broad dependency does not retain an unusable mutation log");
        update_check(!garb_cascade_update(c),"nested broad dependency requests full cascade");
        os64_html_reader_t *reader=os64_html_reader_new(doc,os64_html_version(doc));
        os64_html_set_attr(doc,(os64_html_node_t *)by_id(doc->document,"a"),"title","kept",4);
        const os64_html_change_entry_t *entry=NULL;
        update_check(reader && os64_html_changes(reader,&entry)==1 && entry && entry->kind==OS64_HTML_CHANGE_ATTR,
                     "broad cascade leaves another native reader's invalidations intact");
        os64_html_reader_free(reader);
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
    for(int growth=0;growth<2;++growth) {
        points=0;
        for(size_t failure=0;failure<=points;++failure) {
            char html[1024]="<!doctype html><body>";
            for(int i=0;i<27;++i)strcat(html,"<p></p>");
            os64_html_document_t *doc=update_doc(growth?html:"<!doctype html><div id=a><p>text</p></div>");
            garb_parsed_t sheet;const char *text="div{--x:red}p{color:var(--x)}";
            garb_parse_sheet_text(text,strlen(text),&sheet);garb_sheet_in_t in={.sheet=&sheet};
            garb_cascade_t *c=garb_cascade(&in,1,doc,(garb_env_t){.width=800,.height=600});
            if(growth) {
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
        printf("libgarb updates: %s sweep, %zu allocation positions\n",growth?"slot growth":"inline style",points);
    }
    printf("libgarb updates: %u checks, %u failed\n",update_checks,update_failures);
    return update_failures?1:0;
}
