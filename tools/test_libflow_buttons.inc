// A <button> the page draws (STYLED_BUTTONS.md): which buttons stay the
// face's own widget, and how one laid out is laid out — a formatting
// context's root, as wide as its content, its content centred in the
// height it was given. Included by test_libflow_host.c.

static void button_cases(void)
{
    cascade_layout_case("buttons: text alone, styled nowhere, is the face's widget",
        "<!doctype html><button>Go</button>", 400,
        "page 400 42\n"
        "block html 0 0 400 42\n"
        "  block body 8 8 384 26\n"
        "    line 8 8 384 26 base 20\n"
        "      atomic button 8 8 160 20 control 0\n");
    cascade_layout_case("buttons: an empty one is laid out, not captioned",
        "<!doctype html><button></button>", 400,
        "page 400 36\n"
        "block html 0 0 400 36\n"
        "  block body 8 8 384 20\n"
        "    line 8 8 384 20 base 14\n"
        "      atomic button 8 22 0 0 control 0\n"
        "        block button 8 22 0 0\n");
    cascade_layout_case("buttons: white space alone is empty",
        "<!doctype html><button> </button>", 400,
        "page 400 36\n"
        "block html 0 0 400 36\n"
        "  block body 8 8 384 20\n"
        "    line 8 8 384 20 base 14\n"
        "      atomic button 8 22 0 0 control 0\n"
        "        block button 8 22 0 0\n");
    cascade_layout_case("buttons: a sheet's background lays out a text button",
        "<!doctype html><style>button{background:white}</style><button>Go</button>", 400,
        "page 400 36\n"
        "block html 0 0 400 36\n"
        "  block body 8 8 384 20\n"
        "    line 8 8 384 20 base 14\n"
        "      atomic button 8 8 16 20 control 0\n"
        "        block button 8 8 16 20\n"
        "          line 8 8 16 20 base 14\n"
        "            text \"Go\" 8 10 16 16 serif 16\n");
    cascade_layout_case("buttons: a sheet's border lays it out, even none",
        "<!doctype html><style>button{border:0}</style><button>Go</button>", 400,
        "page 400 36\n"
        "block html 0 0 400 36\n"
        "  block body 8 8 384 20\n"
        "    line 8 8 384 20 base 14\n"
        "      atomic button 8 8 16 20 control 0\n"
        "        block button 8 8 16 20\n"
        "          line 8 8 16 20 base 14\n"
        "            text \"Go\" 8 10 16 16 serif 16\n");
    cascade_layout_case("buttons: the style attribute's radius lays it out",
        "<!doctype html><button style='border-radius:4px'>Go</button>", 400,
        "page 400 36\n"
        "block html 0 0 400 36\n"
        "  block body 8 8 384 20\n"
        "    line 8 8 384 20 base 14\n"
        "      atomic button 8 8 16 20 control 0\n"
        "        block button 8 8 16 20\n"
        "          line 8 8 16 20 base 14\n"
        "            text \"Go\" 8 10 16 16 serif 16\n");
    cascade_layout_case("buttons: a background reverted keeps the face",
        "<!doctype html><style>button{background:revert}</style><button>Go</button>", 400,
        "page 400 42\n"
        "block html 0 0 400 42\n"
        "  block body 8 8 384 26\n"
        "    line 8 8 384 26 base 20\n"
        "      atomic button 8 8 160 20 control 0\n");
    cascade_layout_case("buttons: a colour or a padding alone keeps the face",
        "<!doctype html><style>button{color:red;padding:9px}</style><button>Go</button>", 400,
        "page 400 60\n"
        "block html 0 0 400 60\n"
        "  block body 8 8 384 44\n"
        "    line 8 8 384 44 base 38\n"
        "      atomic button 8 8 178 38 control 0\n");
    cascade_layout_case("buttons: a block-level one is as wide as its content",
        "<!doctype html><style>body{margin:0}button{display:block;border:0;padding:0;font:16px serif}"
        "</style><button>Go</button>", 400,
        "page 400 20\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    block button 0 0 16 20 control 0\n"
        "      line 0 0 16 20 base 14\n"
        "        text \"Go\" 0 2 16 16 serif 16\n");
    cascade_layout_case("buttons: a set width is kept",
        "<!doctype html><style>body{margin:0}button{display:block;border:0;padding:0;width:50%}"
        "</style><button>Go</button>", 400,
        "page 400 20\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    block button 0 0 200 20 control 0\n"
        "      line 0 0 200 20 base 14\n"
        "        text \"Go\" 92 2 16 16 serif 16\n");
    cascade_layout_case("buttons: content is centred in the height it was given",
        "<!doctype html><style>body{margin:0}button{height:100px;border:0;padding:0;font:16px serif}"
        "</style><button>Go</button>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    line 0 0 400 100 base 54\n"
        "      atomic button 0 0 16 100 control 0\n"
        "        block button 0 0 16 100\n"
        "          line 0 40 16 20 base 14\n"
        "            text \"Go\" 0 42 16 16 serif 16\n");
    cascade_layout_case("buttons: and a block-level one's",
        "<!doctype html><style>body{margin:0}button{display:block;height:100px;border:0;padding:0;"
        "font:16px serif}</style><button>Go</button>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block button 0 0 16 100 control 0\n"
        "      line 0 40 16 20 base 14\n"
        "        text \"Go\" 0 42 16 16 serif 16\n");
    cascade_layout_case("buttons: a flex one places its items itself",
        "<!doctype html><style>body{margin:0}button{display:flex;height:100px;border:0;padding:0;"
        "font:16px serif}</style><button><span>a</span><span>b</span></button>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block button 0 0 16 100 control 0\n"
        "      block span 0 0 8 100\n"
        "        line 0 0 8 20 base 14\n"
        "          text \"a\" 0 2 8 16 serif 16\n"
        "      block span 8 0 8 100\n"
        "        line 8 0 8 20 base 14\n"
        "          text \"b\" 8 2 8 16 serif 16\n");
    cascade_layout_case("buttons: its ::before is drawn once it is laid out",
        "<!doctype html><style>body{margin:0}button{border:0;padding:0;font:16px serif}"
        "button::before{content:'x'}</style><button></button>", 400,
        "page 400 20\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    line 0 0 400 20 base 14\n"
        "      atomic button 0 0 8 20 control 0\n"
        "        block button 0 0 8 20\n"
        "          line 0 0 8 20 base 14\n"
        "            span button 0 2 8 16\n"
        "            text \"x\" 0 2 8 16 serif 16\n");
    cascade_layout_case("buttons: a formatting context's root, which holds its floats",
        "<!doctype html><style>body{margin:0}button{display:block;border:0;padding:0}"
        "span{float:left;width:10px;height:50px}</style><button><span></span></button>", 400,
        "page 400 50\n"
        "block html 0 0 400 50\n"
        "  block body 0 0 400 50\n"
        "    block button 0 0 10 50 control 0\n"
        "      line 10 0 0 0 base 14\n"
        "      block span 0 0 10 50 float-left\n");
    cascade_layout_case("buttons: a ::before that draws lays out a text button",
        "<!doctype html><style>body{margin:0}button{padding:0;font:16px serif}"
        "button::before{content:'*'}</style><button>Go</button>", 400,
        "page 400 20\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    line 0 0 400 20 base 14\n"
        "      atomic button 0 0 24 20 control 0\n"
        "        block button 0 0 24 20\n"
        "          line 0 0 24 20 base 14\n"
        "            span button 0 2 8 16\n"
        "            text \"*\" 0 2 8 16 serif 16\n"
        "            text \"Go\" 8 2 16 16 serif 16\n");
    cascade_layout_case("buttons: an inline one is an atom all the same",
        "<!doctype html><style>body{margin:0}button{display:inline;background:none;border:0;padding:0;"
        "font:16px serif}</style><p>x <button>more</button> y", 400,
        "page 400 52\n"
        "block html 0 0 400 52\n"
        "  block body 0 16 400 20\n"
        "    block p 0 16 400 20\n"
        "      line 0 16 400 20 base 14\n"
        "        text \"x \" 0 18 12 16 serif 16\n"
        "        atomic button 12 16 32 20 control 0\n"
        "          block button 12 16 32 20\n"
        "            line 12 16 32 20 base 14\n"
        "              text \"more\" 12 18 32 16 serif 16\n"
        "        text \" y\" 44 18 12 16 serif 16\n");
    cascade_layout_case("buttons: a table one is a block, still a control",
        "<!doctype html><style>body{margin:0}button{display:table;background:none;border:0;padding:0;"
        "font:16px serif}</style><button>Go</button>", 400,
        "page 400 20\n"
        "block html 0 0 400 20\n"
        "  block body 0 0 400 20\n"
        "    block button 0 0 16 20 control 0\n"
        "      line 0 0 16 20 base 14\n"
        "        text \"Go\" 0 2 16 16 serif 16\n");
    cascade_layout_case("buttons: a min-height centres its content too",
        "<!doctype html><style>body{margin:0}button{min-height:60px;border:0;padding:0;font:16px serif}"
        "</style><button>Go</button>", 400,
        "page 400 60\n"
        "block html 0 0 400 60\n"
        "  block body 0 0 400 60\n"
        "    line 0 0 400 60 base 34\n"
        "      atomic button 0 0 16 60 control 0\n"
        "        block button 0 0 16 60\n"
        "          line 0 20 16 20 base 14\n"
        "            text \"Go\" 0 22 16 16 serif 16\n");
    cascade_layout_case("buttons: one a flex container stretches is centred in its new height",
        "<!doctype html><style>body{margin:0}div{display:flex;width:200px;height:100px}"
        "button{flex:1;background:none;border:0;padding:0;font:16px serif}</style>"
        "<div><button>Go</button></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 200 100\n"
        "      block button 0 0 200 100 control 0\n"
        "        line 0 40 200 20 base 14\n"
        "          text \"Go\" 92 42 16 16 serif 16\n");
    cascade_layout_case("buttons: and one a grid stretches",
        "<!doctype html><style>body{margin:0}div{display:grid;width:200px;height:100px}"
        "button{background:none;border:0;padding:0;font:16px serif}</style>"
        "<div><button>Go</button></div>", 400,
        "page 400 100\n"
        "block html 0 0 400 100\n"
        "  block body 0 0 400 100\n"
        "    block div 0 0 200 100\n"
        "      block button 0 0 200 100 control 0\n"
        "        line 0 40 200 20 base 14\n"
        "          text \"Go\" 92 42 16 16 serif 16\n");
    cascade_layout_case("buttons: a ::before that displays nothing keeps the face",
        "<!doctype html><style>button::before{content:'icon';display:none}</style><button>Go</button>",
        400,
        "page 400 42\n"
        "block html 0 0 400 42\n"
        "  block body 8 8 384 26\n"
        "    line 8 8 384 26 base 20\n"
        "      atomic button 8 8 160 20 control 0\n");
}
