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

static void io_cases(void) {
    io_scroll_into_view();
    io_container_root();
    io_containing_blocks();
    io_after_frames();
    io_infinite_list();
    io_covered();
}
#undef IO_OUT
#undef IO_LOG
