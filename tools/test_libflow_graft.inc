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
        "<!doctype html><div id=grow style='height:20px'></div><p>before <img src=missing.png alt=' repeated  alt '> after</p><p>end</p>"
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
