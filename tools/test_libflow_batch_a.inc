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
// span is an inline box's piece, which draws none. A negative offset
// that would turn the ring inside out is held: the ring's outside is
// never under twice its width each way (CSS UI 4 § 3.5).
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
    // p: 10 x 4 at y 10, right after the div's 10 (its 20 tall line
    // overflows it), less 4 a side and plus 1: across, 10 - 6 = 4, which is
    // at least twice the width, 2, so 3..7; down, 4 - 6 = -2 is held to 2,
    // about the p's middle: 11..13.
    bool held = para != NULL && flow_outline_rect(para, &r);
    snprintf(ink, sizeof(ink), "%d %d %d %d", r.x, r.y, r.w, r.h);
    expect("outline: an offset that would turn the ring inside out is held to twice its width",
           held && strcmp(ink, "3 11 4 2") == 0, ink);
    flow_free(tree);
    sheeted_close(&p);
}

// A block's ellipsis (flow_box_t.ellipsis): made for a block whose
// text-overflow is ellipsis and whose overflow is not visible, of U+2026
// in its own fonts (the test faces have every scalar), and for no other.
// flow_visit's record of the kinds it hands over: a LINE only as the
// last box of a line of a box with an ellipsis, after its pieces.
typedef struct {
    char kinds[64];
    int n;
} KindLog;

static void log_kind(void *ctx, const flow_box_t *b)
{
    KindLog *k = ctx;
    if (k->n < 63 && b->kind >= FLOW_BOX_LINE)
        k->kinds[k->n++] = b->kind == FLOW_BOX_LINE ? 'L' : b->kind == FLOW_BOX_TEXT ? 'T' : 'o';
    k->kinds[k->n] = '\0';
}

// The outline step (flow_visitor_t.outline): each box's outline handed
// over once, last in its stacking context. The page: a with an outline,
// then a positioned z-index 1 context b holding c with one, then d. The
// root context paints a and d, then its member b's context — b, c, and
// c's outline, last in b's — and then, last of all (step 10 comes after
// the members of step 9), a's outline.
typedef struct {
    char seq[128];
    int n;
} OutlineLog;

static void outline_log(OutlineLog *k, char what, const flow_box_t *b)
{
    if (b->node == NULL || b->node->kind != OS64_HTML_ELEMENT || b->kind == FLOW_BOX_LINE ||
        b->node->tag == OS64_HTML_TAG_HTML || b->node->tag == OS64_HTML_TAG_BODY || k->n > 120)
        return;
    k->seq[k->n++] = what;
    k->seq[k->n++] = (char)os64_html_attr(b->node, "id")->value[0];
    k->seq[k->n] = '\0';
}

static void outline_visit(void *ctx, const flow_box_t *b) { outline_log(ctx, 'v', b); }
static void outline_ring(void *ctx, const flow_box_t *b) { outline_log(ctx, 'o', b); }

static void outline_step_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>div { height: 10px; outline: 1px solid }"
                     " #b { position: relative; z-index: 1; outline: none } #d { outline: none }"
                     "</style><div id=a></div><div id=b><div id=c></div></div><div id=d></div>",
                 200, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 200, &p.env);
    OutlineLog log = {{0}, 0};
    flow_visitor_t vis = {outline_visit, NULL, NULL, &log, outline_ring};
    flow_visit_groups(tree, (os64_gui_rect_t){0, 0, 200, 600}, (flow_point_t){0, 0}, &vis);
    expect("outline: each last in its stacking context", strcmp(log.seq, "vavdvbvcocoa") == 0,
           log.seq);
    OutlineLog plain = {{0}, 0};
    flow_visit(tree, (os64_gui_rect_t){0, 0, 200, 600}, (flow_point_t){0, 0}, outline_visit, &plain);
    expect("outline: flow_visit hands over no outline step", strcmp(plain.seq, "vavdvbvc") == 0,
           plain.seq);
    flow_free(tree);
    sheeted_close(&p);
    // A ring reaches past its box's overflow: the box 0, 0, 20 x 10 is out
    // of a view 30 to 40 down that its ring, offset 20 and 5 wide (to 35),
    // meets. The outline step asks the ring, not the box.
    sheeted_open(&p, "<!doctype html><style>body { margin: 0 } div { width: 20px; height: 10px;"
                     " outline: 5px solid; outline-offset: 20px }</style><div id=a></div>",
                 200, 600);
    tree = flow_layout(p.doc, p.page, 200, &p.env);
    OutlineLog far = {{0}, 0};
    flow_visitor_t fv = {outline_visit, NULL, NULL, &far, outline_ring};
    flow_visit_groups(tree, (os64_gui_rect_t){0, 30, 200, 10}, (flow_point_t){0, 0}, &fv);
    expect("outline: a ring in view is handed over though its box is not",
           strcmp(far.seq, "oa") == 0, far.seq);
    flow_free(tree);
    sheeted_close(&p);
}

