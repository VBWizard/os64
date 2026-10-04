// The blend (PILE3.md § The blend): colours keep their alpha, and a box
// below full opacity is a stacking context — stacked even in the flow — and
// a GROUP the walk brackets for a face to composite whole.

typedef struct {
    char log[512];
    size_t n;
    os64_gui_rect_t bounds[4];
    int32_t opens;
} GroupLog;

static void glog(GroupLog *g, const char *what)
{
    size_t len = strlen(what);
    if (g->n + len + 1 < sizeof(g->log)) {
        memcpy(g->log + g->n, what, len);
        g->n += len;
        g->log[g->n++] = ' ';
        g->log[g->n] = '\0';
    }
}

static void glog_visit(void *ctx, const flow_box_t *b)
{
    GroupLog *g = ctx;
    if (b->kind == FLOW_BOX_TEXT)
        glog(g, b->text[0] == 'a' ? "a" : b->text[0] == 'b' ? "b" : "t");
}

static void glog_open(void *ctx, const flow_box_t *b, os64_gui_rect_t bounds)
{
    (void)b;
    GroupLog *g = ctx;
    if (g->opens < 4)
        g->bounds[g->opens] = bounds;
    g->opens++;
    glog(g, "(");
}

static void glog_close(void *ctx, const flow_box_t *b)
{
    (void)b;
    glog(ctx, ")");
}

static void blend_cases(void)
{
    flow_point_t top = {0, 0};
    // A block below full opacity is stacked and not positioned: it stays
    // in its parent's overflow and is painted, as one group, after the
    // paragraph written after it — which it overlaps by a negative margin,
    // so a point on both is the group's.
    Sheeted q;
    sheeted_open(&q, "<!doctype html><style>body{margin:0} p{margin:0}"
                     ".h{opacity:0.5;height:20px} .up{margin-top:-20px}</style>"
                     "<div id=h class=h>a</div><p class=up>b</p>",
                 400, 600);
    flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *h = box_with_id(t, q.doc->document, "h");
    GroupLog g = {{0}, 0, {{0}}, 0};
    flow_visitor_t v = {glog_visit, glog_open, glog_close, &g};
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &v);
    expect("blend: a block below full opacity is stacked, not positioned, and one group",
           h != NULL && h->stacked && !h->positioned && flow_npositioned(t) == 0 &&
               strcmp(g.log, "b ( a ) ") == 0 && g.opens == 1 && g.bounds[0].y == 0 &&
               g.bounds[0].h == 20 && dump_has(t, "block div 0 0 400 20 stacked"), g.log);
    const flow_box_t *hit = t != NULL ? flow_hit(t, 2, 5, top) : NULL;
    expect("blend: what is painted over the flow is hit over it", inside_of(hit, h), NULL);
    flow_free(t);
    sheeted_close(&q);

    // A positioned group's bounds hold its positioned members, wherever
    // they are: the absolute box 50 below reaches 60. Inside it a nested
    // group opens and closes within it. An inline-block below full opacity
    // is not stacked: its content is painted where its atom is.
    sheeted_open(&q, "<!doctype html><style>body{margin:0}"
                     "#r{position:relative;opacity:0.5;height:10px}"
                     ".abs{position:absolute;top:50px;width:10px;height:10px}"
                     ".in{opacity:0.25;height:5px} #ib{display:inline-block;opacity:0.5}"
                     "</style><div id=r><div class=in></div><div class=abs></div></div>"
                     "<span id=ib>t</span>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    const flow_box_t *ib = box_with_id(t, q.doc->document, "ib");
    GroupLog n = {{0}, 0, {{0}}, 0};
    flow_visitor_t nv = {glog_visit, glog_open, glog_close, &n};
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &nv);
    expect("blend: a group holds its positioned members, and groups nest",
           n.opens == 2 && strncmp(n.log, "t ( ( ) ) ", 10) == 0 && n.bounds[0].y == 0 &&
               n.bounds[0].h == 60 && n.bounds[1].h == 5, n.log);
    expect("blend: an inline-block below full opacity is not stacked",
           ib != NULL && ib->first != NULL && !ib->first->stacked, NULL);
    flow_free(t);
    sheeted_close(&q);

    // opacity: 0 paints nothing and opens no group; the page's own
    // flow_visit hears no groups at all.
    sheeted_open(&q, "<!doctype html><style>body{margin:0} .o{opacity:0} .h{opacity:0.5}"
                     "</style><div class=o>a</div><div class=h>b</div>",
                 400, 600);
    t = flow_layout(q.doc, q.page, 400, &q.env);
    GroupLog z = {{0}, 0, {{0}}, 0};
    flow_visitor_t zv = {glog_visit, glog_open, glog_close, &z};
    flow_visit_groups(t, (os64_gui_rect_t){0, 0, 400, 600}, top, &zv);
    expect("blend: opacity 0 opens no group and paints nothing",
           strcmp(z.log, "( b ) ") == 0, z.log);
    flow_free(t);
    sheeted_close(&q);
}

