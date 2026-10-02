// Positioned layout (POSITION.md, slice P1), hand-computed from CSS 2.1 §
// 9.3, § 9.7, § 10.1, § 10.3.7, § 10.6.4 and Appendix E. The test font's
// numbers at 16px: ascent 12, descent 4, a line 20 with its baseline 14
// down; a glyph 8 wide, the narrow ones (i l t f r and the space) 4.

static void position_style_cases(void)
{
    // § 9.7: an absolute or fixed box is blockified and floats nowhere; the
    // display it was given survives as one bit. A list item stays one. A
    // translucent background keeps its alpha, in the flow or out of it, for
    // the face to blend (0.5 is 128 of 255); an opaque one is opaque.
    // z-index is kept; opacity is clamped
    // to 0..1 in thousandths. display: contents makes no box to position.
    cascade_case("positioning: blockified, what it computes to", 800,
        "<!doctype html><style>body{margin:0}"
        ".a{position:absolute} .f{position:fixed} .r{position:relative}"
        ".glass{background:rgba(0,0,0,0.5)} .ink{background:rgba(0,0,0,1)}"
        ".z{z-index:5} .zn{z-index:-2} .o{opacity:0} .h{opacity:0.5} .big{opacity:7}"
        ".ins{inset:1px 2% auto -3px}</style>"
        "<span class=a>a</span><img class=a><span class=a style='display:inline-block'>b</span>"
        "<div class=a style='display:table-cell;float:left'>c</div><li class=f>d</li>"
        "<span class=f>e</span>"
        "<div class='a glass'>g</div><div class='r glass'>h</div><div class='a ink'>k</div>"
        "<span class='r z'>z</span><span class='r zn o'>y</span><b class=h>x</b><i class=big>w</i>"
        "<div class='r ins'>i</div><span style='position:absolute;display:contents'>c</span>"
        "<div style='pointer-events:none'><b>n</b><i style='pointer-events:auto'>a</i></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block holds-block\n"
        "    span block position=absolute specified-inline\n"
        "    img block position=absolute specified-inline\n"
        "    span block position=absolute specified-inline\n"
        "    div block position=absolute\n"
        "    li list-item position=fixed\n"
        "    span block position=fixed specified-inline\n"
        "    div block bg=#000000/128 position=absolute\n"
        "    div block bg=#000000/128 position=relative\n"
        "    div block bg=#000000 position=absolute\n"
        "    span inline position=relative z=5\n"
        "    span inline position=relative z=-2 opacity=0.000\n"
        "    b inline weight=700 opacity=0.500\n"
        "    i inline italic\n"
        "    div block position=relative inset=1/2%/auto/-3\n"
        "    span contents position=absolute\n"
        // pointer-events is inherited: the b is as its div is.
        "    div block pointer-events=none\n"
        "      b inline weight=700\n"
        "      i inline italic pointer-events=auto\n");

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
    // Fixed boxes (P2) against the viewport, 400 x 600, whatever is round
    // them: a header across the top, 0 0 400 30, with an absolute badge in
    // it at 392 (the header is its containing block); a footer 10 from the
    // right and on the bottom, 400 - 10 - 50 = 340 and 600 - 20 = 580; and
    // one inside a relative div, at 5 5 all the same. The page is as tall
    // and as wide as the flow and the relative div make it (1000 + 20, and
    // 100 more of each for the div's offset), never the fixed boxes.
    cascade_layout_case("positioned: fixed, against the viewport",
        "<!doctype html><style>body{margin:0} p{margin:0;height:1000px}"
        ".h{position:fixed;top:0;left:0;right:0;height:30px}"
        ".f{position:fixed;bottom:0;right:10px;width:50px;height:20px}"
        ".r{position:relative;left:100px;top:100px}"
        ".in{position:fixed;left:5px;top:5px;width:10px;height:10px}"
        ".h b{position:absolute;right:0;top:0;width:8px;height:8px}</style>"
        "<div class=h><b></b></div><p>long</p><div class=f></div>"
        "<div class=r>x<span class=in></span></div>", 400,
        "page 500 1120\n"
        "block html 0 0 400 1020\n"
        "  block body 0 0 400 1020\n"
        "    block div 0 0 400 30 positioned fixed\n"
        "      line 0 0 400 0 base 14\n"
        "      block b 392 0 8 8 positioned\n"
        "    block p 0 0 400 1000\n"
        "      line 0 0 400 20 base 14\n"
        "        text \"long\" 0 2 28 16 serif 16\n"
        "    block div 340 580 50 20 positioned fixed\n"
        "    block div 100 1100 400 20 positioned\n"
        "      line 100 1100 400 20 base 14\n"
        "        text \"x\" 100 1102 8 16 serif 16\n"
        "      block span 5 5 10 10 positioned fixed\n");

    // A fixed span inside a link is out of flow: it splits nothing, and
    // it is a fixed box of its own.
    {
        char *got = cascade_layout_of("<!doctype html><p>before <a href=x>link "
                                      "<span style='position:fixed'>top</span> more</a> after</p>",
                                      400);
        expect("positioned: a fixed span in a link splits no paragraph",
               got != NULL && strstr(got, "block anon") == NULL &&
                   strstr(got, " positioned fixed\n") != NULL, got);
        free(got);
    }

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
    // at the content edge, 20 + 20 = 40; one that LEADS its line has
    // nothing to break away from, and stands at that line's top, 40.
    // Between blocks, below the previous one's own margin: A ends at 80,
    // its margin 10, so 90 — not the 40 it collapses to with B, which is at
    // 80 + 40 = 120.
    cascade_layout_case("positioned: static positions",
        "<!doctype html><style>body{margin:0} .line{position:relative;width:300px}"
        ".a{position:absolute} #A{height:20px;margin-bottom:10px}"
        "#B{height:20px;margin-top:40px} #between{width:10px;height:5px}</style>"
        "<div class=line>before <span class=a>S</span>after</div>"
        "<div class=line>before <div class=a>D</div>after</div>"
        "<div class=line><div class=a>L</div>after</div>"
        "<div id=A></div><div class=a id=between></div><div id=B></div>", 400,
        "page 400 140\n"
        "block html 0 0 400 140\n"
        "  block body 0 0 400 140\n"
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
        "    block div 0 40 300 20 positioned\n"
        "      line 0 40 300 20 base 14\n"
        "        text \"after\" 0 42 28 16 serif 16\n"
        "      block div 0 40 8 20 positioned\n"
        "        line 0 40 8 20 base 14\n"
        "          text \"L\" 0 42 8 16 serif 16\n"
        "    block div 0 60 400 20\n"
        "    block div 0 90 10 5 positioned\n"
        "    block div 0 120 400 20\n");

    // The initial containing block is the viewport, 600 high (§ 10.1):
    // bottom 0 is 590, and the page reaches it. A drop whose containing
    // block is OUTSIDE an overflow: hidden box escapes its clip; one whose
    // containing block is that box keeps it (§ 11.1.1), and reaches no
    // further down the page than it (40) — it is what that box scrolls, 60
    // past it, and the escaped one gives its own box nothing to scroll.
    // left: -9999px is where it says,
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
        "      block div 0 0 400 20 scroller 0 range 0 0\n"
        "        line 0 0 400 0 base 14 clip 0 0 400 20\n"
        "        block div 0 50 30 30 positioned\n"
        "    block div 0 20 400 20 positioned scroller 1 range 0 60\n"
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
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 600}, kNoScroll, seen_add, &seen);
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
    const flow_box_t *h = flow_hit(t, 20, 8, kNoScroll);
    expect("positioned door: a click over the page's text finds the modal",
           h != NULL && h->kind == FLOW_BOX_TEXT && h->length == 4 &&
           memcmp(h->text, "over", 4) == 0, NULL);
    h = flow_hit(t, 102, 8, kNoScroll);
    expect("positioned door: beside the modal, the page's text",
           h != NULL && h->kind == FLOW_BOX_TEXT && memcmp(h->text, "under", 5) == 0, NULL);
    h = flow_hit_in_flow(t, 20, 8);
    expect("positioned door: in the flow alone, the text under the modal",
           h != NULL && h->kind == FLOW_BOX_TEXT && memcmp(h->text, "under", 5) == 0, NULL);
    h = flow_hit(t, 150, 28, kNoScroll);
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

    // A relative box is in the flow: what lies under a point inside one is
    // its text, not the box round it that its overflow rect leaves out —
    // the body relative, as pages write it, and a relative wrapper in it.
    {
        Sheeted q;
        sheeted_open(&q, "<!doctype html><style>body{margin:0;position:relative}"
                         "div{position:relative;top:5px} .m{position:absolute;top:0;left:0;"
                         "width:50px;height:50px}</style><div>moved text</div><p class=m>over</p>",
                     400, 600);
        flow_tree_t *v = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *h = v != NULL ? flow_hit_in_flow(v, 5, 10) : NULL;
        expect("positioned door: in the flow, a relative box's text is found",
               h != NULL && h->kind == FLOW_BOX_TEXT && memcmp(h->text, "moved", 5) == 0, NULL);
        flow_free(v);
        sheeted_close(&q);
    }

    // pointer-events: none (ruling 7): an invisible overlay over the whole
    // page lets a click through to the text under it; a button inside it
    // that asks for the pointer back (`auto`) still takes one; and the
    // property is inherited, so the overlay's other text is passed through.
    {
        Sheeted q;
        sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                         ".veil{position:absolute;top:0;left:0;width:400px;height:100px;"
                         "opacity:0;pointer-events:none} .veil b{pointer-events:auto}</style>"
                         "<p>under the veil</p><div class=veil>veiled <b>button</b></div>",
                     400, 600);
        flow_tree_t *v = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *h = v != NULL ? flow_hit(v, 20, 8, kNoScroll) : NULL;
        expect("positioned door: pointer-events: none lets the click through",
               h != NULL && h->kind == FLOW_BOX_TEXT && memcmp(h->text, "under", 5) == 0, NULL);
        // "veiled " is 7 x 8 less a narrow 'l' and 'i': 8+8+4+4+8+8+4 = 44,
        // so "button" starts at 44.
        h = v != NULL ? flow_hit(v, 50, 8, kNoScroll) : NULL;
        expect("positioned door: a child that sets pointer-events: auto is hit",
               h != NULL && h->kind == FLOW_BOX_TEXT && memcmp(h->text, "button", 6) == 0, NULL);
        flow_free(v);
        sheeted_close(&q);
    }

    // One viewport height: a cascade judged at another is refused.
    p.env.viewport_height = 599;
    expect("positioned door: a cascade judged at another height is refused",
           flow_layout(p.doc, p.page, 400, &p.env) == NULL, NULL);
    sheeted_close(&p);
}

