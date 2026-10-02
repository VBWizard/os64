// Scroll containers (PILE3.md § Scrolling boxes): a box whose overflow is
// not visible or clip cuts its content to its padding box, and its content
// is drawn, painted and hit moved back by however far the face scrolled it.

// Whether the tree's dump holds `line` somewhere.
static bool dump_has(const flow_tree_t *t, const char *line)
{
    int64_t need = flow_dump(t, NULL, 0);
    char *text = malloc((size_t)need + 1);
    if (text == NULL)
        return false;
    flow_dump(t, text, (size_t)need + 1);
    bool found = strstr(text, line) != NULL;
    free(text);
    return found;
}

static void scroll_cases(void)
{
    flow_point_t top = {0, 0};
    // A box 100 tall with a 10 border and 5 padding: its padding box is
    // 10..120, and ten 20px paragraphs from 15 reach 215, 220 with the end
    // padding — 100 to scroll.
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0;height:20px}"
                     "#s{height:100px;overflow:auto;border:10px solid;padding:5px}"
                     ".tail{height:1000px}</style><div id=s>"
                     "<p id=p0>0</p><p>1</p><p>2</p><p>3</p><p id=p4>4</p>"
                     "<p>5</p><p>6</p><p>7</p><p id=p8>8</p><p id=p9>9</p></div>"
                     "<p class=tail></p>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const os64_html_node_t *root = q.doc->document;
    const flow_box_t *s = box_with_id(t, root, "s"), *p0 = box_with_id(t, root, "p0");
    const flow_box_t *p4 = box_with_id(t, root, "p4"), *p8 = box_with_id(t, root, "p8");
    const flow_box_t *p9 = box_with_id(t, root, "p9");
    flow_point_t range = flow_scroll_range(t, 0);
    expect("scroll: the box is the tree's one scroll container, with 100 to scroll down",
           t != NULL && flow_nscrollers(t) == 1 && s != NULL && flow_scroller(t, 0) == s &&
               s->scroller == 0 && range.x == 0 && range.y == 100, NULL);
    expect("scroll: its content's container is it, and its own is none",
           p4 != NULL && flow_box_scroller(p4) == 0 && flow_box_scroller(s) == -1, NULL);
    expect("scroll: what it scrolls does not make the page taller",
           t != NULL && flow_height(t) == 130 + 1000, NULL);
    flow_point_t past = flow_scroll_set(t, 0, (flow_point_t){7, 500});
    flow_point_t before = flow_scroll_set(t, 0, (flow_point_t){-5, -5});
    expect("scroll: a position is held to the range",
           past.x == 0 && past.y == 100 && before.x == 0 && before.y == 0, NULL);

    flow_scroll_set(t, 0, (flow_point_t){0, 50});
    os64_gui_rect_t r4 = p4 != NULL ? flow_box_doc_rect(p4, top) : (os64_gui_rect_t){0};
    os64_gui_rect_t c4 = p4 != NULL ? flow_box_doc_clip(p4, top) : (os64_gui_rect_t){0};
    expect("scroll: scrolled 50, the content is drawn 50 up and the clip stays",
           p4 != NULL && p4->rect.y == 95 && r4.y == 45 && p4->clipped && c4.x == 10 &&
               c4.y == 10 && c4.w == 380 && c4.h == 110 &&
               flow_box_doc_rect(s, top).y == 0, NULL);
    const flow_box_t *h = t != NULL ? flow_hit(t, 20, 50, top) : NULL;
    expect("scroll: a point in the padding box hits the content scrolled there",
           inside_of(h, p4), NULL);
    // 125 is the bottom border: unscrolled-and-clipped content (p8 at 175
    // less 50) is not there to be hit, the box's own border is.
    h = t != NULL ? flow_hit(t, 20, 125, top) : NULL;
    expect("scroll: past the padding box, the content is not hit but the box is",
           h == s && !inside_of(h, p8), NULL);

    flow_scroll_set(t, 0, (flow_point_t){0, 100});
    Seen seen = {0};
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 130}, top, seen_add, &seen);
    expect("scroll: scrolled to the end, the first paragraph is not visited and the last is",
           !seen_has(&seen, p0) && seen_has(&seen, p9), NULL);
    expect("scroll: the dump says where it is scrolled to",
           dump_has(t, "scroller 0 range 0 100 at 0 100"), NULL);
    flow_free(t);
    sheeted_close(&q);

    // A box its content is the containing block of moves with the scroll;
    // one whose containing block is outside does not, and is not clipped.
    // A sticky header in a tall wrapper rides the scroll container's top,
    // not the page's. The absolute box at 150..170 is inside what the
    // paragraph already reaches, 10 + 300.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     ".abs{position:absolute;width:20px;height:20px}"
                     "#s{position:relative;height:100px;overflow:auto}"
                     ".st{position:sticky;top:0;height:10px}"
                     "#s2{height:50px;overflow:auto}</style>"
                     "<div id=s><div style='height:310px'><div id=st class=st></div>"
                     "<p style='height:300px'></p></div>"
                     "<div id=ab class=abs style='top:150px'></div></div>"
                     "<div style='position:relative'><div id=s2>"
                     "<div id=esc class=abs style='top:0'></div></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    root = q.doc->document;
    s = box_with_id(t, root, "s");
    const flow_box_t *ab = box_with_id(t, root, "ab"), *st = box_with_id(t, root, "st");
    const flow_box_t *esc = box_with_id(t, root, "esc");
    int32_t s2 = box_with_id(t, root, "s2") != NULL ? box_with_id(t, root, "s2")->scroller : -1;
    flow_scroll_set(t, 0, (flow_point_t){0, 120});
    expect("scroll: its content's absolute box moves with it, and is its to scroll",
           ab != NULL && flow_box_scroller(ab) == 0 &&
               flow_box_doc_rect(ab, top).y == 30 && flow_scroll_range(t, 0).y == 210, NULL);
    expect("scroll: a sticky header rides the scroll container's top",
           st != NULL && flow_box_doc_rect(st, top).y == 0 &&
               flow_box_doc_rect(st, (flow_point_t){0, 40}).y == 0, NULL);
    expect("scroll: an absolute box whose containing block is outside does not move",
           esc != NULL && s2 >= 0 && flow_box_scroller(esc) == -1 &&
               flow_scroll_range(t, s2).y == 0 && !esc->clipped, NULL);
    flow_free(t);
    sheeted_close(&q);

    // A sticky header straight inside a scroll container sticks for all of
    // what the container scrolls through — its containing block's content
    // reaches that far — not one screenful of it.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     "#s{height:100px;overflow:auto} .st{position:sticky;top:0;height:10px}"
                     "</style><div id=s><div id=st class=st></div>"
                     "<p style='height:500px'></p></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    st = box_with_id(t, q.doc->document, "st");
    flow_scroll_set(t, 0, (flow_point_t){0, 300});
    expect("scroll: a sticky child of the container sticks the whole way down",
           st != NULL && flow_scroll_range(t, 0).y == 410 &&
               flow_box_doc_rect(st, top).y == 0, NULL);
    flow_scroll_set(t, 0, (flow_point_t){0, 410});
    expect("scroll: and at the very end still",
           st != NULL && flow_box_doc_rect(st, top).y == 0, NULL);
    flow_free(t);
    sheeted_close(&q);

    // A control in a box: covered while it is scrolled out of the padding
    // box, as a face that draws live widgets asks (POSITION.md, ruling 9),
    // and reachable once scrolled into it.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     "#s{height:100px;overflow:auto} .tall{height:1000px}</style>"
                     "<div id=s><p style='height:200px'></p><input name=q></div>"
                     "<p class=tall></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *field = t != NULL ? flow_control(t, 0) : NULL;
    bool hidden_below = field != NULL && flow_box_covered(t, field, top);
    flow_scroll_set(t, 0, (flow_point_t){0, 150});
    expect("scroll: a control below its box's edge is covered, and scrolled in is not",
           hidden_below && field != NULL && !flow_box_covered(t, field, top), NULL);
    flow_free(t);
    sheeted_close(&q);

    // The container an inline-block makes is under the atom on the line,
    // which is the element's first box: a face that keeps a position by
    // element finds the container by flow_scroller_for (Quinn, #201).
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     "#ib{display:inline-block;width:100px;height:100px;overflow:auto}"
                     "</style><span id=ib><div style='height:300px'></div></span>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const os64_html_node_t *ibn = NULL;
    const flow_box_t *ib = box_with_id(t, q.doc->document, "ib");
    if (ib != NULL)
        ibn = ib->node;
    expect("scroll: an inline-block's first box is its atom, and flow_scroller_for finds it",
           ib != NULL && ib->kind == FLOW_BOX_ATOMIC && ib->scroller < 0 &&
               flow_scroller_for(t, ibn) == 0 && flow_scroller(t, 0)->node == ibn &&
               flow_scroller_for(t, q.doc->document) == -1, NULL);
    flow_free(t);
    sheeted_close(&q);

    // Positioned content is what can be seen of it: behind a sticky box it
    // still reaches its container (Firefox: 120), and cut by an
    // overflow: clip wrapper it reaches only as far as the wrapper (0).
    const char *reach[] = {
        "<div id=s><div style='position:sticky;height:20px'>"
        "<div class=abs></div></div></div>",
        "<div id=s><div style='position:relative;height:50px;overflow:clip'>"
        "<div class=abs></div></div></div>",
    };
    const int32_t reach_y[] = {120, 0};
    for (size_t k = 0; k < sizeof(reach) / sizeof(reach[0]); k++) {
        char html[512];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body{margin:0} #s{height:100px;overflow:auto}"
                 ".abs{position:absolute;top:200px;height:20px;width:20px}</style>%s",
                 reach[k]);
        sheeted_open(&q, html, 400, 600);
        t = flow_layout(q.doc, q.page, 400, &q.env);
        int32_t s = flow_scroller_for(t, box_with_id(t, q.doc->document, "s") != NULL
                                              ? box_with_id(t, q.doc->document, "s")->node
                                              : NULL);
        expect(k == 0 ? "scroll: a positioned box behind a sticky one reaches its container"
                      : "scroll: a positioned box an overflow: clip cuts reaches no further",
               s == 0 && flow_scroll_range(t, s).y == reach_y[k], NULL);
        flow_free(t);
        sheeted_close(&q);
    }

    // Revealing a target: its top at the box's top, and across as little
    // as shows it — a 20px target at 200 in a 100px view scrolls 120
    // (Firefox, Quinn's #201 probe); one already in view across stays.
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     "#s{position:relative;width:100px;height:100px;overflow:auto}"
                     ".t{position:absolute;width:20px;height:20px}</style>"
                     "<div id=s><div id=far class=t style='left:200px;top:150px'></div>"
                     "<div id=near class=t style='left:40px;top:30px'></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *far = box_with_id(t, q.doc->document, "far");
    const flow_box_t *near = box_with_id(t, q.doc->document, "near");
    flow_scroll_reveal(t, far);
    flow_point_t at_far = flow_scroll_at(t, 0);
    flow_scroll_reveal(t, near);
    flow_point_t at_near = flow_scroll_at(t, 0);
    expect("scroll: a revealed target is shown on both axes",
           far != NULL && near != NULL && at_far.x == 120 && at_far.y == 70 &&
               at_near.x == 40 && at_near.y == 30, NULL);
    flow_free(t);
    sheeted_close(&q);

    // Nested: the inner box's content moves by both.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     ".s{overflow:auto} #o{height:100px} #i{height:50px}</style>"
                     "<div id=o class=s><p style='height:40px'></p><div id=i class=s>"
                     "<p style='height:20px'></p><p id=x style='height:200px'></p></div>"
                     "<p style='height:200px'></p></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *in = box_with_id(t, q.doc->document, "i");
    const flow_box_t *x = box_with_id(t, q.doc->document, "x");
    int32_t ii = in != NULL ? in->scroller : -1;
    flow_scroll_set(t, 0, (flow_point_t){0, 30});
    flow_scroll_set(t, ii, (flow_point_t){0, 15});
    os64_gui_rect_t cx = x != NULL ? flow_box_doc_clip(x, top) : (os64_gui_rect_t){0};
    expect("scroll: nested, the inner content moves by both, and both clip it",
           x != NULL && ii == 1 && flow_box_scroller(x) == 1 && flow_box_scroller(in) == 0 &&
               x->rect.y == 60 && flow_box_doc_rect(x, top).y == 15 && cx.y == 10 &&
               cx.h == 50, NULL);
    flow_free(t);
    sheeted_close(&q);

    // A fixed box that scrolls: its content moves with the page's scroll
    // and back by its own.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     "#f{position:fixed;top:0;left:0;width:100px;height:50px;overflow:auto}"
                     ".tall{height:2000px}</style><div id=f><p id=y style='height:200px'></p>"
                     "</div><p class=tall></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *y = box_with_id(t, q.doc->document, "y");
    flow_scroll_set(t, 0, (flow_point_t){0, 20});
    expect("scroll: in a fixed box, the content moves with the page and back by the box",
           y != NULL && flow_box_doc_rect(y, (flow_point_t){0, 500}).y == 480, NULL);
    flow_free(t);
    sheeted_close(&q);

    // The viewport's own overflow: html scrolls, or the body does while
    // html is visible. Neither is a scroll container, and the page is as
    // tall as its content.
    const char *viewport[] = {
        "<!doctype html><style>html{overflow-y:scroll} body{margin:0}</style>"
        "<p style='height:900px;margin:0'></p>",
        "<!doctype html><style>body{margin:0;height:100px;overflow:auto}</style>"
        "<p style='height:900px;margin:0'></p>",
    };
    for (size_t k = 0; k < sizeof(viewport) / sizeof(viewport[0]); k++) {
        sheeted_open(&q, viewport[k], 400, 600);
        t = flow_layout(q.doc, q.page, 400, &q.env);
        expect("scroll: the viewport's overflow makes no scroll container, and cuts nothing",
               t != NULL && flow_nscrollers(t) == 0 && flow_height(t) == 900, NULL);
        flow_free(t);
        sheeted_close(&q);
    }
}
