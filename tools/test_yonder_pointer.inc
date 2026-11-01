/* Drive the same page ingress as native events, including libui defaults. */
static void pointer_window(uint8_t type, int x, int y, int button, unsigned buttons)
{
    g.running = true;
    g.root.bounds = g.view.bounds;
    g.view.cls = &kViewClass;
    check(os64_ui_on_gesture_cancel(&g.ui, pointer_widget_canceled), "pointer: native cancellation observer");
    os64_gui_event_t event = {.type=type, .mouse={.x=x,.y=y,.button=button,.buttons=buttons}};
    window_event(NULL,&event);
}
static void pointer_move(int x, int y, unsigned buttons)
{
    pointer_window(OS64_GUI_EVENT_MOUSE_MOVE,x,y,0,buttons);
}
static void pointer_frame_cases(void)
{
    const char *handlers[] = {
        "", "e.preventDefault()", "e.target.remove()",
        "e.target.setAttribute('src','/changed')", "location.href='/chosen'",
        "e.target.removeAttribute('src')", "e.target.setAttribute('src','')"
    };
    const char *destinations[] = {"/frame-source",NULL,NULL,"/changed","/chosen",NULL,NULL};
    for (unsigned i=0;i<sizeof(handlers)/sizeof(handlers[0]);i++) {
        char html[1024];
        snprintf(html,sizeof(html),"<iframe id=f width=200 height=100 src=/frame-source></iframe>"
            "<p id=out>0</p><script>var n=0;document.getElementById('f').onclick=e=>{"
            "document.getElementById('out').textContent=String(++n);%s};</script>",handlers[i]);
        loop_page(html);
        check(loop_settle() && g.page.tree!=NULL,"pointer frame: fixture arrives");
        input_click_at("f");
        check(probe_text_is("out","1"),"pointer frame: exactly one DOM click");
        if(destinations[i]) {
            char expected[128];snprintf(expected,sizeof(expected),"http://fixture.test%s",destinations[i]);
            const yonder_trip_t *trip=pool_work.job;
            check(g.stream.active && trip && os64_streq(trip->url,expected),
                  "pointer frame: source follows after click, with mutation and script-navigation precedence");
            if(i==0 && trip) check(trip->metadata.activation,"pointer frame: source navigation is user initiated");
        } else check(!g.stream.active,"pointer frame: cancellation/removal prevents source navigation");
        loop_drop();
    }
    for (int off=0;off<2;off++) {
        loop_modern_off=off;
        loop_page("<frameset><frame id=f src=/legacy-frame></frameset>");
        check(loop_settle() && g.page.tree!=NULL,"pointer frame: frameset arrives");
        input_click_at("f");
        const yonder_trip_t *trip=pool_work.job;
        check(g.stream.active && trip && os64_streq(trip->url,"http://fixture.test/legacy-frame"),
              "pointer frame: FRAME source opens with Modern Web on and off");
        loop_drop();
    }
    loop_modern_off=false;
    loop_page("<iframe id=f src=#destination></iframe><p id=destination>here</p>");
    check(loop_settle() && g.page.tree!=NULL,"pointer frame: same-document source arrives");
    input_click_at("f");
    check(!g.stream.active && strstr(g.page.way.url,"#destination") && scripts_live(),
          "pointer frame: fragment source runs outside the completed input task");
    loop_drop();
}
static void pointer_disabled_fieldsets(void)
{
    const char *cases[]={
        "<fieldset disabled>%s</fieldset>",
        "<fieldset disabled><legend><span>%s</span></legend></fieldset>",
        "<fieldset disabled><legend>first</legend><legend>%s</legend></fieldset>",
        "<fieldset disabled><legend>outer first</legend><fieldset><legend>%s</legend></fieldset></fieldset>",
        "<fieldset disabled><legend><fieldset disabled><legend>%s</legend></fieldset></legend></fieldset>",
        "<fieldset disabled><legend><fieldset disabled>%s</fieldset></legend></fieldset>"};
    for(unsigned radio=0;radio<2;++radio)for(unsigned i=0;i<6;++i) {
        char control[128],group[512],html[1400];
        snprintf(control,sizeof(control),"<input id=c type=%s name=g>",radio?"radio":"checkbox");
        snprintf(group,sizeof(group),cases[i],control);
        snprintf(html,sizeof(html),"<input id=other type=radio name=g checked>%s<p id=out>0</p>"
            "<script>var c=document.getElementById('c'),count=0;"
            "c.oninput=c.onchange=()=>document.getElementById('out').textContent=String(++count);"
            "if(c.type==='checkbox')c.indeterminate=true;</script>",group);
        probe_page(html,true);script_turn();ui_click_on("c");
        bool expected=i==1||i==4,checked=false,other=false,indeterminate=false;
        os64_page_state_t *state=os64_page_shared_state(page_model(&g.page));
        check(os64_page_node_checked(state,probe_id("c"),&checked)==0 && checked==expected &&
              probe_field("c")->u.check.checked==expected,
              "pointer: checkbox/radio native activation honors fieldsets and first-legend exceptions");
        check(probe_text_is("out",expected?"2":"0"),
              "pointer: fieldset-disabled controls emit no input/change events");
        if(radio)check(os64_page_node_checked(state,probe_id("other"),&other)==0 && other==!expected,
              "pointer: a disabled radio does not clear its enabled group peer");
        else check(os64_page_node_indeterminate(state,probe_id("c"),&indeterminate)==0 && indeterminate==!expected,
              "pointer: disabled checkbox preactivation preserves indeterminate");
        probe_drop();
    }
    for(unsigned disabled=0;disabled<2;++disabled) {
        char html[700];snprintf(html,sizeof(html),
            "<fieldset id=f %s><input id=c type=checkbox></fieldset>"
            "<script>var f=document.getElementById('f'),c=document.getElementById('c');"
            "c.onpointerdown=()=>f.disabled=%s;</script>",disabled?"disabled":"",disabled?"false":"true");
        probe_page(html,true);script_turn();ui_click_on("c");
        check(probe_field("c")->u.check.checked==(disabled!=0),
              "pointer: activation observes fieldset changes made during pointerdown");
        probe_drop();
    }
}
static void pointer_window_cases(void)
{
    pointer_disabled_fieldsets();
    probe_page("<!doctype html><style>body{margin:0}#pad{width:100px;height:80px;background:green}</style>"
        "<div id=pad></div><p id=out>-</p><script>var pad=document.getElementById('pad'),log=[];"
        "function note(e){log.push(e.type+':'+e.target.id+':'+e.buttons);document.getElementById('out').textContent=log.join('|')}"
        "for(let type of ['pointerdown','pointermove','pointerup','pointercancel','lostpointercapture','click'])pad.addEventListener(type,note);"
        "pad.onpointerdown=e=>{pad.setPointerCapture(e.pointerId);e.preventDefault()};</script>",true);
    check(script_turn(),"pointer: capture page initializes");
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,10,0,1);
    pointer_move(500,300,1);
    g.bar.up = true;
    g.qpanel.bounds = (os64_gui_rect_t){0,-30,800,30};
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,20,-10,0,0);
    g.bar.up = false;
    check(probe_text_is("out","pointerdown:pad:1|pointermove:pad:1|pointerup:pad:0|lostpointercapture:pad:0|click:pad:0"),
          "pointer: native captured release and click survive release over the question bar");
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,10,0,1);
    g.page.diag=yonder_diag_new("https://fixture.test/pointer",1,0);
    check(g.page.diag!=NULL,"pointer: input-loss diagnostic allocated");
    os64_gui_event_t lost={.type=OS64_GUI_EVENT_INPUT_LOST}; window_event(NULL,&lost);
    check(os64_streq(yonder_diag_fact_value(g.page.diag,"pointer input"),
          "native input queue lost events; active gesture canceled"),
          "pointer: input loss records why an active stroke was canceled");
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,10,0,0);
    check(strstr(probe_text_of("out"),"pointercancel:pad:")!=NULL,
          "pointer: input loss cancels capture through window ingress");
    probe_drop();

    probe_page("<input id=c type=checkbox><button id=b>button</button><p id=out>-</p>"
        "<script>var count=0;document.getElementById('c').onclick=e=>{document.getElementById('out').textContent="
        "'checked:'+e.target.checked+':'+e.constructor.name};"
        "document.getElementById('b').onclick=e=>{document.getElementById('out').textContent='button:'+(++count)};</script>",true);
    script_turn();
    ui_click_on("c");
    check(probe_text_is("out","checked:true:PointerEvent") && probe_field("c")->u.check.checked,
          "pointer: checkbox preactivation and native widget agree");
    ui_click_on("b");
    check(probe_text_is("out","button:1"),"pointer: native button dispatches one click");
    probe_drop();

    probe_page("<!doctype html><style>body{margin:0}#pad{position:absolute;left:0;top:0;width:100px;height:80px}</style>"
        "<div id=pad></div><p id=out>-</p><script>var n=0,p=document.getElementById('pad');"
        "p.onpointerenter=()=>{document.getElementById('out').textContent='entered'};"
        "p.onpointerleave=()=>{document.getElementById('out').textContent='left'};"
        "setTimeout(()=>p.style.left='200px',100);</script>",true);
    script_turn();pointer_move(10,10,0);
    check(probe_text_is("out","entered"),"pointer: native entry");
    now_ms+=200; for (int i=0;i<6;i++) { script_turn(); now_ms+=20; }
    check(probe_text_is("out","left"),"pointer: moving layout under a stationary cursor dispatches leave");
    probe_drop();
    probe_page("<!doctype html><style>body{margin:0}#pad{width:100px;height:80px}</style>"
        "<div id=pad></div><p id=out></p><script>var p=document.getElementById('pad');"
        "p.onpointerdown=e=>{document.getElementById('out').textContent=[e.clientX,e.pageX,e.screenX,e.ctrlKey].join()};</script>",true);
    script_turn(); g.zoom=1500; g.sx=30; g.view.bounds.x=5;
    os64_gui_event_t fractional={.type=OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,
        .mouse={.x=15,.y=15,.button=0,.buttons=1,.modifiers=OS64_GUI_MOD_CTRL}};
    g.running=true;g.root.bounds=(os64_gui_rect_t){0,0,800,600};g.view.cls=&kViewClass;
    window_event(NULL,&fractional);
    check(probe_text_is("out","6.666666666666666,26.666666666666664,75,true"),
          "pointer: native coordinates retain zoom fractions, scroll, window origin and modifiers");
    fractional.type=OS64_GUI_EVENT_MOUSE_BUTTON_UP;fractional.mouse.buttons=0;window_event(NULL,&fractional);
    probe_drop();

    probe_page("<!doctype html><style>body{margin:0}#pad{position:absolute;left:0;top:0;width:100px;height:80px}</style>"
        "<div id=pad></div><p id=out data-hits=0 data-ticks=0></p><script>var p=document.getElementById('pad'),o=document.getElementById('out'),n=0,t=0;"
        "p.onpointerenter=()=>{p.style.left='200px';o.setAttribute('data-hits',++n)};"
        "p.onpointerleave=()=>{p.style.left='0px';o.setAttribute('data-hits',++n)};"
        "setInterval(()=>o.setAttribute('data-ticks',++t),10);</script>",true);
    script_turn();pointer_move(10,10,0);
    for(int i=0;i<6;i++) {
        now_ms+=20;
        int before=atoi(os64_html_attr(probe_id("out"),"data-hits")->value);
        script_turn();
        int after=atoi(os64_html_attr(probe_id("out"),"data-hits")->value);
        check(after-before<=1,"pointer: one self-moving boundary pass per rendering opportunity");
        script_turn();
        check(atoi(os64_html_attr(probe_id("out"),"data-hits")->value)==after,
              "pointer: stationary rehit does not spin at an unchanged clock");
    }
    check(atoi(os64_html_attr(probe_id("out"),"data-ticks")->value)>0,
          "pointer: timers progress beside a self-moving boundary listener");
    probe_drop();

    probe_page("<!doctype html><style>body{margin:0}a{display:block;width:100px;height:80px}</style>"
        "<a id=a href=/before>menu</a><p id=out>-</p><script>var a=document.getElementById('a'),log=[];"
        "a.onpointerdown=e=>{e.preventDefault();a.setPointerCapture(e.pointerId);a.href='/after';"
        "log.push('down');document.getElementById('out').textContent=log.join()};"
        "a.onpointercancel=e=>{log.push('cancel');document.getElementById('out').textContent=log.join()};"
        "a.onauxclick=e=>{document.getElementById('out').textContent='BAD auxclick'};</script>",true);
    script_turn();
    g.menus=(os64_ui_menu_session_t *)(uintptr_t)1;
    for (int i=0;i<CONTEXT_MENUS;i++) g.context_menus[i]=(os64_ui_menu_model_t *)(uintptr_t)1;
    unsigned menus=yonder_host_list.menus;
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,10,OS64_GUI_MOUSE_RIGHT,2);
    check(probe_text_is("out","down,cancel") && yonder_host_list.menus==menus+1,
          "pointer: eligible right press delivers down and cancel before opening the native menu");
    check(strstr(g.context_link,"/before")!=NULL,
          "pointer: menu address is captured before the press listener rewrites the link");
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,10,OS64_GUI_MOUSE_RIGHT,0);
    check(probe_text_is("out","down,cancel"),"pointer: menu release tail cannot emit auxclick");
    g.menus=NULL;
    memset(g.context_menus,0,sizeof(g.context_menus));
    probe_drop();

    probe_page("<input id=c type=checkbox><p id=out>-</p>"
        "<script>document.getElementById('c').onpointerdown=e=>e.target.disabled=true;</script>",true);
    script_turn();ui_click_on("c");
    check(!probe_field("c")->u.check.checked && g.ui.grab==NULL,
          "pointer: a control disabled during pointerdown cannot take the native grab or activate");
    probe_drop();

    probe_page("<!doctype html><style>body{margin:0}#pad{width:100px;height:80px}</style>"
        "<div id=pad></div><script>document.getElementById('pad').onpointerdown=e=>{"
        "e.preventDefault();e.target.setPointerCapture(e.pointerId)};</script>",true);
    script_turn();pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,10,0,1);
    page_departs();probe_drop();
    probe_page("<!doctype html><style>body{margin:0}#pad{width:100px;height:80px}</style>"
        "<div id=pad></div><p id=out>-</p><script>var n=0,p=document.getElementById('pad');"
        "p.onpointerdown=()=>document.getElementById('out').textContent='down';"
        "p.onclick=()=>document.getElementById('out').textContent='click:'+(++n);</script>",true);
    script_turn();
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,10,OS64_GUI_MOUSE_MIDDLE,5);
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,10,OS64_GUI_MOUSE_MIDDLE,1);
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,10,OS64_GUI_MOUSE_LEFT,0);
    check(probe_text_is("out","-"),"pointer: a new document cannot inherit the old drag's chord or release");
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,10,0,1);
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,10,0,0);
    check(probe_text_is("out","click:1"),"pointer: a fresh press works after navigation drains the old gesture");
    probe_drop();

    probe_page("<!doctype html><style>body{margin:0}#pad{width:100px;height:80px}</style>"
        "<div id=pad></div><script>var p=document.getElementById('pad');p.onpointerdown=()=>{"
        "p.style.width='101px';p.offsetWidth};</script>",true);
    script_turn();
    geometry_layout_delay=((int64_t)g.script_ms+1000)*1000;
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,10,10,0,1);
    geometry_layout_delay=0;clock_us=10000000;
    check(!scripts_live() && s_pointer_draining,
          "pointer: a fatal pressed packet retains window-level tail draining after runtime retirement");
    pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,10,10,0,0);
    check(!s_pointer_draining && g.ui.grab==NULL,"pointer: failed-runtime release drains without a native activation");
    probe_drop();

    pointer_frame_cases();

}