// Fixed boxes (P2): their coordinates are the viewport's, and the door
// moves them by the scroll. A header 30 high across the top, a field 100
// down the page, and a thousand pixels of page after it.
static void position_fixed_door_cases(void)
{
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0} .gap{height:100px}"
                     ".long{height:1000px} .h{position:fixed;top:0;left:0;right:0;height:30px;"
                     "background:silver}</style><div class=h></div><p class=gap></p>"
                     "<p><input name=q></p><p class=long>page</p>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *h = t != NULL ? flow_positioned(t, 0) : NULL;
    const flow_box_t *field = t != NULL ? flow_control(t, 0) : NULL;
    expect("fixed door: the header is fixed and listed",
           h != NULL && h->fixed && h->positioned && h->rect.y == 0 && h->rect.h == 30, NULL);
    expect("fixed door: the page is as tall as its flow, no taller for the header",
           t != NULL && flow_height(t) == flow_root(t)->rect.h, NULL);
    flow_point_t top = {0, 0}, down = {0, 500}, near = {0, 90};
    os64_gui_rect_t r = flow_box_doc_rect(h, down);
    expect("fixed door: its document rect is its rect moved by the scroll",
           r.x == 0 && r.y == 500 && r.w == 400 && r.h == 30, NULL);
    r = flow_box_doc_rect(field, down);
    expect("fixed door: an in-flow box's document rect is its rect",
           field != NULL && r.y == field->rect.y, NULL);

    // Scrolled 500 down, a click 10 below the view's top is on the header;
    // not scrolled, the same document point is the page's.
    const flow_box_t *hit = t != NULL ? flow_hit(t, 20, 510, down) : NULL;
    expect("fixed door: scrolled, a click at the view's top is the header", hit == h, NULL);
    hit = t != NULL ? flow_hit(t, 20, 510, top) : NULL;
    expect("fixed door: not scrolled, the same point is the page", hit != NULL && hit != h, NULL);

    // Painting the view scrolled 500 down hands the header over; the same
    // document rect unscrolled does not, the header being at the top.
    Seen seen = {0};
    flow_visit(t, (os64_gui_rect_t){0, 500, 400, 100}, down, seen_add, &seen);
    bool painted = false;
    for (int i = 0; i < seen.n; i++)
        painted |= seen.v[i] == h;
    expect("fixed door: scrolled, the header is painted in view", painted, NULL);
    Seen none = {0};
    flow_visit(t, (os64_gui_rect_t){0, 500, 400, 100}, top, seen_add, &none);
    painted = false;
    for (int i = 0; i < none.n; i++)
        painted |= none.v[i] == h;
    expect("fixed door: unscrolled, the header is not in that part", !painted, NULL);

    // The field, 100 down, is under the header at a scroll of 90 (the
    // header's document 90..120) and not at the top.
    expect("fixed door: the field is not covered at the top",
           field != NULL && !flow_box_covered(t, field, top), NULL);
    expect("fixed door: the field under the fixed header is covered",
           field != NULL && flow_box_covered(t, field, near), NULL);
    expect("fixed door: the header covers nothing painted after it",
           h != NULL && !flow_box_covered(t, h, near), NULL);
    flow_free(t);
    sheeted_close(&q);

    // Covered is the pointer's question, asked at the control's centre
    // (ruling 9). A hidden overlay over everything, written last, takes no
    // click and covers nothing; a floating label that lets the pointer
    // through, and an icon in the field's corner, leave it usable.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0} .wrap{position:relative}"
                     ".label{position:absolute;left:4px;top:2px;pointer-events:none}"
                     ".icon{position:absolute;right:0;top:0;width:8px;height:8px}"
                     ".veil{position:fixed;inset:0;visibility:hidden}</style>"
                     "<p><a href=x>a link</a></p><p class=wrap><input name=a>"
                     "<span class=label>Email</span></p>"
                     "<p class=wrap style='width:160px'><input name=b><span class=icon></span></p>"
                     "<div class=veil></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    hit = t != NULL ? flow_hit(t, 10, 10, top) : NULL;
    expect("fixed door: a hidden overlay takes no click", hit != NULL && hit->link == 0, NULL);
    const flow_box_t *a = t != NULL ? flow_control(t, 0) : NULL;
    const flow_box_t *b = t != NULL ? flow_control(t, 1) : NULL;
    expect("fixed door: a floating label that lets the pointer through covers nothing",
           a != NULL && !flow_box_covered(t, a, top), NULL);
    expect("fixed door: an icon in the corner does not cover the field's centre",
           b != NULL && !flow_box_covered(t, b, top), NULL);
    flow_free(t);
    sheeted_close(&q);
}

