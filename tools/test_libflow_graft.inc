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
        // A drawn stamp is the tree's own count of its writes: a kept tree's
        // moves as its boxes are written, and says nothing of the page.
        x.drawn=y.drawn=0;
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
// A WRITE THAT CHANGED ONLY AN ELEMENT'S INSETS (restyle.inc keep_move,
// BRIEF_LAYOUT_NEEDS.md N4): taken where nothing reads the insets (no box,
// a static box) and where a positioned block-level box only moves, the kept
// tree then answering as a whole layout of the changed document does,
// every box, overflow, clip, layer, extent and range (flow_restyle_check);
// declined as geometry, saying which case, everywhere else. A row names its
// style before and the one written (or, where it names an attribute, that
// attribute written with `after` under the row's sheet), and `inside` the
// markup in the element. One under a `display: none` ancestor is answered
// as no style having come out different, and nothing is written.
static void restyle_offset_cases(void)
{
    static const struct {
        const char *before, *after, *said;      // `said` NULL: taken (or, hidden, nothing)
        bool hidden;
        const char *tag, *sheet, *attr, *inside;
        uint64_t moved;
    } kRows[] = {
        {"left:0px", "left:5px", NULL, false, NULL, NULL, NULL, NULL, 0},
        {"position:relative;left:0px", "position:relative;left:0.5px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:relative;left:2px", "position:relative;left:6px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:relative;left:0px", "position:relative;left:8.5px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:relative;top:0px", "position:relative;top:100px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:relative;left:3px", "position:relative;left:3px;right:9px", NULL, false, NULL, NULL, NULL, NULL, 0},
        {"position:relative;left:1%", "position:relative;left:2%", "an inset, a percentage", false, NULL, NULL, NULL, NULL, 0},
        {"position:relative;left:1px", "position:relative;right:1px", "an inset, another side places it", false, NULL, NULL, NULL, NULL, 0},
        {"position:relative;left:1px;right:2px", "position:relative;right:5px", "an inset, another side places it", false, NULL, NULL, NULL, NULL, 0},
        {"position:sticky;top:0px", "position:sticky;top:10px", "an inset, sticky", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;width:100%;left:0px", "position:absolute;width:100%;left:30px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:absolute;width:50px;left:0px;transform:scaleX(0.5)", "position:absolute;width:50px;left:1.37px;transform:scaleX(0.7)",
         NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:absolute;width:10px;top:50%;left:0px", "position:absolute;width:10px;top:50%;left:10px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:absolute;left:0px;top:0px", "position:absolute;left:0px;top:5px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:absolute;width:10px;top:0px;bottom:0px;left:0px", "position:absolute;width:10px;top:0px;bottom:0px;left:5px",
         NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:fixed;width:20px;top:0px", "position:fixed;width:20px;top:3px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:absolute;width:20px;bottom:0px", "position:absolute;width:20px;bottom:7.25px", NULL, false, NULL, NULL, NULL, NULL, 1},
        {"position:absolute;left:0px", "position:absolute;left:30px", "an inset, its width follows them", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;height:auto;top:0px;bottom:0px;width:5px", "position:absolute;height:auto;top:4px;bottom:0px;width:5px",
         "an inset, its height follows them", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;width:50px;left:0px;right:0px;margin:0 auto", "position:absolute;width:50px;left:10px;right:0px;margin:0 auto",
         "an inset, auto margins place it", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;width:50px;left:0px;right:0px;margin-left:auto", "position:absolute;width:50px;left:10px;right:0px;margin-left:auto",
         "an inset, auto margins place it", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;width:50px;left:0px;margin:0 auto", "position:absolute;width:50px;right:0px;margin:0 auto",
         "an inset, another side places it", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;display:table;left:0px", "position:absolute;display:table;left:5px", "an inset, a table", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;aspect-ratio:2;height:20px;left:0px", "position:absolute;aspect-ratio:2;height:20px;left:5px",
         "an inset, an aspect ratio", false, NULL, NULL, NULL, NULL, 0},
        {"position:relative;left:0px", "position:relative;left:3px", "an inset, a relative inline", false, "span", NULL, NULL, NULL, 0},
        {"position:relative;display:inline-block;left:0px", "position:relative;display:inline-block;left:3px",
         "an inset, a relative inline", false, "span", NULL, NULL, NULL, 0},
        {"position:relative;display:table-row;left:0px", "position:relative;display:table-row;left:3px",
         "an inset, laid out unpositioned", false, "div", NULL, NULL, NULL, 0},
        {"", "on", "an inset, a pseudo-element", false, "div",
         "#m::before{content:'x';position:relative;left:0} #m.on::before{left:4px}", "class", NULL, 0},
        {"position:relative;left:0px", "position:relative;left:9px", NULL, true, NULL, NULL, NULL, NULL, 0},
        {"display:none;position:relative;left:0px", "display:none;position:relative;left:9px", NULL, false, NULL, NULL, NULL, NULL, 0},
        {"display:contents;position:relative;left:0px", "display:contents;position:relative;left:9px", NULL, false, NULL, NULL, NULL, NULL, 0},
        // What is in it moves with it: lines, an inline's pieces, a nested
        // absolute box, an inline-block's content; and what it cannot carry.
        {"position:absolute;width:120px;left:0px", "position:absolute;width:120px;left:11.6px", NULL, false, NULL, NULL, NULL,
         "one <b>two</b> three<div style='position:absolute;left:3px;top:4px;width:9px'>n</div>"
         "<span style='display:inline-block;width:12px'>i</span>", 1},
        {"position:absolute;width:120px;left:0px", "position:absolute;width:120px;left:4px", "an inset, a clip inside", false, NULL, NULL, NULL,
         "<div style='overflow:clip;height:5px'>x</div>", 0},
        {"position:absolute;width:120px;left:0px;overflow:clip", "position:absolute;width:120px;left:4px;overflow:clip",
         "an inset, a clip", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;width:120px;left:0px;overflow:hidden", "position:absolute;width:120px;left:4px;overflow:hidden",
         "an inset, a scroll container", false, NULL, NULL, NULL, NULL, 0},
        {"position:absolute;width:120px;left:0px", "position:absolute;width:120px;left:4px", "an inset, a scroll container inside", false, NULL, NULL, NULL,
         "<div style='overflow:auto;height:5px'>x<br>y</div>", 0},
        {"position:absolute;width:120px;left:0px", "position:absolute;width:120px;left:4px", "an inset, a fixed box inside", false, NULL, NULL, NULL,
         "<div style='position:fixed;top:0;width:5px'>f</div>", 0},
        {"position:absolute;width:120px;left:0px", "position:absolute;width:120px;left:4px", "an inset, a transform inside", false, NULL, NULL, NULL,
         "<div style='transform:rotate(3deg)'>t</div>", 0},
        {"position:absolute;width:120px;left:0px", "position:absolute;width:120px;left:4px", "an inset, a sticky box inside", false, NULL, NULL, NULL,
         "<div style='position:sticky;top:0'>s</div>", 0},
    };
    for (size_t i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++) {
        const char *tag = kRows[i].tag != NULL ? kRows[i].tag : "div";
        const char *attr = kRows[i].attr != NULL ? kRows[i].attr : "style";
        char html[1024];
        snprintf(html, sizeof(html), "<!doctype html><style>%s</style><div style='position:relative;width:400px;height:100px'%s>"
                 "<%s id=m %s='%s'>%s</%s></div><p id=c>c</p>", kRows[i].sheet != NULL ? kRows[i].sheet : "",
                 kRows[i].hidden ? " hidden" : "", tag, attr, kRows[i].before,
                 kRows[i].inside != NULL ? kRows[i].inside : "m", tag);
        Sheeted p;
        sheeted_open(&p, html, 800, 600);
        flow_tree_t *old = graft_layout(&p, NULL, NULL, NULL);
        bool changed = !os64_html_set_attr(p.doc, graft_node(&p, "m"), attr, kRows[i].after, strlen(kRows[i].after));
        graft_update(&p);
        flow_restyle_t kept = restyle_keep(&p, old);
        char name[200];
        snprintf(name, sizeof(name), "restyle: an inset-only write, %s (%s)",
                 kRows[i].hidden ? "under a display: none ancestor, no style comes out different"
                 : kRows[i].said != NULL ? kRows[i].said : "taken", kRows[i].after);
        bool ok = old != NULL && changed;
        if (kRows[i].hidden)
            ok = ok && kept.kind == FLOW_RESTYLE_NOTHING;
        else if (kRows[i].said != NULL)
            ok = ok && kept.kind == FLOW_RESTYLE_DECLINED && kept.why == FLOW_RESTYLE_GEOMETRY &&
                 strcmp(kept.detail, kRows[i].said) == 0;
        else {
            // Taken: the kept tree is a whole layout of the changed page.
            flow_tree_t *whole = graft_layout(&p, NULL, NULL, NULL);
            const os64_html_node_t *at = NULL;
            const char *what = NULL;
            ok = ok && kept.kind == FLOW_RESTYLE_PAINT && kept.moved == kRows[i].moved && whole != NULL &&
                 flow_restyle_check(old, whole, &at, &what);
            if (what != NULL)
                printf("  restyle row %zu: %s\n", i, what);
            flow_free(whole);
        }
        expect(name, ok, kept.detail);
        flow_free(old);
        sheeted_close(&p);
    }
}

// MOVES IN A ROW (N4): each write's move adds to what the box moved since
// the layout, a box moved inside a moved box is where both put it, a box
// moved back is where it began, its own transform follows it, and a
// layout that grafts from a tree holding moves places what moved afresh;
// after each, the kept tree is an independent whole layout of the page
// (graft_oracle: the dump, every public box, the painter's walk) and the
// kept-tree check agrees.
static void restyle_move_sequences(void)
{
    // Paragraphs enough that the cascade's retained budget (its initial
    // state's bytes) outlasts the row of updates, which then stay updates.
    static char html[16384];
    size_t len = (size_t)snprintf(html, sizeof(html), "<!doctype html><style>p{color:red;padding:2px}</style>"
                     "<div style='position:relative;width:400px;height:300px'>"
                     "<div id=a style='position:absolute;width:200px;left:0px;top:0px;transform:scaleX(0.9)'>alpha <b>beta</b>"
                     "<div id=b style='position:absolute;width:50px;left:10px;top:5px'>inner <i>x</i></div>"
                     "<span style='display:inline-block;width:20px'>ib</span></div></div><p id=c>tail text</p>");
    for (int i = 0; i < 400 && len < sizeof(html) - 64; i++)
        len += (size_t)snprintf(html + len, sizeof(html) - len, "<p>%d</p>", i);
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    flow_tree_t *old = graft_layout(&p, NULL, NULL, NULL);
    static const struct {
        const char *id, *style;
        uint64_t moved;
    } kSteps[] = {
        {"a", "position:absolute;width:200px;left:3.3px;top:0px;transform:scaleX(0.9)", 1},
        {"a", "position:absolute;width:200px;left:7.9px;top:0.6px;transform:scaleX(0.9)", 1},
        {"b", "position:absolute;width:50px;left:12.25px;top:5px", 1},
        {"a", "position:absolute;width:200px;left:0.4px;top:0px;transform:scaleX(0.75)", 1},
        {"b", "position:absolute;width:50px;left:10px;top:5px", 1},
        {"a", "position:absolute;width:200px;left:0px;top:0px;transform:scaleX(0.9)", 1},
        {"a", "position:absolute;width:200px;left:631.269px;top:2px;transform:scaleX(0.31)", 1},
    };
    for (size_t i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]) && old != NULL; i++) {
        os64_html_set_attr(p.doc, graft_node(&p, kSteps[i].id), "style", kSteps[i].style, strlen(kSteps[i].style));
        graft_update(&p);
        flow_restyle_t kept = restyle_keep(&p, old);
        flow_tree_t *whole = graft_layout(&p, NULL, NULL, NULL);
        const os64_html_node_t *at = NULL;
        const char *what = NULL;
        char name[160];
        snprintf(name, sizeof(name), "restyle: moves in a row, step %zu (%s %s)", i + 1, kSteps[i].id, kSteps[i].style);
        expect(name, kept.kind == FLOW_RESTYLE_PAINT && kept.moved == kSteps[i].moved && whole != NULL &&
                         flow_restyle_check(old, whole, &at, &what), what != NULL ? what : kept.detail);
        graft_oracle(&p, old);
        flow_free(whole);
    }
    // A change placement reads, laid out grafting from the tree that holds
    // the moves: the moved boxes are placed afresh, where they stand.
    os64_html_set_attr(p.doc, graft_node(&p, "c"), "style", "padding-top:9px", 15);
    graft_update(&p);
    flow_restyle_t kept = restyle_keep(&p, old);
    expect("restyle: moves in a row, then a padding is declined", kept.kind == FLOW_RESTYLE_DECLINED, NULL);
    flow_tree_t *next = graft_layout(&p, old, NULL, NULL);
    flow_free(old);
    graft_oracle(&p, next);
    flow_free(next);
    sheeted_close(&p);
}

