// The legacy box and its line clamp (flow_style_t.legacy_box): a
// `display: -webkit-box` laid out as the flex container every engine reads
// it as (Compatibility § 3.4.4-5), and a vertical one with a
// `-webkit-line-clamp` laid out as a block that ends at its last shown
// line, that line ending in its block's ellipsis (flow_box_t.line_clamp_end,
// flow_line_cut) and the rest overflowing, scrollable. Every number is
// worked by hand from the monospace test face: 16px, every glyph 8 wide,
// lines 20 tall, the ellipsis one glyph.

#define CLAMP_STYLE "<!doctype html><style>body{margin:0} p{margin:0}" \
    " div{width:80px;font:16px monospace;line-height:20px}" \
    " .c{display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:2;overflow:hidden}"

// The lines of `b`'s own flow, in order, as far down as they go, each
// "y+h" and a "*" where it is the clamp's end: "0+20 20+20*".
static void clamp_lines_of(const flow_box_t *b, char *out, size_t cap, size_t *n)
{
    for (const flow_box_t *c = b != NULL ? b->first : NULL; c != NULL; c = c->next) {
        if (c->kind == FLOW_BOX_LINE)
            *n += (size_t)snprintf(out + *n, *n < cap ? cap - *n : 0, "%s%d+%d%s",
                                   *n > 0 ? " " : "", (int)c->rect.y, (int)c->rect.h,
                                   c->line_clamp_end ? "*" : "");
        else if (c->kind == FLOW_BOX_BLOCK)
            clamp_lines_of(c, out, cap, n);
        if (*n >= cap)
            return;
    }
}

static void clamp_lines(const flow_box_t *b, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = '\0';
    clamp_lines_of(b, out, cap, &n);
}

// The clamp's end line under `b`, or NULL.
static const flow_box_t *clamp_end_line(const flow_box_t *b)
{
    for (const flow_box_t *c = b != NULL ? b->first : NULL; c != NULL; c = c->next) {
        if (c->kind == FLOW_BOX_LINE && c->line_clamp_end)
            return c;
        const flow_box_t *in = c->kind == FLOW_BOX_BLOCK ? clamp_end_line(c) : NULL;
        if (in != NULL)
            return in;
    }
    return NULL;
}

