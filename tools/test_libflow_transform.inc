// tools/test_libflow_transform.inc — transforms, the model and the
// translations (PILE3.md § Transforms, slice S7a), included by
// test_libflow_host.c. Every expected geometry is worked by hand from CSS
// Transforms 1 and 2 and the design, never captured from the engine
// (LAYOUT.md § Proof). Sized boxes, body margin 0, a 400 by 600 viewport.
//
// A box is drawn moved by `by`, where its matrix sends its transform-origin
// — the whole of a translation; for any other matrix the box upright, where
// the product of its functions sends the point it is turned about, which is
// that product's own translation whatever the origin is.

#define XFORM_STYLE "<!doctype html><style>body{margin:0} p{margin:0}"

static void transform_layout_cases(void)
{
    // A translation's percentages are of the box's own border box: 50% of
    // 100 across, -25% of 40 down. The box keeps its place in the flow —
    // the next one starts at 40 — and is stacked, as a transformed box is
    // a stacking context. Drawn from -10 to 30, it reaches nothing the
    // flow does not.
    cascade_layout_case("transform: a translation's percentages, the layout unmoved",
        XFORM_STYLE ".t{width:100px;height:40px;transform:translate(50%,-25%)}"
        ".n{height:10px}</style><div class=t></div><div class=n></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 0 400 50\n"
        "    block div 0 0 100 40 stacked transform 50 -10\n"
        "    block div 0 40 400 10\n");

    // Transforms 2 § 6: translate, then rotate, then scale, then the list.
    // T(10, 0) R(90deg) T(20, 0) sends the origin 10 across and, the 20
    // turned a quarter clockwise, 20 down. Taken the list first, it would
    // be 30 across. The box is drawn to 60, and the page reaches it.
    cascade_layout_case("transform: the individual properties before the list",
        XFORM_STYLE ".t{width:100px;height:40px;transform-origin:0 0;translate:10px;"
        "rotate:90deg;transform:translateX(20px)}</style><div class=t></div>", 400,
        "page 400 60\n"
        "block html 0 0 400 40\n"
        "  block body 0 0 400 40\n"
        "    block div 0 0 100 40 stacked transform 10 20 upright\n");

    // The centring every dialog does: left and top 50% of the 400 by 600
    // viewport put the corner at 200, 300; translate(-50%, -50%) of its own
    // 100 by 60 takes it back by half. The page reaches where it is drawn,
    // 330 down — not where it was laid out, 360.
    cascade_layout_case("transform: translate(-50%, -50%) centres an absolute box",
        XFORM_STYLE ".c{position:absolute;left:50%;top:50%;width:100px;height:60px;"
        "transform:translate(-50%,-50%)}</style><div class=c></div>", 400,
        "page 400 330\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 200 300 100 60 positioned transform -50 -30\n");

    // What does more than translate is drawn upright until slice S7b, moved
    // by the product's translation: a scale or a turn alone, none; a
    // translation after a scale of 2, twice its 10, down as well as across;
    // after a quarter turn, turned from across to down. matrix()'s
    // translation is in CSS pixels, and with nothing else it is exact.
    // The last two are drawn to 70 and 80.
    cascade_layout_case("transform: a scale and a turn upright, a translation after them moved",
        XFORM_STYLE "div{width:100px;height:10px} .a{transform:scale(2)}"
        ".b{transform:scale(2) translate(10px)} .c{transform:rotate(90deg) translateX(10px)}"
        ".d{transform:rotate(45deg)} .e{transform:scale(2) translateY(10px)}"
        ".m{transform:matrix(1,0,0,1,10,20)}</style>"
        "<div class=a></div><div class=b></div><div class=c></div><div class=d></div>"
        "<div class=e></div><div class=m></div>", 400,
        "page 400 80\n"
        "block html 0 0 400 60\n"
        "  block body 0 0 400 60\n"
        "    block div 0 0 100 10 stacked transform 0 0 upright\n"
        "    block div 0 10 100 10 stacked transform 20 0 upright\n"
        "    block div 0 20 100 10 stacked transform 0 10 upright\n"
        "    block div 0 30 100 10 stacked transform 0 0 upright\n"
        "    block div 0 40 100 10 stacked transform 0 20 upright\n"
        "    block div 0 50 100 10 stacked transform 10 20\n");

    // A transformed box is the containing block of an absolute box in it
    // and of a FIXED one (Transforms 1 § 3): both placed against its
    // padding box at 0, 50, and neither fixed to the viewport. The box is
    // 200 across, so drawn 10 to the right it reaches 210.
    cascade_layout_case("transform: the containing block of an absolute and a fixed box",
        XFORM_STYLE ".sp{height:50px} .t{width:200px;height:100px;transform:translateX(10px)}"
        ".a{position:absolute;top:5px;left:5px;width:10px;height:10px}"
        ".f{position:fixed;top:7px;left:7px;width:10px;height:10px}</style>"
        "<div class=sp></div><div class=t><div class=a></div><div class=f></div></div>", 400,
        "page 400 150\n"
        "block html 0 0 400 150\n"
        "  block body 0 0 400 150\n"
        "    block div 0 0 400 50\n"
        "    block div 0 50 200 100 stacked transform 10 0\n"
        "      line 0 50 200 0 base 14\n"
        "      block div 5 55 10 10 positioned\n"
        "      block div 7 57 10 10 positioned\n");

    // The page's extent reaches a box where it is drawn: 350 + 100 across,
    // 600 + 100 down, though the flow ends at 100.
    cascade_layout_case("transform: the page reaches a translated box",
        XFORM_STYLE ".t{width:100px;height:100px;transform:translate(350px,600px)}</style>"
        "<div class=t></div>", 400,
        "page 450 700\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 100 100 stacked transform 350 600\n");

    // And not where it was laid out: 100 tall in a body of 50, drawn 50 up,
    // it ends where the body does.
    cascade_layout_case("transform: the page does not reach where a box was laid out",
        XFORM_STYLE "body{height:50px} .t{height:100px;transform:translateY(-50px)}</style>"
        "<div class=t></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 0 400 50\n"
        "    block div 0 0 400 100 stacked transform 0 -50\n");

    // So does a scroll container's range: its content, 50 tall, drawn 100
    // down, ends at 150, 50 past the 100 of its padding box. What clips
    // the content is the container's padding box.
    cascade_layout_case("transform: a scroll container reaches a translated box",
        XFORM_STYLE ".s{height:100px;overflow:auto} .k{height:50px;transform:translateY(100px)}"
        "</style><div class=s><div class=k></div></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100 scroller 0 range 0 50\n"
        "      block div 0 0 400 50 stacked transform 0 100 clip 0 0 400 100\n");

    // Nor a scroll container's: 150 tall, drawn 100 up, it ends at 50,
    // inside the padding box, and there is nothing to scroll to.
    cascade_layout_case("transform: a scroll container does not reach where a box was laid out",
        XFORM_STYLE ".s{height:100px;overflow:auto} .k{height:150px;transform:translateY(-100px)}"
        "</style><div class=s><div class=k></div></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100 scroller 0 range 0 0\n"
        "      block div 0 0 400 150 stacked transform 0 -100 clip 0 0 400 100\n");

    // Sticky and transformed: the transform frame inside the sticky one.
    // A block that fills the body it is in has nowhere to stick to.
    cascade_layout_case("transform: a sticky box transformed",
        XFORM_STYLE ".st{position:sticky;top:0;width:100px;height:10px;"
        "transform:translateX(5px)}</style><div class=st></div>", 400,
        "page 400 10\n"
        "block html 0 0 400 10\n"
        "  block body 0 0 400 10\n"
        "    block div 0 0 100 10 positioned transform 5 0 sticky x 0..0 y 0..0\n");

    // grokipedia's drawer, Tailwind 3's -translate-x-full: six functions,
    // all but the first doing nothing, so a translation, drawn exactly —
    // off the page to the left, all 300 of it. Fixed, it reaches nothing.
    cascade_layout_case("transform: Tailwind's drawer is a translation",
        XFORM_STYLE ".tw{--tw-translate-x:-100%;--tw-translate-y:0;--tw-rotate:0;"
        "--tw-skew-x:0;--tw-skew-y:0;--tw-scale-x:1;--tw-scale-y:1;"
        "transform:translate(var(--tw-translate-x),var(--tw-translate-y)) rotate(var(--tw-rotate))"
        " skewX(var(--tw-skew-x)) skewY(var(--tw-skew-y)) scaleX(var(--tw-scale-x))"
        " scaleY(var(--tw-scale-y))}"
        ".drawer{position:fixed;top:0;left:0;width:300px;height:600px} .p{height:20px}</style>"
        "<div class='tw drawer'></div><div class=p></div>", 400,
        "page 400 20\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    block div 0 0 300 600 positioned fixed transform -300 0\n"
        "    block div 0 0 400 20\n");
}

