/* THE PAINT BENEATH THE PICTURE (VIDEO.md § 5, V.7.1): a paint inside a
 * video's picture begins at the video. Each case gives a page's <video> a
 * frame of its own, paints the window whole, then paints the part a frame
 * marks with --paint-check on every covered paint: the covered paint and
 * the ordinary one must agree to the pixel, and the cover must be taken,
 * or refused, as the case says. */

static uint32_t cover_frame[800 * 600];

/* The video `id` given a frame of w by h (its natural size too), the size
 * its player was told, so no paint asks the (absent) player for another. */
static void cover_video(const char *id, uint32_t w, uint32_t h, uint32_t natural_w, uint32_t natural_h)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            cover_frame[y * w + x] = 0xff000000u | ((x * 7) & 0xff) << 16 | ((y * 5) & 0xff) << 8 | ((x ^ y) & 0xff);
    os64_memset(&s_videos[0], 0, sizeof(s_videos[0]));
    s_videos[0].node = probe_id(id);
    s_videos[0].width = natural_w;
    s_videos[0].height = natural_h;
    s_videos[0].asked_w = w;
    s_videos[0].asked_h = h;
    s_videos[0].frame = (os64_media_player_frame_t){cover_frame, w, h, 1, true};
}

typedef struct {
    uint64_t covered, pieces, again, compared, differed;
} CoverCounts;

/* The window painted whole, then `dirty` (window coordinates) painted, with
 * every covered paint checked when `checked` (and `between`, when not NULL,
 * run between the two): what the counts moved by. */
static CoverCounts cover_run_checked(os64_gui_rect_t dirty, bool checked, void (*between)(void))
{
    os64_draw_ctx_t screen = {0};
    screen.surf.width = screen.surf.pitch_px = 800;
    screen.surf.height = 600;
    screen.surf.pixels = os64_calloc(800u * 600u, sizeof(uint32_t));
    g.paint_cache = yonder_paint_cache_new(32u << 20);
    g.paint_scratch = yonder_scratch_new();
    g.ui.dirty = g.view.bounds;
    view_paint(&g.view, &screen, NULL);
    CoverCounts was = {s_cost.covered_paints, s_cost.covered_pieces, s_cost.covered_again, s_paint_compared,
                       s_paint_differed};
    s_paint_check = checked ? 1 : 0;
    s_paint_checked = 0;
    if (between != NULL)
        between();
    g.ui.dirty = dirty;
    view_paint(&g.view, &screen, NULL);
    s_paint_check = 0;
    CoverCounts moved = {s_cost.covered_paints - was.covered, s_cost.covered_pieces - was.pieces,
                         s_cost.covered_again - was.again, s_paint_compared - was.compared,
                         s_paint_differed - was.differed};
    yonder_paint_cache_free(g.paint_cache);
    g.paint_cache = NULL;
    yonder_scratch_free(g.paint_scratch);
    g.paint_scratch = NULL;
    os64_free(screen.surf.pixels);
    return moved;
}

static CoverCounts cover_run(os64_gui_rect_t dirty)
{
    return cover_run_checked(dirty, true, NULL);
}

static void cover_said(const char *what, CoverCounts c)
{
    printf("  %s: covered %lu, pieces %lu, again %lu, compared %lu, differed %lu\n", what, (unsigned long)c.covered,
           (unsigned long)c.pieces, (unsigned long)c.again, (unsigned long)c.compared, (unsigned long)c.differed);
}

/* A covered paint, `pieces` of the rest painted the ordinary way, the two
 * paints agreeing. */
static void cover_expect(const char *what, CoverCounts c, uint64_t pieces)
{
    bool ok = c.covered == 1 && c.pieces == pieces && c.again == 0 && c.compared == 1 && c.differed == 0;
    check(ok, what);
    if (!ok)
        cover_said(what, c);
}

static void cover_refused(const char *what, CoverCounts c)
{
    bool ok = c.covered == 0;
    check(ok, what);
    if (!ok)
        cover_said(what, c);
}

