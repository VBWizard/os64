// The Modern Web arc's slice 4, CSS batch A (MODERN_WEB_BRIEFS.md § Slice
// 4): each property computed from an author sheet and, where it moves
// anything, laid out — every dump worked from the specification by hand,
// as test_libflow_cascade.inc's are. The viewport is WIDTH x 600.

// The first box of an element with tag `tag` and kind `kind`, in tree order.
static const flow_box_t *batch_a_box(const flow_box_t *b, os64_html_tag_t tag, flow_box_kind_t kind)
{
    if (b == NULL)
        return NULL;
    if (b->kind == kind && b->node != NULL && b->node->kind == OS64_HTML_ELEMENT &&
        b->node->tag == tag)
        return b;
    for (const flow_box_t *c = b->first; c != NULL; c = c->next) {
        const flow_box_t *found = batch_a_box(c, tag, kind);
        if (found != NULL)
            return found;
    }
    return NULL;
}

// Where an outline is drawn, and the ink it adds (flow_outline_rect, the
// box's ink): the div's border box 0 0 20 10, grown by its offset and its
// width, 1 + 2, on every side: -3 -3 26 16, reaching 3 past the box on
// every side. Its ink is measured past its overflow, which its span's 20
// tall line takes 10 below it: 3 above, right and left, none below. The
// span is an inline box's piece, which draws none; a negative offset past
// the box's middle draws none either (an empty rect).
static void outline_door_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>body { margin: 0 } div { width: 20px; height: 10px;"
                     " outline: 2px solid red; outline-offset: 1px } span { outline: 1px solid }"
                     " p { margin: 0; width: 10px; height: 4px; outline: 1px solid;"
                     " outline-offset: -4px }</style><div><span>x</span></div><p></p>",
                 100, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 100, &p.env);
    const flow_box_t *root = tree != NULL ? flow_root(tree) : NULL;
    const flow_box_t *div = batch_a_box(root, OS64_HTML_TAG_DIV, FLOW_BOX_BLOCK);
    const flow_box_t *span = batch_a_box(root, OS64_HTML_TAG_SPAN, FLOW_BOX_SPAN);
    const flow_box_t *para = batch_a_box(root, OS64_HTML_TAG_P, FLOW_BOX_BLOCK);
    os64_gui_rect_t r = {0, 0, 0, 0};
    expect("outline: drawn round the border box, grown by its offset and its width",
           div != NULL && flow_outline_rect(div, &r) && r.x == -3 && r.y == -3 && r.w == 26 &&
               r.h == 16, NULL);
    char ink[64];
    snprintf(ink, sizeof(ink), "%d %d %d %d", div ? div->ink[0] : -1, div ? div->ink[1] : -1,
             div ? div->ink[2] : -1, div ? div->ink[3] : -1);
    expect("outline: in the box's ink, past its overflow", strcmp(ink, "3 3 0 3") == 0, ink);
    expect("outline: an inline box's piece draws none (booked)",
           span != NULL && !flow_outline_rect(span, &r), NULL);
    // p: 10 x 4 at y 30 (after the div's 10 and its 20 tall line), less 4
    // a side and plus 1: -3 across a 4px height is no rect.
    expect("outline: an offset that turns the ring inside out draws none",
           para != NULL && !flow_outline_rect(para, &r), NULL);
    flow_free(tree);
    sheeted_close(&p);
}