// THE MOVES' WALK (N4): a seeded walk of inset writes over positioned
// boxes of the table's rows (absolute with a length or percentage width,
// relative blocks, one inside another, one transformed, text, an inline
// and an inline-block inside), some of them writes a move does not take
// (a percentage, the other side, a width that follows). After every kept
// restyle the tree is a whole layout of the page (flow_restyle_check, and
// graft_oracle every fifth); after a declined one the page is laid out
// grafting from the tree that held the moves, and that too is held to a
// whole layout.
static void restyle_move_walk(void)
{
    static char html[16384];
    size_t len = (size_t)snprintf(html, sizeof(html), "<!doctype html><style>p{color:red;padding:2px}</style>"
        "<div style='position:relative;width:500px;height:400px'>"
        "<div id=w0 style='position:absolute;width:40%%;left:0px;top:0px'>w0 <b>bold</b> text"
        "<div id=w1 style='position:absolute;width:60px;left:5px;top:7px'>w1 <i>in</i></div></div>"
        "<div id=w2 style='position:relative;left:0px;top:0px'>w2 <span style='display:inline-block;width:9px'>i</span></div>"
        "<div id=w3 style='position:absolute;width:80px;top:0px;right:0px;transform:rotate(4deg)'>w3</div>"
        "<div id=w4 style='position:absolute;width:30px;height:20px;bottom:0px;left:0px'>w4</div></div>");
    for (int i = 0; i < 400 && len < sizeof(html) - 64; i++)
        len += (size_t)snprintf(html + len, sizeof(html) - len, "<p>%d</p>", i);
    static const char *const kBase[5] = {
        "position:absolute;width:40%%;left:%s;top:%s", "position:absolute;width:60px;left:%s;top:%s",
        "position:relative;left:%s;top:%s", "position:absolute;width:80px;top:%s;right:%s;transform:rotate(4deg)",
        "position:absolute;width:30px;height:20px;bottom:%s;left:%s"};
    static const char *const kValues[] = {"0px", "1px", "0.3px", "2.75px", "-4.5px", "13.125px", "40px", "7%",
                                          "auto", "631.269px", "0.016px"};
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    flow_tree_t *old = graft_layout(&p, NULL, NULL, NULL);
    uint32_t seed = 0x4e34u;
    unsigned taken = 0, declined = 0, wrong = 0, renewed = 0;
    for (int step = 0; step < 240 && old != NULL; step++) {
        seed = seed * 1103515245u + 12345u;
        int box = (int)((seed >> 16) % 5);
        seed = seed * 1103515245u + 12345u;
        const char *v0 = kValues[(seed >> 16) % (sizeof(kValues) / sizeof(kValues[0]))];
        seed = seed * 1103515245u + 12345u;
        const char *v1 = (seed >> 16) % 3 == 0 ? kValues[(seed >> 20) % (sizeof(kValues) / sizeof(kValues[0]))] : "0px";
        char style[160], id[4];
        snprintf(style, sizeof(style), kBase[box], v0, v1);
        snprintf(id, sizeof(id), "w%d", box);
        os64_html_set_attr(p.doc, graft_node(&p, id), "style", style, strlen(style));
        // A cascade past its retained budget is renewed, as a host renews
        // it, and the walk goes on from a fresh layout under the new one.
        if (!garb_cascade_update(p.cascade)) {
            garb_sheet_in_t in[16];
            for (int32_t j = 0; j < p.n; j++)
                in[j] = (garb_sheet_in_t){.sheet = &p.sheets[j]};
            garb_cascade_t *fresh = garb_cascade(in, p.n, p.doc, garb_cascade_env(p.cascade));
            flow_free(old);
            garb_cascade_free(p.cascade);
            p.cascade = fresh;
            p.env.cascade = fresh;
            old = graft_layout(&p, NULL, NULL, NULL);
            renewed++;
            continue;
        }
        flow_restyle_t kept = restyle_keep(&p, old);
        if (kept.kind == FLOW_RESTYLE_DECLINED) {
            declined++;
            flow_tree_t *next = graft_layout(&p, old, NULL, NULL);
            flow_free(old);
            old = next;
            if (step % 5 == 0)
                graft_oracle(&p, old);
            continue;
        }
        taken += kept.moved > 0;
        flow_tree_t *whole = graft_layout(&p, NULL, NULL, NULL);
        const os64_html_node_t *at = NULL;
        const char *what = NULL;
        if (whole == NULL || !flow_restyle_check(old, whole, &at, &what)) {
            if (wrong++ == 0)
                printf("  move walk step %d (%s): %s\n", step, style, what != NULL ? what : "no whole layout");
        }
        if (step % 5 == 0)
            graft_oracle(&p, old);
        flow_free(whole);
    }
    printf("libflow move walk: %u kept moves, %u declined, %u cascades renewed\n", taken, declined, renewed);
    expect("restyle: the moves' walk keeps moves and declines others, and every kept tree is a whole layout",
           wrong == 0 && taken >= 40 && declined >= 20, NULL);
    flow_free(old);
    sheeted_close(&p);
}

