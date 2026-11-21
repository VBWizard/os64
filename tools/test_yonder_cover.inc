/* THE PAINT BENEATH THE PICTURE (VIDEO.md § 5, V.7.1): a paint inside a
 * video's picture begins at the video. Each case gives a page's <video> a
 * frame of its own, paints the window whole, then paints the part a frame
 * marks with --paint-check on every covered paint: the covered paint and
 * the ordinary one must agree to the pixel, and the cover must be taken,
 * or refused, as the case says. */

static uint32_t cover_frame[480 * 360];

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
 * every covered paint checked when `checked`: what the counts moved by. */
static CoverCounts cover_run_checked(os64_gui_rect_t dirty, bool checked)
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
    return cover_run_checked(dirty, true);
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
    /* The paint a little past the picture: the rest the ordinary way. */
    cover_expect("cover: a paint a pixel past the picture paints that pixel's strip the ordinary way",
                 cover_run((os64_gui_rect_t){20, 20, 321, 180}), 1);
    cover_expect("cover: a paint of the whole window is the picture's part and four pieces about it",
                 cover_run(g.view.bounds), 4);
    /* A resize in flight: the frame is not the fitted size. */
    s_videos[0].frame.w = 300;
    cover_refused("cover: a frame not at the fitted size is no cover", cover_run((os64_gui_rect_t){20, 20, 320, 180}));
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
    cover_expect("cover: a letterboxed picture covers its band, and its bars are painted the ordinary way",
                 cover_run((os64_gui_rect_t){20, 20, 320, 240}), 2);
    cover_drop();

    /* A negative z-index sibling beneath, a positive one over. */
    snprintf(html, sizeof(html), "%s<div style='position:absolute;z-index:-1;left:0;top:0;width:500px;"
             "height:300px;background:orange'></div><video id=v></video><div style='position:absolute;z-index:2;"
             "left:100px;top:100px;width:30px;height:30px;background:rgba(255,255,255,.4)'></div>", style);
    probe_page(html, true);
    cover_video("v", 320, 180, 320, 180);
    cover_expect("cover: a negative z-index beneath and a positive one over agree with the ordinary paint",
                 cover_run((os64_gui_rect_t){20, 20, 320, 180}), 0);
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
    cover_drop();

    /* At 150%, an edge that falls between pixels. */
    snprintf(html, sizeof(html), "%s<div id=glow></div><video id=v style='left:13.5px;top:9.25px'></video>", style);
    probe_page(html, true);
    g.zoom = 1500;
    check(lay_out_page(&g.page, 800, 600), "cover: the page laid out at 150%");
    cover_video("v", 480, 270, 320, 180);
    cover_expect("cover: at 150% with a fractional edge, the covered paint agrees with the ordinary one",
                 cover_run((os64_gui_rect_t){20, 13, 480, 271}), 1);
    cover_drop();

    /* A canvas beneath the video, drawn by the page: the covered paint
     * acknowledges its damage, which would otherwise mark it again. */
    snprintf(html, sizeof(html), "%s<canvas id=c width=320 height=180 style='position:absolute;left:20px;"
             "top:20px'></canvas><video id=v></video><script>const x=document.getElementById('c')"
             ".getContext('2d');x.fillStyle='red';x.fillRect(0,0,50,50);</script>", style);
    probe_page(html, true);
    while (yonder_scripts_pending(g.page.scripts))
        script_turn();
    cover_video("v", 320, 180, 320, 180);
    yonder_scripts_canvas_visit_damage(g.page.scripts, canvas_damage_mark, &g.page);
    cover_pending = 0;
    yonder_scripts_canvas_visit_damage(g.page.scripts, cover_count_damage, NULL);
    uint32_t before = cover_pending;
    /* Without the check, whose ordinary paint would draw the canvas. */
    CoverCounts c = cover_run_checked((os64_gui_rect_t){20, 20, 320, 180}, false);
    cover_pending = 0;
    yonder_scripts_canvas_visit_damage(g.page.scripts, cover_count_damage, NULL);
    check(before == 1 && c.covered == 1 && cover_pending == 0,
          "cover: a canvas beneath the picture, marked for the paint, is acknowledged by the covered paint");
    if (!(before == 1 && c.covered == 1 && cover_pending == 0)) {
        printf("  canvas: pending before %u, after %u\n", before, cover_pending);
        cover_said("canvas", c);
    }
    cover_drop();
}
