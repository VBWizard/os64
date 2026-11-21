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

/* The number after `label` in a record line, 0 for none. */
static unsigned record_number(const char *line, const char *label)
{
    const char *at=line?strstr(line,label):NULL;
    unsigned n=0;
    if(at) sscanf(at+strlen(label),"%u",&n);
    return n;
}

/* A GEOMETRY READ ANSWERED FROM THE SHOWN TREE (N2, shown_pending): a page
 * whose script writes on a bar's twelve parts and reads them back, the
 * writes and reads by the id of the button clicked. */
static const char kKeptReadsPage[]=
    "<!doctype html><style>#bar{display:flex;width:600px}.part{flex:1;height:12px;transform:scaleX(1)}"
    "#box{width:300px;height:40px;overflow:auto}#tall{height:200px}%s</style>"
    "<div id=bar></div><div id=box><div id=tall></div></div><p id=out>-</p>"
    "<button id=transform>t</button><button id=colour>c</button><button id=klass>k</button>"
    "<button id=scroll>s</button><button id=scrollmove>m</button><button id=rect>r</button>"
    "<button id=quiet>q</button><script>"
    "const bar=document.getElementById('bar'),box=document.getElementById('box');"
    "for(let i=0;i<12;i++){const d=document.createElement('div');d.className='part';bar.appendChild(d)}let t=0;"
    "document.onclick=e=>{t++;const m=e.target.id,parts=bar.children;let sum=0;"
    "if(m==='quiet'){for(const p of parts)p.style.transform='scaleX('+(t/10)+')';return}"
    "if(m==='scroll'){bar.style.backgroundColor='rgb(0,'+t+',0)';sum=box.scrollHeight}"
    "else if(m==='scrollmove'){parts[0].style.transform='scaleX('+(t/10)+')';sum=box.scrollHeight}"
    "else for(const p of parts){"
    "if(m==='transform')p.style.transform='scaleX('+(t/10)+')';"
    "else if(m==='colour')p.style.backgroundColor='rgb('+t+',0,0)';"
    "else if(m==='klass')p.className='part k'+t;"
    "else if(m==='rect'){p.style.transform='scaleX('+(t/10)+')';sum+=Math.round(p.getBoundingClientRect().width);continue}"
    "sum+=p.clientWidth}"
    "document.getElementById('out').textContent=sum}</script>";
static void kept_reads_click(const char *id)
{
    os64_dom_event_t click={.type="click",.bubbles=true,.cancelable=true};
    os64_js_outcome_t out;
    yonder_scripts_dispatch(g.page.scripts,probe_id(id),&click,NULL,&out);
}
/* The read check's comparison (read_difference): each member a read takes,
 * and the scroll members only for a read of the reach. */
