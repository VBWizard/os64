/* VIDEOS ON PLANES (VIDEO_PLANE.md § 5, P3a): a page's <video> whose
 * player has a plane (fake_plane, test_yonder_scripts_host.c). Each case
 * runs a paint's steps around a real render of the window: planes_prepare,
 * the render (view_paint), planes_judge, planes_hole and the edits the
 * publish would carry (the publish itself is a syscall the host cannot
 * make). */

static uint32_t plane_pic[320 * 180];
static os64_draw_ctx_t plane_screen;
static os64_draw_ctx_t *plane_saved_ctx;

/* A page with the video `v` laid out at 320x180, its player's plane on and
 * holding `shown`'s picture at that size. */
static void plane_page(const char *html)
{
    probe_page(html, true);
    for (uint32_t i = 0; i < 320u * 180u; i++)
        plane_pic[i] = 0xff000000u | ((i * 13) & 0xff) << 16 | ((i / 320 * 7) & 0xff) << 8 | 0x40;
    os64_memset(&s_videos[0], 0, sizeof(s_videos[0]));
    s_videos[0].node = probe_id("v");
    s_videos[0].width = s_videos[0].asked_w = 320;
    s_videos[0].height = s_videos[0].asked_h = 180;
    s_videos[0].seen.picture = true;
    fake_plane = (__typeof__(fake_plane)){0};
    fake_plane.on = true;
    fake_plane.facts.plane_state = OS64_MEDIA_PLANE_ON;
    fake_plane.facts.plane_name = 257;
    fake_plane.facts.plane_ready = true;
    fake_plane.facts.plane_pic_w = 320;
    fake_plane.facts.plane_pic_h = 180;
    fake_plane.facts.plane_serial = 7;
    fake_plane.pixels = plane_pic;
    fake_plane.w = 320;
    fake_plane.h = 180;
    fake_plane.serial = 7;
    plane_screen = (os64_draw_ctx_t){0};
    plane_screen.surf.width = plane_screen.surf.pitch_px = 800;
    plane_screen.surf.height = 600;
    plane_screen.surf.pixels = os64_calloc(800u * 600u, sizeof(uint32_t));
    plane_saved_ctx = g.ui.ctx;
    g.ui.ctx = &plane_screen;
    g.paint_cache = yonder_paint_cache_new(32u << 20);
    g.paint_scratch = yonder_scratch_new();
}

static void plane_drop(void)
{
    planes_release();
    os64_free(s_videos[0].tiles);
    os64_memset(&s_videos[0], 0, sizeof(s_videos[0]));
    for (int32_t i = 0; i < PLANE_HIDES_MAX; i++)
        if (s_plane_hides[i].owed)
            plane_hide_free(i);
    yonder_paint_cache_free(g.paint_cache);
    g.paint_cache = NULL;
    yonder_scratch_free(g.paint_scratch);
    g.paint_scratch = NULL;
    yonder_canvas_keeps_free(g.canvas_keeps);       /* made at a paint, as the window's close frees them */
    g.canvas_keeps = NULL;
    os64_free(plane_screen.surf.pixels);
    g.ui.ctx = plane_saved_ctx;
    fake_plane = (__typeof__(fake_plane)){0};
    planes_counts_restart();
    probe_drop();
}

/* A widget that paints a pill the colour of the planes' plain frame. */
static void plane_pill_paint(os64_ui_widget_t *w, os64_draw_ctx_t *ctx, const os64_ui_theme_t *t)
{
    (void)t;
    os64_draw_fill_round_rect(&ctx->surf, w->bounds, w->bounds.h / 2, 0xff204060u);
}

/* The whole window rendered, as the render after planes_prepare would. */
static os64_gui_rect_t plane_render(void)
{
    g.ui.dirty = g.view.bounds;
    view_paint(&g.view, &plane_screen, NULL);
    return g.view.bounds;
}

static const char *const kPlanePage =
    "<!doctype html><style>body{margin:0;background:#123}"
    "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}</style><video id=v></video>";

