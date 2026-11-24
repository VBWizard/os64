// Whole-layout oracle for copied geometry. The source is freed before public
// observations, so source-arena borrowing becomes a sanitizer error.
static flow_tree_t *graft_layout(Sheeted *p, const flow_tree_t *old, flow_profile_t *profile,
                                 const flow_reuse_inputs_t *given)
{
    flow_reuse_inputs_t inputs = {.resource_owner = p->doc};
    os64_page_t *model = os64_page_rebuild(p->page);
    flow_tree_t *t = flow_layout_incremental(p->doc, model, 800, &p->env, old,
                                            given ? given : &inputs, profile, NULL, NULL);
    os64_page_free(model);
    return t;
}
static void graft_runs_equal(const os64_text_run_t *a, const os64_text_run_t *b)
{
    expect("graft: run presence agrees", !!a == !!b, NULL);
    if (!a || !b) return;
    os64_text_run_view_t x, y;
    expect("graft: live run views", os64_text_run_view(a,&x)==OS64_FONT_OK &&
           os64_text_run_view(b,&y)==OS64_FONT_OK, NULL);
    bool same = x.byte_count==y.byte_count && x.glyph_count==y.glyph_count &&
        x.caret_count==y.caret_count && x.advance_x==y.advance_x && x.ascent==y.ascent &&
        x.descent==y.descent && x.line_height==y.line_height &&
        !memcmp(x.bytes,y.bytes,x.byte_count) && !memcmp(&x.ink,&y.ink,sizeof(x.ink));
    expect("graft: run bytes and metrics agree",same,NULL);
    for(size_t i=0;same && i<x.glyph_count;++i) {
        const os64_text_placement_t *u=&x.glyphs[i],*v=&y.glyphs[i];
        expect("graft: glyph placement agrees",u->byte_begin==v->byte_begin && u->byte_end==v->byte_end &&
            u->font_identity==v->font_identity && u->glyph_index==v->glyph_index &&
            u->x==v->x && u->y==v->y && u->advance_x==v->advance_x &&
            !memcmp(&u->ink,&v->ink,sizeof(u->ink)),NULL);
    }
    for(size_t i=0;same && i<x.caret_count;++i)
        expect("graft: caret positions agree",x.carets[i].byte_offset==y.carets[i].byte_offset &&
               x.carets[i].x==y.carets[i].x,NULL);
    os64_font_rect_t ra,rb;
    expect("graft: selection rectangle agrees",os64_text_selection(a,0,x.byte_count,&ra)==OS64_FONT_OK &&
           os64_text_selection(b,0,y.byte_count,&rb)==OS64_FONT_OK && !memcmp(&ra,&rb,sizeof(ra)),NULL);
}
static void graft_public_equal(const flow_tree_t *ta,const flow_tree_t *tb,
                                const flow_box_t *a,const flow_box_t *b)
{
    expect("graft: public tree shape agrees",!!a==!!b,NULL);
    for(;a && b;a=a->next,b=b->next) {
        flow_box_t x=*a,y=*b;
        x.style=y.style=NULL;x.run=y.run=NULL;x.text=y.text=NULL;x.ellipsis=y.ellipsis=NULL;
        x.frame=y.frame=NULL;x.parent=y.parent=x.first=y.first=x.next=y.next=NULL;
        expect("graft: public geometry, links, controls, clips and ink agree",!memcmp(&x,&y,sizeof(x)),NULL);
        expect("graft: public text bytes agree",a->length==b->length &&
               (!a->length || (a->text && b->text && !memcmp(a->text,b->text,a->length))),NULL);
        expect("graft: public style values agree",(!a->style && !b->style) ||
               (a->style && b->style && f_style_equal(a->style,b->style)),NULL);
        graft_runs_equal(a->run,b->run);graft_runs_equal(a->ellipsis,b->ellipsis);
        os64_gui_rect_t ca=flow_box_doc_clip(a,(flow_point_t){0}),cb=flow_box_doc_clip(b,(flow_point_t){0});
        expect("graft: effective clipping agrees",!memcmp(&ca,&cb,sizeof(ca)),NULL);
        int32_t hx=a->rect.x+a->rect.w/2,hy=a->rect.y+a->rect.h/2;
        const flow_box_t *ha=flow_hit(ta,hx,hy,(flow_point_t){0}),*hb=flow_hit(tb,hx,hy,(flow_point_t){0});
        expect("graft: hit identity agrees",(!ha && !hb) || (ha && hb && ha->kind==hb->kind &&
               ha->node==hb->node && ha->begin==hb->begin && ha->link==hb->link && ha->control==hb->control),NULL);
        graft_public_equal(ta,tb,a->first,b->first);
    }
    expect("graft: public sibling count agrees",!a && !b,NULL);
}
typedef struct {uint64_t hash,count;} GraftPaint;
static void graft_paint(void *opaque,const flow_box_t *b)
{
    GraftPaint *p=opaque;p->count++;
    uint64_t fields[]={b->kind,(uintptr_t)b->node,b->rect.x,b->rect.y,b->rect.w,b->rect.h,
        b->baseline,b->link,b->control,b->begin,b->length,b->decoration,b->underline_color,b->line_through_color};
    for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);++i)p->hash=(p->hash^fields[i])*UINT64_C(1099511628211);
}
static void graft_oracle(Sheeted *p,flow_tree_t *tree)
{
    garb_sheet_in_t sheets[16];
    for(int32_t i=0;i<p->n;++i)sheets[i]=(garb_sheet_in_t){.sheet=&p->sheets[i]};
    garb_cascade_t *cascade=garb_cascade(sheets,p->n,p->doc,garb_cascade_env(p->cascade));
    flow_env_t env=p->env;env.cascade=cascade;
    flow_tree_t *whole=flow_layout(p->doc,flow_model(tree),800,&env);
    expect("graft: oracle trees complete",tree && whole && !flow_incomplete(tree) && !flow_incomplete(whole),NULL);
    if(tree && whole) {
        int64_t n=flow_dump(tree,NULL,0),m=flow_dump(whole,NULL,0);
        char *a=malloc((size_t)n+1),*b=malloc((size_t)m+1);
        flow_dump(tree,a,(size_t)n+1);flow_dump(whole,b,(size_t)m+1);
        expect("graft: dump matches independent whole layout",n==m && !strcmp(a,b),NULL);free(a);free(b);
        graft_public_equal(tree,whole,flow_root(tree),flow_root(whole));
        expect("graft: scroller count agrees",flow_nscrollers(tree)==flow_nscrollers(whole),NULL);
        for(int32_t i=0;i<flow_nscrollers(tree) && i<flow_nscrollers(whole);++i) {
            flow_point_t a=flow_scroll_range(tree,i),b=flow_scroll_range(whole,i);
            expect("graft: scroll reach agrees",a.x==b.x && a.y==b.y,NULL);
        }
        GraftPaint pa={0},pb={0};
        flow_visit(tree,(os64_gui_rect_t){0,0,800,1000000},(flow_point_t){0},graft_paint,&pa);
        flow_visit(whole,(os64_gui_rect_t){0,0,800,1000000},(flow_point_t){0},graft_paint,&pb);
        expect("graft: painter traversal agrees",pa.hash==pb.hash && pa.count==pb.count,NULL);
    }
    flow_free(whole);garb_cascade_free(cascade);
}
static void graft_update(Sheeted *p)
{
    expect("graft: localized cascade update accepted",garb_cascade_update(p->cascade),NULL);
}
static os64_html_node_t *graft_node(Sheeted *p,const char *id)
{
    return (os64_html_node_t *)computed_find(p->doc->document,id);
}
static void graft_cases(void)
{
    const char *html="<!doctype html><style>p{margin:4px}#clip{width:90px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}</style>"
        "<div id=grow style='height:20px'></div><p id=kept><a href=x>hello <b>bold</b></a> world</p>"
        "<ol><li>first<li><p>nested list text</p></ol><p id=clip>long clipped text with ellipsis</p>"
        "<p>image <img src=known.png> <input value=control></p><pre>tabs\tand spaces</pre>"
        "<div style='height:25px;overflow:auto'><p>scroll content</p><p>more content</p></div>";
    Sheeted p;sheeted_open(&p,html,800,600);
    flow_profile_t profile;
    flow_tree_t *old=graft_layout(&p,NULL,&profile,NULL);
    expect("graft: first layout does fresh work",old && !profile.reused && profile.measures>0,NULL);
    for(int round=0;round<5;++round) {
        if(round) {
            const char *style=round%2 ? "height:90px" : "height:10px";
            os64_html_set_attr(p.doc,graft_node(&p,"grow"),"style",style,strlen(style));graft_update(&p);
        }
        flow_tree_t *next=graft_layout(&p,old,&profile,NULL);
        expect("graft: unchanged subtrees copy geometry",next && profile.reused>0,NULL);
        if(!round)expect("graft: unchanged layout avoids IFC measurements",profile.measures==0,NULL);
        flow_free(old);graft_oracle(&p,next);old=next;
    }
    flow_free(old);sheeted_close(&p);

    // Generated content is rebuilt with the new counter/quote state, even
    // when the originating element's ordinary style is unchanged.
    sheeted_open(&p,"<!doctype html><style>body{counter-reset:n}"
        ".count{counter-increment:n}.count::before{content:counter(n) attr(title);color:var(--ink,red)}"
        ".on{--ink:blue}.on::after{content:'tail'}</style>"
        "<div id=grow class=count title=A></div><section><p id=kept class=count title=B></p>"
        "<q>quote <q>inner</q></q></section><p>stable tail</p>",800,600);
    old=graft_layout(&p,NULL,NULL,NULL);
    for(int round=0;round<4;++round) {
        if(round==0)os64_html_set_attr(p.doc,graft_node(&p,"grow"),"class","off",3);
        if(round==1)os64_html_set_attr(p.doc,graft_node(&p,"kept"),"class","count on",8);
        if(round==2)os64_html_set_attr(p.doc,graft_node(&p,"kept"),"title","changed",7);
        if(round==3)os64_html_set_attr(p.doc,graft_node(&p,"kept"),"class","off",3);
        graft_update(&p);
        flow_tree_t *next=graft_layout(&p,old,&profile,NULL);
        flow_free(old);graft_oracle(&p,next);old=next;
    }
    flow_free(old);sheeted_close(&p);

    const char *fixtures[]={
        "<!doctype html><div id=grow style='height:10px;margin-bottom:20px'></div>"
        "<div id=empty style='margin-top:-10px'></div><p style='margin-top:25px'>margin proof</p><p>tail</p><p>end</p>",
        "<!doctype html><div id=grow style='height:20px'></div><div style='margin:20px'><div style='margin:-15px'></div></div><p>after empty</p><p>end</p>",
        "<!doctype html><div style='float:left;width:300px;height:100px'></div><div id=grow style='height:200px'></div>"
        "<div><p style='margin:0;margin-top:-80px'>alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega</p></div><p>end</p>",
        "<!doctype html><div style='float:left;width:300px;height:100px'></div><div id=grow style='height:200px'></div>"
        "<section style='position:relative;top:120px'><div><p style='margin:0;margin-top:-80px'>alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega</p></div></section><p>end</p>",
        "<!doctype html><div id=grow style='height:20px'></div><div style='display:grid;height:300px;align-items:center'>"
        "<div><p>grid descendant <b>text</b></p></div></div><p>end</p>",
        "<!doctype html><div id=grow style='height:20px'></div><div style='display:flex;height:300px;align-items:center'>"
        "<div><p>flex descendant <b>text</b></p></div></div><p>end</p>",
        "<!doctype html><div id=grow style='height:20px'></div><table style='height:250px'><tr><td style='vertical-align:middle'>"
        "<p>cell descendant</p></td><td>other<br>cell</td></tr></table><p>end</p>",
        "<!doctype html><div id=grow style='height:20px'></div><p>before <img src=missing.png alt=' repeated  alt '> after</p><p>end</p>",
        // A percentage height is of the height it is placed in: a copy
        // keyed without it would keep the old one.
        "<!doctype html><div id=grow style='height:20px'><p style='height:50%;margin:0'>half</p></div><p>end</p>",
        // An inline picture's percentage height is of its block's: widths
        // measured against the old height are not this layout's.
        "<!doctype html><style>#grow{display:flex}</style><div id=grow style='height:20px'>"
        "<div style='height:100%'><img src=known.png style='height:100%'></div></div><p>end</p>"
    };
    for(size_t i=0;i<sizeof(fixtures)/sizeof(fixtures[0]);++i) {
        sheeted_open(&p,fixtures[i],800,600);old=graft_layout(&p,NULL,NULL,NULL);
        const char *style=i==0 ? "height:10px;margin-bottom:30px" : i==2 || i==3 ? "height:100px" : "height:90px";
        os64_html_set_attr(p.doc,graft_node(&p,"grow"),"style",style,strlen(style));
        if(i==0)os64_html_set_attr(p.doc,graft_node(&p,"empty"),"style","margin-top:-20px",16);
        graft_update(&p);
        flow_tree_t *next=graft_layout(&p,old,&profile,NULL);
        flow_reuse_stats_t stats=flow_reuse_stats(next);
        if(i==0)expect("graft: equal margin sums with different extrema refuse",stats.refused[FLOW_REUSE_CALL_INPUTS]>0,NULL);
        if(i==2 || i==3)expect("graft: negative-margin float query bound refuses",stats.refused[FLOW_REUSE_FLOATS]>0,NULL);
        if(i>=4 && i<=6)expect("graft: descendant survives parent placement/translation",profile.reused>0,NULL);
        flow_free(old);graft_oracle(&p,next);flow_free(next);sheeted_close(&p);
    }
}
static void graft_stress(void)
{
    Sheeted p;flow_profile_t before,after;
    char *html=malloc(180000);strcpy(html,"<!doctype html><style>p{margin:4px}</style><div style='display:grid'><article>");
    strcat(html,"<aside style='float:right;width:240px;height:120px'>infobox</aside>");
    for(int i=0;i<24;++i) {
        char row[256];snprintf(row,sizeof(row),"<p id=p%d>Article paragraph %d: alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma.</p>",i,i);
        strcat(html,row);
    }
    strcat(html,"</article></div>");sheeted_open(&p,html,800,600);
    flow_tree_t *old=graft_layout(&p,NULL,&before,NULL);
    os64_html_set_text(p.doc,graft_node(&p,"p0")->first_child,"A changed paragraph beside the infobox.",38);graft_update(&p);
    flow_tree_t *next=graft_layout(&p,old,&after,NULL);
    expect("graft: synthetic article reuses below float but measures affected paragraphs",
           after.reused>0 && after.measures>0 && after.measures<before.measures &&
           flow_reuse_stats(next).refused[FLOW_REUSE_FLOATS]>0,NULL);
    flow_free(old);graft_oracle(&p,next);flow_free(next);sheeted_close(&p);

    sheeted_open(&p,"<!doctype html><div id=outer style='display:flex;width:700px'>"
        "<div style='flex:1'><p>wrapping text to give intrinsic widths and a changing final height</p></div>"
        "<div style='flex:1'><p>another item with words</p></div></div><p>tail</p>"
        "<p>stable</p><p>stable</p><p>stable</p><p>stable</p><p>stable</p>"
        "<p>stable</p><p>stable</p><p>stable</p><p>stable</p><p>stable</p>",800,600);
    old=graft_layout(&p,NULL,NULL,NULL);
    os64_html_set_attr(p.doc,graft_node(&p,"outer"),"style","display:flex;width:300px",24);graft_update(&p);
    next=graft_layout(&p,old,&after,NULL);
    expect("graft: intrinsic memo survives different placed width without importing height",
           flow_reuse_stats(next).intrinsic_hits>0 && after.measures>0,NULL);
    flow_free(old);graft_oracle(&p,next);flow_free(next);sheeted_close(&p);

    strcpy(html,"<!doctype html><ol id=list>");
    for(int i=0;i<2000;++i)strcat(html,"<li>One row of unchanged content</li>");
    strcat(html,"</ol>");sheeted_open(&p,html,800,600);
    old=graft_layout(&p,NULL,&before,NULL);int64_t status;
    os64_html_node_t *li=os64_html_create_element(p.doc,OS64_HTML_NS_HTML,"li",&status);
    os64_html_node_t *text=os64_html_create_text(p.doc,"The appended row",16,&status);
    os64_html_insert(p.doc,li,text,NULL);os64_html_insert(p.doc,graft_node(&p,"list"),li,NULL);graft_update(&p);
    next=graft_layout(&p,old,&after,NULL);
    flow_reuse_stats_t stats=flow_reuse_stats(next);
    expect("graft: append measures the new row rather than 2000 old rows",
           before.measures>=2000 && after.measures<=2 && after.reused>=2000,NULL);
    expect("graft: unchanged outside markers avoid repeated shaping",stats.marker_measures==1,NULL);
    expect("graft: persistent and staging metadata share the ceiling",stats.metadata_bytes>0 &&
           stats.metadata_peak>=stats.metadata_bytes && stats.metadata_peak<=FLOW_ARENA_DEFAULT,NULL);
    printf("graft append: %llu -> %llu measures, %llu copied boxes, %llu marker measures, %zu peak metadata bytes\n",
           (unsigned long long)before.measures,(unsigned long long)after.measures,
           (unsigned long long)after.reused,(unsigned long long)stats.marker_measures,stats.metadata_peak);
    flow_free(old);graft_oracle(&p,next);flow_free(next);sheeted_close(&p);free(html);

    sheeted_open(&p,"<!doctype html><p id=before>before</p><p id=kept><a href=kept>kept link</a><input value=kept></p>"
        "<p>padding one</p><p>padding two</p><p>padding three</p><p>padding four</p>",800,600);
    old=graft_layout(&p,NULL,NULL,NULL);
    os64_html_node_t *anchor=os64_html_create_element(p.doc,OS64_HTML_NS_HTML,"a",&status);
    os64_html_set_attr(p.doc,anchor,"href","first",5);
    text=os64_html_create_text(p.doc,"new link",8,&status);os64_html_insert(p.doc,anchor,text,NULL);
    os64_html_insert(p.doc,graft_node(&p,"before"),anchor,NULL);
    os64_html_node_t *input=os64_html_create_element(p.doc,OS64_HTML_NS_HTML,"input",&status);
    os64_html_insert(p.doc,graft_node(&p,"before"),input,NULL);graft_update(&p);
    next=graft_layout(&p,old,&after,NULL);
    expect("graft: unaffected content survives earlier model index insertion",after.reused>0,NULL);
    flow_free(old);graft_oracle(&p,next);flow_free(next);sheeted_close(&p);
}
static void graft_prefix(const flow_tree_t *tree,const flow_tree_t *whole)
{
    if(!tree || !flow_root(tree))return;
    Flat p={0},w={0};flatten(&p,flow_root(tree),-1);flatten(&w,flow_root(whole),-1);
    bool prefix=p.n<=w.n,same=true,path=true;
    for(size_t i=0;prefix && i<p.n;++i) {
        prefix=same_item(p.v[i],w.v[i]);
        if(!p.v[i]->unfinished && !has_unfinished_below(&p,i))
            same &= !memcmp(&p.v[i]->rect,&w.v[i]->rect,sizeof(p.v[i]->rect));
        if(p.v[i]->unfinished) {
            int32_t up=(int32_t)p.n-1;
            while(up>=0 && (size_t)up!=i)up=p.parent[up];
            path &= up==(int32_t)i;
        }
    }
    expect("graft failure: visible tree is a prefix",prefix,NULL);
    expect("graft failure: completed geometry stays put",same,NULL);
    expect("graft failure: unfinished boxes lead to the last",path,NULL);
    int64_t rx=800,ry=0;reach_of(flow_root(tree),&rx,&ry);
    expect("graft failure: unplaced work does not extend page bounds",
           flow_width(tree)<=rx+1 && flow_height(tree)<=ry+1,NULL);
    free(p.v);free(p.parent);free(w.v);free(w.parent);
}
static void graft_failures(void)
{
    char html[7000];strcpy(html,"<!doctype html><p id=changed>first</p>");
    for(int i=0;i<85;++i)strcat(html,"<p>kept <a href=x><b>bold link</b></a></p>");
    Sheeted p;sheeted_open(&p,html,800,600);
    flow_tree_t *old=graft_layout(&p,NULL,NULL,NULL);
    os64_html_set_text(p.doc,graft_node(&p,"changed")->first_child,"changed first",13);graft_update(&p);
    os64_page_t *model=os64_page_rebuild(p.page);
    flow_tree_t *whole=flow_layout(p.doc,model,800,&p.env);
    flow_reuse_inputs_t input={.resource_owner=p.doc};
    // Consume the old snapshot's pending delta before fixing the ownership
    // baseline: its reader may release log entries on that first read.
    flow_tree_t *warm=flow_layout_incremental(p.doc,model,800,&p.env,old,&input,NULL,NULL,NULL);
    flow_free(warm);
    size_t held=live,points=0,partial=0;
    for(int mode=0;mode<2;++mode) {
        for(size_t at=0;at<=points;++at) {
            size_t base=allocations;fail_single=mode!=0;fail_at=at ? base+at : 0;
            flow_tree_t *trial=flow_layout_incremental(p.doc,model,800,&p.env,old,&input,NULL,NULL,NULL);
            fail_at=0;fail_single=false;
            if(!at)points=allocations-base;
            if(trial && flow_incomplete(trial)) { partial++;graft_prefix(trial,whole); }
            else if(trial) {
                int64_t n=flow_dump(trial,NULL,0),m=flow_dump(whole,NULL,0);
                char *a=malloc((size_t)n+1),*b=malloc((size_t)m+1);
                flow_dump(trial,a,(size_t)n+1);flow_dump(whole,b,(size_t)m+1);
                expect("graft failure: complete fallback matches whole",n==m && !strcmp(a,b),NULL);free(a);free(b);
            }
            flow_free(trial);expect("graft failure: ownership returns to source baseline",live==held,NULL);
        }
    }
    printf("graft sweep: %zu allocation positions, single/persistent, %zu partial trees\n",points,partial);
    expect("graft: sweep exercises private copy failures",partial>0,NULL);
    flow_tree_t *next=graft_layout(&p,old,NULL,NULL);
    flow_free(old);graft_oracle(&p,next);flow_free(next);flow_free(whole);os64_page_free(model);sheeted_close(&p);
}

