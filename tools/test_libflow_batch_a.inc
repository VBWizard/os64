// The Modern Web arc's slice 4, CSS batch A (MODERN_WEB_BRIEFS.md § Slice
// 4): each property computed from an author sheet and, where it moves
// anything, laid out — every dump worked from the specification by hand,
// as test_libflow_cascade.inc's are. The viewport is WIDTH x 600.

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