static void clamp_layout_cases(void)
{
    Sheeted q;
    sheeted_open(&q, CLAMP_STYLE
                     " .one{-webkit-line-clamp:1} .row{-webkit-box-orient:horizontal}"
                     " .pad{padding-bottom:5px}</style>"
                     // Three lines: "aaaa bbbb", "cccc dddd", "eeee".
                     "<div class=c id=three>aaaa bbbb cccc dddd eeee</div>"
                     // One line: nothing to clamp.
                     "<div class=c id=short>aaaa bbbb</div>"
                     // Two paragraphs: the clamp counts into them.
                     "<div class=c id=nested><p>aaaa bbbb cccc</p><p id=after>dddd</p></div>"
                     // A full first line, clamped at 1: its ellipsis cuts it.
                     "<div class='c one' id=full>aaaaaaaaaa bbbb</div>"
                     // Horizontal: a flex row, which clamps nothing.
                     "<div class='c row' id=row>aaaa bbbb cccc dddd eeee</div>"
                     // Padding below the last shown line is the box's own.
                     "<div class='c pad' id=pad>aaaa bbbb cccc dddd eeee</div>"
                     // Exactly as many lines as the clamp: no ellipsis.
                     "<div class=c id=two>aaaa bbbb cccc</div>"
                     // A block, a picture made a block and a nested
                     // context after the second line are content too.
                     "<div class=c id=blk>aaaa bbbb cccc<div style=height:100px></div></div>"
                     "<div class=c id=img>aaaa bbbb cccc"
                     "<img style=display:block;width:10px;height:100px></div>"
                     "<div class=c id=ctx>aaaa bbbb cccc"
                     "<div style=display:flow-root;height:30px></div></div>"
                     // An empty block after them is nothing to show.
                     "<div class=c id=empty>aaaa bbbb cccc<div></div></div>",
                 300, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 300, &q.env);
    const os64_html_node_t *d = q.doc->document;
    char lines[256];

    const flow_box_t *three = box_with_id(t, d, "three");
    clamp_lines(three, lines, sizeof(lines));
    expect("line clamp: the box ends at its second line", three != NULL && three->rect.h == 40,
           lines);
    expect("line clamp: the second line is its end, the third laid out below",
           strcmp(lines, "0+20 20+20* 40+20") == 0, lines);
    const flow_box_t *end = clamp_end_line(three);
    flow_line_cut_t cut = end != NULL ? flow_line_cut(t, end) : (flow_line_cut_t){false, 0, 0};
    // "cccc dddd" ends at 72, the ellipsis fits before 80: nothing cut.
    expect("line clamp: the ellipsis follows the content where it fits",
           cut.on && cut.at == 72 && cut.limit == 72, NULL);
    expect("line clamp: the box holds an ellipsis for it", three->ellipsis != NULL, NULL);
    const flow_box_t *first = three != NULL ? first_of_kind(three, FLOW_BOX_LINE) : NULL;
    flow_line_cut_t none = first != NULL ? flow_line_cut(t, first) : (flow_line_cut_t){true, 0, 0};
    expect("line clamp: a line before the end has no ellipsis (no text-overflow asked)",
           !none.on, NULL);
    int32_t sc = three != NULL ? three->scroller : -1;
    flow_point_t range = sc >= 0 ? flow_scroll_range(t, sc) : (flow_point_t){0, 0};
    expect("line clamp: the hidden line is scrollable overflow (scrollHeight 60)",
           sc >= 0 && range.y == 20, NULL);

    const flow_box_t *shortb = box_with_id(t, d, "short");
    expect("line clamp: one line, nothing clamped, no end",
           shortb != NULL && shortb->rect.h == 20 && clamp_end_line(shortb) == NULL, NULL);

    const flow_box_t *nested = box_with_id(t, d, "nested");
    clamp_lines(nested, lines, sizeof(lines));
    // Below #three (40) and #short (20): page coordinates start at 60.
    expect("line clamp: lines are counted into the paragraphs inside",
           nested != NULL && nested->rect.h == 40 && strcmp(lines, "60+20 80+20* 100+20") == 0,
           lines);
    const flow_box_t *nend = clamp_end_line(nested);
    expect("line clamp: the end's block is the first paragraph, which holds the ellipsis",
           nend != NULL && nend->parent != NULL && nend->parent->ellipsis != NULL &&
               nend->parent->node != NULL && nend->parent->node->tag == OS64_HTML_TAG_P,
           NULL);

    const flow_box_t *full = box_with_id(t, d, "full");
    const flow_box_t *fend = clamp_end_line(full);
    cut = fend != NULL ? flow_line_cut(t, fend) : (flow_line_cut_t){false, 0, 0};
    expect("line clamp: a full last line is cut to make room, nine glyphs kept",
           full != NULL && full->rect.h == 20 && cut.on && cut.limit == 72 && cut.at == 72, NULL);

    const flow_box_t *row = box_with_id(t, d, "row");
    expect("line clamp: a horizontal legacy box clamps nothing",
           row != NULL && clamp_end_line(row) == NULL && row->ellipsis == NULL, NULL);

    const flow_box_t *pad = box_with_id(t, d, "pad");
    expect("line clamp: the box's own padding follows its last shown line",
           pad != NULL && pad->rect.h == 45, NULL);

    const flow_box_t *two = box_with_id(t, d, "two");
    expect("line clamp: as many lines as the clamp, nothing follows, no end",
           two != NULL && two->rect.h == 40 && clamp_end_line(two) == NULL, NULL);

    static const char *const kFollowed[] = {"blk", "img", "ctx"};
    for (size_t k = 0; k < sizeof(kFollowed) / sizeof(kFollowed[0]); k++) {
        const flow_box_t *f = box_with_id(t, d, kFollowed[k]);
        char what[96];
        snprintf(what, sizeof(what), "line clamp: a box after the last line is content (%s)",
                 kFollowed[k]);
        expect(what, f != NULL && f->rect.h == 40 && clamp_end_line(f) != NULL &&
                         f->scroller >= 0 && flow_scroll_range(t, f->scroller).y > 0, NULL);
    }
    const flow_box_t *empty = box_with_id(t, d, "empty");
    expect("line clamp: an empty block after the last line is nothing to show",
           empty != NULL && empty->rect.h == 40 && clamp_end_line(empty) == NULL, NULL);
    flow_free(t);
    sheeted_close(&q);
}