static void read_difference_cases(void)
{
    os64_dom_geometry_t a={.client_width=10,.offset_left=3,.scroll_height=50},b=a;
    char said[128];
    read_difference(&a,&b,OS64_DOM_NEED_REACH,said,sizeof(said));
    check(said[0]==0,"read check: the same answers differ in nothing");
    b.client_width=11;
    read_difference(&a,&b,OS64_DOM_NEED_BOXES,said,sizeof(said));
    check(strstr(said,"clientWidth (10 kept, 11 current)")!=NULL,"read check: a client member is named, with both values");
    b=a; b.offset_parent=(const os64_html_node_t *)&a;
    read_difference(&a,&b,OS64_DOM_NEED_BOXES,said,sizeof(said));
    check(!strcmp(said,"offsetParent"),"read check: an offset parent is named");
    b=a; b.scroll_height=60;
    read_difference(&a,&b,OS64_DOM_NEED_BOXES,said,sizeof(said));
    check(said[0]==0,"read check: a read of boxes takes no scroll member");
    read_difference(&a,&b,OS64_DOM_NEED_REACH,said,sizeof(said));
    check(strstr(said,"scrollHeight")!=NULL,"read check: a read of the reach does");
}
static void kept_reads_cases(void)
{
    read_difference_cases();
    char html[4096];
    snprintf(html,sizeof(html),kKeptReadsPage,"");
    s_ledger=YONDER_LEDGER_ON;
    probe_page(html,true);
    script_turn();
    g.page.diag=diag_new_claimed("https://fixture.test/kept-reads",1);
    s_layout_profile=true;
    uint64_t layouts=geometry_layout_total;
    kept_reads_click("transform");
    const char *reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"from the kept tree ")==12 && geometry_layout_total==layouts,
          "kept reads: twelve transforms written and read back are answered from the tree, with no layout");
    if(record_number(reads,"from the kept tree ")!=12 || geometry_layout_total!=layouts)
        printf("  layout reads: %s (layouts %lu)\n",reads?reads:"(none)",(unsigned long)(geometry_layout_total-layouts));
    check(probe_text_is("out","600"),"kept reads: and they answer as a layout would (twelve parts of 50 px)");
    script_turn();
    kept_reads_click("colour");
    reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"from the kept tree ")==24,"kept reads: a colour written is answered from the tree");
    script_turn();
    kept_reads_click("klass");
    reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"from the kept tree ")==24,"kept reads: a class written is not");
    script_turn();
    kept_reads_click("scroll");
    reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"from the kept tree ")==25 && probe_text_is("out","200"),
          "kept reads: a scroll read with a colour pending is answered from the tree");
    script_turn();
    kept_reads_click("scrollmove");
    reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"from the kept tree ")==25 && record_number(reads,"scroll ")==1,
          "kept reads: a scroll read with a transform pending makes the page current, and is counted");
    script_turn();
    kept_reads_click("rect");
    reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"rects ")==12,"kept reads: a rectangle read with a transform pending makes the page current, and is counted");
    if(record_number(reads,"rects ")!=12) printf("  layout reads: %s\n",reads?reads:"(none)");
    script_turn();
    // --geometry-check: every read the tree answered, answered the old way
    // too, and none differs.
    s_geometry_check=1;
    s_reads_checked=0;
    s_reads_compared=s_reads_differed=0;
    kept_reads_click("transform");
    const char *checked=yonder_diag_fact_value(g.page.diag,"read check");
    check(checked && !strncmp(checked,"12 compared, 0 differed",23),
          "kept reads: --geometry-check answers each the old way too, and none differs");
    if(!checked || strncmp(checked,"12 compared, 0 differed",23)) printf("  read check: %s\n",checked?checked:"(none)");
    s_geometry_check=s_reads_checked=0;
    s_reads_compared=s_reads_differed=0;
    script_turn();
    // A covered window owes its rendering step while only paint-only writes
    // are pending, and pays it once seen. (The other clicks end by writing
    // a text, which no covered window may owe.)
    kept_reads_click("quiet");
    check(page_rebuild_owed(&g.page),"covered: the writes are pending");
    g.covered=true;
    check(script_rendering_step() && page_rebuild_owed(&g.page),"covered: the rendering step is owed, not run");
    reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"steps owed while covered ")==1,"covered: and counted");
    g.covered=false;
    check(script_rendering_step() && !page_rebuild_owed(&g.page),"covered: seen again, the step is run");
    // A layout asked while a step is owed (a picture arriving, a resize)
    // pays the step first, where it used to be refused as out of memory.
    kept_reads_click("quiet");
    g.covered=true;
    check(script_rendering_step() && page_rebuild_owed(&g.page),"covered: owed again");
    check(relayout(true) && !page_rebuild_owed(&g.page),"covered: a layout asked while it is owed pays it first");
    g.covered=false;
    s_layout_profile=false;
    probe_drop();
    // A selector that reads the style attribute: nothing is answered from
    // the tree.
    snprintf(html,sizeof(html),kKeptReadsPage,"[style*=zzz]{width:1px}");
    probe_page(html,true);
    script_turn();
    g.page.diag=diag_new_claimed("https://fixture.test/kept-reads-style",1);
    s_layout_profile=true;
    kept_reads_click("transform");
    reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    check(record_number(reads,"from the kept tree ")==0 && probe_text_is("out","600"),
          "kept reads: a selector reading the style attribute makes every read current");
    s_layout_profile=false;
    probe_drop();
    s_ledger=YONDER_LEDGER_OFF;
}

