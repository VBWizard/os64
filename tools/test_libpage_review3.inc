// Request and geometry regressions from the second combined review.
static void review3_nul_fragments(void)
{
    const char *fragments[]={"%00","foo%00bar","%00foo","foo%00"};
    for(size_t i=0;i<sizeof(fragments)/sizeof(fragments[0]);i++) {
        char html[300];
        snprintf(html,sizeof(html),"<p id=foo>Prefix</p><a href='#%s'>Go</a>",fragments[i]);
        run((PCase){.steps="A14",.name="NUL fragment refuses prefix or top",.html=html,.link=true,
            .verdict=OS64_PAGE_REFUSED,.check_reason=true,.reason=OS64_PAGE_REASON_BAD_ACTION});
        snprintf(html,sizeof(html),"<form action='/next#%s'><button>Go</button>",fragments[i]);
        run((PCase){.steps="A14",.name="NUL form fragment refuses",.html=html,
            .verdict=OS64_PAGE_REFUSED,.check_reason=true,.reason=OS64_PAGE_REASON_BAD_ACTION});
        os64_html_document_t *doc=parse(html,"utf-8");os64_url_t base;os64_url_parse(kPage,&base);
        wend_page_t *drawn=wend_render_html(doc,&base,40,NULL,0);char url[300],fragment[80];
        state_expect("legacy form refuses NUL fragment",wend_form_url(drawn,0,kPage,url,sizeof(url),fragment,sizeof(fragment))==WEND_FORM_BAD_ACTION);
        wend_page_free(drawn);os64_html_document_free(doc);
        snprintf(html,sizeof(html),"<form action=/ok><button formaction='/next#%s'>Go</button>",fragments[i]);
        doc=parse(html,"utf-8");drawn=wend_render_html(doc,&base,40,NULL,0);
        state_expect("legacy override refuses NUL fragment",wend_form_url(drawn,0,kPage,url,sizeof(url),fragment,sizeof(fragment))==WEND_FORM_BAD_ACTION);
        wend_page_free(drawn);os64_html_document_free(doc);
        snprintf(html,sizeof(html),"<frameset><frame src='/next#%s'></frameset>",fragments[i]);
        doc=parse(html,"utf-8");drawn=wend_render_html(doc,&base,40,NULL,0);
        state_expect("frame refuses NUL fragment",drawn && drawn->nspots==1 && drawn->spots[0].url && !drawn->spots[0].url[0]);
        wend_page_free(drawn);os64_html_document_free(doc);
        snprintf(html,sizeof(html),"<meta http-equiv=refresh content='0;url=#%s'><p id=foo>Prefix</p>",fragments[i]);
        run((PCase){.steps="A14,K1",.name="NUL refresh never chooses a prefix",.html=html,.refresh=true,
            .verdict=OS64_PAGE_NOTHING,.check_reason=true,.reason=OS64_PAGE_REASON_NO_REFRESH});
    }
    run((PCase){.steps="A14",.name="literal percent-zero is decoded once",.html="<p id='%00'>Literal</p><a href='#%2500'>Go</a>",.link=true,
        .verdict=OS64_PAGE_FRAGMENT,.check_anchor=true,.want_anchor=true,.fragment="%00"});
}

static void review3_opaque_schemes(void)
{
    run((PCase){.steps="A16,G3",.name="opaque scheme same document",.document="DATA:text/plain,MiXeD%2f",
        .html="<p id=target>Target</p><a href='data:text/plain,MiXeD%2f#target'>Go</a>",.link=true,
        .verdict=OS64_PAGE_FRAGMENT,.check_anchor=true,.want_anchor=true});
    run((PCase){.steps="A16",.name="opaque payload retains case",.document="data:text/plain,MiXeD%2f",
        .html="<a href='DaTa:text/plain,mixed%2F#target'>Go</a>",.link=true,.url="data:text/plain,mixed%2F"});
    run((PCase){.steps="A16",.name="opaque base normalizes scheme",.document="data:text/plain,Original",
        .html="<base href='DaTa:text/plain,MiXeD'><a href='#target'>Go</a>",.link=true,.url="data:text/plain,MiXeD"});
    run((PCase){.steps="A16",.name="mail action normalizes scheme only",
        .html="<form action='MaIlTo:User@EXAMPLE'><input name=q value=A><button>Go</button>",.index=1,.url="mailto:User@EXAMPLE?q=A"});
    run((PCase){.steps="A16,K1",.name="opaque refresh same document",.document="DATA:text/plain,X",
        .html="<meta http-equiv=refresh content='0;url=data:text/plain,X#target'><p id=target>Target</p>",.refresh=true,
        .verdict=OS64_PAGE_FRAGMENT,.check_anchor=true,.want_anchor=true});
}