static void ellipsis_door_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>div, p { width: 40px; white-space: nowrap;"
                     " text-overflow: ellipsis } div { overflow: hidden }"
                     " section { overflow: hidden }</style>"
                     "<div>abcdefghij</div><p>abcdefghij</p><section>abcdefghij</section>",
                 100, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 100, &p.env);
    const flow_box_t *root = tree != NULL ? flow_root(tree) : NULL;
    const flow_box_t *div = batch_a_box(root, OS64_HTML_TAG_DIV, FLOW_BOX_BLOCK);
    const flow_box_t *para = batch_a_box(root, OS64_HTML_TAG_P, FLOW_BOX_BLOCK);
    const flow_box_t *sect = batch_a_box(root, OS64_HTML_TAG_SECTION, FLOW_BOX_BLOCK);
    os64_text_run_view_t v;
    expect("text-overflow: a block that clips has its ellipsis, U+2026, 8 wide",
           div != NULL && div->ellipsis != NULL &&
               os64_text_run_view(div->ellipsis, &v) == OS64_FONT_OK && v.byte_count == 3 &&
               memcmp(v.bytes, "\xe2\x80\xa6", 3) == 0 && v.advance_x == 8 * 64, NULL);
    expect("text-overflow: none on a block whose overflow is visible",
           para != NULL && para->ellipsis == NULL, NULL);
    expect("text-overflow: none on a block that clips without asking",
           sect != NULL && sect->ellipsis == NULL, NULL);
    // The div's one line is handed over after its text; the p's and the
    // section's are not.
    KindLog log = {{0}, 0};
    flow_visit(tree, (os64_gui_rect_t){0, 0, 100, 600}, (flow_point_t){0, 0}, log_kind, &log);
    expect("text-overflow: the walk hands over a line of a box with an ellipsis, after it",
           strcmp(log.kinds, "TLTT") == 0, log.kinds);
    flow_free(tree);
    sheeted_close(&p);
}

// The cut has two doors (flow_line_cut): what is not drawn is not hit.
// Each div is 40 wide, its ellipsis 8, its limit 32. In the first, abcd
// ends at 32 and an inline-block 30 wide follows it, left out; in the
// second a link's text is kept to abcd, 0..32. A point on the kept text
// hits it; a point on the hidden atom (35), on the link's hidden tail
// (36) or on the ellipsis (33) hits the block: the ellipsis stands in no
// inline box.
static void ellipsis_hit_cases(void)
{
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>body { margin: 0 } div { width: 40px;"
                     " overflow: hidden; white-space: nowrap; text-overflow: ellipsis }"
                     " i { display: inline-block; width: 30px; height: 10px }</style>"
                     "<div>abcd<i></i></div><div><a href=x>abcdefghij</a></div>",
                 100, 600);
    flow_tree_t *tree = flow_layout(p.doc, p.page, 100, &p.env);
    const flow_point_t o = {0, 0};
    const flow_box_t *atom = tree != NULL ? flow_hit(tree, 35, 5, o) : NULL;
    const flow_box_t *kept = tree != NULL ? flow_hit(tree, 10, 25, o) : NULL;
    const flow_box_t *tail = tree != NULL ? flow_hit(tree, 36, 25, o) : NULL;
    const flow_box_t *dots = tree != NULL ? flow_hit(tree, 33, 25, o) : NULL;
    expect("text-overflow: an atom past the cut is not hit; its block is",
           atom != NULL && atom->kind == FLOW_BOX_BLOCK && atom->node != NULL &&
               atom->node->tag == OS64_HTML_TAG_DIV, NULL);
    expect("text-overflow: a link's kept text is hit",
           kept != NULL && kept->kind == FLOW_BOX_TEXT, NULL);
    expect("text-overflow: a link's hidden tail is not hit; its block is",
           tail != NULL && tail->kind == FLOW_BOX_BLOCK && tail->node->tag == OS64_HTML_TAG_DIV,
           NULL);
    expect("text-overflow: the ellipsis is its block's, not the link's",
           dots != NULL && dots->kind == FLOW_BOX_BLOCK && dots->node->tag == OS64_HTML_TAG_DIV,
           NULL);
    flow_free(tree);
    sheeted_close(&p);
}

// What a hostile sheet can ask of slice 4's properties lays out whole:
// a tracking of a hundred thousand pixels either way across a long word
// (each run still fits the engine's coordinates: none trails a run, and a
// window too wide is halved), a ratio of a millionth and of a million,
// an outline a million pixels wide and as far off, every character an
// opportunity in a long text, an ellipsis on a line it cannot fit.
static void hostile_batch_a_cases(void)
{
    static const char *const kPages[] = {
        "<p style='letter-spacing: 100000px'>abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
        "<p style='letter-spacing: -100000px'>abcdefghijklmnopqrstuvwxyz abc",
        "<div style='width: 1000000px; aspect-ratio: 1 / 1000000'></div>"
        "<div style='height: 1000000px; aspect-ratio: 1000000'></div>",
        "<div style='outline: 1000000px solid; outline-offset: -1000000px'>a</div>"
        "<div style='outline: 1000000px solid; outline-offset: 1000000px'>a</div>",
        "<p style='word-break: break-all; width: 1px'>abcdefghijklmnopqrstuvwxyz0123456789",
        "<p style='overflow-wrap: anywhere; width: 0'>abcdefghij klmnop",
        "<p style='width: 0; overflow: hidden; white-space: nowrap; text-overflow: ellipsis'>abc",
    };
    for (size_t k = 0; k < sizeof(kPages) / sizeof(kPages[0]); k++) {
        char html[512];
        snprintf(html, sizeof(html), "<!doctype html>%s", kPages[k]);
        Sheeted p;
        sheeted_open(&p, html, 300, 600);
        flow_tree_t *tree = flow_layout(p.doc, p.page, 300, &p.env);
        expect("batch A: a hostile page lays out whole", tree != NULL && !flow_incomplete(tree),
               kPages[k]);
        flow_free(tree);
        sheeted_close(&p);
    }
}