static void graft_retain_failure(void)
{
    Sheeted p;sheeted_open(&p,"<!doctype html><p>retained text</p><p>tail</p>",800,600);
    flow_tree_t *old=graft_layout(&p,NULL,NULL,NULL);
    Flat flat={0};flatten(&flat,flow_root(old),-1);
    os64_text_run_t *run=NULL;
    for(size_t i=0;i<flat.n && !run;++i)run=(os64_text_run_t *)flat.v[i]->run;
    expect("graft retain: source has shaped text",run!=NULL,NULL);
    if(run) {
        size_t refs=run->refs;run->refs=SIZE_MAX;
        flow_tree_t *trial=graft_layout(&p,old,NULL,NULL);
        expect("graft retain: overflow leaves an incomplete prefix",trial && flow_incomplete(trial) &&
               flow_reuse_stats(trial).refused[FLOW_REUSE_RETAIN]>0,NULL);
        expect("graft retain: failed retain leaves source ownership intact",run->refs==SIZE_MAX,NULL);
        run->refs=refs;
        graft_prefix(trial,old);
        flow_profile_t profile;
        flow_tree_t *fresh=graft_layout(&p,trial,&profile,NULL);
        expect("graft retain: incomplete source supplies neither geometry nor intrinsic memo",
               fresh && !flow_incomplete(fresh) && !profile.reused &&
               !flow_reuse_stats(fresh).intrinsic_hits,NULL);
        flow_free(trial);flow_free(fresh);
    }
    free(flat.v);free(flat.parent);
    flow_tree_t *next=graft_layout(&p,old,NULL,NULL);
    flow_free(old);graft_oracle(&p,next);flow_free(next);sheeted_close(&p);
}

