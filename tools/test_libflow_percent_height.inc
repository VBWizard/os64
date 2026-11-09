// A percentage height (CSS 2.1 § 10.5): of the containing block's height
// where that does not wait on its content — a length, a percentage that
// itself resolved, an absolute box's, and above the root the viewport's —
// and as if `auto` where it does. Included by test_libflow_host.c, whose
// cascade_layout_case lays out at 400 wide in a 600-high viewport.

static void percent_height_cases(void)
{
    cascade_layout_case("percent height: the root's is of the viewport, the body's of the root's",
        "<!doctype html><style>html{height:100%}body{height:100%;margin:0}</style>", 400,
        "page 400 600\n"
        "block html 0 0 400 600\n"
        "  block body 0 0 400 600\n");
    cascade_layout_case("percent height: `inherit` carries the percentage, which resolves anew",
        "<!doctype html><style>html{height:50%}body{height:inherit;margin:0}</style>", 400,
        "page 400 300\n"
        "block html 0 0 400 300\n"
        "  block body 0 0 400 150\n");
    cascade_layout_case("percent height: a full-window button in a centring flex body",
        "<!doctype html><style>html{height:100%}body{height:inherit;display:flex;"
        "align-items:center;justify-content:center;margin:0}"
        "button{display:block;width:100%;height:100%;border:0;padding:0;background:white}</style>"
        "<body><button></button>", 400,
        "page 400 600\n"
        "block html 0 0 400 600\n"
        "  block body 0 0 400 600\n"
        "    block button 0 0 400 600 control 0\n");
    cascade_layout_case("percent height: a fill of a 6px track is 6px",
        "<!doctype html><style>body{margin:0}.t{height:6px}.f{height:100%;width:50%}</style>"
        "<div class=t><div class=f></div></div>", 400,
        "page 400 6\n"
        "block html 0 0 400 6\n"
        "  block body 0 0 400 6\n"
        "    block div 0 0 400 6\n"
        "      block div 0 0 200 6\n");
    cascade_layout_case("percent height: of a parent its content sizes, as if auto",
        "<!doctype html><style>body{margin:0}.f{height:100%}</style>"
        "<div><div class=f></div></div>", 400,
        "page 400 0\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    block div 0 0 400 0\n"
        "      block div 0 0 400 0\n");
    cascade_layout_case("percent height: bars of a 32px row that aligns them to its end",
        "<!doctype html><style>body{margin:0}.r{display:flex;align-items:flex-end;height:32px}"
        ".b{width:10px;height:100%}.h{width:10px;height:50%}</style>"
        "<div class=r><div class=b></div><div class=h></div></div>", 400,
        "page 400 32\n"
        "block html 0 0 400 32\n"
        "  block body 0 0 400 32\n"
        "    block div 0 0 400 32\n"
        "      block div 0 0 10 32\n"
        "      block div 10 16 10 16\n");
    cascade_layout_case("percent height: a border box's is of the content box, its frame within",
        "<!doctype html><style>body{margin:0}.p{height:100px;padding:5px}"
        ".c{box-sizing:border-box;height:100%;padding:10px;border:2px solid}</style>"
        "<div class=p><div class=c></div></div>", 400,
        "page 400 110\n"
        "block html 0 0 400 110\n"
        "  block body 0 0 400 110\n"
        "    block div 0 0 400 110\n"
        "      block div 5 5 390 100\n");
    cascade_layout_case("percent height: min-height of a definite height holds a short box",
        "<!doctype html><style>body{margin:0}.p{height:200px}.c{min-height:50%}</style>"
        "<div class=p><div class=c></div></div>", 400,
        "page 400 200\n"
        "block html 0 0 400 200\n"
        "  block body 0 0 400 200\n"
        "    block div 0 0 400 200\n"
        "      block div 0 0 400 100\n");
    cascade_layout_case("percent height: max-height of a definite height cuts a tall one",
        "<!doctype html><style>body{margin:0}.p{height:200px}.c{height:150px;max-height:25%}</style>"
        "<div class=p><div class=c></div></div>", 400,
        "page 400 200\n"
        "block html 0 0 400 200\n"
        "  block body 0 0 400 200\n"
        "    block div 0 0 400 200\n"
        "      block div 0 0 400 50\n");
    cascade_layout_case("percent height: a limit of an auto height is as if unset",
        "<!doctype html><style>body{margin:0}.c{min-height:50%}</style>"
        "<div><div class=c></div></div>", 400,
        "page 400 0\n"
        "block html 0 0 400 0\n"
        "  block body 0 0 400 0\n"
        "    block div 0 0 400 0\n"
        "      block div 0 0 400 0\n");
    cascade_layout_case("percent height: the parent's used height, its limit applied, is what it is of",
        "<!doctype html><style>body{margin:0}.p{height:300px;max-height:100px}.c{height:50%}</style>"
        "<div class=p><div class=c></div></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      block div 0 0 400 50\n");
    cascade_layout_case("percent height: a chain resolves link by link",
        "<!doctype html><style>html,body{height:100%;margin:0}.a{height:50%}.b{height:50%}</style>"
        "<div class=a><div class=b></div></div>", 400,
        "page 400 600\n"
        "block html 0 0 400 600\n"
        "  block body 0 0 400 600\n"
        "    block div 0 0 400 300\n"
        "      block div 0 0 400 150\n");
    cascade_layout_case("percent height: an absolute box's height is definite for its children",
        "<!doctype html><style>body{margin:0;position:relative;height:200px}"
        ".a{position:absolute;top:0;left:0;width:100px;height:80px}.c{height:50%}</style>"
        "<div class=a><div class=c></div></div>", 400,
        "page 400 200\n"
        "block html 0 0 400 200\n"
        "  block body 0 0 400 200 positioned\n"
        "    line 0 0 400 0 base 14\n"
        "    block div 0 0 100 80 positioned\n"
        "      block div 0 0 100 40\n");
    cascade_layout_case("percent height: a flex container's own resolves, and its row is that tall",
        "<!doctype html><style>html,body{height:100%;margin:0}.r{display:flex;height:50%}"
        ".i{width:10px}</style><div class=r><div class=i></div></div>", 400,
        "page 400 600\n"
        "block html 0 0 400 600\n"
        "  block body 0 0 400 600\n"
        "    block div 0 0 400 300\n"
        "      block div 0 0 10 300\n");
    cascade_layout_case("percent height: an item's percentage of an auto-height row stretches",
        "<!doctype html><style>body{margin:0}.r{display:flex}.i{width:10px;height:100%}"
        ".t{width:10px;height:40px}</style>"
        "<div class=r><div class=i></div><div class=t></div></div>", 400,
        "page 400 40\n"
        "block html 0 0 400 40\n"
        "  block body 0 0 400 40\n"
        "    block div 0 0 400 40\n"
        "      block div 0 0 10 40\n"
        "      block div 10 0 10 40\n");
    cascade_layout_case("percent height: a grid item's is of its area, sized after it: as if auto, stretched",
        "<!doctype html><style>body{margin:0}.g{display:grid;height:100px}.i{height:50%}</style>"
        "<div class=g><div class=i></div></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      block div 0 0 400 100\n");
    cascade_layout_case("percent height: a block inside an inline is of the block round the inline",
        "<!doctype html><style>body{margin:0}.p{height:100px}.c{height:50%}</style>"
        "<div class=p><span><div class=c></div></span></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      block div 0 0 400 50\n");
    cascade_layout_case("percent height: an item's resolved percentage keeps it out of the stretch",
        "<!doctype html><style>body{margin:0}.r{display:flex;height:100px}.i{width:10px;height:50%}</style>"
        "<div class=r><div class=i></div></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      block div 0 0 10 50\n");
    cascade_layout_case("percent height: a picture's, and its width by its ratio",
        "<!doctype html><style>body{margin:0}.p{height:50px}img{display:block;height:100%}</style>"
        "<div class=p><img src=known.png></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 0 400 50\n"
        "    block div 0 0 400 50\n"
        "      replaced img 0 0 67 50\n");
    cascade_layout_case("percent height: a min-height that resolves holds margins in, as a length does",
        "<!doctype html><style>body{margin:0}.p{height:200px}.c{min-height:10%}.c>div{margin-bottom:30px}</style>"
        "<div class=p><div class=c><div></div></div><div>x</div></div>", 400,
        "page 400 200\n"
        "block html 0 0 400 200\n"
        "  block body 0 0 400 200\n"
        "    block div 0 0 400 200\n"
        "      block div 0 0 400 30\n"
        "        block div 0 0 400 0\n"
        "      block div 0 30 400 20\n"
        "        line 0 30 400 20 base 14\n"
        "          text \"x\" 0 32 8 16 serif 16\n");
    cascade_layout_case("percent height: an inline-block among mixed content is of the block holding it",
        "<!doctype html><style>body{margin:0}.p{height:100px}.ib{display:inline-block;width:10px;height:50%}</style>"
        "<div class=p><div></div>x<span class=ib></span></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      block div 0 0 400 0\n"
        "      block anon 0 0 400 56\n"
        "        line 0 0 400 56 base 50\n"
        "          text \"x\" 0 38 8 16 serif 16\n"
        "          atomic span 8 0 10 50\n"
        "            block span 8 0 10 50\n");
    cascade_layout_case("percent height: a picture item of a definite row keeps its ratio",
        "<!doctype html><style>body{margin:0}.r{display:flex;height:100px}img{height:50%}</style>"
        "<div class=r><img src=known.png></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      replaced img 0 0 67 50\n");
    cascade_layout_case("percent height: an absolute box's limit is of the block it is placed in",
        "<!doctype html><style>body{margin:0}.g{position:relative;height:200px}.p{height:100px}"
        ".a{position:absolute;top:0;left:0;width:10px;min-height:50%}</style>"
        "<div class=g><div class=p><div class=a></div></div></div>", 400,
        "page 400 200\n"
        "block html 0 0 400 200\n"
        "  block body 0 0 400 200\n"
        "    block div 0 0 400 200 positioned\n"
        "      block div 0 0 400 100\n"
        "        line 0 0 400 0 base 14\n"
        "        block div 0 0 10 100 positioned\n");
    cascade_layout_case("percent height: a percentage top in an anonymous block is of the block round it",
        "<!doctype html><style>body{margin:0}.p{height:100px}.r{position:relative;top:50%}</style>"
        "<div class=p><div></div>x<span class=r>y</span></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      block div 0 0 400 0\n"
        "      block anon 0 0 400 20\n"
        "        line 0 0 400 20 base 14\n"
        "          span span 8 52 8 16\n"
        "          text \"x\" 0 2 8 16 serif 16\n"
        "          text \"y\" 8 52 8 16 serif 16\n");
    cascade_layout_case("percent height: a max-height of an auto height cuts nothing",
        "<!doctype html><style>body{margin:0}.c{height:40px;max-height:50%}</style>"
        "<div><div class=c></div></div>", 400,
        "page 400 40\n"
        "block html 0 0 400 40\n"
        "  block body 0 0 400 40\n"
        "    block div 0 0 400 40\n"
        "      block div 0 0 400 40\n");
    cascade_layout_case("percent height: an inline picture's is of the block its line is in",
        "<!doctype html><style>body{margin:0}</style>"
        "<div style='height:50px'><img src=known.png style='height:100%'></div>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 0 400 50\n"
        "    block div 0 0 400 50\n"
        "      line 0 0 400 56 base 50\n"
        "        atomic img 0 0 67 50\n");
    cascade_layout_case("percent height: an inline-block's, with a ratio, gives its width",
        "<!doctype html><style>body{margin:0}</style>"
        "<div style='height:100px'><span style='display:inline-block;aspect-ratio:1;height:50%'></span></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 400 100\n"
        "      line 0 0 400 56 base 50\n"
        "        atomic span 0 0 50 50\n"
        "          block span 0 0 50 50\n");
}