static void plane_cases(void)
{
    Video *v = &s_videos[0];

    /* Nothing over the picture: placed, the clip a hole, one place edit
     * with the fitted picture as its origin. */
    plane_page(kPlanePage);
    planes_prepare();
    check(v->candidate && fake_plane.want && fake_plane.pins == 1, "planes: nothing over it, a candidate, wanted");
    os64_gui_rect_t d = plane_render();
    check(!planes_judge(&d, 0, 1) && v->place, "planes: nothing over it, placed");
    planes_hole(&d, 1);
    bool hole = v->clip.w == 320 && v->clip.h == 180;
    for (int32_t y = v->clip.y; y < v->clip.y + v->clip.h && hole; y++)
        for (int32_t x = v->clip.x; x < v->clip.x + v->clip.w && hole; x++)
            hole = plane_screen.surf.pixels[(size_t)y * 800 + x] == 0;
    check(hole, "planes: the clip a see-through hole");
    check(plane_screen.surf.pixels[(size_t)(v->clip.y - 1) * 800 + v->clip.x] != 0, "planes: above the clip, not");
    os64_gui_plane_edit_t edits[OS64_GUI_PLANE_EDITS_MAX];
    os64_gui_rect_t all = d;
    uint32_t n = planes_edits(edits, &all, true);
    check(n == 1 && edits[0].plane == 257 && !edits[0].hide && edits[0].origin.w == 320 && edits[0].origin.h == 180 &&
              os64_memcmp(&edits[0].clip, &v->clip, sizeof(v->clip)) == 0,
          "planes: one place edit, the fitted picture its origin");
    planes_commit();
    check(v->placed && v->moves_on == 1 && v->reason == YONDER_PLANE_PLACED, "planes: committed, one move onto it");
    planes_release();
    check(fake_plane.pins == 0, "planes: the view given back after the paint");

    /* Placed, then the page moves under it: the paint that paints the moved
     * page carries the moved place, worked out from the scroll and layout
     * that render paints, and the hole is cut where the picture now is. */
    {
        os64_gui_rect_t was = v->placed_clip, was_origin = v->placed_origin;
        g.sy += 20;
        planes_prepare();
        d = plane_render();
        check(!planes_judge(&d, 0, 1) && v->place, "planes: moved, still placed");
        planes_hole(&d, 1);
        all = d;
        n = planes_edits(edits, &all, true);
        check(n == 1 && !edits[0].hide && edits[0].origin.y == was_origin.y - 20 &&
                  edits[0].origin.x == was_origin.x && edits[0].clip.y == was.y - 20,
              "planes: the moved place rides the paint of the moved page, from its scroll");
        check(plane_screen.surf.pixels[(size_t)(was.y - 20 + 5) * 800 + was.x + 5] == 0 &&
                  plane_screen.surf.pixels[(size_t)(was.y + was.h - 5) * 800 + was.x + 5] != 0,
              "planes: the hole cut at the new place, and the old place's foot repainted");
        planes_commit();
        planes_release();
        g.sy -= 20;
        planes_prepare();
        d = plane_render();
        (void)planes_judge(&d, 0, 1);
        planes_hole(&d, 1);
        all = d;
        (void)planes_edits(edits, &all, true);
        planes_commit();
        planes_release();
    }

    /* Placed, then what lies over it cannot be taken as a layer (here the
     * layers' room is full when one is needed): it leaves, its old clip
     * marked to be painted again, and the next edit is its hide. */
    planes_prepare();
    d = plane_render();
    plane_screen.surf.pixels[(size_t)(v->clip.y + 10) * 800 + v->clip.x + 10] ^= 0x00ffffffu;
    s_plane_nlayers = PLANE_LAYERS_MAX;
    check(planes_judge(&d, 0, 1) && !v->place && v->reason == YONDER_PLANE_NOT_LAYER && v->not_layer,
          "planes: placed, no layer of what lies over it: it leaves");
    s_plane_nlayers = 0;
    all = d;
    n = planes_edits(edits, &all, true);
    check(n == 1 && edits[0].hide && edits[0].plane == 257, "planes: the leaving video's hide");
    planes_commit();
    check(!v->placed && v->moves_off == 1, "planes: committed, one move off it");
    planes_release();
    /* The next render tells the map, though the video is no candidate, and
     * the part forgotten is known again; the paint after places it. */
    planes_prepare();
    check(!v->candidate && v->reason == YONDER_PLANE_NOT_LAYER && v->mapped, "planes: no layer, still mapped");
    d = plane_render();
    (void)planes_judge(&d, 0, 1);
    check(!v->not_layer, "planes: the part painted again, nothing over it, known clear again");
    planes_release();
    planes_prepare();
    check(v->candidate, "planes: a candidate again");
    plane_drop();

    /* Something opaque over the picture from the start: placed, with an
     * overlay over it in the edit, the box's pixels opaque in the hole and
     * the rest of the clip see-through; --plane-check finds nothing wrong. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px;background:#456}"
               "#o{position:absolute;left:60px;top:60px;width:50px;height:20px;background:red}</style>"
               "<video id=v></video><div id=o></div>");
    s_plane_check = 1;
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    planes_hole(&d, 1);
    all = d;
    n = planes_edits(edits, &all, true);
    {
        os64_gui_rect_t red = {g.view.bounds.x + 60, g.view.bounds.y + 60, 50, 20};
        bool covered = true, opaque = true, clear = true;
        for (int32_t y = v->clip.y; y < v->clip.y + v->clip.h; y++)
            for (int32_t x = v->clip.x; x < v->clip.x + v->clip.w; x++) {
                uint32_t px = plane_screen.surf.pixels[(size_t)y * 800 + x];
                bool in_red = x >= red.x && x < red.x + red.w && y >= red.y && y < red.y + red.h;
                if (in_red) {
                    opaque &= px == 0xffff0000u;
                    bool in = false;
                    for (uint32_t k = 0; k < edits[0].noverlays && !in; k++)
                        in = x >= edits[0].overlay[k].x && x < edits[0].overlay[k].x + edits[0].overlay[k].w &&
                             y >= edits[0].overlay[k].y && y < edits[0].overlay[k].y + edits[0].overlay[k].h;
                    covered &= in;
                } else {
                    clear &= px == 0;
                }
            }
        check(v->place && n == 1 && !edits[0].hide && edits[0].noverlays >= 1, "planes: an opaque box over it: placed, overlays in the edit");
        check(opaque && covered, "planes: the box opaque in the hole, inside the overlays");
        check(clear, "planes: the rest of the clip see-through");
        check(v->checked > 0 && v->checked_wrong == 0, "planes: --plane-check, nothing wrong");
    }
    planes_release();
    /* A layer gone wrong (its red made green) is what --plane-check is for. */
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    for (uint32_t k = 0; k < s_plane_nlayers; k++) {
        os64_gui_rect_t at = s_plane_layers[k].at;
        for (size_t i = 0; i < (size_t)at.w * (size_t)at.h; i++)
            if (s_plane_layers[k].px[i] == 0xffff0000u)
                s_plane_layers[k].px[i] = 0xff00ff00u;
    }
    {
        uint64_t wrong = v->checked_wrong;
        planes_hole(&d, 1);
        check(v->checked_wrong > wrong, "planes: --plane-check finds a wrong layer");
    }
    s_plane_check = 0;
    plane_drop();

    /* An overlay the colour of the picture under it (black over a black
     * frame): its cut is noted, so its layer is made, opaque black. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#o{position:absolute;left:60px;top:60px;width:50px;height:20px;background:#000}</style>"
               "<video id=v></video><div id=o></div>");
    for (uint32_t i = 0; i < 320u * 180u; i++)
        plane_pic[i] = 0xff000000u;
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    planes_hole(&d, 1);
    check(v->place && v->noverlays >= 1 &&
              plane_screen.surf.pixels[(size_t)(g.view.bounds.y + 65) * 800 + g.view.bounds.x + 70] == 0xff000000u,
          "planes: black over a black frame is a layer, opaque black");
    plane_drop();

    /* The same overlay filtered: its boxes draw on a layer's sheets, and
     * laying the layer onto the window is its drawing, noted all the same. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#o{position:absolute;left:60px;top:60px;width:50px;height:20px;background:#000;"
               "filter:brightness(.5)}</style>"
               "<video id=v></video><div id=o></div>");
    for (uint32_t i = 0; i < 320u * 180u; i++)
        plane_pic[i] = 0xff000000u;
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    check(v->place && v->noverlays >= 1, "planes: a filtered black overlay on a black frame is a layer over it");
    plane_drop();

    /* See-through over it: half-red controls and a caption's text, whose
     * edges are partly covered. Placed; the layer's coverage between 0 and
     * 255, and --plane-check off by one at most. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#c{position:absolute;left:40px;top:180px;width:320px;height:40px;background:rgba(255,0,0,.5)}"
               "#t{position:absolute;left:60px;top:150px;margin:0;font-size:14px;color:#fff}</style>"
               "<video id=v></video><div id=c></div><p id=t>A caption over the picture</p>");
    s_plane_check = 1;
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    planes_hole(&d, 1);
    {
        uint32_t half = plane_screen.surf.pixels[(size_t)(g.view.bounds.y + 200) * 800 + g.view.bounds.x + 100];
        check(v->place && (half >> 24) >= 126 && (half >> 24) <= 129 && (half & 0xff0000u) >> 16 >= 126 &&
                  (half & 0xffffu) == 0,
              "planes: half-red controls are a half-covered layer, premultiplied");
        check(v->checked > 0 && v->checked_wrong == 0, "planes: see-through and text, --plane-check nothing wrong");
    }
    s_plane_check = 0;
    plane_drop();

    /* Three see-through boxes stacked over the picture, each rounding on
     * its own: the largest difference --plane-check finds is printed, and
     * must stay within the rule's two (VIDEO_PLANE.md § 5.6). */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "div{position:absolute;left:60px;top:60px;width:200px;height:100px}"
               "#a{background:rgba(255,40,0,.35)}#b{background:rgba(0,200,90,.45);left:90px}"
               "#c{background:rgba(30,60,250,.55);left:120px}</style>"
               "<video id=v></video><div id=a></div><div id=b></div><div id=c></div>");
    s_plane_check = 1;
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    planes_hole(&d, 1);
    {
        printf("planes: three stacked see-through boxes: the largest difference %d; off by one %lu, by two "
               "%lu, wrong %lu\n", (int)v->checked_far_most, (unsigned long)v->checked_near,
               (unsigned long)v->checked_two, (unsigned long)v->checked_wrong);
        check(v->place && v->checked > 0 && v->checked_wrong == 0,
              "planes: three stacked see-through boxes stay within the rule's two");
    }
    s_plane_check = 0;
    plane_drop();

    /* More over it than the kernel blends: an opaque box over most of a
     * 640x360 picture needs overlays past the budget, so it is not placed. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:640px;height:360px}"
               "#o{position:absolute;left:40px;top:40px;width:640px;height:300px;background:red}</style>"
               "<video id=v></video><div id=o></div>");
    {
        /* The page's picture made larger than the budget's worth: 640x360. */
        static uint32_t big[640 * 360];
        for (uint32_t i = 0; i < 640u * 360u; i++)
            big[i] = 0xff204060u;
        fake_plane.pixels = big;
        fake_plane.w = s_videos[0].width = s_videos[0].asked_w = 640;
        fake_plane.h = s_videos[0].height = s_videos[0].asked_h = 360;
        fake_plane.facts.plane_pic_w = 640;
        fake_plane.facts.plane_pic_h = 360;
    }
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    check(v->mapped && !v->place && v->reason == YONDER_PLANE_BUDGET && v->overlay_area > PLANE_OVERLAY_BUDGET,
          "planes: more over it than the budget: not placed, the budget its reason");
    plane_drop();

    /* A widget libui paints over the clip, after the view: a pill the very
     * colour of the frame beneath it. Its coverage comes from the same two
     * paints as the page's, never from its colour: opaque at its body,
     * see-through at its corners. */
    plane_page(kPlanePage);
    {
        static const os64_ui_class_t kPill = {.name = "pill", .paint = plane_pill_paint};
        os64_ui_widget_t root = {0}, over = {0};
        os64_ui_widget_t *saved_root = g.ui.root, *saved_next = g.view.next_sibling;
        for (uint32_t i = 0; i < 320u * 180u; i++)
            plane_pic[i] = 0xff204060u;
        root.first_child = &g.view;
        g.view.next_sibling = &over;
        over.cls = &kPill;
        over.bounds = (os64_gui_rect_t){g.view.bounds.x + 100, g.view.bounds.y + 100, 60, 20};
        g.ui.root = &root;
        planes_prepare();
        d = plane_render();
        plane_pill_paint(&over, &plane_screen, NULL);     /* the render's widget, after the view */
        planes_judge(&d, 0, 1);
        planes_hole(&d, 1);
        uint32_t body = plane_screen.surf.pixels[(size_t)(over.bounds.y + 10) * 800 + over.bounds.x + 30];
        uint32_t corner = plane_screen.surf.pixels[(size_t)over.bounds.y * 800 + over.bounds.x];
        check(v->place && v->noverlays >= 1 && body == 0xff204060u,
              "planes: a widget the picture's colour, opaque at its body, with an overlay");
        check(corner == 0, "planes: and see-through at its corners");
        check(plane_screen.surf.pixels[(size_t)(over.bounds.y + 30) * 800 + over.bounds.x + 5] == 0,
              "planes: beside the widget, see-through");
        /* Placed with that overlay; a different overlay next paint is a new
         * place, an edit of its own. */
        all = d;
        n = planes_edits(edits, &all, true);
        planes_commit();
        check(n == 1 && plane_place_same(v), "planes: committed with its overlays");
        v->overlays[0].w--;
        check(!plane_place_same(v), "planes: other overlays, another place");
        v->overlays[0].w++;
        planes_release();
        g.ui.root = saved_root;
        g.view.next_sibling = saved_next;
    }
    plane_drop();

    /* A video wider than its scroll box: the box's bar is drawn over the
     * picture after the walk (box_bars), and it is in the layer. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#s{position:absolute;left:40px;top:40px;width:300px;height:180px;overflow:auto}"
               "#v{display:block;width:320px;height:180px}</style><div id=s><video id=v></video></div>");
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    planes_hole(&d, 1);
    {
        uint32_t bar = 0;
        for (int32_t y = v->clip.y + v->clip.h - 12; y < v->clip.y + v->clip.h && v->clip.h > 0; y++)
            bar |= plane_screen.surf.pixels[(size_t)y * 800 + v->clip.x + 150] >> 24;
        check(v->mapped && v->place && bar != 0, "planes: a scroll box's bar over the picture is in the layer");
    }
    plane_drop();

    /* Two videos side by side, the second with a background of its own, and
     * text after both: a box painted after the first video but under the
     * second's picture is over neither, and text beside them is not over
     * them however wide the clip it is painted with; both are placed. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#w{position:absolute;left:400px;top:40px;width:320px;height:180px;background:#456}"
               "p{position:absolute;left:730px;top:60px;width:60px;margin:0;font-size:10px}</style>"
               "<video id=v></video><video id=w></video><p>Text after both videos, over neither.</p>");
    os64_memset(&s_videos[1], 0, sizeof(s_videos[1]));
    s_videos[1].node = probe_id("w");
    s_videos[1].width = s_videos[1].asked_w = 320;
    s_videos[1].height = s_videos[1].asked_h = 180;
    s_videos[1].seen.picture = true;
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    check(v->place && s_videos[1].place, "planes: two videos side by side and text after them, both placed");
    planes_release();
    os64_free(s_videos[1].tiles);
    os64_memset(&s_videos[1], 0, sizeof(s_videos[1]));
    plane_drop();

    /* A see-through group laid over the whole video, whose one child draws
     * beside the picture: opening the group draws nothing, so the video is
     * placed. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#g{position:absolute;left:40px;top:40px;width:400px;height:180px;opacity:.8}"
               "#c{position:absolute;left:340px;top:10px;width:20px;height:20px;background:red}</style>"
               "<video id=v></video><div id=g><div id=c></div></div>");
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    check(v->place, "planes: a group over the video drawing only beside it, placed");
    plane_drop();

    /* Not ready at the fitted size, and ready on an older picture than
     * `shown`'s: both not ready, both wanted. */
    plane_page(kPlanePage);
    fake_plane.facts.plane_pic_w = 300;
    planes_prepare();
    check(!v->candidate && v->reason == YONDER_PLANE_NOT_READY && fake_plane.want,
          "planes: another size: not ready, wanted");
    planes_release();
    fake_plane.facts.plane_pic_w = 320;
    fake_plane.facts.plane_serial = 6;
    planes_prepare();
    check(!v->candidate && v->reason == YONDER_PLANE_NOT_READY, "planes: an older picture than shown's: not ready");
    plane_drop();

    /* Out of view: not wanted. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:2000px;width:320px;height:180px}</style><video id=v></video>");
    planes_prepare();
    check(!v->candidate && v->reason == YONDER_PLANE_OUT_OF_VIEW && !fake_plane.want, "planes: out of view: not wanted");
    plane_drop();

    /* A session closed with its plane placed: a hide owed, its last picture
     * copied and painted back into the clip while the page stands. */
    plane_page(kPlanePage);
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    planes_hole(&d, 1);
    all = d;
    (void)planes_edits(edits, &all, true);
    planes_commit();
    planes_release();
    os64_gui_rect_t clip = v->placed_clip, origin = v->placed_origin;
    plane_close(v);
    v->tiles = NULL;
    check(s_plane_hides[0].owed && s_plane_hides[0].copy != NULL && s_plane_hides[0].name == 257 &&
              fake_plane.pins == 0,
          "planes: closed placed, a hide owed with a copy");
    os64_memset(v, 0, sizeof(*v));
    (void)plane_render();
    planes_hides_let_go();
    check(s_plane_hides[0].owed && s_plane_hides[0].copy == NULL, "planes: the copy let go after its render");
    bool back = true;
    for (int32_t y = clip.y; y < clip.y + clip.h && back; y++)
        for (int32_t x = clip.x; x < clip.x + clip.w && back; x++)
            back = plane_screen.surf.pixels[(size_t)y * 800 + x] ==
                   plane_pic[(size_t)(y - origin.y) * 320 + (x - origin.x)];
    check(back, "planes: the last picture painted back where the hole was, in the box's own paint");
    all = d;
    n = planes_edits(edits, &all, true);
    check(n == 1 && edits[0].hide && edits[0].plane == 257, "planes: the owed hide in the next publish");
    plane_drop();

    /* Closed placed, then scrolled before the publish: the copy is not
     * painted where the page has moved, and it goes all the same. */
    plane_page(kPlanePage);
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    planes_hole(&d, 1);
    all = d;
    (void)planes_edits(edits, &all, true);
    planes_commit();
    planes_release();
    clip = v->placed_clip;
    plane_close(v);
    v->tiles = NULL;
    os64_memset(v, 0, sizeof(*v));
    origin = s_plane_hides[0].origin;
    g.sy += 40;
    (void)plane_render();
    g.sy -= 40;
    planes_hides_let_go();
    check(plane_screen.surf.pixels[(size_t)(clip.y + 5) * 800 + clip.x + 5] !=
                  plane_pic[(size_t)(clip.y + 5 - origin.y) * 320 + (clip.x + 5 - origin.x)] &&
              s_plane_hides[0].owed && s_plane_hides[0].copy == NULL,
          "planes: scrolled since the close, the copy not painted, and let go");
    plane_drop();

    /* No plane yet: asked at the first cover, once; not at all with planes
     * off. */
    plane_page(kPlanePage);
    fake_plane.facts.plane_state = OS64_MEDIA_PLANE_NONE;
    v->frame = (os64_media_player_frame_t){plane_pic, 320, 180, 1, true};
    planes_prepare();
    planes_prepare();
    check(fake_plane.asks == 1 && v->plane_asked, "planes: asked at the first cover, once");
    plane_drop();
    plane_page(kPlanePage);
    fake_plane.facts.plane_state = OS64_MEDIA_PLANE_NONE;
    v->frame = (os64_media_player_frame_t){plane_pic, 320, 180, 1, true};
    s_planes = false;
    planes_prepare();
    s_planes = true;
    check(fake_plane.asks == 0, "planes: off, never asked");
    plane_drop();
}