// WHAT A MOVE PAINTS (N4): the marks a kept move hands over cover where the
// box was and where it is now, with its text, and are the whole of it.
typedef struct {
    os64_gui_rect_t r[64];
    int n;
} MoveMarks;
static void move_mark(void *ctx, const flow_box_t *box, os64_gui_rect_t r)
{
    (void)box;
    MoveMarks *m = ctx;
    if (m->n < 64)
        m->r[m->n++] = r;
}
static bool move_covered(const MoveMarks *m, os64_gui_rect_t want)
{
    for (int i = 0; i < m->n; i++)
        if (m->r[i].x <= want.x && m->r[i].y <= want.y && m->r[i].x + m->r[i].w >= want.x + want.w &&
            m->r[i].y + m->r[i].h >= want.y + want.h)
            return true;
    return false;
}
static void restyle_move_marks(void)
{
    static char html[16384];
    size_t len = (size_t)snprintf(html, sizeof(html), "<!doctype html><style>p{color:red;padding:2px}</style>"
        "<div style='position:relative;width:400px;height:100px'><div id=m style='position:absolute;width:50px;left:0px;top:0px'>"
        "moved <div id=in style='opacity:.5'>text</div></div></div>");
    for (int i = 0; i < 400 && len < sizeof(html) - 64; i++)
        len += (size_t)snprintf(html + len, sizeof(html) - len, "<p>%d</p>", i);
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    flow_tree_t *old = graft_layout(&p, NULL, NULL, NULL);
    os64_html_node_t *m = graft_node(&p, "m");
    os64_gui_rect_t before = old != NULL && flow_box_for(old, m) != NULL ? flow_box_for(old, m)->overflow
                                                                          : (os64_gui_rect_t){0};
    // A box inside the moved one draws somewhere else too: its drawn stamp
    // moves, so a kept drawing of it (yonder's keep.h) is not reused.
    const flow_box_t *inner = old != NULL ? flow_box_for(old, graft_node(&p, "in")) : NULL;
    uint64_t inner_drawn = inner != NULL ? inner->drawn : 0;
    static const char moved[] = "position:absolute;width:50px;left:200.5px;top:30px";
    os64_html_set_attr(p.doc, m, "style", moved, sizeof(moved) - 1);
    graft_update(&p);
    flow_reuse_inputs_t inputs = {.resource_owner = p.doc};
    os64_page_t *model = os64_page_rebuild(p.page);
    MoveMarks marks = {0};
    flow_restyle_marks_t hand = {{0, 0}, move_mark, &marks};
    flow_restyle_t kept = old != NULL ? flow_restyle_in_place(old, p.doc, model, &p.env, &inputs, 800, &hand)
                                      : (flow_restyle_t){0};
    os64_page_free(model);
    os64_gui_rect_t after = old != NULL && flow_box_for(old, m) != NULL ? flow_box_for(old, m)->overflow
                                                                         : (os64_gui_rect_t){0};
    expect("restyle: a kept move marks where the box was and where it is, and that is all of it",
           kept.kind == FLOW_RESTYLE_PAINT && kept.moved == 1 && kept.parts == FLOW_PARTS_MARKED &&
               before.w > 0 && after.x != before.x && move_covered(&marks, before) && move_covered(&marks, after),
           NULL);
    expect("restyle: and every box inside the moved one is stamped as drawn anew",
           inner != NULL && inner->drawn != inner_drawn, NULL);
    flow_free(old);
    sheeted_close(&p);
}

