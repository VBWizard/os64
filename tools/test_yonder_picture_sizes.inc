/* Pictures kept at the size they are drawn (PICTURE_SIZES.md): the pure
 * rules in scale.c, the worker that reduces a decode, and the page that
 * asks, judges, resamples and draws. The fixture decoder's "SIZED <w>x<h>"
 * is the picture; the network is picture_http_bytes. */

static double sizes_min(double a, double b) { return a < b ? a : b; }
static double sizes_max(double a, double b) { return a > b ? a : b; }
static uint32_t sizes_round(double v) { return (uint32_t)(v + 0.5); }

static uint32_t sizes_ref_pixel(const uint32_t *src, uint32_t sw, uint32_t sh, uint32_t dw,
                                uint32_t dh, uint32_t ox, uint32_t oy)
{
    /* The definition, in doubles: every source pixel weighed by the area of
     * it under the output pixel, and by its alpha. */
    double x0 = (double)ox * sw / dw, x1 = (double)(ox + 1) * sw / dw;
    double y0 = (double)oy * sh / dh, y1 = (double)(oy + 1) * sh / dh;
    double a = 0, c[3] = {0, 0, 0}, area = 0;
    for (uint32_t y = (uint32_t)y0; y < sh && y < y1; y++)
        for (uint32_t x = (uint32_t)x0; x < sw && x < x1; x++) {
            double w = (sizes_min(x1, x + 1.0) - sizes_max(x0, x)) * (sizes_min(y1, y + 1.0) - sizes_max(y0, y));
            uint32_t p = src[(size_t)y * sw + x];
            double wa = w * (p >> 24);
            area += w;
            a += wa;
            c[0] += wa * ((p >> 16) & 0xff);
            c[1] += wa * ((p >> 8) & 0xff);
            c[2] += wa * (p & 0xff);
        }
    if (a == 0)
        return 0;
    return sizes_round(a / area) << 24 | sizes_round(c[0] / a) << 16 | sizes_round(c[1] / a) << 8 |
           sizes_round(c[2] / a);
}

static void sizes_pure_cases(void)
{
    uint32_t kw, kh;
    yonder_picture_kept_size(400, 200, 100, 50, &kw, &kh);
    check(kw == 100 && kh == 50, "sizes: a rectangle of the picture's shape is kept exactly");
    yonder_picture_kept_size(400, 200, 200, 50, &kw, &kh);
    check(kw == 200 && kh == 100, "sizes: the axis that asks more is met, the other keeps the shape");
    yonder_picture_kept_size(400, 200, 30, 90, &kw, &kh);
    check(kw == 180 && kh == 90, "sizes: and so on the other axis");
    yonder_picture_kept_size(400, 200, 401, 10, &kw, &kh);
    check(kw == 400 && kh == 200, "sizes: never past its own size");
    yonder_picture_kept_size(400, 200, 0, 50, &kw, &kh);
    check(kw == 400 && kh == 200, "sizes: no rectangle keeps it whole");
    yonder_picture_kept_size(1000, 1, 3, 3, &kw, &kh);
    check(kw == 1000 && kh == 1, "sizes: a strip met on its short axis stays whole");
    yonder_picture_kept_size(7, 3, 2, 1, &kw, &kh);
    check(kw == 3 && kh == 1, "sizes: the other axis rounds up");
    /* Asked again of its own answer, it answers no less: a picture kept at
     * what it said is never found smaller than it needs. */
    uint32_t seed = 12345, bad = 0;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t nw = 1 + (seed >> 8) % 3000, nh = 1 + (seed >> 3) % 2000;
        seed = seed * 1103515245u + 12345u;
        uint32_t dw = 1 + (seed >> 8) % 3500, dh = 1 + (seed >> 3) % 2500, aw, ah;
        yonder_picture_kept_size(nw, nh, dw, dh, &kw, &kh);
        yonder_picture_kept_size(nw, nh, kw, kh, &aw, &ah);
        bad += aw < kw || ah < kh || kw > nw || kh > nh || kw == 0 || kh == 0 ||
               kw < (dw < nw ? dw : nw) || kh < (dh < nh ? dh : nh);
    }
    check(bad == 0, "sizes: the kept size covers what was asked, within the picture, and is stable asked of itself");

    /* The reduction against the definition, alpha included. */
    static const uint32_t shapes[][4] = {{8, 8, 4, 4}, {7, 5, 3, 2}, {13, 9, 13, 3}, {6, 6, 1, 1},
                                         {1, 9, 1, 4}, {9, 1, 2, 1}, {31, 17, 10, 6}, {5, 5, 5, 5}};
    for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
        uint32_t sw = shapes[s][0], sh = shapes[s][1], dw = shapes[s][2], dh = shapes[s][3];
        uint32_t *src = os64_malloc((size_t)sw * sh * 4), *dst = os64_malloc((size_t)dw * dh * 4);
        for (uint32_t i = 0; i < sw * sh; i++) {
            seed = seed * 1103515245u + 12345u;
            uint32_t a = (seed >> 24) % 4 == 0 ? 0 : (seed >> 16) & 0xff;
            src[i] = a << 24 | (seed & 0xffffff);
        }
        check(yonder_reduce_picture(src, sw, sh, dst, dw, dh), "sizes: a reduction runs");
        uint32_t off = 0;
        for (uint32_t y = 0; y < dh; y++)
            for (uint32_t x = 0; x < dw; x++) {
                uint32_t got = dst[y * dw + x], want = sizes_ref_pixel(src, sw, sh, dw, dh, x, y);
                for (int c = 0; c < 32; c += 8) {
                    int d = (int)((got >> c) & 0xff) - (int)((want >> c) & 0xff);
                    if (d > 1 || d < -1)
                        off++;
                }
            }
        check(off == 0, "sizes: each pixel the alpha-weighed average of the pixels under it");
        os64_free(src);
        os64_free(dst);
    }
    uint32_t one = 0xff102030, out = 0xdeadbeef;
    check(!yonder_reduce_picture(&one, 1, 1, &out, 2, 1) && !yonder_reduce_picture(&one, 1, 1, &out, 0, 1) &&
          out == 0xdeadbeef, "sizes: a reduction that would grow, or to nothing, is refused untouched");
    uint32_t clear[4] = {0, 0, 0, 0};
    check(yonder_reduce_picture(clear, 2, 2, &out, 1, 1) && out == 0, "sizes: transparent stays transparent");
}