// Corners (PILE3.md § The box's edge): a radius resolves against the border
// box, and radii that would overlap along a side are all scaled down
// together until they meet (Backgrounds 3 § 5.5).
static void radius_cases(void)
{
    struct { const char *css; int32_t h, v; } k[] = {
        {"border-radius:50%", 50, 25},              // 100 x 50: half of each
        {"border-radius:40px", 25, 25},             // 80 down a 50 side: 5/8 of 40
        {"border-radius:10px / 20px", 10, 20},      // fits: as written
        {"border-top-left-radius:0 30px", 0, 30},   // kept; a face draws it square
        // Scaled from the radii as written, never capped first (Quinn,
        // #203; Firefox): one 100px corner of a 50px-high box is 50 x 50,
        // not 100 x 50; 100px and 10px corners stay circular at 50/110.
        {"border-top-left-radius:100px", 50, 50},
        {"border-radius:100px 10px", 45, 45},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        char html[256];
        snprintf(html, sizeof(html),
                 "<!doctype html><style>body{margin:0} div{width:100px;height:50px;%s}</style>"
                 "<div id=d></div>",
                 k[i].css);
        Sheeted q;
        sheeted_open(&q, html, 400, 600);
        flow_tree_t *t = flow_layout(q.doc, q.page, 400, &q.env);
        const flow_box_t *d = box_with_id(t, q.doc->document, "d");
        int32_t r[4][2] = {{0}};
        flow_box_radii(d, r);
        char got[64];
        snprintf(got, sizeof(got), "%s: %d %d", k[i].css, r[0][0], r[0][1]);
        expect("radius: a corner resolved and held to its sides",
               d != NULL && r[0][0] == k[i].h && r[0][1] == k[i].v, got);
        flow_free(t);
        sheeted_close(&q);
    }
}

