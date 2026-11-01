/* Real script, layout and window painter: pixel-only changes reuse the tree. */
static void canvas_partial_repaint(uint32_t *pixels)
{
    os64_draw_ctx_t partial={0};
    partial.surf.width=partial.surf.pitch_px=800;partial.surf.height=600;partial.surf.pixels=pixels;
    view_paint(&g.view,&partial,NULL);
    yonder_paint_cache_free(g.paint_cache);g.paint_cache=NULL;
    yonder_scratch_free(g.paint_scratch);g.paint_scratch=NULL;
}
static void canvas_damage_cases(void)
{
    const char *styles[]={"","object-fit:contain;object-position:20% 70%", "transform:rotate(8deg)","opacity:0.6"};
    for(unsigned i=0;i<4;++i) {
        char html[1200];snprintf(html,sizeof(html),
            "<!doctype html><style>body{margin:0;background:white}canvas{display:block;width:300px;height:200px;"
            "border:2px solid black;%s}#cover{position:absolute;left:65px;top:35px;width:20px;height:20px;background:blue}</style>"
            "<canvas id=c width=200 height=100></canvas><div id=cover></div>"
            "<script>var x=document.getElementById('c').getContext('2d');x.fillStyle='red';x.fillRect(0,0,200,100);</script>"
            "<script>x.clearRect(40,30,10,10);</script>",styles[i]);
        probe_page(html,true);script_turn();g.sy=30;
        uint32_t *before=stand_in_shot();g.ui.dirty=(os64_gui_rect_t){0};script_turn();
        check(g.ui.dirty.w>0 && (i<2?g.ui.dirty.w<100:g.ui.dirty.w>=800),
              "Canvas: scrolled/object-fit damage stays small; effects keep the viewport fallback");
        canvas_partial_repaint(before);uint32_t *whole=stand_in_shot();
        check(!memcmp(before,whole,800u*600u*sizeof(uint32_t)),
              "Canvas: partial repaint matches full with scroll, object-fit, overlap and effects");
        os64_free(before);os64_free(whole);probe_drop();
    }
}
static void canvas_window_cases(void)
{
    canvas_damage_cases();
    probe_page("<!doctype html><style>body{margin:0;background:white}canvas{display:block;width:400px;height:200px;border:2px solid black}</style>"
        "<canvas id=c width=200 height=100></canvas><canvas id=b width=200 height=100></canvas>"
        "<script>var c=document.getElementById('c'),x=c.getContext('2d'),b=document.getElementById('b').getContext('2d');"
        "x.fillRect(0,0,200,100);b.fillRect(0,0,200,100);</script>"
        "<script>x.clearRect(50,30,4,4);b.clearRect(60,40,4,4);</script>",true);
    script_turn();uint32_t *initial=stand_in_shot();
    g.ui.dirty=(os64_gui_rect_t){0};script_turn();
    check(g.ui.dirty.w>0 && g.ui.dirty.w<100 && g.ui.dirty.h<300 &&
          g.ui.dirty.y<70 && g.ui.dirty.y+g.ui.dirty.h>280,
          "Canvas: damage maps scaled bitmap pixels and borders for both dirty canvases");
    canvas_partial_repaint(initial);
    uint32_t *updated=stand_in_shot();
    check(!memcmp(initial,updated,800u*600u*sizeof(uint32_t)),
          "Canvas: painting only mapped damage matches repainting the whole view");
    os64_free(initial);
    check((updated[64*800+104]&0xffffff)==0xffffff &&
          (updated[288*800+124]&0xffffff)==0xffffff,
          "Canvas: scaled partial clears reach both canvas presentations");
    os64_free(updated);probe_drop();

    probe_page("<!doctype html><style>body{margin:0;background:white}canvas{display:block;width:80px;height:80px}"
        "#cover{position:absolute;left:30px;top:30px;width:20px;height:20px;background:blue}</style>"
        "<canvas id=c width=4 height=4></canvas><div id=cover></div>"
        "<script>var c=document.getElementById('c'),ctx=c.getContext('2d');"
        "ctx.fillStyle='red';ctx.fillRect(0,0,4,4);</script>"
        "<script>ctx.clearRect(0,0,4,4);ctx.fillStyle='lime';ctx.fillRect(0,0,4,4);</script>"
        "<script>c.style.width='160px';ctx.fillStyle='red';ctx.fillRect(0,0,4,4);</script>",true);
    script_turn();
    uint32_t *px=stand_in_shot();
    check((px[10*800+10]&0xffffff)==0xff0000 && (px[40*800+40]&0xffffff)==0xff,
          "Canvas: retained red pixels paint below the overlapping blue element");
    os64_free(px);
    flow_tree_t *tree=g.page.tree;
    uint64_t version=os64_html_version(page_doc(&g.page));
    g.ui.dirty=(os64_gui_rect_t){0};
    script_turn();
    check(tree==g.page.tree && version==os64_html_version(page_doc(&g.page)) && g.ui.dirty.w>0,
          "Canvas: pixel-only script dirties paint without replacing layout or DOM version");
    px=stand_in_shot();
    check((px[10*800+10]&0xffffff)==0x00ff00 && (px[40*800+40]&0xffffff)==0xff,
          "Canvas: updated pixels preserve overlapping content");
    os64_free(px);
    script_turn(); px=stand_in_shot();
    check((px[10*800+120]&0xffffff)==0xff0000 && (px[40*800+40]&0xffffff)==0xff,
          "Canvas: simultaneous CSS size and pixel changes paint using fresh geometry");
    os64_free(px); probe_drop();

    probe_page("<!doctype html><style>body{margin:0;background:white}canvas{display:block}</style>"
        "<canvas id=c width=40 height=40></canvas><script>var c=document.getElementById('c');"
        "var ctx=c.getContext('2d');ctx.fillStyle='rgba(255,0,0,0.5)';ctx.fillRect(0,0,40,40);</script>"
        "<script>c.width=c.width;</script>",true);
    script_turn();px=stand_in_shot();
    check((px[20*800+20]&0xffffff)==0xff7f7f,"Canvas: premultiplied half-red blends once over white");
    os64_free(px);tree=g.page.tree;
    g.sy=20;g.ui.dirty=(os64_gui_rect_t){0};
    script_turn();
    check(g.ui.dirty.x<=g.view.bounds.x && g.ui.dirty.y<=g.view.bounds.y &&
          g.ui.dirty.x+g.ui.dirty.w>=g.view.bounds.x+g.view.bounds.w &&
          g.ui.dirty.y+g.ui.dirty.h>=g.view.bounds.y+g.view.bounds.h,
          "Canvas: scrolled damage covers the viewport in screen coordinates");
    px=stand_in_shot();
    check(tree==g.page.tree && (px[20*800+20]&0xffffff)==0xffffff,
          "Canvas: same-value dimension reset clears visible pixels without layout");
    os64_free(px);probe_drop();

    probe_page("<!doctype html><canvas id=c width=4 height=4></canvas><p id=result></p>"
        "<script>var c=document.getElementById('c'),x=c.getContext('2d');"
        "x.fillRect(0,0,4,4);var observed=0;"
        "new MutationObserver(function(records){observed+=records.length;"
        "document.getElementById('result').textContent=observed+':'+records[0].oldValue+':'"
        "+x.getImageData(0,0,1,1).data[3];}).observe(c,{attributes:true,attributeOldValue:true});</script>"
        "<script>c.setAttribute('width','4');</script>",true);
    script_turn();script_turn();
    check(probe_text_is("result","1:4:0"),
          "Canvas and MutationObserver both receive a same-value dimension assignment");
    probe_drop();
}