static yonder_picture_job_t *sizes_job(uint32_t cw, uint32_t ch)
{
    yonder_picture_job_t *job = os64_calloc(1, sizeof(*job));
    check(job != NULL, "sizes: job allocated");
    job->kind = YONDER_JOB_PICTURE;
    job->generation = g.page_serial;
    os64_strcopy(job->url, sizeof(job->url), "https://t.test/a.png");
    job->cover_w = cw;
    job->cover_h = ch;
    return job;
}

static void sizes_worker_cases(void)
{
    size_t baseline = live;
    picture_http_bytes = "SIZED 400x200"; picture_http_type = "image/png"; picture_http_status = 200;
    picture_http_failure = false;
    void *out = NULL;
    yonder_picture_job_t *job = sizes_job(100, 50);
    check(yonder_picture_run(job, NULL, NULL, &out) == 1, "sizes: a covered decode succeeds");
    yonder_picture_t *p = out;
    check(p && p->width == 400 && p->height == 200 && p->image.width == 100 && p->image.height == 50 &&
          p->cost == 100u * 50u * 4u, "sizes: the worker keeps the covering size, and says the natural one");
    check(p && (p->image.pixels[0] >> 24) == 0xff && (p->image.pixels[0] & 0xff) == 0x40,
          "sizes: the kept pixels are the picture's");
    yonder_picture_release(job, out);
    job = sizes_job(0, 0); out = NULL;
    (void)yonder_picture_run(job, NULL, NULL, &out); p = out;
    check(p && p->image.width == 400 && p->image.height == 200, "sizes: no rectangle keeps it whole");
    yonder_picture_release(job, out);
    job = sizes_job(800, 10); out = NULL;
    (void)yonder_picture_run(job, NULL, NULL, &out); p = out;
    check(p && p->image.width == 400 && p->image.height == 200, "sizes: a rectangle past it keeps it whole");
    yonder_picture_release(job, out);
    /* An SVG asked to cover a rectangle is a document all the same. */
    picture_http_bytes = picture_svg_source; picture_http_type = "image/svg+xml";
    job = sizes_job(10, 5); job->svg_enabled = true; out = NULL;
    check(yonder_picture_run(job, NULL, NULL, &out) == 1 && ((yonder_picture_t *)out)->svg &&
          ((yonder_picture_t *)out)->width == 64, "sizes: an SVG keeps its document");
    yonder_picture_release(job, out);
    picture_http_bytes = "SIZED 400x200"; picture_http_type = "image/png";
    /* Every allocation cut: a picture all the same, whole or kept smaller,
     * or a failure, and nothing left behind; and a cancel before the
     * reduction keeps it whole. */
    job = sizes_job(100, 50); out = NULL;
    size_t before = attempts;
    (void)yonder_picture_run(job, NULL, NULL, &out);
    size_t allocations = attempts - before;
    yonder_picture_release(job, out);
    for (size_t i = 1; i <= allocations; i++) {
        job = sizes_job(100, 50); out = NULL;
        fail_at = attempts + i;
        int64_t v = yonder_picture_run(job, NULL, NULL, &out);
        fail_at = 0;
        p = out;
        check(v != 1 || (p && p->width == 400 && (p->image.width == 100 || p->image.width == 400) &&
                         p->cost == (size_t)p->image.width * p->image.height * 4u),
              "sizes: a cut allocation leaves a whole picture or a kept one, costed as it is");
        yonder_picture_release(job, out);
        check(live == baseline, "sizes: a cut allocation leaves nothing");
    }
    picture_http_bytes = NULL; picture_http_type = NULL;
    check(live == baseline, "sizes: the worker cases leave nothing behind");
}

