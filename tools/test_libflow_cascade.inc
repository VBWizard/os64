// The page's own sheets (GARB.md, G3): libgarb's author winners computed
// into the styles, each dump worked from the standard by hand, the same
// way test_libflow_style.inc's are. The viewport is WIDTH x 600.

static void cascade_cases(void)
{
    // Author rules outrank the chapter and the hints (`text=`); a style
    // attribute outranks every rule; a class outranks a type.
    cascade_case("the author over the chapter and the hints", 800,
        "<!doctype html><style>body { margin: 0; color: #123456 } p { margin: 1em 2px }"
        " .x { color: red }</style><body text=blue><p class=x>a<p style='margin-left: 5px'>b",
        PAGE_OPEN
        "  body block color=#123456 holds-block\n"
        "    p block color=#ff0000 margin=16/2/16/2\n"
        "    p block margin=16/2/16/5\n");

    // html 20px. div: 150% of 20 = 30; its 1em margin is its OWN 30; 1rem
    // is the root's 20. span: larger, 30 * 6/5 = 36. b: x-small, 16 * 3/4
    // = 12, and still the chapter's bolder.
    cascade_case("font sizes: %, em, rem, larger, a keyword", 800,
        "<!doctype html><style>html { font-size: 20px } div { font-size: 150%;"
        " margin-top: 1em; padding-left: 1rem } span { font-size: larger }"
        " b { font-size: x-small }</style><div>a<span>b</span><b>c</b></div>",
        "html block size=20 holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block size=30 margin=30/0/0/0 padding=0/0/0/20\n"
        "      span inline size=36\n"
        "      b inline weight=700 size=12\n");

    // initial: margins 0, upright. inherit and unset (color inherits): the
    // parent's green, so nothing prints. revert: the chapter's own value —
    // a link's underline, h1's 2em, over an earlier author 10px. unset on
    // a margin: initial, 0.
    cascade_case("the CSS-wide keywords", 800,
        "<!doctype html><style>p { color: green; margin: initial }"
        " em { color: inherit; font-style: initial } a { color: unset;"
        " text-decoration: revert } h1 { font-size: 10px }"
        " h1 { font-size: revert; margin: unset }</style>"
        "<p>a<em>b</em><a href=x>c</a></p><h1>t</h1>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block color=#008000\n"
        "      em inline\n"
        "      a inline underline\n"
        "    h1 block weight=700 size=32\n");

    // calc(50% - 10px) keeps both halves; 10vh of 600 is 60; 2 * 4px + 1em
    // is 8 + 16.
    cascade_case("var(), calc() and the viewport", 800,
        "<!doctype html><style>:root { --w: 50%; --pad: 3px } div { width: calc(var(--w) - 10px);"
        " padding: var(--pad); height: 10vh; margin-left: calc(2 * 4px + 1em) }</style>"
        "<div>x</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block margin=0/0/0/24 padding=3/3/3/3 width=50%-10 height=60\n");

    // An inline li leaves its ul holding no block. dashed is drawn solid.
    cascade_case("display and borders", 800,
        "<!doctype html><style>li { display: inline } span { display: block;"
        " border: 2px dashed blue } i { border-style: solid; display: none }</style>"
        "<ul><li>a</li></ul><span>s</span><i>x</i>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    ul block margin=16/0/16/0 padding=0/0/0/40\n"
        "      li inline\n"
        "    span block border=2:solid:#0000ff\n"
        "    i none\n");

    // A style with no width anywhere is medium, 3px; the colour is
    // currentColor, red.
    cascade_case("a border style alone is medium and currentColor", 800,
        "<!doctype html><style>div { border-top-style: solid; color: red }</style><div>x</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block color=#ff0000 border=3:solid:#ff0000/0:none:#ff0000/0:none:#ff0000"
        "/0:none:#ff0000\n");

    // `border-width: revert` goes back to the user-agent origin, which
    // declared no width for a div: the style the author drew is medium.
    cascade_case("a width reverted is no width declared", 800,
        "<!doctype html><style>div { border-style: solid; border-width: revert }</style>"
        "<div>x</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block border=3:solid:ink\n");

    // The narrow query holds at 400 and not at 800.
    static const char kMedia[] =
        "<!doctype html><style>p { color: red } @media (max-width: 500px) {"
        " p { color: green } }</style><p>x";
    cascade_case("@media at 800", 800, kMedia,
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block color=#ff0000 margin=16/0/16/0\n");
    cascade_case("@media at 400", 400, kMedia,
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block color=#008000 margin=16/0/16/0\n");

    // Quirks: the first p in the body loses its top margin (§15.3.9).
    // Families as the sheet wrote them; Georgia alone guesses serif.
    cascade_case("font families, in quirks mode", 800,
        "<style>p { font-family: \"Courier New\", Arial, monospace; font-weight: bold }"
        " q { font-family: Georgia }</style><p>a<q>b</q>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    p block font=\"Courier New\",\"Arial\",mono weight=700 margin=0/0/16/0\n"
        "      q inline font=\"Georgia\",serif\n");

    // The layout dumps are test_libflow_layout.inc's: 16px text is 20
    // tall with its content area 2 below the line's top.

    // Negative margins collapse too (CSS 2.1 § 8.3.1): the largest
    // positive plus the most negative. body's 8 collapses with the first
    // p's 0 at y 8; that p ends at 28. Then 0 and -10 give -10: the second
    // p at 18, ending 38; body 8..38, and its 8 below makes the page 46.
    cascade_layout_case("a negative margin pulls a box up",
        "<!doctype html><style>p { margin: 0 } .up { margin-top: -10px }</style>"
        "<p>a<p class=up>b", 200,
        "page 200 46\n"
        "block html 0 0 200 46\n"
        "  block body 8 8 184 30\n"
        "    block p 8 8 184 20\n"
        "      line 8 8 184 20 base 14\n"
        "        text \"a\" 8 10 8 16 serif 16\n"
        "    block p 8 18 184 20\n"
        "      line 8 18 184 20 base 14\n"
        "        text \"b\" 8 20 8 16 serif 16\n");

    // 30 below and -10 above: 30 + -10 = 20, so the second p is at 48.
    cascade_layout_case("a positive and a negative margin sum",
        "<!doctype html><style>p { margin: 0 } .a { margin-bottom: 30px }"
        " .b { margin-top: -10px }</style><p class=a>a<p class=b>b", 200,
        "page 200 76\n"
        "block html 0 0 200 76\n"
        "  block body 8 8 184 60\n"
        "    block p 8 8 184 20\n"
        "      line 8 8 184 20 base 14\n"
        "        text \"a\" 8 10 8 16 serif 16\n"
        "    block p 8 48 184 20\n"
        "      line 8 48 184 20 base 14\n"
        "        text \"b\" 8 50 8 16 serif 16\n");

    // Inline-blocks (CSS 2.1 § 10.3.9, § 10.8.1). The span shrinks to its
    // text, "x y" = 8 + 4 + 8 = 20, framed by 3 + 2 a side to 30, after a
    // 4px margin: 12..42, and b at 46. The div is 50% of 200. Each sits on
    // its LAST line's baseline: the div's p is 16 down, so the line's
    // baseline is 16 + 14 = 30 and a, the span's text and b all have their
    // tops at 30 - 12 = 18 — the span's box 5 above its text, at 11. A
    // marquee is as wide as the line, so it wraps below, and its overflow
    // is hidden, so it sits on its bottom edge: that line's baseline is 20,
    // and the strut hangs 6 below it.
    // An auto-width inline-block's percentages are of the LINE's width, as
    // it was measured (CSS 2.1 § 10.3.9, § 8.3): 5% of 200 is 10, so the
    // span's box is 10 + "x" 8 + 10 = 28 wide after a 10px margin, at 18,
    // its text at 28, and b at 18 + 28 + 10 = 56.
    cascade_layout_case("an inline-block's percentages are the line's",
        "<!doctype html><style>body { margin: 0 } span { display: inline-block;"
        " padding: 0 5%; margin: 0 5% }</style>a<span>x</span>b", 200,
        "page 200 20\n"
        "block html 0 0 200 20\n"
        "  block body 0 0 200 20\n"
        "    line 0 0 200 20 base 14\n"
        "      text \"a\" 0 2 8 16 serif 16\n"
        "      atomic span 18 0 28 20\n"
        "        block span 18 0 28 20\n"
        "          line 28 0 8 20 base 14\n"
        "            text \"x\" 28 2 8 16 serif 16\n"
        "      text \"b\" 56 2 8 16 serif 16\n");

    cascade_layout_case("inline-blocks shrink to fit and sit on their last line",
        "<!doctype html><style>body { margin: 0 } span { display: inline-block;"
        " margin: 0 4px; border: 2px solid; padding: 3px } div { display: inline-block;"
        " width: 50% }</style>a<span>x y</span>b<div><p>para</p></div><marquee>m</marquee>",
        200,
        "page 200 78\n"
        "block html 0 0 200 78\n"
        "  block body 0 0 200 78\n"
        "    block anon 0 0 200 78\n"
        "      line 0 0 200 52 base 30\n"
        "        text \"a\" 0 18 8 16 serif 16\n"
        "        atomic span 12 11 30 30\n"
        "          block span 12 11 30 30\n"
        "            line 17 16 20 20 base 14\n"
        "              text \"x y\" 17 18 20 16 serif 16\n"
        "        text \"b\" 46 18 8 16 serif 16\n"
        "        atomic div 54 0 100 52\n"
        "          block div 54 0 100 52\n"
        "            block p 54 16 100 20\n"
        "              line 54 16 100 20 base 14\n"
        "                text \"para\" 54 18 28 16 serif 16\n"
        "      line 0 52 200 26 base 20\n"
        "        atomic marquee 0 52 200 20\n"
        "          block marquee 0 52 200 20\n"
        "            line 0 52 200 20 base 14\n"
        "              text \"m\" 0 54 12 16 serif 16\n");
}
