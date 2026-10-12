// tools/test_libflow_transform.inc — transforms, the model and the
// translations and the painter (PILE3.md § Transforms, slices S7a and
// S7b), included by test_libflow_host.c. Every expected geometry is worked
// by hand from CSS Transforms 1 and 2 and the design, never captured from
// the engine (LAYOUT.md § Proof). Sized boxes, body margin 0, a 400 by 600
// viewport.
//
// A translation is drawn moved by `by`, where its matrix sends its
// transform-origin ("transform dx dy" in a dump). A flat matrix that does
// more is drawn THROUGH it ("through a b c d e f", thousandths). One that
// is not flat is drawn upright, moved by `by`: where the product of its
// functions sends the point it is turned about, whatever the origin is.

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
    // T(10, 0) R(90deg) T(20, 0) sends (x, y) to (10 - y, x + 20): the 20
    // turned a quarter clockwise is 20 down. Taken the list first, it would
    // be (30 - y, x). Drawn through, its corners land from -30 to 10 across
    // and 20 to 120 down, and the page reaches 120.
    cascade_layout_case("transform: the individual properties before the list",
        XFORM_STYLE ".t{width:100px;height:40px;transform-origin:0 0;translate:10px;"
        "rotate:90deg;transform:translateX(20px)}</style><div class=t></div>", 400,
        "page 400 120\n"
        "block html 0 0 400 40\n"
        "  block body 0 0 400 40\n"
        "    block div 0 0 100 40 stacked through 0 1000 -1000 0 10000 20000\n");

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

    // What does more than translate is drawn through its matrix, about each
    // box's centre (50, 5 + its top): a scale of 2 is 2x - 50 across; a
    // translation after it is scaled too, 10 becoming 20; a quarter turn
    // sends (x, y) to (75 - y, x - 15) at the third box, its corners from
    // -15 to 85 down; an eighth turn's thousandths are 707, its corners to
    // 73.9 down. matrix()'s translation is in CSS pixels, and with nothing
    // else it is an offset. The page reaches the quarter turn's 85.
    cascade_layout_case("transform: a scale and a turn drawn through, a translation an offset",
        XFORM_STYLE "div{width:100px;height:10px} .a{transform:scale(2)}"
        ".b{transform:scale(2) translate(10px)} .c{transform:rotate(90deg) translateX(10px)}"
        ".d{transform:rotate(45deg)} .e{transform:scale(2) translateY(10px)}"
        ".m{transform:matrix(1,0,0,1,10,20)}</style>"
        "<div class=a></div><div class=b></div><div class=c></div><div class=d></div>"
        "<div class=e></div><div class=m></div>", 400,
        "page 400 85\n"
        "block html 0 0 400 60\n"
        "  block body 0 0 400 60\n"
        "    block div 0 0 100 10 stacked through 2000 0 0 2000 -50000 -5000\n"
        "    block div 0 10 100 10 stacked through 2000 0 0 2000 -30000 -15000\n"
        "    block div 0 20 100 10 stacked through 0 1000 -1000 0 75000 -15000\n"
        "    block div 0 30 100 10 stacked through 707 707 -707 707 39393 -25104\n"
        "    block div 0 40 100 10 stacked through 2000 0 0 2000 -50000 -25000\n"
        "    block div 0 50 100 10 stacked transform 10 20\n");

    // A transformed root moves the whole page: 100 tall, drawn from 600
    // down, the page reaches 700.
    cascade_layout_case("transform: a transformed root reaches the page moved",
        XFORM_STYLE "html{transform:translateY(600px)} div{height:100px}</style><div></div>", 400,
        "page 400 700\n"
        "block html 0 0 400 100 transform 0 600\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n");

    // `position: fixed` on the root changes none of that: the root is laid
    // out in the flow whatever its position (POSITION.md § Booked).
    cascade_layout_case("transform: a root written fixed is laid out in the flow, moved",
        XFORM_STYLE "html{position:fixed;transform:translateY(600px)} div{height:100px}</style>"
        "<div></div>", 400,
        "page 400 700\n"
        "block html 0 0 400 100 transform 0 600\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n");

    // Nor does `display: inline`: the root is blockified (Display 3 §
    // 2.7), so its transform applies, as a block's does.
    cascade_layout_case("transform: a root written inline is a block, and moved",
        XFORM_STYLE "html{display:inline;position:relative;transform:translateY(600px)}"
        "div{height:100px}</style><div></div>", 400,
        "page 400 700\n"
        "block html 0 0 400 100 transform 0 600\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n");

    // A skew and a 3D rotation are flat, drawn through: skewX(45deg) about
    // (50, 5) sends (x, y) to (x + y - 5, y); rotateX(60deg) about (50, 15)
    // halves the height, y to 0.5 y + 7.5. A turn about y in perspective
    // is not flat — its w reads x — and is drawn upright, its centre where
    // it was.
    cascade_layout_case("transform: a skew and a 3D turn drawn through, a perspective upright",
        XFORM_STYLE "div{width:100px;height:10px} .s{transform:skewX(45deg)}"
        ".x{transform:rotateX(60deg)} .p{transform:perspective(100px) rotateY(45deg)}</style>"
        "<div class=s></div><div class=x></div><div class=p></div>", 400,
        "page 400 30\n"
        "block html 0 0 400 30\n"
        "  block body 0 0 400 30\n"
        "    block div 0 0 100 10 stacked through 1000 0 1000 1000 -5000 0\n"
        "    block div 0 10 100 10 stacked through 1000 0 0 500 0 7500\n"
        "    block div 0 20 100 10 stacked transform 0 0 upright\n");

    // Flat means w reads nothing but itself, exactly: an m3 of 5e-10 makes
    // w read x, and the box is drawn upright; a matrix whose every entry is
    // 1e-10 of the identity's divides back to the identity, a translation
    // of nothing.
    cascade_layout_case("transform: flatness is exact, and judged with w divided out",
        XFORM_STYLE "div{width:100px;height:10px}"
        ".a{transform-origin:0 0;transform:matrix3d(1,0,0,5e-10, 0,1,0,0, 0,0,1,0, 0,0,0,1)}"
        ".b{transform:matrix3d(1e-10,0,0,0, 0,1e-10,0,0, 0,0,1e-10,0, 0,0,0,1e-10)}</style>"
        "<div class=a></div><div class=b></div>", 400,
        "page 400 20\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    block div 0 0 100 10 stacked transform 0 0 upright\n"
        "    block div 0 10 100 10 stacked transform 0 0\n");

    // A matrix may hold any finite number: scale(1e20) is dumped held to
    // what the dump's integers can say, and reaches as far as a rect can.
    cascade_layout_case("transform: a huge scale is dumped held",
        XFORM_STYLE "div{width:100px;height:10px;transform:scale(1e20)}</style><div></div>", 400,
        "page 1073741823 1073741823\n"
        "block html 0 0 400 10\n"
        "  block body 0 0 400 10\n"
        "    block div 0 0 100 10 stacked through 9000000000000000000 0 0 9000000000000000000"
        " -9000000000000000000 -9000000000000000000\n");

    // A whole number of quarter turns is exact in any unit — 450deg is a
    // quarter, -100grad three — and a turn a hair short of a quarter is
    // worked as written: rotateX(89.99999999999994deg) keeps its height,
    // its cosine about 1e-15.
    cascade_layout_case("transform: quarter turns exact in their unit, a hair short not",
        XFORM_STYLE "div{width:100px;height:10px;transform-origin:0 0}"
        ".a{transform:rotate(450deg)} .b{transform:rotate(-100grad)}"
        ".c{transform:rotateX(89.99999999999994deg)}</style>"
        "<div class=a></div><div class=b></div><div class=c></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 30\n"
        "  block body 0 0 400 30\n"
        "    block div 0 0 100 10 stacked through 0 1000 -1000 0 0 0\n"
        "    block div 0 10 100 10 stacked through 0 -1000 1000 0 -10000 10000\n"
        "    block div 0 20 100 10 stacked through 1000 0 0 0 0 20000\n");

    // A scale written as a calc() of a percentage is that hundredth, in a
    // list and as the property: half about the centre (50, 5), 0.5x + 35
    // with the 20 halved; twice about (50, 15), the 10 doubled, reaching 25.
    cascade_layout_case("transform: a scale of calc(%), in the list and alone",
        XFORM_STYLE "div{width:100px;height:10px}"
        ".a{transform:scale(calc(50%)) translateX(20px)}"
        ".b{scale:calc(200%);transform:translateX(10px)}</style>"
        "<div class=a></div><div class=b></div>", 400,
        "page 400 25\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    block div 0 0 100 10 stacked through 500 0 0 500 35000 2500\n"
        "    block div 0 10 100 10 stacked through 2000 0 0 2000 -30000 -15000\n");

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

    // In a scroll container that is its containing block, it sticks for
    // all the container scrolls through: the 90 below it in the 100 tall
    // padding box, and the 410 more the content reaches. The dump meets
    // a sticky box's clip only out to its sticky frame, which moves it, so
    // here it is none.
    cascade_layout_case("transform: a sticky box transformed sticks the whole scroll",
        XFORM_STYLE "#s{height:100px;overflow:auto} .st{position:sticky;top:0;width:100px;"
        "height:10px;transform:translateX(5px)}</style>"
        "<div id=s><div class=st></div><p style='height:500px'></p></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100 scroller 0 range 0 410\n"
        "      block div 0 0 100 10 positioned transform 5 0 sticky x 0..0 y 0..500"
        " clip -1073741824 -1073741824 2147483647 2147483647\n"
        "      block p 0 10 400 500 clip 0 0 400 100\n");

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

    // Positioned, it is the absolute boxes' containing block, but its
    // transform is still nothing: a fixed box in it is fixed to the
    // viewport, at its corner, not to the inline.
    sheeted_open(&q, XFORM_STYLE "span{position:relative;transform:translate(10px)}"
                     ".f{position:fixed;top:0;left:0;width:5px;height:5px}</style>"
                     "<p>yy<span>x<b class=f id=f></b></span></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    ab = box_with_id(t, q.doc->document, "f");
    expect("transform: a positioned plain inline is no fixed box's containing block",
           ab != NULL && ab->fixed && ab->rect.x == 0 && ab->rect.y == 0, NULL);
    flow_free(t);
    sheeted_close(&q);
}

