static void layout_profile_stream_cases(void)
{
    int64_t saved_step=clock_step;clock_step=17;
    const char *page="<!doctype html><style>p{color:red}</style><p>first</p>"
        "<script>var first=document.body.offsetHeight;</script><p>second</p>"
        "<script>var second=document.body.offsetHeight;</script><p id=out>done</p>";
    for(int enabled=0;enabled<2;++enabled) {
        loop_page(page);
        if(!g.stream.diag)g.stream.diag=diag_new_claimed("http://fixture.test/profile-stream",1);
        yonder_diag_t *record=g.stream.diag;
        s_layout_profile=enabled;
        check(loop_settle() && !g.stream.active && g.page.diag==record && probe_text_is("out","done"),
              "layout profile: stream measurements survive record ownership transfer");
        const char *stream=yonder_diag_fact_value(record,"layout stream");
        const char *shown=yonder_diag_fact_value(record,"layout");
        if(enabled) {
            check(stream && strstr(stream,"cascade 34 us") && strstr(stream,"style 34 us") &&
                  strstr(stream,"2 layout attempts") && strstr(stream,"0 refused/partial") &&
                  strstr(stream,"0 clock errors"),"layout profile: parser geometry reads have their own totals");
            check(shown && strstr(shown,"1 layout attempts"),
                  "layout profile: stream measurements do not inflate the shown-layout total");
        } else check(!stream && !shown,"layout profile: disabled stream instrumentation writes no totals");
        loop_drop();
    }
    loop_page("<!doctype html><p>No geometry reads while parsing</p>");
    if(!g.stream.diag)g.stream.diag=diag_new_claimed("http://fixture.test/profile-quiet",1);
    check(loop_settle() && !yonder_diag_fact_value(g.page.diag,"layout stream"),
          "layout profile: parser chunks alone are not recorded as layouts");
    loop_drop();s_layout_profile=false;clock_step=saved_step;
}
typedef struct { int64_t now, step; unsigned calls; } ProfileClock;
static int64_t profile_test_clock(void *opaque)
{
    ProfileClock *clock=opaque; ++clock->calls; clock->now+=clock->step; return clock->now;
}
/* Every box's border box and every element's colour, transform count and
 * opacity, in tree order: what a kept tree must answer as a whole layout
 * would (restyle_kept_cases). Answers how many it wrote. */
static size_t tree_answers(const flow_tree_t *t, int64_t *out, size_t cap)
{
    size_t n = 0;
    for (const flow_box_t *b = flow_root(t); b != NULL && n + 7 <= cap;) {
        out[n++] = b->rect.x; out[n++] = b->rect.y; out[n++] = b->rect.w; out[n++] = b->rect.h;
        out[n++] = b->style ? (int64_t)b->style->color : -1;
        out[n++] = b->style ? b->style->ntransform : -1;
        out[n++] = b->style ? b->style->opacity : -1;
        if (b->first != NULL) { b = b->first; continue; }
        while (b != NULL && b->next == NULL) b = b->parent;
        b = b != NULL ? b->next : NULL;
    }
    return n;
}

/* THE RESTYLE KEPT IN THE SHOWN TREE (flow_restyle_in_place, N1): a change
 * only the painter reads is written into the tree on screen, which stays
 * the same tree, and answers as a whole layout would; anything else is
 * declined by reason and laid out as before. */