// Cutting words at their characters costs the text, not its square: a run
// holds every word of its text, and word_pieces finds each word's first
// caret by halving rather than walking from the run's start. 4,000 words
// of `ab ` under break-all (laid out) and under anywhere in a float
// (measured for min-content, then laid out): each word steps through its
// cut and the caret that ends it, two a word each time the context is
// measured. Walked from the start, it was about 8,000 for each word.
static void piece_bounds(void)
{
    static const char *const kStyles[] = {
        "<p style='word-break: break-all'>",
        "<div style='float: left; overflow-wrap: anywhere'>",
    };
    size_t words = 4000;
    for (size_t k = 0; k < 2; k++) {
        char *html = malloc(words * 3 + 128);
        strcpy(html, "<!doctype html>");
        strcat(html, kStyles[k]);
        size_t n = strlen(html);
        for (size_t i = 0; i < words; i++, n += 3)
            memcpy(html + n, "ab ", 3);
        html[n] = '\0';
        Sheeted p;
        sheeted_open(&p, html, 800, 600);
        FStyles *st = f_style_build(p.doc, p.page, &p.env);
        FBoxes *bx = st != NULL ? f_boxes_build(p.doc, p.page, st, &p.env) : NULL;
        FLayout *lay = bx != NULL ? f_layout(bx, p.doc, p.page, &p.env, 800) : NULL;
        bool ok = lay != NULL && !lay->incomplete && lay->measures > 0 &&
                  lay->piece_carets <= 2 * words * lay->measures;
        expect(k == 0 ? "word-break: break-all cuts words in time linear in the text"
                      : "overflow-wrap: anywhere measures min-content in time linear in the text",
               ok, NULL);
        if (!ok && lay != NULL)
            fprintf(stderr, "    carets %llu, measures %llu\n",
                    (unsigned long long)lay->piece_carets, (unsigned long long)lay->measures);
        f_layout_free(lay);
        f_boxes_free(bx);
        f_style_free(st);
        sheeted_close(&p);
        free(html);
    }
}