/* THE N2 TABLE'S TEST (libflow pending.inc, kPending): every longhand the
 * table admits, on each side its rule admits, set alone on every element of
 * a page made to vary what a box can be (positioned, fixed and absolute
 * under a transform and a filter, flex, inline, inline-block, a table cell,
 * a scroll container, a picture). Where N2 judges the write PAINT, a layout
 * after it must answer every offset, client and scroll member and
 * offsetParent as the layout before it did (and the rectangles, but for
 * the families a transform's reach and matrix move). A longhand in the
 * table with no sample here, or admitted on no element, fails. */
static const struct { const char *prop; const char *pairs[3][2]; } kTableSamples[]={
    {"color",{{"red","blue"}}},
    {"background-color",{{"red","blue"}}},
    {"background-image",{{"none","linear-gradient(red,blue)"}}},
    {"background-repeat",{{"repeat","no-repeat"}}},
    {"background-position-x",{{"0px","5px"}}},
    {"background-position-y",{{"0px","5px"}}},
    {"background-size",{{"auto","10px 10px"}}},
    {"background-origin",{{"padding-box","content-box"}}},
    {"background-clip",{{"border-box","content-box"}}},
    {"border-top-color",{{"red","blue"}}},
    {"border-right-color",{{"red","blue"}}},
    {"border-bottom-color",{{"red","blue"}}},
    {"border-left-color",{{"red","blue"}}},
    {"border-top-left-radius",{{"0px","5px"}}},
    {"border-top-right-radius",{{"0px","5px"}}},
    {"border-bottom-right-radius",{{"0px","5px"}}},
    {"border-bottom-left-radius",{{"0px","5px"}}},
    {"box-shadow",{{"none","0 0 5px red"}}},
    {"text-shadow",{{"none","1px 1px red"}}},
    {"outline-width",{{"1px","5px"}}},
    {"outline-style",{{"none","solid"}}},
    {"outline-color",{{"red","blue"}}},
    {"outline-offset",{{"0px","3px"}}},
    {"opacity",{{"1","0.5"},{"0.5","0"}}},
    {"z-index",{{"auto","5"}}},
    {"isolation",{{"auto","isolate"}}},
    {"mix-blend-mode",{{"normal","multiply"}}},
    {"mask-image",{{"none","linear-gradient(red,blue)"}}},
    {"mask-repeat",{{"repeat","no-repeat"}}},
    {"mask-position-x",{{"0px","5px"}}},
    {"mask-position-y",{{"0px","5px"}}},
    {"mask-size",{{"auto","10px"}}},
    {"mask-origin",{{"border-box","content-box"}}},
    {"mask-clip",{{"border-box","content-box"}}},
    {"mask-composite",{{"add","subtract"}}},
    {"mask-mode",{{"match-source","alpha"}}},
    {"clip-path",{{"none","url(#clip)"}}},  // libgarb reads url() and none
    {"object-fit",{{"fill","contain"}}},
    {"object-position",{{"50% 50%","0 0"}}},
    {"image-rendering",{{"auto","pixelated"}}},
    {"cursor",{{"auto","pointer"}}},
    {"pointer-events",{{"auto","none"}}},
    {"transform-origin",{{"50% 50%","0 0"}}},
    {"fill",{{"black","red"}}},
    // The side of none: both values drawn (a plain inline, which no
    // transform reaches, is admitted either way).
    {"transform",{{"translateX(3px)","scale(2)"}}},
    {"translate",{{"3px","5px 2px"}}},
    {"rotate",{{"10deg","45deg"}}},
    {"scale",{{"2","0.5"}}},
    {"filter",{{"blur(1px)","grayscale(1)"}}},
    {"backdrop-filter",{{"blur(1px)","blur(3px)"}}},
    {"visibility",{{"visible","hidden"},{"hidden","visible"}}},
};
static const char kTablePage[]=
    "<!doctype html><style>body{margin:0}#row{display:flex}#row>div{flex:1;height:10px}</style>"
    "<div id=rel style='position:relative;padding:3px'><div id=fx style='position:fixed;top:5px;left:5px;width:10px;height:10px'></div>"
    "<div id=ab style='position:absolute;top:2px;right:3px;width:6px;height:6px'></div>text</div>"
    "<div id=tf style='transform:translateX(2px);height:20px'><div id=tfab style='position:absolute;top:3px;left:4px;width:8px;height:8px'></div></div>"
    "<div id=fl style='filter:blur(1px);height:20px'><div id=flfx style='position:fixed;bottom:3px;left:4px;width:8px;height:8px'></div></div>"
    "<div id=row><div id=item1></div><div id=item2></div></div>"
    "<p id=para>a <span id=inl>plain inline</span> and <span id=ib style='display:inline-block;width:30px'>block</span></p>"
    "<table><tr><td id=cell>cell</td><td>other</td></tr></table>"
    "<div id=sc style='overflow:auto;height:30px;width:100px'><div id=sccontent style='height:100px;width:200px'>x</div></div>"
    "<img id=pic width=20 height=10 alt=''>";
