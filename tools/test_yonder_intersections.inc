/* INTERSECTION OBSERVATIONS ON REAL LAYOUT (INTERSECTION_OBSERVER.md § 8):
 * yonder's batch (geometry.c's yonder_intersections) answered from the
 * page's own boxes, and the step where the window loop takes it. Each page
 * writes what its observers saw into #out, which is fixed at the top so
 * it moves nothing it is written about. */

/* The window loop's turns for scripts and observations: a task (the
 * delivery among them), then the step, then the frame's end; until a turn
 * runs nothing and queues nothing. */
static bool io_turns(void) {
    for(int i=0;i<64;i++) {
        bool ran=script_turn();
        bool queued=intersections_turn();
        motion_painted();
        if(!ran && !queued) return true;
    }
    return false;
}

#define IO_OUT "<p id=out style='position:fixed;top:0;right:0;margin:0'>-</p>"
#define IO_LOG "<script>var log=[];function said(s){log.push(s);document.getElementById('out').textContent=log.join(',')}</script>"

/* A target below the fold: not intersecting at first, intersecting when the
 * view is scrolled to it, wholly or half, and not again when it leaves. */
static void io_scroll_into_view(void) {
    loop_page("<style>body{margin:0}#t{height:100px}</style>" IO_OUT IO_LOG
        "<div style='height:3000px'></div><div id=t></div><div style='height:3000px'></div><script>"
        "new IntersectionObserver(function(es){es.forEach(function(e){said(e.isIntersecting+':'+"
        "Math.round(e.intersectionRatio*100)+':'+Math.round(e.boundingClientRect.top))})},{threshold:[0,0.5,1]})"
        ".observe(document.getElementById('t'));</script>");
    check(loop_settle() && g.page.tree!=NULL,"io: the page arrives");
    check(io_turns() && probe_text_is("out","false:0:3000"),"io: the first observation, below the fold");
    int32_t h=g.view.bounds.h;
    scroll_to(0,3100-h);
    char want[128];
    snprintf(want,sizeof(want),"false:0:3000,true:100:%d",h-100);
    check(io_turns() && probe_text_is("out",want),"io: scrolled to, it is wholly in view");
    scroll_to(0,3050-h);
    snprintf(want,sizeof(want),"false:0:3000,true:100:%d,true:50:%d",h-100,h-50);
    check(io_turns() && probe_text_is("out",want),"io: half out, the 0.5 threshold");
    scroll_to(0,3051-h);
    check(io_turns() && probe_text_is("out",want),"io: moving inside one threshold band says nothing");
    scroll_to(0,0);
    snprintf(want,sizeof(want),"false:0:3000,true:100:%d,true:50:%d,false:0:3000",h-100,h-50);
    check(io_turns() && probe_text_is("out",want),"io: leaving the view");
    loop_drop();
}

/* A scroll container as the root: its padding box is the root's
 * rectangle, and its scroll brings the target in. A second observer on the
 * viewport sees the same target clipped by the container. */
static void io_container_root(void) {
    loop_page("<style>body{margin:0}#box{height:200px;width:300px;overflow:auto;border:5px solid}"
        "#gap{height:500px}#item{height:50px}</style>" IO_OUT IO_LOG
        "<div id=box><div id=gap></div><div id=item></div></div><script>"
        "var box=document.getElementById('box'),item=document.getElementById('item');"
        "new IntersectionObserver(function(es){var e=es[0];said('root:'+e.isIntersecting+':'+"
        "Math.round(e.intersectionRatio*100)+':'+Math.round(e.rootBounds.top)+'-'+Math.round(e.rootBounds.bottom))},"
        "{root:box,threshold:[0,1]}).observe(item);"
        "new IntersectionObserver(function(es){said('view:'+es[0].isIntersecting)}).observe(item);</script>");
    check(loop_settle() && io_turns() && probe_text_is("out","root:false:0:5-205,view:false"),
          "io: an element root's rectangle is its padding box, and the container clips for the viewport too");
    check(wheel_box(g.view.bounds.x+20,g.view.bounds.y+20,0,350),"io: the container scrolls");
    check(io_turns() && probe_text_is("out","root:false:0:5-205,view:false,root:true:100:5-205,view:true"),
          "io: scrolled into the container, it is in both");
    loop_drop();
}

/* The chain follows containing blocks: an absolute box escapes a static
 * parent's clip and is not on that parent's chain; an in-flow child is
 * clipped by it. */