static void batch_a_cases(void)
{
    // cursor is inherited, the keyword after the pictures kept; user-select
    // is not: the p inside the div is auto again, and prints nothing.
    cascade_case("cursor and user-select", 800,
        "<!doctype html><style>div { cursor: pointer; user-select: none }"
        " span { cursor: url(hand.cur) 2 3, url(b.png), text }</style>"
        "<div>a<span>b</span><p>c</p></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block cursor=pointer user-select=none holds-block\n"
        "      span inline cursor=text\n"
        "      p block margin=16/0/16/0\n");
    // `inherit` takes the parent's, `initial` is auto.
    cascade_case("cursor and user-select: the CSS-wide keywords", 800,
        "<!doctype html><style>body { cursor: wait; user-select: all }"
        " p { user-select: inherit } em { cursor: initial }</style><p>a<em>b</em>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 cursor=wait user-select=all holds-block\n"
        "    p block margin=16/0/16/0 user-select=all\n"
        "      em inline italic cursor=auto\n");

    // With the switch off neither is a property at all.
    s_modern_off = true;
    cascade_case("cursor and user-select: unknown with the Modern Web switch off", 800,
        "<!doctype html><style>div { cursor: pointer; user-select: none }</style><div>a</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block\n");
    s_modern_off = false;

    // ── word-break and overflow-wrap (CSS Text 3 § 5.1, § 5.4) ──

    // Both inherited; word-wrap is overflow-wrap's other name.
    cascade_case("word-break and overflow-wrap are inherited", 800,
        "<!doctype html><style>div { word-break: break-all; word-wrap: anywhere }"
        " p { word-break: keep-all }</style><div><p>a</p><span>b</span></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block word-break=break-all overflow-wrap=anywhere holds-block\n"
        "      p block margin=16/0/16/0 word-break=keep-all\n"
        "      span inline\n");

    // The layouts: 16px serif, a-e, g and h 8 wide, f i j 4, k 8, l 4, m
    // 12, a space 4; each div 40 wide. abcde is 40 and fills a line;
    // fghij is 28.
    // break-all: an opportunity between every two characters, so the
    // first line takes what fits, abcde, and f starts the next.
    cascade_layout_case("word-break: break-all breaks between any two characters",
        "<!doctype html><style>body { margin: 0 } div { width: 40px; word-break: break-all }"
        "</style><div>abcdefghij</div>", 200,
        "page 200 40\n"
        "block html 0 0 200 40\n"
        "  block body 0 0 200 40\n"
        "    block div 0 0 40 40\n"
        "      line 0 0 40 20 base 14\n"
        "        text \"abcde\" 0 2 40 16 serif 16\n"
        "      line 0 20 40 20 base 14\n"
        "        text \"fghij\" 0 22 28 16 serif 16\n");
    // anywhere: `ab ` breaks at its space as ever (the line has an
    // opportunity), then the word that is wider than any line is cut
    // where it would overflow. Text that does not wrap is not cut.
    cascade_layout_case("overflow-wrap: anywhere cuts only what would overflow",
        "<!doctype html><style>body { margin: 0 } div { width: 40px }</style>"
        "<div style='overflow-wrap: anywhere'>ab abcdefghij</div>"
        "<div style='word-break: break-all; white-space: nowrap'>abcdefghij</div>", 200,
        "page 200 80\n"
        "block html 0 0 200 80\n"
        "  block body 0 0 200 80\n"
        "    block div 0 0 40 60\n"
        "      line 0 0 40 20 base 14\n"
        "        text \"ab\" 0 2 16 16 serif 16\n"
        "      line 0 20 40 20 base 14\n"
        "        text \"abcde\" 0 22 40 16 serif 16\n"
        "      line 0 40 40 20 base 14\n"
        "        text \"fghij\" 0 42 28 16 serif 16\n"
        "    block div 0 60 40 20\n"
        "      line 0 60 40 20 base 14\n"
        "        text \"abcdefghij\" 0 62 68 16 serif 16\n");
    // break-word (by word-wrap's name here, and word-break's legacy value)
    // cuts the same, and a cut word's last piece breaks at its space as
    // any word: fghij and its space are 32, klm 24 more does not fit.
    cascade_layout_case("overflow-wrap: break-word and word-break: break-word cut the same",
        "<!doctype html><style>body { margin: 0 } div { width: 40px }</style>"
        "<div style='word-wrap: break-word'>abcdefghij klm</div>"
        "<div style='word-break: break-word'>abcdefghij</div>", 200,
        "page 200 100\n"
        "block html 0 0 200 100\n"
        "  block body 0 0 200 100\n"
        "    block div 0 0 40 60\n"
        "      line 0 0 40 20 base 14\n"
        "        text \"abcde\" 0 2 40 16 serif 16\n"
        "      line 0 20 40 20 base 14\n"
        "        text \"fghij\" 0 22 28 16 serif 16\n"
        "      line 0 40 40 20 base 14\n"
        "        text \"klm\" 0 42 24 16 serif 16\n"
        "    block div 0 60 40 40\n"
        "      line 0 60 40 20 base 14\n"
        "        text \"abcde\" 0 62 40 16 serif 16\n"
        "      line 0 80 40 20 base 14\n"
        "        text \"fghij\" 0 82 28 16 serif 16\n");
    // min-content (§ 5.4): anywhere's breaks count, so a float shrinks to
    // its room, 40, and its word is cut there; break-word's do not, so
    // the float's min-content is the whole word, 68, and it does not fit
    // beside the first: it goes below, to 40, and is not cut, its line
    // being as wide as it. The root holds both floats: 60 tall.
    cascade_layout_case("overflow-wrap: anywhere counts toward min-content, break-word does not",
        "<!doctype html><style>body { margin: 0 } div { width: 40px } span { float: left }"
        "</style><div><span style='overflow-wrap: anywhere'>abcdefghij</span></div>"
        "<div><span style='overflow-wrap: break-word'>abcdefghij</span></div>", 200,
        "page 200 60\n"
        "block html 0 0 200 60\n"
        "  block body 0 0 200 0\n"
        "    block div 0 0 40 0\n"
        "      line 0 0 40 0 base 14\n"
        "      block span 0 0 40 40 float-left\n"
        "        line 0 0 40 20 base 14\n"
        "          text \"abcde\" 0 2 40 16 serif 16\n"
        "        line 0 20 40 20 base 14\n"
        "          text \"fghij\" 0 22 28 16 serif 16\n"
        "    block div 0 0 40 0\n"
        "      line 40 0 0 0 base 14\n"
        "      block span 0 40 68 20 float-left\n"
        "        line 0 40 68 20 base 14\n"
        "          text \"abcdefghij\" 0 42 68 16 serif 16\n");
    // ── letter-spacing (CSS Text 3 § 7.2) ──

    // It computes to a length, which is what a child inherits: 0.25em of
    // 16 is 4, and the p's 32px font does not make it 8. normal is 0.
    cascade_case("letter-spacing computes to a length, and that is inherited", 800,
        "<!doctype html><style>div { letter-spacing: 0.25em } p { font-size: 32px }"
        " span { letter-spacing: normal }</style><div><p>a<span>b</span></p></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block letter-spacing=4 holds-block\n"
        "      p block size=32 margin=32/0/32/0\n"
        "        span inline letter-spacing=0\n");
    // 2px after every character of a run but its last: "ab cd" is a 8, b
    // 8, the space 4, c 8, d 8 and four spacings, 44. Two runs, ab and the
    // b element's cd, each lose their last: 18 and 18. Measured for
    // breaking, ab and its space are 20 + 6 and cd 18 more, past 40: ab
    // ends its line, drawn 18 wide without its space. 0.25em of 8px is 2,
    // after a's 4: ab is 10.
    cascade_layout_case("letter-spacing: after every character of a run but the last",
        "<!doctype html><style>body { margin: 0 } p { margin: 0; letter-spacing: 2px }</style>"
        "<p>ab cd</p><p>ab<b>cd</b></p><p style='width: 40px'>ab cd</p>"
        "<p style='letter-spacing: 0.25em; font-size: 8px'>ab</p>", 200,
        "page 200 90\n"
        "block html 0 0 200 90\n"
        "  block body 0 0 200 90\n"
        "    block p 0 0 200 20\n"
        "      line 0 0 200 20 base 14\n"
        "        text \"ab cd\" 0 2 44 16 serif 16\n"
        "    block p 0 20 200 20\n"
        "      line 0 20 200 20 base 14\n"
        "        span b 18 22 18 16\n"
        "        text \"ab\" 0 22 18 16 serif 16\n"
        "        text \"cd\" 18 22 18 16 serif 16 bold\n"
        "    block p 0 40 40 40\n"
        "      line 0 40 40 20 base 14\n"
        "        text \"ab\" 0 42 18 16 serif 16\n"
        "      line 0 60 40 20 base 14\n"
        "        text \"cd\" 0 62 18 16 serif 16\n"
        "    block p 0 80 200 10\n"
        "      line 0 80 200 10 base 7\n"
        "        text \"ab\" 0 81 10 8 serif 8\n");

    // ── outline (CSS UI 4 § 3) ──

    // Not inherited. A style alone draws medium (3) in the element's
    // colour; auto is drawn solid; none draws nothing whatever its colour;
    // an offset in ems is the element's own; inherit takes the parent's
    // colour as it computed, the div's red.
    cascade_case("outline: its width, style, colour and offset", 800,
        "<!doctype html><style>div { outline: 2px dashed red; outline-offset: 3px; color: blue }"
        " p { outline-style: solid } span { outline: auto } b { outline: none;"
        " outline-color: red } i { outline: 1em solid; outline-offset: -0.5em }"
        " em { outline-color: inherit; outline-style: dotted }</style>"
        "<div><p>a<span>s</span><b>b</b><i>i</i></p><em>e</em></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block color=#0000ff outline=2:dashed:#ff0000 outline-offset=3 holds-block\n"
        "      p block margin=16/0/16/0 outline=3:solid:#0000ff\n"
        "        span inline outline=3:auto:#0000ff\n"
        "        b inline weight=700\n"
        "        i inline italic outline=16:solid:#0000ff outline-offset=-8\n"
        "      em inline italic outline=3:dotted:#ff0000\n");
    // It takes no room: the div is where it would be without one.
    cascade_layout_case("outline: takes no room",
        "<!doctype html><style>body { margin: 0 } div { width: 20px; height: 10px;"
        " outline: 2px solid red; outline-offset: 1px }</style><div></div><p>x</p>", 100,
        "page 100 62\n"
        "block html 0 0 100 62\n"
        "  block body 0 0 100 46\n"
        "    block div 0 0 20 10\n"
        "    block p 0 26 100 20\n"
        "      line 0 26 100 20 base 14\n"
        "        text \"x\" 0 28 8 16 serif 16\n");
    outline_door_cases();

    // ── CSS Logical 1, in a horizontal left-to-right writing mode ──

    // A flow-relative declaration is its physical twin's for the cascade:
    // the later of margin-left and margin-inline-start wins, and so does
    // margin-right after margin-inline-end; #x's padding-inline outranks
    // p's padding-left by specificity, whatever the order. padding-block
    // is top and bottom, 1em of 16. inline-size is the width,
    // max-block-size the max-height, border-block-start the top border,
    // inset-inline-start the left inset.
    cascade_case("flow-relative properties are their physical twins in the cascade", 800,
        "<!doctype html><style>p { margin-left: 1px; margin-inline-start: 2px;"
        " margin-inline-end: 3px; margin-right: 4px }"
        " #x { padding-inline: 5px 6px } p { padding-left: 7px; padding-block: 1em }"
        " div { inline-size: 50%; max-block-size: 10px; border-block-start: 2px solid red;"
        " inset-inline-start: 3px; position: relative }</style>"
        "<p id=x>a</p><p>b</p><div>c</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block margin=16/4/16/2 padding=16/6/16/5\n"
        "    p block margin=16/4/16/2 padding=16/0/16/7\n"
        "    div block border=2:solid:#ff0000/0:none:ink/0:none:ink/0:none:ink width=50%"
        " max-height=10 position=relative inset=auto/auto/auto/3\n");

    s_modern_off = true;
    cascade_case("flow-relative properties: unknown with the Modern Web switch off", 800,
        "<!doctype html><style>p { margin-left: 1px; margin-inline-start: 2px;"
        " padding-inline: 5px }</style><p>a</p>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block margin=16/0/16/1\n");
    cascade_case("outline: unknown with the Modern Web switch off", 800,
        "<!doctype html><style>div { outline: 2px solid red }</style><div>a</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block\n");
    cascade_case("letter-spacing: unknown with the Modern Web switch off", 800,
        "<!doctype html><style>div { letter-spacing: 3px }</style><div>a</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block\n");
    cascade_layout_case("word-break: unknown with the Modern Web switch off",
        "<!doctype html><style>body { margin: 0 } div { width: 40px; word-break: break-all }"
        "</style><div>abcdefghij</div>", 200,
        "page 200 20\n"
        "block html 0 0 200 20\n"
        "  block body 0 0 200 20\n"
        "    block div 0 0 40 20\n"
        "      line 0 0 40 20 base 14\n"
        "        text \"abcdefghij\" 0 2 68 16 serif 16\n");
    s_modern_off = false;
}
