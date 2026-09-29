// Positioned layout (POSITION.md, slice P1), hand-computed from CSS 2.1 §
// 9.3, § 9.7, § 10.1, § 10.3.7, § 10.6.4 and Appendix E. The test font's
// numbers at 16px: ascent 12, descent 4, a line 20 with its baseline 14
// down; a glyph 8 wide, the narrow ones (i l t f r and the space) 4.

static void position_style_cases(void)
{
    // § 9.7: an absolute or fixed box is blockified and floats nowhere; the
    // display it was given survives as one bit. A list item stays one. A
    // translucent background on an OUT-OF-FLOW box draws nothing, a
    // relative one's is laid over the paper as ever (0.5 black on fefefe
    // is 7f); an opaque one is drawn. z-index is kept; opacity is clamped
    // to 0..1 in thousandths. display: contents makes no box to position.
    cascade_case("positioning: blockified, what it computes to", 800,
        "<!doctype html><style>body{margin:0}"
        ".a{position:absolute} .f{position:fixed} .r{position:relative}"
        ".glass{background:rgba(0,0,0,0.5)} .ink{background:rgba(0,0,0,1)}"
        ".z{z-index:5} .zn{z-index:-2} .o{opacity:0} .h{opacity:0.5} .big{opacity:7}"
        ".ins{inset:1px 2% auto -3px}</style>"
        "<span class=a>a</span><img class=a><span class=a style='display:inline-block'>b</span>"
        "<div class=a style='display:table-cell;float:left'>c</div><li class=f>d</li>"
        "<div class='a glass'>g</div><div class='r glass'>h</div><div class='a ink'>k</div>"
        "<span class='r z'>z</span><span class='r zn o'>y</span><b class=h>x</b><i class=big>w</i>"
        "<div class='r ins'>i</div><span style='position:absolute;display:contents'>c</span>",
        "html block holds-block\n"
        "  head none\n"
        "  body block holds-block\n"
        "    span block position=absolute specified-inline\n"
        "    img block position=absolute specified-inline\n"
        "    span block position=absolute specified-inline\n"
        "    div block position=absolute\n"
        "    li list-item position=fixed\n"
        "    div block position=absolute\n"
        "    div block bg=#7f7f7f position=relative\n"
        "    div block bg=#000000 position=absolute\n"
        "    span inline position=relative z=5\n"
        "    span inline position=relative z=-2 opacity=0.000\n"
        "    b inline weight=700 opacity=0.500\n"
        "    i inline italic\n"
        "    div block position=relative inset=1/2%/auto/-3\n"
        "    span contents position=absolute\n");

    // An out-of-flow child makes nothing hold a block: the span with an
    // absolute div in it is not split round it, and the body with only
    // that holds none either.
    cascade_case("an out-of-flow child holds no block", 800,
        "<!doctype html><style>div{position:absolute}</style><span>a<div>b</div>c</span>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8\n"
        "    span inline\n"
        "      div block position=absolute\n");
}