static void io_containing_blocks(void) {
    loop_page("<style>body{margin:0}#clip{overflow:hidden;height:50px;width:50px}#in{height:20px;margin-top:100px}"
        "#abs{position:absolute;top:200px;left:0;width:10px;height:10px}</style>" IO_OUT IO_LOG
        "<div id=clip><div id=in></div><div id=abs></div></div><script>"
        "var o=new IntersectionObserver(function(es){es.forEach(function(e){said(e.target.id+':'+e.isIntersecting)})});"
        "o.observe(document.getElementById('in'));o.observe(document.getElementById('abs'));"
        "var r=new IntersectionObserver(function(es){es.forEach(function(e){said('r'+e.target.id+':'+e.isIntersecting)})},"
        "{root:document.getElementById('clip'),rootMargin:'1000px'});"
        "r.observe(document.getElementById('in'));r.observe(document.getElementById('abs'));</script>");
    check(loop_settle() && io_turns() && probe_text_is("out","in:false,abs:true,rin:true,rabs:false"),
          "io: the clip cuts its in-flow child, and an absolute box escapes it and its chain");
    loop_drop();
    /* A scaled root's padding box is inset by its painted border, 2 x 10. */
    loop_page("<style>body{margin:0}#r{overflow:hidden;width:100px;height:100px;border:10px solid;"
        "transform:scale(2);transform-origin:0 0}#t{height:10px}</style>" IO_OUT IO_LOG
        "<div id=r><div id=t></div></div><script>"
        "new IntersectionObserver(function(es){var b=es[0].rootBounds;said([b.left,b.top,b.right,b.bottom].join(' '))},"
        "{root:document.getElementById('r')}).observe(document.getElementById('t'));</script>");
    check(loop_settle() && io_turns() && probe_text_is("out","20 20 220 220"),
          "io: a transformed root's rectangle is its padding box where it is painted");
    loop_drop();
}

/* The step runs after the animation frame callbacks, on the layout they
 * asked for, and the callback after the frame; display none is no box. */
static void io_after_frames(void) {
    loop_page("<style>body{margin:0}#t{position:absolute;top:5000px;left:0;width:10px;height:10px}</style>"
        IO_OUT IO_LOG "<div id=t></div><script>"
        "var t=document.getElementById('t');"
        "new IntersectionObserver(function(es){said('io:'+es[es.length-1].isIntersecting)}).observe(t);"
        "setTimeout(function(){requestAnimationFrame(function(){t.style.top='0px';said('raf')})},10);"
        "setTimeout(function(){t.style.display='none'},100);</script>");
    check(loop_settle() && io_turns() && probe_text_is("out","io:false"),"io: first, off the page's view");
    now_ms+=10;
    check(io_turns(),"io: the timer asks for a frame");
    now_ms+=20;
    check(io_turns() && probe_text_is("out","io:false,raf,io:true"),
          "io: the frame moved it, and the step saw the frame's layout");
    now_ms+=100;
    check(io_turns() && probe_text_is("out","io:false,raf,io:true,io:false"),"io: display none is no box");
    loop_drop();
}

/* What infinite lists are made of: a sentinel after the list that grows
 * it when it shows. */
static void io_infinite_list(void) {
    loop_page("<style>body{margin:0}.i{height:300px}</style>" IO_OUT
        "<div id=list></div><div id=end style='height:1px'></div><script>"
        "var list=document.getElementById('list'),n=0;function more(){for(var k=0;k<10;k++){"
        "var d=document.createElement('div');d.className='i';d.textContent=++n;list.appendChild(d)}"
        "document.getElementById('out').textContent=n}more();"
        "new IntersectionObserver(function(es){if(es[es.length-1].isIntersecting)more()})"
        ".observe(document.getElementById('end'));</script>");
    check(loop_settle() && io_turns() && probe_text_is("out","10"),"io: ten items, the sentinel out of view");
    scroll_to(0,3000);
    check(io_turns() && probe_text_is("out","20"),"io: at the end, ten more");
    scroll_to(0,6000);
    check(io_turns() && probe_text_is("out","30"),"io: and ten more again");
    check(io_turns() && probe_text_is("out","30"),"io: and no more while the view rests");
    loop_drop();
}

/* With the window covered there is no rendering opportunity: the step
 * waits, and runs once it is uncovered. */
static void io_covered(void) {
    loop_page("<style>body{margin:0}</style>" IO_OUT IO_LOG "<div id=t style='height:10px'></div><script>"
        "new IntersectionObserver(function(es){said(String(es[0].isIntersecting))})"
        ".observe(document.getElementById('t'));</script>");
    check(loop_settle(),"io: covered page arrives");
    g.covered=true;
    check(io_turns() && probe_text_is("out","-"),"io: covered, nothing is observed");
    g.covered=false;
    check(io_turns() && probe_text_is("out","true"),"io: uncovered, the first observation");
    loop_drop();
}

/* CSSOM View § 7 on real layout: a script scrolls a box and the page, the
 * box's scroll reads back held to its range with its scrolling area, the
 * page's is the view's, and an observer of the box's item sees it come in
 * (what WPT's observers do to move their roots). */