// A MOVE ANOTHER ELEMENT'S CHANGE MADE (N4): a class on an ancestor moves a
// positioned box through a selector, so the change log names the ancestor
// and not the box. The box is kept moved; a later change elsewhere is laid
// out grafting from the kept tree, and the box must not be copied back to
// where its records put it (move_box marks it changed in the snapshot).
static void restyle_move_by_a_class(void)
{
    static char html[16384];
    size_t len = (size_t)snprintf(html, sizeof(html), "<!doctype html><style>p{color:red;padding:2px}"
        ".menu{position:absolute;width:200px;left:-900px;top:0px}.nav.open .menu{left:40.5px}</style>"
        "<div class=nav id=nav><div style='position:relative;width:300px'>"
        "<div class=menu id=menu>menu <b>items</b></div>"
        "<span>r</span></div></div><p id=c>tail</p>");
    for (int i = 0; i < 400 && len < sizeof(html) - 64; i++)
        len += (size_t)snprintf(html + len, sizeof(html) - len, "<p>%d</p>", i);
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    flow_tree_t *old = graft_layout(&p, NULL, NULL, NULL);
    os64_html_set_attr(p.doc, graft_node(&p, "nav"), "class", "nav open", 8);
    graft_update(&p);
    flow_restyle_t kept = old != NULL ? restyle_keep(&p, old) : (flow_restyle_t){0};
    char said[96];
    snprintf(said, sizeof(said), "kind %d why %d moved %llu written %llu %s", (int)kept.kind, (int)kept.why,
             (unsigned long long)kept.moved, (unsigned long long)kept.written, kept.detail);
    expect("restyle: a class on an ancestor that moves a box is kept as a move", kept.kind == FLOW_RESTYLE_PAINT &&
           kept.moved == 1, said);
    if (old != NULL)
        graft_oracle(&p, old);
    os64_html_set_attr(p.doc, graft_node(&p, "c"), "style", "padding-top:3px", 15);
    graft_update(&p);
    flow_tree_t *next = graft_layout(&p, old, NULL, NULL);
    flow_free(old);
    graft_oracle(&p, next);
    flow_free(next);
    sheeted_close(&p);
}

