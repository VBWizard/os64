/* Rendering opportunities through the real script host and window painter. */
static void animation_window_cases(void)
{
    bool was_animations=s_animations,was_reduce=s_reduce_motion;
    now_ms=1000;clock_step=0;
    probe_page("<!doctype html><style>body{margin:0;background:white}canvas{display:block}</style>"
        "<canvas id=c width=40 height=40></canvas><script>var c=document.getElementById('c'),ctx=c.getContext('2d');"
        "ctx.fillStyle='red';ctx.fillRect(0,0,40,40);"
        "requestAnimationFrame(()=>{ctx.fillStyle='blue';ctx.fillRect(0,0,40,40)});"
        "requestAnimationFrame(()=>{ctx.fillStyle='lime';ctx.fillRect(0,0,20,40);"
        "requestAnimationFrame(()=>{ctx.fillStyle='yellow';ctx.fillRect(0,0,40,40)})});</script>",true);
    script_turn();
    flow_tree_t *tree=g.page.tree;
    uint64_t version=os64_html_version(page_doc(&g.page));
    check(animation_next()==1020,"frame window: ticker deadline for pending callbacks");
    now_ms=1020;g.ui.dirty=(os64_gui_rect_t){0};
    script_turn();
    check(g.raf_painting && animation_next()==YONDER_NEVER,
          "frame window: another batch waits for this paint");
    check(tree==g.page.tree && version==os64_html_version(page_doc(&g.page)) && g.ui.dirty.w>0,
          "frame window: two pixel callbacks reuse layout and mark paint damage");
    uint32_t *px=stand_in_shot();
    check((px[10*800+10]&0xffffff)==0x00ff00 && (px[10*800+30]&0xffffff)==0x0000ff,
          "frame window: one paint contains both ordered callbacks");
    os64_free(px);
    clock_us+=70000;now_ms+=70;motion_painted();
    check(animation_next()==1100,"frame window: expensive paint leaves a fixed input/idle opportunity");
    now_ms=1099;script_turn();
    check(!g.raf_painting,"frame window: callback waits through post-paint pause");
    g.covered=true;now_ms=6000;
    script_turn();
    check(animation_next()==YONDER_NEVER && !g.raf_painting,
          "frame window: covered page retains callback without running it");
    g.covered=false;s_animations=false;
    script_turn();px=stand_in_shot();
    check((px[10*800+10]&0xffffff)==0xffff00 &&
          yonder_scripts_animation_next(g.page.scripts)==YONDER_NEVER,
          "frame window: uncover runs one retained batch, independent of CSS animation preference");
    os64_free(px);motion_painted();probe_drop();

    now_ms=1000;
    probe_page("<div id=box style='width:20px;height:20px'>old</div><script>"
        "var b=document.getElementById('box');"
        "requestAnimationFrame(()=>{b.style.width='60px';Promise.resolve().then(()=>b.textContent='microtask')});"
        "requestAnimationFrame(()=>{b.style.height='30px'});</script>",true);
    script_turn();
    uint64_t updates=garb_cascade_updates(g.page.cascade);
    now_ms=1020;script_turn();
    const flow_box_t *box=flow_box_for(g.page.tree,probe_id("box"));
    check(box && box->rect.w==60 && box->rect.h==30 && probe_text_is("box","microtask"),
          "frame window: callback and microtask DOM edits publish together");
    check(garb_cascade_updates(g.page.cascade)==updates+1,
          "frame window: multiple callback mutations consume one cascade update");
    motion_painted();probe_drop();

    now_ms=1000;s_animations=true;s_reduce_motion=false;
    probe_page("<!doctype html><style>@keyframes travel{to{margin-left:100px}}"
        "#box{width:20px;height:20px;animation:travel 1s linear infinite}</style>"
        "<div id=box></div><script>requestAnimationFrame(()=>{document.getElementById('box').style.height='30px'})</script>",true);
    relayout(true);script_turn();
    now_ms=1100;
    uint64_t layouts=s_cost.layouts;
    script_turn();motion_tick();
    check(s_cost.layouts==layouts+1,
          "frame window: callback mutation and CSS animation share the rendering layout");
    motion_painted();probe_drop();

    now_ms=1000;
    probe_page("<p id=timer>waiting</p><canvas id=c></canvas><script>"
        "function again(){requestAnimationFrame(again)}requestAnimationFrame(again);"
        "setTimeout(()=>document.getElementById('timer').textContent='fired',20);</script>",true);
    script_turn();now_ms=1020;script_turn();
    check(probe_text_is("timer","fired") && g.raf_painting,
          "frame window: ordinary due timer and frame both make progress");
    motion_painted();probe_drop();
    probe_page("<p>replacement</p>",true);
    check(animation_next()==YONDER_NEVER,"frame window: navigation drops old callbacks");
    probe_drop();
    // Repeated costly paints must still admit timers, pointer input and idle
    // work; a late frame resumes once, without replaying missed intervals.
    now_ms=1000;
    probe_page("<button id=hit>waiting</button><p id=timer>waiting</p><p id=idle>waiting</p>"
        "<p id=count>0</p><script>var n=0;function frame(){"
        "document.getElementById('count').textContent=String(++n);requestAnimationFrame(frame)}"
        "requestAnimationFrame(frame);"
        "document.getElementById('hit').onclick=()=>{document.getElementById('hit').textContent='clicked';"
        "requestIdleCallback(()=>document.getElementById('idle').textContent='ran')};"
        "setTimeout(()=>document.getElementById('timer').textContent='fired',95);"
        "</script>",true);
    script_turn();
    now_ms=1020;script_turn();
    now_ms=1090;clock_us+=70000;motion_painted();
    check(animation_next()==1100,"frame pacing: a slow paint incurs ten milliseconds, not its own cost");
    now_ms=1095;input_click_at("hit");script_turn();
    check(probe_text_is("hit","clicked") && probe_text_is("timer","fired"),
          "frame pacing: pointer and timer tasks run during the pause");
    motion_painted();script_turn();
    check(probe_text_is("idle","ran"),"frame pacing: eligible idle work uses the post-paint gap");
    check(probe_text_is("count","1"),"frame pacing: ordinary task does not bypass frame deadline");
    now_ms=1100;script_turn();motion_painted();
    check(probe_text_is("count","2") && animation_next()==1120,
          "frame pacing: fast frame retains nominal DOM deadline");
    now_ms=9000;script_turn();motion_painted();
    check(probe_text_is("count","3"),"frame pacing: long absence runs one batch without catching up");
    probe_drop();
    // Wake for CSS alone: idle must get a turn between frames even when
    // no ordinary task or animation-frame callback made script_turn succeed.
    for (int kind=0;kind<2;kind++) {
        now_ms=1000;s_animations=true;s_reduce_motion=false;
        char html[640];
        os64_snprintf(html,sizeof(html),
            "<!doctype html><style>@keyframes move{to{%s}}"
            "#box{width:20px;height:20px;background:red;animation:move 1s linear infinite}</style>"
            "<div id=box></div><p id=result>waiting</p><script>"
            "requestIdleCallback(()=>document.getElementById('result').textContent='idle ran')</script>",
            kind ? "background-color:blue" : "margin-left:100px");
        probe_page(html,true);relayout(true);script_turn();
        check(yonder_scripts_idle_next(g.page.scripts)==0,
              "frame idle: callback is runnable");
        bool ready=script_turn();
        check(!ready && probe_text_is("result","waiting"),
              "frame idle: due CSS frame defers idle without running a script task");
        check(!script_followup(ready,false),
              "frame idle: no retry without task or rendering progress");
        motion_tick();
        bool painted=frame_unpainted() || g.raf_painting;
        motion_painted();
        check(painted && script_followup(ready,painted),
              "frame idle: completed CSS frame schedules a follow-up without another event");
        g.scripts_on=false;
        check(!script_followup(false,painted),"frame idle: disabled scripts do not poll");
        g.scripts_on=true;
        os64_html_set_attr(g.page.way.doc,probe_id("box"),"title","changed",7);
        check(!script_followup(false,painted),"frame idle: owed rebuild blocks follow-up");
        check(script_rebuild(),"frame idle: native rebuild completes");
        motion_painted();
        check(script_turn() && probe_text_is("result","idle ran"),
              "frame idle: follow-up delivers idle callback before next CSS frame");
        motion_painted();
        check(!script_followup(false,false),"frame idle: drained work does not poll");
        probe_drop();
    }
    s_animations=was_animations;s_reduce_motion=was_reduce;
}