static void review3_refresh_failure(void)
{
    char html[6400];size_t at=(size_t)sprintf(html,"<meta id=first http-equiv=refresh content='5;url=/first#");
    memset(html+at,'x',6000);at+=6000;
    strcpy(html+at,"'><meta id=second http-equiv=refresh content='0;url=/second'>");
    os64_html_document_t *doc=parse(html,"utf-8");os64_page_t *page=os64_page_build(doc,kPage,NULL);
    const os64_html_node_t *first=os64_page_anchor(page,"first");
    const os64_page_refresh_t *refresh=os64_page_refresh(page);
    state_expect("first refresh initially wins",refresh && refresh->node==first && refresh->url.url && strcmp(refresh->url.url,"http://host/first")==0);
    os64_page_free(page);
    size_t before=allocations;page=os64_page_build(doc,kPage,NULL);size_t count=allocations-before;os64_page_free(page);
    bool saw_refusal=false;
    for(size_t i=1;i<=count;i++) {
        fail_single=true;fail_at=allocations+i;page=os64_page_build(doc,kPage,NULL);fail_at=0;fail_single=false;
        if(page) {
            refresh=os64_page_refresh(page);
            state_expect("allocation failure cannot replace winning refresh",refresh && refresh->node==first);
            if(refresh && refresh->url.refused==OS64_PAGE_REASON_NO_MEMORY) {
                saw_refusal=true;os64_page_request_t req;
                state_expect("refresh allocation refusal retained",os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_REFRESH,0,0,0},&req)==OS64_PAGE_REFUSED && req.reason==OS64_PAGE_REASON_NO_MEMORY);
                state_expect("winning refresh delay retained",refresh->seconds==5);
                os64_page_request_free(&req);
            }
        }
        os64_page_free(page);
    }
    state_expect("refresh failure sweep reached refusal",saw_refusal);
    os64_html_document_free(doc);
}

static void review3_unrendered(void)
{
    const char *markup[]={"<area id=x>","<base id=x>","<link id=x>","<meta id=x>","<embed id=x>",
        "<param id=x>","<source id=x>","<track id=x>","<wbr id=x>","<basefont id=x>","<bgsound id=x>",
        "<table><colgroup id=x><col></colgroup></table>","<table><colgroup><col id=x></colgroup></table>",
        "<img alt='' id=x>","<frameset><frame id=x></frameset>"};
    for(size_t i=0;i<sizeof(markup)/sizeof(markup[0]);i++) {
        char html[250];snprintf(html,sizeof(html),"%s<p>Other content</p><a href=#x>Go</a>",markup[i]);
        os64_html_document_t *doc=parse(html,"utf-8");os64_page_t *page=os64_page_build(doc,kPage,NULL);
        const os64_html_node_t *node=os64_page_anchor(page,"x");os64_url_t base;os64_url_parse(kPage,&base);
        for(int width=80;width>=20;width-=60) {
            wend_page_t *drawn=wend_render_html(doc,&base,width,NULL,0);
            state_expect("unrendered target has no geometry",node && wend_node_line(drawn,node)==-1);
            wend_page_free(drawn);
        }
        os64_page_free(page);os64_html_document_free(doc);
    }
}

static void review3_document_failure(void)
{
    char document[3600],html[3800];strcpy(document,"DATA:text/plain,");
    size_t at=strlen(document);memset(document+at,'X',1900);document[at+1900]=0;
    snprintf(html,sizeof(html),"<p id=target>Target</p><a href='data%s#target'>Go</a>",document+4);
    os64_html_document_t *doc=parse(html,"utf-8");
    size_t before=allocations;os64_page_t *page=os64_page_build(doc,document,NULL);size_t count=allocations-before;
    os64_page_request_t initial = {0};
    state_expect("long opaque document setup",page && os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,0,0,0},&initial)==OS64_PAGE_FRAGMENT);
    os64_page_request_free(&initial);os64_page_free(page);
    for(size_t i=1;i<=count;i++) {
        fail_single=true;fail_at=allocations+i;page=os64_page_build(doc,document,NULL);fail_at=0;fail_single=false;
        if(page && os64_page_nlinks(page)) {
            os64_page_request_t req;
            os64_page_verdict_t v=os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_LINK,0,0,0},&req);
            state_expect("canonical allocation failure cannot turn move into fetch",v==OS64_PAGE_FRAGMENT || (v==OS64_PAGE_REFUSED && req.reason==OS64_PAGE_REASON_NO_MEMORY));
            os64_page_request_free(&req);
        }
        os64_page_free(page);
    }
    os64_html_document_free(doc);
}

static void review3_cases(void)
{
    review3_nul_fragments();review3_opaque_schemes();review3_refresh_failure();review3_unrendered();review3_document_failure();
}
