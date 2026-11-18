/* Rendering opportunities through the real script host and window painter,
 * on frames of the display (PILE3.md § The animations' frames, on the
 * display's beat). host_frame stands in for the compositor's answer; each
 * frame is closed as the window loop closes it, after the turn's paint. */
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
    frame_close();s_frames.asked=false;
    script_turn();
    check(!g.raf_painting && yonder_scripts_animation_next(g.page.scripts)!=YONDER_NEVER,
          "frame window: with no frame of the display, no callbacks");
    unsigned asked=host_frame_requests;
    pictures_schedule();
    check(animation_next()<=now_ms && host_frame_requests==asked+1 && s_frames.asked,
          "frame window: pending callbacks ask for the display's next frame");
    pictures_schedule();
    check(host_frame_requests==asked+1,"frame window: one request out at a time");
    flow_tree_t *tree=g.page.tree;
    uint64_t version=os64_html_version(page_doc(&g.page));
    now_ms=1016;host_frame();g.ui.dirty=(os64_gui_rect_t){0};
    script_turn();
    check(g.raf_painting && animation_next()==YONDER_NEVER,
          "frame window: another batch waits for this paint");
    check(tree==g.page.tree && version==os64_html_version(page_doc(&g.page)) && g.ui.dirty.w>0,
          "frame window: two pixel callbacks reuse layout and mark paint damage");
    uint32_t *px=stand_in_shot();
    check((px[10*800+10]&0xffffff)==0x00ff00 && (px[10*800+30]&0xffffff)==0x0000ff,
          "frame window: one paint contains both ordered callbacks");
    os64_free(px);
    clock_us+=70000;now_ms+=70;motion_painted();frame_close();
    check(animation_next()<=now_ms,"frame window: after an expensive paint the next batch is due, for the next frame");
    script_turn();
    check(!g.raf_painting,"frame window: and waits for that frame, whatever the clock says");
    g.covered=true;now_ms=6000;
    asked=host_frame_requests;s_frames.asked=false;
    pictures_schedule();
    check(host_frame_requests==asked && !s_frames.asked,"frame window: a covered window asks for no frame");
    host_frame();script_turn();
    check(animation_next()==YONDER_NEVER && !g.raf_painting,
          "frame window: covered page retains callback without running it");
    frame_close();
    g.covered=false;s_animations=false;
    host_frame();script_turn();px=stand_in_shot();
    check((px[10*800+10]&0xffffff)==0xffff00 &&
          yonder_scripts_animation_next(g.page.scripts)==YONDER_NEVER,
          "frame window: uncover runs one retained batch, independent of CSS animation preference");
    os64_free(px);motion_painted();frame_close();probe_drop();

    now_ms=1000;
    probe_page("<div id=box style='width:20px;height:20px'>old</div><script>"
        "var b=document.getElementById('box');"
        "requestAnimationFrame(()=>{b.style.width='60px';Promise.resolve().then(()=>b.textContent='microtask')});"
        "requestAnimationFrame(()=>{b.style.height='30px'});</script>",true);
    script_turn();
    uint64_t updates=garb_cascade_updates(g.page.cascade);
    now_ms=1016;host_frame();script_turn();
    const flow_box_t *box=flow_box_for(g.page.tree,probe_id("box"));
    check(box && box->rect.w==60 && box->rect.h==30 && probe_text_is("box","microtask"),
          "frame window: callback and microtask DOM edits publish together");
    check(garb_cascade_updates(g.page.cascade)==updates+1,
          "frame window: multiple callback mutations consume one cascade update");
    motion_painted();frame_close();probe_drop();

    now_ms=1000;s_animations=true;s_reduce_motion=false;
    probe_page("<!doctype html><style>@keyframes travel{to{margin-left:100px}}"
        "#box{width:20px;height:20px;animation:travel 1s linear infinite}</style>"
        "<div id=box></div><script>requestAnimationFrame(()=>{document.getElementById('box').style.height='30px'})</script>",true);
    relayout(true);script_turn();
    now_ms=1100;
    uint64_t layouts=s_cost.layouts;
    motion_tick();
    check(s_cost.layouts==layouts,"frame window: a CSS animation due lays out only in a frame");
    host_frame();script_turn();motion_tick();
    check(s_cost.layouts==layouts+1,
          "frame window: callback mutation and CSS animation share the rendering layout");
    motion_painted();frame_close();probe_drop();

    // One time a frame: its blank, which the callbacks, the timeline and the
    // CSS layout all take, while performance.now() runs on.
    now_ms=1000;s_animations=true;s_reduce_motion=false;
    probe_page("<!doctype html><style>@keyframes travel{to{margin-left:100px}}"
        "#box{width:20px;height:20px;animation:travel 1s linear infinite}</style>"
        "<div id=box></div><p id=r>waiting</p><script>requestAnimationFrame(t=>{"
        "var tl=document.timeline.currentTime,now=performance.now();"
        "document.getElementById('r').textContent=t===tl&&now-t===5?'one time':t+' '+tl+' '+now})</script>",true);
    relayout(true);script_turn();
    now_ms=2000;host_frame();now_ms=2005;
    script_turn();motion_tick();
    check(probe_text_is("r","one time"),
          "frame time: the callback and the timeline take the frame's time, performance.now() runs on");
    check(s_env.now_ms==2000,"frame time: the frame's CSS layout is the page at the frame's time");
    motion_painted();frame_close();
    // A scroll's paint sampled the animations at now; a frame whose blank
    // came before it takes that later time, never going back.
    now_ms=3000;(void)animation_time_us();now_ms=2995;host_frame();
    check(s_frames.time_us==3000000,"frame time: a frame never hands the animations an earlier time");
    frame_close();
    // The answer arrives as the window's event, and opens the frame; one
    // nothing asked for opens nothing.
    {
        bool was_running=g.running;
        g.running=true;
        uint64_t number=s_frames.number+1, blank=4000000;
        os64_gui_event_t answer={.type=OS64_GUI_EVENT_FRAME,
            .frame={.number_lo=(uint32_t)number,.number_hi=(uint32_t)(number>>32),
                    .blank_lo=(uint32_t)blank,.blank_hi=(uint32_t)(blank>>32),.period_ns=16666667}};
        now_ms=4001;
        s_frames.asked=false;
        window_event(NULL,&answer);
        check(!s_frames.open,"frame time: an answer nothing asked for opens no frame");
        s_frames.asked=true;
        uint64_t answers=s_cost.frame_answers;
        window_event(NULL,&answer);
        check(s_frames.open && s_frames.time_us==4000000 && s_cost.frame_answers==answers+1,
              "frame time: the answer, among the window's events, opens the frame at its blank");
        frame_close();
        g.running=was_running;
    }
    probe_drop();

    now_ms=1000;
    probe_page("<p id=timer>waiting</p><canvas id=c></canvas><script>"
        "function again(){requestAnimationFrame(again)}requestAnimationFrame(again);"
        "setTimeout(()=>document.getElementById('timer').textContent='fired',20);</script>",true);
    script_turn();now_ms=1020;host_frame();script_turn();
    check(probe_text_is("timer","fired") && g.raf_painting,
          "frame window: ordinary due timer and frame both make progress");
    motion_painted();frame_close();probe_drop();
    probe_page("<p>replacement</p>",true);
    check(animation_next()==YONDER_NEVER,"frame window: navigation drops old callbacks");
    asked=host_frame_requests;s_frames.asked=false;pictures_schedule();
    check(host_frame_requests==asked,"frame window: a still page asks for no frame");
    probe_drop();
    // A costly frame still leaves timers, pointer input and idle work their
    // turns between frames; a late frame resumes once, without replaying
    // missed ones.
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
    now_ms=1016;host_frame();script_turn();
    now_ms=1090;clock_us+=70000;motion_painted();frame_close();
    check(probe_text_is("count","1") && animation_next()<=now_ms,
          "frame pacing: a slow frame's successor waits for a frame, not for its cost");
    now_ms=1095;input_click_at("hit");script_turn();
    check(probe_text_is("hit","clicked") && probe_text_is("timer","fired"),
          "frame pacing: pointer and timer tasks run between frames");
    script_turn();
    check(probe_text_is("idle","ran"),"frame pacing: eligible idle work runs before the next frame");
    check(probe_text_is("count","1"),"frame pacing: ordinary task does not run frame callbacks");
    now_ms=1100;host_frame();script_turn();motion_painted();frame_close();
    check(probe_text_is("count","2") && animation_next()<=now_ms,
          "frame pacing: each frame runs the next batch, which is due again at once");
    now_ms=9000;host_frame();script_turn();motion_painted();frame_close();
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
        host_frame();
        bool ready=script_turn();
        check(!ready && probe_text_is("result","waiting"),
              "frame idle: an open frame's due CSS work defers idle without running a script task");
        check(!script_followup(ready,false),
              "frame idle: no retry without task or rendering progress");
        motion_tick();
        bool painted=frame_unpainted() || g.raf_painting;
        motion_painted();frame_close();
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