/* The page side: one picture at a time in the fixture pool's slot. */
static void sizes_open(const char *html, const char *picture)
{
    probe_page(html, false);
    picture_http_bytes = picture; picture_http_type = "image/png"; picture_http_status = 200;
    picture_http_failure = false;
    pool_open = true; g.pool = (os64_work_pool_t *)&pool_fake; memset(&pool_work, 0, sizeof(pool_work));
}
static void sizes_close(void)
{
    if (pool_work.job != NULL) { pool_work.release(pool_work.job, NULL); memset(&pool_work, 0, sizeof(pool_work)); }
    pool_open = false; g.pool = NULL;
    if (g.page.diag != NULL) { yonder_diag_free(g.page.diag); g.page.diag = NULL; }
    probe_drop();
    picture_http_bytes = NULL; picture_http_type = NULL; picture_http_failure = false;
}
static const yonder_picture_job_t *sizes_held(void)
{
    const yonder_picture_job_t *job = pool_work.job;
    return job != NULL && job->kind == YONDER_JOB_PICTURE ? job : NULL;
}
/* The job in the pool, run, arrived and fed after as reaped() does. */
static bool sizes_run(void)
{
    yonder_picture_job_t *job = pool_work.job;
    if (job == NULL || job->kind != YONDER_JOB_PICTURE) return false;
    memset(&pool_work, 0, sizeof(pool_work));
    void *out = NULL;
    int64_t v = yonder_picture_run(job, NULL, NULL, &out);
    picture_arrived(job, out);
    yonder_picture_release(job, out);
    pictures_feed(&g.page);
    return v == 1;
}
static void sizes_layout(uint32_t zoom)
{
    g.zoom = zoom;
    check(page_lay_out(&g.page, 800, 600, zoom, &g.page), "sizes: layout");
    css_pictures(&g.page);
}
static os64_gui_rect_t sizes_box(const char *id)
{
    const flow_box_t *b = flow_box_for(g.page.tree, probe_id(id));
    return b != NULL ? b->rect : (os64_gui_rect_t){0, 0, 0, 0};
}