static void batch_a_cases(void)
{
    hostile_batch_a_cases();
    piece_bounds();
    ellipsis_hit_cases();
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
    // A box of no width is still a line to break: break-word cuts its word
    // to a character a line there, as it does at 1px (measuring min-content
    // is said by Segs.measuring, never by a width of 0).
    cascade_layout_case("overflow-wrap: break-word in a box of no width",
        "<!doctype html><style>body { margin: 0 } div { width: 0; overflow-wrap: break-word }"
        "</style><div>abc</div>", 200,
        "page 200 60\n"
        "block html 0 0 200 60\n"
        "  block body 0 0 200 60\n"
        "    block div 0 0 0 60\n"
        "      line 0 0 0 20 base 14\n"
        "        text \"a\" 0 2 8 16 serif 16\n"
        "      line 0 20 0 20 base 14\n"
        "        text \"b\" 0 22 8 16 serif 16\n"
        "      line 0 40 0 20 base 14\n"
        "        text \"c\" 0 42 8 16 serif 16\n");

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
    // 2px after every character but the line's last (CSS Text 3 § 7.2: none
    // at the end of a line): "ab cd" is a 8, b 8, the space 4, c 8, d 8 and
    // four spacings, 44. Across an element's edge the characters are
    // spaced as any two are: ab is 20 with b's spacing, and the b
    // element's cd, ending the line, 18. Measured for breaking, ab and its
    // space are 20 + 6, and cd fits if 18 does: 44 does not fit in 40, so
    // ab ends its line, drawn 18 wide without its space or its last
    // spacing. 0.25em of 8px is 2, after a's 4: ab is 10.
    cascade_layout_case("letter-spacing: after every character but the line's last",
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
        "        span b 20 22 18 16\n"
        "        text \"ab\" 0 22 20 16 serif 16\n"
        "        text \"cd\" 20 22 18 16 serif 16 bold\n"
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
    // outline-width's computed value is its length whatever the style (CSS
    // UI 4 § 3.2), so `inherit` passes on 4 from a solid outline and 7 from
    // one whose style is none; `initial` and `revert` are medium, 3. A
    // style of none draws nothing and prints nothing.
    cascade_case("outline: the width is computed whatever the style", 800,
        "<!doctype html><style>.p { outline: 4px solid } .p i { outline-width: inherit;"
        " outline-style: solid } .q { outline: 7px none } .q i { outline-width: inherit;"
        " outline-style: solid } .e { outline-style: solid; outline-width: initial } .r {"
        " outline-style: solid; outline-width: revert }</style><div class=p><i>a</i></div>"
        "<div class=q><i>b</i></div><div class=e></div><div class=r></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block outline=4:solid:ink\n"
        "      i inline italic outline=4:solid:ink\n"
        "    div block\n"
        "      i inline italic outline=7:solid:ink\n"
        "    div block outline=3:solid:ink\n"
        "    div block outline=3:solid:ink\n");
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
    outline_step_cases();

    // ── aspect-ratio (CSS Sizing 4 § 4) ──

    // Kept as width over height, to three places in the dump; `auto`
    // beside it is shown; a degenerate ratio is none.
    cascade_case("aspect-ratio: its ratio, auto beside it, a degenerate one none", 800,
        "<!doctype html><style>div { aspect-ratio: 16 / 9 } p { aspect-ratio: 2 auto }"
        " span { aspect-ratio: 0 / 1 } b { aspect-ratio: 1 / 0 }</style>"
        "<div><p>a<span>b</span><b>c</b></p></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block aspect-ratio=1.778 holds-block\n"
        "      p block margin=16/0/16/0 aspect-ratio=auto/2.000\n"
        "        span inline\n"
        "        b inline weight=700\n");
    // Blocks, 16px text 20 a line. a: 100 wide, 2 to 1: 50. b: 50 high,
    // 16/9 across, 88.9, drawn 89. c: 40 wide at 4 is 10, but its two
    // lines are 40: min-height auto holds it to its content (§ 4.3). d
    // scrolls and e has a min-height of its own, so neither grows: 10. f:
    // border-box, the ratio its border box's, 100 by 50 with 10 of padding
    // round a 30 content. g: `auto 2` is the content box's, 80 by 40, 60
    // with its padding: under `auto && <ratio>` the ratio works on the
    // content box "in all cases" (§ 4.1), a box with no natural ratio
    // included.
    cascade_layout_case("aspect-ratio: a block's height from its width, or its width from its height",
        "<!doctype html><style>body { margin: 0 } p { margin: 0 }"
        " .a { width: 100px; aspect-ratio: 2 } .b { height: 50px; aspect-ratio: 16 / 9 }"
        " .c { width: 40px; aspect-ratio: 4 } .d { width: 40px; aspect-ratio: 4; overflow: hidden }"
        " .e { width: 40px; aspect-ratio: 4; min-height: 0 }"
        " .f { width: 100px; padding: 10px; box-sizing: border-box; aspect-ratio: 2 }"
        " .g { width: 100px; padding: 10px; box-sizing: border-box; aspect-ratio: auto 2 }</style>"
        "<div class=a></div><div class=b></div><div class=c>ab cd ef</div>"
        "<div class=d>ab cd ef</div><div class=e>ab cd ef</div><div class=f></div>"
        "<div class=g></div>", 200,
        "page 200 270\n"
        "block html 0 0 200 270\n"
        "  block body 0 0 200 270\n"
        "    block div 0 0 100 50\n"
        "    block div 0 50 89 50\n"
        "    block div 0 100 40 40\n"
        "      line 0 100 40 20 base 14\n"
        "        text \"ab cd\" 0 102 36 16 serif 16\n"
        "      line 0 120 40 20 base 14\n"
        "        text \"ef\" 0 122 12 16 serif 16\n"
        "    block div 0 140 40 10 scroller 0 range 0 30\n"
        "      line 0 140 40 20 base 14 clip 0 140 40 10\n"
        "        text \"ab cd\" 0 142 36 16 serif 16 clip 0 140 40 10\n"
        "      line 0 160 40 20 base 14 clip 0 140 40 10\n"
        "        text \"ef\" 0 162 12 16 serif 16 clip 0 140 40 10\n"
        "    block div 0 150 40 10\n"
        "      line 0 150 40 20 base 14\n"
        "        text \"ab cd\" 0 152 36 16 serif 16\n"
        "      line 0 170 40 20 base 14\n"
        "        text \"ef\" 0 172 12 16 serif 16\n"
        "    block div 0 160 100 50\n"
        "    block div 0 210 100 60\n");
    // Pictures (the test oracle's known.png is 40 x 30): a ratio alone is
    // the page's, 80 by 80; `auto 1` keeps the picture's own 4 to 3, 80 by
    // 60; one not known at 2 to 1 is 80 by 40, not guessed square. An
    // inline-block 20 high at 3 is 60 wide. A box with a ratio is not
    // auto-high for margins: the two 10s meet between the divs, never
    // through one (the p's 16 above wins over 10).
    cascade_layout_case("aspect-ratio: pictures, an inline-block, margins",
        "<!doctype html><style>body { margin: 0 } img { display: block }"
        " .p { width: 80px; aspect-ratio: 1 } .q { width: 80px; aspect-ratio: auto 1 }"
        " .r { width: 80px; aspect-ratio: 2 } span { display: inline-block; height: 20px;"
        " aspect-ratio: 3 } div.m { margin: 10px 0; aspect-ratio: 10 }</style>"
        "<img class=p src=known.png><img class=q src=known.png><img class=r src=missing.png>"
        "<p>x<span></span></p><div class=m></div><div class=m></div>", 200,
        "page 200 298\n"
        "block html 0 0 200 298\n"
        "  block body 0 0 200 288\n"
        "    replaced img 0 0 80 80\n"
        "    replaced img 0 80 80 60\n"
        "    replaced img 0 140 80 40\n"
        "    block p 0 196 200 26\n"
        "      line 0 196 200 26 base 20\n"
        "        text \"x\" 0 204 8 16 serif 16\n"
        "        atomic span 8 196 60 20\n"
        "          block span 8 196 60 20\n"
        "    block div 0 238 200 20\n"
        "    block div 0 268 200 20\n");

    // The cold reader's cases (Sizing 4 § 4.1, Flexbox 1 § 9.2.3 B and §
    // 9.8, Box Alignment 3 § 6.2): a flex item 20 high at 1 is 20 wide; a
    // float 20 high at 2 is 40 wide, not shrunk to its nothing; an absolute
    // box 20 high at 3 is 60 wide.
    cascade_layout_case("aspect-ratio: a flex item, a float and an absolute box take their widths",
        "<!doctype html><style>body { margin: 0 } .f { display: flex } .a { height: 20px;"
        " aspect-ratio: 1 } .fl { float: left; height: 20px; aspect-ratio: 2 } .ab { position:"
        " absolute; top: 60px; left: 0; height: 20px; aspect-ratio: 3 } .blk { height: 20px;"
        " aspect-ratio: 1 }</style><div class=f><div class=a></div></div><div class=blk></div>"
        "<div class=fl></div><div class=ab></div>", 200,
        "page 200 80\n"
        "block html 0 0 200 60\n"
        "  block body 0 0 200 40\n"
        "    block div 0 0 200 20\n"
        "      block div 0 0 20 20\n"
        "    block div 0 20 20 20\n"
        "    block div 0 40 40 20 float-left\n"
        "    block div 0 60 60 20 positioned\n");
    // A column 100 wide stretches its item across, which its ratio of 2
    // makes 50 high; a row item 40 wide at 1, not stretched, is 40 high; a
    // grid item stretched across its 60 column is placed down at the start
    // (`normal` is `start` for a box with a ratio), 20 high at 3.
    cascade_layout_case("aspect-ratio: a column item, a row item and a grid item take their heights",
        "<!doctype html><style>body { margin: 0 } .col { display: flex; flex-direction: column;"
        " width: 100px } .c { aspect-ratio: 2 } .row { display: flex; align-items: flex-start }"
        " .r { width: 40px; aspect-ratio: 1 } .g { display: grid; width: 60px } .gi {"
        " aspect-ratio: 3 }</style><div class=col><div class=c></div></div><div class=row><div"
        " class=r></div></div><div class=g><div class=gi></div></div>", 200,
        "page 200 110\n"
        "block html 0 0 200 110\n"
        "  block body 0 0 200 110\n"
        "    block div 0 0 100 50\n"
        "      block div 0 0 100 50\n"
        "    block div 0 50 200 40\n"
        "      block div 0 50 40 40\n"
        "    block div 0 90 60 20\n"
        "      block div 0 90 60 20\n");
    // A row 30 high stretches its item down, which is then 60 wide at 2
    // (its stretched height is definite, § 9.8); a column item given 80 at
    // 4 is 20 high; `align-self: stretch` the page wrote stretches a grid
    // item with a ratio down its 50; a float shrinks to its child's
    // width, the 10 high child's ratio of 5 making its min- and
    // max-content 50; a grid row is as tall as a ratio item 30 wide at 1,
    // and its neighbour stretched to it.
    cascade_layout_case("aspect-ratio: stretched sizes, a written stretch, intrinsic widths",
        "<!doctype html><style>body { margin: 0 } div { margin: 0 } .f { display: flex;"
        " height: 30px } .i { aspect-ratio: 2 } .c { display: flex; flex-direction: column;"
        " width: 200px; align-items: flex-start } .w { width: 80px; aspect-ratio: 4 } .g {"
        " display: grid; width: 60px } .s { aspect-ratio: 3; align-self: stretch } .fl {"
        " float: left } .k { height: 10px; aspect-ratio: 5 } .gr { display: grid;"
        " grid-template-columns: 30px 30px } .tall { aspect-ratio: 1 }</style>"
        "<div class=f><div class=i></div></div><div class=c><div class=w></div></div>"
        "<div class=g style='height: 50px'><div class=s></div></div><div class=fl><div class=k>"
        "</div></div><div style='clear: both' class=gr><div class=tall>x</div><div>y</div></div>",
        200,
        "page 200 140\n"
        "block html 0 0 200 140\n"
        "  block body 0 0 200 140\n"
        "    block div 0 0 200 30\n"
        "      block div 0 0 60 30\n"
        "    block div 0 30 200 20\n"
        "      block div 0 30 80 20\n"
        "    block div 0 50 60 50\n"
        "      block div 0 50 60 50\n"
        "    block div 0 100 50 10 float-left\n"
        "      block div 0 100 50 10\n"
        "    block div 0 110 200 30 clearance=10\n"
        "      block div 0 110 30 30\n"
        "        line 0 110 30 20 base 14\n"
        "          text \"x\" 0 112 8 16 serif 16\n"
        "      block div 30 110 30 30\n"
        "        line 30 110 30 20 base 14\n"
        "          text \"y\" 30 112 8 16 serif 16\n");

    // Codex round 1: a ratio narrows no box below its automatic minimum,
    // its min-content width, capped by its maximum (Sizing 4 § 4.3).
    // abcdefghijkl is 80 wide in the test faces, with no break in it; each
    // box is 10 high at 1, so its ratio alone would make it 10 wide.
    cascade_layout_case("aspect-ratio: the automatic minimum widens a block, a float, an absolute box",
        "<!doctype html><style>body { margin: 0 } .r { height: 10px; aspect-ratio: 1 }"
        " .fl { float: left } .ab { position: absolute; top: 40px; left: 0 }</style>"
        "<div class=r>abcdefghijkl</div><div class='r fl'>abcdefghijkl</div>"
        "<div class='r ab'>abcdefghijkl</div>", 200,
        "page 200 58\n"
        "block html 0 0 200 20\n"
        "  block body 0 0 200 10\n"
        "    block div 0 0 80 10\n"
        "      line 0 0 80 20 base 14\n"
        "        text \"abcdefghijkl\" 0 2 80 16 serif 16\n"
        "    block div 0 10 80 10 float-left\n"
        "      line 0 10 80 20 base 14\n"
        "        text \"abcdefghijkl\" 0 12 80 16 serif 16\n"
        "    block div 0 40 80 10 positioned\n"
        "      line 0 40 80 20 base 14\n"
        "        text \"abcdefghijkl\" 0 42 80 16 serif 16\n");
    // An inline-block, a flex item and a float sized by a child with a
    // ratio, each the same; a `min-width: 0`, a box that scrolls across,
    // and a max-width of 50 leave the ratio's 10, 10 and 50.
    cascade_layout_case("aspect-ratio: the automatic minimum in an atom, a flex item, an intrinsic width",
        "<!doctype html><style>body { margin: 0 } .r { height: 10px; aspect-ratio: 1 }"
        " .ib { display: inline-block } .f { display: flex } .fl { float: left }"
        " .z { min-width: 0 } .h { overflow: hidden } .m { max-width: 50px }</style>"
        "<div><span class='r ib'>abcdefghijkl</span></div><div class=f><div class=r>abcdefghijkl"
        "</div></div><div class=fl><div class=r>abcdefghijkl</div></div><div class='r z'"
        " style='clear: both'>abcdefghijkl</div><div class='r h'>abcdefghijkl</div><div class='r m'>"
        "abcdefghijkl</div>", 200,
        "page 200 78\n"
        "block html 0 0 200 70\n"
        "  block body 0 0 200 70\n"
        "    block div 0 0 200 20\n"
        "      line 0 0 200 20 base 14\n"
        "        atomic span 0 0 80 10\n"
        "          block span 0 0 80 10\n"
        "            line 0 0 80 20 base 14\n"
        "              text \"abcdefghijkl\" 0 2 80 16 serif 16\n"
        "    block div 0 20 200 10\n"
        "      block div 0 20 80 10\n"
        "        line 0 20 80 20 base 14\n"
        "          text \"abcdefghijkl\" 0 22 80 16 serif 16\n"
        "    block div 0 30 80 10 float-left\n"
        "      block div 0 30 80 10\n"
        "        line 0 30 80 20 base 14\n"
        "          text \"abcdefghijkl\" 0 32 80 16 serif 16\n"
        "    block div 0 40 10 10 clearance=10\n"
        "      line 0 40 10 20 base 14\n"
        "        text \"abcdefghijkl\" 0 42 80 16 serif 16\n"
        "    block div 0 50 10 10 scroller 0 range 70 10\n"
        "      line 0 50 10 20 base 14 clip 0 50 10 10\n"
        "        text \"abcdefghijkl\" 0 52 80 16 serif 16 clip 0 50 10 10\n"
        "    block div 0 60 50 10\n"
        "      line 0 60 50 20 base 14\n"
        "        text \"abcdefghijkl\" 0 62 80 16 serif 16\n");
    // A column item 100 wide at 2 holding five lines, 100 high, in a column
    // 60 high: its automatic minimum is its content's height, so it does
    // not shrink to fit (Flexbox 1 § 4.5; the transferred size suggestion
    // is a replaced box's).
    cascade_layout_case("aspect-ratio: a column item does not shrink below its content",
        "<!doctype html><style>body { margin: 0 } .c { display: flex; flex-direction: column;"
        " height: 60px; width: 100px } .i { aspect-ratio: 2 }</style><div class=c><div class=i>"
        "a<br>b<br>c<br>d<br>e</div></div>", 200,
        "page 200 100\n"
        "block html 0 0 200 60\n"
        "  block body 0 0 200 60\n"
        "    block div 0 0 100 60\n"
        "      block div 0 0 100 100\n"
        "        line 0 0 100 20 base 14\n"
        "          text \"a\" 0 2 8 16 serif 16\n"
        "        line 0 20 100 20 base 14\n"
        "          text \"b\" 0 22 8 16 serif 16\n"
        "        line 0 40 100 20 base 14\n"
        "          text \"c\" 0 42 8 16 serif 16\n"
        "        line 0 60 100 20 base 14\n"
        "          text \"d\" 0 62 8 16 serif 16\n"
        "        line 0 80 100 20 base 14\n"
        "          text \"e\" 0 82 8 16 serif 16\n");

    // Second cold read. An absolute box with a ratio whose two sizes are
    // left to it and all four insets set has its BLOCK axis as the
    // ratio-dependent one (Position 3 § 4.1): WPT abspos-003, 100 wide in
    // a 100 x 500 block, is 100 high and not stretched to 500; abspos-005,
    // given 100 wide among the same insets, the same; abspos-006, given 100
    // high in a 500 x 100 block, is 100 wide.
    cascade_layout_case("aspect-ratio: an absolute box with every inset set (WPT abspos-003, -005, -006)",
        "<!doctype html><style>body { margin: 0 } .c { position: relative } .r {"
        " position: absolute; left: 0; right: 0; top: 0; bottom: 0; aspect-ratio: 1 }</style>"
        "<div class=c style='width: 100px; height: 500px'><div class=r></div></div>"
        "<div class=c style='width: 100px; height: 500px'><div class=r style='width: 100px'></div></div>"
        "<div class=c style='width: 500px; height: 100px'><div class=r style='height: 100px'></div></div>",
        800,
        "page 800 1100\n"
        "block html 0 0 800 1100\n"
        "  block body 0 0 800 1100\n"
        "    block div 0 0 100 500 positioned\n"
        "      line 0 0 100 0 base 14\n"
        "      block div 0 0 100 100 positioned\n"
        "    block div 0 500 100 500 positioned\n"
        "      line 0 500 100 0 base 14\n"
        "      block div 0 500 100 100 positioned\n"
        "    block div 0 1000 500 100 positioned\n"
        "      line 0 1000 500 0 base 14\n"
        "      block div 0 1000 100 100 positioned\n");
    // WPT abspos-021: inset 0 in a block 200 wide and 0 high, max-height
    // 100 at 1. The width stretches to 200, but the max-height carried
    // across the ratio (Sizing 4 § 4.4) holds it to 100, and the height
    // its ratio gives is 100.
    cascade_layout_case("aspect-ratio: a max-height held across the ratio (WPT abspos-021)",
        "<!doctype html><style>body { margin: 0 }</style><div style='position: relative;"
        " width: 200px'><div style='position: absolute; inset: 0; max-height: 100px;"
        " aspect-ratio: 1 / 1'></div></div>", 800,
        "page 800 100\n"
        "block html 0 0 800 0\n"
        "  block body 0 0 800 0\n"
        "    block div 0 0 200 0 positioned\n"
        "      line 0 0 200 0 base 14\n"
        "      block div 0 0 100 100 positioned\n");
    // Height 100, max-height 50, at 2: the height is held first, so the
    // width is 100, in every formatting context — a block, a float, an
    // inline-block, an absolute box, a flex item, an item stretched across
    // a column and a grid item (whose widths are carried the max-height's
    // 50 across, Sizing 4 § 4.4), and a float sized by such a child.
    cascade_layout_case("aspect-ratio: the height held first, in every context",
        "<!doctype html><style>body { margin: 0 } .r { height: 100px; max-height: 50px;"
        " aspect-ratio: 2 / 1 } .f { float: left } .ib { display: inline-block } .a {"
        " position: absolute; top: 300px; left: 0 } .x { display: flex } .col { display: flex;"
        " flex-direction: column; width: 300px } .g { display: grid; width: 300px } .m {"
        " max-height: 50px; aspect-ratio: 2 / 1 }</style>"
        "<div class=r></div><div class='r f'></div><p style='clear: both; margin: 0'>"
        "<span class='r ib'></span></p><div class='r a'></div><div class=x><div class=r></div>"
        "</div><div class=col><div class=m></div></div><div class=g><div class=m></div></div>"
        "<div class=f><div class=r></div></div>",
        800,
        "page 800 356\n"
        "block html 0 0 800 356\n"
        "  block body 0 0 800 306\n"
        "    block div 0 0 100 50\n"
        "    block div 0 50 100 50 float-left\n"
        "    block p 0 100 800 56 clearance=50\n"
        "      line 0 100 800 56 base 50\n"
        "        atomic span 0 100 100 50\n"
        "          block span 0 100 100 50\n"
        "    block div 0 300 100 50 positioned\n"
        "    block div 0 156 800 50\n"
        "      block div 0 156 100 50\n"
        "    block div 0 206 300 50\n"
        "      block div 0 206 100 50\n"
        "    block div 0 256 300 50\n"
        "      block div 0 256 100 50\n"
        "    block div 0 306 100 50 float-left\n"
        "      block div 0 306 100 50\n");
    // WPT block-aspect-ratio-025: a float of height 100%, which is auto
    // here, with min-height 100 at 1 — the min-height carried across makes
    // it 100 wide; -027: a block 100/80 with max-height 80, stretched
    // across the page, is held to the 100 its max-height gives (and 20 of
    // padding below makes it square); -043: height 200 at 1/2 with
    // max-height 100 holding a 100-wide child is 100 square, its automatic
    // minimum flooring the width the max-height would carry across.
    cascade_layout_case("aspect-ratio: limits carried across (WPT block-aspect-ratio-025, -027, -043)",
        "<!doctype html><style>body { margin: 0 } p { margin: 0 } i { display: inline-block;"
        " width: 20px; height: 10px; vertical-align: bottom }</style>"
        "<div style='aspect-ratio: 1/1; height: 100%; min-height: 100px; float: left'></div>"
        "<div style='clear: both; height: 100px'><div style='aspect-ratio: 100/80;"
        " max-height: 80px; padding-bottom: 20px'><i></i><i></i><i></i><i></i><i></i><i></i>"
        "<i></i><i></i><i></i><i></i></div></div>"
        "<div style='height: 200px; aspect-ratio: 1 / 2; max-height: 100px'><div"
        " style='width: 100px'></div></div>",
        800,
        "page 800 300\n"
        "block html 0 0 800 300\n"
        "  block body 0 0 800 300\n"
        "    block div 0 0 100 100 float-left\n"
        "    block div 0 100 800 100 clearance=100\n"
        "      block div 0 100 100 100\n"
        "        line 0 100 100 20 base 14\n"
        "          atomic i 0 110 20 10\n"
        "            block i 0 110 20 10\n"
        "          atomic i 20 110 20 10\n"
        "            block i 20 110 20 10\n"
        "          atomic i 40 110 20 10\n"
        "            block i 40 110 20 10\n"
        "          atomic i 60 110 20 10\n"
        "            block i 60 110 20 10\n"
        "          atomic i 80 110 20 10\n"
        "            block i 80 110 20 10\n"
        "        line 0 120 100 20 base 14\n"
        "          atomic i 0 130 20 10\n"
        "            block i 0 130 20 10\n"
        "          atomic i 20 130 20 10\n"
        "            block i 20 130 20 10\n"
        "          atomic i 40 130 20 10\n"
        "            block i 40 130 20 10\n"
        "          atomic i 60 130 20 10\n"
        "            block i 60 130 20 10\n"
        "          atomic i 80 130 20 10\n"
        "            block i 80 130 20 10\n"
        "    block div 0 200 100 100\n"
        "      block div 0 200 100 0\n");

    // WPT flex-aspect-ratio-021 and -022: under `flex-basis: content`
    // the base is the ratio's from the definite cross size (Flexbox 1 §
    // 9.2.3 B) though a main size is given — a row item 100 high at 1 and
    // given 20 wide is 100 wide; a column item 100 wide given 20 high is
    // 100 high.
    cascade_layout_case("aspect-ratio: flex-basis content takes the ratio (WPT flex-aspect-ratio-021, -022)",
        "<!doctype html><style>body { margin: 0 }</style><div style='display: flex'><div"
        " style='flex-basis: content; height: 100px; aspect-ratio: 1/1; width: 20px;"
        " min-width: 0'></div></div><div style='display: flex; flex-direction: column'><div"
        " style='flex-basis: content; width: 100px; aspect-ratio: 1/1; height: 20px;"
        " min-height: 0'></div></div>", 800,
        "page 800 200\n"
        "block html 0 0 800 200\n"
        "  block body 0 0 800 200\n"
        "    block div 0 0 800 100\n"
        "      block div 0 0 100 100\n"
        "    block div 0 100 800 100\n"
        "      block div 0 100 100 100\n");

    // ── text-overflow (CSS Overflow 4 § 4.1) ──

    // Not inherited; the line is laid out as it would be, past its block:
    // where it is cut and the ellipsis goes is the painter's.
    cascade_case("text-overflow: not inherited", 800,
        "<!doctype html><style>div { text-overflow: ellipsis } p { text-overflow: clip }"
        "</style><div><p>a</p><span>b</span></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block text-overflow=ellipsis holds-block\n"
        "      p block margin=16/0/16/0\n"
        "      span inline\n");
    ellipsis_door_cases();

    // ── object-fit and object-position (CSS Images 3 § 4.5, § 4.6) ──

    // Neither inherited; inherit takes the parent's. `right 1em top 2px`
    // is 100% less 16, and 2; a three-part position is a background's
    // alone, so `right 1em top` would be no declaration at all.
    cascade_case("object-fit and object-position", 800,
        "<!doctype html><style>img { object-fit: cover; object-position: right 1em top 2px }"
        " p { object-fit: inherit; object-position: inherit } span { object-position: center }"
        " b { object-position: right 1em top }</style>"
        "<div style='object-fit: none; object-position: 10%'><p><img src=a.png><span>s</span>"
        "<b>b</b></div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block object-fit=none object-position=10%/50% holds-block\n"
        "      p block margin=16/0/16/0 object-fit=none object-position=10%/50%\n"
        "        img inline object-fit=cover object-position=100%-16/2\n"
        "        span inline\n"
        "        b inline weight=700\n");

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

    // The cold reader's case: 10px across two element edges. ab is 8 + 10
    // + 8 + 10 = 36, cd from 36 the same, ef from 72 ends the line, 8 + 10
    // + 4, 22. A line ending at a break leaves out its last spacing too:
    // abc 44, def 40. Its max-content width leaves out the same: an
    // inline-block of abc shrinks to 44.
    cascade_layout_case("letter-spacing: spaced across an element's edge, not at a line's end",
        "<!doctype html><style>body { margin: 0 } p { margin: 0; letter-spacing: 10px }"
        " b { font-weight: normal } i { display: inline-block; font-style: normal }</style>"
        "<p>ab<b>cd</b>ef</p><p>abc<br>def</p><p><i>abc</i></p>", 300,
        "page 300 80\n"
        "block html 0 0 300 80\n"
        "  block body 0 0 300 80\n"
        "    block p 0 0 300 20\n"
        "      line 0 0 300 20 base 14\n"
        "        span b 36 2 36 16\n"
        "        text \"ab\" 0 2 36 16 serif 16\n"
        "        text \"cd\" 36 2 36 16 serif 16\n"
        "        text \"ef\" 72 2 22 16 serif 16\n"
        "    block p 0 20 300 40\n"
        "      line 0 20 300 20 base 14\n"
        "        text \"abc\" 0 22 44 16 serif 16\n"
        "      line 0 40 300 20 base 14\n"
        "        text \"def\" 0 42 40 16 serif 16\n"
        "    block p 0 60 300 20\n"
        "      line 0 60 300 20 base 14\n"
        "        atomic i 0 60 44 20\n"
        "          block i 0 60 44 20\n"
        "            line 0 60 44 20 base 14\n"
        "              text \"abc\" 0 62 44 16 serif 16\n");

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
    cascade_case("object-fit: unknown with the Modern Web switch off", 800,
        "<!doctype html><style>img { object-fit: cover; object-position: 0 0 }</style>"
        "<img src=a.png>",
        PAGE_OPEN
        "  body block margin=8/8/8/8\n"
        "    img inline\n");
    cascade_case("text-overflow: unknown with the Modern Web switch off", 800,
        "<!doctype html><style>div { text-overflow: ellipsis }</style><div>a</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block\n");
    cascade_layout_case("aspect-ratio: unknown with the Modern Web switch off",
        "<!doctype html><style>body { margin: 0 } div { width: 100px; aspect-ratio: 2 }</style>"
        "<div></div>", 200,
        "page 200 0\n"
        "block html 0 0 200 0\n"
        "  block body 0 0 200 0\n"
        "    block div 0 0 100 0\n");
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