// Gradients (PILE3.md § Gradients): what libgarb kept, read into flow's.
// The initial direction is down, 180 degrees; `to top right` is a corner,
// whose angle the box's shape decides; 0.25turn is 90 degrees. A stop
// written with two positions is two stops; a hint has no colour; a stop
// with none is auto. A radial's initial is an ellipse to the farthest
// corner at the centre; a circle given one length keeps it for both
// radii. currentColor in a stop is the element's colour.
static void gradient_style_cases(void)
{
    cascade_case("gradients: what a page wrote, read", 400,
        "<!doctype html><style>"
        ".a{background-image:linear-gradient(red,blue)}"
        ".b{background-image:linear-gradient(to top right,red 10%,30%,blue 2px 4px)}"
        ".c{background-image:repeating-radial-gradient(circle 20px at 10px 50%,red,blue)}"
        ".d{color:green;background-image:linear-gradient(0.25turn,red,currentColor)}"
        ".e{background:radial-gradient(red,blue)}"
        "</style><div class=a></div><div class=b></div><div class=c></div>"
        "<div class=d></div><div class=e></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block gradient=linear:180000:#ff0000@auto,#0000ff@auto\n"
        "    div block gradient=linear:to 1 -1:#ff0000@10%,@30%,#0000ff@2,#0000ff@4\n"
        "    div block gradient=repeating-radial:circle size 20/20 at 10/50%:"
        "#ff0000@auto,#0000ff@auto\n"
        "    div block color=#008000 gradient=linear:90000:#ff0000@auto,currentColor@auto\n"
        "    div block gradient=radial:ellipse farthest-corner at 50%/50%:"
        "#ff0000@auto,#0000ff@auto\n");
    // An angle is taken round the circle before it is scaled: 1e20 degrees
    // is fmod's 280, not whatever an overflowed integer makes of it.
    cascade_case("gradients: an angle of any size", 400,
        "<!doctype html><style>"
        ".a{background-image:linear-gradient(1e20deg,red,blue)}"
        ".b{background-image:linear-gradient(-90deg,red,blue)}"
        ".c{background-image:linear-gradient(720.5deg,red,blue)}"
        "</style><div class=a></div><div class=b></div><div class=c></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block gradient=linear:280000:#ff0000@auto,#0000ff@auto\n"
        "    div block gradient=linear:270000:#ff0000@auto,#0000ff@auto\n"
        "    div block gradient=linear:500:#ff0000@auto,#0000ff@auto\n");
    // Past 1e300 too, to the top of a double's range. The remainders are
    // fmod's of the double libgarb reads, which is within a few units in
    // the last place of the text (tokenize.c, BROWSER_DEBTS.md):
    // 1.0000000000000002e301 reads as 1.0000000000000008e301 (48 degrees),
    // 1e308 as 1.0000000000000006e308 (320).
    cascade_case("gradients: every finite angle", 400,
        "<!doctype html><style>"
        ".d{background-image:linear-gradient(1.0000000000000002e301deg,red,blue)}"
        ".e{background-image:linear-gradient(-1.0000000000000002e301deg,red,blue)}"
        ".f{background-image:linear-gradient(1e308deg,red,blue)}"
        "</style><div class=d></div><div class=e></div><div class=f></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block gradient=linear:48000:#ff0000@auto,#0000ff@auto\n"
        "    div block gradient=linear:312000:#ff0000@auto,#0000ff@auto\n"
        "    div block gradient=linear:320000:#ff0000@auto,#0000ff@auto\n");
    // currentColor is the colour of the element that USES it (CSS Color 4
    // § 4.4): a background or border colour taken by `inherit` is the
    // child's colour, and one written before `color` in the same rule is
    // the colour the rule ends with.
    cascade_case("currentColor resolves where it is used", 400,
        "<!doctype html><style>"
        ".p{color:#008000;background-color:currentColor;border:1px solid}"
        ".c{color:#0000ff;background-color:inherit;border:1px solid;border-color:inherit}"
        ".d{background-color:currentColor;color:#ff0000}"
        "</style><div class=p><div class=c></div></div><div class=d></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block color=#008000 bg=#008000 border=1:solid:#008000 holds-block\n"
        "      div block color=#0000ff bg=#0000ff border=1:solid:#0000ff\n"
        "    div block color=#ff0000 bg=#ff0000\n");
}