// THE RESTYLE KEPT IN THE TREE (flow_restyle_in_place) beside the check that
// judges it (flow_restyle_check). A paint-only change is written into the
// tree laid out before it, which then answers as a whole layout of the
// changed document does; before the write, the check tells the two apart and
// names the element. A change placement reads is declined, the tree left as
// it was, and the check names what moved (in a box of fixed height, so that
// no box before it in tree order moves first).
static bool restyle_check_says(const flow_tree_t *a, const flow_tree_t *b, const char *want,
                               const os64_html_node_t *at)
{
    const os64_html_node_t *node = NULL;
    const char *what = NULL;
    bool same = flow_restyle_check(a, b, &node, &what);
    if (want == NULL)
        return same && what == NULL;
    return !same && what != NULL && strcmp(what, want) == 0 && node == at;
}
static flow_restyle_t restyle_keep(Sheeted *p, flow_tree_t *tree)
{
    flow_reuse_inputs_t inputs = {.resource_owner = p->doc};
    os64_page_t *model = os64_page_rebuild(p->page);
    flow_restyle_t kept = flow_restyle_in_place(tree, p->doc, model, &p->env, &inputs, 800, NULL);
    os64_page_free(model);
    return kept;
}
static void restyle_in_place_cases(void)
{
    Sheeted p;sheeted_open(&p,"<!doctype html><style>p{margin:4px}</style><p id=a>first <b>bold</b></p>"
        "<div style='height:60px'><p id=q>second</p></div><p>tail</p>",800,600);
    os64_html_node_t *q=graft_node(&p,"q");
    flow_tree_t *old=graft_layout(&p,NULL,NULL,NULL);
    os64_html_set_attr(p.doc,q,"style","color:green",11);graft_update(&p);
    flow_tree_t *whole=graft_layout(&p,NULL,NULL,NULL);
    expect("restyle: before the write the check tells a colour apart, at its element",
           old && whole && restyle_check_says(old,whole,"a style",q),NULL);
    flow_restyle_t kept=restyle_keep(&p,old);
    expect("restyle: a colour is written in place",kept.kind==FLOW_RESTYLE_PAINT && kept.written==1,NULL);
    expect("restyle: and the kept tree answers as a whole layout",restyle_check_says(old,whole,NULL,NULL),NULL);
    flow_free(whole);
    os64_html_set_attr(p.doc,q,"style","color:green;padding-top:7px",27);graft_update(&p);
    whole=graft_layout(&p,NULL,NULL,NULL);
    expect("restyle: the check tells a padding apart, at its element",
           whole && restyle_check_says(old,whole,"a border box",q),NULL);
    kept=restyle_keep(&p,old);
    expect("restyle: a padding is declined as geometry",
           kept.kind==FLOW_RESTYLE_DECLINED && kept.why==FLOW_RESTYLE_GEOMETRY,NULL);
    flow_free(whole);flow_free(old);sheeted_close(&p);
    // One change to element `id` of `html`, restyled in the tree laid out
    // before it: what the restyle answered, and whether the kept tree then
    // answers as a whole layout of the changed document does.
    static const struct {
        const char *name, *html, *id, *style;
        flow_restyle_kind_t kind;
        flow_restyle_reason_t why;
        uint64_t later;     // the change's time, ms after the first layout's
    } one[] = {
        // Another element's colour, with the animated one's sample moved on:
        // the animation's own clock is the tick's, not a restyle's.
        {"restyle: an animated element whose sample moved is declined as motion",
         "<!doctype html><style>@keyframes k{to{color:blue}}#c{animation:k 10s linear;color:red}</style>"
         "<span id=c>moving</span><p id=o>other</p>", "o", "color:green",
         FLOW_RESTYLE_DECLINED, FLOW_RESTYLE_MOTION, 5000},
        // An outline on a positioned box that makes no context is its
        // context's to paint, which must learn that it holds one.
        {"restyle: an outline on a positioned box is kept, and held where a whole layout holds it",
         "<!doctype html><div id=r style='position:relative'>ringed</div>", "r", "position:relative;outline:2px solid red",
         FLOW_RESTYLE_PAINT, FLOW_RESTYLE_NONE, 0},
        // plus-darker is drawn as normal, with no group; multiply needs one.
        {"restyle: a blend that comes to need a group is declined as stacking",
         "<!doctype html><div id=m style='mix-blend-mode:plus-darker'>blended</div>", "m", "mix-blend-mode:multiply",
         FLOW_RESTYLE_DECLINED, FLOW_RESTYLE_STACKING, 0},
        // The child's colour at the animation's start is red either way, but
        // its `inherit` endpoint follows the parent's.
        {"restyle: an animation's inherited endpoint moving is declined as motion",
         "<!doctype html><style>@keyframes k{from{color:red}to{color:inherit}}#c{animation:k 10s linear}</style>"
         "<div id=a style='color:red'><span id=c>moving</span></div>", "a", "color:blue",
         FLOW_RESTYLE_DECLINED, FLOW_RESTYLE_MOTION, 0},
    };
    // Laid out as a face lays out a page that animates: on a timeline, its
    // animations ticking, from time 0.
    for (size_t i = 0; i < sizeof(one) / sizeof(one[0]); i++) {
        sheeted_open(&p, one[i].html, 800, 600);
        flow_timeline_t *tl = flow_timeline_new(p.doc);
        p.env.ticks = true;
        p.env.timeline = tl;
        p.env.now_ms = 0;
        old = graft_layout(&p, NULL, NULL, NULL);
        flow_timeline_commit(&tl, old);
        p.env.timeline = tl;
        os64_html_set_attr(p.doc, graft_node(&p, one[i].id), "style", one[i].style, strlen(one[i].style));
        graft_update(&p);
        p.env.now_ms = one[i].later;
        whole = graft_layout(&p, NULL, NULL, NULL);
        kept = restyle_keep(&p, old);
        bool ok = old && whole && kept.kind == one[i].kind && kept.why == one[i].why &&
                  (kept.kind == FLOW_RESTYLE_DECLINED || restyle_check_says(old, whole, NULL, NULL));
        char said[96];
        snprintf(said, sizeof(said), "kind %d why %d", (int)kept.kind, (int)kept.why);
        expect(one[i].name, ok, ok ? NULL : said);
        flow_free(whole); flow_free(old); flow_timeline_free(tl); sheeted_close(&p);
    }
    // A text under an element styled `display: none` is structure all the
    // same: a select's hidden option is what its width is measured from.
    // The decline is told at an element the tree styled.
    sheeted_open(&p, "<!doctype html><select><option id=o hidden selected>Choose</option><option>A</option>"
                     "</select><p id=c>c</p>",
                 800, 600);
    old = graft_layout(&p, NULL, NULL, NULL);
    {
        os64_html_node_t *o = graft_node(&p, "o");
        bool changed = o != NULL && o->first_child != NULL &&
                       !os64_html_set_text(p.doc, o->first_child, "Choose one of these, please", 27) &&
                       !os64_html_set_attr(p.doc, graft_node(&p, "c"), "style", "color:green", 11);
        graft_update(&p);
        kept = restyle_keep(&p, old);
        char said[96];
        snprintf(said, sizeof(said), "kind %d why %d (%s)", (int)kept.kind, (int)kept.why, kept.detail);
        expect("restyle: a hidden option's text is structure, told at an element the tree styled",
               changed && kept.kind == FLOW_RESTYLE_DECLINED && kept.why == FLOW_RESTYLE_STRUCTURE &&
                   kept.at != NULL && kept.at->kind == OS64_HTML_ELEMENT && strncmp(kept.detail, "text", 4) == 0,
               said);
    }
    flow_free(old); sheeted_close(&p);
    // Changes to a node outside the page (a fragment a script builds: its
    // children, a text, an attribute boxes read) are read by no box: beside
    // a colour in the page they keep the restyle, and the kept tree answers
    // as a whole layout does. The fragment's insertion is then structure.
    sheeted_open(&p, "<!doctype html><p id=c>c</p>", 800, 600);
    old = graft_layout(&p, NULL, NULL, NULL);
    {
        int64_t why = 0;
        os64_html_node_t *frag = os64_html_create_element(p.doc, OS64_HTML_NS_HTML, "div", &why);
        os64_html_node_t *link = os64_html_create_element(p.doc, OS64_HTML_NS_HTML, "a", &why);
        os64_html_node_t *words = os64_html_create_text(p.doc, "built", 5, &why);
        bool changed = frag && link && words && !os64_html_insert(p.doc, frag, link, NULL) &&
                       !os64_html_insert(p.doc, link, words, NULL) && !os64_html_set_attr(p.doc, link, "href", "x", 1) &&
                       !os64_html_set_text(p.doc, words, "built again", 11) &&
                       !os64_html_set_attr(p.doc, graft_node(&p, "c"), "style", "color:green", 11);
        graft_update(&p);
        whole = graft_layout(&p, NULL, NULL, NULL);
        kept = restyle_keep(&p, old);
        char said[96];
        snprintf(said, sizeof(said), "kind %d why %d (%s)", (int)kept.kind, (int)kept.why, kept.detail);
        expect("restyle: changes to a fragment outside the page keep a colour beside them, as a whole layout",
               changed && kept.kind == FLOW_RESTYLE_PAINT && restyle_check_says(old, whole, NULL, NULL), said);
        flow_free(whole);
        bool inserted = changed && !os64_html_insert(p.doc, p.doc->body, frag, NULL);
        (void)garb_cascade_update(p.cascade); /* structure is asked before the cascade */
        kept = restyle_keep(&p, old);
        expect("restyle: the fragment's insertion is structure, told at the element it went into",
               inserted && kept.why == FLOW_RESTYLE_STRUCTURE && kept.at == p.doc->body &&
                   strcmp(kept.detail, "children") == 0,
               kept.detail);
    }
    flow_free(old); sheeted_close(&p);
    // A node removed from the page and changed after: the removal is in the
    // log, and is structure.
    sheeted_open(&p, "<!doctype html><p id=r>gone</p><p id=c>c</p>", 800, 600);
    old = graft_layout(&p, NULL, NULL, NULL);
    {
        os64_html_node_t *r = graft_node(&p, "r");
        bool changed = r != NULL && !os64_html_remove(p.doc, r) && !os64_html_set_text(p.doc, r->first_child, "x", 1);
        (void)garb_cascade_update(p.cascade); /* structure is asked before the cascade */
        kept = restyle_keep(&p, old);
        expect("restyle: a node removed then changed is declined for its removal",
               changed && kept.why == FLOW_RESTYLE_STRUCTURE && strcmp(kept.detail, "children") == 0, kept.detail);
    }
    flow_free(old); sheeted_close(&p);
    // A host's child in no slot is in the page, though no box is made for
    // it: its `slot` attribute set to a slot's name puts it on screen, and
    // is structure.
    sheeted_open(&p, "<!doctype html><div id=h><span id=u>unassigned</span></div><p id=c>c</p>", 800, 600);
    {
        int64_t why = 0;
        os64_html_node_t *host = graft_node(&p, "h"), *u = graft_node(&p, "u");
        os64_html_node_t *root = host != NULL ? os64_html_attach_shadow(p.doc, host, OS64_HTML_SHADOW_OPEN, NULL) : NULL;
        os64_html_node_t *slot = root != NULL ? os64_html_create_element(p.doc, OS64_HTML_NS_HTML, "slot", &why) : NULL;
        bool built = slot && !os64_html_set_attr(p.doc, slot, "name", "x", 1) && !os64_html_insert(p.doc, root, slot, NULL);
        (void)garb_cascade_update(p.cascade); /* a shadow root may cost a fresh build */
        old = graft_layout(&p, NULL, NULL, NULL);
        bool unshown = built && old != NULL && flow_box_for(old, u) == NULL;
        bool changed = unshown && !os64_html_set_attr(p.doc, u, "slot", "x", 1);
        (void)garb_cascade_update(p.cascade); /* structure is asked before the cascade */
        kept = restyle_keep(&p, old);
        expect("restyle: a host's child in no slot given a slot's name is structure",
               changed && kept.why == FLOW_RESTYLE_STRUCTURE && strncmp(kept.detail, "attribute slot", 14) == 0, kept.detail);
        flow_free(old);
    }
    sheeted_close(&p);
    // A tree's records point into the cascade it was laid out in (a
    // family's names are its strings), which a host frees with the tree: a
    // colour changed under a cascade built fresh is declined, not kept in a
    // tree that would outlive what its other records read.
    sheeted_open(&p, "<!doctype html><style>p{font-family:Serif}#c.on{color:red}</style><p id=c>c</p><p>d</p>",
                 800, 600);
    old = graft_layout(&p, NULL, NULL, NULL);
    os64_html_set_attr(p.doc, graft_node(&p, "c"), "class", "on", 2);
    graft_update(&p);
    {
        garb_sheet_in_t sheets[16];
        for (int32_t i = 0; i < p.n; ++i) sheets[i] = (garb_sheet_in_t){.sheet = &p.sheets[i]};
        garb_cascade_t *fresh = garb_cascade(sheets, p.n, p.doc, garb_cascade_env(p.cascade));
        garb_cascade_t *had = p.cascade;
        p.cascade = fresh;
        p.env.cascade = fresh;
        kept = restyle_keep(&p, old);
        expect("restyle: a colour under a cascade built fresh is declined as another cascade",
               fresh != NULL && kept.kind == FLOW_RESTYLE_DECLINED && kept.why == FLOW_RESTYLE_INPUTS &&
                   strcmp(kept.detail, "another cascade") == 0 && kept.written == 0, kept.detail);
        p.cascade = had;
        p.env.cascade = had;
        garb_cascade_free(fresh);
    }
    flow_free(old); sheeted_close(&p);
    // A positioned pseudo-element is its own anchor, and its node is made
    // again by every style pass: a colour written beside it is kept, and
    // the kept tree answers as a whole layout does.
    sheeted_open(&p, "<!doctype html><style>#m{position:relative}#m::before{content:'';position:absolute;"
                     "inset:0}#m.hot{color:red}</style><div id=m>menu</div>",
                 800, 600);
    old = graft_layout(&p, NULL, NULL, NULL);
    os64_html_set_attr(p.doc, graft_node(&p, "m"), "class", "hot", 3);
    graft_update(&p);
    whole = graft_layout(&p, NULL, NULL, NULL);
    kept = restyle_keep(&p, old);
    {
        char said[96];
        snprintf(said, sizeof(said), "kind %d why %d (%s)", (int)kept.kind, (int)kept.why, kept.detail);
        expect("restyle: a positioned ::before is its own anchor in both builds, and a colour beside it is kept",
               kept.kind == FLOW_RESTYLE_PAINT && restyle_check_says(old, whole, NULL, NULL), said);
    }
    flow_free(whole); flow_free(old); sheeted_close(&p);
    // The check names an outline a kept tree does not hold where a whole
    // layout holds one: two layouts of one document, one with the root's
    // mark cleared, as a restyle that did not hand an outline up would
    // leave it.
    sheeted_open(&p, "<!doctype html><div style='position:relative;outline:2px solid red'>ringed</div>", 800, 600);
    old = graft_layout(&p, NULL, NULL, NULL);
    whole = graft_layout(&p, NULL, NULL, NULL);
    if (old != NULL)
        ((flow_box_t *)flow_root(old))->holds_outline = false;
    expect("restyle: the check names an outline not held",
           old && whole && flow_root(whole)->holds_outline &&
           restyle_check_says(old, whole, "an outline not held", flow_root(old)->node), NULL);
    flow_free(whole); flow_free(old); sheeted_close(&p);
}

