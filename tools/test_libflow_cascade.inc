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

    // The limits and box-sizing as the struct holds them; `none` is none.
    cascade_case("min-width, max-width, box-sizing", 800,
        "<!doctype html><style>div { min-width: 10px; max-width: 50%; max-height: none;"
        " min-height: 2em; box-sizing: border-box }</style><div>x</div>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block min-width=10 max-width=50% min-height=32 border-box\n");

    // 150% of the div's 10px computes to 15px, which the span inherits
    // as 15px whatever its own size; 1.5 stays a number. 2em of 10 is 20.
    cascade_case("line-height, text-indent, text-transform as they inherit", 800,
        "<!doctype html><style>div { line-height: 150%; font-size: 10px; text-indent: 2em }"
        " span { font-size: 20px } p { line-height: 1.5; text-transform: uppercase }</style>"
        "<div>x<span>y</span></div><p>z</p>",
        PAGE_OPEN
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block size=10 line-height=15 indent=20\n"
        "      span inline size=20\n"
        "    p block margin=16/0/16/0 line-height=1.500 transform=uppercase\n");

    // A background picture keeps its url() as written and the sheet it
    // came from (0: the page's first sheet; -1: a style attribute, which
    // wins here). `right 10px bottom` is calc(100% - 10px) and 100%.
    cascade_case("background pictures, their repeat and position", 800,
        "<!doctype html><style>body { background: url(sky.png) no-repeat right 10px bottom }"
        " p { background-image: url(a.gif) }</style>"
        "<p style='background: url(b.gif) repeat-x 5px 50%'>x</p>",
        PAGE_OPEN
        "  body block bg-image=\"sky.png\"@0 no-repeat 100%-10/100% margin=8/8/8/8 holds-block\n"
        "    p block bg-image=\"b.gif\"@-1 repeat-x 5/50% margin=16/0/16/0\n");

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
    // marquee is as wide as the line, so it wraps below.
    cascade_layout_case("inline-blocks shrink to fit and sit on their last line",
        "<!doctype html><style>body { margin: 0 } span { display: inline-block;"
        " margin: 0 4px; border: 2px solid; padding: 3px } div { display: inline-block;"
        " width: 50% }</style>a<span>x y</span>b<div><p>para</p></div><marquee>m</marquee>",
        200,
        "page 200 72\n"
        "block html 0 0 200 72\n"
        "  block body 0 0 200 72\n"
        "    block anon 0 0 200 72\n"
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
        "      line 0 52 200 20 base 14\n"
        "        atomic marquee 0 52 200 20\n"
        "          block marquee 0 52 200 20\n"
        "            line 0 52 200 20 base 14\n"
        "              text \"m\" 0 54 12 16 serif 16\n");

    // G5: box-sizing and the limits (CSS 2.1 § 10.4, CSS Sizing 3 § 4.1).
    // The div's frame is 2 + 3 a side, 10; auto would make it 190 wide,
    // max-width holds it to 100, and then its auto margins share the 90
    // left over: 45 each. Its text starts 5 in, at 50, 7 down. The p is 50
    // wide AS A BORDER BOX: its content 50 - 12 = 38, starting 6 in.
    cascade_layout_case("max-width centres with auto margins; border-box",
        "<!doctype html><style>body { margin: 0 } div { max-width: 100px; margin: 0 auto;"
        " border: 2px solid; padding: 3px } p { margin: 0; width: 50px;"
        " box-sizing: border-box; padding: 5px; border: 1px solid }</style>"
        "<div>x</div><p>y</p>",
        200,
        "page 200 62\n"
        "block html 0 0 200 62\n"
        "  block body 0 0 200 62\n"
        "    block div 45 0 110 30\n"
        "      line 50 5 100 20 base 14\n"
        "        text \"x\" 50 7 8 16 serif 16\n"
        "    block p 0 30 50 32\n"
        "      line 6 36 38 20 base 14\n"
        "        text \"y\" 6 38 8 16 serif 16\n");

    // A min-height stops the bottom margin collapsing through (§ 8.3.1):
    // the first p's 10 still collapses up through the div and body (to y
    // 10), but its bottom 10 stays inside the div, whose content is
    // 20 + 10 = 30, held up to 50. The next p is 10 below that, at 70.
    cascade_layout_case("min-height holds a box open",
        "<!doctype html><style>body { margin: 0 } div { min-height: 50px }"
        " p { margin: 10px 0 }</style><div><p>a</p></div><p>b</p>",
        200,
        "page 200 100\n"
        "block html 0 0 200 100\n"
        "  block body 0 10 200 80\n"
        "    block div 0 10 200 50\n"
        "      block p 0 10 200 20\n"
        "        line 0 10 200 20 base 14\n"
        "          text \"a\" 0 12 8 16 serif 16\n"
        "    block p 0 70 200 20\n"
        "      line 0 70 200 20 base 14\n"
        "        text \"b\" 0 72 8 16 serif 16\n");

    // `img { max-width: 100% }` in a 20px box: the 40x30 picture is held
    // to 20 wide and keeps its ratio, 15 tall. On its line with the
    // strut (14 above the baseline, 6 below) it makes the line 21.
    cascade_layout_case("a picture's max-width keeps its ratio",
        "<!doctype html><style>body { margin: 0 } div { width: 20px }"
        " img { max-width: 100% }</style><div><img src=known.png></div>",
        200,
        "page 200 21\n"
        "block html 0 0 200 21\n"
        "  block body 0 0 200 21\n"
        "    block div 0 0 20 21\n"
        "      line 0 0 20 21 base 15\n"
        "        atomic img 0 0 20 15\n");

    // line-height (CSS 2.1 § 10.8.1). The face's ascent and descent are 16
    // of it; the rest is leading, half above and half below. 30px: 7
    // each way, so the line is 30 with its baseline 12 + 7 = 19 down and
    // the text's top at 7. A number, 2, is 32 of a 16px font: 8 each way,
    // baseline 20, the line from 30 to 62.
    cascade_layout_case("line-height as a length and as a number",
        "<!doctype html><style>body { margin: 0 } p { margin: 0; line-height: 30px }"
        " div { line-height: 2 }</style><p>a</p><div>b</div>",
        200,
        "page 200 62\n"
        "block html 0 0 200 62\n"
        "  block body 0 0 200 62\n"
        "    block p 0 0 200 30\n"
        "      line 0 0 200 30 base 19\n"
        "        text \"a\" 0 7 8 16 serif 16\n"
        "    block div 0 30 200 32\n"
        "      line 0 30 200 32 base 20\n"
        "        text \"b\" 0 38 8 16 serif 16\n");

    // text-indent moves each p's first line 20 in; text-transform changes
    // the letters and not their places (every letter here is 8 wide, the
    // space 4: "Ab Cd" is 36). pre-line collapses the spaces, drops the one
    // before the newline, and breaks there: "a b", then "c".
    cascade_layout_case("text-indent, text-transform, pre-line",
        "<!doctype html><style>body { margin: 0 } p { margin: 0; text-indent: 20px }"
        " .u { text-transform: uppercase } .c { text-transform: capitalize }"
        " div { white-space: pre-line }</style>"
        "<p>ab</p><p class=u>ab</p><p class=c>ab cd</p><div>a   b \n  c</div>",
        200,
        "page 200 100\n"
        "block html 0 0 200 100\n"
        "  block body 0 0 200 100\n"
        "    block p 0 0 200 20\n"
        "      line 0 0 200 20 base 14\n"
        "        text \"ab\" 20 2 16 16 serif 16\n"
        "    block p 0 20 200 20\n"
        "      line 0 20 200 20 base 14\n"
        "        text \"AB\" 20 22 16 16 serif 16\n"
        "    block p 0 40 200 20\n"
        "      line 0 40 200 20 base 14\n"
        "        text \"Ab Cd\" 20 42 36 16 serif 16\n"
        "    block div 0 60 200 40\n"
        "      line 0 60 200 20 base 14\n"
        "        text \"a b\" 0 62 20 16 serif 16\n"
        "      line 0 80 200 20 base 14\n"
        "        text \"c\" 0 82 8 16 serif 16\n");

    // An atom whose margins outrun the line has no content width, never a
    // negative one: 150 + 150 of margin in 184 leaves the marquee 0 wide at
    // 8 + 150, its x overflowing it.
    cascade_layout_case("an inline-block's margins wider than the line leave it no width",
        "<!doctype html><style>marquee { margin: 0 150px }</style><marquee>x</marquee>",
        200,
        "page 200 36\n"
        "block html 0 0 200 36\n"
        "  block body 8 8 184 20\n"
        "    line 8 8 184 20 base 14\n"
        "      atomic marquee 158 8 0 20\n"
        "        block marquee 158 8 0 20\n"
        "          line 158 8 0 20 base 14\n"
        "            text \"x\" 158 10 8 16 serif 16\n");

    // overflow (CSS Overflow 3). The div hides: its content is cut to its
    // padding box, 50 by 10, which every box inside carries as its clip;
    // and it starts a block formatting context, so the p's 5px top margin
    // stays inside it instead of moving it down. Its own overflow stops at
    // its border box, so the page is the div's 10 tall.
    cascade_layout_case("overflow: hidden clips, and holds a margin in",
        "<!doctype html><style>body { margin: 0 } div { width: 50px; height: 10px;"
        " overflow: hidden } p { margin: 5px 0 0; width: 100px }</style>"
        "<div><p>ab cd ef</p></div>",
        200,
        "page 200 10\n"
        "block html 0 0 200 10\n"
        "  block body 0 0 200 10\n"
        "    block div 0 0 50 10\n"
        "      block p 0 5 100 20 clip 0 0 50 10\n"
        "        line 0 5 100 20 base 14 clip 0 0 50 10\n"
        "          text \"ab cd ef\" 0 7 52 16 serif 16 clip 0 0 50 10\n");

    // overflow-x alone: the y axis computes to auto, which is drawn
    // unclipped, so the clip is x's and the y span is everything.
    cascade_layout_case("overflow-x: hidden clips one axis",
        "<!doctype html><style>body { margin: 0 } div { width: 30px; overflow-x: hidden }"
        " p { margin: 0; width: 60px }</style><div><p>ab</p></div>",
        200,
        "page 200 20\n"
        "block html 0 0 200 20\n"
        "  block body 0 0 200 20\n"
        "    block div 0 0 30 20\n"
        "      block p 0 0 60 20 clip 0 -1073741824 30 2147483647\n"
        "        line 0 0 60 20 base 14 clip 0 -1073741824 30 2147483647\n"
        "          text \"ab\" 0 2 16 16 serif 16 clip 0 -1073741824 30 2147483647\n");
}
