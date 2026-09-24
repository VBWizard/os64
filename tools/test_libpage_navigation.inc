#include "../userland/apps/wend/render.h"

static void navigation_flags(void)
{
    const char *types[] = {"text","search","tel","url","email","password","date","month",
        "week","time","datetime-local","number","range","color","checkbox","radio",
        "file","hidden","submit","image","reset","button"};
    for (size_t i=0;i<sizeof(types)/sizeof(types[0]);i++) {
        char html[180];
        snprintf(html,sizeof(html),"<input type='%s' readonly><input type='%s' disabled>",types[i],types[i]);
        os64_html_document_t *doc=parse(html,"utf-8");
        os64_page_t *page=os64_page_build(doc,kPage,NULL);
        const os64_page_control_t *c=os64_page_control(page,0);
        state_expect("effective readonly by type",c && c->readonly==(i<12));
        state_expect("validation bars by type",c && c->barred_from_validation==(i<12 || i==17 || i>=20));
        state_expect("disabled bars by type",os64_page_control(page,1)->barred_from_validation);
        os64_page_free(page);os64_html_document_free(doc);
    }
    os64_html_document_t *doc=parse("<textarea readonly></textarea><select readonly></select>"
        "<button type=button>B</button><button type=reset>R</button><button>S</button>"
        "<datalist><button>S</button></datalist>","utf-8");
    os64_page_t *page=os64_page_build(doc,kPage,NULL);
    for (int i=0;i<6;i++) {
        const os64_page_control_t *c=os64_page_control(page,i);
        state_expect("effective element readonly",c && c->readonly==(i==0));
        state_expect("element validation eligibility",c && c->barred_from_validation==(i!=1 && i!=4));
    }
    os64_page_free(page);os64_html_document_free(doc);
}

static void navigation_limits(void)
{
    const char *forms[] = {"<form>","<form action='data:text/plain,x'>",
        "<form action='mailto:a@b'>","<form action='mailto:a@b' method=post>",
        "<form action='mailto:a@b' method=post enctype=text/plain>","<form method=post>"};
    for (int i=0;i<6;i++) {
        char html[240];snprintf(html,sizeof(html),"%s<input name=q value=abcdef><button>Go</button>",forms[i]);
        os64_html_document_t *doc=parse(html,"utf-8");
        os64_page_options_t opt=os64_page_options_default();opt.max_body=1;
        os64_page_t *page=os64_page_build(doc,kPage,&opt);
        os64_page_request_t req;
        os64_page_verdict_t v=os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,1,0,0},&req);
        state_expect("limit follows representation",i==5 ? v==OS64_PAGE_REFUSED && req.reason==OS64_PAGE_REASON_BODY_TOO_LONG : v==OS64_PAGE_NAVIGATE && req.body==NULL);
        os64_page_request_free(&req);os64_page_free(page);os64_html_document_free(doc);
    }
    for (int mail=0;mail<2;mail++) {
        char *html=malloc(OS64_URL_REF_MAX*2);
        size_t at=(size_t)sprintf(html,"<form %s><input name=q value='",mail?"action=mailto:a@b method=post enctype=text/plain":"");
        memset(html+at,' ',OS64_URL_REF_MAX);at+=OS64_URL_REF_MAX;
        strcpy(html+at,"'><button>Go</button>");
        os64_html_document_t *doc=parse(html,"utf-8");free(html);
        os64_page_t *page=os64_page_build(doc,kPage,NULL);os64_page_request_t req;
        os64_page_verdict_t v=os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,1,0,0},&req);
        state_expect("URL overflow reason",v==OS64_PAGE_REFUSED && req.reason==OS64_PAGE_REASON_TOO_LONG);
        os64_page_request_free(&req);os64_page_free(page);os64_html_document_free(doc);
    }
}