// The groups a walk opens: how many, the last one's box and bounds, and
// whether box `watch` was visited.
typedef struct {
    int32_t opens, closes;
    const flow_box_t *box, *watch;
    os64_gui_rect_t bounds, first;
    bool seen;
} XformGroups;

static void xform_group_visit(void *ctx, const flow_box_t *b)
{
    XformGroups *g = ctx;
    g->seen = g->seen || b == g->watch;
}

static void xform_group_open(void *ctx, const flow_box_t *b, os64_gui_rect_t bounds)
{
    XformGroups *g = ctx;
    if (g->opens++ == 0)
        g->first = bounds;
    g->box = b;
    g->bounds = bounds;
}

static void xform_group_close(void *ctx, const flow_box_t *b)
{
    (void)b;
    ((XformGroups *)ctx)->closes++;
}

static bool rect_is(os64_gui_rect_t r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

// Slice S7b (PILE3.md § Transforms, the painter): a flat matrix that does
// more than translate is drawn THROUGH — painted in the box's own
// coordinates and laid through the matrix — so where a box is painted and
// where it is part. Every answer is worked by hand from the matrix.
static void transform_through_cases(void)
{
    flow_point_t top = {0, 0};
    Sheeted q;

    // A quarter turn about the centre (150, 110) of a box at 100, 100,
    // 100 by 20: (x, y) goes to (260 - y, x - 40). The box stands 140 to
    // 160 across and 60 to 160 down; its first 20 pixels, the child, 60
    // to 80 down. A point drawn there is the child's, taken back to (110,
    // 110); one further down is the box's; where it was laid out is no
    // longer it.
    sheeted_open(&q, XFORM_STYLE ".t{position:absolute;left:100px;top:100px;width:100px;"
                     "height:20px;transform:rotate(90deg)} .c{width:20px;height:20px}"
                     "</style><div class=t id=t><div class=c id=c></div></div>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *tb = box_with_id(t, q.doc->document, "t");
    const flow_box_t *cb = box_with_id(t, q.doc->document, "c");
    double m[6] = {0};
    os64_gui_rect_t clip = {0};
    flow_point_t upright = {1, 1}, back = {0, 0};
    bool through = flow_box_through(tb, top, m, &clip, &upright);
    bool point = flow_box_point(cb, top, 150, 70, &back);
    expect("transform: a turned box is where its corners go, its child too",
           tb != NULL && cb != NULL && rect_is(flow_box_doc_rect(tb, top), 140, 60, 20, 100) &&
               rect_is(flow_box_doc_rect(cb, top), 140, 60, 20, 20) &&
               flow_box_doc_offset(cb, top).x == 0 && flow_box_doc_offset(cb, top).y == 0,
           NULL);
    expect("transform: a turned box hands the face its matrix",
           through && fabs(m[0]) < 1e-9 && fabs(m[1] - 1) < 1e-9 && fabs(m[2] + 1) < 1e-9 &&
               fabs(m[3]) < 1e-9 && fabs(m[4] - 260) < 1e-9 && fabs(m[5] + 40) < 1e-9 &&
               upright.x == 0 && upright.y == 0 && clip.w >= 1000000 &&
               !flow_box_through(cb, top, m, NULL, NULL),
           NULL);
    expect("transform: a turned box is hit through its matrix, and not where it was laid out",
           t != NULL && point && back.x == 110 && back.y == 110 && flow_hit(t, 150, 70, top) == cb &&
               flow_hit(t, 150, 150, top) == tb && !inside_of(flow_hit(t, 110, 110, top), tb),
           NULL);
    XformGroups gr = {0};
    gr.watch = cb;
    flow_visitor_t vis = {xform_group_visit, xform_group_open, xform_group_close, &gr};
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &vis);
    expect("transform: a turned box is a group, its bounds its own",
           gr.opens == 1 && gr.closes == 1 && gr.box == tb && rect_is(gr.bounds, 100, 100, 100, 20) &&
               gr.seen,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A turned box inside a translated one: what is inside the turn is
    // painted in the turn's own coordinates, the translation outside it
    // nothing there — it goes into the turn's matrix instead. About (50,
    // 10), (x, y) goes to (60 - y, x - 40), and 30 across more.
    sheeted_open(&q, XFORM_STYLE ".m{transform:translateX(30px)}"
                     ".t{width:100px;height:20px;transform:rotate(90deg)}</style>"
                     "<div class=m><div class=t id=t><p id=p>x</p></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    tb = box_with_id(t, q.doc->document, "t");
    const flow_box_t *inner = box_with_id(t, q.doc->document, "p");
    through = flow_box_through(tb, top, m, NULL, NULL);
    expect("transform: a turn inside a translation paints in its own coordinates",
           through && inner != NULL && flow_box_doc_offset(inner, top).x == 0 &&
               flow_box_doc_offset(inner, top).y == 0 && fabs(m[4] - 90) < 1e-9 &&
               fabs(m[5] + 40) < 1e-9,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // Layout geometry leaves every transform out: offsetTop and its kin
    // see the box where it was laid out, moved by a translation or turned.
    sheeted_open(&q, XFORM_STYLE ".m{height:10px;transform:translate(7px,9px)}"
                     ".t{width:100px;height:20px;transform:rotate(90deg)}</style>"
                     "<div class=m id=m></div><div class=t><p id=p>x</p></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *mb = box_with_id(t, q.doc->document, "m");
    const flow_box_t *pb = box_with_id(t, q.doc->document, "p");
    flow_point_t lm = flow_box_layout_offset(mb, top), lp = flow_box_layout_offset(pb, top);
    expect("transform: layout geometry leaves transforms out",
           mb != NULL && pb != NULL && lm.x == 0 && lm.y == 0 && lp.x == 0 && lp.y == 0 &&
               flow_box_doc_offset(mb, top).x == 7 && flow_box_doc_offset(mb, top).y == 9 &&
               flow_box_doc_rect(pb, top).x != pb->rect.x,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A matrix however small but not 0 is undone: scaleX(1e-12) takes a
    // point at its centre back in.
    sheeted_open(&q, XFORM_STYLE ".t{width:100px;height:20px;transform:scaleX(1e-12)}"
                     "</style><div class=t id=t></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    tb = box_with_id(t, q.doc->document, "t");
    expect("transform: a matrix however small but not 0 is undone, and a pixel wide",
           tb != NULL && flow_box_point(tb, top, 50, 10, &back) && back.y == 10 &&
               flow_box_doc_rect(tb, top).w == 1,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A shadow is in no box's overflow, but a group holds what it paints:
    // 0 0 10px 5px reaches 0 + 5 + 2 x 10 + 1 = 26 past the box, so the
    // turned box's 100 by 20 is held from -26, 152 by 72.
    sheeted_open(&q, XFORM_STYLE ".t{width:100px;height:20px;box-shadow:0 0 10px 5px #000;"
                     "transform:rotate(90deg)}</style><div class=t></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    XformGroups shadowed = {0};
    vis.ctx = &shadowed;
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &vis);
    expect("transform: a turned group holds the shadows it paints",
           shadowed.opens == 1 && rect_is(shadowed.first, -26, -26, 152, 72), NULL);
    vis.ctx = &gr;
    flow_free(t);
    sheeted_close(&q);

    // A matrix however large is undone, scaled first: scale(1e200)'s
    // determinant is past a double, its inverse is not.
    sheeted_open(&q, XFORM_STYLE ".t{width:100px;height:20px;transform:scale(1e200)}"
                     "</style><div class=t id=t></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    tb = box_with_id(t, q.doc->document, "t");
    through = flow_box_through(tb, top, m, NULL, NULL);
    double inv[6];
    os64_gui_rect_t big_back = flow_affine_rect_back(m, (os64_gui_rect_t){0, 0, 400, 600});
    expect("transform: a matrix however large is undone",
           through && flow_affine_invert(m, inv) && big_back.w > 0 && big_back.h > 0 &&
               flow_box_point(tb, top, 200, 300, &back),
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // One huge on one axis and tiny on the other is undone too, each
    // column scaled apart: matrix(1e300, 0, 0, 1e-300, 0, .5) has
    // determinant 1. An edge-on turn, rotateX(90deg), is collapsed: it is
    // hit nowhere.
    sheeted_open(&q, XFORM_STYLE ".t{width:100px;height:20px;transform-origin:0 0;"
                     "transform:matrix(1e300,0,0,1e-300,0,.5)} .x{width:100px;height:20px;"
                     "transform:rotateX(90deg)} .h{width:100px;height:20px;"
                     "transform:rotateX(89.99999999999994deg)}</style><div class=t id=t></div>"
                     "<div class=x id=x></div><div class=h id=h></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    tb = box_with_id(t, q.doc->document, "t");
    const flow_box_t *xb = box_with_id(t, q.doc->document, "x");
    through = flow_box_through(tb, top, m, NULL, NULL);
    os64_gui_rect_t thin = flow_affine_rect_back(m, (os64_gui_rect_t){0, 0, 400, 600});
    expect("transform: a matrix huge on one axis and tiny on the other is undone",
           through && flow_affine_invert(m, inv) && thin.w > 0 && thin.h > 0, NULL);
    // Entries spanning a double's whole range, determinant 1: undone.
    const double span[6] = {1e300, 1e-300, 1e300, 2e-300, 0, 0};
    expect("transform: a matrix spanning a double's range is undone",
           flow_affine_invert(span, inv) && isfinite(inv[0]) && isfinite(inv[3]), NULL);
    const flow_box_t *hb = box_with_id(t, q.doc->document, "h");
    double hm[6];
    expect("transform: a turn a hair short of edge-on is still undone",
           hb != NULL && flow_box_through(hb, top, hm, NULL, NULL) && hm[3] != 0 &&
               flow_affine_invert(hm, inv),
           NULL);
    expect("transform: an edge-on turn is hit nowhere",
           xb != NULL && !flow_box_point(xb, top, 50, 30, &back) &&
               !inside_of(flow_hit(t, 50, 30, top), xb),
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A layer clipped away entirely adds nothing, not even its corner: a
    // relative box pushed 1000 below its 20 tall clipping wrapper.
    sheeted_open(&q, XFORM_STYLE ".t{width:100px;height:20px;transform:rotate(90deg)}"
                     ".w{overflow:clip;height:20px} .r{position:relative;top:1000px;height:10px}"
                     "</style><div class=t><div class=w><div class=r></div></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    XformGroups away = {0};
    vis.ctx = &away;
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &vis);
    expect("transform: a layer clipped away adds nothing to a turned group's bounds",
           away.opens == 1 && rect_is(away.first, 0, 0, 100, 20), NULL);
    vis.ctx = &gr;
    flow_free(t);
    sheeted_close(&q);

    // A layer inside a turned group is counted only as far as what clips it
    // inside the group lets it show: a 5000 tall relative box in a 20 tall
    // clipping wrapper adds 20, not 5000, to the surface.
    sheeted_open(&q, XFORM_STYLE ".t{width:100px;height:20px;transform:rotate(90deg)}"
                     ".w{overflow:clip;height:20px} .r{position:relative;height:5000px}</style>"
                     "<div class=t><div class=w><div class=r></div></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    XformGroups clipped = {0};
    vis.ctx = &clipped;
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &vis);
    expect("transform: a turned group's bounds are cut by the clips inside it",
           clipped.opens == 1 && rect_is(clipped.first, 0, 0, 100, 20), NULL);
    vis.ctx = &gr;
    flow_free(t);
    sheeted_close(&q);

    // Turns nest: a quarter turn about (25, 25) inside one about (50, 50)
    // is a half turn, (x, y) to (100 - x, 50 - y). The inner box stands 50
    // to 100 across and 0 to 50 down; (90, 10) is its (10, 40).
    sheeted_open(&q, XFORM_STYLE ".o{position:absolute;left:0;top:0;width:100px;height:100px;"
                     "transform:rotate(90deg)} .i{position:absolute;left:0;top:0;width:50px;"
                     "height:50px;transform:rotate(90deg)}</style>"
                     "<div class=o id=o><div class=i id=i></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *ib = box_with_id(t, q.doc->document, "i");
    point = flow_box_point(ib, top, 90, 10, &back);
    expect("transform: turns nest, drawn and hit through both",
           ib != NULL && rect_is(flow_box_doc_rect(ib, top), 50, 0, 50, 50) && point &&
               back.x == 10 && back.y == 40 && flow_hit(t, 90, 10, top) == ib,
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // A turned group inside a turned group is counted through its matrix:
    // the inner bar, 100 by 10 about (50, 5), goes to (55 - y, x - 45) in
    // the outer box's own coordinates, 45 to 55 across and -45 to 55 down,
    // so the outer group's bounds reach 45 above its 100 by 100.
    sheeted_open(&q, XFORM_STYLE ".o{position:absolute;left:0;top:0;width:100px;height:100px;"
                     "transform:rotate(90deg)} .i{position:absolute;left:0;top:0;width:100px;"
                     "height:10px;transform:rotate(90deg)}</style>"
                     "<div class=o><div class=i></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    XformGroups nest = {0};
    vis.ctx = &nest;
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &vis);
    expect("transform: a turned group counts a turned group inside it turned",
           nest.opens == 2 && rect_is(nest.first, 0, -45, 100, 145), NULL);
    vis.ctx = &gr;
    flow_free(t);
    sheeted_close(&q);

    // What clips a turned box from outside stays where it is: about (50,
    // 10), (x, y) goes to (60 - y, x - 40), drawn from -40 to 60 down, and
    // the 50 tall wrapper cuts it there. (50, 45) is its (85, 10); (50, 55)
    // would be its (95, 10), but is clipped away.
    sheeted_open(&q, XFORM_STYLE ".w{overflow:clip;height:50px} .t{width:100px;height:20px;"
                     "transform:rotate(90deg)}</style><div class=w><div class=t id=t></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    tb = box_with_id(t, q.doc->document, "t");
    through = flow_box_through(tb, top, m, &clip, NULL);
    expect("transform: what clips a turned box from outside stays put",
           through && clip.y == 0 && clip.h == 50 && flow_hit(t, 50, 45, top) == tb &&
               !inside_of(flow_hit(t, 50, 55, top), tb),
           NULL);
    flow_free(t);
    sheeted_close(&q);

    // The walk culls against the view taken back through the matrix: a
    // box laid out 500 down, turned back a quarter about its corner (0,
    // 500) — (x, y) to (y - 500, 500 - x) — is drawn from 100 to 500 down,
    // in a 300 tall view, and is walked; translucent too, it is one group.
    sheeted_open(&q, XFORM_STYLE ".t{position:absolute;left:0;top:500px;width:400px;height:10px;"
                     "transform-origin:0 0;transform:rotate(-90deg);opacity:.5}"
                     ".c{height:10px}</style><div class=t id=t><div class=c id=c></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    cb = box_with_id(t, q.doc->document, "c");
    XformGroups cull = {0};
    cull.watch = cb;
    vis.ctx = &cull;
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 300}, top, &vis);
    expect("transform: a box laid out off the view but drawn in it is walked, one group",
           cb != NULL && cull.seen && cull.opens == 1 && cull.closes == 1, NULL);
    flow_free(t);
    sheeted_close(&q);

    // And the other way about: turned a quarter back about its corner (0,
    // 0) — (x, y) to (y, -x) — a bar laid in the view is drawn above it,
    // but a face that refuses to lay it draws it upright, its corner where
    // it was, in the view: the walk keeps it for that face.
    sheeted_open(&q, XFORM_STYLE ".t{position:absolute;left:0;top:0;width:400px;height:10px;"
                     "transform-origin:0 0;transform:rotate(-90deg)}"
                     ".c{height:10px}</style><div class=t><div class=c id=c></div></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    cb = box_with_id(t, q.doc->document, "c");
    XformGroups refuse = {0};
    refuse.watch = cb;
    vis.ctx = &refuse;
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 300}, top, &vis);
    expect("transform: a box drawn off the view is walked where an upright drawing shows it",
           cb != NULL && refuse.seen, NULL);
    vis.ctx = &gr;
    flow_free(t);
    sheeted_close(&q);
}

static void transform_cases(void)
{
    transform_layout_cases();
    transform_door_cases();
    transform_through_cases();
}