static void restyle_in_place_cases(void)
{
    restyle_offset_cases();
    restyle_move_by_a_class();
    restyle_move_marks();
    restyle_move_sequences();
    restyle_move_walk();
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

// THE INTRINSIC WIDTHS COPIED WHATEVER THE ROLE (RELAYOUT_GRAFT.md §
// Intrinsic widths have a separate memo; layout.c intrinsic_copy). Each
// layout is compared with a fresh one (graft_oracle), so a copy of widths a
// fresh measure would not find shows as a box placed elsewhere.
static int64_t intr_ticks;
static int64_t intr_clock(void *ctx) { (void)ctx; return intr_ticks += 3; }
static flow_tree_t *intr_layout(Sheeted *p, const flow_tree_t *old, bool timed)
{
    flow_reuse_inputs_t inputs = {.resource_owner = p->doc};
    os64_page_t *model = os64_page_rebuild(p->page);
    flow_profile_t profile;
    flow_tree_t *t = flow_layout_incremental(p->doc, model, 800, &p->env, old, &inputs, &profile,
                                            timed ? intr_clock : NULL, NULL);
    os64_page_free(model);
    return t;
}
// A cascade's update may refuse (a change too broad to mark): the caller
// then builds it anew, as a page does.
static void intr_update(Sheeted *p)
{
    if (garb_cascade_update(p->cascade)) return;
    garb_sheet_in_t sheets[16];
    for (int32_t i = 0; i < p->n; ++i) sheets[i] = (garb_sheet_in_t){.sheet = &p->sheets[i]};
    garb_cascade_t *fresh = garb_cascade(sheets, p->n, p->doc, garb_cascade_env(p->cascade));
    garb_cascade_free(p->cascade);
    p->cascade = fresh;
    p->env.cascade = fresh;
}
static void intr_style(Sheeted *p, const char *id, const char *style)
{
    os64_html_set_attr(p->doc, graft_node(p, id), "style", style, strlen(style));
    intr_update(p);
}
typedef struct {
    char html[24000];
    size_t n;
    int ids;
    char base[64][96];      // each element's own style, a mutation's prefix
    bool text[64];          // a paragraph whose first child is its text
    uint32_t rng;
} IntrWalk;
static uint32_t intr_rand(IntrWalk *w) { w->rng = w->rng * 1103515245u + 12345u; return w->rng >> 8; }
static void intr_put(IntrWalk *w, const char *s)
{
    size_t k = strlen(s);
    if (w->n + k < sizeof(w->html)) { memcpy(w->html + w->n, s, k); w->n += k; w->html[w->n] = 0; }
}
static const char *const kIntrWords[] = {"alpha", "beta", "gamma delta", "epsilon", "a", "longerword", "zeta eta theta"};
static void intr_words(IntrWalk *w, char *out, size_t cap)
{
    out[0] = 0;
    int n = 1 + (int)(intr_rand(w) % 5);
    for (int i = 0; i < n; i++) {
        if (i) strncat(out, " ", cap - strlen(out) - 1);
        strncat(out, kIntrWords[intr_rand(w) % 7], cap - strlen(out) - 1);
    }
}
static int intr_open(IntrWalk *w, const char *tag, const char *style)
{
    int id = w->ids++;
    char buf[200];
    snprintf(w->base[id], sizeof(w->base[id]), "%s", style);
    snprintf(buf, sizeof(buf), "<%s id=e%d style='%s'>", tag, id, style);
    intr_put(w, buf);
    return id;
}
static void intr_node(IntrWalk *w, int depth)
{
    char words[96], buf[160];
    unsigned kind = depth >= 3 || w->ids >= 56 ? 0 : intr_rand(w) % 8;
    switch (kind) {
    case 0: case 1: {
        int id = intr_open(w, "p", "margin:0");
        intr_words(w, words, sizeof(words));
        intr_put(w, words);
        if (intr_rand(w) % 3 == 0) intr_put(w, " <img src=known.png style='height:50%'>");
        intr_put(w, "</p>");
        w->text[id] = true;
        return;
    }
    case 2: case 3: {
        static const char *const kFlex[] = {"display:flex", "display:flex;flex-wrap:wrap",
                                            "display:flex;flex-direction:column", "display:inline-flex"};
        intr_open(w, "div", kFlex[intr_rand(w) % 4]);
        if (intr_rand(w) % 3 == 0) { intr_words(w, words, sizeof(words)); intr_put(w, words); }
        break;
    }
    case 4:
        intr_open(w, "div", intr_rand(w) % 2 ? "display:grid;grid-template-columns:auto auto"
                                              : "display:grid;grid-template-columns:repeat(2,minmax(0,auto))");
        break;
    case 5:
        intr_open(w, "span", "display:inline-block");
        break;
    case 6: {
        static const char *const kPlaced[] = {"float:left", "position:relative",
                                              "position:absolute;top:0;left:0", "height:100%"};
        intr_open(w, "div", kPlaced[intr_rand(w) % 4]);
        break;
    }
    case 7:
        if (intr_rand(w) % 2) {
            // A picture: blockified in a flex or grid container, where its
            // percentage limits are of its containing block's height.
            int id = w->ids++;
            snprintf(w->base[id], sizeof(w->base[id]), "min-height:%d%%", 20 + (int)(intr_rand(w) % 60));
            snprintf(buf, sizeof(buf), "<img id=e%d src=known.png style='%s'>", id, w->base[id]);
            intr_put(w, buf);
            return;
        }
        intr_open(w, "div", "");
        break;
    default:
        intr_open(w, "div", "");
        break;
    }
    int n = 1 + (int)(intr_rand(w) % 3);
    for (int i = 0; i < n; i++) intr_node(w, depth + 1);
    snprintf(buf, sizeof(buf), "</%s>", kind == 5 ? "span" : "div");
    intr_put(w, buf);
}
static const char *const kIntrExtras[] = {
    "", "min-width:40px", "max-width:90px", "width:130px", "aspect-ratio:2", "aspect-ratio:1;height:30px",
    "height:50%", "height:70px", "min-height:20px", "flex:1", "padding:3px", "max-width:30%", "min-width:0",
};
static void intr_walk(uint32_t seed, int steps, uint64_t *roots, uint64_t *refused)
{
    IntrWalk *w = calloc(1, sizeof(*w));
    w->rng = seed;
    intr_put(w, "<!doctype html><style>p{margin:0}</style><div id=e0 style='display:flex;height:300px;width:760px'>");
    int top = w->ids++;
    snprintf(w->base[top], sizeof(w->base[top]), "display:flex;width:760px");
    for (int i = 0; i < 3; i++) intr_node(w, 0);
    intr_put(w, "</div><p id=tail>tail</p>");
    Sheeted p;
    sheeted_open(&p, w->html, 800, 600);
    flow_tree_t *old = intr_layout(&p, NULL, false);
    for (int step = 0; step < steps; step++) {
        int changes = 1 + (int)(intr_rand(w) % 2);
        for (int c = 0; c < changes; c++) {
            unsigned what = intr_rand(w) % 4;
            char id[16], style[200], words[96];
            if (what == 0) {
                snprintf(style, sizeof(style), "%s;height:%dpx", w->base[top], 40 + (int)(intr_rand(w) % 400));
                os64_html_set_attr(p.doc, graft_node(&p, "e0"), "style", style, strlen(style));
                continue;
            }
            int e = 1 + (int)(intr_rand(w) % (unsigned)(w->ids > 1 ? w->ids - 1 : 1));
            snprintf(id, sizeof(id), "e%d", e);
            if (what == 1 && w->text[e]) {
                intr_words(w, words, sizeof(words));
                os64_html_set_text(p.doc, graft_node(&p, id)->first_child, words, strlen(words));
                continue;
            }
            snprintf(style, sizeof(style), "%s;%s", w->base[e], kIntrExtras[intr_rand(w) % 13]);
            os64_html_set_attr(p.doc, graft_node(&p, id), "style", style, strlen(style));
        }
        intr_update(&p);
        flow_tree_t *next = intr_layout(&p, old, false);
        flow_reuse_stats_t s = flow_reuse_stats(next);
        *roots += s.intrinsic_roots;
        for (int r = 0; r < FLOW_INTRINSIC_NREFUSALS; r++) refused[r] += s.intrinsic_refused[r];
        flow_free(old);
        graft_oracle(&p, next);
        old = next;
    }
    flow_free(old);
    sheeted_close(&p);
    free(w);
}
static void graft_intrinsic_walk(int seeds, int steps)
{
    uint64_t roots = 0, refused[FLOW_INTRINSIC_NREFUSALS] = {0};
    for (int s = 1; s <= seeds; s++) intr_walk((uint32_t)s * 2654435761u, steps, &roots, refused);
    printf("intrinsic walk: %d seeds, %d steps each: %llu copies; refused: %s %llu, %s %llu, %s %llu, %s %llu\n",
           seeds, steps, (unsigned long long)roots,
           flow_intrinsic_refusal_name(FLOW_INTRINSIC_HEIGHT), (unsigned long long)refused[FLOW_INTRINSIC_HEIGHT],
           flow_intrinsic_refusal_name(FLOW_INTRINSIC_ORDER), (unsigned long long)refused[FLOW_INTRINSIC_ORDER],
           flow_intrinsic_refusal_name(FLOW_INTRINSIC_KNOWN), (unsigned long long)refused[FLOW_INTRINSIC_KNOWN],
           flow_intrinsic_refusal_name(FLOW_INTRINSIC_TABLE), (unsigned long long)refused[FLOW_INTRINSIC_TABLE]);
    expect("intrinsic walk: role subtrees' widths are copied", roots > 0, NULL);
}
static void graft_intrinsic_cases(void)
{
    char got[96];
    // A flex container left alone beside a change: its widths and its
    // items' are copied, counted by the root's role when profiled.
    Sheeted p;
    sheeted_open(&p, "<!doctype html><div style='display:flex'><div id=f style='display:flex'>"
                     "<div><p>item one words</p></div><div><p>item two</p></div></div>"
                     "<div id=grow style='height:10px'></div></div><p>tail</p>", 800, 600);
    flow_tree_t *old = intr_layout(&p, NULL, false);
    intr_style(&p, "grow", "height:40px");
    flow_tree_t *next = intr_layout(&p, old, true);
    flow_reuse_stats_t s = flow_reuse_stats(next);
    expect("intrinsic: a flex item's widths are copied with what its measure measured",
           s.intrinsic_roots >= 1 && s.intrinsic_hits >= 5 && s.intrinsic_role_roots[FLOW_ROLE_ITEM] >= 1, NULL);
    flow_free(old); graft_oracle(&p, next); old = next;
    // Its text changed: measured again, and the copy is of what did not.
    os64_html_set_text(p.doc, graft_node(&p, "f")->first_child->first_child->first_child, "a changed item", 14);
    intr_update(&p);
    next = intr_layout(&p, old, false);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    // A flex container that is no item, measured inside a changed
    // absolute box: counted by its own role, though it refuses nothing.
    sheeted_open(&p, "<!doctype html><div style='position:relative'><div id=ab style='position:absolute'>"
                     "<div id=f style='display:flex'><div>one word</div><div>two words here</div></div></div></div>"
                     "<p>tail</p>", 800, 600);
    old = intr_layout(&p, NULL, false);
    intr_style(&p, "ab", "position:absolute;padding:3px");
    next = intr_layout(&p, old, true);
    s = flow_reuse_stats(next);
    snprintf(got, sizeof(got), "roots %llu, flex %llu, none %llu", (unsigned long long)s.intrinsic_roots,
             (unsigned long long)s.intrinsic_role_roots[FLOW_ROLE_FLEX],
             (unsigned long long)s.intrinsic_role_roots[FLOW_ROLES]);
    expect("intrinsic: a flex container's copy is counted as a flex container's, not as none",
           s.intrinsic_role_roots[FLOW_ROLE_FLEX] >= 1, got);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    // A block-level picture's measure reads its containing block's height:
    // its percentage limit carried through its ratio, under a container
    // that seeds it before measuring it.
    sheeted_open(&p, "<!doctype html><div id=f style='display:flex;align-items:flex-start;height:400px'>"
                     "<img src=known.png style='min-height:50%'></div>", 800, 600);
    old = intr_layout(&p, NULL, false);
    intr_style(&p, "f", "display:flex;align-items:flex-start;height:600px");
    next = intr_layout(&p, old, false);
    s = flow_reuse_stats(next);
    expect("intrinsic: a picture under its containing block's new height is measured again",
           s.intrinsic_refused[FLOW_INTRINSIC_HEIGHT] >= 1, NULL);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    // A ratio container's height, made from its width, read by its
    // anonymous item's line: the container untouched, its parent narrower.
    sheeted_open(&p, "<!doctype html><div id=p style='width:600px'><div style='display:flex;aspect-ratio:2'>"
                     "<span><img src=known.png style='height:50%'></span> loose</div></div><p>tail</p>", 800, 600);
    old = intr_layout(&p, NULL, false);
    intr_style(&p, "p", "width:400px");
    next = intr_layout(&p, old, false);
    s = flow_reuse_stats(next);
    expect("intrinsic: an item under a ratio container's new height is measured again",
           s.intrinsic_refused[FLOW_INTRINSIC_HEIGHT] >= 1, NULL);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    // An anonymous item's measure reads its container's definite height,
    // which the container's percentage of its parent's moves while the
    // container itself is untouched.
    sheeted_open(&p, "<!doctype html><div id=p style='height:40px'><div style='display:flex;height:100%'>"
                     "<span><img src=known.png style='height:100%'></span> loose</div></div><p>tail</p>", 800, 600);
    old = intr_layout(&p, NULL, false);
    intr_style(&p, "p", "height:90px");
    next = intr_layout(&p, old, false);
    s = flow_reuse_stats(next);
    snprintf(got, sizeof(got), "roots %llu height %llu", (unsigned long long)s.intrinsic_roots,
             (unsigned long long)s.intrinsic_refused[FLOW_INTRINSIC_HEIGHT]);
    expect("intrinsic: an anonymous item under a container's new definite height is measured again",
           s.intrinsic_refused[FLOW_INTRINSIC_HEIGHT] >= 1, got);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    // An inline-block's content measured with its line's box is copied
    // with it: a measure avoided each.
    sheeted_open(&p, "<!doctype html><div style='display:flex'><div><p>a <span style='display:inline-block'>"
                     "<b style='display:block'>in</b></span> b</p></div><div id=grow style='height:10px'></div></div>",
                 800, 600);
    old = intr_layout(&p, NULL, false);
    intr_style(&p, "grow", "height:30px");
    next = intr_layout(&p, old, false);
    s = flow_reuse_stats(next);
    snprintf(got, sizeof(got), "roots %llu hits %llu", (unsigned long long)s.intrinsic_roots,
             (unsigned long long)s.intrinsic_hits);
    expect("intrinsic: an atom's content is copied with the box whose measure measured it",
           s.intrinsic_roots == 1 && s.intrinsic_hits == 5, got);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    // A box measured after a copy, outside the root's measure, takes a
    // number past the copy's: the next layout's copy of the root does not
    // take it with it.
    // (By class: a style attribute reparsed costs the cascade's retained
    // budget, which three layouts' updates may spend.)
    sheeted_open(&p, "<!doctype html><style>.a{height:20px}.b{height:30px}</style><div style='display:flex'>"
                     "<div style='position:relative'><p>words here</p><div style='position:absolute'>later words</div>"
                     "</div><div id=grow style='height:10px'></div></div><p>a</p><p>b</p><p>c</p><p>d</p><p>e</p>"
                     "<p>f</p><p>g</p><p>h</p><p>i</p><p>j</p>", 800, 600);
    old = intr_layout(&p, NULL, false);
    os64_html_set_attr(p.doc, graft_node(&p, "grow"), "class", "a", 1);
    intr_update(&p);
    next = intr_layout(&p, old, false);
    uint64_t hits = flow_reuse_stats(next).intrinsic_hits, roots = flow_reuse_stats(next).intrinsic_roots;
    flow_free(old); graft_oracle(&p, next); old = next;
    os64_html_set_attr(p.doc, graft_node(&p, "grow"), "class", "b", 1);
    intr_update(&p);
    next = intr_layout(&p, old, false);
    s = flow_reuse_stats(next);
    snprintf(got, sizeof(got), "hits %llu then %llu, copies %llu then %llu", (unsigned long long)hits,
             (unsigned long long)s.intrinsic_hits, (unsigned long long)roots, (unsigned long long)s.intrinsic_roots);
    expect("intrinsic: a copy of a copy takes what the first took, the box measured after it a copy of its own",
           hits > 0 && roots == 2 && s.intrinsic_hits == hits && s.intrinsic_roots == roots, got);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    // A table in a measured subtree is measured fresh.
    sheeted_open(&p, "<!doctype html><div style='display:flex'><div><table><tr><td>cell words</td></tr></table></div>"
                     "<div id=grow style='height:10px'></div></div>", 800, 600);
    old = intr_layout(&p, NULL, false);
    intr_style(&p, "grow", "height:30px");
    next = intr_layout(&p, old, false);
    s = flow_reuse_stats(next);
    expect("intrinsic: a table is measured fresh", s.intrinsic_refused[FLOW_INTRINSIC_TABLE] >= 1, NULL);
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);

    graft_intrinsic_walk(12, 24);
}