// WHAT IS PENDING (flow_pending): a tree laid out as a face lays out a page
// that animates (on a timeline, ticking), the document written after it
// with the cascade not brought up to date, and the verdict asked.
static flow_pending_verdict_t pending_after(const char *html, const char *id, const char *attr, const char *value,
                                            bool *transforms)
{
    Sheeted p;sheeted_open(&p,html,800,600);
    flow_timeline_t *tl=flow_timeline_new(p.doc);
    p.env.ticks=true; p.env.timeline=tl; p.env.now_ms=0;
    flow_tree_t *t=graft_layout(&p,NULL,NULL,NULL);
    flow_timeline_commit(&tl,t);
    os64_html_set_attr(p.doc,graft_node(&p,id),attr,value,strlen(value));
    flow_pending_t after=flow_pending(t,p.doc,p.cascade);
    if(transforms) *transforms=after.transforms;
    flow_pending_verdict_t v=t ? after.verdict : (flow_pending_verdict_t)-1;
    flow_free(t);flow_timeline_free(tl);sheeted_close(&p);
    return v;
}
static void pending_cases(void)
{
    const char *page="<!doctype html><style>.m{animation:k 10s}@keyframes k{to{opacity:.5}}</style>"
        "<div id=box style='position:relative'><p id=t style='transform:translateX(5px);color:red'>moving</p>"
        "<p id=n style='color:red'>still</p><span id=i style='color:red'>inline</span><p id=m class=m>animated</p></div>";
    bool tf=false;
    expect("pending: a colour written is PAINT",
           pending_after(page,"n","style","color:blue",&tf)==FLOW_PENDING_PAINT && !tf,NULL);
    expect("pending: a transform moved on its side of none is PAINT, and says transforms",
           pending_after(page,"t","style","transform:translateX(9px);color:red",&tf)==FLOW_PENDING_PAINT && tf,NULL);
    expect("pending: a transform from none is RESTYLE (a containing block appears)",
           pending_after(page,"n","style","color:red;transform:scale(2)",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a transform to none is RESTYLE",
           pending_after(page,"t","style","transform:none;color:red",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a filter from none is RESTYLE (a containing block appears), on a plain inline too",
           pending_after(page,"n","style","color:red;filter:blur(1px)",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(page,"i","style","color:red;filter:blur(1px)",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a CSS-wide keyword on the transform or filter family is RESTYLE (its side is not known)",
           pending_after(page,"t","style","transform:initial;color:red",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(page,"t","style","transform:unset;color:red",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(page,"t","style","transform:inherit;color:red",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a CSS-wide keyword on a plain longhand is RESTYLE too (its value comes from elsewhere)",
           pending_after(page,"n","style","color:inherit",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(page,"n","style","color:initial",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(page,"n","style","color:unset",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(page,"n","style","color:revert",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a transform's origin is PAINT, and says transforms (it moves the reach)",
           pending_after(page,"t","style","transform:translateX(5px);color:red;transform-origin:0 0",&tf)==FLOW_PENDING_PAINT && tf,NULL);
    expect("pending: a transform on a plain inline is PAINT (no containing block can come or go)",
           pending_after(page,"i","style","color:red;transform:scale(2)",NULL)==FLOW_PENDING_PAINT,NULL);
    expect("pending: a padding is RESTYLE",
           pending_after(page,"n","style","color:red;padding:4px",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a declaration removed is RESTYLE (the sheets' value is not known)",
           pending_after(page,"t","style","color:red",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: var() is RESTYLE",
           pending_after(page,"n","style","color:var(--x)",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: visibility to hidden is PAINT, to collapse RESTYLE",
           pending_after(page,"n","style","color:red;visibility:hidden",NULL)==FLOW_PENDING_PAINT &&
           pending_after(page,"n","style","color:red;visibility:collapse",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a class written is RESTYLE",
           pending_after(page,"n","class","on",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: an animated element's style is RESTYLE",
           pending_after(page,"m","style","color:blue",NULL)==FLOW_PENDING_RESTYLE,NULL);
    char reads[2048];
    snprintf(reads,sizeof(reads),"%s<style>[style*=blue]{width:10px}</style>",page);
    expect("pending: a selector reading the style attribute makes every write RESTYLE",
           pending_after(reads,"n","style","color:blue",NULL)==FLOW_PENDING_RESTYLE,NULL);
    /* A transform libflow cannot work out keeps its old value, so a value
     * other than none may compute to none: a write is PAINT only where no
     * out-of-flow box could have the element as its containing block. */
    const char *held="<!doctype html><div id=c style='transform:translateX(10px)'>"
        "<div style='position:absolute;left:0;width:50%'>abs</div></div>"
        "<div id=e style='transform:translateX(10px)'><p>flow</p></div>"
        "<div id=r style='position:relative;transform:translateX(10px)'><div style='position:absolute'>a</div></div>"
        "<div id=f style='position:relative;transform:translateX(10px)'><div style='position:fixed'>f</div></div>"
        "<div id=g style='filter:blur(1px)'><div style='position:absolute'>g</div></div>";
    expect("pending: a transform write over an absolute box it holds is RESTYLE (it may compute to none)",
           pending_after(held,"c","style","transform:translateX(min(50%,100px))",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: and over in-flow boxes alone is PAINT",
           pending_after(held,"e","style","transform:translateX(min(50%,100px))",NULL)==FLOW_PENDING_PAINT,NULL);
    expect("pending: a positioned box's absolute ones keep it as their block",
           pending_after(held,"r","style","position:relative;transform:translateX(20px)",NULL)==FLOW_PENDING_PAINT,NULL);
    expect("pending: its fixed ones do not",
           pending_after(held,"f","style","position:relative;transform:translateX(20px)",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending: a filter write over an absolute box it holds is RESTYLE too",
           pending_after(held,"g","style","filter:blur(2px)",NULL)==FLOW_PENDING_RESTYLE,NULL);
    /* N2a: a style reader narrows to the elements it may match, by its
     * exact tests (a type, an id, a class, another attribute). */
    const char *mute="<!doctype html><style>.area:has(>.mute[style*='display: none']){width:9px}"
        ".cap[style*=red]{margin:3px}</style><div class=area><p id=mu class=mute>m</p></div>"
        "<p id=cap class=cap>c</p><p id=n style='color:red'>n</p><p id=h class='hov'>h</p>";
    expect("pending N2a: a write on an element a style reader may match is RESTYLE (inside :has())",
           pending_after(mute,"mu","style","display:none",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending N2a: and plain",pending_after(mute,"cap","style","color:blue",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending N2a: a write on an element no style reader may match is judged as ever",
           pending_after(mute,"n","style","color:blue",NULL)==FLOW_PENDING_PAINT,NULL);
    {
        Sheeted p;sheeted_open(&p,mute,800,600);
        flow_tree_t *t=graft_layout(&p,NULL,NULL,NULL);
        os64_html_set_attr(p.doc,graft_node(&p,"mu"),"style","display:none",12);
        flow_pending_t said=flow_pending(t,p.doc,p.cascade);
        expect("pending N2a: says so, and where",
               said.style_read && !strcmp(said.style_read_at,"p#mu.mute") &&
               !strcmp(said.why,"restyle: a selector may read its style"),NULL);
        flow_free(t);sheeted_close(&p);
    }
    /* A pseudo-class never narrows: the element is not hovered, and the
     * reader is still taken to match it. */
    const char *hover="<!doctype html><style>.hov:hover[style*=red]{width:9px}</style>"
        "<p id=h class=hov>h</p><p id=n style='color:red'>n</p>";
    expect("pending N2a: a pseudo-class does not narrow",
           pending_after(hover,"h","style","color:blue",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(hover,"n","style","color:blue",NULL)==FLOW_PENDING_PAINT,NULL);
    /* The cascade's own matchers: a quirks-mode document folds a class's
     * case, and so does the reader. */
    const char *quirks="<style>.CAP[style*=red]{margin:3px}</style><p id=cap class=cap>c</p><p id=n style='color:red'>n</p>";
    expect("pending N2a: matched as the cascade matches (quirks folds a class)",
           pending_after(quirks,"cap","style","color:blue",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(quirks,"n","style","color:blue",NULL)==FLOW_PENDING_PAINT,NULL);
    /* Bare readers narrow to no element: every write is RESTYLE. */
    const char *bare_not="<!doctype html><style>.a:not([style*=x]){width:9px}</style><p id=n style='color:red'>n</p>";
    const char *bare_host="<!doctype html><style>:host([style*=x]){width:9px}</style><p id=n style='color:red'>n</p>";
    const char *bare_slot="<!doctype html><style>::slotted(.s[style*=x]){width:9px}</style><p id=n style='color:red'>n</p>";
    expect("pending N2a: a reader inside :not() alone is bare",
           pending_after(bare_not,"n","style","color:blue",NULL)==FLOW_PENDING_RESTYLE,NULL);
    expect("pending N2a: :host() and ::slotted() readers are bare",
           pending_after(bare_host,"n","style","color:blue",NULL)==FLOW_PENDING_RESTYLE &&
           pending_after(bare_slot,"n","style","color:blue",NULL)==FLOW_PENDING_RESTYLE,NULL);
    {
        /* Past GARB_STYLE_READERS readers, bare. */
        static char many[GARB_STYLE_READERS*40+200];
        size_t at=(size_t)snprintf(many,sizeof(many),"<!doctype html><style>");
        for(int k=0;k<=GARB_STYLE_READERS;k++)
            at+=(size_t)snprintf(many+at,sizeof(many)-at,".r%d[style*=x]{width:9px}",k);
        snprintf(many+at,sizeof(many)-at,"</style><p id=n style='color:red'>n</p>");
        expect("pending N2a: past the readers' cap, bare",
               pending_after(many,"n","style","color:blue",NULL)==FLOW_PENDING_RESTYLE,NULL);
    }
    // Made current again by a restyle kept in the tree: nothing pending,
    // and a write after it judged from there.
    Sheeted p;sheeted_open(&p,page,800,600);
    flow_tree_t *t=graft_layout(&p,NULL,NULL,NULL);
    expect("pending: nothing after a layout",t && flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_NONE,NULL);
    os64_html_node_t *n=graft_node(&p,"n");
    os64_html_set_attr(p.doc,n,"class","on",2);
    expect("pending: RESTYLE before the restyle",flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_RESTYLE,NULL);
    graft_update(&p);
    flow_restyle_t kept=restyle_keep(&p,t);
    expect("pending: a restyle kept makes the tree current",
           kept.kind!=FLOW_RESTYLE_DECLINED && flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_NONE,NULL);
    os64_html_set_attr(p.doc,n,"style","color:green",11);
    // In the same batch, another element's style written back to what the
    // cascade parsed (the text differs, the declarations do not): that one
    // is NONE, and the verdict stays PAINT.
    const char *again="transform:translateX(5px);color:red;";
    os64_html_set_attr(p.doc,graft_node(&p,"t"),"style",again,strlen(again));
    expect("pending: and a write after it is judged from there, a NONE beside it leaving it PAINT",
           flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_PAINT,NULL);
    {
        flow_pending_t said=flow_pending(t,p.doc,p.cascade);
        expect("pending: PAINT says it was a style write, and where",
               !strcmp(said.why,"paint: a style write") && said.at[0]!='\0',NULL);
    }
    /* NONE advanced (flow_pending_advance): the same declarations in other
     * text make the tree and its cascade current at the document's version
     * with nothing laid out, and a NONE after it is still NONE (the cascade
     * where the tree is); a verdict that is not NONE advances nothing. */
    graft_update(&p);
    kept=restyle_keep(&p,t);
    {
        const os64_html_attr_t *a=os64_html_attr(n,"style");
        char again[256];
        snprintf(again,sizeof(again),"%s ",a!=NULL && a->value!=NULL ? a->value : "");
        os64_html_set_attr(p.doc,n,"style",again,strlen(again));
        flow_pending_t judged;
        expect("pending: the same declarations in other text advance the tree with no layout",
               flow_pending_advance(t,p.doc,p.cascade,&judged) && judged.verdict==FLOW_PENDING_NONE &&
               garb_cascade_version(p.cascade)==os64_html_version(p.doc),NULL);
        expect("pending: and leave it NONE, its cascade where the tree is",
               flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_NONE,NULL);
        os64_html_set_attr(p.doc,n,"style","color:purple",12);
        expect("pending: a verdict that is not NONE advances nothing",
               !flow_pending_advance(t,p.doc,p.cascade,&judged) && judged.verdict!=FLOW_PENDING_NONE &&
               garb_cascade_version(p.cascade)!=os64_html_version(p.doc),NULL);
    }
    /* The log lost: everything is owed, never nothing pending, and it stays
     * so until the tree is made current again, whatever is written after. */
    graft_update(&p);
    kept=restyle_keep(&p,t);
    expect("pending: current again before the loss",
           kept.kind!=FLOW_RESTYLE_DECLINED && flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_NONE,NULL);
    os64_html_changes_limit(p.doc,0);
    os64_html_set_attr(p.doc,n,"style","color:blue",10);
    os64_html_changes_limit(p.doc,65536);
    {
        flow_pending_t lost=flow_pending(t,p.doc,p.cascade);
        expect("pending: a lost log is RESTYLE, and says so",
               lost.verdict==FLOW_PENDING_RESTYLE && lost.lost && !strcmp(lost.why,"restyle: the log was lost"),NULL);
        expect("pending: and advances nothing",!flow_pending_advance(t,p.doc,p.cascade,NULL),NULL);
        os64_html_set_attr(p.doc,n,"style","color:green",11);
        lost=flow_pending(t,p.doc,p.cascade);
        expect("pending: and stays RESTYLE whatever is written after",
               lost.verdict==FLOW_PENDING_RESTYLE && !lost.lost,NULL);
    }
    os64_html_set_text(p.doc,n->first_child,"x",1);
    expect("pending: a text change is RESTYLE",flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_RESTYLE,NULL);
    os64_html_set_attr(p.doc,n,"style","color:blue",10);
    expect("pending: and a paint write after it leaves it RESTYLE",
           flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_RESTYLE,NULL);
    flow_free(t);sheeted_close(&p);
    // The cascade brought up to date without the tree: its parse is no
    // longer the tree's, so nothing can be judged against it.
    sheeted_open(&p,page,800,600);
    t=graft_layout(&p,NULL,NULL,NULL);
    os64_html_set_attr(p.doc,graft_node(&p,"n"),"style","color:blue",10);
    graft_update(&p);
    expect("pending: a cascade at another version than the tree's is RESTYLE",
           flow_pending(t,p.doc,p.cascade).verdict==FLOW_PENDING_RESTYLE,NULL);
    flow_free(t);sheeted_close(&p);
    // A style write judged RESTYLE names what made it one.
    sheeted_open(&p,page,800,600);
    t=graft_layout(&p,NULL,NULL,NULL);
    {
        os64_html_node_t *at=graft_node(&p,"n");
        const os64_html_attr_t *a=os64_html_attr(at,"style");
        char wider[256];
        snprintf(wider,sizeof(wider),"%s;width:31px",a!=NULL && a->value!=NULL ? a->value : "");
        os64_html_set_attr(p.doc,at,"style",wider,strlen(wider));
        flow_pending_t said=flow_pending(t,p.doc,p.cascade);
        expect("pending: a style write judged RESTYLE names the longhand that made it one",
               said.verdict==FLOW_PENDING_RESTYLE && !strcmp(said.why,"restyle: a style write: width"),said.why);
    }
    flow_free(t);sheeted_close(&p);
}