static const char *const kTableIds[]={"rel","fx","ab","tf","tfab","fl","flfx","row","item1","para","inl","ib","cell","sc","sccontent","pic"};
#define TABLE_NIDS (sizeof(kTableIds)/sizeof(kTableIds[0]))
/* Every element's answers in the shown tree, in kTableIds' order. */
static void table_answers(os64_dom_geometry_t *out)
{
    for(size_t i=0;i<TABLE_NIDS;i++){
        memset(&out[i],0,sizeof(out[i]));
        yonder_geometry_snapshot(g.page.way.doc,g.page.tree,probe_id(kTableIds[i]),800,600,g.zoom,(flow_point_t){0,0},&out[i]);
    }
}
/* The first difference a write of a longhand of `kind` must not make, or
 * NULL. */
static const char *table_difference(const os64_dom_geometry_t *a, const os64_dom_geometry_t *b, int kind)
{
    if(a->offset_left!=b->offset_left || a->offset_top!=b->offset_top) return "offsetLeft/Top";
    if(a->offset_width!=b->offset_width || a->offset_height!=b->offset_height) return "offsetWidth/Height";
    if(a->client_left!=b->client_left || a->client_top!=b->client_top) return "clientLeft/Top";
    if(a->client_width!=b->client_width || a->client_height!=b->client_height) return "clientWidth/Height";
    if(a->offset_parent!=b->offset_parent) return "offsetParent";
    if(a->boxed!=b->boxed) return "a box";
    if(a->scroll_left!=b->scroll_left || a->scroll_top!=b->scroll_top) return "scrollLeft/Top";
    // pending.inc's PENDING_TRANSFORM (2) and PENDING_ORIGIN (5) move a
    // transform's matrix and reach; PENDING_FILTER (3) a filter's bounds.
    bool reach=kind==2 || kind==5;
    if(!reach && (a->scroll_width!=b->scroll_width || a->scroll_height!=b->scroll_height)) return "scrollWidth/Height";
    bool drawn=kind==2 || kind==3 || kind==5;
    if(!drawn && (a->x!=b->x || a->y!=b->y || a->width!=b->width || a->height!=b->height)) return "the rectangle";
    return NULL;
}
static void pending_table_cases(void)
{
    bool restyle=s_restyle_in_place;
    s_restyle_in_place=false;   // every change laid out whole
    // Every longhand the table admits has a sample.
    for(int prop=0;prop<GARB_NPROPS;prop++){
        if(!flow_pending_longhand(prop)) continue;
        bool sampled=false;
        for(size_t k=0;k<sizeof(kTableSamples)/sizeof(kTableSamples[0]) && !sampled;k++)
            sampled=!strcmp(kTableSamples[k].prop,garb_prop_name((garb_prop_t)prop));
        if(!sampled) printf("  the N2 table admits %s, which has no sample here\n",garb_prop_name((garb_prop_t)prop));
        check(sampled,"table: every longhand the N2 table admits has a sample");
    }
    probe_page(kTablePage,false);
    static os64_dom_geometry_t before[TABLE_NIDS],after[TABLE_NIDS];
    unsigned admitted_total=0,differed=0;
    for(size_t k=0;k<sizeof(kTableSamples)/sizeof(kTableSamples[0]);k++){
        int prop=-1;
        for(int q=0;q<GARB_NPROPS && prop<0;q++) if(!strcmp(garb_prop_name((garb_prop_t)q),kTableSamples[k].prop)) prop=q;
        int kind=prop>=0?flow_pending_longhand(prop):0;
        check(kind!=0,"table: each sample names a longhand the table admits");
        unsigned admitted=0;
        for(int pair=0;pair<3 && kTableSamples[k].pairs[pair][0];pair++)
            for(size_t e=0;e<TABLE_NIDS;e++){
                os64_html_node_t *el=probe_id(kTableIds[e]);
                // The element's own style, copied: a write replaces the
                // attribute it lives in.
                const os64_html_attr_t *had=os64_html_attr(el,"style");
                bool styled=had && had->value;
                char base[256]="",value[512];
                if(styled) snprintf(base,sizeof(base),"%s;",had->value);
                snprintf(value,sizeof(value),"%s%s:%s",base,kTableSamples[k].prop,kTableSamples[k].pairs[pair][0]);
                os64_html_set_attr(g.page.way.doc,el,"style",value,strlen(value));
                script_rebuild();
                table_answers(before);
                snprintf(value,sizeof(value),"%s%s:%s",base,kTableSamples[k].prop,kTableSamples[k].pairs[pair][1]);
                os64_html_set_attr(g.page.way.doc,el,"style",value,strlen(value));
                if(shown_pending().verdict==FLOW_PENDING_PAINT){
                    admitted++;
                    script_rebuild();
                    table_answers(after);
                    for(size_t i=0;i<TABLE_NIDS;i++){
                        const char *what=table_difference(&before[i],&after[i],kind);
                        if(what){
                            differed++;
                            printf("  %s:%s -> %s on #%s moved %s of #%s\n",kTableSamples[k].prop,
                                   kTableSamples[k].pairs[pair][0],kTableSamples[k].pairs[pair][1],
                                   kTableIds[e],what,kTableIds[i]);
                            break;
                        }
                    }
                }
                if(styled) os64_html_set_attr(g.page.way.doc,el,"style",base,strlen(base)-1);
                else os64_html_remove_attr(g.page.way.doc,el,"style");
                script_rebuild();
            }
        if(!admitted) printf("  %s was admitted on no element\n",kTableSamples[k].prop);
        check(admitted>0,"table: each longhand is admitted somewhere, so the test reaches it");
        admitted_total+=admitted;
    }
    check(differed==0,"table: no write the N2 table admits moves a box, a client, offset or scroll answer, or an offset parent");
    printf("  N2 table: %u writes admitted, %u moved something\n",admitted_total,differed);
    probe_drop();
    s_restyle_in_place=restyle;
}