// ROLES INSIDE A COPY (G1; RELAYOUT_GRAFT.md § Roles inside a copy). Each
// case changes a box beside the subtree, so the subtree is copied, and the
// layout is compared with a fresh one (graft_oracle); a staged field's case
// has a reader outside the copy that reads it.
static flow_profile_t roles_profile;
static flow_tree_t *roles_layout(Sheeted *p, const flow_tree_t *old)
{
    flow_reuse_inputs_t inputs = {.resource_owner = p->doc};
    os64_page_t *model = os64_page_rebuild(p->page);
    flow_tree_t *t = flow_layout_incremental(p->doc, model, 800, &p->env, old, &inputs, &roles_profile, NULL, NULL);
    os64_page_free(model);
    return t;
}
// Lays the page out, changes #grow's class to `g`, lays it out again
// against the first and compares; returns the second's stats.
static flow_reuse_stats_t roles_case(const char *html, uint64_t *reused)
{
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    flow_tree_t *old = roles_layout(&p, NULL);
    os64_html_set_attr(p.doc, graft_node(&p, "grow"), "class", "g", 1);
    intr_update(&p);
    flow_tree_t *next = roles_layout(&p, old);
    flow_reuse_stats_t s = flow_reuse_stats(next);
    *reused = roles_profile.reused;
    flow_free(old); graft_oracle(&p, next); flow_free(next); sheeted_close(&p);
    return s;
}
// #grow starts with a height, so the cursor has placed something before
// what follows it either way (whether it had is a call input).
#define ROLES_HEAD "<!doctype html><style>#grow{height:10px}#grow.g{height:30px} p{margin:0}</style>"
static void graft_roles_cases(void)
{
    uint64_t reused;
    flow_reuse_stats_t s;
    // A wrapped flex container copied inside a baseline-aligned item: the
    // item's baseline is its container's first line's, which the copied
    // items' flex lines say (the first in the tree, baseline-aligned, is on
    // its third line, and would be taken from any line).
    s = roles_case(ROLES_HEAD "<div style='display:flex;align-items:baseline'>"
        "<div><div><div style='display:flex;flex-wrap:wrap;width:100px'>"
        "<div style='order:2;align-self:baseline;width:80px;font-size:30px;padding-top:20px'>late</div>"
        "<div style='order:1;width:80px'>first</div><div style='order:1;width:80px'>second</div>"
        "</div></div></div><div style='font-size:20px'>beside</div><div id=grow></div></div>", &reused);
    expect("roles: a wrapped flex container is copied with its items' flex lines", reused >= 5, NULL);
    // A grid copied the same way: its baseline is its first row's first
    // column's item, which the copied items' rows and columns say (the
    // tree's first item of row 1 is in its second column, and tree order
    // would take it).
    s = roles_case(ROLES_HEAD "<div style='display:flex;align-items:baseline'>"
        "<div><div><div style='display:grid;grid-template-columns:80px 80px'>"
        "<div style='grid-row:2;font-size:30px;padding-top:20px'>late</div>"
        "<div style='grid-row:1;grid-column:2'>first</div>"
        "<div style='grid-row:1;grid-column:1;font-size:24px;padding-top:12px'>second</div>"
        "</div></div></div><div style='font-size:20px'>beside</div><div id=grow></div></div>", &reused);
    expect("roles: a grid is copied with its items' rows and columns", reused >= 5, NULL);
    // An inline-block's content, and an absolute box whose containing
    // block is inside the copy, its placeholder on a copied line.
    s = roles_case(ROLES_HEAD "<div id=grow></div><div style='position:relative'>"
        "<p>words <span style='display:inline-block;padding:3px'><b style='display:block'>in</b></span>"
        " and <span style='position:absolute;top:4px'>abs</span> after</p>"
        "<div style='position:absolute;right:0'>block abs</div></div><p>tail</p>", &reused);
    expect("roles: an atom's content and absolute boxes inside are copied", reused >= 6, NULL);
    // Transformed and filtered boxes inside, and a relative one beside its
    // changed sibling.
    s = roles_case(ROLES_HEAD "<div id=grow></div><div><div style='transform:translateX(5px)'><p>moved</p></div>"
        "<div style='filter:blur(1px)'><p>blurred</p></div><div style='position:relative;top:3px'><p>rel</p></div>"
        "</div><p>tail</p>", &reused);
    expect("roles: transformed, filtered and relative boxes are copied", reused >= 7, NULL);

    // No refusal by name for a box that is copied: an absolute box whose
    // containing block is its parent is no root itself (refused for its
    // role), and its parent is copied with it.
    s = roles_case(ROLES_HEAD "<div id=grow></div><div style='position:relative'>"
        "<div style='position:absolute;right:0'>a</div><p>x</p></div><p>tail</p>", &reused);
    expect("roles: an absolute box copied with its containing block is no refusal by name",
           s.refused[FLOW_REUSE_ABSOLUTE_OUTSIDE] == 0 && s.refused[FLOW_REUSE_ATOM_FLOATS] == 0 && reused >= 3,
           NULL);
    // Refused by name: an absolute box whose containing block is outside
    // the div holding it, which grows (#grow), so the box moves: a copy of
    // the div would leave it where it was. The box's own pair is refused
    // for its role; the name is the div's.
    s = roles_case(ROLES_HEAD "<div style='position:relative'><div><p>x</p>"
        "<div style='position:absolute;bottom:0'>abs</div></div><div id=grow></div></div><p>tail</p>", &reused);
    expect("roles: an absolute box escaping the subtree refuses it by name",
           s.refused[FLOW_REUSE_ABSOLUTE_OUTSIDE] >= 1, NULL);
    // Refused by name: an atom whose content made a float query in its
    // line's space (its own `clear`, asked as it is entered: graft_enter).
    s = roles_case(ROLES_HEAD "<div id=grow></div><div style='float:left;width:20px;height:5px'></div>"
        "<div><p>a <span style='display:inline-block;clear:both'>c</span> b</p></div><p>tail</p>", &reused);
    expect("roles: an atom whose content asked the floats refuses by name",
           s.refused[FLOW_REUSE_ATOM_FLOATS] >= 1, NULL);
    // Still refused: a float inside, a block-level picture inside.
    s = roles_case(ROLES_HEAD "<div id=grow></div><div><div style='float:left;width:30px'>f</div><p>beside</p></div>"
        "<p>tail</p>", &reused);
    expect("roles: a float inside refuses", s.refused[FLOW_REUSE_BOX_ROLE] >= 1, NULL);
    s = roles_case(ROLES_HEAD "<div id=grow></div><div><img src=known.png style='display:block'><p>x</p></div>"
        "<p>tail</p>", &reused);
    expect("roles: a block-level picture inside refuses", s.refused[FLOW_REUSE_BOX_ROLE] >= 1, NULL);
}