static void io_element_scroll(void) {
    loop_page("<style>body{margin:0}#box{height:200px;width:300px;overflow:auto}#gap{height:500px}"
        "#item{height:50px}</style>" IO_OUT IO_LOG
        "<div id=box><div id=gap></div><div id=item></div></div><div style='height:5000px'></div><script>"
        "var box=document.getElementById('box');"
        "new IntersectionObserver(function(es){said('io:'+es[0].isIntersecting)},{root:box})"
        ".observe(document.getElementById('item'));"
        "setTimeout(function(){box.scrollTop=1e6;document.documentElement.scrollTop=1000;"
        "said([box.scrollTop,box.scrollHeight,box.clientHeight,scrollY,document.documentElement.scrollHeight>5000].join(' '))},10);"
        "</script>");
    check(loop_settle() && io_turns() && probe_text_is("out","io:false"),"scroll: the item starts out of its box");
    now_ms+=20;
    check(io_turns() && probe_text_is("out","io:false,350 550 200 1000 true,io:true"),
          "scroll: a script's scrollTop scrolls the box to its range, the root's scrolls the page, "
          "and the observer sees the item come in");
    check(g.sy==1000,"scroll: the page moved");
    loop_drop();
    /* The getters read the scroller the setters move: a body that scrolls
     * in standards mode (the app-shell layout), and an inline-block's
     * container rather than its atom; scrollBy adds up on both. */
    loop_page("<style>html{overflow:hidden}body{margin:0;overflow:auto;height:300px}"
        "#ib{display:inline-block;overflow:auto;width:100px;height:50px}</style>" IO_OUT IO_LOG
        "<span id=ib><div style='height:300px'></div></span><div style='height:4000px'></div><script>"
        "setTimeout(function(){var b=document.body,ib=document.getElementById('ib');"
        "b.scrollTop=500;b.scrollBy(0,100);b.scrollBy(0,100);ib.scrollTop=30;ib.scrollBy(0,10);"
        "said([b.scrollTop,ib.scrollTop,ib.scrollHeight,ib.clientHeight].join(' '))},10);</script>");
    check(loop_settle() && io_turns(),"scroll: an app shell arrives");
    now_ms+=20;
    bool shell=io_turns() && probe_text_is("out","700 40 300 50");
    if(!shell) fprintf(stderr,"app shell said: %s\n",probe_text_of("out"));
    check(shell,"scroll: a scrolling body and an inline-block read back what was set, and scrollBy adds");
    loop_drop();
    /* An inline-block's and an inline-flex's own border is not overflow,
     * and the root in quirks mode, which scrolls nothing, has an area. */
    loop_page("<style>body{margin:0}.a{width:100px;height:50px;border:10px solid;padding:0}</style>" IO_OUT IO_LOG
        "<span id=ib class=a style='display:inline-block'></span><span id=if class=a style='display:inline-flex'>"
        "</span><div style='height:3000px'></div><script>setTimeout(function(){var ib=document.getElementById('ib'),"
        "f=document.getElementById('if');said([ib.scrollWidth,ib.scrollHeight,f.scrollWidth,f.scrollHeight].join(' '))"
        "},10)</script>");
    check(loop_settle() && io_turns(),"scroll: bordered atoms arrive");
    now_ms+=20;
    bool atoms=io_turns() && probe_text_is("out","100 50 100 50");
    if(!atoms) fprintf(stderr,"atoms said: %s\n",probe_text_of("out"));
    check(atoms,"scroll: an inline-block's and an inline-flex's border is not their overflow");
    os64_html_document_t *doc=g.page.way.doc;
    os64_html_quirks_t mode=doc->quirks;
    doc->quirks=OS64_HTML_QUIRKS;
    os64_dom_geometry_t root;
    bool measured=yonder_geometry_snapshot(doc,g.page.tree,doc->html,800,600,1000,(flow_point_t){0,0},&root);
    doc->quirks=mode;
    check(measured && root.scroll_height>=3000 && root.scroll_top==0,
          "scroll: the root in quirks mode scrolls nothing and has its box's area");
    loop_drop();
    /* A script's scroll replaces an older one a later layout would bring
     * back: scrolled down, emptied (the layout holds it at 0), set to 0,
     * refilled: it stays at the top. */
    loop_page("<style>body{margin:0}#l{height:100px;overflow:auto}.r{height:50px}</style>" IO_OUT IO_LOG
        "<div id=l></div><script>var l=document.getElementById('l');"
        "function fill(){for(var i=0;i<40;i++){var d=document.createElement('div');d.className='r';l.appendChild(d)}}"
        "fill();setTimeout(function(){l.scrollTop=1000;said(l.scrollTop)},10);"
        "setTimeout(function(){l.replaceChildren()},20);"
        "setTimeout(function(){l.scrollTop=0;fill()},30);"
        "setTimeout(function(){said(l.scrollTop)},40);</script>");
    check(loop_settle() && io_turns(),"scroll: a list arrives");
    for(int i=0;i<4;i++){ now_ms+=10; check(io_turns(),"scroll: the list's timers"); }
    check(probe_text_is("out","1000,0"),"scroll: reset to the top and refilled, it stays at the top");
    loop_drop();
}

static void io_cases(void) {
    io_element_scroll();
    io_scroll_into_view();
    io_container_root();
    io_containing_blocks();
    io_after_frames();
    io_infinite_list();
    io_covered();
}
#undef IO_OUT
#undef IO_LOG