/* THE RANDOM WALK (N2): a page whose script, from a fixed seed, makes
 * random writes of every kind (paint-only longhands by style.x, cssText and
 * setAttribute; geometry; classes; text; custom properties) between random
 * reads of every kind. With --geometry-check, every Nth read the tree
 * answers is answered the old way too, which makes the page current: N is 1
 * (only the writes since the last read pending), then 3 and 7 (writes on
 * many elements, judged and merged, pile up between checks). None may
 * differ, and the tree must have answered some. */
static const char kWalkPage[]=
    "<!doctype html><style>.t{transform:translateX(2px)}.wide{width:120px}#sc{overflow:auto;height:40px}"
    "#sc>div{height:120px}.pos{position:relative}.f{filter:blur(1px)}</style>"
    "<div id=sc><div id=in></div></div>"
    "<div id=a class=t>alpha</div><div id=b class=pos>beta<span id=c>gamma</span></div>"
    "<p id=d class=f>delta</p><div id=e style='display:flex'><div id=e1>x</div><div id=e2>y</div></div>"
    "<span id=g>inline</span><button id=go>go</button><p id=out>-</p><script>"
    "let seed=0x5eed;function r(n){seed=(Math.imul(seed,1103515245)+12345)&0x7fffffff;return (seed>>>16)%n}"
    "const els=['a','b','c','d','e','e1','e2','g','sc','in'].map(i=>document.getElementById(i));"
    "const paint=[['color',['red','blue','green']],['backgroundColor',['red','blue']],['opacity',['1','0.5','0']],"
    "['visibility',['visible','hidden']],['boxShadow',['none','0 0 3px red']],['outlineWidth',['1px','4px']],"
    "['transform',['translateX(3px)','scale(1.5)','rotate(5deg)','none']],['zIndex',['auto','3']],"
    "['filter',['blur(1px)','grayscale(1)','none']]];"
    "let sum=0;"
    "document.onclick=()=>{for(let i=0;i<300;i++){const el=els[r(els.length)],k=r(16);"
    "if(k<6){const p=paint[r(paint.length)];el.style[p[0]]=p[1][r(p[1].length)]}"
    "else if(k==6){el.style.cssText=el.style.cssText+';color:'+['red','blue'][r(2)]}"
    "else if(k==7){el.setAttribute('style','color:'+['red','blue'][r(2)]+';opacity:0.5')}"
    "else if(k==8){el.style.width=r(2)?'':(50+r(100))+'px'}"
    "else if(k==9){el.classList.toggle(['t','wide','pos','f'][r(4)])}"
    "else if(k==10&&r(4)==0){el.firstChild&&el.firstChild.nodeType==3?el.firstChild.data='t'+r(9):0}"
    "else if(k==11&&r(4)==0){el.style.setProperty('--v',''+r(9))}"
    "else{const q=r(9);"
    "sum+=q==0?el.clientWidth:q==1?el.clientHeight:q==2?el.offsetLeft:q==3?el.offsetTop:q==4?el.offsetWidth:"
    "q==5?(el.offsetParent?1:0):q==6?el.scrollTop:q==7?el.scrollHeight:Math.round(el.getBoundingClientRect().width)}}"
    "document.getElementById('out').textContent='walked'};"
    "document.getElementById('out').textContent='ready'"
    "</script>";
