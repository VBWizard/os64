// Slice S7c, the text clip (docs/design/pending/PILE3.md § The text clip):
// the fill colour's cascade and which text a background clipped to its
// text is cut to. Every answer is worked by hand from Backgrounds 4
// § 2.8, the Compatibility Standard § 3.4.6.1, and Chrome's table in the
// design note — the probe pages Chrome was asked.

#define CLIP_STYLE "<!doctype html><style>body{margin:0} p{margin:0}"

// Every TEXT box's text that flow_visit_text_clip hands over, each between
// bars: "|a|b|".
typedef struct {
    char text[512];
    size_t len;
} ClipTexts;

static void clip_text_visit(void *ctx, const flow_box_t *b)
{
    ClipTexts *t = ctx;
    if (b->kind != FLOW_BOX_TEXT)
        return;     // a line whose ellipsis the clip holds
    size_t n = b->length;
    if (t->len == 0 && t->len + 1 < sizeof(t->text))
        t->text[t->len++] = '|';
    if (t->len + n + 2 >= sizeof(t->text))
        return;
    memcpy(t->text + t->len, b->text, n);
    t->len += n;
    t->text[t->len++] = '|';
    t->text[t->len] = '\0';
}

static ClipTexts clip_texts(const flow_box_t *box)
{
    ClipTexts t = {{0}, 0};
    flow_visit_text_clip(box, clip_text_visit, &t);
    return t;
}

// Whether some box handed over holds `word` (a box keeps the spaces round
// its words).
static bool clip_holds(const ClipTexts *t, const char *word)
{
    return strstr(t->text, word) != NULL;
}

static void cliptext_cases(void)
{
    // The fill colour is inherited, and currentColor in it is a keyword:
    // each element fills with its OWN colour unless a colour is written.
    // The div fills transparent over red; its first p inherits both; the
    // second is blue and still fills transparent; the third writes
    // currentColor back and fills with its green. `initial` is currentColor
    // too, so the fourth fills with its inherited red; `unset` on an
    // inherited property inherits, so the fifth fills transparent. The
    // section never writes one and fills with whatever its colour is, so
    // its p fills blue.
    cascade_case("text clip: the fill colour inherits, currentColor resolves per element", 800,
        CLIP_STYLE "div{color:red;-webkit-text-fill-color:transparent} .b{color:blue}"
        " .c{color:green;-webkit-text-fill-color:currentcolor}"
        " .n{-webkit-text-fill-color:initial} .u{-webkit-text-fill-color:unset}"
        " section{color:red} section p{color:blue}</style>"
        "<div><p>x<p class=b>y<p class=c>z<p class=n>n<p class=u>u</div>"
        "<section><p>w</p></section>",
        PAGE_OPEN
        "  body block holds-block\n"
        "    div block color=#ff0000 fill=#000000/0 holds-block\n"
        "      p block fill=#000000/0\n"
        "      p block color=#0000ff fill=#000000/0\n"
        "      p block color=#008000\n"
        "      p block\n"
        "      p block fill=#000000/0\n"
        "    section block color=#ff0000 holds-block\n"
        "      p block color=#0000ff\n");

    // Which text a block's clip holds — each row of Chrome's table: its
    // own, a relatively positioned span's, a plain inline-block's, a
    // float's, one below full opacity, a block child's, and a relatively
    // positioned block with a z-index; NOT an absolutely positioned one's,
    // a fixed one's, nor a transformed inline-block's.
    Sheeted q;
    sheeted_open(&q, CLIP_STYLE ".g{width:600px}</style>"
                     "<div class=g id=g>own <span style=position:relative>rel</span> "
                     "<span style=display:inline-block>ib</span> "
                     "<span style=display:inline-block;transform:rotate(10deg)>turned</span> "
                     "<span style=float:left>fl</span> <span style=opacity:.5>op</span> "
                     "<span style=position:absolute>abs</span> "
                     "<span style=position:fixed>fix</span>"
                     "<div>blk</div><div style=position:relative;z-index:1>zi</div></div>",
                 800, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 800, &q.env);
    ClipTexts held = clip_texts(box_with_id(t, q.doc->document, "g"));
    expect("text clip: a block's clip holds its in-flow and floated text, Chrome's rows",
           clip_holds(&held, "own") && clip_holds(&held, "rel") && clip_holds(&held, "ib") &&
               clip_holds(&held, "fl") && clip_holds(&held, "op") && clip_holds(&held, "blk") &&
               clip_holds(&held, "zi"),
           held.text);
    expect("text clip: an absolute, a fixed and a transformed descendant's text is not held",
           !clip_holds(&held, "abs") && !clip_holds(&held, "fix") && !clip_holds(&held, "turned"),
           held.text);
    flow_free(t);
    sheeted_close(&q);

    // A list's marker is not the text: an ol's clip holds its items, not
    // their numbers.
    sheeted_open(&q, CLIP_STYLE "</style><ol id=o><li>one<li>two</ol>", 800, 600);
    t = flow_layout(q.doc, q.page, 800, &q.env);
    held = clip_texts(box_with_id(t, q.doc->document, "o"));
    expect("text clip: a marker is not held, its item's text is",
           clip_holds(&held, "one") && clip_holds(&held, "two") && strstr(held.text, "1") == NULL &&
               strstr(held.text, "2") == NULL,
           held.text);
    flow_free(t);
    sheeted_close(&q);

    // A span's piece holds the text on ITS line that the span holds: "aa"
    // then the span's "bb", a break, and its "cc dd" and an inline-block —
    // the first piece holds bb and not aa, the second cc and dd and the
    // inline-block's text, the atom being the span's on that line.
    sheeted_open(&q, CLIP_STYLE "</style><div id=w>aa <span id=s>bb<br>cc dd "
                     "<i style=display:inline-block>ib</i></span></div>",
                 800, 600);
    t = flow_layout(q.doc, q.page, 800, &q.env);
    const flow_box_t *w = box_with_id(t, q.doc->document, "w");
    const flow_box_t *first = first_of_kind(w, FLOW_BOX_SPAN);
    const flow_box_t *line2 = first != NULL && first->parent != NULL ? first->parent->next : NULL;
    const flow_box_t *second = first_of_kind(line2, FLOW_BOX_SPAN);
    ClipTexts one = clip_texts(first), two = clip_texts(second);
    expect("text clip: a span's piece holds the span's text on its own line",
           first != NULL && second != NULL && clip_holds(&one, "bb") && !clip_holds(&one, "aa") &&
               !clip_holds(&one, "cc") && clip_holds(&two, "cc") && clip_holds(&two, "dd") &&
               clip_holds(&two, "ib") && !clip_holds(&one, "ib"),
           one.text);
    flow_free(t);
    sheeted_close(&q);
}