// background-size, -origin and -clip as computed: an em is the box's
// font's, a single length's height is auto, and the shorthand's one box
// is both the origin and the clip.
static void background_box_cases(void)
{
    cascade_case("backgrounds: size, origin and clip, read", 400,
        "<!doctype html><style>"
        ".a{background-size:cover;background-origin:border-box}"
        ".b{font-size:10px;background-size:2em 50%;background-clip:content-box}"
        ".c{background-size:30px}"
        ".d{background:linear-gradient(red,blue) 0 0/contain content-box}"
        ".e{background-size:20px 20px;background-clip:text}"
        ".f{background-size:20px 20px;background:red}"
        "</style><div class=a></div><div class=b></div><div class=c></div>"
        "<div class=d></div><div class=e></div><div class=f></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block bg-size=cover bg-origin=border-box\n"
        "    div block size=10 bg-size=20/50% bg-clip=content-box\n"
        "    div block bg-size=30/auto\n"
        "    div block bg-size=contain bg-origin=content-box bg-clip=content-box "
        "gradient=linear:180000:#ff0000@auto,#0000ff@auto\n"
        "    div block bg-size=20/20 bg-clip=text\n"
        "    div block bg=#ff0000\n");
    // Layers: the images say how many; a shorter list repeats (the one
    // repeat, the two sizes taken in turn), a longer one is cut; `inherit`
    // takes the parent's whole list.
    cascade_case("backgrounds: every layer, read", 400,
        "<!doctype html><style>"
        ".a{background-image:url(a.png),url(b.png),url(c.png);background-repeat:no-repeat;"
        "background-size:10px,cover;background-clip:content-box,padding-box,border-box,text}"
        ".b{background:url(a.png) 1px 2px,linear-gradient(red,blue) content-box}"
        ".c{background-image:inherit}"
        "</style><div class=a></div><div class=b><div class=c></div></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block bg-image=\"a.png\"@0 no-repeat 0%/0% bg-size=10/auto bg-clip=content-box"
        " layer=1 bg-image=\"b.png\"@0 no-repeat 0%/0% bg-size=cover bg-clip=padding-box"
        " layer=2 bg-image=\"c.png\"@0 no-repeat 0%/0% bg-size=10/auto\n"
        "    div block bg-image=\"a.png\"@0 repeat 1/2"
        " layer=1 bg-origin=content-box bg-clip=content-box"
        " gradient=linear:180000:#ff0000@auto,#0000ff@auto holds-block\n"
        "      div block bg-image=\"a.png\"@0 repeat 0%/0%"
        " layer=1 gradient=linear:180000:#ff0000@auto,#0000ff@auto\n");
}

// image-rendering is inherited, its crisp spellings one answer and every
// other keyword the other.
static void image_rendering_cases(void)
{
    cascade_case("image-rendering: pixelated inherited, auto back to smooth", 400,
        "<!doctype html><style>.a{image-rendering:pixelated}.c{image-rendering:auto}"
        ".d{image-rendering:-webkit-optimize-contrast}</style>"
        "<div class=a><p></p><p class=c><span class=d></span></p></div>",
        "html block holds-block\n"
        "  head none\n"
        "  body block margin=8/8/8/8 holds-block\n"
        "    div block pixelated holds-block\n"
        "      p block margin=16/0/16/0\n"
        "      p block margin=16/0/16/0 smooth\n"
        "        span inline pixelated\n");
}

// Zoom (PILE3.md § Zoom): at 2, every CSS pixel is two of the tree's. 1em
// is the zoomed font's (32), 50px is 100, a 1px border 2, and 25vh is a
// quarter of the 600 CSS px viewport, 300 device pixels.
static void zoom_cases(void)
{
    kEnv.zoom = 2000;
    cascade_layout_case("zoom 2: lengths, the font, the border and vh doubled",
        "<!doctype html><style>body{margin:0}"
        "div{width:50px;height:25vh;border-left:1px solid;margin-left:1em}</style><div></div>",
        400,
        "page 400 300\n"
        "block html 0 0 400 300\n"
        "  block body 0 0 400 300\n"
        "    block div 32 0 102 300\n");
    // A picture's size from its attributes, and the oracle's own 40 x 30,
    // both in CSS pixels: 20 x 40 and 80 x 60 on the page.
    char *got = cascade_layout_of("<!doctype html><img src=known.png width=10 height=20>"
                                  "<img src=known.png>", 400);
    int32_t size[2][2] = {{0}}, n = 0;
    for (const char *at = got; at != NULL && n < 2 && (at = strstr(at, "atomic img ")) != NULL;
         at++, n++) {
        int x, y;
        sscanf(at, "atomic img %d %d %d %d", &x, &y, &size[n][0], &size[n][1]);
    }
    expect("zoom 2: a picture's attribute size and its own size doubled",
           n == 2 && size[0][0] == 20 && size[0][1] == 40 && size[1][0] == 80 && size[1][1] == 60,
           got);
    free(got);
    kEnv.zoom = 0;
}
