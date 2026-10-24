// Boxes placed above their origin, each dump worked from the standard by
// hand (the font facts are test_libflow_layout.inc's). A box laid out from
// 0 and moved into place after — an inline-block's content, an absolute
// box — puts a negative top margin above its origin, and the page's first
// box can stand above the canvas. A negative y is a place like any other
// (CSS 2.1 § 10.6.3, § 8.3.1): the blocks round it keep their heights.

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