static void restyle_kept_cases(void)
{
    probe_page("<!doctype html><style>.on{color:red}.far{transform:translateX(5px)}.near{transform:translateX(9px)}"
               ".pad{padding-left:40px}.dim{opacity:.5}.gone{opacity:0}.blue{color:blue}"
               ".shadow{box-shadow:0 0 20px red}@keyframes pulse{to{opacity:.5}}.spin{animation:pulse 1s infinite}"
               ".big{transform:scale(1)}.small{transform:scale(.5)}"
               ".near1{transform:translateX(1px)}.away{transform:translateX(3000px)}</style>"
               "<p id=p class=far>text <b>bold</b></p><p id=q>more</p><div id=mix>loose <p>block</p> tail</div>"
               "<p><u id=u>under</u></p><canvas id=cv width=100 height=20></canvas>"
               "<div id=sc class=big style=width:20px;height:20px></div>"
               "<div id=far class=near1 style=position:absolute;left:0;top:0;width:8px;height:8px></div>"
               "<div id=scroller style=height:20px;overflow:auto><p>one</p><p>two</p><p>three</p></div>",false);
    g.page.diag=diag_new_claimed("https://fixture.test/restyle-kept",1);
    s_layout_profile=true;
    s_geometry_check=1;      /* every kept tree laid out whole beside it and compared */
    /* The first change after the page arrived meets inputs its first layout
     * left moving (the fonts' revision), and a title is an attribute the
     * restyle does not judge (structure); a change after it is the case
     * under test. */
    check(!os64_html_set_attr(g.page.way.doc,probe_id("q"),"title","t",1) && script_rebuild(),"kept: a first change");
    const flow_tree_t *shown=g.page.tree;
    const flow_style_t *before=flow_style_for(g.page.tree,probe_id("p"));
    uint32_t colour=before?before->color:0;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","far on",6) && script_rebuild(),
          "kept: a colour published");
    const char *kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(g.page.tree==shown && kept && strstr(kept,"paint only 1"),"kept: a colour is kept in the tree on screen");
    if(!kept || !strstr(kept,"paint only 1")) printf("  layout kept: %s\n",kept?kept:"(none)");
    const flow_style_t *ps=flow_style_for(g.page.tree,probe_id("p"));
    check(ps && ps==before && ps->color!=colour,"kept: and the tree's own style holds the new colour");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","near on",7) && script_rebuild(),
          "kept: a transform's value published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(g.page.tree==shown && kept && strstr(kept,"paint only 2"),"kept: a transform's value is kept");
    if(!kept || !strstr(kept,"paint only 2")) printf("  layout kept: %s\n",kept?kept:"(none)");
    /* What the kept tree answers is what a whole layout answers: what
     * tree_answers takes of every box, and what a script's
     * getBoundingClientRect sees through the transform (the frame the
     * restyle filled again). The geometry check compares the rest. */
    static int64_t kept_answers[4096], fresh_answers[4096];
    size_t nk=tree_answers(g.page.tree,kept_answers,4096);
    os64_dom_geometry_t kept_rect,fresh_rect;
    memset(&kept_rect,0,sizeof(kept_rect)); memset(&fresh_rect,0,sizeof(fresh_rect));
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("p"),800,600,g.zoom,(flow_point_t){0,0},&kept_rect),
          "kept: the transformed element measured in the kept tree");
    s_restyle_in_place=false;
    check(relayout(true),"kept: a whole layout beside it");
    size_t nf=tree_answers(g.page.tree,fresh_answers,4096);
    check(yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id("p"),800,600,g.zoom,(flow_point_t){0,0},&fresh_rect),
          "kept: and in the whole layout");
    s_restyle_in_place=true;
    check(nk==nf && nk>0 && !memcmp(kept_answers,fresh_answers,nk*sizeof(int64_t)),
          "kept: every box's border box, colour, transforms and opacity as a whole layout has them");
    check(kept_rect.x==fresh_rect.x && kept_rect.y==fresh_rect.y && kept_rect.width==fresh_rect.width &&
          kept_rect.height==fresh_rect.height,
          "kept: a script measures the transformed element as in a whole layout");
    if(kept_rect.x!=fresh_rect.x) printf("  kept x %g, whole %g\n",kept_rect.x,fresh_rect.x);
    shown=g.page.tree;
    /* A colour over anonymous boxes (the loose text round #mix's block):
     * they take their parent's inherited values again, which the geometry
     * check compares; the element's boxes are stamped drawn again for the
     * paint cache. A shadow grows the box's ink. */
    const flow_box_t *mix=flow_box_for(g.page.tree,probe_id("mix"));
    uint64_t drawn=mix?mix->drawn:0;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("mix"),"class","blue",4) && script_rebuild(),
          "kept: a colour over anonymous boxes published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(g.page.tree==shown && kept && strstr(kept,"paint only 3"),"kept: a colour over anonymous boxes is kept");
    if(!kept || !strstr(kept,"paint only 3")) printf("  layout kept: %s\n",kept?kept:"(none)");
    check(mix && flow_box_for(g.page.tree,probe_id("mix"))==mix && mix->drawn>drawn,
          "kept: and its box is stamped drawn again");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("q"),"class","shadow",6) && script_rebuild(),
          "kept: a shadow published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(g.page.tree==shown && kept && strstr(kept,"paint only 4"),"kept: a shadow is kept, its ink grown");
    if(!kept || !strstr(kept,"paint only 4")) printf("  layout kept: %s\n",kept?kept:"(none)");
    /* A colour change on an element an animation runs on is the
     * animation's to lay out. */
    check(!os64_html_set_attr(g.page.way.doc,probe_id("mix"),"class","spin",4) && script_rebuild() &&
          !os64_html_set_attr(g.page.way.doc,probe_id("mix"),"class","spin blue",9) && script_rebuild(),
          "kept: a colour on an animated element published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"motion "),"kept: a change on an animated element is declined as motion");
    if(!kept || !strstr(kept,"motion ")) printf("  layout kept: %s\n",kept?kept:"(none)");
    /* An underline's colour is copied into its fragments, which no restyle
     * in place refreshes yet: declined as decoration. */
    check(!os64_html_set_attr(g.page.way.doc,probe_id("u"),"class","on",2) && script_rebuild(),
          "kept: an underlined colour published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"decoration 1"),"kept: an underlined colour is declined as decoration");
    if(!kept || !strstr(kept,"decoration 1")) printf("  layout kept: %s\n",kept?kept:"(none)");
    shown=g.page.tree;
    /* Declined: padding moves boxes; an opacity across 1 makes or ends a
     * group (stacking), and across 0 paints the box or not (geometry: the
     * record's `transparent`); a text changes the boxes' text. */
    check(!os64_html_set_attr(g.page.way.doc,probe_id("q"),"class","pad",3) && script_rebuild(),
          "kept: a padding published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(g.page.tree!=shown && kept && strstr(kept,"geometry 1"),"kept: a padding is declined as geometry");
    if(!kept || !strstr(kept,"geometry 1")) printf("  layout kept: %s\n",kept?kept:"(none)");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("q"),"class","pad dim",7) && script_rebuild(),
          "kept: an opacity below 1 published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"stacking 1"),"kept: an opacity across 1 is declined as stacking");
    if(!kept || !strstr(kept,"stacking 1")) printf("  layout kept: %s\n",kept?kept:"(none)");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("q"),"class","pad gone",8) && script_rebuild(),
          "kept: an opacity to 0 published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"geometry 2"),"kept: an opacity across 0 is declined as geometry (unpainted)");
    if(!kept || !strstr(kept,"geometry 2")) printf("  layout kept: %s\n",kept?kept:"(none)");
    check(!os64_html_set_text(g.page.way.doc,probe_id("q")->first_child,"changed",7) && script_rebuild(),
          "kept: a text published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"structure 2"),"kept: a text is declined as structure");
    if(!kept || !strstr(kept,"structure 2")) printf("  layout kept: %s\n",kept?kept:"(none)");
    /* The record says what each decline found, and where. */
    const char *declined=yonder_diag_fact_value(g.page.diag,"layout kept, declined at");
    check(declined && strstr(declined,"structure 1: text (at p#q") && strstr(declined,"geometry 1: placement byte ") &&
          strstr(declined,"stacking 1: a group came or went"),
          "kept: the record names what each decline found, and where");
    if(!declined || !strstr(declined,"structure 1: text (at p#q")) printf("  declined at: %s\n",declined?declined:"(none)");
    /* An attribute box generation or placement read themselves (a canvas's
     * size) is structural, whatever the styles say. */
    check(!os64_html_set_attr(g.page.way.doc,probe_id("cv"),"width","400",3) && script_rebuild(),
          "kept: a canvas's width attribute published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"structure 3"),"kept: an attribute boxes read is declined as structure");
    if(!kept || !strstr(kept,"structure 3")) printf("  layout kept: %s\n",kept?kept:"(none)");
    /* A transform that comes to be drawn through its matrix becomes a group
     * of its own, with layers round it: declined as stacking. */
    check(!os64_html_set_attr(g.page.way.doc,probe_id("sc"),"class","small",5) && script_rebuild(),
          "kept: a scale(1) to scale(.5) published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"stacking 2"),"kept: a transform crossing into its matrix is declined as stacking");
    if(!kept || !strstr(kept,"stacking 2")) printf("  layout kept: %s\n",kept?kept:"(none)");
    /* A transform that moves a positioned box far out moves the page's
     * extent: kept, and the extent reached again (the check compares it). */
    /* Scrolled first: the check's whole layout is scrolled where the kept
     * tree is before the two are compared, as a layout's scrolls are set
     * again. */
    int32_t scroller=flow_scroller_for(g.page.tree,probe_id("scroller"));
    check(scroller>=0 && flow_scroll_set(g.page.tree,scroller,(flow_point_t){0,10}).y==10,
          "kept: a scroll container scrolled before the change");
    int32_t extent=flow_width(g.page.tree);
    check(!os64_html_set_attr(g.page.way.doc,probe_id("far"),"class","away",4) && script_rebuild(),
          "kept: a transform moving a box far out published");
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    check(kept && strstr(kept,"paint only 5") && flow_width(g.page.tree)>extent+2000,
          "kept: a transform far out is kept, and the page reaches it");
    if(!kept || !strstr(kept,"paint only 5")) printf("  layout kept: %s (extent %d -> %d)\n",kept?kept:"(none)",
                                                       extent,flow_width(g.page.tree));
    const char *geometry=yonder_diag_fact_value(g.page.diag,"geometry check");
    check(geometry && !strncmp(geometry,"5 compared, 0 differed",22),
          "kept: --geometry-check laid each kept tree out whole beside it, and none differed");
    if(!geometry || strncmp(geometry,"5 compared, 0 differed",22)) printf("  geometry check: %s\n",geometry?geometry:"(none)");
    s_geometry_check=s_geometry_checked=0;
    s_geometry_compared=s_geometry_differed=0;
    s_layout_profile=false;
    probe_drop();
    /* The playing page's case (tools/pages/layout-ticks.html): a script
     * writes a transform on each of a bar's twelve parts and reads its
     * clientWidth back, each read a forced layout the restyle kept in the
     * tree may answer (the cases below say which do and why the others
     * do not). */
    s_ledger=YONDER_LEDGER_ON;
    probe_page("<!doctype html><style>#bar{display:flex;width:600px}.part{flex:1;height:12px}</style>"
               "<div id=bar></div><p id=out>-</p><script>"
               "const bar=document.getElementById('bar');for(let i=0;i<12;i++){const d=document.createElement('div');"
               "d.className='part';bar.appendChild(d)}let t=0;"
               "window.onclick=()=>{t++;let sum=0;for(const p of bar.children){p.style.transform='scaleX('+(t/10)+')';"
               "sum+=p.clientWidth}document.getElementById('out').textContent=sum}</script>",true);
    script_turn();
    g.page.diag=diag_new_claimed("https://fixture.test/restyle-bar",1);
    s_layout_profile=true;
    s_geometry_check=1;
    os64_dom_event_t click={.type="click",.bubbles=true,.cancelable=true};
    os64_js_outcome_t out;
    /* The first click gives each part a transform where it had none, which
     * makes a containing block (geometry). The second moves transforms that
     * are there, and its inline styles may cost the cascade a fresh build
     * (libgarb's `attributes`), which the restyle declines: the tree's
     * records point into the cascade it was laid out in, which goes with
     * it (flow_restyle_in_place). */
    yonder_scripts_dispatch(g.page.scripts,NULL,&click,NULL,&out);
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    unsigned painted_before=0,painted=0,fresh_before=0,fresh=0,geometry_before=0,geometry_after=0;
    unsigned text_before=0,text_after=0;
    if(kept && strstr(kept,"paint only ")) sscanf(strstr(kept,"paint only ")+11,"%u",&painted_before);
    if(kept && strstr(kept,"inputs ")) sscanf(strstr(kept,"inputs ")+7,"%u",&fresh_before);
    if(kept && strstr(kept,"geometry ")) sscanf(strstr(kept,"geometry ")+9,"%u",&geometry_before);
    if(kept && strstr(kept,"structure ")) sscanf(strstr(kept,"structure ")+10,"%u",&text_before);
    yonder_scripts_dispatch(g.page.scripts,NULL,&click,NULL,&out);
    kept=yonder_diag_fact_value(g.page.diag,"layout kept");
    if(kept && strstr(kept,"paint only ")) sscanf(strstr(kept,"paint only ")+11,"%u",&painted);
    if(kept && strstr(kept,"inputs ")) sscanf(strstr(kept,"inputs ")+7,"%u",&fresh);
    if(kept && strstr(kept,"geometry ")) sscanf(strstr(kept,"geometry ")+9,"%u",&geometry_after);
    if(kept && strstr(kept,"structure ")) sscanf(strstr(kept,"structure ")+10,"%u",&text_after);
    /* Of the second click's twelve reads, the first meets the text the first
     * click wrote (#out: structure), and each of the other eleven is kept in
     * the tree or meets a fresh cascade (inputs, which the record names);
     * some are kept, and none is geometry. */
    const char *declined_at=yonder_diag_fact_value(g.page.diag,"layout kept, declined at");
    bool answered=painted>painted_before && painted-painted_before+fresh-fresh_before==11 &&
                  text_after-text_before==1 && geometry_after==geometry_before &&
                  (fresh==fresh_before || (declined_at && strstr(declined_at,"another cascade")));
    check(answered,"kept: twelve write-then-read pairs: eleven kept or meeting a fresh cascade, one the text written");
    if(!answered && declined_at) printf("  declined at: %s\n",declined_at);
    if(!answered)
        printf("  layout kept: %s (before the second click: %u kept)\n",kept?kept:"(none)",painted_before);
    geometry=yonder_diag_fact_value(g.page.diag,"geometry check");
    check(geometry && strstr(geometry," 0 differed"),"kept: and each answers as a whole layout");
    if(!geometry || !strstr(geometry," 0 differed")) printf("  geometry check: %s\n",geometry?geometry:"(none)");
    check(probe_text_is("out","600"),"kept: the reads answered, twelve parts of 50 px");
    s_geometry_check=s_geometry_checked=0;
    s_geometry_compared=s_geometry_differed=0;
    s_layout_profile=false;
    probe_drop();
    s_ledger=YONDER_LEDGER_OFF;
}

static void layout_profile_cases(void)
{
    restyle_kept_cases();
    /* The cases below measure what a layout needed, so every change is laid
     * out (the restyle kept in the tree would answer the paint-only ones
     * itself). */
    s_restyle_in_place=false;
    probe_page("<style>p{margin:0}</style><p id=p>first line</p><p>second line</p>",false);
    flow_env_t env=s_env;env.cascade=g.page.cascade;env.ctx=&g.page;
    flow_profile_t profile;
    ProfileClock clock={1000,13,0};
    flow_tree_t *tree=flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,&profile,profile_test_clock,&clock);
    check(tree && clock.calls==8 && profile.style_us==13 && profile.boxes_us==13 &&
          profile.layout_us==13 && profile.public_us==13 && !profile.clock_errors &&
          profile.elements>=2 && profile.boxes>=2 && profile.measures>=2 && !profile.reused,
          "layout profile: four independent phase clocks and actual work counts");
    char measured[16384],plain[16384];
    int64_t measured_len=flow_dump(tree,measured,sizeof(measured));flow_free(tree);
    clock.calls=0;
    tree=flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,NULL,profile_test_clock,&clock);
    int64_t plain_len=flow_dump(tree,plain,sizeof(plain));
    check(tree && clock.calls==0 && measured_len==plain_len && measured_len>0 &&
          measured_len<(int64_t)sizeof(measured) && !strcmp(measured,plain),
          "layout profile: disabled instrumentation reads no clock and preserves layout dumps");
    flow_free(tree);
    clock.step=-13;clock.calls=0;
    tree=flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,&profile,profile_test_clock,&clock);
    check(tree && profile.clock_errors==4 && !profile.style_us && !profile.boxes_us &&
          !profile.layout_us && !profile.public_us,"layout profile: backward clocks are explicit invalid samples");
    flow_free(tree);
    env.text=NULL;profile.style_us=999;
    check(!flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,&profile,profile_test_clock,&clock) && !profile.style_us,
          "layout profile: refused input clears the output sink");

    g.page.diag=diag_new_claimed("https://fixture.test/profile",1);
    int64_t saved_step=clock_step;clock_step=17;s_layout_profile=true;
    g.page.sheets_changed=true;
    check(page_lay_out(&g.page,800,600,g.zoom,&g.page),"layout profile: instrumented window layout");
    const char *line=yonder_diag_fact_value(g.page.diag,"layout");
    check(line && strstr(line,"cascade 17 us") && strstr(line,"style 17 us") &&
          strstr(line,"public 17 us") && strstr(line,"1 layout attempts") && strstr(line,"0 clock errors"),
          "layout profile: phase evidence reaches the owning page record");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","changed",7) && page_model_refresh(&g.page),
          "layout profile: model rebuild instrumented");
    line=yonder_diag_fact_value(g.page.diag,"layout");
    check(line && strstr(line,"model rebuild 17 us") && strstr(line,"1 model rebuilds"),
          "layout profile: model cost remains separate from layout");
    const char *cascade=yonder_diag_fact_value(g.page.diag,"layout cascade");
    check(cascade && !strcmp(cascade,"updated 0, built fresh 1: sheets changed 1; anchors held 0, bare 0; "
                                     "bare anchors asked again 0, turned 0, marked 0"),
          "layout profile: a cascade built fresh says why");
    g.page.sheets_changed=false;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"title","t",1) && script_rebuild(),
          "layout profile: a change published");
    cascade=yonder_diag_fact_value(g.page.diag,"layout cascade");
    check(cascade && !strcmp(cascade,"updated 1, built fresh 1: sheets changed 1; anchors held 0, bare 0; "
                                     "bare anchors asked again 0, turned 0, marked 0"),
          "layout profile: and the cascade it updated is counted");
    /* Each layout whole: what became of its cascade sorts the counts, and
     * the slowest says its phases, its cascade's fate and its attempts. */
    const char *slow=yonder_diag_fact_value(g.page.diag,"layout slowest");
    const char *by=yonder_diag_fact_value(g.page.diag,"layout by cascade");
    check(by && strstr(by,"cascade updated 1, ") && strstr(by,"; cascade built fresh 1, ") && strstr(by," ms each"),
          "layout profile: layouts are counted by what became of their cascade");
    check(slow && strstr(slow," ms at +") && strstr(slow,": cascade ") && strstr(slow,", the rest ") &&
          (strstr(slow,"(updated)") || strstr(slow,"(built fresh: sheets changed)")) && strstr(slow,"; 1 attempt"),
          "layout profile: the slowest layout says its phases, its cascade's fate and its attempts");
    s_layout_profile=false;clock_step=saved_step;
    probe_drop();
    /* A page whose sheets hold :has(), anchored and in a subject, keeps
     * its cascade across a change. */
    probe_page("<style>.g:has(.x) p{color:red}.a:has(.b){margin:1px}</style><div class=g><p id=p>t</p></div>"
               "<div class=a><i id=i></i></div>",false);
    g.page.diag=diag_new_claimed("https://fixture.test/profile-has",1);
    s_layout_profile=true;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("i"),"class","b",1) && script_rebuild(),
          "layout profile: a :has() page's change published");
    cascade=yonder_diag_fact_value(g.page.diag,"layout cascade");
    const char *kept_has="updated 1, built fresh 0; anchors held 1, bare 0; bare anchors asked again 0, turned 0, "
                         "marked 0";
    check(cascade && !strcmp(cascade,kept_has),
          "layout profile: a :has() page's cascade is updated, not built fresh, and says its anchor");
    if(!cascade || strcmp(cascade,kept_has)) printf("  layout cascade: %s\n",cascade?cascade:"(none)");
    /* --cascade-check: each update checked against a fresh cascade, which
     * agrees, so the record says nothing of it. */
    s_cascade_check=1;
    uint32_t checked=s_cascade_checked;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("i"),"class","",0) && script_rebuild(),
          "cascade check: a change published");
    check(s_cascade_checked==checked+1 && s_cascade_differed==0 &&
          yonder_diag_fact_value(g.page.diag,"cascade check")==NULL,
          "cascade check: the update was checked, and agreed with a fresh cascade");
    s_cascade_check=0;
    s_layout_profile=false;
    probe_drop();
    /* What each layout needed (yonder_needed_t): an attribute no rule reads
     * changes nothing; a colour is paint only; padding that moves the text
     * inside a block moves one node's boxes. */
    probe_page("<!doctype html><style>.on{color:red}.pad{padding-left:40px}.z{z-index:3}</style><p id=p>text</p><p id=q>more</p><div id=tip style=position:absolute;width:9px;height:9px></div>",false);
    g.page.diag=diag_new_claimed("https://fixture.test/profile-needed",1);
    s_layout_profile=true;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("q"),"title","t",1) && script_rebuild(),
          "layout needed: an attribute no rule reads published");
    const char *needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    check(needed && !strncmp(needed,"nothing moved 1 (",17) && strstr(needed,"paint only 0 ("),
          "layout needed: and it needed nothing");
    if(!needed || strncmp(needed,"nothing moved 1 (",17)) printf("  layout needed: %s\n",needed?needed:"(none)");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","on",2) && script_rebuild(),
          "layout needed: a colour published");
    needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    check(needed && strstr(needed,"paint only 1 ("),"layout needed: and it was paint only");
    if(!needed || !strstr(needed,"paint only 1 (")) printf("  layout needed: %s\n",needed?needed:"(none)");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","pad",3) && script_rebuild(),
          "layout needed: a padding published");
    needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    check(needed && strstr(needed,"one node's boxes moved 1 ("),"layout needed: and it moved one node's boxes");
    if(!needed || !strstr(needed,"one node's boxes moved 1 (")) printf("  layout needed: %s\n",needed?needed:"(none)");
    /* A style placement may read that moves nothing is a class of its own;
     * an empty positioned box taken away moves its own boxes, which went. */
    check(!os64_html_set_attr(g.page.way.doc,probe_id("q"),"class","z",1) && script_rebuild(),
          "layout needed: a z-index published");
    needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    check(needed && strstr(needed,"restyled, nothing moved 1 ("),"layout needed: and it was restyled, nothing moved");
    if(!needed || !strstr(needed,"restyled, nothing moved 1 (")) printf("  layout needed: %s\n",needed?needed:"(none)");
    check(os64_html_remove(g.page.way.doc,probe_id("tip"))==OS64_HTML_OK && script_rebuild(),
          "layout needed: a positioned box removed");
    needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    check(needed && strstr(needed,"one node's boxes moved 2 ("),"layout needed: and its boxes went");
    if(!needed || !strstr(needed,"one node's boxes moved 2 (")) printf("  layout needed: %s\n",needed?needed:"(none)");
    s_layout_profile=false;
    probe_drop();
    /* A pseudo-element's own change counts, though its node is new at every
     * layout: a colour is paint only; longer content moves its box and the
     * text after it. */
    probe_page("<!doctype html><style>#p::before{content:'X';color:red}#p.on::before{color:blue}"
        "#p.long::before{content:'XXXX'}</style><p id=p>text</p>",false);
    g.page.diag=diag_new_claimed("https://fixture.test/profile-pseudo",1);
    s_layout_profile=true;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","on",2) && script_rebuild(),
          "layout needed: a ::before's colour published");
    needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    check(needed && strstr(needed,"paint only 1 ("),"layout needed: and it was paint only");
    if(!needed || !strstr(needed,"paint only 1 (")) printf("  layout needed: %s\n",needed?needed:"(none)");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","long",4) && script_rebuild(),
          "layout needed: a ::before's longer content published");
    needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    check(needed && strstr(needed,"more moved 1 ("),"layout needed: and its box and the text after it moved");
    if(!needed || !strstr(needed,"more moved 1 (")) printf("  layout needed: %s\n",needed?needed:"(none)");
    s_layout_profile=false;
    probe_drop();
    /* A read made by the shown page's own script forces its layout there:
     * that is the forced column (its ledger says so), and the `d` reading
     * counts it. */
    s_ledger=YONDER_LEDGER_ON;
    probe_page("<!doctype html><style>.pad{padding-left:40px}</style><p id=p>text</p><script>"
        "window.onclick=function(){var p=document.getElementById('p');p.className='pad';p.offsetWidth}</script>",true);
    script_turn();
    g.page.diag=diag_new_claimed("https://fixture.test/profile-forced",1);
    s_layout_profile=true;
    cost_reset();
    os64_dom_event_t click={.type="click",.bubbles=true,.cancelable=true};
    os64_js_outcome_t out;
    yonder_scripts_dispatch(g.page.scripts,NULL,&click,NULL,&out);
    needed=yonder_diag_fact_value(g.page.diag,"layout needed");
    const char *forced=needed?strstr(needed,"forced by a read: "):NULL;
    check(forced && strstr(forced,"one node's boxes moved 1 ("),"layout needed: a read on the shown page is forced");
    if(!forced || !strstr(forced,"one node's boxes moved 1 (")) printf("  layout needed: %s\n",needed?needed:"(none)");
    diag_paint_reading(&g.page,1);
    const char *reading=yonder_diag_fact_value(g.page.diag,"layout needed reading 1");
    forced=reading?strstr(reading,"forced by a read: "):NULL;
    check(forced && strstr(forced,"one node's boxes moved 1 ("),"layout needed: and the reading counts it");
    if(!forced || !strstr(forced,"one node's boxes moved 1 (")) printf("  reading: %s\n",reading?reading:"(none)");
    cost_reset();
    diag_paint_reading(&g.page,2);
    reading=yonder_diag_fact_value(g.page.diag,"layout needed reading 2");
    check(reading && !strstr(reading," 1 ("),"layout needed: and the next reading starts again");
    s_layout_profile=false;
    probe_drop();
    s_ledger=YONDER_LEDGER_OFF;
    /* A bare anchor keeps the cascade tracking: the change that turns it is
     * an update, and the record says what the anchor cost it. */
    probe_page("<style>:has(> .x) p{color:red}</style><div><p id=p>t</p></div>",false);
    g.page.diag=diag_new_claimed("https://fixture.test/profile-bare",1);
    s_layout_profile=true;
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","x",1) && script_rebuild(),
          "layout profile: a bare anchor's page's change published");
    cascade=yonder_diag_fact_value(g.page.diag,"layout cascade");
    check(cascade && strstr(cascade,"updated 1, built fresh 0; anchors held 1, bare 1; bare anchors asked again ") &&
          strstr(cascade,", turned 1, marked ") && !strstr(cascade,"asked again 0,"),
          "layout profile: a bare anchor's page updates its cascade, and says what the anchor cost");
    if(!cascade || !strstr(cascade,"turned 1")) printf("  layout cascade: %s\n",cascade?cascade:"(none)");
    s_layout_profile=false;
    probe_drop();
    layout_profile_stream_cases();
    s_restyle_in_place=true;
}