/* The window painted whole with covers on and again with them off: every
 * pixel the same, the pieces about the covered part included, which the
 * check (the covered part's own) does not see. */
static void cover_whole_same(const char *what)
{
    uint32_t *shot[2];
    for (int k = 0; k < 2; k++) {
        os64_draw_ctx_t screen = {0};
        screen.surf.width = screen.surf.pitch_px = 800;
        screen.surf.height = 600;
        screen.surf.pixels = os64_calloc(800u * 600u, sizeof(uint32_t));
        g.paint_cache = yonder_paint_cache_new(32u << 20);
        g.paint_scratch = yonder_scratch_new();
        s_covers = k == 0;
        g.ui.dirty = g.view.bounds;
        view_paint(&g.view, &screen, NULL);
        s_covers = true;
        yonder_paint_cache_free(g.paint_cache);
        g.paint_cache = NULL;
        yonder_scratch_free(g.paint_scratch);
        g.paint_scratch = NULL;
        shot[k] = screen.surf.pixels;
    }
    int64_t first = -1;
    for (int64_t i = 0; i < 800 * 600 && first < 0; i++)
        if ((shot[0][i] ^ shot[1][i]) & 0x00ffffffu)
            first = i;
    check(first < 0, what);
    if (first >= 0)
        printf("  %s: first difference at %ld,%ld\n", what, (long)(first % 800), (long)(first / 800));
    os64_free(shot[0]);
    os64_free(shot[1]);
}

static void cover_drop(void)
{
    os64_memset(&s_videos[0], 0, sizeof(s_videos[0]));
    probe_drop();
}

static uint32_t cover_pending;
static void cover_count_damage(void *opaque, const os64_html_node_t *node, uint32_t width, uint32_t height,
                               const draw2d_damage_t *damage)
{
    (void)opaque; (void)node; (void)width; (void)height; (void)damage;
    cover_pending++;
}

/* The page's canvas drawn again (its click handler) and the damage marked,
 * as a script turn's end marks it, and how many were pending then. */
static uint32_t cover_marked;
static void cover_mark_canvases(void)
{
    os64_dom_event_t click = {.type = "click", .bubbles = true, .cancelable = true};
    os64_js_outcome_t out;
    yonder_scripts_dispatch(g.page.scripts, NULL, &click, NULL, &out);
    while (yonder_scripts_pending(g.page.scripts))
        script_turn();
    yonder_scripts_canvas_visit_damage(g.page.scripts, canvas_damage_mark, &g.page);
    cover_pending = 0;
    yonder_scripts_canvas_visit_damage(g.page.scripts, cover_count_damage, NULL);
    cover_marked = cover_pending;
}