// z-index (P3): the paint order a flow_visit hands over, worked from
// Appendix E, and what a click finds.
static int seen_index(const Seen *s, const flow_box_t *b)
{
    for (int i = 0; i < s->n; i++)
        if (s->v[i] == b)
            return i;
    return -1;
}

static const flow_box_t *box_with_id(const flow_tree_t *t, const os64_html_node_t *n, const char *id)
{
    for (const os64_html_node_t *c = n; c != NULL; c = c->next) {
        const char *v = c->kind == OS64_HTML_ELEMENT && os64_html_attr(c, "id") != NULL
                            ? os64_html_attr(c, "id")->value : NULL;
        if (v != NULL && strcmp(v, id) == 0)
            return flow_box_for(t, c);
        const flow_box_t *in = box_with_id(t, c->first_child, id);
        if (in != NULL)
            return in;
    }
    return NULL;
}

static void position_zindex_cases(void)
{
    flow_point_t top = {0, 0};
    // Four boxes over one corner and a line of text in the flow: D (-1)
    // under the text, then C (auto, tree order), B (1), A (2) — A on top.
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     "div{position:absolute;top:0;left:0;width:100px;height:100px}"
                     "#a{z-index:2} #b{z-index:1} #d{z-index:-1}</style>"
                     "<div id=a>A</div><div id=b>B</div><div id=c>C</div><div id=d>D</div>"
                     "<p id=t>text</p>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const os64_html_node_t *root = q.doc->document;
    const flow_box_t *a = box_with_id(t, root, "a"), *b = box_with_id(t, root, "b");
    const flow_box_t *c = box_with_id(t, root, "c"), *d = box_with_id(t, root, "d");
    const flow_box_t *pt = box_with_id(t, root, "t");
    Seen seen = {0};
    flow_visit(t, (os64_gui_rect_t){0, 0, 400, 600}, top, seen_add, &seen);
    int ia = seen_index(&seen, a), ib = seen_index(&seen, b), ic = seen_index(&seen, c);
    int id = seen_index(&seen, d), ip = seen_index(&seen, pt);
    expect("z-index: -1, the flow, auto, 1, 2 — in that order",
           id >= 0 && ip > id && ic > ip && ib > ic && ia > ib, NULL);
    expect("z-index: the list is in paint order",
           flow_positioned(t, 0) == d && flow_positioned(t, 1) == c &&
               flow_positioned(t, 2) == b && flow_positioned(t, 3) == a, NULL);
    const flow_box_t *h = flow_hit(t, 4, 10, top);
    expect("z-index: a click finds the highest", h != NULL && h->kind == FLOW_BOX_TEXT &&
                                                    h->length == 1 && h->text[0] == 'A', NULL);
    flow_free(t);
    sheeted_close(&q);

    // A context keeps its members inside it: 100 in a context of 1 is
    // under a sibling of 2. A positioned parent with no z-index is no
    // context, and lends it 100 in the root's: it is on top.
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     ".box{position:absolute;top:0;left:0;width:100px;height:100px}"
                     ".kid{position:absolute;z-index:100;top:0;left:0;width:50px;height:50px}"
                     "</style><div class=box style='z-index:1'><div class=kid>K</div></div>"
                     "<div class=box style='z-index:2'>P</div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    h = t != NULL ? flow_hit(t, 4, 10, top) : NULL;
    expect("z-index: a member stays inside its context", h != NULL && h->kind == FLOW_BOX_TEXT &&
                                                             h->text[0] == 'P', NULL);
    flow_free(t);
    sheeted_close(&q);
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     ".box{position:absolute;top:0;left:0;width:100px;height:100px}"
                     ".kid{position:absolute;z-index:100;top:0;left:0;width:50px;height:50px}"
                     "</style><div class=box><div class=kid>K</div></div>"
                     "<div class=box style='z-index:2'>P</div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    h = t != NULL ? flow_hit(t, 4, 10, top) : NULL;
    expect("z-index: a parent that is no context lends its members to the root's",
           h != NULL && h->kind == FLOW_BOX_TEXT && h->text[0] == 'K', NULL);
    flow_free(t);
    sheeted_close(&q);

    // A form control made block-level — by positioning, or `display: block`
    // from a sheet — is still a control: listed, and its box says which.
    {
        char *got = cascade_layout_of("<!doctype html><style>body{margin:0}"
                                      ".b{display:block;width:100px}"
                                      ".a{position:absolute;top:50px;left:0}</style>"
                                      "<input name=x class=b><input name=y class=a>", 400);
        expect("a block-level control is a control, by display or by position",
               got != NULL && strstr(got, "replaced input 0 0 100 20 control 0\n") != NULL &&
                   strstr(got, " control 1 positioned\n") != NULL, got);
        free(got);
    }

    // Fable's first carry-in: a custom checkbox whose real input is raised
    // over its stand-in by z-index keeps its widget: the pointer reaches
    // the input at its centre. In tree order the stand-in, written after
    // it, would answer there.
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     "label{position:relative;display:inline-block;width:20px;height:20px}"
                     "input{position:absolute;z-index:1;opacity:0;top:0;left:0;margin:0;"
                     "width:20px;height:20px} .box{position:absolute;top:0;left:0;width:20px;"
                     "height:20px;background:silver}</style>"
                     "<label><input type=checkbox name=c><span class=box></span></label>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *in = t != NULL ? flow_control(t, 0) : NULL;
    expect("z-index: a raised invisible input keeps its widget",
           in != NULL && !flow_box_covered(t, in, top), NULL);
    flow_free(t);
    sheeted_close(&q);

    // And the second: a fixed header written first with a z-index stays
    // over the page's content in a relative wrapper written after it,
    // which tree order would have painted over it.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     ".h{position:fixed;top:0;left:0;right:0;height:30px;z-index:10}"
                     ".wrap{position:relative} .long{height:1000px}</style>"
                     "<div class=h>header</div><div class=wrap><p class=long>content</p></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    h = t != NULL ? flow_hit(t, 4, 510, (flow_point_t){0, 500}) : NULL;
    expect("z-index: a raised fixed header is over a relative wrapper written after it",
           h != NULL && h->fixed, NULL);
    flow_free(t);
    sheeted_close(&q);
}

