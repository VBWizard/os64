// Two rules a search page's carousel broke at once, each dump worked from
// the standard by hand (the font facts are test_libflow_layout.inc's).
//
// ORIGIN: a box laid out from 0 and moved into place after — an
// inline-block's content, an absolute box — puts a negative top margin
// above its origin, and the page's first box can stand above the canvas.
// A negative y is a place like any other (CSS 2.1 § 10.6.3, § 8.3.1): the
// blocks round it keep their heights.
//
// NOWRAP: the break opportunity between an inline-block and its neighbour
// is the `white-space` of the nearest element holding both (CSS Text 3 §
// 5.1, CSS 2.1 § 16.6), not the inline-block's own, which governs only
// its content.

static void origin_cases(void)
{
    // .v's -12 collapses with .c's and body's top margins (none of them
    // has a border or padding to stop it), and html's do not collapse
    // (§ 8.3.1): body, .c and .v stand at -12, 100 tall, to 88. .a
    // follows at 88, to 98: body is -12..98, 110, and html 0..98.
    cascade_layout_case("a negative margin carries the first box above the canvas, whole",
        "<!doctype html><style>body{margin:0} .c{width:250px} .v{margin-top:-12px}"
        " .h{height:100px} .a{height:10px}</style>"
        "<div class=c><div class=v><div class=h></div></div></div><div class=a></div>", 800,
        "page 800 98\n"
        "block html 0 0 800 98\n"
        "  block body 0 -12 800 110\n"
        "    block div 0 -12 250 100\n"
        "      block div 0 -12 250 100\n"
        "        block div 0 -12 250 100\n"
        "    block div 0 88 800 10\n");

    // The inline-block's content is laid out from 0, its border edge at
    // -30 there: the line inside is its content, 20 tall, and so is every
    // block round it. On the outer line it sits on its baseline (§ 10.8.1):
    // its last line's, 14 below its border edge, so its margin box reaches
    // 14 - 30 above the baseline (nothing) and 6 below it, the strut's 14
    // and 6. The line is 20 and the box at its top.
    cascade_layout_case("an inline-block with a negative top margin keeps its height",
        "<!doctype html><style>body{margin:0}"
        " .i{display:inline-block;width:100px;margin-top:-30px}</style>"
        "<div class=i><div><div>x</div></div></div>", 800,
        "page 800 20\n"
        "block html 0 0 800 20\n"
        "  block body 0 0 800 20\n"
        "    block anon 0 0 800 20\n"
        "      line 0 0 800 20 base 14\n"
        "        atomic div 0 0 100 20\n"
        "          block div 0 0 100 20\n"
        "            block div 0 0 100 20\n"
        "              block div 0 0 100 20\n"
        "                line 0 0 100 20 base 14\n"
        "                  text \"x\" 0 2 8 16 serif 16\n");

    // An absolute box establishes a formatting context, so .m's -10 stays
    // inside it (§ 8.3.1): .m stands at 50 - 10 = 40, 30 tall to 70, and
    // the box, its top at 50, is 20 tall. Laid out from 0, .m was at -10.
    cascade_layout_case("an absolute box whose content starts above its top",
        "<!doctype html><style>body{margin:0}"
        " .p{position:absolute;top:50px;left:0;width:100px} .m{margin-top:-10px}"
        " .h{height:30px}</style><div class=p><div class=m><div class=h></div></div></div>",
        800,
        "page 800 70\n"
        "block html 0 0 800 0\n"
        "  block body 0 0 800 0\n"
        "    line 0 0 800 0 base 14\n"
        "    block div 0 50 100 20 positioned\n"
        "      block div 0 40 100 30\n"
        "        block div 0 40 100 30\n");

    // A table is laid out from 0 too: its absolute box's top is 100 - 50
    // = 50, its two 20px rows at 50 and 70, and the row group round them
    // 50..90 — where, laid out, the rows were at -50 and -30.
    cascade_layout_case("a table's row group above the table's origin holds its rows",
        "<!doctype html><style>body{margin:0} table{border-spacing:0}"
        " td{padding:0;height:20px;width:50px}"
        " .p{position:absolute;top:100px;left:0;margin-top:-50px}</style>"
        "<table class=p><tbody><tr><td>a</td></tr><tr><td>b</td></tr></tbody></table>", 800,
        "page 800 90\n"
        "block html 0 0 800 0\n"
        "  block body 0 0 800 0\n"
        "    line 0 0 800 0 base 14\n"
        "    table table 0 50 50 40 positioned\n"
        "      row-group tbody 0 50 50 40\n"
        "        row tr 0 50 50 20\n"
        "          cell td 0 50 50 20\n"
        "            line 0 50 50 20 base 14\n"
        "              text \"a\" 0 52 8 16 serif 16\n"
        "        row tr 0 70 50 20\n"
        "          cell td 0 70 50 20\n"
        "            line 0 70 50 20 base 14\n"
        "              text \"b\" 0 72 8 16 serif 16\n");

    // A relative box is laid out at its offset (§ 9.4.3), here 50 up from
    // 30, so its content is placed at -20; the flow goes on as if it had
    // not moved: its parent is where it was, 30, and as tall as the box,
    // 20, and the next block follows at 50.
    cascade_layout_case("a relative box moved above the canvas leaves its parent where it was",
        "<!doctype html><style>body{margin:0} .h{height:10px}</style>"
        "<div style='height:30px'></div><div><div style='position:relative;top:-50px;"
        "height:20px'></div></div><div class=h></div>", 800,
        "page 800 60\n"
        "block html 0 0 800 60\n"
        "  block body 0 0 800 60\n"
        "    block div 0 0 800 30\n"
        "    block div 0 30 800 20\n"
        "      block div 0 -20 800 20 positioned\n"
        "    block div 0 50 800 10\n");

    // And one whose parent already holds content above the canvas: the
    // first child's -10 carries the parent and body to -10 (-10..-5); the
    // relative box's place is -5..5, drawn 20 lower at 15. The parent is
    // -10..5, the next block 5..15, body -10..15; the page reaches the
    // moved box's 25.
    cascade_layout_case("a relative box after content above the canvas",
        "<!doctype html><style>body{margin:0}</style><div>"
        "<div style='margin-top:-10px;height:5px'></div>"
        "<div style='position:relative;top:20px;height:10px'></div></div>"
        "<div style='height:10px'></div>", 800,
        "page 800 25\n"
        "block html 0 0 800 15\n"
        "  block body 0 -10 800 25\n"
        "    block div 0 -10 800 15\n"
        "      block div 0 -10 800 5\n"
        "      block div 0 15 800 10 positioned\n"
        "    block div 0 5 800 10\n");

    // A list item inside an inline-block pulled up 30 from a line at 40:
    // its content starts at 10 (laid out from 0, at -30), so the item has
    // content and its marker stands on that, the item 10 tall, not on a
    // line of its own. The marker's "• " is 12 wide, outside the item's
    // 20px of padding: at 8. The inline-block's margin box reaches -30 +
    // 10 below the line's top, so the line is the strut's 20.
    cascade_layout_case("a list item above its origin keeps its marker on its content",
        "<!doctype html><style>body{margin:0}"
        " .i{display:inline-block;vertical-align:top;width:100px;margin-top:-30px}"
        " .h{height:10px}</style><div style='height:40px'></div><div class=i>"
        "<ul style='margin:0;padding:0 0 0 20px'><li><div class=h></div></li></ul></div>", 800,
        "page 800 60\n"
        "block html 0 0 800 60\n"
        "  block body 0 0 800 60\n"
        "    block div 0 0 800 40\n"
        "    block anon 0 40 800 20\n"
        "      line 0 40 800 20 base 14\n"
        "        atomic div 0 10 100 10\n"
        "          block div 0 10 100 10\n"
        "            block ul 0 10 100 10\n"
        "              block li 20 10 80 10\n"
        "                block div 20 10 80 10\n"
        "                marker \"• \" 8 12 12 16\n");

    // A table after a block in the same pulled-up inline-block: the block
    // is 10..20, so the table is placed at 20 (laid out from 0, at -20),
    // its one line of "x" 20 tall, and its row group with it. The
    // inline-block is 30 from 10; its margin box ends at 10 + 30 - 30
    // below the line's top, so the line is the strut's.
    cascade_layout_case("a table above its origin follows the block before it",
        "<!doctype html><style>body{margin:0}"
        " .i{display:inline-block;vertical-align:top;width:100px;margin-top:-30px}"
        " .h{height:10px}</style><div style='height:40px'></div><div class=i><div>"
        "<div class=h></div><table style='border-spacing:0'><tr><td style='padding:0'>x</td>"
        "</tr></table></div></div>", 800,
        "page 800 60\n"
        "block html 0 0 800 60\n"
        "  block body 0 0 800 60\n"
        "    block div 0 0 800 40\n"
        "    block anon 0 40 800 20\n"
        "      line 0 40 800 20 base 14\n"
        "        atomic div 0 10 100 30\n"
        "          block div 0 10 100 30\n"
        "            block div 0 10 100 30\n"
        "              block div 0 10 100 10\n"
        "              table table 0 20 8 20\n"
        "                row-group tbody 0 20 8 20\n"
        "                  row tr 0 20 8 20\n"
        "                    cell td 0 20 8 20\n"
        "                      line 0 20 8 20 base 14\n"
        "                        text \"x\" 0 22 8 16 serif 16\n");

    // A box with nothing in it is still empty: its margins collapse
    // through it with the ones round it (§ 8.3.1), -10 and 5 to -5, so
    // the next box is at 20 - 5 = 15, and the empty one's border edge is
    // where its top margin puts it, 20 - 10 = 10, 0 tall.
    cascade_layout_case("an empty box stays empty, its margins collapsing through",
        "<!doctype html><style>body{margin:0} .h{height:20px}"
        " .e{margin-top:-10px;margin-bottom:5px}</style>"
        "<div class=h></div><div class=e></div><div class=h></div>", 800,
        "page 800 35\n"
        "block html 0 0 800 35\n"
        "  block body 0 0 800 35\n"
        "    block div 0 0 800 20\n"
        "    block div 0 10 800 0\n"
        "    block div 0 15 800 20\n");
}