static void cover_cases(void)
{
    static const char *const style =
        "<!doctype html><style>body{margin:0;background:#123}"
        "#glow{position:absolute;left:0;top:0;width:400px;height:240px;background:linear-gradient(red,blue);"
        "filter:blur(8px);transform:scale(1.2)}"
        "#v{position:absolute;left:20px;top:20px;width:320px;height:180px;display:block}"
        "#cap{position:absolute;left:30px;top:150px;width:200px;height:30px;background:rgba(0,0,0,.5);color:white}"
        "#mix{position:absolute;left:250px;top:40px;width:40px;height:40px;background:#888;"
        "mix-blend-mode:difference}#bd{position:absolute;left:60px;top:60px;width:50px;height:30px;"
        "backdrop-filter:blur(3px)}</style>";
    char html[2048];
    /* The measured page's shape: a glow scaled and blurred beneath, a
     * caption, a blend and a backdrop filter over the picture. */
    snprintf(html, sizeof(html), "%s<div id=glow></div><video id=v></video><div id=cap>caption</div>"
             "<div id=mix></div><div id=bd></div>", style);
    probe_page(html, true);
    cover_video("v", 320, 180, 320, 180);
    cover_expect("cover: a frame's paint begins at the video, beneath it a scaled and blurred glow, over it a "
                 "caption, a blend and a backdrop filter, and agrees with the ordinary paint",
                 cover_run((os64_gui_rect_t){20, 20, 320, 180}), 0);
    /* The paint a little past the picture would be split, and the page's
     * backdrop filter and blend may read across the split: not split. */
    cover_refused("cover: with a backdrop reader over the picture, a paint past it is not split",
                  cover_run((os64_gui_rect_t){20, 20, 321, 180}));
    cover_whole_same("cover: the measured page's shape, the window painted whole with covers and without is the same");
    cover_drop();
    /* The same page with no backdrop reader: split, the rest the ordinary way. */
    snprintf(html, sizeof(html), "%s<div id=glow></div><video id=v></video><div id=cap>caption</div>", style);
    probe_page(html, true);
    cover_video("v", 320, 180, 320, 180);
    cover_expect("cover: a paint a pixel past the picture paints that pixel's strip the ordinary way",
                 cover_run((os64_gui_rect_t){20, 20, 321, 180}), 1);
    cover_expect("cover: a paint of the whole window is the picture's part and four pieces about it",
                 cover_run(g.view.bounds), 4);
    cover_whole_same("cover: with no backdrop reader, the window painted whole with covers and without is the same");
    /* A resize in flight: the frame is not the fitted size. */
    s_videos[0].frame.w = 300;
    cover_refused("cover: a frame not at the fitted size is no cover", cover_run((os64_gui_rect_t){20, 20, 320, 180}));
    cover_drop();

    /* A blur with no transform beneath an inset video, a paint of the whole
     * window split about the picture: a piece's layer is grown past its
     * view by the blur's reach, and must not be laid over the picture
     * (Codex, #698). */
    probe_page("<!doctype html><style>body{margin:0;background:#123}"
               "#g2{position:absolute;left:0;top:0;width:420px;height:260px;background:linear-gradient(red,blue);"
               "filter:blur(8px)}#v{position:absolute;left:40px;top:40px;width:320px;height:180px}</style>"
               "<div id=g2></div><video id=v></video>", true);
    cover_video("v", 320, 180, 320, 180);
    cover_expect("cover: an untransformed blur beneath, the whole window split about the picture",
                 cover_run(g.view.bounds), 4);
    cover_whole_same("cover: an untransformed blur beneath, the window painted whole with covers and without is "
                     "the same");
    cover_drop();

    /* The counted table (W0's lines): one-offs fill it, a thing that comes
     * again before eleven newer ones keeps its row, and once counted twice
     * no burst of one-offs pushes it out. */
    {
        CountedAt t;
        counted_at_reset(&t);
        char name[16];
        for (int k = 0; k < 12; k++) {
            os64_snprintf(name, sizeof(name), "once%d", k);
            counted_at(&t, name, "", "");
        }
        counted_at(&t, "again", "", "");
        for (int k = 0; k < 10; k++) {
            os64_snprintf(name, sizeof(name), "later%d", k);
            counted_at(&t, name, "", "");
        }
        counted_at(&t, "again", "", "");
        for (int k = 0; k < 40; k++) {
            os64_snprintf(name, sizeof(name), "burst%d", k);
            counted_at(&t, name, "", "");
        }
        uint64_t total = t.others;
        for (int k = 0; k < t.nrows; k++)
            total += t.row[k].n;
        check(t.nrows == 12 && os64_streq(t.row[0].what, "again") && t.row[0].n == 2 && total == 64,
              "cover: the counted table keeps a thing counted twice through a burst, and loses no count");
    }

    /* Layers beneath the video enough to run the ordinary paint out of its
     * budget, and one over it: the ordinary paint draws that one plain, so
     * the paint is not covered, and the window is the same (Codex, #698). */
    probe_page("<!doctype html><style>body{margin:0;background:#123}"
               ".b{position:absolute;left:0;top:0;width:800px;height:600px;filter:blur(2px);"
               "background:rgba(200,100,50,.2)}#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#over{position:absolute;left:60px;top:60px;width:100px;height:60px;background:#fff;"
               "filter:blur(3px)}</style><div class=b></div><div class=b></div><div class=b></div>"
               "<div class=b></div><video id=v></video><div id=over></div>", true);
    cover_video("v", 320, 180, 320, 180);
    {
        /* The picture's part alone: each layer charges only that part, the
         * budget cannot run out, and the paint is covered. */
        cover_expect("cover: layers that cannot run the budget out over the picture's part, covered",
                     cover_run((os64_gui_rect_t){40, 40, 320, 180}), 0);
        /* The whole window: five layers of it pass four views' budget. Under
         * --layout-profile the record says which limit and which element:
         * the four full-window layers charge the budget exactly, and the
         * overlay's is the charge that passes it (WHOLE_REPAINTS.md W0). */
        uint64_t budget = s_cost.covered_budget;
        bool profiled = s_layout_profile;
        yonder_diag_t *was_diag = g.page.diag;
        g.page.diag = yonder_diag_new("https://fixture.test/not-covered", 3, 0);
        s_layout_profile = true;
        CoverCounts b = cover_run(g.view.bounds);
        check(b.covered == 0 && s_cost.covered_budget > budget,
              "cover: layers that might run the budget out keep the paint the ordinary way");
        if (!(b.covered == 0 && s_cost.covered_budget > budget))
            cover_said("the layers' budget", b);
        counted_at_reset(&s_script_changes);
        diag_paint_reading(&g.page, 1);
        const char *said = g.page.diag != NULL ? yonder_diag_fact_value(g.page.diag, "not covered reading 1") : NULL;
        check(said != NULL && strstr(said, "budget ") == said && strstr(said, ": div#over (charged ") != NULL &&
                  strstr(said, "of 1920000; view 800x600") != NULL,
              "cover: the record names the budget and the element whose charge passed it");
        if (said == NULL || strstr(said, ": div#over (charged ") == NULL)
            fprintf(stderr, "  not covered: %s\n", said != NULL ? said : "(none)");
        /* A script's change: the record says what it changed and where, the
         * inline style as it now stands. */
        os64_html_node_t *over = (os64_html_node_t *)probe_id("over");
        s_layout_profile = true;
        check(over != NULL && os64_html_set_attr((os64_html_document_t *)page_doc(&g.page), over, "style",
                                                 "opacity:.5", 10) == OS64_HTML_OK,
              "cover: a style written for the change note");
        script_changes_note(&g.page);
        script_changes_note(&g.page);
        diag_paint_reading(&g.page, 2);
        s_layout_profile = profiled;
        said = g.page.diag != NULL ? yonder_diag_fact_value(g.page.diag, "whole views by a script's change reading 2")
                                   : NULL;
        check(g.page.diag != NULL && yonder_diag_fact_value(g.page.diag, "not covered reading 2") == NULL,
              "cover: a reading starts the tables again");
        check(said != NULL && strstr(said, "attribute style 2: div#over (opacity:.5)") == said,
              "cover: the record names a script's change, once a rebuild, with the style as it stands");
        if (said == NULL || strstr(said, "attribute style 2: div#over (opacity:.5)") != said)
            fprintf(stderr, "  whole views by a script's change: %s\n", said != NULL ? said : "(none)");
        yonder_diag_free(g.page.diag);
        g.page.diag = was_diag;
    }
    cover_whole_same("cover: layers that might run the budget out, the window painted whole with covers and without "
                     "is the same");
    cover_drop();

    /* The same with the layers beneath drawn through a matrix that moves and
     * halves them: their bounds are in the matrix's own pixels, met with
     * the view taken back through it (Codex, #698 round 2). */
    probe_page("<!doctype html><style>body{margin:0;background:#123}"
               ".t{position:absolute;left:1000px;top:0;width:1580px;height:1180px;filter:blur(2px);"
               "background:rgba(200,100,50,.2);transform:translateX(-1000px) scale(.5);transform-origin:0 0}"
               "#v{position:absolute;left:0;top:0;width:800px;height:600px}"
               "#over{position:absolute;left:60px;top:60px;width:200px;height:200px;background:#fff;"
               "filter:blur(3px)}</style><div class=t></div><div class=t></div><div class=t></div>"
               "<div class=t></div><video id=v></video><div id=over></div>", true);
    cover_video("v", 800, 600, 800, 600);
    {
        uint64_t budget = s_cost.covered_budget;
        CoverCounts b = cover_run(g.view.bounds);
        check(b.covered == 0 && s_cost.covered_budget > budget,
              "cover: transformed layers that might run the budget out keep the paint the ordinary way");
        if (!(b.covered == 0 && s_cost.covered_budget > budget))
            cover_said("transformed layers' budget", b);
    }
    cover_whole_same("cover: transformed layers that might run the budget out, the window painted whole with covers "
                     "and without is the same");
    cover_drop();

    /* A transformed overlay whose sheet for the whole window is past the
     * sheets' limit, which the ordinary paint draws upright: a split paint
     * would ask for smaller sheets and lay it through its matrix, so the
     * paint is the ordinary one (Codex, #698 round 3). */
    probe_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#big{position:absolute;left:0;top:0;width:2000px;height:1500px;"
               "background:linear-gradient(90deg,rgba(255,0,0,.4),rgba(0,0,255,.4));"
               "transform:scale(.4);transform-origin:0 0}</style><video id=v></video><div id=big></div>", true);
    cover_video("v", 320, 180, 320, 180);
    {
        uint64_t limited = s_cost.covered_budget;
        CoverCounts b = cover_run(g.view.bounds);
        check(b.covered == 0 && s_cost.covered_budget > limited,
              "cover: a sheet past its limit keeps the paint the ordinary way");
        if (!(b.covered == 0 && s_cost.covered_budget > limited))
            cover_said("a sheet past its limit", b);
    }
    cover_whole_same("cover: a sheet past its limit, the window painted whole with covers and without is the same");
    cover_drop();

    /* A transformed group inside a blurred parent: the parent's layer grows
     * the view by its reach, and the child's sheet is taken from that grown
     * view, past the sheets' limit for the whole window though not for the
     * picture's part (Codex, #698 round 4). */
    probe_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#p{position:absolute;left:0;top:0;width:800px;height:600px;filter:blur(100px)}"
               "#c{position:absolute;left:0;top:0;width:2000px;height:1500px;"
               "background:linear-gradient(90deg,rgba(255,0,0,.4),rgba(0,0,255,.4));"
               "transform:scale(.6);transform-origin:0 0}</style><video id=v></video>"
               "<div id=p><div id=c></div></div>", true);
    cover_video("v", 320, 180, 320, 180);
    {
        uint64_t limited = s_cost.covered_budget;
        CoverCounts b = cover_run(g.view.bounds);
        check(b.covered == 0 && s_cost.covered_budget > limited,
              "cover: a sheet past its limit under a filtered parent keeps the paint the ordinary way");
        if (!(b.covered == 0 && s_cost.covered_budget > limited))
            cover_said("a sheet past its limit under a filtered parent", b);
    }
    cover_whole_same("cover: a sheet past its limit under a filtered parent, the window painted whole with covers "
                     "and without is the same");
    cover_drop();

    /* A group drawn through a matrix whose upright place lies far from what
     * it draws: the sheet the painter asks for is the window taken back
     * through the matrix (about 200x150 near 800,475 of its 4000x2500),
     * well within the limit, so the paint is covered. The upright place the
     * walk also takes in (the window moved by the matrix's translation,
     * near 3200,1900) is no part of the sheet; counting it, the bound took
     * the two together, about 3200x2025, and refused the paint, as it
     * refused every whole repaint of the P5's watch page (WHOLE_REPAINTS.md,
     * W0's record). */
    probe_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#far{position:absolute;left:0;top:0;width:4000px;height:2500px;"
               "background:linear-gradient(135deg,rgba(255,0,0,.4),rgba(0,0,255,.4));"
               "transform:translate(-3200px,-1900px) scale(4);transform-origin:0 0}</style>"
               "<video id=v></video><div id=far></div>", true);
    cover_video("v", 320, 180, 320, 180);
    cover_expect("cover: a matrix whose upright place lies far from its sheet, the whole window split",
                 cover_run(g.view.bounds), 4);
    cover_whole_same("cover: a matrix whose upright place lies far from its sheet, the window painted whole with "
                     "covers and without is the same");
    cover_drop();

    /* A video in a group is drawn on its sheets, never plain. */
    snprintf(html, sizeof(html), "%s<div id=glow></div><div style='opacity:.9'><video id=v></video></div>", style);
    probe_page(html, true);
    cover_video("v", 320, 180, 320, 180);
    cover_refused("cover: a video inside an opacity is no cover", cover_run((os64_gui_rect_t){20, 20, 320, 180}));
    cover_drop();

    /* An absolutely positioned video inside a box that groups and clips,
     * its containing block outside it: painted inside the group, no cover. */
    probe_page("<!doctype html><style>body{margin:0}</style><div style='opacity:.5;overflow:hidden;height:10px'>"
               "<video id=v style='position:absolute;left:20px;top:20px;width:320px;height:180px'></video></div>",
               true);
    cover_video("v", 320, 180, 320, 180);
    cover_refused("cover: a positioned video painted inside an ancestor's group is no cover",
                  cover_run((os64_gui_rect_t){20, 20, 320, 180}));
    cover_drop();

    /* A letterboxed picture: the bars are the box's, painted the ordinary
     * way, the picture's band covered. */
    snprintf(html, sizeof(html), "%s<div id=glow></div><video id=v style='height:240px'></video>",
             style);
    probe_page(html, true);
    cover_video("v", 320, 180, 320, 180);
    {
        os64_gui_rect_t m = video_frame_mark(&s_videos[0], (os64_gui_rect_t){20, 20, 320, 240});
        check(m.x == 20 && m.y == 50 && m.w == 320 && m.h == 180,
              "cover: with no backdrop reader, a letterboxed frame marks only its picture's band");
    }
    cover_expect("cover: a letterboxed picture covers its band, and its bars are painted the ordinary way",
                 cover_run((os64_gui_rect_t){20, 20, 320, 240}), 2);
    cover_whole_same("cover: a letterboxed picture, the window painted whole with covers and without is the same");
    cover_drop();

    /* A negative z-index sibling beneath, a positive one over. */
    snprintf(html, sizeof(html), "%s<div style='position:absolute;z-index:-1;left:0;top:0;width:500px;"
             "height:300px;background:orange'></div><video id=v></video><div style='position:absolute;z-index:2;"
             "left:100px;top:100px;width:30px;height:30px;background:rgba(255,255,255,.4)'></div>", style);
    probe_page(html, true);
    cover_video("v", 320, 180, 320, 180);
    cover_expect("cover: a negative z-index beneath and a positive one over agree with the ordinary paint",
                 cover_run((os64_gui_rect_t){20, 20, 320, 180}), 0);
    cover_whole_same("cover: z-indices both ways, the window painted whole with covers and without is the same");
    cover_drop();

    /* In a scroller that cuts the picture: the clip's part is covered, the
     * rest of the paint the ordinary way. */
    probe_page("<!doctype html><style>body{margin:0;background:#123}</style>"
               "<div style='position:relative;overflow:hidden;width:200px;height:100px;background:yellow'>"
               "<div style='height:40px'>above</div>"
               "<video id=v style='display:block;width:320px;height:180px'></video></div>", true);
    cover_video("v", 320, 180, 320, 180);
    cover_expect("cover: a scroller cutting the picture covers what it shows, and the rest is ordinary",
                 cover_run((os64_gui_rect_t){0, 40, 320, 180}), 2);
    cover_whole_same("cover: a scroller cutting the picture, the window painted whole with covers and without is the same");
    cover_drop();

    /* At 150%, an edge that falls between pixels. */
    snprintf(html, sizeof(html), "%s<div id=glow></div><video id=v style='left:13.5px;top:9.25px'></video>", style);
    probe_page(html, true);
    g.zoom = 1500;
    check(lay_out_page(&g.page, 800, 600), "cover: the page laid out at 150%");
    cover_video("v", 480, 270, 320, 180);
    cover_expect("cover: at 150% with a fractional edge, the covered paint agrees with the ordinary one",
                 cover_run((os64_gui_rect_t){20, 13, 480, 271}), 1);
    cover_whole_same("cover: at 150%, the window painted whole with covers and without is the same");
    cover_drop();

    /* A canvas beneath the video, drawn by the page: the covered paint
     * acknowledges its damage, which would otherwise mark it again. */
    snprintf(html, sizeof(html), "%s<canvas id=c width=320 height=180 style='position:absolute;left:20px;"
             "top:20px'></canvas><video id=v></video><script>const x=document.getElementById('c')"
             ".getContext('2d');x.fillStyle='red';x.fillRect(0,0,50,50);window.onclick=()=>{x.fillStyle='blue';"
             "x.fillRect(10,10,60,60)};</script>", style);
    probe_page(html, true);
    while (yonder_scripts_pending(g.page.scripts))
        script_turn();
    cover_video("v", 320, 180, 320, 180);
    /* Marked between the whole paint (whose list is then empty, so it
     * acknowledges nothing) and the paint of the picture's part, which is
     * the one held to acknowledging it; without the check, whose ordinary
     * paint would draw the canvas. */
    CoverCounts c = cover_run_checked((os64_gui_rect_t){20, 20, 320, 180}, false, cover_mark_canvases);
    cover_pending = 0;
    yonder_scripts_canvas_visit_damage(g.page.scripts, cover_count_damage, NULL);
    check(cover_marked == 1 && c.covered == 1 && cover_pending == 0,
          "cover: a canvas beneath the picture, marked for the paint, is acknowledged by the covered paint");
    if (!(cover_marked == 1 && c.covered == 1 && cover_pending == 0)) {
        printf("  canvas: pending when marked %u, after %u\n", cover_marked, cover_pending);
        cover_said("canvas", c);
    }
    cover_drop();

    /* A backdrop filter across the edge of a letterboxed picture's band
     * reads pixels a split there would paint in another piece: the paint is
     * not split (the ordinary way whole), and the window is the same with
     * covers and without. */
    snprintf(html, sizeof(html), "%s<div id=glow></div><video id=v style='height:240px'></video>"
             "<div style='position:absolute;left:40px;top:40px;width:200px;height:40px;"
             "background:rgba(0,0,0,.3);backdrop-filter:blur(4px)'></div>", style);
    probe_page(html, true);
    cover_video("v", 320, 180, 320, 180);
    uint64_t refused = s_cost.covered_refused;
    /* And a frame marks the box's part, which the reader reads, not only
     * the picture's band. */
    os64_gui_rect_t box_part = {20, 20, 320, 240}, marked = video_frame_mark(&s_videos[0], box_part);
    check(marked.x == 20 && marked.y == 20 && marked.w == 320 && marked.h == 240,
          "cover: with a backdrop reader near, a frame marks its box's part");
    CoverCounts r = cover_run((os64_gui_rect_t){20, 20, 320, 240});
    check(r.covered == 0 && s_cost.covered_refused > refused,
          "cover: a backdrop filter across the covered part's edge keeps the paint whole");
    if (!(r.covered == 0 && s_cost.covered_refused > refused))
        cover_said("backdrop across the edge", r);
    cover_whole_same("cover: a backdrop filter across the covered part's edge, the window painted whole with covers "
                     "and without is the same");
    cover_drop();
}