static void navigation_direction(void)
{
    const char *isolated[]={"<bdi>א</bdi>","<script>א</script>","<style>א</style>",
        "<textarea>א</textarea>","<span dir=rtl>א</span>","<span dir=auto>א</span>","<span dir=ltr>א</span>"};
    for (size_t i=0;i<sizeof(isolated)/sizeof(isolated[0]);i++) {
        char html[300];snprintf(html,sizeof(html),"<form method=post><div dir=auto>%sA<input name=q dirname=d></div><button>Go</button>",isolated[i]);
        run((PCase){.steps="D22",.name="auto excludes isolated subtree",.html=html,.index=-1,.body="q=&d=ltr"});
    }
    run((PCase){.steps="D22",.name="invalid dir does not isolate",.html="<form method=post><div dir=auto><span dir=bogus>א</span>A<input name=q dirname=d></div><button>Go</button>",.index=-1,.body="q=&d=rtl"});
    run((PCase){.steps="D22",.name="bdi defaults to auto",.html="<form method=post><bdi>א<input name=q dirname=d></bdi><button>Go</button>",.index=-1,.body="q=&d=rtl"});
    run((PCase){.steps="D22",.name="telephone does not submit dirname",.html="<form method=post dir=rtl><input type=tel name=q dirname=d><button>Go</button>",.index=-1,.body="q="});
    run((PCase){.steps="D22",.name="control auto reads edited value",.html="<form method=post><input name=q dirname=d dir=auto><button>Go</button>",.index=-1,.setup=boundary_unicode,.body="q=%C3%A9&d=ltr"});
}

static void navigation_geometry(void)
{
    const char *html="<meta http-equiv=refresh content='0;url=#x'><p><a name=x href=/old>Legacy first</a></p>"
        "<p>one two three four five six seven eight nine ten eleven twelve</p>"
        "<h2 id=x>ID winner</h2><p id=x>Duplicate loser</p>"
        "<p><a name=y href=/next>Named link</a></p>"
        "<div hidden id=hidden>Hidden</div><p id=top>Claimed top</p><script id=script>x</script>";
    os64_html_document_t *doc=parse(html,"utf-8");os64_page_t *page=os64_page_build(doc,kPage,NULL);
    const os64_html_node_t *target=NULL;
    state_expect("ID precedence",os64_page_resolve_fragment(page,"x",&target)==OS64_PAGE_REASON_OK && target->tag==OS64_HTML_TAG_H2);
    int32_t previous=-1;
    for(int width=80;width>=20;width-=60) {
        wend_page_t *drawn=wend_render_html(doc,page,width);
        int32_t row=wend_node_line(drawn,target);
        state_expect("resolved node reaches its row",row>=0 && row<drawn->nlines && strcmp(drawn->lines[row].text,"ID winner")==0);
        if(previous>=0) state_expect("geometry rebuilt on reflow",row>previous);
        previous=row;
        const os64_html_node_t *node=NULL;
        state_expect("named link selected",os64_page_resolve_fragment(page,"y",&node)==0 && node!=NULL);
        row=wend_node_line(drawn,node);
        state_expect("named link rendered",row>=0 && strstr(drawn->lines[row].text,"Named link")!=NULL);
        os64_page_resolve_fragment(page,"hidden",&node);
        state_expect("hidden target has no geometry",node!=NULL && wend_node_line(drawn,node)==-1);
        os64_page_resolve_fragment(page,"script",&node);
        state_expect("script target has no geometry",node!=NULL && wend_node_line(drawn,node)==-1);
        os64_page_resolve_fragment(page,"top",&node);
        state_expect("claimed top is a node",node!=NULL && wend_node_line(drawn,node)>0);
        state_expect("fallback top",os64_page_resolve_fragment(page,"TOP",&node)==0 && node==NULL);
        state_expect("empty top",os64_page_resolve_fragment(page,"",&node)==0 && node==NULL);
        state_expect("absent fragment",os64_page_resolve_fragment(page,"absent",&node)==OS64_PAGE_REASON_NO_ANCHOR);
        wend_page_free(drawn);
    }
    size_t before=allocations;wend_page_t *drawn=wend_render_html(doc,page,40);
    size_t count=allocations-before;wend_page_free(drawn);
    for (size_t i=1;i<=count;i++) {
        fail_single=true;fail_at=allocations+i;drawn=wend_render_html(doc,page,40);fail_at=0;fail_single=false;
        if(drawn && drawn->incomplete) state_expect("incomplete geometry refuses",wend_node_line(drawn,target)==-1);
        wend_page_free(drawn);
    }
    os64_page_free(page);
    before=allocations;page=os64_page_build(doc,kPage,NULL);count=allocations-before;os64_page_free(page);
    for (size_t i=1;i<=count;i++) {
        fail_single=true;fail_at=allocations+i;page=os64_page_build(doc,kPage,NULL);fail_at=0;fail_single=false;
        if(page && os64_page_incomplete(page)) {
            const char *names[]={"x","top","TOP","absent"};
            for(int j=0;j<4;j++) state_expect("incomplete lookup cannot choose fallback",os64_page_resolve_fragment(page,names[j],&target)==OS64_PAGE_REASON_NO_MEMORY && target==NULL);
            state_expect("incomplete empty fragment",os64_page_resolve_fragment(page,"",&target)==0 && target==NULL);
            if(os64_page_refresh(page)) {
                os64_page_request_t req;
                os64_page_verdict_t v=os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_REFRESH,0,0,0},&req);
                state_expect("incomplete activation refuses",v==OS64_PAGE_REFUSED && req.reason==OS64_PAGE_REASON_NO_MEMORY);
                os64_page_request_free(&req);
            }
        }
        os64_page_free(page);
    }
    os64_html_document_free(doc);
}