static void nowrap_atom_cases(void)
{
    // Three 100px inline-blocks in a nowrap box 250 wide: one line, the
    // third past its end, whatever white-space each sets inside.
    cascade_layout_case("inline-blocks in a nowrap box stay on one line",
        "<!doctype html><style>body{margin:0} .s{white-space:nowrap}"
        " .s .i{display:inline-block;vertical-align:top;white-space:normal;width:100px;"
        "height:50px}</style><div style='width:250px'><div class=s><div class=i></div>"
        "<div class=i></div><div class=i></div></div></div><div style='height:10px'></div>",
        800,
        "page 800 60\n"
        "block html 0 0 800 60\n"
        "  block body 0 0 800 60\n"
        "    block div 0 0 250 50\n"
        "      block div 0 0 250 50\n"
        "        line 0 0 250 50 base 14\n"
        "          atomic div 0 0 100 50\n"
        "            block div 0 0 100 50\n"
        "          atomic div 100 0 100 50\n"
        "            block div 100 0 100 50\n"
        "          atomic div 200 0 100 50\n"
        "            block div 200 0 100 50\n"
        "    block div 0 50 800 10\n");

    // The other way round: two inline-blocks nowrap inside, in a normal
    // box 150 wide. Their own white-space is not asked; the box's is, so
    // the second goes to a line of its own. Empty, each sits on its bottom
    // margin edge: 20 above the baseline and the strut's 6 below, 26 a line.
    cascade_layout_case("nowrap inside inline-blocks does not join them on one line",
        "<!doctype html><style>body{margin:0}</style><div style='width:150px'>"
        "<b style='display:inline-block;width:100px;height:20px;white-space:nowrap'></b>"
        "<b style='display:inline-block;width:100px;height:20px;white-space:nowrap'></b></div>",
        800,
        "page 800 52\n"
        "block html 0 0 800 52\n"
        "  block body 0 0 800 52\n"
        "    block div 0 0 150 52\n"
        "      line 0 0 150 26 base 20\n"
        "        atomic b 0 0 100 20\n"
        "          block b 0 0 100 20\n"
        "      line 0 26 150 26 base 20\n"
        "        atomic b 0 26 100 20\n"
        "          block b 0 26 100 20\n");

    // The element holding both sides of a break, not the atom's parent:
    // "xxxx" and the inline-block opening a nowrap span are held by the
    // normal div, so the line may break between them, and the 100px box
    // does not fit after 32 in 120: it starts the second line, the span's
    // empty first piece left at the end of the first.
    cascade_layout_case("an inline-block opening a nowrap span may still go to the next line",
        "<!doctype html><style>body{margin:0}"
        " .i{display:inline-block;vertical-align:top;width:100px;height:20px}</style>"
        "<div style='width:120px'>xxxx<span style='white-space:nowrap'>"
        "<b class=i style='white-space:normal'></b></span></div>", 800,
        "page 800 40\n"
        "block html 0 0 800 40\n"
        "  block body 0 0 800 40\n"
        "    block div 0 0 120 40\n"
        "      line 0 0 120 20 base 14\n"
        "        span span 32 2 0 16\n"
        "        text \"xxxx\" 0 2 32 16 serif 16\n"
        "      line 0 20 120 20 base 14\n"
        "        span span 0 22 100 16\n"
        "        atomic b 0 20 100 20\n"
        "          block b 0 20 100 20\n");

    // And inside a nowrap box, a normal span lets its own two inline-blocks
    // part and nothing else: "xxxx" and the first are held by the box,
    // the two by the span, the second and "yyyy" by the box again. One
    // break, between the two: "xxxx" and the first (132, past the 120),
    // then the second and "yyyy".
    cascade_layout_case("a normal span in a nowrap box breaks only between what it holds",
        "<!doctype html><style>body{margin:0}"
        " .i{display:inline-block;vertical-align:top;width:100px;height:20px}</style>"
        "<div style='width:120px;white-space:nowrap'>xxxx<span style='white-space:normal'>"
        "<b class=i></b><b class=i></b></span>yyyy</div>", 800,
        "page 800 40\n"
        "block html 0 0 800 40\n"
        "  block body 0 0 800 40\n"
        "    block div 0 0 120 40\n"
        "      line 0 0 120 20 base 14\n"
        "        span span 32 2 100 16\n"
        "        text \"xxxx\" 0 2 32 16 serif 16\n"
        "        atomic b 32 0 100 20\n"
        "          block b 32 0 100 20\n"
        "      line 0 20 120 20 base 14\n"
        "        span span 0 22 100 16\n"
        "        atomic b 0 20 100 20\n"
        "          block b 0 20 100 20\n"
        "        text \"yyyy\" 100 22 32 16 serif 16\n");

    // The run is unbreakable, so its min-content width is all of it,
    // 3 x 40 = 120, and a float shrinking to fit in 100 is 120 (§ 10.3.5).
    // An empty inline-block's baseline is its bottom margin edge: 20 above
    // the baseline, nothing below, and the strut 14 and 6, so the line is
    // 26, its baseline 20 down.
    cascade_layout_case("a nowrap run of inline-blocks is a float's min-content width",
        "<!doctype html><style>body{margin:0} .f{float:left} .s{white-space:nowrap}"
        " .i{display:inline-block;white-space:normal;width:40px;height:20px}</style>"
        "<div style='width:100px'><div class=f><div class=s><span class=i></span>"
        "<span class=i></span><span class=i></span></div></div></div>", 800,
        "page 800 26\n"
        "block html 0 0 800 26\n"
        "  block body 0 0 800 0\n"
        "    block div 0 0 100 0\n"
        "      line 0 0 100 0 base 14\n"
        "      block div 0 0 120 26 float-left\n"
        "        block div 0 0 120 26\n"
        "          line 0 0 120 26 base 20\n"
        "            atomic span 0 0 40 20\n"
        "              block span 0 0 40 20\n"
        "            atomic span 40 0 40 20\n"
        "              block span 40 0 40 20\n"
        "            atomic span 80 0 40 20\n"
        "              block span 80 0 40 20\n");

    // The same in a table 50 wide: the first cell's min-content is its
    // unbroken 80, the second's one inline-block, 40 (it may break between
    // them), so the table is 120 (§ 17.5.2.2). The second cell's two lines
    // are 52, the row's height; the first's one line of 26 is in its
    // middle, at 13.
    cascade_layout_case("a nowrap run of inline-blocks is a table cell's min-content width",
        "<!doctype html><style>body{margin:0} table{border-spacing:0} td{padding:0}"
        " .s{white-space:nowrap} .i{display:inline-block;white-space:normal;width:40px;"
        "height:20px}</style><table style='width:50px'><tr><td><div class=s>"
        "<span class=i></span><span class=i></span></div></td><td><span class=i></span>"
        "<span class=i></span></td></tr></table>", 800,
        "page 800 52\n"
        "block html 0 0 800 52\n"
        "  block body 0 0 800 52\n"
        "    table table 0 0 120 52\n"
        "      row-group tbody 0 0 120 52\n"
        "        row tr 0 0 120 52\n"
        "          cell td 0 0 80 52\n"
        "            block div 0 13 80 26\n"
        "              line 0 13 80 26 base 20\n"
        "                atomic span 0 13 40 20\n"
        "                  block span 0 13 40 20\n"
        "                atomic span 40 13 40 20\n"
        "                  block span 40 13 40 20\n"
        "          cell td 80 0 40 52\n"
        "            line 80 0 40 26 base 20\n"
        "              atomic span 80 0 40 20\n"
        "                block span 80 0 40 20\n"
        "            line 80 26 40 26 base 20\n"
        "              atomic span 80 26 40 20\n"
        "                block span 80 26 40 20\n");

    // Both rules at once, in a search page's shape: a strip of five 100px
    // slides, nowrap, in a scrolling viewport inside a grid area two
    // 40px rows tall, all inside a 300px inline-block pulled up 32px.
    // The inline-block is top-aligned on the line at 50, so its border
    // edge is at 18. The grid area is 18..98; the viewport's -4 and -8
    // stay inside the grid item, so it is 14..86 (4 + 60 + 8), its strip
    // 18..78 on one line, 200px of it scrolled out of view. The block
    // after the grid starts at 98, not on top of it; the inline-block is
    // 80 + 30 = 110, its margin box 78 below the line's top, the line's
    // height.
    cascade_layout_case("a strip of slides in a grid area, in a pulled-up inline-block",
        "<!doctype html><style>body{margin:0}"
        " .w{display:inline-block;vertical-align:top;width:300px;margin-top:-32px}"
        " .g{display:grid;grid-template-columns:minmax(0,1fr);grid-auto-rows:40px}"
        " .card{grid-row:span 2;position:relative}"
        " .vp{overflow:hidden;margin-top:-4px;margin-bottom:-8px;padding-top:4px;"
        "padding-bottom:8px} .bar{white-space:nowrap}"
        " .sl{display:inline-block;vertical-align:top;white-space:normal;width:100px;"
        "height:60px} .res{height:30px}</style>"
        "<div style='height:50px'></div><div class=w><div><div class=g><div class=card>"
        "<div class=vp><div class=bar><div class=sl></div><div class=sl></div>"
        "<div class=sl></div><div class=sl></div><div class=sl></div></div></div></div>"
        "</div></div><div class=res></div></div>", 800,
        "page 800 128\n"
        "block html 0 0 800 128\n"
        "  block body 0 0 800 128\n"
        "    block div 0 0 800 50\n"
        "    block anon 0 50 800 78\n"
        "      line 0 50 800 78 base 14\n"
        "        atomic div 0 18 300 110\n"
        "          block div 0 18 300 110\n"
        "            block div 0 18 300 80\n"
        "              block div 0 18 300 80\n"
        "                block div 0 18 300 80 positioned\n"
        "                  block div 0 14 300 72 scroller 0 range 200 0\n"
        "                    block div 0 18 300 60 clip 0 14 300 72\n"
        "                      line 0 18 300 60 base 14 clip 0 14 300 72\n"
        "                        atomic div 0 18 100 60 clip 0 14 300 72\n"
        "                          block div 0 18 100 60 clip 0 14 300 72\n"
        "                        atomic div 100 18 100 60 clip 0 14 300 72\n"
        "                          block div 100 18 100 60 clip 0 14 300 72\n"
        "                        atomic div 200 18 100 60 clip 0 14 300 72\n"
        "                          block div 200 18 100 60 clip 0 14 300 72\n"
        "                        atomic div 300 18 100 60 clip 0 14 300 72\n"
        "                          block div 300 18 100 60 clip 0 14 300 72\n"
        "                        atomic div 400 18 100 60 clip 0 14 300 72\n"
        "                          block div 400 18 100 60 clip 0 14 300 72\n"
        "            block div 0 98 300 30\n");
}