static void random_walk(uint32_t every)
{
    s_ledger=YONDER_LEDGER_ON;
    probe_page(kWalkPage,true);
    script_turn();
    check(probe_text_is("out","ready"),"random walk: the page's script ran");
    g.page.diag=diag_new_claimed("https://fixture.test/random-walk",1);
    s_layout_profile=true;
    s_geometry_check=every;
    s_reads_checked=0;
    s_reads_compared=s_reads_differed=0;
    for(int round=0;round<10;round++){
        os64_dom_event_t click={.type="click",.bubbles=true,.cancelable=true};
        os64_js_outcome_t out={0};
        os64_js_status_t status=yonder_scripts_dispatch(g.page.scripts,probe_id("go"),&click,NULL,&out);
        if(status!=OS64_JS_OK) printf("  random walk round %d: %s (line %u)\n",round,out.message,out.line);
        script_turn();
    }
    check(probe_text_is("out","walked"),"random walk: the walk ran to its end");
    const char *checked=yonder_diag_fact_value(g.page.diag,"read check");
    const char *reads=yonder_diag_fact_value(g.page.diag,"layout reads");
    unsigned compared=record_number(checked,""),kept=record_number(reads,"from the kept tree ");
    check(checked && strstr(checked," 0 differed") && compared>30 && kept>=compared && kept<compared*every+every,
          "random walk: every Nth read the tree answered is answered the old way too, and none differs");
    printf("  random walk, every %u: %s; %s\n",every,checked?checked:"(no read check)",reads?reads:"(no reads line)");
    s_geometry_check=s_reads_checked=0;
    s_reads_compared=s_reads_differed=0;
    s_layout_profile=false;
    probe_drop();
    s_ledger=YONDER_LEDGER_OFF;
}
static void random_walk_cases(void)
{
    random_walk(1);
    random_walk(3);
    random_walk(7);
}

static void layout_profile_cases(void)
{
    restyle_kept_cases();
    kept_reads_cases();
    pending_table_cases();
    random_walk_cases();
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