// The legacy box as a flex container: a 200-wide row of two items, the
// first 50 wide unless it grows, the second 50.
static void legacy_flex_cases(void)
{
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     " .b{display:-webkit-box;width:200px} i,b{display:block;height:10px;width:50px}"
                     " .grow{-webkit-box-flex:1} .end{-webkit-box-pack:end}"
                     " .center{-webkit-box-pack:center} .justify{-webkit-box-pack:justify}"
                     " .v{-webkit-box-orient:vertical} .rev{-webkit-box-direction:reverse}"
                     " .late{-webkit-box-ordinal-group:2}"
                     " .flexy{flex:0 0 120px}</style>"
                     "<div class=b><i id=g class=grow></i><b id=g2></b></div>"
                     "<div class='b end'><i id=e></i><b></b></div>"
                     "<div class='b center'><i id=m></i><b></b></div>"
                     "<div class='b justify'><i></i><b id=j></b></div>"
                     "<div class='b v'><i></i><b id=v></b></div>"
                     "<div class='b rev'><i id=r></i><b></b></div>"
                     "<div class=b><i id=o class=late></i><b id=o2></b></div>"
                     "<div class=b><i id=x class=flexy></i><b></b></div>"
                     // The direction is inherited: the inner box runs
                     // backwards too.
                     "<div class='b rev'><div class=b><i id=ri></i><b></b></div></div>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const os64_html_node_t *d = q.doc->document;
    const flow_box_t *g = box_with_id(t, d, "g"), *g2 = box_with_id(t, d, "g2");
    expect("legacy box: -webkit-box-flex grows an item into the free space",
           g != NULL && g2 != NULL && g->rect.w == 150 && g2->rect.x == 150, NULL);
    const flow_box_t *e = box_with_id(t, d, "e");
    expect("legacy box: -webkit-box-pack end", e != NULL && e->rect.x == 100, NULL);
    const flow_box_t *m = box_with_id(t, d, "m");
    expect("legacy box: -webkit-box-pack center", m != NULL && m->rect.x == 50, NULL);
    const flow_box_t *j = box_with_id(t, d, "j");
    expect("legacy box: -webkit-box-pack justify", j != NULL && j->rect.x == 150, NULL);
    const flow_box_t *v = box_with_id(t, d, "v");
    expect("legacy box: vertical stacks its items",
           v != NULL && v->rect.x == 0 && v->parent != NULL && v->rect.y == v->parent->rect.y + 10,
           NULL);
    const flow_box_t *r = box_with_id(t, d, "r");
    expect("legacy box: reverse runs the row backwards", r != NULL && r->rect.x == 150, NULL);
    const flow_box_t *o = box_with_id(t, d, "o"), *o2 = box_with_id(t, d, "o2");
    expect("legacy box: -webkit-box-ordinal-group places an item after its siblings",
           o != NULL && o2 != NULL && o2->rect.x == 0 && o->rect.x == 50, NULL);
    const flow_box_t *x = box_with_id(t, d, "x");
    expect("legacy box: an item's own flex properties are not read",
           x != NULL && x->rect.w == 50, NULL);
    const flow_box_t *ri = box_with_id(t, d, "ri");
    expect("legacy box: -webkit-box-direction is inherited", ri != NULL && ri->rect.x == 150,
           NULL);
    flow_free(t);
    sheeted_close(&q);
}

static void clamp_computed_cases(void)
{
    Sheeted p;
    sheeted_open(&p, CLAMP_STYLE " .i{display:-webkit-inline-box} .f{display:-webkit-flex}"
                     " .k{-webkit-box-pack:justify;-webkit-box-align:center;-webkit-box-flex:2.5;"
                     "-webkit-box-ordinal-group:3}</style>"
                     "<div class=c id=c>a</div><div id=i class=i>b</div><div id=f class=f>c</div>"
                     "<div id=k class=k>d</div><div id=plain>e</div>",
                 300, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 300, &p.env);
    const os64_html_node_t *root = p.doc->document;
    flow_computed_t *c = flow_computed_new(tree, &p.env, computed_find(root, "c"), NULL);
    computed_is(c, "legacy box: a clamping one is still -webkit-box", GARB_DISPLAY, "-webkit-box");
    computed_is(c, "legacy box: its orient", GARB_BOX_ORIENT, "vertical");
    computed_is(c, "legacy box: its clamp", GARB_LINE_CLAMP, "2");
    flow_computed_free(c);
    c = flow_computed_new(tree, &p.env, computed_find(root, "i"), NULL);
    computed_is(c, "legacy box: the inline one", GARB_DISPLAY, "-webkit-inline-box");
    flow_computed_free(c);
    c = flow_computed_new(tree, &p.env, computed_find(root, "f"), NULL);
    computed_is(c, "legacy box: -webkit-flex is flex", GARB_DISPLAY, "flex");
    flow_computed_free(c);
    c = flow_computed_new(tree, &p.env, computed_find(root, "k"), NULL);
    computed_is(c, "legacy box: pack", GARB_BOX_PACK, "justify");
    computed_is(c, "legacy box: align", GARB_BOX_ALIGN, "center");
    computed_is(c, "legacy box: flex", GARB_BOX_FLEX, "2.5");
    computed_is(c, "legacy box: ordinal group", GARB_BOX_ORDINAL_GROUP, "3");
    flow_computed_free(c);
    c = flow_computed_new(tree, &p.env, computed_find(root, "plain"), NULL);
    computed_is(c, "legacy box: initial orient", GARB_BOX_ORIENT, "horizontal");
    computed_is(c, "legacy box: initial align", GARB_BOX_ALIGN, "stretch");
    computed_is(c, "legacy box: initial ordinal group", GARB_BOX_ORDINAL_GROUP, "1");
    computed_is(c, "legacy box: initial clamp", GARB_LINE_CLAMP, "none");
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);
}

static void clamp_cases(void)
{
    clamp_layout_cases();
    legacy_flex_cases();
    clamp_computed_cases();
}
