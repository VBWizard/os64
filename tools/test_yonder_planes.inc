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

    /* Placed, then the page draws over the picture: it leaves, its old clip
     * marked to be painted again, and the next edit is its hide. */
    planes_prepare();
    d = plane_render();
    plane_screen.surf.pixels[(size_t)(v->clip.y + 10) * 800 + v->clip.x + 10] ^= 0x00ffffffu;
    check(planes_judge(&d, 0, 1) && !v->place && v->reason == YONDER_PLANE_OVER,
          "planes: placed, something drawn over it: it leaves");
    all = d;
    n = planes_edits(edits, &all, true);
    check(n == 1 && edits[0].hide && edits[0].plane == 257, "planes: the leaving video's hide");
    planes_commit();
    check(!v->placed && v->moves_off == 1, "planes: committed, one move off it");
    planes_release();
    /* The ink goes: the next render tells the map, though the video is no
     * candidate, and the paint after places it again. */
    planes_prepare();
    check(!v->candidate && v->reason == YONDER_PLANE_OVER && v->mapped, "planes: inked, still mapped");
    d = plane_render();
    (void)planes_judge(&d, 0, 1);
    planes_release();
    planes_prepare();
    check(v->candidate, "planes: the ink gone, a candidate again");
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
    check(!v->place && v->reason == YONDER_PLANE_OVER && planes_edits(edits, &all, true) == 0,
          "planes: something over it: not placed, no edit");
    plane_drop();

    /* An overlay the colour of the picture under it (black over a black
     * frame): its cut is noted, so it is something over it all the same. */
    plane_page("<!doctype html><style>body{margin:0;background:#123}"
               "#v{position:absolute;left:40px;top:40px;width:320px;height:180px}"
               "#o{position:absolute;left:60px;top:60px;width:50px;height:20px;background:#000}</style>"
               "<video id=v></video><div id=o></div>");
    for (uint32_t i = 0; i < 320u * 180u; i++)
        plane_pic[i] = 0xff000000u;
    planes_prepare();
    d = plane_render();
    planes_judge(&d, 0, 1);
    check(!v->place && v->reason == YONDER_PLANE_OVER, "planes: black over a black frame is something over it");
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
    check(!v->place && v->reason == YONDER_PLANE_OVER, "planes: a filtered black overlay on a black frame is over it");
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
        if (fresh && k == 0)
            fake_plane.facts.plane_placed_first_us = clock_us;
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
    /* The record's reading says the same, and starts again. */
    f = fake_plane.facts;
    DiagText t = {0};
    video_glass_reading(&t, v, &f);
    check(t.s != NULL && strstr(t.s, "reached the glass 181 pictures since the last reading, the longest without a "
                                     "new one 700 ms") != NULL,
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
}