/* WHAT REACHED THE GLASS (--video-rate and the record): a video on the
 * page's paint reaches the glass only with the page's publishes, so a held
 * page shows its rate fall and its gap rise; on its plane, the show
 * thread's publishes keep reaching it while the page is held. Each loop
 * turn runs videos_glass_watch, as videos_tick does. */
static void glass_paint_once(void)
{
    videos_glass_watch();
    planes_prepare();
    (void)plane_render();
    videos_glass_published();
    planes_release();
}

/* The plane's show thread publishing `n` pictures, one each `step` us, in
 * one run (`fresh`: the first starts a new run). */
static void glass_plane_publishes(int n, int64_t step, bool fresh)
{
    for (int k = 0; k < n; k++) {
        clock_us += step;
        fake_plane.facts.plane_placed_published++;
        if (fresh && k == 0) {
            fake_plane.facts.plane_placed_first_us = clock_us;
            fake_plane.facts.plane_placed_runs++;
        }
        fake_plane.facts.plane_placed_last_us = clock_us;
    }
}

static void glass_cases(void)
{
    Video *v = &s_videos[0];
    os64_media_player_facts_t f;

    /* On the page's paint: a picture a paint, 33 ms apart, then a 3 s hold. */
    plane_page(kPlanePage);
    v->playing = true;
    glass_paint_once();
    check(v->page_pictures == 1 && v->glass_watch_from_us == clock_us && v->glass_last_us == clock_us,
          "glass: the page's first picture, the watch begun at it");
    for (int k = 0; k < 30; k++) {
        clock_us += 33000;
        fake_plane.serial++;
        glass_paint_once();
    }
    video_glass_open_gap(v, os64_micros());
    check(v->page_pictures == 31 && v->glass_gap_us == 33000, "glass: on the page's paint, 31 pictures, 33 ms apart");
    v->glass_gap_us = v->glass_gap_reading_us = 0;
    uint64_t before = video_glass_pictures(v, &fake_plane.facts);
    clock_us += 1000000;            /* the page's thread held: no turn, no paint, */
    fake_plane.serial += 30;        /* while the pace goes on picking */
    video_glass_open_gap(v, os64_micros());
    check(video_glass_pictures(v, &fake_plane.facts) == before && v->glass_gap_us == 1000000,
          "glass: held on the page's paint, no picture reaches it and the gap grows");
    v->glass_gap_us = 0;            /* a rate line written mid-hold */
    clock_us += 2000000;
    fake_plane.serial += 60;
    clock_us += 33000;
    fake_plane.serial++;
    glass_paint_once();
    check(v->glass_gap_us == 3033000 && v->glass_gap_reading_us == 3033000,
          "glass: the picture ending the hold counts it whole, in the next line and in the reading");
    /* A pause between two pictures is no gap: the watch ends at the turn
     * that sees it, and starts again at the turn that sees it play. */
    v->glass_gap_us = 0;
    v->playing = false;
    videos_glass_watch();
    check(v->glass_watch_from_us == 0, "glass: paused, the watch ended");
    clock_us += 2000000;
    v->playing = true;
    fake_plane.serial++;
    glass_paint_once();
    check(v->glass_gap_us == 0 && v->glass_last_us == clock_us, "glass: a pause of 2 s is no gap");
    /* Placed, a page paint that renders its box with a newer picture puts
     * none of the page's on the glass: the box is a hole. */
    uint64_t page = v->page_pictures;
    uint32_t serial = v->glass_serial;
    v->placed = true;
    fake_plane.serial++;
    glass_paint_once();
    check(v->glass_serial != serial && v->page_pictures == page,
          "glass: placed, the page's render of its box with a newer picture is no picture on the glass");
    v->placed = false;
    plane_drop();

    /* On its plane: the show thread publishes every 33 ms while the page is
     * held 3 s; the plane's own longest gap is the player's. The run's first
     * picture came 400 ms after the page's last: that gap is the glass's. */
    plane_page(kPlanePage);
    v->playing = true;
    glass_paint_once();
    v->glass_gap_us = 0;
    v->placed = true;
    glass_plane_publishes(1, 400000, true);
    glass_plane_publishes(89, 33000, false);
    fake_plane.gap = 34000;
    video_glass_open_gap(v, os64_micros());
    f = fake_plane.facts;
    check(video_glass_pictures(v, &f) == 91 && v->glass_gap_us == 400000,
          "glass: on its plane, the page's last picture to the run's first is a gap");
    v->glass_gap_us = 0;
    glass_plane_publishes(90, 33000, false);
    fake_plane.gap = 34000;
    video_glass_open_gap(v, os64_micros());
    check(v->glass_gap_us == 34000, "glass: on its plane through a hold, the player's own 34 ms gap");
    /* The plane stops publishing (taken off, or the show thread held): the
     * gap runs from its last picture, not from the last line. */
    v->glass_gap_us = 0;
    clock_us += 500000;
    video_glass_open_gap(v, os64_micros());
    check(v->glass_gap_us == 500000, "glass: the gap after the plane's last picture runs from it");
    clock_us += 200000;
    video_glass_open_gap(v, os64_micros());
    check(v->glass_gap_us == 700000, "glass: and is not cut by the line before");
    /* Paused and played again, a run's pictures from before the pause start
     * no gap in the new watch. */
    v->playing = false;
    videos_glass_watch();
    clock_us += 3000000;
    v->playing = true;
    videos_glass_watch();
    v->glass_gap_us = 0;
    clock_us += 10000;
    video_glass_open_gap(v, os64_micros());
    check(v->glass_gap_us == 10000, "glass: the plane's pictures from before a pause are not the new watch's");
    /* Two runs begun between folds: the time before the latest holds the
     * earlier run's pictures, so no bridge is counted from the last fold. */
    v->glass_gap_us = 0;
    uint64_t runs = fake_plane.facts.plane_placed_runs;
    glass_plane_publishes(30, 33000, true);
    clock_us += 100000;
    glass_plane_publishes(30, 33000, true);
    check(fake_plane.facts.plane_placed_runs == runs + 2, "glass: two runs begun");
    video_glass_open_gap(v, os64_micros());
    check(v->glass_gap_us == 0, "glass: two runs between folds bridge nothing");
    /* The player's gap inside a watch survives the watch's end. */
    v->glass_gap_us = 0;
    fake_plane.gap = 2000000;
    v->playing = false;
    videos_glass_watch();
    check(v->glass_gap_us == 2000000 && v->glass_watch_from_us == 0,
          "glass: a stall the player saw before a pause is counted as the watch ends");
    v->playing = true;
    videos_glass_watch();
    v->glass_gap_us = v->glass_gap_reading_us = 0;
    clock_us += 10000;
    /* The record's reading says the same, and starts again. */
    f = fake_plane.facts;
    DiagText t = {0};
    video_glass_reading(&t, v, &f);
    check(t.s != NULL && strstr(t.s, "reached the glass 241 pictures since the last reading, the longest without a "
                                     "new one 10 ms") != NULL,
          "glass: the record's words");
    os64_free(t.s);
    videos_glass_reading_restart();
    t = (DiagText){0};
    video_glass_reading(&t, v, &f);
    check(t.s != NULL && strstr(t.s, "reached the glass 0 pictures since the last reading, the longest without a "
                                     "new one 10 ms") != NULL,
          "glass: a reading starts the words again, the open gap still counted");
    os64_free(t.s);
    v->placed = false;
    plane_drop();

    /* A paint of only the box's letterboxing draws none of the picture: no
     * picture reaches the glass, and the next paint that draws it counts. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:320px}</style><video id=v></video>");
    v->playing = true;
    videos_glass_watch();
    planes_prepare();       /* `shown` pinned, the picture the paint draws */
    g.ui.dirty = (os64_gui_rect_t){g.view.bounds.x + 40, g.view.bounds.y + 40, 320, 40};
    view_paint(&g.view, &plane_screen, NULL);
    check(v->view.frame != NULL && !v->painted_new && v->glass_serial == 0,
          "glass: a paint of the letterboxing alone draws no picture");
    (void)plane_render();
    check(v->painted_new && v->glass_serial == fake_plane.serial, "glass: the paint that draws it does");
    planes_release();
    v->painted_new = false;
    plane_drop();
}