static void navigation_block_geometry(void)
{
    const char *tags[]={"div","h2","p","blockquote","ul","li","dd","pre","form","fieldset","hr"};
    for(size_t i=0;i<sizeof(tags)/sizeof(tags[0]);i++) {
        char html[200];snprintf(html,sizeof(html),"<div>Before<%s id=x>Target</%s></div>",tags[i],tags[i]);
        os64_html_document_t *doc=parse(html,"utf-8");os64_page_t *page=os64_page_build(doc,kPage,NULL);
        wend_page_t *drawn=wend_render_html(doc,page,40);
        int32_t row=wend_node_line(drawn,os64_page_anchor(page,"x"));
        state_expect("block target starts after preceding text",row>0);
        wend_page_free(drawn);os64_page_free(page);os64_html_document_free(doc);
    }
}

static void navigation_long_fragment(void)
{
    char name[1101], html[2400];memset(name,'x',1100);name[1100]=0;
    snprintf(html,sizeof(html),"<meta http-equiv=refresh content='0;url=/other#%s'><a href='/other#%s'>Go</a>",name,name);
    for(int link=0;link<2;link++) {
        os64_html_document_t *doc=parse(html,"utf-8");
        os64_page_t *page=os64_page_build(doc,kPage,NULL);os64_page_request_t req;
        state_expect("long fragment navigation",os64_page_activate(page,(os64_page_what_t){link?OS64_PAGE_ACTIVATE_LINK:OS64_PAGE_ACTIVATE_REFRESH,0,0,0},&req)==OS64_PAGE_NAVIGATE);
        os64_page_free(page);os64_html_document_free(doc);
        state_expect("fragment survives source destruction",req.fragment && strcmp(req.fragment,name)==0);
        char destination[1300];snprintf(destination,sizeof(destination),"<p>Before</p><h2 id='%s'>Long target</h2>",name);
        doc=parse(destination,"utf-8");page=os64_page_build(doc,req.url,NULL);
        const os64_html_node_t *node=NULL;
        state_expect("destination resolves entire fragment",os64_page_resolve_fragment(page,req.fragment,&node)==0 && node!=NULL);
        wend_page_t *drawn=wend_render_html(doc,page,40);
        int32_t row=wend_node_line(drawn,node);
        state_expect("long target geometry",row>=0 && strcmp(drawn->lines[row].text,"Long target")==0);
        wend_page_free(drawn);os64_page_request_free(&req);os64_page_free(page);os64_html_document_free(doc);
    }
    snprintf(html,sizeof(html),"<form action='/other#%s'><button>Go</button>",name);
    os64_html_document_t *doc=parse(html,"utf-8");os64_page_t *page=os64_page_build(doc,kPage,NULL);
    os64_page_request_t req;
    state_expect("form keeps its whole fragment",
        os64_page_activate(page,(os64_page_what_t){OS64_PAGE_ACTIVATE_CONTROL,0,0,0},&req)==OS64_PAGE_NAVIGATE &&
        req.fragment && strcmp(req.fragment,name)==0);
    os64_page_request_free(&req);os64_page_free(page);os64_html_document_free(doc);
}

static void navigation_cases(void)
{
    navigation_flags();navigation_limits();navigation_direction();navigation_geometry();navigation_block_geometry();navigation_long_fragment();
}