static void position_layout_cases(void)
{
    // An absolute box in each corner of a relative parent. The parent is
    // 2 + 3 + 200 + 3 + 2 = 210 across and 110 down; its padding box, the
    // containing block, is at (2, 2), 206 x 106. Right: 2 + 206 - 10 = 198;
    // bottom: 2 + 106 - 10 = 98. The parent holds nothing in flow — its
    // four placeholders make one empty line — and none of it moves.
    cascade_layout_case("positioned: an absolute box in each corner",
        "<!doctype html><style>body{margin:0}"
        ".r{position:relative;width:200px;height:100px;border:2px solid;padding:3px}"
        ".a{position:absolute;width:10px;height:10px}"
        ".tl{top:0;left:0} .tr{top:0;right:0} .bl{bottom:0;left:0} .br{bottom:0;right:0}"
        "</style><div class=r><div class='a tl'></div><div class='a tr'></div>"
        "<div class='a bl'></div><div class='a br'></div></div>", 400,
        "page 400 110\n"
        "block html 0 0 400 110\n"
        "  block body 0 0 400 110\n"
        "    block div 0 0 210 110 positioned\n"
        "      line 5 5 200 0 base 14\n"
        "      block div 2 2 10 10 positioned\n"
        "      block div 198 2 10 10 positioned\n"
        "      block div 2 98 10 10 positioned\n"
        "      block div 198 98 10 10 positioned\n");

    // An INLINE containing block (§ 10.1, 4.1): the relative link's one
    // piece, "link" at 20..44 on the line, its content area 2..18 (the
    // baseline 14 less the ascent 12, plus the descent 4). The badge, top 0
    // right 0, is at 44 - 6 = 38, 2 — in the link, which it splits nothing
    // of (it is not in flow).
    cascade_layout_case("positioned: the corner badge of a relative link",
        "<!doctype html><style>body{margin:0} p{margin:0} a{position:relative}"
        ".b{position:absolute;top:0;right:0;width:6px;height:6px}</style>"
        "<p>xx <a href=x>link<span class=b></span></a> yy</p>", 200,
        "page 200 20\n"
        "block html 0 0 200 20\n"
        "  block body 0 0 200 20\n"
        "    block p 0 0 200 20\n"
        "      line 0 0 200 20 base 14\n"
        "        span a 20 2 24 16 link 0\n"
        "        text \"xx \" 0 2 20 16 serif 16\n"
        "        text \"link\" 20 2 24 16 serif 16 underline link 0\n"
        "        text \" yy\" 44 2 20 16 serif 16\n"
        "      block span 38 2 6 6 link 0 positioned\n");

    // § 10.3.7 and § 10.6.4 against a 200 x 100 containing block at 0, 0:
    //   stretch — left 10, right 20, width auto: 200 - 10 - 20 - 8 of
    //     padding = 162, a 170 border box at 10; top 5.
    //   shrink — right 0, left and width auto: shrink-to-fit, "ab cd" is
    //     8 + 8 + 4 + 8 + 8 = 36 at most, in 200 of room: 36, at 164.
    //   centre — both sides and both margins auto: (200 - 100) / 2 = 50
    //     across, (100 - 40) / 2 = 30 down.
    //   over — left, right and width set: right is ignored, x 10.
    //   wide — 300 in 200 with auto margins: across, the start margin is
    //     never below 0, so x 0; down, the two are equal even negative:
    //     (100 - 140) / 2 = -20.
    //   lauto — left solved: 200 - 10 - 50 = 140.
    //   rauto — bottom 10: 100 - 10 - 20 = 70 down.
    //   pct — 10% 50% 50% 25% of 200 x 100: 20, 50, 100 x 25; the height's
    //     percentage resolves, the containing block's being known.
    // The page is as tall as what reaches furthest down: wide, to 120.
    cascade_layout_case("positioned: the width and height equations",
        "<!doctype html><style>body{margin:0}"
        ".r{position:relative;width:200px;height:100px} .a{position:absolute}"
        "#stretch{left:10px;right:20px;top:5px;height:8px;padding:0 4px}"
        "#shrink{right:0;top:0}"
        "#centre{left:0;right:0;width:100px;top:0;bottom:0;height:40px;margin:auto}"
        "#over{left:10px;right:10px;width:100px;top:0;height:1px}"
        "#wide{left:0;right:0;width:300px;margin:auto;top:0;bottom:0;height:140px}"
        "#lauto{right:10px;width:50px;top:0;height:1px}"
        "#rauto{left:10px;width:50px;bottom:10px;height:20px}"
        "#pct{left:10%;top:50%;width:50%;height:25%}</style>"
        "<div class=r><div class=a id=stretch></div><div class=a id=shrink>ab cd</div>"
        "<div class=a id=centre></div><div class=a id=over></div><div class=a id=wide></div>"
        "<div class=a id=lauto></div><div class=a id=rauto></div><div class=a id=pct></div>"
        "</div>", 400,
        "page 400 120\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 200 100 positioned\n"
        "      line 0 0 200 0 base 14\n"
        "      block div 10 5 170 8 positioned\n"
        "      block div 164 0 36 20 positioned\n"
        "        line 164 0 36 20 base 14\n"
        "          text \"ab cd\" 164 2 36 16 serif 16\n"
        "      block div 50 30 100 40 positioned\n"
        "      block div 10 0 100 1 positioned\n"
        "      block div 0 -20 300 140 positioned\n"
        "      block div 140 0 50 1 positioned\n"
        "      block div 10 70 50 20 positioned\n"
        "      block div 20 50 100 25 positioned\n");

    // Static positions (ruling 5, from Chrome). An absolute SPAN written
    // mid-line stands at the pen, "before " = 44, at the line's top; an
    // absolute DIV would have broken the line, so it starts the next one,
    // at the content edge, 20 + 20 = 40. Between blocks, below the
    // previous one's own margin: A ends at 60, its margin 10, so 70 — not
    // the 40 it collapses to with B, which is at 60 + 40 = 100.
    cascade_layout_case("positioned: static positions",
        "<!doctype html><style>body{margin:0} .line{position:relative;width:300px}"
        ".a{position:absolute} #A{height:20px;margin-bottom:10px}"
        "#B{height:20px;margin-top:40px} #between{width:10px;height:5px}</style>"
        "<div class=line>before <span class=a>S</span>after</div>"
        "<div class=line>before <div class=a>D</div>after</div>"
        "<div id=A></div><div class=a id=between></div><div id=B></div>", 400,
        "page 400 120\n"
        "block html 0 0 400 120\n"
        "  block body 0 0 400 120\n"
        "    block div 0 0 300 20 positioned\n"
        "      line 0 0 300 20 base 14\n"
        "        text \"before \" 0 2 44 16 serif 16\n"
        "        text \"after\" 44 2 28 16 serif 16\n"
        "      block span 44 0 8 20 positioned\n"
        "        line 44 0 8 20 base 14\n"
        "          text \"S\" 44 2 8 16 serif 16\n"
        "    block div 0 20 300 20 positioned\n"
        "      line 0 20 300 20 base 14\n"
        "        text \"before \" 0 22 44 16 serif 16\n"
        "        text \"after\" 44 22 28 16 serif 16\n"
        "      block div 0 40 8 20 positioned\n"
        "        line 0 40 8 20 base 14\n"
        "          text \"D\" 0 42 8 16 serif 16\n"
        "    block div 0 40 400 20\n"
        "    block div 0 70 10 5 positioned\n"
        "    block div 0 100 400 20\n");

    // The initial containing block is the viewport, 600 high (§ 10.1):
    // bottom 0 is 590, and the page reaches it. A drop whose containing
    // block is OUTSIDE an overflow: hidden box escapes its clip; one whose
    // containing block is that box keeps it (§ 11.1.1), and reaches no
    // further down the page than it (40). left: -9999px is where it says,
    // and widens nothing: "skip to content" is 28 + 4 + 12 + 4 + 48 = 96.
    // Relative boxes nested three deep move 10, 5 each, everything inside
    // with them: the innermost at 30, 60 + 15, and 30 + 400 wide is what
    // the page can be scrolled to.
    cascade_layout_case("positioned: the viewport, clips, off the page, relative",
        "<!doctype html><style>body{margin:0} p{margin:0}"
        ".foot{position:absolute;bottom:0;left:0;width:10px;height:10px}"
        ".rel{position:relative} .hid{overflow:hidden;height:20px}"
        ".drop{position:absolute;top:50px;left:0;width:30px;height:30px}"
        ".skip{position:absolute;left:-9999px} .nudge{position:relative;left:10px;top:5px}"
        "</style><div class=foot></div>"
        "<div class=rel><div class=hid><div class=drop></div></div></div>"
        "<div class='hid rel'><div class=drop></div></div>"
        "<p>a<span class=skip>skip to content</span>b</p>"
        "<div class=nudge><div class=nudge><div class=nudge>x</div></div></div>", 400,
        "page 430 600\n"
        "block html 0 0 400 80\n"
        "  block body 0 0 400 80\n"
        "    block div 0 590 10 10 positioned\n"
        "    block div 0 0 400 20 positioned\n"
        "      block div 0 0 400 20\n"
        "        line 0 0 400 0 base 14 clip 0 0 400 20\n"
        "        block div 0 50 30 30 positioned\n"
        "    block div 0 20 400 20 positioned\n"
        "      line 0 20 400 0 base 14 clip 0 20 400 20\n"
        "      block div 0 70 30 30 positioned clip 0 20 400 20\n"
        "    block p 0 40 400 20\n"
        "      line 0 40 400 20 base 14\n"
        "        text \"a\" 0 42 8 16 serif 16\n"
        "        text \"b\" 8 42 8 16 serif 16\n"
        "      block span -9999 40 96 20 positioned\n"
        "        line -9999 40 96 20 base 14\n"
        "          text \"skip to content\" -9999 42 96 16 serif 16\n"
        "    block div 10 65 400 20 positioned\n"
        "      block div 20 70 400 20 positioned\n"
        "        block div 30 75 400 20 positioned\n"
        "          line 30 75 400 20 base 14\n"
        "            text \"x\" 30 77 8 16 serif 16\n");

    // A relative span across a line break moves both its pieces, 4 up. A
    // relative table cell, list item and inline-block are containing
    // blocks: the cell's 40 + 1 + 1 square at 0, 40 puts its badge at
    // 42 - 4 = 38 — laid out once the row has stretched it; the item's 380
    // from 20, at 396; the inline-block's 50 from 8, at 54.
    cascade_layout_case("positioned: a relative span that breaks; cells, items, atoms",
        "<!doctype html><style>body{margin:0} ul{margin:0;padding:0 0 0 20px}"
        ".up{position:relative;top:-4px} .rel{position:relative}"
        ".a{position:absolute;top:0;right:0;width:4px;height:4px}"
        "td{padding:1px} table{border-spacing:0} .ib{display:inline-block;width:50px;height:30px}"
        "</style><div style='width:30px'><span class=up>aa bb</span></div>"
        "<table><tr><td class=rel style='width:40px;height:40px'>c<span class=a></span></td>"
        "</tr></table><ul><li class=rel>item<span class=a></span></li></ul>"
        "<div>x<span class='ib rel'>y<span class=a></span></span></div>", 400,
        "page 400 132\n"
        "block html 0 0 400 132\n"
        "  block body 0 0 400 132\n"
        "    block div 0 0 30 40\n"
        "      line 0 0 30 20 base 14\n"
        "        span span 0 -2 16 16\n"
        "        text \"aa\" 0 -2 16 16 serif 16\n"
        "      line 0 20 30 20 base 14\n"
        "        span span 0 18 16 16\n"
        "        text \"bb\" 0 18 16 16 serif 16\n"
        "    table table 0 40 42 42\n"
        "      row-group tbody 0 40 42 42\n"
        "        row tr 0 40 42 42\n"
        "          cell td 0 40 42 42 positioned\n"
        "            line 1 41 40 20 base 14\n"
        "              text \"c\" 1 43 8 16 serif 16\n"
        "            block span 38 40 4 4 positioned\n"
        "    block ul 0 82 400 20\n"
        "      block li 20 82 380 20 positioned\n"
        "        line 20 82 380 20 base 14\n"
        "          text \"item\" 20 84 28 16 serif 16\n"
        "        block span 396 82 4 4 positioned\n"
        "        marker \"\xe2\x80\xa2 \" 8 84 12 16\n"
        "    block div 0 102 400 30\n"
        "      line 0 102 400 30 base 14\n"
        "        text \"x\" 0 104 8 16 serif 16\n"
        "        atomic span 8 102 50 30\n"
        "          block span 8 102 50 30\n"
        "            line 8 102 50 20 base 14\n"
        "              text \"y\" 8 104 8 16 serif 16\n"
        "            block span 54 102 4 4 positioned\n");

    // The chapter's open dialog, with no sheet: absolute, left and right 0,
    // margins auto, fit-content. "Hi" is 12 at most; its frame 2 x (3 + 16)
    // = 38; so 50 wide, (800 - 50) / 2 = 375 across. Down, at its static
    // position below the first p's margin: 16 + 20 + 16 = 52, and 3 + 16 +
    // 20 + 16 + 3 = 58 tall. The p after it is where it would be without it.
    cascade_layout_case("positioned: an open dialog, from the chapter alone",
        "<!doctype html><p>text</p><dialog open>Hi</dialog><p>more</p>", 800,
        "page 800 110\n"
        "block html 0 0 800 88\n"
        "  block body 8 16 784 56\n"
        "    block p 8 16 784 20\n"
        "      line 8 16 784 20 base 14\n"
        "        text \"text\" 8 18 24 16 serif 16\n"
        "    block dialog 375 52 50 58 positioned\n"
        "      line 394 71 12 20 base 14\n"
        "        text \"Hi\" 394 73 12 16 serif 16\n"
        "    block p 8 52 784 20\n"
        "      line 8 52 784 20 base 14\n"
        "        text \"more\" 8 54 32 16 serif 16\n");
}