static void sizes_page_cases(void)
{
    /* A box layout has fixed: asked for at its size from the start. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block}</style>"
               "<img id=pic src='https://t.test/a.png' width=100 height=50>", "SIZED 400x200");
    pictures_start(&g.page);
    const yonder_picture_job_t *job = sizes_held();
    check(job && job->cover_w == 100 && job->cover_h == 50 && !job->resample,
          "sizes: a fixed box asks to be covered from the first ask");
    check(sizes_run() && g.page.pics[0].width == 400 && g.page.pics[0].image.width == 100 &&
          g.page.kept_bytes == 100u * 50u * 4u, "sizes: and arrives at that size, its own size kept apart");
    sizes_layout(1000);
    check(g.page.resample_owed == 0 && sizes_held() == NULL, "sizes: a picture kept at its need is left alone");
    sizes_close();

    /* A box that waits on the picture: asked for whole, then judged. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:100px}</style>"
               "<img id=pic src='https://t.test/a.png'>", "SIZED 400x200");
    g.page.diag = diag_new_claimed("http://s.test/", 1);
    pictures_start(&g.page);
    job = sizes_held();
    check(job && job->cover_w == 0 && job->cover_h == 0, "sizes: a box that waits on the picture asks for it whole");
    check(sizes_run() && g.page.pics[0].image.width == 400, "sizes: and it arrives whole");
    sizes_layout(1000);
    os64_gui_rect_t was = sizes_box("pic");
    job = sizes_held();
    check(was.w == 100 && was.h == 50 && job && job->resample && job->cover_w == 100 && job->cover_h == 50,
          "sizes: drawn at a quarter, it is asked for again at the size drawn");
    size_t held = live;
    check(sizes_run() && g.page.pics[0].image.width == 100 && g.page.pics[0].image.height == 50 &&
          g.page.pics[0].width == 400 && g.page.kept_bytes == 100u * 50u * 4u && g.page.pics[0].cost == 20000,
          "sizes: the resample replaces the pixels and the account moves by the difference");
    check(live < held, "sizes: the whole decode is let go");
    sizes_layout(1000);
    os64_gui_rect_t now = sizes_box("pic");
    check(now.x == was.x && now.y == was.y && now.w == was.w && now.h == was.h && g.page.resample_owed == 0,
          "sizes: the page lays out the same, and asks nothing more");
    uint32_t *px = stand_in_shot();
    check((px[25 * 800 + 50] & 0xff) == 0x40 && (px[25 * 800 + 50] >> 24) == 0xff,
          "sizes: the kept picture is what is painted");
    os64_free(px);
    diag_observe(&g.page, "when the page was left");
    char text[4096];
    yonder_diag_render(g.page.diag, text, sizeof(text));
    check(strstr(text, "picture sizes: 1 kept at the size they are drawn, 292 KiB less than whole") != NULL,
          "sizes: the record says what was kept smaller and what it spared");
    /* Zoomed in: asked for again bigger; out again: smaller. */
    sizes_layout(2000);
    job = sizes_held();
    check(job && job->resample && job->cover_w == 200 && job->cover_h == 100, "sizes: drawn bigger, it grows");
    check(sizes_run() && g.page.pics[0].image.width == 200, "sizes: to the size drawn");
    sizes_layout(1000);
    job = sizes_held();
    check(job && job->cover_w == 100, "sizes: and shrinks back when the zoom does");
    /* Left with a resample out: cancelled, its job and address released. */
    held = live;
    pictures_leave(&g.page);
    check(pool_work.job == NULL && !g.page.pics[0].resampling && live < held && g.page.in_flight == 0,
          "sizes: leaving the page cancels a resample through its release");
    sizes_close();

    /* Twice the area and not less: 282 pixels wide shrinks, 283 does not. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:283px}</style>"
               "<img id=pic src='https://t.test/a.png'>", "SIZED 400x200");
    pictures_start(&g.page);
    check(sizes_run(), "sizes: whole at 283");
    sizes_layout(1000);
    /* The auto height is 141.5, laid out as 142, and that axis asks more. */
    check(sizes_held() == NULL && g.page.pics[0].need_w == 284 && g.page.pics[0].need_h == 142,
          "sizes: under twice the area, it is kept as it is");
    sizes_close();
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:282px}</style>"
               "<img id=pic src='https://t.test/a.png'>", "SIZED 400x200");
    pictures_start(&g.page);
    check(sizes_run(), "sizes: whole at 282");
    sizes_layout(1000);
    job = sizes_held();
    check(job && job->resample && job->cover_w == 282 && job->cover_h == 141, "sizes: at twice the area it shrinks");
    sizes_close();

    /* A resample that fails keeps the picture, says so, and is not asked
     * for again at that size. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:100px}</style>"
               "<img id=pic src='https://t.test/a.png'>", "SIZED 400x200");
    g.page.diag = diag_new_claimed("http://s.test/", 2);
    pictures_start(&g.page);
    check(sizes_run(), "sizes: whole first");
    sizes_layout(1000);
    picture_http_failure = true;
    check(!sizes_run() && g.page.pics[0].image.width == 400 && g.page.pics[0].state == PIC_SHOWN &&
          sizes_held() == NULL, "sizes: a failed resample keeps the picture, and does not ask again at once");
    sizes_layout(1000);
    check(sizes_held() == NULL, "sizes: nor at the next layout, the need unchanged");
    yonder_diag_render(g.page.diag, text, sizeof(text));
    check(strstr(text, "could not be had again at the size it is drawn") != NULL && strstr(text, "FAILED") == NULL,
          "sizes: on the record as a fact, not a failure");
    picture_http_failure = false;
    sizes_layout(2000);
    check(sizes_held() != NULL, "sizes: a new need asks again");
    sizes_close();

    /* Past the memory kept whole: asked for again at the size drawn. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:100px}</style>"
               "<img id=pic src='https://t.test/a.png'>", "SIZED 400x200");
    g.page.diag = diag_new_claimed("http://s.test/", 3);
    pictures_start(&g.page);
    g.page.kept_bytes = PICTURES_KEPT_MAX - 30000;
    check(sizes_run() && g.page.pics[0].state == PIC_NOT_KEPT && g.page.not_kept == 1 &&
          g.page.pics[0].width == 400 && g.pictures_moved, "sizes: past the memory whole, its size is kept");
    sizes_layout(1000);
    check(sizes_box("pic").h == 50, "sizes: its frame is laid out at its size");
    job = sizes_held();
    check(job && job->resample && job->cover_w == 100, "sizes: and it is asked for at the size drawn");
    check(sizes_run() && g.page.pics[0].state == PIC_SHOWN && g.page.not_kept == 0 &&
          g.page.kept_bytes == PICTURES_KEPT_MAX - 30000 + 20000, "sizes: which fits, and is shown");
    yonder_diag_render(g.page.diag, text, sizeof(text));
    check(strstr(text, "kept at the size it is drawn: whole, it was past the memory kept") != NULL,
          "sizes: the record says so");
    g.page.kept_bytes = 0;
    sizes_close();

    /* Two uses: the larger need wins. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:100px}"
               "div{width:300px;height:150px;background-image:url(https://t.test/a.png);"
               "background-size:200px 100px}</style><img src='https://t.test/a.png'><div></div>",
               "SIZED 400x200");
    pictures_start(&g.page);
    check(sizes_run(), "sizes: two uses, one picture");
    sizes_layout(1000);
    job = sizes_held();
    check(g.page.npics == 1 && job && job->cover_w == 200 && job->cover_h == 100,
          "sizes: kept at the larger of its uses");
    sizes_close();

    /* The canvas's picture is drawn in the ROOT's origin box. */
    sizes_open("<!doctype html><style>html{margin:0}body{margin:100px;height:50px;"
               "background:url(https://t.test/a.png) no-repeat;background-size:100% 100%}</style>",
               "SIZED 1600x800");
    pictures_start(&g.page);
    sizes_layout(1000);
    check(sizes_run(), "sizes: the canvas's picture arrives");
    sizes_layout(1000);
    job = sizes_held();
    const flow_box_t *root = flow_root(g.page.tree);
    uint32_t kw, kh;
    yonder_picture_kept_size(1600, 800, (uint32_t)root->rect.w, (uint32_t)root->rect.h, &kw, &kh);
    check(job && job->cover_w == kw && job->cover_h == kh && kw == 800,
          "sizes: the canvas's picture is kept at the root's origin box, not the body's");
    sizes_close();

    /* An SVG is drawn from its document: never resampled. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:16px}</style>"
               "<img src='https://t.test/a.svg'>", picture_svg_source);
    picture_http_type = "image/svg+xml";
    pictures_start(&g.page);
    check(sizes_run() && g.page.pics[0].kind == YONDER_PICTURE_SVG, "sizes: the SVG arrives");
    sizes_layout(1000);
    check(sizes_held() == NULL && g.page.resample_owed == 0, "sizes: an SVG is not resampled");
    sizes_close();

    /* No memory to ask for a resample: a fact, the picture as it was, and
     * the next layout asks again. */
    sizes_open("<!doctype html><style>body{margin:0}img{display:block;width:100px}</style>"
               "<img id=pic src='https://t.test/a.png'>", "SIZED 400x200");
    pictures_start(&g.page);
    check(sizes_run(), "sizes: whole, again");
    check(page_lay_out(&g.page, 800, 600, 1000, &g.page), "sizes: laid out");
    size_t at = live;
    fail_at = attempts + 1;          /* the resample's job, the walk's first ask */
    css_pictures(&g.page);
    fail_at = 0;
    check(g.page.pics[0].image.width == 400 && !g.page.pics[0].resampling && g.page.resample_owed == 0 &&
          sizes_held() == NULL && live == at, "sizes: a resample with no memory to ask is not asked, nothing held");
    sizes_layout(1000);
    check(sizes_held() != NULL, "sizes: the next layout asks again");
    sizes_close();
}

static void picture_sizes_cases(void)
{
    sizes_pure_cases();
    sizes_worker_cases();
    sizes_page_cases();
}
