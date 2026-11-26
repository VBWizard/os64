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
    os64_free(plane_screen.surf.pixels);
    g.ui.ctx = plane_saved_ctx;
    fake_plane = (__typeof__(fake_plane)){0};
    planes_counts_restart();
    probe_drop();
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
    uint32_t n = planes_edits(edits, &all);
    check(n == 1 && edits[0].plane == 257 && !edits[0].hide && edits[0].origin.w == 320 && edits[0].origin.h == 180 &&
              os64_memcmp(&edits[0].clip, &v->clip, sizeof(v->clip)) == 0,
          "planes: one place edit, the fitted picture its origin");
    planes_commit();
    check(v->placed && v->moves_on == 1 && v->reason == YONDER_PLANE_PLACED, "planes: committed, one move onto it");
    planes_release();
    check(fake_plane.pins == 0, "planes: the view given back after the paint");

    /* Placed, then the page draws over the picture: it leaves, its old clip
     * marked to be painted again, and the next edit is its hide. */
    planes_prepare();
    d = plane_render();
    plane_screen.surf.pixels[(size_t)(v->clip.y + 10) * 800 + v->clip.x + 10] ^= 0x00ffffffu;
    check(planes_judge(&d, 0, 1) && !v->place && v->reason == YONDER_PLANE_OVER,
          "planes: placed, something drawn over it: it leaves");
    all = d;
    n = planes_edits(edits, &all);
    check(n == 1 && edits[0].hide && edits[0].plane == 257, "planes: the leaving video's hide");
    planes_commit();
    check(!v->placed && v->moves_off == 1, "planes: committed, one move off it");
    plane_drop();

    /* Something over the picture from the start: never placed. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#o{position:absolute;left:60px;top:60px;width:50px;height:20px;background:red}</style>"
               "<video id=v></video><div id=o></div>");
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    all = d;
    check(!v->place && v->reason == YONDER_PLANE_OVER && planes_edits(edits, &all) == 0,
          "planes: something over it: not placed, no edit");
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
    (void)planes_edits(edits, &all);
    planes_commit();
    planes_release();
    os64_gui_rect_t clip = v->placed_clip, origin = v->placed_origin;
    plane_close(v);
    v->tiles = NULL;
    check(s_plane_hides[0].owed && s_plane_hides[0].copy != NULL && s_plane_hides[0].name == 257 &&
              fake_plane.pins == 0,
          "planes: closed placed, a hide owed with a copy");
    planes_hides_paint();
    bool back = true;
    for (int32_t y = clip.y; y < clip.y + clip.h && back; y++)
        for (int32_t x = clip.x; x < clip.x + clip.w && back; x++)
            back = plane_screen.surf.pixels[(size_t)y * 800 + x] ==
                   plane_pic[(size_t)(y - origin.y) * 320 + (x - origin.x)];
    check(back, "planes: the last picture painted back where the hole was");
    os64_memset(v, 0, sizeof(*v));
    all = d;
    n = planes_edits(edits, &all);
    check(n == 1 && edits[0].hide && edits[0].plane == 257, "planes: the owed hide in the next publish");
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