typedef struct {
    const flow_box_t *watch[2];
    int32_t seen[2];
} XformVisits;

static void xform_visit(void *ctx, const flow_box_t *b)
{
    XformVisits *v = ctx;
    for (int i = 0; i < 2; i++)
        v->seen[i] += b == v->watch[i];
}

static void transform_door_cases(void)
{
    flow_point_t top = {0, 0};
    Sheeted q;

    // A transformed block is painted after the flow it is drawn over, so a
    // point on both is its: drawn 20 down, it covers the block after it.
    // Its own place in the flow is no longer it.
    sheeted_open(&q, XFORM_STYLE ".t{height:20px;transform:translateY(20px)} .n{height:20px}"
                     "</style><div class=t id=t></div><div class=n id=n></div>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *tb = box_with_id(t, q.doc->document, "t");
    const flow_box_t *nb = box_with_id(t, q.doc->document, "n");
    const flow_box_t *over = t != NULL ? flow_hit(t, 5, 25, top) : NULL;
    const flow_box_t *left = t != NULL ? flow_hit(t, 5, 5, top) : NULL;
    os64_gui_rect_t drawn = flow_box_doc_rect(tb, top);
    expect("transform: a transformed block is painted, and hit, over the flow",
           tb != NULL && nb != NULL && over == tb && left != tb && drawn.y == 20 && drawn.h == 20,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A transformed inline-block is a layer of its own, its atom and its
    // content drawn and hit 100 across and 5 down, and painted once: not on
    // its line too. Where it was laid out is the paragraph's.
    sheeted_open(&q, XFORM_STYLE ".ib{display:inline-block;width:50px;height:20px;"
                     "transform:translate(100px,5px)}</style><p id=p><span class=ib id=ib></span></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *atom = box_with_id(t, q.doc->document, "ib");
    const flow_box_t *inner = atom != NULL ? atom->first : NULL;
    const flow_box_t *para = box_with_id(t, q.doc->document, "p");
    XformVisits v = {{atom, inner}, {0, 0}};
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 600}, top, xform_visit, &v);
    os64_gui_rect_t at = atom != NULL ? atom->rect : (os64_gui_rect_t){0, 0, 0, 0};
    os64_gui_rect_t moved_at = flow_box_doc_rect(atom, top);
    os64_gui_rect_t inner_at = flow_box_doc_rect(inner, top);
    const flow_box_t *hit_moved = t != NULL ? flow_hit(t, moved_at.x + 25, moved_at.y + 10, top) : NULL;
    const flow_box_t *hit_home = t != NULL ? flow_hit(t, at.x + 25, at.y + 10, top) : NULL;
    expect("transform: a transformed inline-block is one layer, drawn and hit moved",
           atom != NULL && atom->kind == FLOW_BOX_ATOMIC && atom->stacked && inner != NULL &&
               !inner->stacked && moved_at.x == at.x + 100 && moved_at.y == at.y + 5 &&
               inner_at.x == inner->rect.x + 100 && v.seen[0] == 1 && v.seen[1] == 1 &&
               inside_of(hit_moved, atom) && hit_home != NULL && !inside_of(hit_home, atom) &&
               inside_of(hit_home, para),
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A fixed box in a transformed one scrolls with it: its place on the
    // page is the same at every scroll, where a fixed box's follows it.
    sheeted_open(&q, XFORM_STYLE ".t{height:100px;transform:translateX(10px)}"
                     ".f{position:fixed;top:7px;left:7px;width:10px;height:10px}"
                     "</style><div class=t><div class=f id=f></div></div>"
                     "<div class=f id=g></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *f = box_with_id(t, q.doc->document, "f");
    const flow_box_t *g = box_with_id(t, q.doc->document, "g");
    flow_point_t down = {0, 100};
    os64_gui_rect_t f0 = flow_box_doc_rect(f, top), f1 = flow_box_doc_rect(f, down);
    os64_gui_rect_t g1 = flow_box_doc_rect(g, down);
    expect("transform: a fixed box in a transformed one scrolls with it",
           f != NULL && g != NULL && !f->fixed && g->fixed && f0.x == 17 && f0.y == 7 &&
               f1.x == 17 && f1.y == 7 && g1.x == 7 && g1.y == 107,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A transformed box is a stacking context: a child of z-index 5 is
    // painted inside it, under the positioned block after it, which is
    // drawn over both.
    sheeted_open(&q, XFORM_STYLE ".t{height:20px;transform:translateX(0)}"
                     ".c{position:relative;z-index:5;height:20px}"
                     ".s{position:relative;margin-top:-20px;height:20px}"
                     "</style><div class=t><div class=c id=c></div></div><div class=s id=s></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *sb = box_with_id(t, q.doc->document, "s");
    expect("transform: a transformed box is a stacking context",
           sb != NULL && t != NULL && flow_hit(t, 5, 5, top) == sb, NULL);
    flow_free(t);
    sheeted_close(&q);

    // What clips a transformed box from outside stays where it is as the
    // box moves: the padding box of a box that clips and does not scroll,
    // 0 to 50, clips it and the absolute box it contains, drawn 20 down,
    // there and nowhere else. (`hidden` would make a scroll container,
    // whose clip is its own frame's.)
    sheeted_open(&q, XFORM_STYLE ".h{overflow:clip;height:50px}"
                     ".k{height:50px;transform:translateY(20px)}"
                     ".a{position:absolute;top:0;left:0;width:10px;height:10px}</style>"
                     "<div class=h><div class=k id=k><div class=a id=a></div></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *kb = box_with_id(t, q.doc->document, "k");
    const flow_box_t *ab = box_with_id(t, q.doc->document, "a");
    os64_gui_rect_t kc = flow_box_doc_clip(kb, top), ac = flow_box_doc_clip(ab, top);
    os64_gui_rect_t ar = flow_box_doc_rect(ab, top);
    expect("transform: what clips a transformed box from outside does not move",
           kb != NULL && ab != NULL && kc.x == 0 && kc.y == 0 && kc.w == 400 && kc.h == 50 &&
               ac.x == 0 && ac.y == 0 && ac.w == 400 && ac.h == 50 && ar.x == 0 && ar.y == 20,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A plain inline box cannot be transformed (Transforms 1 § 2): nothing
    // of it is stacked or moved, and it is no containing block — the
    // absolute box in it is placed against the viewport, at its corner.
    sheeted_open(&q, XFORM_STYLE "span{transform:translate(10px)}"
                     ".a{position:absolute;top:0;left:0;width:5px;height:5px}</style>"
                     "<p>yy<span>x<b class=a id=a></b></span></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    char *text = NULL;
    if (t != NULL) {
        int64_t need = flow_dump(t, NULL, 0);
        text = malloc((size_t)need + 1);
        flow_dump(t, text, (size_t)need + 1);
    }
    ab = box_with_id(t, q.doc->document, "a");
    expect("transform: a plain inline box's transform does nothing",
           text != NULL && strstr(text, "transform") == NULL && strstr(text, "stacked") == NULL &&
               ab != NULL && ab->rect.x == 0 && ab->rect.y == 0,
           text);
    free(text);
    flow_free(t);
    sheeted_close(&q);
}

static void transform_cases(void)
{
    transform_layout_cases();
    transform_door_cases();
}