// Sticky boxes (P4): where the flow put them, drawn moved by the scroll
// until their containing block runs out (CSS Position 3 § 3.4).
static bool inside_of(const flow_box_t *b, const flow_box_t *outer)
{
    while (b != NULL && b != outer)
        b = b->parent;
    return b != NULL;
}

static bool seen_has(const Seen *s, const flow_box_t *b)
{
    return seen_index(s, b) >= 0;
}

static void position_sticky_cases(void)
{
    // A header 30 high at 100, in a wrapper 100..900: it stays put until
    // the page scrolls past 100, then rides the view's top, and lets go
    // 30 above the wrapper's end, at 870 — 770 below where it began.
    Sheeted q;
    const char *page =
        "<!doctype html><style>body{margin:0} p{margin:0} .gap{height:100px}"
        ".wrap{height:800px} .s{position:sticky;top:0;height:30px} .long{height:1000px}</style>"
        "<p class=gap></p><div class=wrap id=w><div class=s id=s>head</div>"
        "<p><input name=q></p></div><p class=long></p>";
    sheeted_open(&q, page, 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const os64_html_node_t *root = q.doc->document;
    const flow_box_t *s = box_with_id(t, root, "s"), *w = box_with_id(t, root, "w");
    const flow_box_t *field = t != NULL ? flow_control(t, 0) : NULL;
    expect("sticky: the header is positioned and sticky, where the flow put it",
           s != NULL && s->positioned && s->frame != NULL && s->frame->box == s &&
               s->rect.y == 100 && s->rect.h == 30 && !s->fixed, NULL);
    expect("sticky: what is inside it moves with it",
           s != NULL && s->first != NULL && s->first->frame == s->frame, NULL);
    struct { int32_t scroll, y; } at[] = {{0, 100}, {50, 100}, {100, 100}, {300, 300},
                                          {870, 870}, {1000, 870}};
    bool rides = s != NULL;
    for (size_t k = 0; rides && k < sizeof(at) / sizeof(at[0]); k++)
        rides = flow_box_doc_rect(s, (flow_point_t){0, at[k].scroll}).y == at[k].y;
    expect("sticky: stays, rides the view's top, lets go at its wrapper's end", rides, NULL);
    const flow_box_t *text = s != NULL ? s->first : NULL;
    while (text != NULL && text->kind != FLOW_BOX_TEXT)
        text = text->first;
    expect("sticky: its text is drawn moved as far",
           text != NULL && flow_box_doc_rect(text, (flow_point_t){0, 300}).y ==
                               text->rect.y + 200, NULL);
    expect("sticky: an in-flow box beside it does not move",
           w != NULL && flow_box_doc_rect(w, (flow_point_t){0, 300}).y == 100, NULL);

    flow_point_t top = {0, 0}, down = {0, 300};
    const flow_box_t *h = t != NULL ? flow_hit(t, 10, 310, down) : NULL;
    expect("sticky: scrolled, a click at the view's top is the header", inside_of(h, s), NULL);
    h = t != NULL ? flow_hit(t, 10, 310, top) : NULL;
    expect("sticky: not scrolled, the same point is the wrapper", h == w, NULL);
    Seen seen = {0}, none = {0};
    flow_visit(t, (os64_gui_rect_t){0, 300, 400, 100}, down, seen_add, &seen);
    flow_visit(t, (os64_gui_rect_t){0, 300, 400, 100}, top, seen_add, &none);
    expect("sticky: scrolled, the header is painted in view, and not unscrolled",
           seen_has(&seen, s) && !seen_has(&none, s), NULL);
    flow_point_t over = {0, field != NULL ? field->rect.y - 10 : 0};
    expect("sticky: the field is not covered unscrolled, and is under the stuck header",
           field != NULL && !flow_box_covered(t, field, top) &&
               flow_box_covered(t, field, over), NULL);
    flow_free(t);
    sheeted_close(&q);
    {
        char *got = cascade_layout_of(page, 400);
        expect("sticky: the dump says how far it may move",
               got != NULL && strstr(got, " positioned sticky x 0..0 y 0..770\n") != NULL, got);
        free(got);
    }

    // `bottom: 0` on a footer at 1000 in a page 2000 tall: pushed up to the
    // view's foot while it is below it, as far up as the body's top.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0} .top{height:1000px}"
                     ".f{position:sticky;bottom:0;height:20px} .after{height:980px}</style>"
                     "<p class=top></p><div class=f id=f>foot</div><p class=after></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *f = box_with_id(t, q.doc->document, "f");
    expect("sticky: a bottom one is held at the view's foot, and let go in view",
           f != NULL && f->frame != NULL && f->frame->lo[1] == -1000 &&
               f->frame->hi[1] == 980 &&
               flow_box_doc_rect(f, top).y == 580 &&
               flow_box_doc_rect(f, (flow_point_t){0, 500}).y == 1000 &&
               flow_box_doc_rect(f, (flow_point_t){0, 1400}).y == 1000, NULL);
    flow_free(t);
    sheeted_close(&q);

    // Inside a scroll container its scrollport is the container's, which
    // nothing scrolls: `top: 50px` pushes it 50 down at every scroll of the
    // page, and the page's scroll moves it no further.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} .box{overflow:auto;height:200px}"
                     ".s{position:sticky;top:50px;height:30px} .long{height:1000px}"
                     ".page{height:2000px}</style><div class=box id=b><div class=s id=s>s</div>"
                     "<div class=long></div></div><div class=page></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    s = box_with_id(t, q.doc->document, "s");
    const flow_box_t *b = box_with_id(t, q.doc->document, "b");
    expect("sticky: in a scroll container, it sticks to the container, not the view",
           s != NULL && s->frame != NULL && s->frame->port == b &&
               flow_box_doc_rect(s, top).y == 50 &&
               flow_box_doc_rect(s, (flow_point_t){0, 500}).y == 50, NULL);
    flow_free(t);
    sheeted_close(&q);

    // A table's heading cell rides the view until the table ends: its
    // containing block is the table.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} table{border-spacing:0}"
                     "th,td{padding:0} th{position:sticky;top:0;height:20px}"
                     "td{height:1000px}</style><table id=t><thead><tr><th id=h>h</th></tr>"
                     "</thead><tbody><tr><td>x</td></tr></tbody></table>"
                     "<div style='height:1000px'></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *th = box_with_id(t, q.doc->document, "h");
    const flow_box_t *tb = box_with_id(t, q.doc->document, "t");
    int32_t end = tb != NULL && th != NULL ? tb->rect.y + tb->rect.h - th->rect.h : -1;
    expect("sticky: a heading cell rides the view until its table ends",
           th != NULL && th->kind == FLOW_BOX_CELL && th->frame != NULL &&
               flow_box_doc_rect(th, top).y == th->rect.y &&
               flow_box_doc_rect(th, (flow_point_t){0, 500}).y == 500 &&
               flow_box_doc_rect(th, (flow_point_t){0, 1500}).y == end, NULL);
    flow_free(t);
    sheeted_close(&q);

    // What is positioned inside it: an absolute badge has the sticky box
    // for its containing block and moves with it; a fixed one is the
    // viewport's and does not; a sticky box inside it is worked out in the
    // frame the outer one moved: it rides with it, and once the outer one
    // has let go at 800 the inner one is pushed on to the outer box's own
    // end, 800 + 100 - 20.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0} .gap{height:100px}"
                     ".wrap{height:800px} .s{position:sticky;top:0;height:100px}"
                     ".in{position:sticky;top:0;height:20px}"
                     ".a{position:absolute;top:0;right:0;width:10px;height:10px}"
                     ".f{position:fixed;top:50px;left:0;width:10px;height:10px}"
                     ".long{height:1000px}</style><p class=gap></p><div class=wrap>"
                     "<div class=s id=s><div class=in id=i>in</div><span class=a id=a></span>"
                     "<span class=f id=f></span></div></div><p class=long></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    root = q.doc->document;
    s = box_with_id(t, root, "s");
    const flow_box_t *in = box_with_id(t, root, "i"), *a = box_with_id(t, root, "a");
    f = box_with_id(t, root, "f");
    expect("sticky: an absolute box in it moves with it",
           a != NULL && s != NULL && a->frame == s->frame &&
               flow_box_doc_rect(a, down).y == 300, NULL);
    expect("sticky: a fixed box in it is the viewport's",
           f != NULL && f->frame == NULL && f->fixed && flow_box_doc_rect(f, down).y == 350,
           NULL);
    expect("sticky: a sticky box in it rides with it, then to the outer box's end",
           in != NULL && in->frame != NULL && in->frame->outer == s->frame &&
               flow_box_doc_rect(in, down).y == 300 &&
               flow_box_doc_rect(in, (flow_point_t){0, 1000}).y == 880, NULL);
    flow_free(t);

    // Positioning off: laid out, and drawn, where the flow puts it.
    q.env.static_only = true;
    t = flow_layout(q.doc, q.page, 400, &q.env);
    s = box_with_id(t, root, "s");
    expect("sticky: positioning off, it does not stick",
           s != NULL && !s->positioned && s->frame == NULL &&
               flow_box_doc_rect(s, down).y == 100, NULL);
    flow_free(t);
    sheeted_close(&q);

    // Quinn's review, round 1. A box taller than its view: `bottom: 0` on
    // 800 pixels after a 1000-pixel spacer, 600 high. The view gives way
    // at its end, so the box shows its top: scrolled 500, at 500, not with
    // its bottom at the view's foot (300); not scrolled, at the top.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0} .top{height:1000px}"
                     ".f{position:sticky;bottom:0;height:800px} .after{height:1000px}</style>"
                     "<p class=top></p><div class=f id=f>tall</div><p class=after></p>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    f = box_with_id(t, q.doc->document, "f");
    expect("sticky: one taller than its view shows its start",
           f != NULL && flow_box_doc_rect(f, (flow_point_t){0, 500}).y == 500 &&
               flow_box_doc_rect(f, top).y == 0, NULL);
    flow_free(t);
    sheeted_close(&q);

    // What clips a sticky box from outside stays put: an `overflow: clip`
    // box 100 high holds a wrapper 1000 high and a header in it. Scrolled
    // 300, the header is pushed to 300, but the clip is still 0..100, so
    // nothing of the header is drawn or clicked there.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} .c{overflow:clip;height:100px}"
                     ".w{height:1000px} .s{position:sticky;top:0;height:30px}"
                     ".page{height:2000px}</style><div class=c><div class=w>"
                     "<div class=s id=s>head</div></div></div><div class=page></div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    s = box_with_id(t, q.doc->document, "s");
    os64_gui_rect_t c = s != NULL ? flow_box_doc_clip(s, down) : (os64_gui_rect_t){0, 0, 0, 0};
    h = t != NULL ? flow_hit(t, 10, 310, down) : NULL;
    const flow_box_t *h0 = t != NULL ? flow_hit(t, 10, 10, top) : NULL;
    expect("sticky: a clip from outside does not move with it",
           s != NULL && s->clipped && flow_box_doc_rect(s, down).y == 300 && c.y == 0 &&
               c.h == 100 && !inside_of(h, s) && inside_of(h0, s), NULL);
    flow_free(t);
    sheeted_close(&q);

    // Sticky boxes nested deep, each a scroll container too: every one's
    // push is worked from its outer one's frame and its scrollport's, the
    // same box. A lookup costs each once, and past F_FRAME_DEPS frames —
    // two a level here, the push and the scroll — they are laid out where
    // the flow put them.
    {
        char html[8192];
        size_t n = (size_t)snprintf(html, sizeof(html),
                                    "<!doctype html><style>body{margin:0} div{position:sticky;"
                                    "top:0;overflow:auto;height:100px}</style>");
        for (int k = 0; k < 60; k++)
            n += (size_t)snprintf(html + n, sizeof(html) - n, "<div id=d%d>", k);
        snprintf(html + n, sizeof(html) - n, "deep");
        sheeted_open(&q, html, 400, 600);
        t = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *d0 = box_with_id(t, q.doc->document, "d0");
        const flow_box_t *d10 = box_with_id(t, q.doc->document, "d10");
        const flow_box_t *d59 = box_with_id(t, q.doc->document, "d59");
        expect("sticky: nested deep, the near ones move and the deep ones stand",
               d0 != NULL && d0->frame->moves && d10 != NULL && d10->frame->moves &&
                   d59 != NULL && !d59->frame->moves &&
                   flow_box_doc_rect(d59, down).y == d59->rect.y, NULL);
        flow_free(t);
        sheeted_close(&q);
    }
}

static void position_cases(void)
{
    position_style_cases();
    position_layout_cases();
    position_door_cases();
    position_fixed_door_cases();
    position_zindex_cases();
    position_sticky_cases();
}
