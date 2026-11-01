/* Real script, layout and window painter: pixel-only changes reuse the tree. */
static void canvas_window_cases(void)
{
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