// What the face reads: the order boxes are painted in, what a click finds,
// the list, and what opacity and the static key do.
static const char *kModalPage =
    "<!doctype html><style>body{margin:0} p{margin:0}"
    ".modal{position:absolute;top:0;left:0;width:100px;height:40px;background:white}"
    ".gone{opacity:0}</style>"
    "<p>under the modal</p><p class=gone>faded <b>out</b></p>"
    "<div class=modal>over</div>";

static void position_door_cases(void)
{
    Sheeted p;
    sheeted_open(&p, kModalPage, 400, 600);
    flow_tree_t *t = flow_layout(p.doc, p.page, 400, &p.env);
    expect("positioned door: laid out", t != NULL && !flow_incomplete(t), NULL);
    if (t == NULL) {
        sheeted_close(&p);
        return;
    }
    const os64_html_node_t *modal = p.doc->body->last_child;
    const flow_box_t *mb = flow_box_for(t, modal);
    expect("positioned door: the modal is the list's one entry",
           flow_npositioned(t) == 1 && flow_positioned(t, 0) == mb && mb->positioned &&
           flow_positioned(t, 1) == NULL, NULL);

    // Appendix E: the page's text is painted before the modal's background,
    // and the modal's text after it — its own text on top of its own
    // background, and nothing of the page over either. The faded p paints
    // nothing, and nothing inside it.
    Seen seen = {0};
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 600}, seen_add, &seen);
    int under = -1, box = -1, over = -1, faded = 0;
    for (int i = 0; i < seen.n; i++) {
        const flow_box_t *b = seen.v[i];
        if (b->kind == FLOW_BOX_TEXT && b->length >= 5 && memcmp(b->text, "under", 5) == 0)
            under = i;
        if (b == mb)
            box = i;
        if (b->kind == FLOW_BOX_TEXT && b->length == 4 && memcmp(b->text, "over", 4) == 0)
            over = i;
        faded += b->unpainted;
    }
    expect("positioned door: page text, then the modal's box, then its text",
           under >= 0 && box > under && over > box, NULL);
    expect("positioned door: nothing unpainted is painted", faded == 0, NULL);
    const os64_html_node_t *gone = p.doc->body->first_child->next;
    const flow_box_t *gb = flow_box_for(t, gone);
    expect("positioned door: opacity 0 is unpainted, and what is inside it",
           gb != NULL && gb->unpainted && gb->first != NULL && gb->first->first != NULL &&
           gb->first->first->unpainted, NULL);

    // A click where the modal covers the page's text is the modal's; one on
    // that text past the modal's 100 ("under the modal" is 104) is the
    // text's; one on the faded paragraph still finds it, as a browser's
    // pointer does.
    const flow_box_t *h = flow_hit(t, 20, 8);
    expect("positioned door: a click over the page's text finds the modal",
           h != NULL && h->kind == FLOW_BOX_TEXT && h->length == 4 &&
           memcmp(h->text, "over", 4) == 0, NULL);
    h = flow_hit(t, 102, 8);
    expect("positioned door: beside the modal, the page's text",
           h != NULL && h->kind == FLOW_BOX_TEXT && memcmp(h->text, "under", 5) == 0, NULL);
    h = flow_hit(t, 150, 28);
    expect("positioned door: an unpainted box is still there to be pointed at",
           h != NULL && h->unpainted, NULL);
    flow_free(t);

    // The static key lays every box out in the flow: the modal is below
    // the paragraphs, and positioned nowhere.
    p.env.static_only = true;
    t = flow_layout(p.doc, p.page, 400, &p.env);
    mb = t != NULL ? flow_box_for(t, modal) : NULL;
    expect("positioned door: static_only lays the page out in document order",
           mb != NULL && !mb->positioned && mb->rect.y == 40 && flow_npositioned(t) == 0, NULL);
    flow_free(t);
    p.env.static_only = false;

    // One viewport height: a cascade judged at another is refused.
    p.env.viewport_height = 599;
    expect("positioned door: a cascade judged at another height is refused",
           flow_layout(p.doc, p.page, 400, &p.env) == NULL, NULL);
    sheeted_close(&p);
}

static void position_cases(void)
{
    position_style_cases();
    position_layout_cases();
    position_door_cases();
}
