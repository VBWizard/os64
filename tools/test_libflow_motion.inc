// tools/test_libflow_motion.inc — animations, the model and its ticks
// (PILE3.md § Animations, slices S8a and S8b), included by
// test_libflow_host.c. Every expected value is headless Chrome 155's
// answer to the probe pages of 10/13 — animations paused at a negative
// delay, read back through getComputedStyle — or worked by hand from Web
// Animations 1, CSS Animations 1 and 2, Easing 1 and Transforms 1 and 2,
// never captured from the engine; but a tick's, which is the layout's at
// the same time, to the bit (the contracts). A length lands on 1/64 px, so
// a translation is held to Chrome's within half of that.

#define MOTION_STYLE "<!doctype html><style>body{margin:0} div{width:100px;height:40px}"
// Paused at a time: a paused animation holds its local time at 0, so a
// delay of -T shows the value at T.
#define AT(d) "animation-play-state:paused;animation-duration:1s;" \
              "animation-timing-function:linear;animation-delay:-" d "s"

typedef struct {
    Sheeted p;
    flow_timeline_t *tl;
    flow_tree_t *tree;
} Moving;

static const os64_html_node_t *motion_find(const os64_html_node_t *n, const char *id)
{
    for (; n != NULL; n = n->next) {
        if (n->kind == OS64_HTML_ELEMENT) {
            const os64_html_attr_t *a = os64_html_attr(n, "id");
            if (a != NULL && a->value != NULL && strcmp(a->value, id) == 0)
                return n;
        }
        const os64_html_node_t *in = motion_find(n->first_child, id);
        if (in != NULL)
            return in;
    }
    return NULL;
}

// The timeline one complete layout builds from none with `n`'s animations
// met in it at `now`, each FAnimation given its record; `*ok` false on no
// memory.
static flow_timeline_t *motion_meet(const os64_html_document_t *doc, const os64_html_node_t *n, FMotion *m,
                                    double now, bool *ok)
{
    flow_timeline_t *none = flow_timeline_new(doc);
    flow_timeline_t *t = none != NULL ? f_timeline_begin(none) : NULL;
    *ok = t != NULL && f_timeline_element(t, n, m, now) && f_timeline_end(t, true);
    flow_timeline_free(none);
    return t;
}

static void moving_open_for(Moving *m, const char *html, uint64_t now, bool ticks)
{
    sheeted_open(&m->p, html, 800, 600);
    m->p.env.ticks = ticks;
    m->tl = flow_timeline_new(m->p.doc);
    m->p.env.timeline = m->tl;
    m->p.env.now_ms = now;
    m->tree = flow_layout(m->p.doc, m->p.page, 800, &m->p.env);
    flow_timeline_commit(&m->tl, m->tree);
    m->p.env.timeline = m->tl;
}

static void moving_open(Moving *m, const char *html, uint64_t now)
{
    moving_open_for(m, html, now, false);
}

// The page laid out again at `now` and shown, as a face does: the tree
// replaced, and its timeline with it.
static void moving_again(Moving *m, uint64_t now)
{
    flow_free(m->tree);
    m->p.env.now_ms = now;
    m->tree = flow_layout(m->p.doc, m->p.page, 800, &m->p.env);
    flow_timeline_commit(&m->tl, m->tree);
    m->p.env.timeline = m->tl;
}

static void moving_close(Moving *m)
{
    flow_free(m->tree);
    flow_timeline_free(m->tl);
    sheeted_close(&m->p);
}

static const flow_box_t *moving_box(const Moving *m, const char *id)
{
    const os64_html_node_t *n = motion_find(m->p.doc->document, id);
    return n != NULL && m->tree != NULL ? flow_box_for(m->tree, n) : NULL;
}

static const flow_style_t *moving_style(const Moving *m, const char *id)
{
    const flow_box_t *b = moving_box(m, id);
    return b != NULL ? b->style : NULL;
}

// Its transform as getComputedStyle answers it: the matrix about 0, 0 of
// its 100 by 40 box, its translation in CSS pixels and its perspective
// row per pixel.
static bool moving_matrix(const Moving *m, const char *id, double out[16])
{
    const flow_style_t *s = moving_style(m, id);
    if (s == NULL)
        return false;
    flow_style_t z = *s;
    z.transform_origin[0] = z.transform_origin[1] = (flow_length_t){FLOW_LENGTH_PX, 0, 0};
    z.transform_origin_z = 0;
    f_transform_matrix(&z, 0, 0, 100 * 64, 40 * 64, out);
    for (int i = 12; i < 15; i++)
        out[i] /= 64;
    out[3] *= 64, out[7] *= 64, out[11] *= 64;
    return true;
}

// Chrome's matrix(a, b, c, d, e, f), to its six digits; a translation to
// half of 1/64 px besides.
static void expect_matrix(const char *name, const Moving *m, const char *id, const double w[6])
{
    double g[16];
    bool ok = moving_matrix(m, id, g);
    const double got[6] = {g[0], g[1], g[4], g[5], g[12], g[13]};
    for (int i = 0; ok && i < 6; i++)
        ok = fabs(got[i] - w[i]) <= (i < 4 ? 6e-6 : 1.0 / 128 + 1e-4) * fmax(1, fabs(w[i]));
    char detail[256];
    snprintf(detail, sizeof(detail), "got %g %g %g %g %g %g", got[0], got[1], got[2], got[3],
             got[4], got[5]);
    expect(name, ok, ok ? NULL : detail);
}

// Chrome's matrix3d(…), the same way.
static void expect_matrix3d(const char *name, const Moving *m, const char *id,
                            const double w[16])
{
    double g[16];
    bool ok = moving_matrix(m, id, g);
    for (int i = 0; ok && i < 16; i++)
        ok = fabs(g[i] - w[i]) <= (i >= 12 && i < 15 ? 1.0 / 128 + 1e-3 : 6e-6) * fmax(1, fabs(w[i]));
    char detail[512];
    snprintf(detail, sizeof(detail), "got %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g",
             g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12],
             g[13], g[14], g[15]);
    expect(name, ok, ok ? NULL : detail);
}

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    char detail[64];
    snprintf(detail, sizeof(detail), "got %#x, want %#x", got, want);
    expect(name, got == want, got == want ? NULL : detail);
}

static void expect_i64(const char *name, int64_t got, int64_t want)
{
    char detail[64];
    snprintf(detail, sizeof(detail), "got %lld, want %lld", (long long)got, (long long)want);
    expect(name, got == want, got == want ? NULL : detail);
}

// ── Easing 1 § 2 ────────────────────────────────────────────────────────

// A cubic Bézier's exact y at `x`, by halving t in long double.
static double reference_cubic(const double p[4], double x)
{
    long double lo = 0, hi = 1, t = 0.5;
    for (int i = 0; i < 200; i++) {
        t = (lo + hi) / 2;
        long double c = 3 * (long double)p[0], b = 3 * ((long double)p[2] - p[0]) - c,
                    a = 1 - c - b, xt = ((a * t + b) * t + c) * t;
        if (xt < x)
            lo = t;
        else
            hi = t;
    }
    long double c = 3 * (long double)p[1], b = 3 * ((long double)p[3] - p[1]) - c, a = 1 - c - b;
    return (double)(((a * t + b) * t + c) * t);
}

static void motion_easing_cases(void)
{
    static const flow_easing_t kEase = {FLOW_EASE_CUBIC, {0.25, 0.1, 0.25, 1}, 0, 0, true};
    static const flow_easing_t kOver = {FLOW_EASE_CUBIC, {0.2, 0.86, 0.32, 1.22}, 0, 0, false};
    // On [0, 1], y at the parameter whose x is the input, found to the
    // last bit: within the curve's slope times a hair of the exact y.
    for (int i = 0; i <= 100; i++) {
        double x = i / 100.0;
        const flow_easing_t *e[2] = {&kEase, &kOver};
        for (int k = 0; k < 2; k++) {
            double got = f_ease(e[k], x, false), want = reference_cubic(e[k]->p, x);
            if (fabs(got - want) > 1e-6) {
                char d[96];
                snprintf(d, sizeof(d), "x %g got %.9f want %.9f", x, got, want);
                expect("easing: a cubic is its curve", false, d);
                return;
            }
        }
    }
    expect("easing: a cubic is its curve", true, NULL);
    // The curve's ends are its ends, however far its controls reach: 0 at
    // 0 and 1 at 1, exactly.
    static const double kFar[][4] = {
        {0, 0.1, 1, 1e6}, {0.5, 0.7, 0.5, -1e6}, {0, 123456.789, 1, -987654.321}, {0.3, 2, 0.7, -1},
    };
    bool ends = true;
    for (size_t i = 0; i < sizeof(kFar) / sizeof(kFar[0]) && ends; i++) {
        flow_easing_t e = {FLOW_EASE_CUBIC, {kFar[i][0], kFar[i][1], kFar[i][2], kFar[i][3]}, 0, 0, false};
        ends = f_ease(&e, 0, false) == 0 && f_ease(&e, 1, false) == 1;
    }
    expect("easing: a cubic's ends are 0 and 1, whatever its controls", ends, NULL);
    // A curve flat in x near an end is solved, not stopped at the first t
    // whose x is near: cubic-bezier(0, 100, 0, 100) at 9.26e-8, about the
    // first 16 ms of two days, is y 1.35, where t = x would say 0.000028.
    flow_easing_t flat = {FLOW_EASE_CUBIC, {0, 100, 0, 100}, 0, 0, false};
    double fy = f_ease(&flat, 9.26e-8, false), fw = reference_cubic(flat.p, 9.26e-8);
    char fd[64];
    snprintf(fd, sizeof(fd), "got %.6g want %.6g", fy, fw);
    expect("easing: a curve flat in x near its start is solved", fabs(fy - fw) < 1e-6 && fy > 1, fd);
    // Outside [0, 1], the tangent at the nearer end (§ 2.2.1): through
    // P0 and P1 below, P2 and P3 above.
    expect("easing: below 0, the line through P0 and P1",
           fabs(f_ease(&kOver, -0.5, false) - 0.86 / 0.2 * -0.5) < 1e-12, NULL);
    expect("easing: above 1, the line through P2 and P3",
           fabs(f_ease(&kOver, 1.5, false) - (1 + (1.22 - 1) / (0.32 - 1) * 0.5)) < 1e-12, NULL);
    // With x1 at 0, below 0 the line through P0 and P2; with both at 0,
    // and above 1 with both at 1, no line: the end's own value.
    static const flow_easing_t kX2 = {FLOW_EASE_CUBIC, {0, 0, 0.5, 0.25}, 0, 0, false};
    static const flow_easing_t kLow = {FLOW_EASE_CUBIC, {0, 0.3, 0, 0.6}, 0, 0, false};
    static const flow_easing_t kHigh = {FLOW_EASE_CUBIC, {1, 0.3, 1, 0.6}, 0, 0, false};
    expect("easing: below 0 with x1 at 0, the line through P0 and P2",
           f_ease(&kX2, -1, false) == -0.5, NULL);
    expect("easing: with both x at an end, past it the end's value",
           f_ease(&kLow, -1, false) == 0 && f_ease(&kHigh, 2, false) == 1, NULL);
    // Steps (§ 2.3.1): the examples' table.
    static const flow_easing_t kEnd = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_END, false};
    static const flow_easing_t kStart = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_START, false};
    static const flow_easing_t kNone = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_NONE, false};
    static const flow_easing_t kBoth = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_BOTH, false};
    expect("easing: jump-end steps at each quarter, from 0",
           f_ease(&kEnd, 0, false) == 0 && f_ease(&kEnd, 0.24, false) == 0 &&
           f_ease(&kEnd, 0.25, false) == 0.25 && f_ease(&kEnd, 0.99, false) == 0.75 &&
           f_ease(&kEnd, 1, false) == 1, NULL);
    expect("easing: jump-start steps first, and holds at 1",
           f_ease(&kStart, 0, false) == 0.25 && f_ease(&kStart, 0.5, false) == 0.75 &&
           f_ease(&kStart, 1, false) == 1, NULL);
    expect("easing: jump-none takes no step at either end",
           f_ease(&kNone, 0, false) == 0 && f_ease(&kNone, 0.25, false) == 1.0 / 3 &&
           f_ease(&kNone, 1, false) == 1, NULL);
    expect("easing: jump-both steps at both ends",
           f_ease(&kBoth, 0, false) == 0.2 && f_ease(&kBoth, 0.99, false) == 0.8 &&
           f_ease(&kBoth, 1, false) == 1, NULL);
    // The before flag holds the bottom of a step at its own edge.
    expect("easing: the before flag holds the bottom of the step",
           f_ease(&kStart, 0, true) == 0 && f_ease(&kEnd, 0.5, true) == 0.25 &&
           f_ease(&kEnd, 0.5, false) == 0.5, NULL);
}

// ── Chrome's answers (PILE3.md § Animations, What Chrome draws) ─────────

static void motion_chrome_cases(void)
{
    Moving m;
    moving_open(&m, MOTION_STYLE
        "@keyframes spin{from{transform:rotate(0deg)}to{transform:rotate(720deg)}}"
        "@keyframes mix{from{transform:translateX(10px) rotate(30deg)}"
        "to{transform:rotate(60deg) translateX(100px)}}"
        "@keyframes flip{from{transform:scaleX(-1)}to{transform:rotate(90deg)}}"
        "@keyframes pct{from{transform:translate(50%)}to{transform:translate(20px)}}"
        "@keyframes over{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "@keyframes k1{from{transform:skewX(0deg)}to{transform:skewY(40deg)}}"
        "@keyframes k2{from{transform:skewX(20deg)}to{transform:skew(40deg,20deg)}}"
        "@keyframes k3{from{transform:rotate(0deg)}to{transform:rotateZ(400deg)}}"
        "@keyframes k6{from{transform:none}to{transform:rotate(400deg) scale(2)}}"
        "@keyframes k7{from{transform:scale(1)}to{transform:scale(1) rotate(400deg)}}"
        "@keyframes k8{from{transform:matrix(1,0,0,1,0,0)}to{transform:rotate(400deg)}}"
        "@keyframes z4{from{transform:rotate(30deg) skewX(10deg)}to{transform:scale(2)}}"
        "@keyframes z5{from{transform:matrix(2,0.5,0.3,1,10,20)}to{transform:matrix(1,0,0,-1,0,0)}}"
        "@keyframes z6{from{transform:rotate(0deg)}to{transform:rotate(1turn)}}"
        "@keyframes z7{from{transform:translate(10%,2px)}to{transform:translateY(30px)}}"
        "@keyframes y4{from{transform:rotate(0)}to{transform:rotateY(60deg)}}"
        "@keyframes y5{from{transform:translateX(10px)}to{transform:translateZ(10px)}}"
        "@keyframes z1{from{transform:scale3d(.475,.475,.475) translate3d(42px,0,0)}"
        "to{transform:scale(.1) translate3d(2000px,0,0)}}"
        "@keyframes z2{from{transform:perspective(400px)}to{transform:rotateY(90deg)}}"
        "@keyframes z3{from{transform:rotateX(30deg)}to{transform:rotateY(60deg)}}"
        "@keyframes sh{from{transform:matrix(-0.984808,0.173648,-0.173648,-0.984808,0,0)}"
        "to{transform:matrix(-0.984808,-0.173648,0.173648,-0.984808,0,0)}}"
        "</style>"
        "<div id=spin style='animation-name:spin;" AT("0.25") "'></div>"
        "<div id=mix style='animation-name:mix;" AT("0.5") "'></div>"
        "<div id=flip style='animation-name:flip;" AT("0.5") "'></div>"
        "<div id=pct style='animation-name:pct;" AT("0.5") "'></div>"
        "<div id=over style='animation-name:over;" AT("0.5")
            ";animation-timing-function:cubic-bezier(0.2,0.86,0.32,1.22)'></div>"
        "<div id=k1 style='animation-name:k1;" AT("0.5") "'></div>"
        "<div id=k2 style='animation-name:k2;" AT("0.5") "'></div>"
        "<div id=k3 style='animation-name:k3;" AT("0.5") "'></div>"
        "<div id=k6 style='animation-name:k6;" AT("0.5") "'></div>"
        "<div id=k7 style='animation-name:k7;" AT("0.5") "'></div>"
        "<div id=k8 style='animation-name:k8;" AT("0.5") "'></div>"
        "<div id=z4 style='animation-name:z4;" AT("0.5") "'></div>"
        "<div id=z5 style='animation-name:z5;" AT("0.5") "'></div>"
        "<div id=z6 style='animation-name:z6;" AT("0.3") "'></div>"
        "<div id=z7 style='animation-name:z7;" AT("0.5") "'></div>"
        "<div id=y4 style='animation-name:y4;" AT("0.5") "'></div>"
        "<div id=y5 style='animation-name:y5;" AT("0.5") "'></div>"
        "<div id=z1 style='animation-name:z1;" AT("0.5") "'></div>"
        "<div id=z2 style='animation-name:z2;" AT("0.5") "'></div>"
        "<div id=z3 style='animation-name:z3;" AT("0.5") "'></div>"
        "<div id=sh style='animation-name:sh;" AT("0.5") "'></div>", 1000);
    expect("motion: the page lays out", m.tree != NULL, NULL);
    // The angle is interpolated, not the matrix: 720deg at a quarter is
    // half a turn, exactly.
    expect_matrix("motion: rotate(0deg) to rotate(720deg) at 25% is 180deg", &m, "spin",
                  (const double[6]){-1, 0, 0, -1, 0, 0});
    expect_matrix("motion: lists that do not pair are matrices", &m, "mix",
                  (const double[6]){0.707107, 0.707107, -0.707107, 0.707107, 30, 43.3013});
    expect_matrix("motion: scaleX(-1) to rotate(90deg), 2D decomposition", &m, "flip",
                  (const double[6]){0, 0, -0.707107, 0.707107, 0, 0});
    expect_matrix("motion: a percentage and a length mix as calc()", &m, "pct",
                  (const double[6]){1, 0, 0, 1, 35, 0});
    expect_matrix("motion: an overshooting easing passes the keyframe", &m, "over",
                  (const double[6]){1, 0, 0, 1, 103.628, 0});
    expect_matrix("motion: skewX to skewY are matrices", &m, "k1",
                  (const double[6]){1.08319, 0.394248, 0.0461182, 0.956478, 0, 0});
    expect_matrix("motion: skewX to skew are matrices", &m, "k2",
                  (const double[6]){1.01641, 0.17922, 0.709374, 0.964181, 0, 0});
    expect_matrix("motion: rotate and rotateZ pair: 200deg", &m, "k3",
                  (const double[6]){-0.939693, -0.34202, 0.34202, -0.939693, 0, 0});
    expect_matrix("motion: none padded with identities", &m, "k6",
                  (const double[6]){-1.40954, -0.51303, 0.51303, -1.40954, 0, 0});
    expect_matrix("motion: a shorter list padded at its end", &m, "k7",
                  (const double[6]){-0.939693, -0.34202, 0.34202, -0.939693, 0, 0});
    expect_matrix("motion: matrix() against rotate() as matrices: 20deg", &m, "k8",
                  (const double[6]){0.939693, 0.34202, -0.34202, 0.939693, 0, 0});
    expect_matrix("motion: a skewed turn's tail, 2D decomposition", &m, "z4",
                  (const double[6]){1.44889, 0.388229, -0.260489, 1.48312, 0, 0});
    expect_matrix("motion: turns 340 degrees apart meet the shorter way, through 180", &m, "sh",
                  (const double[6]){-1, 0, 0, -1, 0, 0});
    expect_matrix("motion: matrix() with matrix() as matrices", &m, "z5",
                  (const double[6]){1.51931, 0.187035, -0.00887065, -0.0527885, 5, 10});
    expect_matrix("motion: degrees with turns, mixed in degrees", &m, "z6",
                  (const double[6]){-0.309017, 0.951057, -0.951057, -0.309017, 0, 0});
    expect_matrix("motion: translate with translateY", &m, "z7",
                  (const double[6]){1, 0, 0, 1, 5, 16});
    expect_matrix3d("motion: rotate with rotateY pairs about y", &m, "y4",
                    (const double[16]){0.866025, 0, -0.5, 0, 0, 1, 0, 0, 0.5, 0, 0.866025, 0,
                                       0, 0, 0, 1});
    expect_matrix3d("motion: translateX with translateZ, as translate3d", &m, "y5",
                    (const double[16]){1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 0, 5, 1});
    expect_matrix3d("motion: scale3d with scale pairs, as scale3d", &m, "z1",
                    (const double[16]){0.2875, 0, 0, 0, 0, 0.2875, 0, 0, 0, 0, 0.7375, 0,
                                       293.537, 0, 0, 1});
    expect_matrix3d("motion: perspective against a turn, 3D decomposition", &m, "z2",
                    (const double[16]){0.707107, 0, -0.707107, 0.000883883, 0, 1, 0, 0,
                                       0.707107, 0, 0.707107, -0.000883883, 0, 0, 0, 1});
    expect_matrix3d("motion: two axes, slerped", &m, "z3",
                    (const double[16]){0.863873, 0.0704647, -0.498757, 0, 0.0704647, 0.963525,
                                       0.258176, 0, 0.498757, -0.258176, 0.827398, 0, 0, 0, 0,
                                       1});
    moving_close(&m);

    moving_open(&m, MOTION_STYLE
        "@keyframes fade{0%{opacity:0}50%{opacity:1;animation-timing-function:steps(2,start)}"
        "100%{opacity:0.2}}"
        "@keyframes col{from{background-color:rgba(255,0,0,0.2)}to{background-color:#0000ff}}"
        "@keyframes sh{from{box-shadow:0 0 0 red}"
        "to{box-shadow:10px 10px 4px blue,inset 0 0 2px green}}"
        "@keyframes vis{from{visibility:hidden}to{visibility:visible}}"
        "@keyframes zi{from{z-index:1}to{z-index:4}}"
        "@keyframes bp{from{background-position:0% 0%}to{background-position:100px 50%}}"
        "@keyframes wd{from{width:auto}to{width:200px}}"
        "@keyframes fw{from{font-weight:100}to{font-weight:900}}"
        "@keyframes cc{from{color:currentcolor}to{color:red}}"
        "@keyframes bc{from{background-color:currentcolor}to{background-color:red}}"
        "@keyframes gr{from{background-image:linear-gradient(red,blue)}"
        "to{background-image:linear-gradient(blue,red)}}"
        "</style><section style='color:blue'>"
        "<div id=f75 style='animation-name:fade;" AT("0.75") "'></div>"
        "<div id=f25 style='animation-name:fade;" AT("0.25") "'></div>"
        "<div id=col style='animation-name:col;" AT("0.5") "'></div>"
        "<div id=sh style='animation-name:sh;" AT("0.5") "'></div>"
        "<div id=vis style='animation-name:vis;" AT("0.1") "'></div>"
        "<div id=zi style='position:relative;animation-name:zi;" AT("0.5") "'></div>"
        "<div id=bp style='animation-name:bp;" AT("0.5") "'></div>"
        "<div id=wd style='animation-name:wd;" AT("0.49") "'></div>"
        "<div id=fw style='animation-name:fw;" AT("0.3") "'></div>"
        "<div id=cc style='animation-name:cc;" AT("0.5") "'></div>"
        "<div id=bc style='color:lime;animation-name:bc;" AT("0.5") "'></div>"
        "<div id=gr style='animation-name:gr;" AT("0.6") "'></div>"
        "</section>", 1000);
    const flow_style_t *s;
    expect_i64("motion: a keyframe's own easing takes its interval",
               (s = moving_style(&m, "f75")) != NULL ? s->opacity : -1, 200);
    expect_i64("motion: the element's easing before it",
               (s = moving_style(&m, "f25")) != NULL ? s->opacity : -1, 500);
    expect_u32("motion: colours mix premultiplied",
               (s = moving_style(&m, "col")) != NULL ? s->background : 0, 0x662B00D5u);
    s = moving_style(&m, "sh");
    bool shadow_ok = s != NULL && s->nbox_shadows == 2 && s->box_shadows[0].x == 5 &&
                     s->box_shadows[0].y == 5 && s->box_shadows[0].blur == 2 &&
                     !s->box_shadows[0].inset && s->box_shadows[0].colour == 0x800080u &&
                     s->box_shadows[1].inset && s->box_shadows[1].blur == 1 &&
                     s->box_shadows[1].colour == 0x7F008000u;
    expect("motion: a shorter shadow list padded with transparent ones", shadow_ok, NULL);
    expect("motion: visible all the way between",
           (s = moving_style(&m, "vis")) != NULL && s->visibility == FLOW_VISIBLE, NULL);
    expect_i64("motion: an integer rounds half up",
               (s = moving_style(&m, "zi")) != NULL ? s->z_index : -1, 3);
    s = moving_style(&m, "bp");
    expect("motion: background-position mixes into calc(0% + 50px) 25%",
           s != NULL && s->backgrounds.nx == 1 &&
           s->backgrounds.x[0].kind == FLOW_LENGTH_PERCENT && s->backgrounds.x[0].value == 0 &&
           s->backgrounds.x[0].offset == 50 * 64 && s->backgrounds.ny == 1 &&
           s->backgrounds.y[0].kind == FLOW_LENGTH_PERCENT &&
           s->backgrounds.y[0].value == 25 * 64, NULL);
    expect("motion: auto against a length is discrete",
           (s = moving_style(&m, "wd")) != NULL && s->width.kind == FLOW_LENGTH_AUTO, NULL);
    expect_i64("motion: font-weight is a number",
               (s = moving_style(&m, "fw")) != NULL ? s->font_weight : -1, 340);
    expect_u32("motion: currentColor in color is the parent's",
               (s = moving_style(&m, "cc")) != NULL ? s->color : 0, 0x800080u);
    expect_u32("motion: currentColor in a background is the element's",
               (s = moving_style(&m, "bc")) != NULL ? s->background : 0, 0x808000u);
    s = moving_style(&m, "gr");
    expect("motion: a gradient is discrete: the second, past one half",
           s != NULL && s->backgrounds.image[0].gradient != NULL &&
           s->backgrounds.image[0].gradient->stops[0].colour == 0x0000ffu, NULL);
    moving_close(&m);

    // A keyframe's url() is read against the sheet its @keyframes rule was
    // written in, as a winner's is (Chrome, 10/14: `url(spinner.png)` in
    // /css/site.css's keyframes is /css/spinner.png): the second sheet
    // here, not the first, nor the document.
    moving_open(&m, MOTION_STYLE "</style><style>"
        "@keyframes bg{to{background-image:url(spinner.png)}}"
        "#bg{animation:bg 1s linear -.75s paused}"
        "</style><div id=bg></div>", 1000);
    s = moving_style(&m, "bg");
    expect("motion: a keyframe's url() is its own sheet's",
           s != NULL && s->backgrounds.nimage == 1 && s->backgrounds.image[0].url != NULL &&
               s->backgrounds.image[0].sheet == 1, NULL);
    moving_close(&m);

    // A turn too small for slerp's division between two matrices that
    // scale a million times: the mix ends at the second matrix — the
    // turn's 1e-6 radians a whole pixel at that scale — and between them
    // the turn is mixed, not held at the first.
    double ma[16], mb[16], mo[16];
    memset(ma, 0, sizeof(ma));
    memset(mb, 0, sizeof(mb));
    ma[0] = ma[5] = 1e6;
    ma[10] = ma[15] = mb[10] = mb[15] = 1;
    mb[0] = mb[5] = 1e6 * cos(1e-6);
    mb[1] = 1e6 * sin(1e-6);
    mb[4] = -mb[1];
    bool ends = f_matrix_mix(ma, mb, 1, mo);
    for (int i = 0; i < 16 && ends; i++)
        ends = fabs(mo[i] - mb[i]) <= 1e-9 * fmax(1, fabs(mb[i]));
    bool midway = f_matrix_mix(ma, mb, 0.5, mo) && fabs(mo[1] - mb[1] / 2) < 1e-6;
    expect("motion: two matrices a hair's turn apart end at the second", ends, NULL);
    expect("motion: and between them the turn is mixed", midway, NULL);

    // The clock: a step's before flag in a backwards delay; 2.5 iterations
    // filling forwards; alternate's second iteration, backwards; a zero
    // duration filling both.
    moving_open(&m, MOTION_STYLE
        "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "</style>"
        "<div id=d4 style='animation:mv 1s steps(4,start) 1s backwards paused'></div>"
        "<div id=d5 style='animation:mv 1s linear 2.5 forwards paused;animation-delay:-10s'></div>"
        "<div id=d6 style='animation:mv 1s linear 3 alternate paused;animation-delay:-1.25s'></div>"
        "<div id=d8 style='animation:mv 0s linear both'></div>", 1000);
    expect_matrix("motion: in a backwards delay, the bottom step", &m, "d4",
                  (const double[6]){1, 0, 0, 1, 0, 0});
    expect_matrix("motion: 2.5 iterations fill at one half", &m, "d5",
                  (const double[6]){1, 0, 0, 1, 50, 0});
    expect_matrix("motion: alternate's second iteration runs backwards", &m, "d6",
                  (const double[6]){1, 0, 0, 1, 75, 0});
    expect_matrix("motion: a zero duration filling both shows its end", &m, "d8",
                  (const double[6]){1, 0, 0, 1, 100, 0});
    moving_close(&m);
}

// ── Keyframes, the cascade, inheritance ─────────────────────────────────

// A keyframe is resolved where it is mixed, against what the animations
// make (PILE3_SPARSE_KEYFRAMES.md): its `em` of the font size every
// animation has made, not of the element's own or the keyframe's; its
// currentColor of the colour they made; the value underneath an implicit
// keyframe a computed value, never resolved again. Chrome 155, 10/15,
// each paused halfway (the probes' rows p1-p9 and q1-q8).
#define HALF(name) name " 1s linear -.5s paused"
#define AT_Q(name, d) name " 1s linear -" d "s paused"

static void motion_sparse_cases(void)
{
    Moving m;
    moving_open(&m, MOTION_STYLE
        "@keyframes fs{to{font-size:30px}}"
        "@keyframes w10em{from{width:10em}to{width:10em}}"
        "@keyframes both1{from{font-size:10px;width:10em}to{font-size:30px;width:100px}}"
        "@keyframes wto100{to{width:100px}}"
        "@keyframes cfs{from{font-size:2em}to{font-size:2em}}"
        "@keyframes col{to{color:rgb(200,0,0)}}"
        "@keyframes bcur{from{background-color:currentColor}to{background-color:currentColor}}"
        "@keyframes pw{to{width:300px}}"
        "@keyframes winh{from{width:inherit}to{width:inherit}}"
        "@keyframes lh{from{line-height:2em}to{line-height:2em}}"
        "@keyframes bw{from{border-left-width:1em}to{border-left-width:1em}}"
        "@keyframes tr{from{transform:translateX(1em)}to{transform:translateX(1em)}}"
        "@keyframes sh{from{box-shadow:1em 0 0 red}to{box-shadow:1em 0 0 red}}"
        "@keyframes cw{from{width:calc(1em + 10px)}to{width:calc(1em + 10px)}}"
        "@keyframes bstyle{from{border-left-style:none}to{border-left-style:solid}}"
        "@keyframes bw5{from{border-left-width:5px}to{border-left-width:5px}}"
        "@keyframes bmix{from{background-color:currentColor}to{background-color:rgb(0,0,200)}}"
        "@keyframes bto{to{background-color:rgb(0,0,200)}}"
        "</style>"
        "<div id=p1 style='font-size:10px;animation:" HALF("fs") "," HALF("w10em") "'></div>"
        "<div id=p2 style='font-size:10px;animation:" HALF("both1") "'></div>"
        "<div id=p3 style='font-size:10px;width:10em;animation:" HALF("fs") "," HALF("wto100") "'></div>"
        "<div style='font-size:10px;animation:" HALF("fs") "'><div id=p4 style='animation:" HALF("w10em")
            "'></div><div id=p5 style='animation:" HALF("cfs") "'></div></div>"
        "<div id=p6 style='color:rgb(0,0,0);animation:" HALF("col") "," HALF("bcur") "'></div>"
        "<div style='width:100px;animation:" HALF("pw") "'><div id=p7 style='animation:" HALF("winh")
            "'></div></div>"
        "<div id=p8 style='font-size:10px;animation:" HALF("fs") "," HALF("lh") "'></div>"
        "<div id=p9 style='font-size:10px;animation:" HALF("fs") "," HALF("bw") "'></div>"
        "<div id=p9b style='font-size:10px;border-left-style:solid;animation:" HALF("fs") "," HALF("bw")
            "'></div>"
        "<div id=q1 style='font-size:10px;animation:" HALF("fs") "," HALF("tr") "'></div>"
        "<div id=q2 style='font-size:10px;animation:" HALF("fs") "," HALF("sh") "'></div>"
        "<div id=q7 style='font-size:10px;animation:" HALF("fs") "," HALF("cw") "'></div>"
        "<div id=q8 style='animation:" AT_Q("bstyle", ".75") "," AT_Q("bw5", ".75") "'></div>"
        "<div id=q8b style='animation:" AT_Q("bstyle", ".25") "," AT_Q("bw5", ".25") "'></div>"
        "<div id=r1 style='color:rgb(0,0,0);animation:" HALF("col") "," HALF("bmix") "'></div>"
        "<div id=r2 style='color:rgb(0,0,0);background-color:currentColor;animation:" HALF("col") ","
            HALF("bto") "'></div>", 1000);
    const flow_style_t *s;
    s = moving_style(&m, "p1");
    expect_i64("motion: a keyframe's em is of the font the animations make",
               s != NULL && s->font_size == 20 * 64 && s->width.kind == FLOW_LENGTH_PX ? s->width.value : -1,
               200 * 64);
    s = moving_style(&m, "p2");
    expect_i64("motion: and not of the keyframe's own font size",
               s != NULL && s->width.kind == FLOW_LENGTH_PX ? s->width.value : -1, 150 * 64);
    s = moving_style(&m, "p3");
    expect_i64("motion: the value under an implicit keyframe is computed, never resolved again",
               s != NULL && s->width.kind == FLOW_LENGTH_PX ? s->width.value : -1, 100 * 64);
    s = moving_style(&m, "p4");
    expect_i64("motion: a child's em is of its parent's animated font",
               s != NULL && s->width.kind == FLOW_LENGTH_PX ? s->width.value : -1, 200 * 64);
    s = moving_style(&m, "p5");
    expect_i64("motion: and so is a child's font size in em", s != NULL ? s->font_size : -1, 40 * 64);
    s = moving_style(&m, "p6");
    expect_u32("motion: a keyframe's currentColor is the colour the animations make",
               s != NULL && s->color == 0x640000u ? s->background : 0, 0x640000u);
    s = moving_style(&m, "p7");
    expect_i64("motion: a keyframe's inherit takes the parent's animated value",
               s != NULL && s->width.kind == FLOW_LENGTH_PX ? s->width.value : -1, 200 * 64);
    s = moving_style(&m, "p8");
    expect_i64("motion: a line height in em is of the font the animations make",
               s != NULL && s->line_height.kind == FLOW_LINE_PX ? s->line_height.value : -1, 40 * 64);
    s = moving_style(&m, "p9");
    expect_i64("motion: a border width in em, its side drawn as none, is 0",
               s != NULL ? s->border_width[FLOW_LEFT] : -1, 0);
    s = moving_style(&m, "p9b");
    expect_i64("motion: and drawn, of the font the animations make",
               s != NULL ? s->border_width[FLOW_LEFT] : -1, 20 * 64);
    s = moving_style(&m, "q1");
    expect_i64("motion: a translation in em is of the font the animations make",
               s != NULL && s->ntransform == 1 && s->transform[0].x.kind == FLOW_LENGTH_PX
                   ? s->transform[0].x.value : -1, 20 * 64);
    s = moving_style(&m, "q2");
    expect_i64("motion: so is a shadow's offset",
               s != NULL && s->nbox_shadows == 1 ? s->box_shadows[0].x : -1, 20);
    s = moving_style(&m, "q7");
    expect_i64("motion: and an em inside calc()",
               s != NULL && s->width.kind == FLOW_LENGTH_PX ? s->width.value : -1, 30 * 64);
    s = moving_style(&m, "q8");
    expect_i64("motion: a side's style an animation draws gives it its own width",
               s != NULL && s->border_style[FLOW_LEFT] == FLOW_BORDER_SOLID ? s->border_width[FLOW_LEFT] : -1,
               5 * 64);
    s = moving_style(&m, "q8b");
    expect_i64("motion: and one drawn as none, none",
               s != NULL && s->border_style[FLOW_LEFT] == FLOW_BORDER_NONE ? s->border_width[FLOW_LEFT] : -1,
               0);
    s = moving_style(&m, "r1");
    expect_u32("motion: a keyframe's currentColor mixed with a colour is the colour the animations make",
               s != NULL ? s->background : 0, 0x320064u);
    s = moving_style(&m, "r2");
    expect_u32("motion: and so is the value underneath in currentColor",
               s != NULL ? s->background : 0, 0x320064u);
    moving_close(&m);
}

static void motion_cascade_cases(void)
{
    Moving m;
    // The implicit keyframes are the element's computed value; an
    // !important winner is not overridden; an animated colour is
    // inherited, and what is in currentColor follows it.
    moving_open(&m, MOTION_STYLE
        "@keyframes in{from{opacity:0}}"
        "@keyframes f{from{opacity:0}to{opacity:1}}"
        "@keyframes c{from{color:#000000}to{color:#ff0000}}"
        "#imp{opacity:0.3 !important}"
        "</style>"
        "<div id=in style='opacity:0.8;animation-name:in;" AT("0.5") "'></div>"
        "<div id=imp style='animation-name:f;" AT("0.5") "'></div>"
        "<div id=par style='border:1px solid;animation-name:c;" AT("0.5") "'><p id=kid>x</p></div>",
        1000);
    const flow_style_t *s;
    expect_i64("motion: an implicit keyframe is the element's value",
               (s = moving_style(&m, "in")) != NULL ? s->opacity : -1, 400);
    expect_i64("motion: an !important winner stands",
               (s = moving_style(&m, "imp")) != NULL ? s->opacity : -1, 300);
    s = moving_style(&m, "par");
    expect_u32("motion: an animated colour", s != NULL ? s->color : 0, 0x800000u);
    expect_u32("motion: a border in currentColor follows it",
               s != NULL ? s->border_color[0] : 0, 0x800000u);
    expect_u32("motion: and a child inherits it",
               (s = moving_style(&m, "kid")) != NULL ? s->color : 0, 0x800000u);
    moving_close(&m);

    // Of two animations of one property the later in animation-name wins;
    // one whose @keyframes do not exist starts nothing.
    moving_open(&m, MOTION_STYLE
        "@keyframes a{from{opacity:0}to{opacity:0.2}}"
        "@keyframes b{from{opacity:1}to{opacity:0.6}}"
        "</style>"
        "<div id=two style='animation:a 1s linear paused,b 1s linear paused;"
        "animation-delay:-0.5s'></div>"
        "<div id=none style='opacity:0.7;animation:missing 1s paused'></div>", 1000);
    expect_i64("motion: the later animation wins",
               (s = moving_style(&m, "two")) != NULL ? s->opacity : -1, 800);
    expect_i64("motion: no rule, no animation",
               (s = moving_style(&m, "none")) != NULL ? s->opacity : -1, 700);
    expect("motion: no rule, no side effects",
           s != NULL && s->animating == 0, NULL);
    moving_close(&m);

    // Chrome's answers (10/13): a rule with only `to` starts from the
    // element's own value; a keyframe's em is the element's font-size when
    // an !important winner holds it; filling backwards, an animation in its
    // delay shows its first keyframe, and not filling, the element's value.
    moving_open(&m, MOTION_STYLE
        "@keyframes to{to{margin-left:200px}}"
        "@keyframes big{from{font-size:40px;margin-left:2em}to{font-size:40px;margin-left:2em}}"
        "@keyframes slide{from{margin-left:0}to{margin-left:200px}}"
        "#fs{font-size:10px !important}"
        "</style>"
        "<div id=to style='margin-left:100px;animation-name:to;" AT("0.5") "'></div>"
        "<div id=fs style='animation-name:big;" AT("0.5") "'>x</div>"
        "<div id=bw style='margin-left:50px;animation:slide 1s 600s backwards'></div>"
        "<div id=nf style='margin-left:50px;animation:slide 1s 600s'></div>", 1000);
    const flow_box_t *b;
    expect_i64("motion: only a to, the start is the element's own value",
               (b = moving_box(&m, "to")) != NULL ? b->rect.x : -1, 150);
    expect_i64("motion: a keyframe's em is the font-size an !important winner holds",
               (b = moving_box(&m, "fs")) != NULL ? b->rect.x : -1, 20);
    expect_i64("motion: in its delay, filling backwards, its first keyframe",
               (b = moving_box(&m, "bw")) != NULL ? b->rect.x : -1, 0);
    expect_i64("motion: in its delay, not filling, the element's value",
               (b = moving_box(&m, "nf")) != NULL ? b->rect.x : -1, 50);
    moving_close(&m);

    // Chrome's answers (10/13): an element that inherits its animation
    // reads a rule's var() against its own custom properties, whether it
    // inherits the shorthand or the name alone; z-index from auto between
    // two numbers is a number, 2.5 rounded up.
    moving_open(&m, MOTION_STYLE
        "@keyframes v{to{margin-left:var(--x)}}"
        "@keyframes z{from{z-index:1}to{z-index:4}}"
        "#par{--x:100px;animation:v 1s linear -.5s paused}"
        "#kid{--x:200px;animation:inherit}"
        "#kn{--x:300px;animation-name:inherit;" AT("0.5") "}"
        "</style><div id=par><p id=kid>a</p><p id=kn>b</p></div>"
        "<div id=z style='animation-name:z;" AT("0.5") "'></div>", 1000);
    expect_i64("motion: the animating element's own var()",
               (s = moving_style(&m, "par")) != NULL ? s->margin[3].value : -1, 50 * 64);
    expect_i64("motion: an inherited animation reads its rule's var() against its own",
               (s = moving_style(&m, "kid")) != NULL ? s->margin[3].value : -1, 100 * 64);
    expect_i64("motion: and an inherited name alone",
               (s = moving_style(&m, "kn")) != NULL ? s->margin[3].value : -1, 150 * 64);
    s = moving_style(&m, "z");
    expect("motion: z-index from auto between two numbers is a number, 2.5 rounded up",
           s != NULL && s->has_z_index && s->z_index == 3, NULL);
    moving_close(&m);

    // An easing that overshoots carries an integer past its type's ends:
    // it is held there, never wrapped (a cast past them is undefined, and
    // the harness's sanitizer says so).
    moving_open(&m, MOTION_STYLE
        "@keyframes big{from{z-index:0;order:0}to{z-index:2000000000;order:2000000000}}"
        "@keyframes neg{from{z-index:0;order:0}to{z-index:-2000000000;order:-2000000000}}"
        "</style><div id=up style='animation:big 1s cubic-bezier(0,3,1,3) -.5s paused'></div>"
        "<div id=down style='animation:neg 1s cubic-bezier(0,3,1,3) -.5s paused'></div>", 1000);
    s = moving_style(&m, "up");
    expect("motion: an overshooting integer is held at its type's top",
           s != NULL && s->z_index == INT32_MAX && s->order == INT32_MAX, NULL);
    s = moving_style(&m, "down");
    expect("motion: and its bottom",
           s != NULL && s->z_index == INT32_MIN && s->order == INT32_MIN, NULL);
    moving_close(&m);

    // Chrome's answers (10/13): a length its property keeps at 0 or more
    // is held at 0 when an overshooting easing carries it below — a
    // padding, a gap, a radius, a background's size, a flex basis, a width
    // in percent, a padding in percent.
    moving_open(&m, MOTION_STYLE
        "@keyframes k{from{padding-left:10px;column-gap:10px;border-top-left-radius:10px;"
        "background-size:10px 10px;flex-basis:10px;width:10%}"
        "to{padding-left:0;column-gap:0;border-top-left-radius:0;background-size:0 0;"
        "flex-basis:0;width:0%}}"
        "@keyframes q{from{padding-left:10%}to{padding-left:0%}}"
        "#k{display:flex;background-image:linear-gradient(red,blue);"
        "animation:k 1s cubic-bezier(0,3,1,3) -.5s paused}"
        "#q{animation:q 1s cubic-bezier(0,3,1,3) -.5s paused}"
        "</style><div id=k></div><div id=q></div>", 1000);
    s = moving_style(&m, "k");
    expect("motion: lengths kept at 0 or more are held there through an overshoot",
           s != NULL && s->padding[3].value == 0 && s->column_gap.value == 0 &&
               s->radius[0][0].value == 0 && s->radius[0][1].value == 0 &&
               s->backgrounds.size[0].size[0].value == 0 &&
               s->backgrounds.size[0].size[1].value == 0 && s->flex_basis.value == 0 &&
               s->width.value == 0 && s->width.offset == 0, NULL);
    s = moving_style(&m, "q");
    expect("motion: and a padding in percent",
           s != NULL && s->padding[3].kind == FLOW_LENGTH_PERCENT && s->padding[3].value == 0, NULL);
    moving_close(&m);

    // Chrome's answers (10/13): a side's width and its style are each their
    // own property. Moved by one animation while another turns the style
    // from none to solid, the width is the first's (15px of 0 to 20 at
    // three quarters); while the style is none, 0; and with the style
    // turned solid and the width not animated, the element's own 3px.
    moving_open(&m, MOTION_STYLE
        "div{border-top:3px none black}"
        "@keyframes w{from{border-top-width:0}to{border-top-width:20px}}"
        "@keyframes st{from{border-top-style:none}to{border-top-style:solid}}"
        "#ba{animation:w 1s linear -.75s paused,st 1s linear -.75s paused}"
        "#bb{animation:w 1s linear -.25s paused,st 1s linear -.25s paused}"
        "#bc{animation:st 1s linear -.75s paused}"
        "</style><div id=ba></div><div id=bb></div><div id=bc></div>", 1000);
    s = moving_style(&m, "ba");
    expect("motion: a width moved by one animation, its style by another",
           s != NULL && s->border_style[0] == FLOW_BORDER_SOLID && s->border_width[0] == 15 * 64, NULL);
    s = moving_style(&m, "bb");
    expect("motion: and while the style is none, no width",
           s != NULL && s->border_style[0] == FLOW_BORDER_NONE && s->border_width[0] == 0, NULL);
    s = moving_style(&m, "bc");
    expect("motion: a style turned solid draws the element's own width",
           s != NULL && s->border_style[0] == FLOW_BORDER_SOLID && s->border_width[0] == 3 * 64, NULL);
    moving_close(&m);
    // And a side whose width no origin declared has its initial one,
    // medium, whatever its style (Chrome, 10/14): a style an animation
    // turns solid draws 3px; a width declared, even 0, is the width; a
    // drawn border an animation turns to none has none; a width animated
    // from nowhere starts at medium (6px of 3 to 7 at three quarters); and
    // a keyframe's own 0 is 0.
    moving_open(&m, MOTION_STYLE
        "@keyframes bs{to{border-top-style:solid}}"
        "@keyframes bn{to{border-top-style:none}}"
        "@keyframes bw{to{border-top-style:solid;border-top-width:7px}}"
        "@keyframes bz{to{border-top-style:solid;border-top-width:0}}"
        "#ua{animation:bs 1s linear -.75s paused}"
        "#ub{border-top-width:10px;animation:bs 1s linear -.75s paused}"
        "#uc{border-top-width:0;animation:bs 1s linear -.75s paused}"
        "#ud{border-top:solid;animation:bn 1s linear -.75s paused}"
        "#ue{animation:bw 1s linear -.75s paused}"
        "#uf{animation:bz 1s linear -1s paused forwards}"
        "</style><div id=ua></div><div id=ub></div><div id=uc></div><div id=ud></div><div id=ue></div>"
        "<div id=uf></div>",
        1000);
    static const struct {
        const char *id, *name;
        flow_unit_t width;
    } kWidths[] = {
        {"ua", "motion: a style turned solid with no width declared draws medium", 3 * 64},
        {"ub", "motion: a width declared under a style of none is the width drawn", 10 * 64},
        {"uc", "motion: a width declared 0 stays 0", 0},
        {"ud", "motion: a drawn border turned to none has no width", 0},
        {"ue", "motion: a width animated from none declared starts at medium", 6 * 64},
        {"uf", "motion: a keyframe's own width of 0 is 0", 0},
    };
    for (size_t i = 0; i < sizeof(kWidths) / sizeof(kWidths[0]); i++) {
        s = moving_style(&m, kWidths[i].id);
        char detail[64];
        snprintf(detail, sizeof(detail), "width %d", s != NULL ? s->border_width[0] : -1);
        expect(kWidths[i].name, s != NULL && s->border_width[0] == kWidths[i].width, detail);
    }
    moving_close(&m);

    // Chrome's answers (10/14): what a style's float, position and
    // overflow make of each other is settled once, from what every
    // animation makes together — an inline span floated by one animation
    // and positioned by another is a block; overflow-x hidden on its own
    // turns a visible overflow-y auto. And a colour or a shadow in
    // currentColor at both ends follows the element's colour as another
    // animation moves it, whatever the element's own colour field was.
    moving_open(&m, MOTION_STYLE
        "@keyframes fl{from{float:left}to{float:left}}"
        "@keyframes po{from{position:relative}to{position:relative}}"
        "@keyframes ox{from{overflow-x:hidden}to{overflow-x:hidden}}"
        "@keyframes col{from{color:rgb(0,0,0)}to{color:rgb(200,0,0)}}"
        "@keyframes sh{from{box-shadow:0 0 0 currentColor;border-top-color:currentColor}"
        "to{box-shadow:10px 0 0 currentColor;border-top-color:currentColor}}"
        "#sp{animation:fl 1s linear -.5s paused,po 1s linear -.5s paused}"
        "#ov{animation:ox 1s linear -.5s paused}"
        "#cc{border-top:2px solid blue;animation:col 1s linear -.5s paused,sh 1s linear -.5s paused}"
        "</style><p><span id=sp>x</span></p><div id=ov></div><div id=cc></div>", 1000);
    s = moving_style(&m, "sp");
    expect("motion: floated by one animation and positioned by another, a block",
           s != NULL && s->display == FLOW_DISPLAY_BLOCK && s->float_side == FLOW_FLOAT_LEFT &&
               s->position == FLOW_POSITION_RELATIVE && s->specified_inline, NULL);
    s = moving_style(&m, "ov");
    expect("motion: an overflow axis animated alone, its pair settled",
           s != NULL && s->overflow_x == FLOW_OVERFLOW_HIDDEN && s->overflow_y == FLOW_OVERFLOW_AUTO, NULL);
    s = moving_style(&m, "cc");
    expect("motion: currentColor at both ends follows the moving colour",
           s != NULL && s->color == 0x640000u && s->nbox_shadows == 1 && s->box_shadows[0].current &&
               s->box_shadows[0].x > 0 && s->border_color[0] == 0x640000u &&
               (s->current_colours & FLOW_CURRENT_BORDER(0)), NULL);
    moving_close(&m);
    // And what the element's own rules settled stays settled: the base is
    // its computed style, and only a keyframe's values are as written
    // (Chrome, the same day). A span its rules float, an animation taking
    // the float away: a block, floated nowhere. An axis its rules hide, an
    // animation setting it back: both axes auto, since its pair was auto.
    // A keyframe hiding one axis beside the other visible, and a later
    // animation setting it back: both visible.
    moving_open(&m, MOTION_STYLE
        "@keyframes nf{from{float:none}to{float:none}}"
        "@keyframes a{from{overflow-x:hidden;overflow-y:visible}to{overflow-x:hidden;overflow-y:visible}}"
        "@keyframes b{from{overflow-x:visible}to{overflow-x:visible}}"
        "#nf{float:left;animation:nf 1s linear -.5s paused}"
        "#p{overflow-x:hidden;animation:b 1s linear -.5s paused}"
        "#o{animation:a 1s linear -.5s paused,b 1s linear -.5s paused}"
        "#q{animation:a 1s linear -.5s paused}"
        "</style><p><span id=nf>x</span></p><div id=p></div><div id=o></div><div id=q></div>", 1000);
    s = moving_style(&m, "nf");
    expect("motion: a float its rules gave, taken away by an animation, leaves a block",
           s != NULL && s->display == FLOW_DISPLAY_BLOCK && s->float_side == FLOW_FLOAT_NONE, NULL);
    s = moving_style(&m, "p");
    expect("motion: an axis its rules hid, set back by an animation, leaves both auto",
           s != NULL && s->overflow_x == FLOW_OVERFLOW_AUTO && s->overflow_y == FLOW_OVERFLOW_AUTO, NULL);
    s = moving_style(&m, "o");
    expect("motion: a keyframe's own overflow pair is not settled before the mix",
           s != NULL && s->overflow_x == FLOW_OVERFLOW_VISIBLE && s->overflow_y == FLOW_OVERFLOW_VISIBLE, NULL);
    s = moving_style(&m, "q");
    expect("motion: and settled after it",
           s != NULL && s->overflow_x == FLOW_OVERFLOW_HIDDEN && s->overflow_y == FLOW_OVERFLOW_AUTO, NULL);
    moving_close(&m);

    // Blocks at one offset that ease as the element's own easing are one
    // keyframe, the last written of each longhand winning; one naming
    // another easing is a keyframe of its own (Chrome, 10/14: 0.5, 0.1,
    // 0.0630713 at a quarter).
    moving_open(&m, MOTION_STYLE
        "@keyframes jk{0%{opacity:0}50%{opacity:.2;animation-timing-function:linear}50%{opacity:1}}"
        "@keyframes jj{0%{opacity:0}50%{opacity:.2;animation-timing-function:ease-in}50%{opacity:1}}"
        "#ja{animation:jk 1s linear -.25s paused}"
        "#jb{animation:jj 1s linear -.25s paused}"
        "#jc{animation:jk 1s ease-in -.25s paused}"
        "</style><div id=ja></div><div id=jb></div><div id=jc></div>", 1000);
    expect_i64("motion: a block easing as the element does joins the one easing as nothing",
               (s = moving_style(&m, "ja")) != NULL ? s->opacity : -1, 500);
    expect_i64("motion: a block naming another easing is a keyframe of its own",
               (s = moving_style(&m, "jb")) != NULL ? s->opacity : -1, 100);
    expect_i64("motion: and so is one naming linear when the element eases in",
               (s = moving_style(&m, "jc")) != NULL ? s->opacity : -1, 63);
    moving_close(&m);

    // The element's easing is one easing with a block's as keyframes are
    // grouped: `ease` naming itself joins, the same curve written out does
    // not, and steps are one however written (Chrome, 10/14: 0.160481, 0.5,
    // 0.802403 at a quarter) — and `ease` is the initial easing, which an
    // element names by naming none.
    moving_open(&m, MOTION_STYLE
        "@keyframes jl{0%{opacity:0}50%{opacity:.2;animation-timing-function:cubic-bezier(0.25,0.1,0.25,1)}"
        "50%{opacity:1}}"
        "@keyframes jm{0%{opacity:0}50%{opacity:.2;animation-timing-function:steps(2, jump-end)}50%{opacity:1}}"
        "@keyframes jn{0%{opacity:0}50%{opacity:.2;animation-timing-function:ease}50%{opacity:1}}"
        "#jd{animation:jl 1s -.25s paused}"
        "#je{animation:jm 1s steps(2) -.25s paused}"
        "#jf{animation:jn 1s -.25s paused}"
        "#jg{animation-name:jn;animation-duration:1s;animation-delay:-.25s;animation-play-state:paused}"
        "</style><div id=jd></div><div id=je></div><div id=jf></div><div id=jg></div>", 1000);
    expect_i64("motion: a curve written out is another easing than the keyword it draws",
               (s = moving_style(&m, "jd")) != NULL ? s->opacity : -1, 160);
    expect_i64("motion: steps are one easing however written",
               (s = moving_style(&m, "je")) != NULL ? s->opacity : -1, 500);
    expect_i64("motion: a keyword naming the element's easing joins it",
               (s = moving_style(&m, "jf")) != NULL ? s->opacity : -1, 802);
    expect_i64("motion: and so does `ease` where the element names no easing",
               (s = moving_style(&m, "jg")) != NULL ? s->opacity : -1, 802);
    moving_close(&m);

    // An implicit keyframe is the value the animations before it made,
    // not the element's own (Chrome, 10/14: 0.25, 0.25, 0.75, 0.125 — the
    // last with the one underneath a quarter through).
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}@keyframes dn{to{opacity:0}}"
        "@keyframes df{from{opacity:0}}@keyframes st{to{opacity:1}}"
        "#ua{animation:up 1s linear -.5s paused,dn 1s linear -.5s paused}"
        "#ub{animation:up 1s linear -.5s paused,df 1s linear -.5s paused}"
        "#uc{animation:up 1s linear -.5s paused,st 1s linear -.5s paused}"
        "#ud{animation:up 1s linear -.25s paused,dn 1s linear -.5s paused}"
        "</style><div id=ua></div><div id=ub></div><div id=uc></div><div id=ud></div>", 1000);
    static const struct { const char *id; int64_t want; } kUnder[] = {
        {"ua", 250}, {"ub", 250}, {"uc", 750}, {"ud", 125},
    };
    for (size_t i = 0; i < sizeof(kUnder) / sizeof(kUnder[0]); i++)
        expect_i64("motion: an implicit keyframe is the value underneath",
                   (s = moving_style(&m, kUnder[i].id)) != NULL ? s->opacity : -1, kUnder[i].want);
    moving_close(&m);

    // A transform underneath that is a blend — rotate(0) to translateX(100px),
    // mixed as matrices — is what an implicit keyframe over it rests on,
    // worked out as drawn (Chrome, 10/14: matrix(1, 0, 0, 1, 50, 0) at the
    // later one's start, matrix(1.5, 0, 0, 1.5, 25, 0) halfway).
    moving_open(&m, MOTION_STYLE
        "@keyframes mv{from{transform:rotate(0)}to{transform:translateX(100px)}}"
        "@keyframes sc{to{transform:scale(2)}}"
        "#ba{animation:mv 1s linear -.5s paused,sc 1s linear 0s paused}"
        "#bb{animation:mv 1s linear -.5s paused,sc 1s linear -.5s paused}"
        "</style><div id=ba></div><div id=bb></div>", 1000);
    expect_matrix("motion: a blend underneath is its matrix at the start of the one over it", &m, "ba",
                  (const double[6]){1, 0, 0, 1, 50, 0});
    expect_matrix("motion: and halfway", &m, "bb", (const double[6]){1.5, 0, 0, 1.5, 25, 0});
    moving_close(&m);

    // Twenty thousand animations each resting on the blend the one under
    // it made: a mix nesting blends past F_BLEND_DEPTH_MAX finds no room,
    // so the nest the painter works out stays shallow, and the page draws.
    {
        enum { kNames = 20000 };
        size_t cap = (size_t)kNames * 4 + 1024, at = 0;
        char *html = malloc(cap);
        at += (size_t)snprintf(html + at, cap - at, MOTION_STYLE
                               "@keyframes ka{to{transform:rotate(45deg)}}@keyframes kb{to{transform:scale(2)}}"
                               "#deep{animation-duration:1s;animation-timing-function:linear;"
                               "animation-delay:-.5s;animation-play-state:paused;animation-name:");
        for (int i = 0; i < kNames; i++)
            at += (size_t)snprintf(html + at, cap - at, "%s%s", i ? "," : "", i % 2 ? "kb" : "ka");
        (void)snprintf(html + at, cap - at, "}</style><div id=deep></div>");
        moving_open(&m, html, 1000);
        double deep[16];
        bool drawn = moving_matrix(&m, "deep", deep);
        for (int i = 0; drawn && i < 16; i++)
            drawn = isfinite(deep[i]);
        expect("motion: animations resting on one another's blends, deeper than the nest allows, draw",
               drawn, NULL);
        moving_close(&m);
        free(html);
    }

    // A shadow and a border mixed from currentColor to a colour use the
    // colour the element ends with, which another animation moves (Chrome,
    // 10/14: rgb(100,0,0) composed, both rgb(50,0,128) halfway to blue).
    moving_open(&m, MOTION_STYLE
        "@keyframes col{from{color:rgb(0,0,0)}to{color:rgb(200,0,0)}}"
        "@keyframes sh{from{box-shadow:5px 0 0 0 currentColor;border-top-color:currentColor}"
        "to{box-shadow:5px 0 0 0 rgb(0,0,255);border-top-color:rgb(0,0,255)}}"
        "#pc{border-top:2px solid;animation:col 1s linear -.5s paused,sh 1s linear -.5s paused}"
        "#pd{border-top:2px solid;animation:sh 1s linear -.5s paused,col 1s linear -.5s paused}"
        "</style><div id=pc></div><div id=pd></div>", 1000);
    for (int i = 0; i < 2; i++) {
        s = moving_style(&m, i == 0 ? "pc" : "pd");
        expect(i == 0 ? "motion: a one-sided currentColor mixes the composed colour"
                      : "motion: and so with the colour's animation named after it",
               s != NULL && s->color == 0x640000u && s->nbox_shadows == 1 &&
                   s->box_shadows[0].colour == 0x320080u && s->border_color[0] == 0x320080u, NULL);
    }
    moving_close(&m);

    // Two axes past the domain are held at its limit (values.h) and
    // compared as the unit vectors they are: x and y, which differ, so they
    // turn as matrices, as the same axes written small do.
    moving_open(&m, MOTION_STYLE
        "@keyframes big{from{transform:rotate3d(1e200,0,0,90deg)}to{transform:rotate3d(0,1e200,0,90deg)}}"
        "@keyframes small{from{transform:rotate3d(1,0,0,90deg)}to{transform:rotate3d(0,1,0,90deg)}}"
        "</style><div id=big style='animation-name:big;" AT("0.5") "'></div>"
        "<div id=small style='animation-name:small;" AT("0.5") "'></div>", 1000);
    double want[16], got[16];
    bool both = moving_matrix(&m, "small", want) && moving_matrix(&m, "big", got);
    bool same = both;
    for (int i = 0; both && i < 16; i++)
        same &= fabs(want[i] - got[i]) < 1e-9;
    expect("motion: rotation axes past the domain compared as units", same, NULL);
    moving_close(&m);

    // And two axes too small to square (the domain bounds how large a
    // number is, never how small): taken by their largest part, they are
    // the same units, where squared whole they would underflow to no axis.
    moving_open(&m, MOTION_STYLE
        "@keyframes tiny{from{transform:rotate3d(1e-200,0,0,90deg)}to{transform:rotate3d(0,1e-200,0,90deg)}}"
        "@keyframes small{from{transform:rotate3d(1,0,0,90deg)}to{transform:rotate3d(0,1,0,90deg)}}"
        "</style><div id=tiny style='animation-name:tiny;" AT("0.5") "'></div>"
        "<div id=small style='animation-name:small;" AT("0.5") "'></div>", 1000);
    both = moving_matrix(&m, "small", want) && moving_matrix(&m, "tiny", got);
    same = both;
    for (int i = 0; both && i < 16; i++)
        same &= fabs(want[i] - got[i]) < 1e-9;
    expect("motion: rotation axes too small to square compared as units", same, NULL);
    moving_close(&m);

    // A matrix past the domain mixes as the one at its limit, and that one
    // as the same matrix written small, scaled by 1e15. Two angles in two
    // units mix in degrees (f_angle_common), and Chrome 155 agrees to its
    // six digits: rotate(1rad) to rotate(0.5turn) at 0.3 is matrix(-0.0716201,
    // 0.997432, ...), and an angle past the domain is held in degrees,
    // whatever its unit: rotate(1e15rad) and rotate(1e13turn), each to
    // rotate(0deg), mix as the same 1e15deg.
    moving_open(&m, MOTION_STYLE
        "@keyframes mpast{from{transform:matrix(1e200,0,0,1e200,0,0)}to{transform:matrix(3e200,1e15,-1e15,3e200,0,0)}}"
        "@keyframes mbig{from{transform:matrix(1e15,0,0,1e15,0,0)}to{transform:matrix(1e15,1e15,-1e15,1e15,0,0)}}"
        "@keyframes msmall{from{transform:matrix(1,0,0,1,0,0)}to{transform:matrix(1,1,-1,1,0,0)}}"
        "@keyframes units{from{transform:rotate(1rad)}to{transform:rotate(0.5turn)}}"
        "@keyframes rad{from{transform:rotate(1e15rad)}to{transform:rotate(0deg)}}"
        "@keyframes turn{from{transform:rotate(1e13turn)}to{transform:rotate(0deg)}}"
        "</style><div id=mpast style='animation-name:mpast;" AT("0.5") "'></div>"
        "<div id=mbig style='animation-name:mbig;" AT("0.5") "'></div>"
        "<div id=msmall style='animation-name:msmall;" AT("0.5") "'></div>"
        "<div id=units style='animation-name:units;" AT("0.3") "'></div>"
        "<div id=rad style='animation-name:rad;" AT("0.3") "'></div>"
        "<div id=turn style='animation-name:turn;" AT("0.3") "'></div>", 1000);
    double small_m[16];
    both = moving_matrix(&m, "msmall", small_m) && moving_matrix(&m, "mbig", want) &&
           moving_matrix(&m, "mpast", got);
    same = both;
    for (int i = 0; both && i < 16; i++) {
        // The 2x2 scaled by 1e15, translation and the rest as they were.
        bool linear = i == 0 || i == 1 || i == 4 || i == 5;
        double w = linear ? small_m[i] * 1e15 : small_m[i];
        same &= want[i] == got[i] && fabs(w - want[i]) <= 1e-9 * fmax(1, fabs(w));
    }
    expect("motion: a matrix past the domain mixes as its limit, and that as its small self", same,
           NULL);
    same = moving_matrix(&m, "units", got) && fabs(got[0] - -0.0716201) < 5e-7 &&
           fabs(got[1] - 0.997432) < 5e-7;
    expect("motion: two units mix in degrees, as Chrome's", same, NULL);
    both = moving_matrix(&m, "rad", want) && moving_matrix(&m, "turn", got);
    same = both;
    for (int i = 0; both && i < 16; i++)
        same &= isfinite(got[i]) && fabs(want[i] - got[i]) < 1e-9;
    expect("motion: an angle past the domain is held in degrees, whatever its unit", same, NULL);
    moving_close(&m);

    // A tail a list multiplies out past a double makes the pair discrete,
    // as Chrome 155's is (twenty-two scale(1e15) and a turn, to
    // rotate(2deg), is rotate(2deg) at one half): past it the second, and
    // before it the first — twenty-one scale(1e15), whose last multiply
    // makes a clean infinity, no zero times it yet to make a NaN, which
    // the painter carries through and never divides down for ever.
    {
        char css[1024];
        int32_t n = snprintf(css, sizeof(css), "%s@keyframes far{from{transform:", MOTION_STYLE);
        for (int32_t i = 0; i < 21; i++)
            n += snprintf(css + n, sizeof(css) - (size_t)n, "scale(1e15) ");
        snprintf(css + n, sizeof(css) - (size_t)n,
                 "}to{transform:rotate(2deg)}}</style>"
                 "<div id=near style='animation:far 1s linear -.25s paused'></div>"
                 "<div id=past style='animation:far 1s linear -.75s paused'></div>"
                 "<div id=two style='transform:rotate(2deg)'></div>");
        moving_open(&m, css, 1000);
        double past[16], two[16], near[16];
        bool flipped = moving_matrix(&m, "past", past) && moving_matrix(&m, "two", two);
        for (int i = 0; flipped && i < 16; i++)
            flipped = past[i] == two[i];
        expect("motion: a tail past a double is discrete, the second past one half", flipped, NULL);
        expect("motion: and the first before it, carried as the infinity it is",
               moving_matrix(&m, "near", near) && !isfinite(near[0]), NULL);
        moving_close(&m);
    }

    // A zero-length axis turns nothing, so it mixes as no turn about the
    // other's axis (Chrome, 10/14: rotate3d(1,0,0,45deg) to
    // rotate3d(0,0,0,90deg) is rotateX(22.5deg) halfway, and none at its
    // end); and a 3D matrix at the domain's limit, with a perspective row,
    // is undone and mixed, where one a list multiplies out past a double's
    // determinant makes no mix, and the pair is discrete.
    moving_open(&m, MOTION_STYLE
        "@keyframes z{from{transform:rotate3d(1,0,0,45deg)}to{transform:rotate3d(0,0,0,90deg)}}"
        "</style><div id=zh style='animation:z 1s linear -.5s paused'></div>"
        "<div id=ze style='animation:z 1s linear -1s paused forwards'></div>"
        "<div id=zx style='transform:rotateX(22.5deg)'></div>", 1000);
    both = moving_matrix(&m, "zx", want) && moving_matrix(&m, "zh", got);
    same = both;
    for (int i = 0; both && i < 16; i++)
        same &= fabs(want[i] - got[i]) < 1e-9;
    expect("motion: a zero-length axis mixes as no turn about the other's", same, NULL);
    same = moving_matrix(&m, "ze", got);
    for (int i = 0; same && i < 16; i++)
        same &= fabs(got[i] - (i % 5 == 0 ? 1 : 0)) < 1e-9;
    expect("motion: and ends turning nothing", same, NULL);
    moving_close(&m);
    {
        double big3d[16] = {1e15, 0, 0, 1e-15, 0, 1e15, 0, 0, 0, 0, 1e15, 0, 0, 0, 0, 1}, out[16];
        bool undone = f_matrix_mix(big3d, big3d, 0.5, out);
        for (int i = 0; undone && i < 16; i++)
            undone = fabs(out[i] - big3d[i]) <= 1e-9 * fmax(fabs(big3d[i]), 1);
        expect("motion: a 3D matrix at the domain's limit is undone", undone, NULL);
        double past[16] = {1e200, 0, 0, 1e-200, 0, 1e200, 0, 0, 0, 0, 1e200, 0, 0, 0, 0, 1};
        expect("motion: a 3D perspective whose inverse a double cannot hold makes no mix",
               !f_matrix_mix(past, big3d, 0.5, out), NULL);
        // With no perspective to undo, only the determinant's zero is read.
        double flat3[16] = {1e200, 0, 0, 0, 0, 1e200, 0, 0, 0, 0, 1e200, 0, 0, 0, 0, 1};
        undone = f_matrix_mix(flat3, flat3, 0.5, out);
        for (int i = 0; undone && i < 16; i++)
            undone = fabs(out[i] - flat3[i]) <= 1e-9 * fmax(fabs(flat3[i]), 1);
        expect("motion: a 3D matrix whose determinant passes a double is undone", undone, NULL);
    }

    // A 2D matrix whose axes are a double's whole range apart keeps both:
    // matrix(1e308, 0, 0, 1e-308, 0, 0), as a list may multiply out, its
    // determinant 1, mixed halfway to the identity is scales of 5e307 and a
    // half. One whose determinant a double rounds to 0 is discrete, as
    // Chrome 155's is: matrix(1e-200, 0, 0, 1e-200, 0, 0) to none is none
    // at one half, where matrix(1e-160, …) mixes to a half.
    {
        double far[16] = {1e308, 0, 0, 0, 0, 1e-308, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        double unit[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, out[16];
        bool kept = f_matrix_mix(far, unit, 0.5, out) && fabs(out[0] / 5e307 - 1) < 1e-9 &&
                    fabs(out[5] - 0.5) < 1e-9 && out[1] == 0 && out[4] == 0;
        expect("motion: a 2D matrix's axes a double's range apart are both kept", kept, NULL);
        double tiny[16] = {1e-200, 0, 0, 0, 0, 1e-200, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        double small[16] = {1e-160, 0, 0, 0, 0, 1e-160, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        expect("motion: a 2D matrix whose determinant a double rounds to 0 makes no mix",
               !f_matrix_mix(tiny, unit, 0.5, out), NULL);
        expect("motion: and one just above it mixes, as Chrome's",
               f_matrix_mix(small, unit, 0.5, out) && fabs(out[0] - 0.5) < 1e-12 &&
                   fabs(out[5] - 0.5) < 1e-12, NULL);
        // One whose determinant passes a double mixes as its small self,
        // scaled: only the determinant's zero and sign are read.
        double huge[16] = {1e200, 0, 0, 0, 0, 1e200, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        double huge2[16] = {3e200, 1e200, 0, 0, -1e200, 3e200, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        double one2[16] = {3, 1, 0, 0, -1, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, want_m[16];
        bool scaled = f_matrix_mix(huge, huge2, 0.5, out) && f_matrix_mix(unit, one2, 0.5, want_m);
        for (int i = 0; scaled && i < 16; i++) {
            bool linear = i == 0 || i == 1 || i == 4 || i == 5;
            double w = linear ? want_m[i] * 1e200 : want_m[i];
            scaled = fabs(w - out[i]) <= 1e-9 * fmax(fabs(w), 1);
        }
        expect("motion: a 2D matrix whose determinant passes a double mixes as its small self",
               scaled, NULL);
        // A singular one a list made that large is still singular: (1, 2; 2,
        // 4) times 1e300, its determinant infinity less infinity, read row
        // by row as 0 — where compared as a NaN it fell through, and its
        // rounding left a second axis of 7e284 to mix.
        double sing[16] = {1e300, 2e300, 0, 0, 2e300, 4e300, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        expect("motion: a singular 2D matrix past a double makes no mix",
               !f_matrix_mix(sing, unit, 0.5, out), NULL);
        // And in 3D, column by column.
        double sing3[16] = {1e300, 2e300, 0, 0, 2e300, 4e300, 0, 0, 0, 0, 3e300, 0, 0, 0, 0, 1};
        expect("motion: a singular 3D matrix past a double makes no mix",
               !f_matrix_mix(sing3, unit, 0.5, out), NULL);
    }

    // Whatever two matrices a list multiplies out to — not held, as what a
    // page writes is — their mix is a matrix or no mix (the pair
    // discrete), never a NaN or an infinity, and at its ends
    // each end itself, never another matrix a part held to a double's range
    // composes: huge scales, shears and turns in 2D, a 2D row whose length
    // passes DBL_MAX though its entries do not, a flipped 2D matrix whose
    // determinant is infinity less infinity, 3D rows whose lengths a
    // double cannot hold, and a perspective undone by an inverse with an
    // entry a double cannot hold.
    {
        const double M = DBL_MAX, H = 1.7e308, r = 0.7071067811865476;
        static double kMats[11][16];
        memset(kMats, 0, sizeof(kMats));
        const double flat[9][6] = {
            {1, 0, 0, 1, 0, 0}, {1e200, 0, 0, 1e200, 0, 0}, {M, 0, 0, M, 0, 0}, {M, 0, M, M, 0, 0},
            {M * r, M * r, -M * r, M * r, 0, 0}, {1e-300, 0, 0, 1e-300, 0, 0}, {0, 0, 0, 0, 0, 0},
            {H, H, 0, 1, 0, 0}, {1e200, 2e200, 2e200, 1e200, 0, 0},
        };
        for (int i = 0; i < 9; i++) {
            kMats[i][0] = flat[i][0];
            kMats[i][1] = flat[i][1];
            kMats[i][4] = flat[i][2];
            kMats[i][5] = flat[i][3];
            kMats[i][12] = flat[i][4];
            kMats[i][13] = flat[i][5];
            kMats[i][10] = kMats[i][15] = 1;
        }
        const double rows3d[16] = {H, H, 0, 0, 0, H, H, 0, H, 0, H, 0, 0, 0, 0, 1};
        memcpy(kMats[9], rows3d, sizeof(rows3d));
        const double persp3d[16] = {1e-310, 0, 0, 1e-10, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(kMats[10], persp3d, sizeof(persp3d));
        bool sound = true, ends = true;
        char detail[160] = "";
        for (int i = 0; i < 11 && sound && ends; i++)
            for (int j = 0; j < 11 && sound && ends; j++)
                for (int k = 0; k <= 4 && sound && ends; k++) {
                    double out[16];
                    if (!f_matrix_mix(kMats[i], kMats[j], k / 4.0, out))
                        continue;
                    const double *end = k == 0 ? kMats[i] : kMats[j];
                    double big = 0;
                    for (int e = 0; e < 16; e++)
                        big = fmax(big, fabs(end[e]));
                    for (int e = 0; e < 16; e++) {
                        sound &= isfinite(out[e]);
                        if ((k == 0 || k == 4) && !(fabs(out[e] - end[e]) <= big * 1e-12)) {
                            ends = false;
                            snprintf(detail, sizeof(detail), "%d to %d at %d: [%d] %.17g, not %.17g", i, j, k,
                                     e, out[e], end[e]);
                        }
                    }
                }
        expect("motion: a mix of matrices is a matrix or none, never a NaN", sound, NULL);
        expect("motion: a mix of matrices is each end at its ends", ends, detail);
        double out[16];
        bool refused = true;
        for (int k = 0; k <= 4; k++)
            refused &= !f_matrix_mix(kMats[7], kMats[0], k / 4.0, out);
        expect("motion: a 2D row longer than a double holds makes no mix", refused, NULL);
    }

    // skew(a, b) in two units is worked in degrees (f_angle_common), each
    // angle held there first: skew(1e308rad, 0deg) is skew(1e15deg, 0deg),
    // and skew(45deg, 50grad) is skew(45deg, 45deg).
    {
        Moving sk;
        moving_open(&sk, MOTION_STYLE "</style>"
                    "<div id=k1 style='transform:skew(1e308rad, 0deg)'></div>"
                    "<div id=k2 style='transform:skew(1e15deg, 0deg)'></div>"
                    "<div id=k3 style='transform:skew(45deg, 50grad)'></div>"
                    "<div id=k4 style='transform:skew(45deg, 45deg)'></div>", 0);
        double m1[16], m2[16], m3[16], m4[16];
        bool got = moving_matrix(&sk, "k1", m1) && moving_matrix(&sk, "k2", m2) &&
                   moving_matrix(&sk, "k3", m3) && moving_matrix(&sk, "k4", m4);
        bool same = got, mixed = got;
        for (int i = 0; got && i < 16; i++) {
            same &= m1[i] == m2[i];
            mixed &= fabs(m3[i] - m4[i]) <= 1e-12;
        }
        expect("motion: skew(1e308rad, 0deg) is skew(1e15deg, 0deg)", same, NULL);
        expect("motion: skew(45deg, 50grad) is skew(45deg, 45deg)", mixed, NULL);
        moving_close(&sk);
    }

    // An easing's y controls past the domain are held at its limit, so a
    // value mixed with itself at the huge fraction they make stays itself.
    {
        Moving ez;
        moving_open(&ez, MOTION_STYLE "</style>"
                    "<div id=e style='animation:k 1s cubic-bezier(0,1e308,1,-1e308)'></div>", 0);
        const flow_style_t *es = moving_style(&ez, "e");
        bool held = es != NULL && es->animations.neasing == 1 &&
                    es->animations.easing[0].p[1] == GARB_EASING_Y_MAX &&
                    es->animations.easing[0].p[3] == -GARB_EASING_Y_MAX;
        expect("motion: an easing's y past the domain is held at its limit", held, NULL);
        moving_close(&ez);
    }
    moving_open(&m, MOTION_STYLE
        "@keyframes sc{from{transform:scale(1.3) rotate(3.7deg)}to{transform:scale(1.3) rotate(3.7deg)}}"
        "@keyframes mx{from{transform:scale(2)}to{transform:rotate(10deg) scale(3)}}"
        "</style><div id=sc style='animation:sc 1s cubic-bezier(0,1e308,1,1e308) -.5s paused'></div>"
        "<div id=mx style='animation:mx 1s cubic-bezier(0,1e308,1,1e308) -.5s paused'></div>", 1000);
    double same_m[16] = {0}, wild[16] = {0};
    bool finite = moving_matrix(&m, "sc", same_m) && moving_matrix(&m, "mx", wild);
    for (int i = 0; finite && i < 16; i++)
        finite = isfinite(same_m[i]) && !isnan(wild[i]);
    double turn = 3.7 * 3.14159265358979323846 / 180;
    // And to the bit, in the functions' own arguments, where (1 - t) a + t a
    // misses by a few last bits of t a (1.2999999999301508 here).
    const flow_style_t *ss = moving_style(&m, "sc");
    bool exact = ss != NULL && ss->ntransform == 2 && ss->transform[0].a[0] == 1.3 &&
                 ss->transform[0].a[1] == 1.3 && ss->transform[1].a[3] == 3.7;
    expect("motion: a value mixed with itself at a huge fraction stays itself",
           finite && exact && fabs(same_m[0] - 1.3 * cos(turn)) < 1e-9 &&
               fabs(same_m[1] - 1.3 * sin(turn)) < 1e-9,
           NULL);
    moving_close(&m);

    // Forty keyframes of one property, 2.5% apart: halfway is the
    // twentieth, however many there are.
    char many[4096];
    size_t at = (size_t)snprintf(many, sizeof(many), MOTION_STYLE "@keyframes many{");
    for (int i = 0; i <= 40; i++)
        at += (size_t)snprintf(many + at, sizeof(many) - at, "%g%%{margin-left:%dpx}", i * 2.5, i * 10);
    snprintf(many + at, sizeof(many) - at,
             "}</style><div id=many style='animation-name:many;" AT("0.5") "'></div>");
    moving_open(&m, many, 1000);
    expect_i64("motion: forty keyframes, halfway",
               (s = moving_style(&m, "many")) != NULL ? s->margin[3].value : -1, 200 * 64);
    moving_close(&m);
}

// ── The side effects (Web Animations 1 § 5.6, Will Change 1) ───────────

// The boxes a walk opens groups for.
typedef struct {
    const flow_box_t *open[64];
    int32_t n;
} MotionGroups;

static void motion_group_visit(void *ctx, const flow_box_t *b)
{
    (void)ctx;
    (void)b;
}

static void motion_group_open(void *ctx, const flow_box_t *b, os64_gui_rect_t bounds)
{
    MotionGroups *g = ctx;
    (void)bounds;
    if (g->n < 64)
        g->open[g->n++] = b;
}

static void motion_effect_cases(void)
{
    // A transform animation from none makes its element the containing
    // block of a fixed child — paused at its start, in its delay, ended
    // filling forwards — and so does will-change: transform; ended with
    // no fill, it is no longer one (Chrome's answers, 10/13).
    Moving m;
    moving_open(&m, MOTION_STYLE
        "@keyframes go{from{transform:none}to{transform:translateX(100px)}}"
        ".p{margin:50px 0 0 200px;width:100px;height:50px}"
        ".f{position:fixed;left:0;top:0;width:10px;height:10px}"
        "</style>"
        "<div class=p style='animation:go 1s linear paused'><div class=f id=f1></div></div>"
        "<div class=p style='animation:go 1s linear 2s paused'><div class=f id=f2></div></div>"
        "<div class=p style='animation:go 1s linear forwards;animation-delay:-5s'>"
        "<div class=f id=f3></div></div>"
        "<div class=p style='animation:go 1s linear;animation-delay:-5s'>"
        "<div class=f id=f4></div></div>"
        "<div class=p style='will-change:transform'><div class=f id=f5></div></div>"
        "<div class=p><div class=f id=f6></div></div>", 1000);
    static const char *const ids[] = {"f1", "f2", "f3", "f4", "f5", "f6"};
    static const bool contained[] = {true, true, true, false, true, false};
    for (int i = 0; i < 6; i++) {
        const flow_box_t *b = moving_box(&m, ids[i]);
        char name[96];
        snprintf(name, sizeof(name), "motion: fixed child %s %s", ids[i],
                 contained[i] ? "inside its animated parent" : "fixed to the viewport");
        expect(name, b != NULL && (b->rect.x == 200) == contained[i], NULL);
    }
    moving_close(&m);

    // An animated opacity is a group for the life of the animation, even
    // at full opacity, and its own 0 leaves it painted; will-change:
    // opacity stacks it and makes no group.
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}"
        "</style>"
        "<div id=g0 style='animation-name:up;" AT("0") "'>a</div>"
        "<div id=g1 style='animation-name:up;animation-play-state:paused;animation-duration:1s;"
        "animation-fill-mode:forwards;animation-delay:-5s'>b</div>"
        "<div id=w style='will-change:opacity'>c</div>", 1000);
    const flow_box_t *g0 = moving_box(&m, "g0"), *g1 = moving_box(&m, "g1"),
                     *w = moving_box(&m, "w");
    expect("motion: at opacity 0 an animated box is still painted",
           g0 != NULL && !g0->unpainted && g0->style->opacity == 0 && g0->stacked, NULL);
    expect("motion: filling at full opacity it is still stacked",
           g1 != NULL && g1->stacked && g1->style->opacity == 1000 &&
           (g1->style->animating & FLOW_WILL_OPACITY), NULL);
    expect("motion: will-change: opacity stacks it",
           w != NULL && w->stacked && w->style->animating == 0, NULL);
    MotionGroups seen = {{NULL}, 0};
    flow_visitor_t v = {motion_group_visit, motion_group_open, NULL, &seen, NULL};
    flow_visit_groups(m.tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0}, &v);
    bool g1_open = false, w_open = false;
    for (int32_t i = 0; i < seen.n; i++) {
        g1_open |= seen.open[i] == g1;
        w_open |= seen.open[i] == w;
    }
    expect("motion: an animated opacity is a group, even at full opacity", g1_open, NULL);
    expect("motion: will-change: opacity alone makes no group", !w_open, NULL);
    moving_close(&m);
}

// A keyframe made by hand — the fuzzer's, and cases of the schedule's —
// resolved to the style the case sets, `r.style`, whatever it is resolved
// against; and the one declaration standing for it, a keyword spelling its
// value, so that two hand keyframes write one declaration exactly when
// they hold one value, which is what the schedule compares (frames_hold).
typedef struct {
    FResolved r;
    garb_set_t set;
    char word[24];
} Hand;

// The resolver for hand keyframes (FResolve): `ctx` holds each animation's
// hands, by keyframe.
static const FResolved *hand_resolved(void *ctx, int32_t a, int32_t j, const flow_style_t *over)
{
    (void)over;
    Hand *const *hands = ctx;
    return &hands[a][j].r;
}

// Keyframe `f` at `offset`, setting `p`: made by hand from `h`, whose style
// the caller sets, its declaration a keyword spelling `value`.
static void hand_frame(FKeyframe *f, Hand *h, garb_prop_t p, double offset, unsigned value)
{
    memset(f, 0, sizeof(*f));
    f->offset = offset;
    snprintf(h->word, sizeof(h->word), "v%u", value);
    memset(&h->set, 0, sizeof(h->set));
    h->set.prop = p;
    h->set.value.kind = GARB_V_KEYWORD;
    h->set.value.keyword = h->word;
    f->decls = &h->set;
    f->ndecls = 1;
    f_props_add(&f->sets, p);
}

// ── The timeline ────────────────────────────────────────────────────────

// Lists for layout_next's walk, from a pool it empties each time.
static unsigned char s_lists[1 << 20];
static size_t s_lists_used;

static void *lists_alloc(void *ctx, size_t bytes)
{
    (void)ctx;
    bytes = (bytes + 15) & ~(size_t)15;
    if (bytes > sizeof(s_lists) - s_lists_used)
        return NULL;
    void *p = s_lists + s_lists_used;
    s_lists_used += bytes;
    return p;
}

// What the schedule says at `now`, asked as the face asks it: after the
// layout at `now` it answers for (f_motion_style with `record`), each
// keyframe made by hand resolved from `hands` (hand_resolved).
static double layout_next(FMotion *m, double now, Hand *const *hands)
{
    flow_style_t at = m->base;
    s_lists_used = 0;
    (void)f_motion_style(m, now, &at, lists_alloc, hand_resolved, (void *)hands, NULL, true);
    double next = f_motion_next(m, now, NULL);
    // The layouts' half and the ticks' are the whole of it.
    double edges = f_motion_next_of(m, now, NULL, F_NEXT_EDGES);
    double values = f_motion_next_of(m, now, NULL, F_NEXT_VALUES);
    if (fmin(edges, values) != next)
        expect("motion: the schedule's two halves are the whole of it", false, NULL);
    return next;
}

// An animation `name` of `ms` milliseconds, once, of opacity, which it holds
// still: so it is due at its end and nothing before.
static void timed(FAnimation *an, const char *name, double ms)
{
    an->timing.name = (flow_name_t){name, (uint32_t)strlen(name)};
    an->timing.duration_ms = ms;
    an->timing.iterations = 1;
    f_props_add(&an->props, GARB_OPACITY);
}

// When an animation that holds still ends: its start, told by its record.
static double motion_end(FAnimation *an, double now)
{
    FMotion alone = {.anims = an, .nanims = 1};
    return layout_next(&alone, now, NULL);
}

// One layout's dealings with the timeline, told directly: begun from the
// one shown, each element in `meets` met at `now`, ended; and, when it
// comes through, shown in its place, as a face shows a tree's.
typedef struct {
    const os64_html_node_t *n;
    FMotion *m;
} Meet;

static bool timeline_layout(flow_timeline_t **shown, double now, bool complete, int count, const Meet *meets)
{
    flow_timeline_t *t = f_timeline_begin(*shown);
    bool ok = t != NULL;
    for (int i = 0; ok && i < count; i++)
        ok = f_timeline_element(t, meets[i].n, meets[i].m, now);
    if (!ok || !f_timeline_end(t, complete)) {
        flow_timeline_free(t);
        return false;
    }
    flow_timeline_free(*shown);
    *shown = t;
    return true;
}

static void motion_timeline_cases(void)
{
    // A start time survives every layout after the one that started it.
    Moving m;
    moving_open(&m, MOTION_STYLE
        "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "</style><div id=a style='animation:mv 1s linear'></div>", 1000);
    expect_matrix("motion: at its start, its first keyframe", &m, "a",
                  (const double[6]){1, 0, 0, 1, 0, 0});
    moving_again(&m, 1500);
    expect_matrix("motion: laid out again later, it has run since its start", &m, "a",
                  (const double[6]){1, 0, 0, 1, 50, 0});
    moving_again(&m, 1750);
    expect_matrix("motion: and again", &m, "a", (const double[6]){1, 0, 0, 1, 75, 0});
    moving_close(&m);

    // Animations off: a tree laid out with no timeline builds none, and
    // shown, it leaves the page's as it was — so turned on again, the
    // animation runs on from its start.
    moving_open(&m, MOTION_STYLE
        "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "</style><div id=a style='animation:mv 1s linear'></div>", 1000);
    m.p.env.timeline = NULL;
    m.p.env.now_ms = 1500;
    flow_tree_t *still = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
    flow_free(m.tree);
    m.tree = still;
    flow_timeline_t *was = m.tl;
    flow_timeline_commit(&m.tl, m.tree);
    m.p.env.timeline = m.tl;
    expect_matrix("motion: laid out with no timeline, its rules' style", &m, "a",
                  (const double[6]){1, 0, 0, 1, 0, 0});
    expect("motion: and shown, the page's timeline is kept", m.tl == was && f_timeline_records(m.tl) == 1, NULL);
    moving_again(&m, 1750);
    expect_matrix("motion: turned on again, it runs on from its start", &m, "a",
                  (const double[6]){1, 0, 0, 1, 75, 0});
    moving_close(&m);

    // Animations 1 § 2's matching, and the records' lives, told directly:
    // each layout begun from the timeline shown and, when it comes through,
    // shown (timeline_layout); a record's start read as when its ten
    // seconds end (motion_end). `a` to `a, a` keeps `a` as the second and
    // starts a first; a name gone is dropped by a complete layout and not
    // by an incomplete one; the holds are let go (a document freed with one
    // held would end the program).
    os64_html_document_t *doc = parse("<!doctype html><p id=x>x</p><p id=y>y</p><p id=z>z</p>");
    const os64_html_node_t *x = motion_find(doc->document, "x");
    const os64_html_node_t *y = motion_find(doc->document, "y");
    const os64_html_node_t *z = motion_find(doc->document, "z");
    flow_timeline_t *tl = flow_timeline_new(doc);
    FAnimation an[2];
    memset(an, 0, sizeof(an));
    for (int i = 0; i < 2; i++)
        timed(&an[i], "a", 10000);
    FMotion one = {.anims = an, .nanims = 1};
    FMotion two = {.anims = an, .nanims = 2};
    bool ok = timeline_layout(&tl, 1000, true, 1, (Meet[]){{x, &one}}) &&
              timeline_layout(&tl, 2000, true, 1, (Meet[]){{x, &two}});
    expect("motion: a to a, a keeps a as the second",
           ok && motion_end(&an[1], 2000) == 11000 && motion_end(&an[0], 2000) == 12000, NULL);
    // The element styled with nothing in an incomplete layout keeps its
    // records; in a complete one they go.
    ok = timeline_layout(&tl, 3000, false, 1, (Meet[]){{y, &one}}) &&
         timeline_layout(&tl, 4000, true, 1, (Meet[]){{x, &one}});
    expect("motion: an incomplete layout drops nothing, a complete one the rest",
           ok && motion_end(&an[0], 4000) == 11000 && f_timeline_records(tl) == 1, NULL);

    // An incomplete layout with no room to carry on what it did not reach
    // fails, and the timeline shown is as it was: z's g, started at 20000,
    // is matched by the next layout with that start.
    FAnimation g;
    memset(&g, 0, sizeof(g));
    timed(&g, "g", 10000);
    FMotion gm = {.anims = &g, .nanims = 1};
    flow_timeline_t *gt = flow_timeline_new(doc);
    ok = timeline_layout(&gt, 20000, true, 1, (Meet[]){{z, &gm}});
    flow_timeline_t *short_of = f_timeline_begin(gt);
    allocations = 0;
    fail_single = true;
    fail_at = 1;
    bool carried = short_of != NULL && f_timeline_end(short_of, false);
    fail_at = 0;
    fail_single = false;
    flow_timeline_free(short_of);
    ok = ok && short_of != NULL && timeline_layout(&gt, 25000, true, 1, (Meet[]){{z, &gm}});
    expect("motion: an incomplete layout with no room to carry on fails, and changes nothing",
           ok && !carried && motion_end(&g, 25000) == 30000 && f_timeline_records(gt) == 1, NULL);
    flow_timeline_free(gt);

    // A layout whose tree the face discards unshown changes nothing: the
    // animation it paused runs on from its start, the one it started is
    // not kept.
    FAnimation ua[2];
    memset(ua, 0, sizeof(ua));
    timed(&ua[0], "r", 1000);
    timed(&ua[1], "s", 1000);
    FMotion uone = {.anims = ua, .nanims = 1}, utwo = {.anims = ua, .nanims = 2};
    flow_timeline_t *ut = flow_timeline_new(doc);
    ok = timeline_layout(&ut, 40000, true, 1, (Meet[]){{z, &uone}});
    ua[0].timing.paused = true;
    flow_timeline_t *unshown = f_timeline_begin(ut);
    ok = ok && unshown != NULL && f_timeline_element(unshown, z, &utwo, 40500) &&
         f_timeline_end(unshown, false);
    flow_timeline_free(unshown);
    ua[0].timing.paused = false;
    ok = ok && timeline_layout(&ut, 40600, true, 1, (Meet[]){{z, &uone}});
    expect("motion: a layout discarded unshown changes nothing",
           ok && f_timeline_records(ut) == 1 && motion_end(&ua[0], 40600) == 41000, NULL);
    flow_timeline_free(ut);

    // A paused animation holds its time — nothing is due while it is — and
    // resumed, it runs from there: started paused at 5000, resumed at
    // 9000, its one second ends at 10000.
    FAnimation p;
    memset(&p, 0, sizeof(p));
    timed(&p, "p", 1000);
    p.timing.paused = true;
    FMotion paused = {.anims = &p, .nanims = 1};
    ok = timeline_layout(&tl, 5000, true, 1, (Meet[]){{y, &paused}});
    expect("motion: paused, nothing is due", ok && isinf(layout_next(&paused, 5000, NULL)), NULL);
    p.timing.paused = false;
    ok = ok && timeline_layout(&tl, 9000, true, 1, (Meet[]){{y, &paused}});
    expect("motion: resumed, it runs from where it was held",
           ok && layout_next(&paused, 9000, NULL) == 10000, NULL);
    // One running before it pauses holds what it has run: started at
    // 11000, paused at 11300, nothing is due at 11500; resumed at 13000,
    // its one second ends at 13700.
    FAnimation r;
    memset(&r, 0, sizeof(r));
    timed(&r, "r", 1000);
    FMotion running = {.anims = &r, .nanims = 1};
    ok = timeline_layout(&tl, 11000, true, 1, (Meet[]){{y, &running}});
    r.timing.paused = true;
    ok = ok && timeline_layout(&tl, 11300, true, 1, (Meet[]){{y, &running}});
    expect("motion: paused while running, nothing is due",
           ok && isinf(layout_next(&running, 11500, NULL)), NULL);
    r.timing.paused = false;
    ok = ok && timeline_layout(&tl, 13000, true, 1, (Meet[]){{y, &running}});
    expect("motion: resumed, it ends what it held short of",
           ok && layout_next(&running, 13000, NULL) == 13700, NULL);

    // A record an incomplete layout made is kept, and so is the one it
    // carried a name on in — once, not again beside it as a record no name
    // took: a, then a and b incomplete, then a again keeps the first a's
    // start.
    FAnimation two_names[2];
    memset(two_names, 0, sizeof(two_names));
    timed(&two_names[0], "a", 10000);
    timed(&two_names[1], "b", 10000);
    FMotion just_a = {.anims = two_names, .nanims = 1};
    FMotion a_and_b = {.anims = two_names, .nanims = 2};
    ok = timeline_layout(&tl, 15000, true, 1, (Meet[]){{x, &just_a}}) &&
         timeline_layout(&tl, 16000, false, 1, (Meet[]){{x, &a_and_b}});
    int32_t carried_on = f_timeline_records(tl);
    ok = ok && timeline_layout(&tl, 17000, true, 1, (Meet[]){{x, &just_a}});
    expect("motion: what an incomplete layout carried on is matched after it",
           ok && carried_on == 2 && motion_end(&two_names[0], 17000) == 25000, NULL);

    // A layout that fails part way takes what it built with it: the
    // timeline shown keeps its records, what the failed layout paused runs
    // on from where it ran, and the next layout makes b and z's a anew and
    // keeps the rest.
    FAnimation u;
    memset(&u, 0, sizeof(u));
    timed(&u, "u", 1000);
    FMotion pausing = {.anims = &u, .nanims = 1};
    ok = timeline_layout(&tl, 18000, true, 2, (Meet[]){{y, &pausing}, {x, &just_a}});
    int32_t kept_records = f_timeline_records(tl), kept_elements = f_timeline_elements(tl);
    u.timing.paused = true;
    flow_timeline_t *failed = f_timeline_begin(tl);
    ok = ok && failed != NULL && f_timeline_element(failed, y, &pausing, 18200) &&
         f_timeline_element(failed, x, &a_and_b, 18200) && f_timeline_element(failed, z, &one, 18200);
    bool grew = f_timeline_records(failed) == kept_records + 2;
    flow_timeline_free(failed);
    u.timing.paused = false;
    expect("motion: a layout that fails leaves the timeline shown as it was",
           ok && grew && kept_records == 2 && f_timeline_records(tl) == kept_records &&
               kept_elements == 2 && f_timeline_elements(tl) == kept_elements, NULL);
    ok = timeline_layout(&tl, 18300, true, 3, (Meet[]){{y, &pausing}, {x, &a_and_b}, {z, &one}});
    expect("motion: and the next layout keeps the shown starts, and makes anew what the failed one made",
           ok && motion_end(&u, 18300) == 19000 && motion_end(&two_names[0], 18300) == 25000 &&
               motion_end(&two_names[1], 18300) == 28300 && motion_end(&an[0], 18300) == 28300 &&
               f_timeline_records(tl) == 4, NULL);

    // Matching at scale: three thousand names, a and b in turn, matched
    // as Animations 1 § 2 says — the last of each name to the last record
    // of its name. Fifteen hundred are laid out at 20000, all three
    // thousand at 21000, which gives the second half the first half's
    // records and starts the first half anew; then a list of the second
    // half alone keeps those records, the earlier start, and the rest go.
    enum { kMany = 3000 };
    FAnimation *many = calloc(kMany, sizeof(*many));
    for (int i = 0; i < kMany; i++)
        timed(&many[i], i % 2 ? "b" : "a", 100000);
    FMotion first = {.anims = many, .nanims = kMany / 2};
    FMotion lots = {.anims = many, .nanims = kMany};
    flow_timeline_t *mt = flow_timeline_new(doc);
    ok = timeline_layout(&mt, 20000, true, 1, (Meet[]){{x, &first}}) &&
         timeline_layout(&mt, 21000, true, 1, (Meet[]){{x, &lots}});
    bool kept = ok && f_timeline_records(mt) == kMany;
    for (int i = 0; kept && i < kMany; i++)
        kept = motion_end(&many[i], 21000) == (i < kMany / 2 ? 121000 : 120000);
    FMotion half = {.anims = many + kMany / 2, .nanims = kMany / 2};
    ok = ok && timeline_layout(&mt, 22000, true, 1, (Meet[]){{x, &half}});
    for (int i = 0; kept && i < kMany / 2; i++)
        kept = motion_end(&many[kMany / 2 + i], 22000) == 120000;
    expect("motion: three thousand names matched, and half of them dropped",
           ok && kept && f_timeline_records(mt) == kMany / 2, NULL);
    free(many);
    flow_timeline_free(mt);
    flow_timeline_free(tl);
    os64_html_document_free(doc);
}

// ── When the page is next due (flow_motion_due) ─────────────────────────

static void motion_due_cases(void)
{
    static const os64_gui_rect_t kView = {0, 0, 800, 600};
    static const struct {
        const char *name, *animation;
        uint64_t due;
    } kCases[] = {
        {"motion due: a continuous value now", "mv 1s linear", 1000},
        {"motion due: steps, while they run, now", "mv 1s steps(4,end)", 1000},
        {"motion due: paused, never", "mv 1s linear paused", FLOW_NEVER},
        {"motion due: in its delay, its start", "mv 1s linear 500ms", 1500},
        {"motion due: a discrete value, while it runs, now", "ff 1s linear", 1000},
        {"motion due: ended and filling, never", "mv 1s linear forwards -2s", FLOW_NEVER},
        {"motion due: hidden to visible, while it runs, now", "hv 1s linear", 1000},
        {"motion due: visible to hidden, while it runs, now", "vh 1s linear", 1000},
        // Two equal ends hold still, whatever the easing, and are due at
        // the interval's far end; a pause that resumes is due there too.
        {"motion due: two equal ends hold still, due at their far end", "hd 1s linear", 1500},
        {"motion due: and so through steps", "hd 1s steps(3,end)", 1500},
        {"motion due: and on a discrete value", "fh 1s linear", 1400},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char html[1024];
        snprintf(html, sizeof(html), MOTION_STYLE
                 "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
                 "@keyframes ff{from{font-family:serif}to{font-family:monospace}}"
                 "@keyframes hv{from{visibility:hidden}to{visibility:visible}}"
                 "@keyframes vh{from{visibility:visible}to{visibility:hidden}}"
                 "@keyframes hd{0%%{opacity:.5}50%%{opacity:.5}100%%{opacity:1}}"
                 "@keyframes fh{0%%{font-family:serif}40%%{font-family:serif}100%%{font-family:monospace}}"
                 "</style><div style='animation:%s'>x</div>", kCases[i].animation);
        Moving m;
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[64];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kCases[i].name, due == kCases[i].due, due == kCases[i].due ? NULL : detail);
        moving_close(&m);
    }
    // steps(2147483647) runs at the frame rate: due now, nothing walked.
    Moving m;
    moving_open(&m, MOTION_STYLE
        "@keyframes c{to{color:red}}"
        "</style><div style='animation:c 1s steps(2147483647)'>x</div>", 1000);
    expect("motion due: two billion steps, while they run, now",
           flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) == 1000, NULL);
    moving_close(&m);

    // An implicit end rests on the value underneath: where an animation
    // under it moves that value, the two ends are equal only at this
    // instant, and the interval moves; where it holds still (paused), the
    // ends hold, and the one resting on it is due at its own end.
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}@keyframes zr{to{opacity:0}}"
        "</style><div style='animation:up 1s linear,zr 1s linear'>x</div>", 1000);
    expect_u32("motion due: equal ends resting on a moving value move",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1000);
    moving_close(&m);
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}@keyframes zr{to{opacity:0}}"
        "</style><div style='animation:up 1s linear paused,zr 1s linear'>x</div>", 1000);
    expect_u32("motion due: and resting on a still one hold, due at their end",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 2000);
    moving_close(&m);
    // An animation running but holding still (its own two ends equal) moves
    // nothing under the one resting on it: both are due at the first's end
    // of its hold, 1500.
    moving_open(&m, MOTION_STYLE
        "@keyframes hd{0%{opacity:0}50%{opacity:0}100%{opacity:1}}@keyframes zr{to{opacity:0}}"
        "</style><div style='animation:hd 1s linear,zr 1s linear'>x</div>", 1000);
    expect_u32("motion due: and resting on one running but holding still hold too",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1500);
    moving_close(&m);
    // Two ends of an animation's own replace the value underneath and what
    // moved it: the one resting on them holds, and both are due at their
    // end.
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}@keyframes hf{from{opacity:.5}to{opacity:.5}}"
        "@keyframes zh{to{opacity:.5}}"
        "</style><div style='animation:up 1s linear,hf 1s linear,zh 1s linear'>x</div>", 1000);
    expect_u32("motion due: and resting on two ends replacing a moving value hold too",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 2000);
    moving_close(&m);
    // And replace a value the schedule's walk could not make: the shadows
    // under them make a list, none at both ends makes none.
    moving_open(&m, MOTION_STYLE
        "@keyframes sh{from{box-shadow:0 0 red}to{box-shadow:5px 5px red}}"
        "@keyframes nn{from{box-shadow:none}to{box-shadow:none}}@keyframes zn{to{box-shadow:none}}"
        "</style><div style='animation:sh 1s linear,nn 1s linear,zn 1s linear'>x</div>", 1000);
    expect_u32("motion due: and resting on two ends replacing an unmade value hold too",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 2000);
    moving_close(&m);

    // Played either way — forwards, reverse, alternate, alternate-reverse
    // — every change is due when it shows. The oracle is the page laid out
    // at every millisecond for two seconds beside it; the face, following
    // only flow_motion_due from 1000, must lay out at each instant the
    // value differs from the one before.
    static const struct {
        const char *name, *keyframes, *animation;
    } kBack[] = {
        {"motion due: a discrete value through a curve crossing one half three times",
         "@keyframes k{from{text-align:left}to{text-align:right}}", "k 1s cubic-bezier(.3,2,.7,-1) 2"},
        {"motion due: steps inside three keyframes",
         "@keyframes k{0%{opacity:0}40%{opacity:.4}100%{opacity:1}}", "k 1s steps(2,end) 2"},
        {"motion due: steps(3, start) forwards",
         "@keyframes k{from{opacity:0}to{opacity:1}}", "k 1s steps(3,start) 2"},
        {"motion due backwards: steps(4) reversed",
         "@keyframes k{from{opacity:0}to{opacity:1}}", "k 1s steps(4,end) reverse 2"},
        {"motion due backwards: steps(3, start) alternating",
         "@keyframes k{from{opacity:0}to{opacity:1}}", "k 1s steps(3,start) alternate 2"},
        {"motion due backwards: a discrete value through an overshooting curve, reversed",
         "@keyframes k{from{text-align:left}to{text-align:right}}", "k 1s cubic-bezier(.3,2,.7,-1) reverse 2"},
        {"motion due backwards: visible to hidden overshooting, alternate-reverse",
         "@keyframes k{from{visibility:visible}to{visibility:hidden}}",
         "k 1s cubic-bezier(0,2,1,2) alternate-reverse 2"},
        {"motion due backwards: hidden to visible, reversed",
         "@keyframes k{from{visibility:hidden}to{visibility:visible}}", "k 1s linear reverse 2"},
        {"motion due backwards: steps inside three keyframes, reversed",
         "@keyframes k{0%{opacity:0}40%{opacity:.4}100%{opacity:1}}", "k 1s steps(2,end) reverse 2"},
        {"motion due: visibility through a curve whose controls are held at the domain's limit",
         "@keyframes k{from{visibility:hidden}to{visibility:visible}}",
         "k 1s cubic-bezier(.5,1e308,.5,-1e308) 2"},
        {"motion due backwards: visible to hidden, reversed",
         "@keyframes k{from{visibility:visible}to{visibility:hidden}}", "k 1s linear reverse 2"},
        {"motion due: a value standing still, then moving",
         "@keyframes k{0%{opacity:1}50%{opacity:1}100%{opacity:0}}", "k 1s linear 2"},
    };
    for (size_t i = 0; i < sizeof(kBack) / sizeof(kBack[0]); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "%s</style><div id=x style='animation:%s'>x</div>",
                 kBack[i].keyframes, kBack[i].animation);
        Moving oracle, face;
        moving_open(&oracle, html, 1000);
        moving_open(&face, html, 1000);
        uint32_t values[2001];
        for (int t = 0; t <= 2000; t++) {
            if (t > 0)
                moving_again(&oracle, 1000 + (uint64_t)t);
            const flow_style_t *st = moving_style(&oracle, "x");
            values[t] = st == NULL ? UINT32_MAX
                      : (uint32_t)st->opacity << 16 | (uint32_t)st->visibility << 8 | (uint32_t)st->text_align;
        }
        bool seen[2001] = {false};
        seen[0] = true;
        for (uint64_t now = 1000; now <= 3000;) {
            uint64_t due = flow_motion_due(face.tree, kView, (flow_point_t){0, 0});
            if (due == FLOW_NEVER || due > 3000)
                break;
            now = due > now ? due : now + 1;
            moving_again(&face, now);
            if (now <= 3000)
                seen[now - 1000] = true;
        }
        int missed = -1;
        for (int t = 1; t <= 2000 && missed < 0; t++)
            if (values[t] != values[t - 1] && !seen[t])
                missed = t;
        char detail[64];
        snprintf(detail, sizeof(detail), "change at %d ms not laid out", 1000 + missed);
        expect(kBack[i].name, missed < 0, missed < 0 ? NULL : detail);
        moving_close(&oracle);
        moving_close(&face);
    }

    // A value that does not change is not moving, however its keyframes
    // are written: due at its iteration's end, not every frame — an
    // opacity, a transform, a shadow, each the same at both ends.
    static const char *const kStill[] = {
        "@keyframes k{from{opacity:1}to{opacity:1}}",
        "@keyframes k{from{transform:rotate(10deg) translateX(5px)}to{transform:rotate(10deg) translateX(5px)}}",
        "@keyframes k{from{box-shadow:2px 3px 4px red}to{box-shadow:2px 3px 4px red}}",
        // Five functions: room in each of the schedule's two looks at the
        // next millisecond, not in both together.
        "@keyframes k{from{transform:rotate(1deg) translateX(5px) scale(2) skewX(5deg) rotate(3deg)}"
        "to{transform:rotate(1deg) translateX(5px) scale(2) skewX(5deg) rotate(3deg)}}",
    };
    for (int i = 0; i < 4; i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "%s</style><div style='animation:k 1s linear infinite'>x</div>",
                 kStill[i]);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        static const char *const kStillNames[] = {
            "motion due: an opacity that stands still is due at its iteration's end",
            "motion due: a transform that stands still is due at its iteration's end",
            "motion due: a shadow that stands still is due at its iteration's end",
            "motion due: a long transform list that stands still is due at its iteration's end",
        };
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kStillNames[i], due == 2000, detail);
        moving_close(&m);
    }

    // What is not drawn moving is not due every frame: an opacity a later
    // animation holds still, whatever the earlier one does to it; and a
    // transform whose matrices cannot be undone, drawn as one and then the
    // other (f_blend_matrix), is due where it flips, at one half.
    moving_open(&m, MOTION_STYLE
        "@keyframes mov{from{opacity:0}to{opacity:1}}"
        "@keyframes fix{from{opacity:1}to{opacity:1}}"
        "@keyframes flat{from{transform:matrix(0,0,0,0,0,0)}to{transform:matrix(1,0,0,1,0,0)}}"
        "</style><div id=ov style='animation:mov 1s linear infinite,fix 1s linear infinite'></div>", 1000);
    expect_u32("motion due: an opacity a later animation holds is not moving",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 2000);
    moving_close(&m);
    // An animation resting on the value underneath shows it through: its
    // own end the element's opacity, it still moves while the one under
    // it does — and so does that one, which it does not replace.
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}@keyframes st{to{opacity:1}}"
        "</style><div id=ov style='animation:up 1s linear,st 1s linear'>x</div>", 1000);
    expect_u32("motion due: an animation resting on a moving one moves",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1000);
    moving_close(&m);
    // An animation resting on the value underneath replaces nothing: the
    // one under it, stepping, runs at the frame rate.
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}@keyframes dn{to{opacity:0}}"
        "</style><div id=ov style='animation:up 1s steps(2, end),dn 1s linear'>x</div>", 1000);
    expect_u32("motion due: the one under an implicit keyframe, while it runs, now",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1000);
    moving_close(&m);
    // A value underneath the schedule's walk had no room to make — a
    // 64-function list held still — is not known, and the animation
    // resting on it, from it to none, moves.
    {
        size_t cap = 64 * 17 * 2 + 512, at = 0;
        char *html = malloc(cap);
        at += (size_t)snprintf(html + at, cap - at, MOTION_STYLE "@keyframes big{from{transform:");
        for (int i = 0; i < 64; i++)
            at += (size_t)snprintf(html + at, cap - at, "translateX(1px) ");
        at += (size_t)snprintf(html + at, cap - at, "}to{transform:");
        for (int i = 0; i < 64; i++)
            at += (size_t)snprintf(html + at, cap - at, "translateX(1px) ");
        (void)snprintf(html + at, cap - at, "}}@keyframes off{to{transform:none}}</style>"
                       "<div style='animation:big 1s linear,off 1s linear'>x</div>");
        moving_open(&m, html, 1000);
        expect_u32("motion due: an animation resting on a value the walk could not make moves",
                   (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1000);
        moving_close(&m);
        free(html);
    }
    // And where the earlier one also sets a colour it holds still, which
    // shows: its clock counts, its opacity still does not move.
    moving_open(&m, MOTION_STYLE
        "@keyframes mov{from{opacity:0;color:red}to{opacity:1;color:red}}"
        "@keyframes fix{from{opacity:1}to{opacity:1}}"
        "</style><div id=ov style='animation:mov 1s linear infinite,fix 1s linear infinite'>x</div>", 1000);
    expect_u32("motion due: and so where the earlier one sets something else that shows",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 2000);
    moving_close(&m);
    moving_open(&m, MOTION_STYLE
        "@keyframes mov{from{opacity:0}to{opacity:1}}"
        "@keyframes fix{from{opacity:1}to{opacity:1}}"
        "</style><div id=ov style='animation:mov 1s linear infinite,fix 1s linear 5s'></div>", 1000);
    expect_u32("motion due: but only while the later one is in effect",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1000);
    moving_close(&m);
    moving_open(&m, MOTION_STYLE
        "@keyframes flat{from{transform:matrix(0,0,0,0,0,0)}to{transform:matrix(1,0,0,1,0,0)}}"
        "</style><div id=fl style='animation:flat 1s linear infinite'></div>", 1000);
    expect_u32("motion due: a transform that cannot be undone, while it runs, now",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1000);
    moving_close(&m);

    // An element out of view whose shadow reaches into it is seen there:
    // a colour animation moving that shadow's currentColor is due.
    moving_open(&m, MOTION_STYLE
        "@keyframes c{to{color:red}}"
        "</style><div style='margin-top:2000px;box-shadow:0 -1900px 0 currentColor;"
        "animation:c 1s linear'>x</div>", 1000);
    expect_u32("motion due: seen by a shadow cast into view",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1000);
    moving_close(&m);

    // Out of view, an animation that only repaints where its element is
    // waits; one that moves it is due all the same, since a later frame
    // may carry it into view (a box sliding in from the left, starting
    // wholly outside).
    moving_open(&m, MOTION_STYLE
        "@keyframes c{to{color:red}}"
        "</style><div style='margin-top:2000px;animation:c 1s linear'>x</div>", 1000);
    expect("motion due: out of view, an animation in place waits",
           flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) == FLOW_NEVER, NULL);
    moving_close(&m);
    moving_open(&m, MOTION_STYLE
        "@keyframes in{from{transform:translateX(-1000px)}to{transform:translateX(1000px)}}"
        "</style><div style='animation:in 1s linear'>x</div>", 1000);
    expect("motion due: out of view, an animation that moves its element is due",
           flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) == 1000, NULL);
    moving_close(&m);

    // A text shadow repaints in place, as far past the element's ink as the
    // farthest offset any frame can take (FMotion.text_reach): out of view
    // it waits; a later frame cast into view is due now, judged by that
    // reach and not by the shadow on show; and an easing that overshoots
    // carries a frame past its keyframes, which the reach counts. The box
    // is 650 to 690 px down, a view of 600 above it.
    static const struct {
        const char *name, *keyframes, *easing, *top;
        bool due;
    } kShadows[] = {
        {"motion due: out of view, a text shadow waits",
         "from{text-shadow:2px 2px 0 red}to{text-shadow:-2px 1px 0 blue}", "linear", "2000px", false},
        {"motion due: a text shadow a later frame casts into view is due",
         "from{text-shadow:0 0 0 red}to{text-shadow:0 -100px 0 red}", "linear", "650px", true},
        {"motion due: a text shadow that falls short of the view waits",
         "from{text-shadow:0 0 0 red}to{text-shadow:0 -40px 0 red}", "linear", "650px", false},
        {"motion due: a text shadow an overshooting easing carries into view is due",
         "from{text-shadow:0 0 0 red}to{text-shadow:0 -40px 0 red}", "cubic-bezier(0.2,0,0.2,2)",
         "650px", true},
    };
    for (size_t i = 0; i < sizeof(kShadows) / sizeof(*kShadows); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "@keyframes ts{%s}</style>"
                 "<div style='margin-top:%s;animation:ts 1s %s'>x</div>",
                 kShadows[i].keyframes, kShadows[i].top, kShadows[i].easing);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kShadows[i].name, due == (kShadows[i].due ? 1000 : FLOW_NEVER), detail);
        moving_close(&m);
    }
    // The reach itself (FMotion.text_reach): the farthest offset among the
    // keyframes, the value underneath and 0, plus their span times the most
    // any easing there overshoots, either side (f_easing_overshoot).
    static const struct {
        const char *name, *own, *keyframes, *easing;
        int32_t reach;
    } kReach[] = {
        {"text reach: the farthest keyframe's offset", "",
         "from{text-shadow:3px -7px 0 red}to{text-shadow:-5px 2px 0 red}", "linear", 7},
        {"text reach: an offset across counts as one down", "",
         "from{text-shadow:-12px 1px 0 red}to{text-shadow:0 0 0 red}", "linear", 12},
        {"text reach: the value underneath is a keyframe's", "text-shadow:0 30px 0 red;",
         "to{text-shadow:0 -4px 0 red}", "linear", 30},
        {"text reach: every shadow in the list", "",
         "from{text-shadow:1px 1px 0 red,0 -9px 0 blue}to{text-shadow:0 0 0 red}", "linear", 9},
        {"text reach: an easing past 1 carries the span past it", "",
         "from{text-shadow:0 0 0 red}to{text-shadow:0 -40px 0 red}", "cubic-bezier(0.2,0,0.2,2)", 80},
        {"text reach: an easing below 0 carries it the other way", "",
         "from{text-shadow:0 0 0 red}to{text-shadow:0 -40px 0 red}", "cubic-bezier(0.2,-0.5,0.2,1)", 60},
        {"text reach: a keyframe's own easing counts", "",
         "from{text-shadow:0 0 0 red;animation-timing-function:cubic-bezier(0.2,0,0.2,1.5)}"
         "to{text-shadow:0 -40px 0 red}", "linear", 60},
        {"text reach: none for an element that sets no text shadow", "",
         "to{color:red}", "linear", 0},
        {"text reach: an overshoot over an overshoot is counted", "",
         "from{text-shadow:0 0 0 red}to{text-shadow:0 -100px 0 red}}"
         "@keyframes back{to{text-shadow:0 0 0 red}",
         "cubic-bezier(0.2,0,0.2,2),back 1s cubic-bezier(0.2,-1,0.2,1)", 500},
        {"text reach: held to what an offset can say", "",
         "from{text-shadow:0 0 0 red}to{text-shadow:0 -40000000px 0 red}", "cubic-bezier(0.2,0,0.2,100)",
         INT32_MAX},
    };
    for (size_t i = 0; i < sizeof(kReach) / sizeof(*kReach); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "@keyframes ts{%s}</style>"
                 "<div id=r style='%sanimation:ts 1s %s'>x</div>",
                 kReach[i].keyframes, kReach[i].own, kReach[i].easing);
        moving_open(&m, html, 1000);
        FStyles *styles = f_style_build(m.p.doc, m.p.page, &m.p.env);
        const FStyled *st = styles != NULL ? f_style_of(styles, motion_find(m.p.doc->document, "r")) : NULL;
        int32_t got = st != NULL && st->motion != NULL ? st->motion->text_reach : -1;
        f_style_free(styles);
        char detail[48];
        snprintf(detail, sizeof(detail), "got %d, want %d", got, kReach[i].reach);
        expect(kReach[i].name, got == kReach[i].reach, detail);
        moving_close(&m);
    }
    static const struct {
        const char *name;
        flow_easing_t e;
        double over;
    } kOvershoot[] = {
        {"overshoot: none for linear", {FLOW_EASE_LINEAR, {0}, 0, FLOW_JUMP_END, false}, 0},
        {"overshoot: none for steps", {FLOW_EASE_STEPS, {0.5, 3, 0.5, -2}, 2, FLOW_JUMP_END, false}, 0},
        {"overshoot: none for a curve that stays inside", {FLOW_EASE_CUBIC, {0.25, 0.1, 0.25, 1}, 0, FLOW_JUMP_END, true}, 0},
        {"overshoot: past 1 by the higher control point", {FLOW_EASE_CUBIC, {0.1, 1.2, 0.2, 1.6}, 0, FLOW_JUMP_END, false}, 0.6},
        {"overshoot: below 0 by the lower control point", {FLOW_EASE_CUBIC, {0.1, -0.7, 0.2, -0.2}, 0, FLOW_JUMP_END, false}, 0.7},
        {"overshoot: the farther of the two sides", {FLOW_EASE_CUBIC, {0.1, -0.3, 0.2, 1.2}, 0, FLOW_JUMP_END, false}, 0.3},
    };
    for (size_t i = 0; i < sizeof(kOvershoot) / sizeof(*kOvershoot); i++) {
        double got = f_easing_overshoot(&kOvershoot[i].e);
        char detail[48];
        snprintf(detail, sizeof(detail), "got %g", got);
        expect(kOvershoot[i].name, fabs(got - kOvershoot[i].over) < 1e-9, detail);
    }
    // A reach past what a quarter of an offset can say still counts: an
    // element 700 million px down cast into view by a frame's overshoot.
    moving_open(&m, MOTION_STYLE
        "@keyframes ts{from{text-shadow:0 0 0 red}to{text-shadow:0 -40000000px 0 red}}</style>"
        "<div style='margin-top:700000000px;animation:ts 1s cubic-bezier(0.2,0,0.2,100)'>x</div>", 1000);
    {
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect("motion due: a far reach is not cut short", due == 1000, detail);
    }
    moving_close(&m);
    // The reach grows the box on every side: a shadow cast into the view
    // from below it, above it, and either side of it.
    static const struct {
        const char *name, *place, *to;
        os64_gui_rect_t view;
    } kSides[] = {
        {"motion due: a text shadow cast up into view", "margin-top:650px", "0 -100px",
         {0, 0, 800, 600}},
        {"motion due: a text shadow cast down into view", "margin-top:650px", "0 400px",
         {0, 1000, 800, 600}},
        {"motion due: a text shadow cast left into view", "margin-left:900px", "-150px 0",
         {0, 0, 800, 600}},
        {"motion due: a text shadow cast right into view", "", "450px 0", {500, 0, 800, 600}},
    };
    for (size_t i = 0; i < sizeof(kSides) / sizeof(*kSides); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE
                 "@keyframes ts{from{text-shadow:0 0 0 red}to{text-shadow:%s 0 red}}</style>"
                 "<div style='%s;animation:ts 1s linear'>x</div>", kSides[i].to, kSides[i].place);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kSides[i].view, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kSides[i].name, due == 1000, detail);
        moving_close(&m);
    }
    // A box drawn outside the element inherits its text shadow, and is
    // judged by the element's reach: a fixed box just below the view.
    for (int far = 0; far < 2; far++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE
                 "@keyframes ts{from{text-shadow:0 0 0 red}to{text-shadow:0 -%dpx 0 red}}"
                 ".c{animation:ts 1s linear}</style><div class=c style='margin-top:2000px'>"
                 "<p style='position:fixed;top:650px;margin:0'>x</p></div>", far ? 100 : 40);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(far ? "motion due: a fixed box inside takes the text shadow's reach"
                   : "motion due: a fixed box inside waits when the reach falls short",
               due == (far ? 1000 : FLOW_NEVER), detail);
        moving_close(&m);
    }

    // An animation of nothing that can show is due for nothing, however
    // fast its clock runs: an empty rule, `display` alone, what an
    // `!important` winner holds — and opacity a later animation in effect
    // replaces, whose own end is what is due.
    static const struct { const char *name, *css; uint64_t due; } kNothing[] = {
        {"motion due: an empty rule is due for nothing", "@keyframes e{}#n{animation:e 1ms infinite}",
         FLOW_NEVER},
        {"motion due: display alone is due for nothing",
         "@keyframes d{to{display:none}}#n{animation:d 1ms infinite}", FLOW_NEVER},
        {"motion due: what an !important winner holds is due for nothing",
         "@keyframes o{to{opacity:0}}#n{opacity:.5 !important;animation:o 1ms infinite}", FLOW_NEVER},
        {"motion due: an animation a later one replaces is due for nothing",
         "@keyframes o{to{opacity:0}}@keyframes s{from{opacity:.5}to{opacity:.5}}"
         "#n{animation:o 7ms infinite,s 1s linear}", 2000},
    };
    // The root's background, or the body's, is the canvas's, painted over
    // the whole view wherever its box is: either animated in place is due
    // with its box moved out of view.
    moving_open(&m, MOTION_STYLE "@keyframes bg{to{background-color:blue}}"
                "body{transform:translateX(-5000px);animation:bg 1s linear}</style><p>x</p>", 1000);
    expect("motion due: the body's background, the canvas's, is due with the body out of view",
           flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) == 1000, NULL);
    moving_close(&m);
    moving_open(&m, MOTION_STYLE "@keyframes bg{to{background-color:blue}}"
                "html{transform:translateX(-5000px);animation:bg 1s linear}</style><p>x</p>", 1000);
    expect("motion due: and so is the root's",
           flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) == 1000, NULL);
    moving_close(&m);
    for (size_t i = 0; i < sizeof(kNothing) / sizeof(kNothing[0]); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "%s</style><div id=n>x</div>", kNothing[i].css);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "due %llu", (unsigned long long)due);
        expect(kNothing[i].name, due == kNothing[i].due, detail);
        moving_close(&m);
    }

    // An element that makes no box hands its animated colour down: it is
    // seen where what it holds is — a block, or bare text — and not when
    // all it holds is out of view.
    static const char *const kContents[] = {
        "<div class=c><p>x</p></div>", "<div class=c>bare text</div>",
        "<div class=c><p style='margin-top:2000px'>x</p></div>",
    };
    for (int i = 0; i < 3; i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "@keyframes c{to{color:red}}"
                 ".c{display:contents;animation:c 1s linear}</style>%s", kContents[i]);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        expect(i == 0 ? "motion due: display: contents, seen through its block"
               : i == 1 ? "motion due: display: contents, seen through its text"
                        : "motion due: display: contents, all it holds out of view",
               due == (i < 2 ? 1000 : FLOW_NEVER), NULL);
        moving_close(&m);
    }

    // An inline element is seen where any of its pieces is: a later line
    // in view with its first scrolled out, or its last piece after a block
    // it was split round; with every piece out of view, it waits.
    static const struct {
        const char *name, *body;
        int32_t y;
        uint64_t due;
    } kPieces[] = {
        {"motion due: an inline element seen by a later line",
         "<p style='width:60px;margin:0'><span class=c>aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp</span></p>",
         60, 1000},
        {"motion due: an inline element seen by its piece after a block inside it",
         "<span class=c>first<div style='height:500px'></div>last</span>", 520, 1000},
        {"motion due: an inline element with every piece out of view waits",
         "<p style='width:60px;margin:0'><span class=c>aa bb cc dd ee ff gg hh ii jj kk ll mm nn oo pp</span></p>",
         5000, FLOW_NEVER},
    };
    for (size_t i = 0; i < sizeof(kPieces) / sizeof(kPieces[0]); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "@keyframes c{to{color:red}}"
                 ".c{animation:c 1s linear}</style>%s", kPieces[i].body);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, (os64_gui_rect_t){0, kPieces[i].y, 800, 50},
                                       (flow_point_t){0, 0});
        expect(kPieces[i].name, due == kPieces[i].due, NULL);
        moving_close(&m);
    }

    // What an element hands down is drawn by a box inside it that is drawn
    // outside it — fixed, positioned, moved by a transform — and that no
    // ancestor's overflow holds: the element is due where that box is
    // seen, and not when it, too, is out of view.
    static const char *const kOutside[] = {
        "<div class=c style='margin-top:2000px'><p style='position:fixed;top:0'>x</p></div>",
        "<div class=c style='position:relative;margin-top:2000px'>"
        "<p style='position:absolute;top:-2000px'>x</p></div>",
        "<div class=c style='margin-top:2000px'><p style='transform:translateY(-2000px)'>x</p></div>",
        "<div class=c style='position:relative;margin-top:2000px'>"
        "<p style='position:absolute;top:100px'>x</p></div>",
    };
    static const char *const kOutsideNames[] = {
        "motion due: seen through a fixed box inside it",
        "motion due: seen through a positioned box inside it",
        "motion due: seen through a box inside it a transform moves",
        "motion due: not when the positioned box is out of view too",
    };
    for (int i = 0; i < 4; i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "@keyframes c{to{color:red}}"
                 ".c{animation:c 1s linear}</style>%s", kOutside[i]);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kOutsideNames[i], due == (i < 3 ? 1000 : FLOW_NEVER), detail);
        moving_close(&m);
    }
    // And only for what it hands down: an inherited value and a fade reach
    // the box inside it; its own background and border do not — unless
    // the author hands them down by keyword, at every element between.
    static const char *const kFixed = "<p style='position:fixed;top:0'>x</p>";
    static const char *const kTakes = "<p style='position:fixed;top:0;background-color:inherit'>x</p>";
    static const struct {
        const char *name, *keyframes, *between, *box;
        uint64_t due;
    } kHanded[] = {
        {"motion due: seen through a box inside it, a fade", "to{opacity:0}", NULL, kFixed, 1000},
        {"motion due: seen through a box inside it, visibility", "to{visibility:hidden}", NULL, kFixed, 1000},
        {"motion due: not for its own background", "to{background-color:red}", NULL, kFixed, FLOW_NEVER},
        {"motion due: nor for its own border", "to{border-top-color:red}", NULL, kFixed, FLOW_NEVER},
        {"motion due: but for a background the box inherits", "to{background-color:red}", NULL, kTakes, 1000},
        {"motion due: and through everything between that inherits it",
         "to{background-color:red}", "background-color:inherit", kTakes, 1000},
        {"motion due: not through an element between that does not",
         "to{background-color:red}", "color:blue", kTakes, FLOW_NEVER},
        {"motion due: unset on a background is no inheritance", "to{background-color:red}", NULL,
         "<p style='position:fixed;top:0;background-color:unset'>x</p>", FLOW_NEVER},
        {"motion due: nor an inherit a later declaration replaces", "to{background-color:red}", NULL,
         "<p style='position:fixed;top:0;background-color:inherit;background-color:blue'>x</p>", FLOW_NEVER},
    };
    for (size_t i = 0; i < sizeof(kHanded) / sizeof(kHanded[0]); i++) {
        // `between`, when given, is the style of an element between them.
        char inner[256], html[768];
        if (kHanded[i].between != NULL)
            snprintf(inner, sizeof(inner), "<div style='%s'>%s</div>", kHanded[i].between, kHanded[i].box);
        else
            snprintf(inner, sizeof(inner), "%s", kHanded[i].box);
        snprintf(html, sizeof(html), MOTION_STYLE "@keyframes c{%s}"
                 ".c{animation:c 1s linear;border-top:2px solid}</style>"
                 "<div class=c style='margin-top:2000px'>%s</div>", kHanded[i].keyframes, inner);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kHanded[i].name, due == kHanded[i].due, detail);
        moving_close(&m);
    }
    // An `inherit` in a keyframe takes as one in a declaration does; and a
    // background cut to the text inside it is drawn through that text,
    // wherever a box moves it — but not through a box that leaves the clip,
    // fixed, or inside an absolute box.
    static const struct {
        const char *name, *html;
        uint64_t due;
    } kThrough[] = {
        {"motion due: through an element between whose keyframes inherit it",
         "@keyframes hold{from{background-color:inherit}to{background-color:inherit}}</style>"
         "<div class=c style='margin-top:2000px'><div style='animation:hold 10s linear'>"
         "<p style='position:fixed;top:0;background-color:inherit'>x</p></div></div>", 1000},
        {"motion due: a background cut to the text a moved box holds",
         "</style><div class=c style='margin-top:2000px;background-clip:text'>"
         "<div style='position:relative;top:-2000px'>x</div></div>", 1000},
        {"motion due: not through a fixed box the clip does not hold",
         "</style><div class=c style='margin-top:2000px;background-clip:text'>"
         "<p style='position:fixed;top:0'>x</p></div>", FLOW_NEVER},
        {"motion due: nor through one inside an absolute box",
         "</style><div class=c style='margin-top:2000px;background-clip:text'><div style='position:absolute'>"
         "<div style='position:relative;top:-2000px'>x</div></div></div>", FLOW_NEVER},
    };
    for (size_t i = 0; i < sizeof(kThrough) / sizeof(kThrough[0]); i++) {
        char html[768];
        snprintf(html, sizeof(html), MOTION_STYLE "@keyframes c{to{background-color:red}}"
                 ".c{animation:c 1s linear}%s", kThrough[i].html);
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[48];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kThrough[i].name, due == kThrough[i].due, detail);
        moving_close(&m);
    }
}

// ── The allocation sweep ────────────────────────────────────────────────

// What the animated page's elements look like: the values its animations
// move, and every box's place.
static char *motion_values(const Moving *m)
{
    static const char *const kIds[] = {"a", "b", "in"};
    char *text = malloc(4096);
    size_t at = 0;
    for (size_t i = 0; i < sizeof(kIds) / sizeof(kIds[0]); i++) {
        const flow_style_t *s = moving_style(m, kIds[i]);
        double mx[16] = {0};
        if (s == NULL || !moving_matrix(m, kIds[i], mx)) {
            at += (size_t)snprintf(text + at, 4096 - at, "%s none\n", kIds[i]);
            continue;
        }
        at += (size_t)snprintf(text + at, 4096 - at,
                               "%s color %08x bg %d %08x margin %d opacity %u matrix %.6f %.6f %.6f %.6f %.3f\n",
                               kIds[i], s->color, s->has_background, s->background, s->margin[3].value,
                               s->opacity, mx[0], mx[1], mx[4], mx[5], mx[12]);
    }
    int64_t need = flow_dump(m->tree, NULL, 0);
    text = realloc(text, at + (size_t)need + 1);
    flow_dump(m->tree, text + at, (size_t)need + 1);
    return text;
}

// Every allocation of an animated page's layout fails in turn — the
// records kept from an earlier layout, and then the layout that makes
// them — and nothing leaks. A layout that comes back whole is the one an
// uninterrupted run makes, and the timeline comes through each failure
// with its start times: the layout after it is that one too. `single`
// fails that one allocation alone, every other succeeding; otherwise it
// and every one after it fail. `ticks`: laid out for a face that ticks,
// whose layout resolves every keyframe of what ticks and lists its boxes.
static void motion_sweep_in(bool single, bool ticks)
{
    static const char kHtml[] = MOTION_STYLE
        "@keyframes mv{from{transform:translateX(0);opacity:0}"
        "to{transform:translateX(100px) rotate(30deg);opacity:1}}"
        "@keyframes col{50%{color:red;margin-left:var(--m)}to{background-color:blue}}"
        "#a{--m:30px;animation:mv 1s linear infinite,col 2s steps(3) both}"
        "#b{animation:mv 700ms ease-in-out 200ms 3 alternate paused}"
        "#in{display:inline-block;animation:col 1s 100ms backwards}"
        "</style><div id=a>moving</div><p id=b>held <span id=in>inside</span></p>"
        "<div id=t style='animation:mv 900ms linear infinite'></div>";
    Moving m;
    moving_open_for(&m, kHtml, 0, ticks);
    moving_again(&m, 500);
    char *whole = motion_values(&m);
    fail_single = single;
    flow_free(m.tree);
    m.tree = NULL;
    size_t base_live = live;
    allocations = 0;
    m.tree = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
    size_t count = allocations;
    flow_free(m.tree);
    m.tree = NULL;
    size_t tried = 0;
    bool failed = false;
    for (size_t at = 1; at <= count && !failed; at++) {
        allocations = 0;
        fail_at = at;
        m.tree = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
        fail_at = 0;
        tried++;
        if (m.tree != NULL && !flow_incomplete(m.tree)) {
            char *got = motion_values(&m);
            bool same = strcmp(got, whole) == 0;
            expect("motion sweep: a layout that comes back whole is the page", same, NULL);
            failed |= !same;
            free(got);
        }
        flow_free(m.tree);
        m.tree = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
        char *again = m.tree != NULL ? motion_values(&m) : NULL;
        bool kept = again != NULL && strcmp(again, whole) == 0;
        expect("motion sweep: the timeline comes through a failure with its start times", kept, NULL);
        failed |= !kept;
        free(again);
        flow_free(m.tree);
        m.tree = NULL;
        expect("motion sweep: nothing leaked", live == base_live, NULL);
        failed |= live != base_live;
    }
    // The records made: a fresh timeline at every failure.
    flow_timeline_free(m.tl);
    m.tl = NULL;
    m.p.env.now_ms = 0;
    base_live = live;
    size_t made = 0;
    for (size_t at = 1; !failed; at++) {
        m.tl = flow_timeline_new(m.p.doc);
        m.p.env.timeline = m.tl;
        allocations = 0;
        fail_at = at;
        m.tree = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
        fail_at = 0;
        // Fewer allocations than the one set to fail: none failed.
        bool reached = allocations >= at;
        flow_free(m.tree);
        m.tree = NULL;
        flow_timeline_free(m.tl);
        m.tl = NULL;
        m.p.env.timeline = NULL;
        made++;
        expect("motion sweep: a first layout that fails leaks nothing", live == base_live, NULL);
        failed |= live != base_live;
        if (!reached)
            break;
    }
    fail_single = false;
    printf("libflow motion sweep: each of %zu allocations failed in turn%s (%zu with the records made)%s, "
           "nothing leaked\n", tried, single ? " alone" : ", and every one after it", made,
           ticks ? ", ticking" : "");
    free(whole);
    moving_close(&m);
}

// A layout that fails part way — each of its allocations failing in turn,
// and every one after it — takes what it built with it: the tree still
// shown keeps running what the failed layout paused, no record it made is
// kept, and the next layout finds the timeline as the shown tree left it
// (x, running since 1000, is 4 s into its 10 at 5000; paused at 3000 and
// resumed, it would be 2).
static void motion_sweep_failed(void)
{
    static const char kFailedPage[] = MOTION_STYLE
        "@keyframes k{from{opacity:0}to{opacity:1}}"
        "#x{animation:k 10s linear}"
        "@media (prefers-reduced-motion:reduce){#x{animation-play-state:paused}#y{animation:k 10s linear}}"
        "</style><div id=x>x</div><div id=y>y</div>";
    static const os64_gui_rect_t kView = {0, 0, 800, 600};
    size_t tried = 0;
    bool failed = false;
    for (size_t at = 1; !failed; at++) {
        Moving m;
        moving_open(&m, kFailedPage, 1000);
        garb_sheet_in_t in[16];
        for (int32_t k = 0; k < m.p.n; k++)
            in[k] = (garb_sheet_in_t){.sheet = &m.p.sheets[k]};
        garb_cascade_t *calm =
            garb_cascade(in, m.p.n, m.p.doc, (garb_env_t){.width = 800, .height = 600, .reduced_motion = true});
        m.p.env.cascade = calm;
        m.p.env.now_ms = 3000;
        allocations = 0;
        fail_at = at;
        flow_tree_t *staged = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
        fail_at = 0;
        bool reached = allocations >= at;
        m.p.env.cascade = m.p.cascade;
        if (staged == NULL && reached) {
            tried++;
            bool due = m.tree != NULL && flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) != FLOW_NEVER;
            bool none = f_timeline_records(m.tl) == 1;
            moving_again(&m, 5000);
            const flow_style_t *xs = moving_style(&m, "x");
            bool ran = xs != NULL && xs->opacity == 400;
            expect("motion sweep: a failed layout leaves the shown tree's animation running", due, NULL);
            expect("motion sweep: and keeps no record it made", none, NULL);
            expect("motion sweep: and the next layout finds the timeline as the shown tree left it", ran, NULL);
            failed = !due || !none || !ran;
        }
        flow_free(staged);
        garb_cascade_free(calm);
        moving_close(&m);
        if (!reached)
            break;
    }
    printf("libflow motion sweep: %zu failed layouts left the timeline shown as it was\n", tried);
}

// ── The predictor against the evaluator, at random ─────────────────────
//
// f_motion_next says when an animation's value next changes; f_motion_style
// says what the value is. The two are written separately, so each case
// here is an animation made at random — one to four keyframes at random
// offsets, the element's own value standing in for any missing at 0 or 1;
// linear, cubic (overshooting, and at a double's extremes) and steps
// easings, the animation's and a keyframe's own; every direction and fill,
// fractional and whole iterations, delays either side of zero, a zero
// duration — over a continuous value (opacity, a colour), a discrete one
// (text-align) and visibility, values from a small pool so that some stand
// still. The evaluator, asked at every millisecond, is the oracle: each
// change must fall on a millisecond the predictor named (one layout early
// is allowed, late never), and a value that never changes may not be
// asked for more than the animation's edges.

typedef struct {
    uint64_t s;
} FuzzRng;

static double fuzz_unit(FuzzRng *r)
{
    r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(r->s >> 11) * 0x1p-53;
}

static int fuzz_below(FuzzRng *r, int n)
{
    int k = (int)(fuzz_unit(r) * n);
    return k < n ? k : n - 1;
}

static flow_easing_t fuzz_easing(FuzzRng *r)
{
    // Ten, the last two the domain's limit, which a page's 1e308 is held
    // at: kFuzzRegressions' seeds were found against a table this long.
    static const double kY[] = {-1, -0.5, 0, 0.25, 0.5, 1, 1.5, 2, GARB_EASING_Y_MAX,
                                -GARB_EASING_Y_MAX};
    flow_easing_t e;
    memset(&e, 0, sizeof(e));
    switch (fuzz_below(r, 4)) {
    case 0:
        e.kind = FLOW_EASE_LINEAR;
        break;
    case 1:
        e.kind = FLOW_EASE_STEPS;
        e.jump = (flow_jump_t)fuzz_below(r, 4);
        e.steps = 1 + fuzz_below(r, 5);
        if (e.jump == FLOW_JUMP_NONE && e.steps < 2)
            e.steps = 2;
        break;
    default:
        e.kind = FLOW_EASE_CUBIC;
        e.p[0] = fuzz_unit(r);
        e.p[2] = fuzz_unit(r);
        e.p[1] = kY[fuzz_below(r, 10)];
        e.p[3] = kY[fuzz_below(r, 10)];
        break;
    }
    return e;
}

enum { FUZZ_OPACITY, FUZZ_COLOR, FUZZ_ALIGN, FUZZ_VISIBILITY, FUZZ_KINDS };

static void fuzz_set(flow_style_t *s, int kind, int v)
{
    static const uint16_t kOpacity[] = {0, 250, 1000};
    static const uint32_t kColor[] = {0x000000u, 0xff0000u, 0x00ff00u};
    static const flow_text_align_t kAlign[] = {FLOW_ALIGN_LEFT, FLOW_ALIGN_RIGHT, FLOW_ALIGN_CENTER};
    static const flow_visibility_t kVis[] = {FLOW_VISIBLE, FLOW_HIDDEN, FLOW_COLLAPSE};
    switch (kind) {
    case FUZZ_OPACITY: s->opacity = kOpacity[v]; break;
    case FUZZ_COLOR: s->color = kColor[v]; break;
    case FUZZ_ALIGN: s->text_align = kAlign[v]; break;
    default: s->visibility = kVis[v]; break;
    }
}

static uint32_t fuzz_get(const flow_style_t *s, int kind)
{
    switch (kind) {
    case FUZZ_OPACITY: return s->opacity;
    case FUZZ_COLOR: return s->color;
    case FUZZ_ALIGN: return (uint32_t)s->text_align;
    default: return (uint32_t)s->visibility;
    }
}

enum { FUZZ_FRAMES = 5 };

// One animation over property `kind` at random, named `name`, into `an`,
// its keyframes into `frames` made from `hands` — one to five, at offsets
// that may repeat, several at 1 among them: true when every one holds the
// element's own value and keyframes stand at 0 and at 1, so that every
// interval lies between two equal keyframes of its own and the animation,
// whatever else it does, stands still where the schedule may see it.
static bool fuzz_animation(FuzzRng *r, int kind, const flow_style_t *base, const char *name,
                           FAnimation *an, FKeyframe *frames, Hand *hands)
{
    static const garb_prop_t kProp[] = {GARB_OPACITY, GARB_COLOR, GARB_TEXT_ALIGN, GARB_VISIBILITY};
    static const double kIterations[] = {0.5, 1, 1.5, 2, 2.75, 3, INFINITY};
    static const double kOffsets[] = {0, 0.2, 0.4, 0.5, 0.75, 1};
    memset(an, 0, sizeof(*an));
    an->timing.name = (flow_name_t){name, 1};
    an->timing.duration_ms = fuzz_below(r, 20) == 0 ? 0 : 100 + fuzz_below(r, 900);
    an->timing.delay_ms = (fuzz_below(r, 1600) - 800) + (fuzz_below(r, 2) ? 0.5 : 0);
    an->timing.iterations = kIterations[fuzz_below(r, 7)];
    an->timing.direction = (flow_direction_t)fuzz_below(r, 4);
    an->timing.fill = (flow_fill_t)fuzz_below(r, 4);
    an->timing.easing = fuzz_easing(r);
    int nk = 1 + fuzz_below(r, FUZZ_FRAMES);
    double offsets[FUZZ_FRAMES];
    for (int k = 0; k < nk; k++) {
        double o = kOffsets[fuzz_below(r, 6)];
        int j = k;
        for (; j > 0 && offsets[j - 1] > o; j--)
            offsets[j] = offsets[j - 1];
        offsets[j] = o;
    }
    bool same = offsets[0] == 0 && offsets[nk - 1] == 1;
    for (int k = 0; k < nk; k++) {
        FKeyframe *f = &frames[k];
        Hand *h = &hands[k];
        h->r.style = *base;
        fuzz_set(&h->r.style, kind, fuzz_below(r, 3));
        hand_frame(f, h, kProp[kind], offsets[k], fuzz_get(&h->r.style, kind));
        same &= fuzz_get(&h->r.style, kind) == fuzz_get(base, kind);
        if (fuzz_below(r, 3) == 0) {
            f->has_easing = true;
            f->easing = fuzz_easing(r);
        }
    }
    f_props_add(&an->props, kProp[kind]);
    an->frames = frames;
    an->nframes = nk;
    return same;
}

// The layouts an animation that stands still may still ask for within
// `horizon`: each phase's end, and each keyframe's edge in each iteration
// the horizon holds.
static int fuzz_edges(const FAnimation *an, int horizon)
{
    double dur = an->timing.duration_ms;
    double iterations = dur > 0 ? fmin(an->timing.iterations, horizon / dur + 1) : 1;
    return 4 + ((int)ceil(iterations) + 1) * (an->nframes + 2) * 2;
}

// The seeds of every case the schedule has failed, run first and every
// time: each holds a rounding the schedule must place as the value does —
// an edge a hair behind the millisecond the progress stands on, a curve
// flat where it meets its level, a progress an ulp short of a keyframe —
// and changes nothing shows yet, under an animation resting on the value
// that will show them later: a crossing whose x rounds to the end the
// progress runs back from, a change just past the instant the progress
// stands on.
static const uint64_t kFuzzRegressions[] = {
    8590968431883392784ULL,  6633983532214220214ULL,  7087835111071699824ULL,
    8040739818910956811ULL,  13009149457079927089ULL, 14972483720120255669ULL,
    14583056590173832281ULL, 8411317154603802884ULL,  8985172716612446494ULL,
    9795968407517579881ULL,  9496088273930796795ULL,  17289734992611070855ULL,
};

typedef struct {
    const os64_html_node_t *x;
    const os64_html_document_t *doc;
    int failed;
    long changes, still, pairs;
} FuzzRun;

enum { FUZZ_HORIZON = 6000 };

// The case `r` makes from where it stands: the property's kind, the base,
// and one animation of it or two — a second a third of the time, the
// later replacing the earlier wherever it is in effect. True when every
// keyframe holds the base's value.
static bool fuzz_case(FuzzRng *r, int *kind, flow_style_t *base, FAnimation an[2],
                      FKeyframe frames[2][FUZZ_FRAMES], Hand hands[2][FUZZ_FRAMES], int *nanims)
{
    *kind = fuzz_below(r, FUZZ_KINDS);
    memset(base, 0, sizeof(*base));
    base->opacity = 1000;
    fuzz_set(base, *kind, fuzz_below(r, 3));
    *nanims = fuzz_below(r, 3) == 0 ? 2 : 1;
    bool still = true;
    for (int a = 0; a < *nanims; a++)
        still &= fuzz_animation(r, *kind, base, a == 0 ? "f" : "g", &an[a], frames[a], hands[a]);
    return still;
}

// An animation moved to the time domain's edge (values.h), its edges still
// inside the horizon: begun the longest time ago, repeating for ever or as
// many times as the domain allows; or an iteration as long as the domain
// allows, ending where its delay put it. The clock there works on times a
// double resolves to a tenth of a microsecond.
static void fuzz_edge(FuzzRng *r, FAnimation *an)
{
    double behind = an->timing.delay_ms + 800;
    an->timing.delay_ms = -GARB_TIME_MS_MAX + behind;
    int k = fuzz_below(r, 3);
    if (k == 2 && an->timing.duration_ms > 0)
        an->timing.duration_ms = GARB_TIME_MS_MAX;
    else
        an->timing.iterations = k == 1 ? GARB_ITERATIONS_MAX : INFINITY;
}

// One case, the animation `r` makes from where it stands, at the time
// domain's edge when `edge` says (fuzz_edge).
static void motion_fuzz_one(FuzzRun *f, FuzzRng *r, long c, bool edge)
{
    enum { kHorizon = FUZZ_HORIZON };
    static uint32_t values[kHorizon + 1];
    static bool seen[kHorizon + 1];
    const os64_html_node_t *x = f->x;
    const os64_html_document_t *doc = f->doc;
    uint64_t seed = r->s;
    int kind, nanims;
    flow_style_t base;
    FAnimation an[2];
    FKeyframe frames[2][FUZZ_FRAMES];
    static Hand hands[2][FUZZ_FRAMES];
    Hand *const by[2] = {hands[0], hands[1]};
    bool still = fuzz_case(r, &kind, &base, an, frames, hands, &nanims);
    for (int a = 0; edge && a < nanims; a++)
        fuzz_edge(r, &an[a]);
    f->pairs += nanims == 2;
    FMotion m;
    memset(&m, 0, sizeof(m));
    m.base = base;
    m.anims = an;
    m.nanims = nanims;
    bool ok;
    flow_timeline_t *tl = motion_meet(doc, x, &m, 0, &ok);
    int horizon = 200;
    for (int a = 0; a < nanims; a++) {
        double end = an[a].timing.delay_ms + an[a].timing.duration_ms * an[a].timing.iterations + 200;
        int h = !(end < kHorizon) ? kHorizon : end < 200 ? 200 : (int)end;
        horizon = h > horizon ? h : horizon;
    }
    for (int t = 0; t <= horizon; t++) {
        flow_style_t out = m.base;
        (void)f_motion_style(&m, t, &out, NULL, hand_resolved, (void *)by, NULL, false);
        values[t] = fuzz_get(&out, kind);
        seen[t] = false;
    }
    seen[0] = true;
    int layouts = 0;
    for (uint64_t now = 0; now <= (uint64_t)horizon;) {
        double d = layout_next(&m, (double)now, by);
        if (!(d < INFINITY))
            break;
        uint64_t due = d <= (double)now ? now : (uint64_t)ceil(d);
        now = due > now ? due : now + 1;
        if (now <= (uint64_t)horizon)
            seen[now] = true;
        layouts++;
    }
    int missed = -1;
    for (int t = 1; t <= horizon; t++) {
        f->changes += values[t] != values[t - 1];
        if (values[t] != values[t - 1] && !seen[t] && missed < 0)
            missed = t;
    }
    int edges = 0;
    for (int a = 0; a < nanims; a++)
        edges += fuzz_edges(&an[a], horizon);
    bool idle = !still || layouts <= edges;
    f->still += still;
    if (!ok || missed >= 0 || !idle) {
        char detail[256];
        snprintf(detail, sizeof(detail), "%scase %ld (seed %llu): kind %d, %d animation%s; %s at %d ms",
                 edge ? "edge " : "", c, (unsigned long long)seed, kind, nanims, nanims > 1 ? "s" : "",
                 !ok ? "no record" : missed >= 0 ? "a change not laid out" : "a still value laid out",
                 missed >= 0 ? missed : layouts);
        expect("motion fuzz: the predictor names every change the evaluator makes", false, detail);
        f->failed++;
        // The whole case, so a failure can be read without a debugger.
        fprintf(stderr, "  base %u\n", fuzz_get(&base, kind));
        for (int a = 0; a < nanims; a++) {
            const flow_animation_t *tm = &an[a].timing;
            const flow_easing_t *ae = &tm->easing;
            fprintf(stderr, "  %s: dur %g delay %g iter %g dir %d fill %d easing %d [%g %g %g %g] "
                    "steps %d jump %d\n", tm->name.text, tm->duration_ms, tm->delay_ms, tm->iterations,
                    tm->direction, tm->fill, ae->kind, ae->p[0], ae->p[1], ae->p[2], ae->p[3], ae->steps,
                    ae->jump);
            for (int j = 0; j < an[a].nframes; j++) {
                const FKeyframe *f = &an[a].frames[j];
                const flow_easing_t *fe = &f->easing;
                fprintf(stderr, "    frame %g: %u", f->offset, fuzz_get(&hands[a][j].r.style, kind));
                if (f->has_easing)
                    fprintf(stderr, " easing %d [%g %g %g %g] steps %d jump %d", fe->kind, fe->p[0],
                            fe->p[1], fe->p[2], fe->p[3], fe->steps, fe->jump);
                fputc('\n', stderr);
            }
        }
        int at = missed >= 0 ? missed : 0;
        for (uint64_t now = 0, prev = 0; now <= (uint64_t)at;) {
            double d = layout_next(&m, (double)now, by);
            uint64_t due = !(d < INFINITY) ? UINT64_MAX : d <= (double)now ? now : (uint64_t)ceil(d);
            prev = now;
            now = due == UINT64_MAX ? UINT64_MAX : due > now ? due : now + 1;
            if (now > (uint64_t)at) {
                fprintf(stderr, "  last layout before it %llu ms: next %.9f\n", (unsigned long long)prev, d);
                break;
            }
        }
        for (int t = at - 3 > 0 ? at - 3 : 0; t <= at + 2 && t <= horizon; t++)
            fprintf(stderr, "  %d ms: %u%s\n", t, values[t], seen[t] ? " laid out" : "");
    }
    flow_timeline_free(tl);
}

static void motion_fuzz_cases(void)
{
    // MOTION_FUZZ_CASES and MOTION_FUZZ_SEED run it longer, or elsewhere.
    const char *env_cases = getenv("MOTION_FUZZ_CASES"), *env_seed = getenv("MOTION_FUZZ_SEED");
    long cases = env_cases != NULL ? strtol(env_cases, NULL, 10) : 3000;
    FuzzRng r = {env_seed != NULL ? strtoull(env_seed, NULL, 10) : 20261014};
    os64_html_document_t *doc = parse("<!doctype html><p id=x>x</p>");
    FuzzRun f = {motion_find(doc->document, "x"), doc, 0, 0, 0, 0};
    for (size_t i = 0; i < sizeof(kFuzzRegressions) / sizeof(kFuzzRegressions[0]); i++) {
        FuzzRng one = {kFuzzRegressions[i]};
        motion_fuzz_one(&f, &one, -1 - (long)i, false);
    }
    for (long c = 0; c < cases && f.failed < 5; c++)
        motion_fuzz_one(&f, &r, c, false);
    // A quarter as many again at the time domain's edge, from a stream of
    // their own.
    FuzzRng er = {r.s ^ 0x5DEECE66DULL};
    for (long c = 0; c < cases / 4 && f.failed < 5; c++)
        motion_fuzz_one(&f, &er, c, true);
    if (f.failed == 0)
        expect("motion fuzz: the predictor names every change the evaluator makes", true, NULL);
    printf("libflow motion fuzz: %ld cases, %ld at the domain's edge, and %zu regressions (%ld with "
           "two animations), %ld changes, %ld standing still\n", cases, cases / 4,
           sizeof(kFuzzRegressions) / sizeof(kFuzzRegressions[0]), f.pairs, f.changes, f.still);
    os64_html_document_free(doc);
}

// `s`'s value of the fuzzer's `kind` made the transform list of its index
// among the three the fuzzer writes.
static void fuzz_as_list(flow_style_t *s, int kind, const flow_style_t *const lists[3])
{
    for (int i = 0; i < 3; i++) {
        flow_style_t one;
        memset(&one, 0, sizeof(one));
        fuzz_set(&one, kind, i);
        if (fuzz_get(&one, kind) == fuzz_get(s, kind)) {
            s->transform = lists[i]->transform;
            s->ntransform = lists[i]->ntransform;
        }
    }
}

// A case the fuzzer found the schedule late for — seed
// 15973564121497552185, a discrete text-align changing at 76 ms, two
// animations of it — made a transform list's: each of the three values a
// list that mixes only discretely (two translations, then a singular
// matrix no decomposition undoes). The schedule compares the keyframes'
// declarations, never the lists they make; the change is due no later
// than its millisecond.
static void motion_list_case_cases(void)
{
    Moving lm;
    moving_open(&lm, MOTION_STYLE "</style>"
                "<div id=v0 style='transform:translateX(0px) translateX(0px) matrix(0,0,0,0,0,0)'></div>"
                "<div id=v1 style='transform:translateX(0px) translateX(0px) matrix(0,0,0,0,1,0)'></div>"
                "<div id=v2 style='transform:translateX(0px) translateX(0px) matrix(0,0,0,0,2,0)'></div>", 0);
    const flow_style_t *lists[3] = {moving_style(&lm, "v0"), moving_style(&lm, "v1"), moving_style(&lm, "v2")};
    FuzzRng r = {15973564121497552185ULL};
    int kind, nanims;
    flow_style_t base;
    FAnimation an[2];
    FKeyframe frames[2][FUZZ_FRAMES];
    static Hand hands[2][FUZZ_FRAMES];
    (void)fuzz_case(&r, &kind, &base, an, frames, hands, &nanims);
    bool shaped = kind == FUZZ_ALIGN && nanims == 2;
    for (int i = 0; i < 3; i++)
        shaped &= lists[i] != NULL && lists[i]->ntransform == 3;
    expect("motion: the list case is the fuzzer's, its values three lists", shaped, NULL);
    if (!shaped) {
        moving_close(&lm);
        return;
    }
    fuzz_as_list(&base, kind, lists);
    for (int a = 0; a < nanims; a++) {
        memset(&an[a].props, 0, sizeof(an[a].props));
        f_props_add(&an[a].props, GARB_TRANSFORM);
        for (int k = 0; k < an[a].nframes; k++) {
            fuzz_as_list(&hands[a][k].r.style, kind, lists);
            memset(&frames[a][k].sets, 0, sizeof(frames[a][k].sets));
            f_props_add(&frames[a][k].sets, GARB_TRANSFORM);
            hands[a][k].set.prop = GARB_TRANSFORM;
        }
    }
    FMotion m;
    memset(&m, 0, sizeof(m));
    m.base = base;
    m.anims = an;
    m.nanims = nanims;
    bool ok;
    flow_timeline_t *tl = motion_meet(lm.p.doc, motion_find(lm.p.doc->document, "v0"), &m, 0, &ok);
    Hand *const by[2] = {hands[0], hands[1]};
    double due = layout_next(&m, 75, by);
    char detail[64];
    snprintf(detail, sizeof(detail), "due %.17g", due);
    expect("motion due: the fuzzer's case made of transform lists is due by its change",
           ok && due >= 75 && due <= 76, detail);
    flow_timeline_free(tl);
    moving_close(&lm);
}

// A page of `divs` elements, each a 64-function transform (libgarb's most)
// out of the lists' budget, then one animating a 64-function list — and,
// when `two`, a one-function list before it, written first.
static char *dropped_page(int divs, bool two, int filler)
{
    size_t cap = (size_t)divs * 24 + 64 * 17 * 3 + 1024, at = 0;
    char *html = malloc(cap);
    at += (size_t)snprintf(html + at, cap - at, MOTION_STYLE
                           "@keyframes short{from{transform:translateX(0px)}to{transform:translateX(10px)}}"
                           ".s{transform:");
    for (int i = 0; i < filler; i++)
        at += (size_t)snprintf(html + at, cap - at, "translateX(1px) ");
    at += (size_t)snprintf(html + at, cap - at, "}@keyframes long{from{transform:");
    for (int i = 0; i < 64; i++)
        at += (size_t)snprintf(html + at, cap - at, "translateX(0px) ");
    at += (size_t)snprintf(html + at, cap - at, "}to{transform:");
    for (int i = 0; i < 64; i++)
        at += (size_t)snprintf(html + at, cap - at, "translateX(1px) ");
    at += (size_t)snprintf(html + at, cap - at, "}}</style>");
    for (int i = 0; i < divs; i++)
        at += (size_t)snprintf(html + at, cap - at, "<div class=s></div>");
    (void)snprintf(html + at, cap - at, "<div id=long style='animation:%slong 1s linear'></div>",
                   two ? "short 1s linear," : "");
    return html;
}

// What styling `divs` of them leaves of the lists' budget.
static size_t dropped_left(int divs, bool two, int filler)
{
    char *html = dropped_page(divs, two, filler);
    Moving m;
    moving_open(&m, html, 1000);
    FStyles *fs = f_style_build(m.p.doc, m.p.page, &m.p.env);
    size_t left = fs != NULL ? fs->lists_left : 0;
    f_style_free(fs);
    moving_close(&m);
    free(html);
    return left;
}

// A page of `divs` elements, each a `filler`-shadow text shadow out of the
// lists' budget, then one animating three keyframes of 64 shadows each,
// the last of them offset 1000 px up.
static char *shadow_budget_page(int divs, int filler)
{
    size_t cap = (size_t)divs * 24 + 64 * 30 * 3 + (size_t)filler * 24 + 1024, at = 0;
    char *html = malloc(cap);
    at += (size_t)snprintf(html + at, cap - at, MOTION_STYLE ".s{text-shadow:");
    for (int i = 0; i < filler; i++)
        at += (size_t)snprintf(html + at, cap - at, "%s0 1px 0 red", i ? "," : "");
    static const char *const kAt[] = {"from", "50%", "to"};
    static const int kY[] = {0, 1, -1000};
    at += (size_t)snprintf(html + at, cap - at, "}@keyframes far{");
    for (int f = 0; f < 3; f++) {
        at += (size_t)snprintf(html + at, cap - at, "%s{text-shadow:", kAt[f]);
        for (int i = 0; i < 64; i++)
            at += (size_t)snprintf(html + at, cap - at, "%s0 %dpx 0 red", i ? "," : "", kY[f]);
        at += (size_t)snprintf(html + at, cap - at, "}");
    }
    at += (size_t)snprintf(html + at, cap - at, "}</style>");
    for (int i = 0; i < divs; i++)
        at += (size_t)snprintf(html + at, cap - at, "<div class=s></div>");
    (void)snprintf(html + at, cap - at, "<div id=far style='animation:far 1s linear'>x</div>");
    return html;
}

static size_t shadow_budget_left(int divs, int filler)
{
    char *html = shadow_budget_page(divs, filler);
    Moving m;
    moving_open(&m, html, 1000);
    FStyles *fs = f_style_build(m.p.doc, m.p.page, &m.p.env);
    size_t left = fs != NULL ? fs->lists_left : 0;
    f_style_free(fs);
    moving_close(&m);
    free(html);
    return left;
}

// A keyframe whose shadow list finds no room holds the value underneath,
// which no bound can see past: with room left for the interval the layout
// mixes (its two keyframes and the mix) and none for the far keyframe, the
// reach is the farthest an offset can say (calibrated as the transform
// pages are, from what an element and the animated one cost).
static void motion_reach_budget_cases(void)
{
    size_t left0 = shadow_budget_left(0, 16), each = left0 - shadow_budget_left(1, 16);
    size_t animated = F_STYLE_LISTS_BUDGET - left0;
    int divs = each > 0 ? (int)((F_STYLE_LISTS_BUDGET - (animated - 2 * each)) / each) : 0;
    char *html = shadow_budget_page(divs, 16);
    Moving m;
    moving_open(&m, html, 1000);
    FStyles *fs = f_style_build(m.p.doc, m.p.page, &m.p.env);
    const FStyled *st = fs != NULL ? f_style_of(fs, motion_find(m.p.doc->document, "far")) : NULL;
    bool mixed = st != NULL && st->motion != NULL && st->style.ntext_shadows == 64;
    int32_t reach = st != NULL && st->motion != NULL ? st->motion->text_reach : -1;
    char detail[96];
    snprintf(detail, sizeof(detail), "mixed %d, reach %d, divs %d", mixed, reach, divs);
    expect("text reach: a keyframe with no room for its shadows is past every bound",
           mixed && reach == INT32_MAX, detail);
    f_style_free(fs);
    moving_close(&m);
    free(html);
}

// A property the layout's style dropped is due for nothing — not by the
// schedule's own reckoning, nor by its look at the next millisecond: an
// opacity moving from 0 to 1 for a second, dropped, is due at its end.
// How many items keyframe `f`'s declaration of `p` holds, or -1.
static int32_t declared_items(const FKeyframe *f, garb_prop_t p)
{
    for (int32_t i = f->ndecls - 1; i >= 0; i--)
        if (f->decls[i].prop == p)
            return f->decls[i].value.nitems;
    return -1;
}

// What styling a page leaves of the lists' budget.
static size_t lists_left_of(const char *html)
{
    Moving m;
    moving_open(&m, html, 1000);
    FStyles *fs = f_style_build(m.p.doc, m.p.page, &m.p.env);
    size_t left = fs != NULL ? fs->lists_left : 0;
    f_style_free(fs);
    moving_close(&m);
    return left;
}

// A keyframe's declaration an `!important` winner holds is never mixed, so
// it is never laid where the keyframe is resolved either: a transform list
// it writes costs the lists' budget nothing.
static void motion_important_budget_cases(void)
{
    char list[64 * 17 + 1] = "";
    for (int i = 0; i < 64; i++)
        strcat(list, "translateX(1px) ");
    char held[4096], none[4096];
    snprintf(held, sizeof(held), MOTION_STYLE
             "@keyframes k{from{opacity:0;transform:%s}to{opacity:1;transform:%s}}"
             "#e{transform:translateX(1px) !important;animation:k 1s linear}</style><div id=e></div>",
             list, list);
    snprintf(none, sizeof(none), MOTION_STYLE
             "@keyframes k{from{opacity:0}to{opacity:1}}"
             "#e{transform:translateX(1px) !important;animation:k 1s linear}</style><div id=e></div>");
    expect_i64("motion: a keyframe's list an important winner holds costs the budget nothing",
               (int64_t)lists_left_of(held), (int64_t)lists_left_of(none));
    // And a keyframe setting the colour is resolved once more, for the
    // colour alone: its transform list is built once, as one setting no
    // colour builds it.
    char coloured[4096], plain[4096];
    snprintf(coloured, sizeof(coloured), MOTION_STYLE
             "@keyframes k{from{color:red;transform:%s}to{color:blue;transform:%s}}"
             "#e{animation:k 1s linear}</style><div id=e></div>", list, list);
    snprintf(plain, sizeof(plain), MOTION_STYLE
             "@keyframes k{from{transform:%s}to{transform:%s}}"
             "#e{animation:k 1s linear}</style><div id=e></div>", list, list);
    expect_i64("motion: the colour's resolution builds no list the colour does not read",
               (int64_t)lists_left_of(coloured), (int64_t)lists_left_of(plain));
}

static void motion_dropped_cases(void)
{
    os64_html_document_t *doc = parse("<!doctype html><p id=x>x</p>");
    const os64_html_node_t *x = motion_find(doc->document, "x");
    flow_style_t base;
    memset(&base, 0, sizeof(base));
    base.opacity = 1000;
    FKeyframe frames[2];
    static Hand hands[2];
    for (int k = 0; k < 2; k++) {
        hands[k].r.style = base;
        hands[k].r.style.opacity = (uint16_t)(k * 1000);
        hand_frame(&frames[k], &hands[k], GARB_OPACITY, k, (unsigned)(k * 1000));
    }
    FAnimation an;
    memset(&an, 0, sizeof(an));
    an.timing.name = (flow_name_t){"o", 1};
    an.timing.duration_ms = 1000;
    an.timing.iterations = 1;
    an.frames = frames;
    an.nframes = 2;
    f_props_add(&an.props, GARB_OPACITY);
    FMotion m;
    memset(&m, 0, sizeof(m));
    m.base = base;
    m.anims = &an;
    m.nanims = 1;
    bool ok;
    flow_timeline_t *tl = motion_meet(doc, x, &m, 0, &ok);
    Hand *const by[1] = {hands};
    double moving = layout_next(&m, 100, by);
    f_props_add(&an.dropped, GARB_OPACITY);
    double dropped = layout_next(&m, 100, by);
    // What an animation dropped is left as it was at every time asked
    // after (f_motion_style), not mixed again where there may be room.
    flow_style_t held = m.base;
    (void)f_motion_style(&m, 500, &held, lists_alloc, hand_resolved, (void *)by, NULL, false);
    expect_i64("motion: a dropped value is left as it was at a later time", held.opacity, 1000);
    char detail[64];
    snprintf(detail, sizeof(detail), "moving %g, dropped %g", moving, dropped);
    expect("motion: a dropped value is due for nothing", ok && moving == 100 && dropped == 1000, detail);
    // Opacity's side effect follows the animation's clock, so its end is
    // due; a dropped list that gives none leaves nothing that can show,
    // and the endless animation is due for nothing at all.
    memset(&an.props, 0, sizeof(an.props));
    memset(&an.dropped, 0, sizeof(an.dropped));
    f_props_add(&an.props, GARB_BOX_SHADOW);
    f_props_add(&an.dropped, GARB_BOX_SHADOW);
    an.timing.iterations = INFINITY;
    double nothing = layout_next(&m, 100, by);
    snprintf(detail, sizeof(detail), "due %g", nothing);
    expect("motion: a dropped list with no side effect is due for nothing", isinf(nothing), detail);
    flow_timeline_free(tl);
    os64_html_document_free(doc);

    // And through a layout: enough elements before it spend the lists'
    // budget that the animated one's keyframes find room for their lists
    // and its mix, half again as much, does not (calibrated from what one
    // element and the animated one cost, so the sizes may change). Its
    // value is left as the base has it, and it is due at its end.
    size_t left0 = dropped_left(0, false, 64), each = left0 - dropped_left(1, false, 64);
    size_t animated = F_STYLE_LISTS_BUDGET - left0;
    int divs = each > 0 ? (int)((left0 + animated - animated * 7 / 10) / each) : 0;
    char *html = dropped_page(divs, false, 64);
    Moving lm;
    moving_open(&lm, html, 1000);
    FStyles *fs = f_style_build(lm.p.doc, lm.p.page, &lm.p.env);
    const os64_html_node_t *ln = motion_find(lm.p.doc->document, "long");
    const FStyled *st = ln != NULL && fs != NULL ? f_style_of(fs, ln) : NULL;
    bool keyframes = st != NULL && st->motion != NULL && st->motion->nanims == 1 &&
                     st->motion->anims[0].nframes == 2 &&
                     declared_items(&st->motion->anims[0].frames[0], GARB_TRANSFORM) == 64 &&
                     declared_items(&st->motion->anims[0].frames[1], GARB_TRANSFORM) == 64;
    expect("motion: the page leaves room for the keyframes and none for their mix",
           keyframes && st->style.ntransform == 0, NULL);
    f_style_free(fs);
    snprintf(detail, sizeof(detail), "due %llu",
             (unsigned long long)flow_motion_due(lm.tree, (os64_gui_rect_t){0, 0, 800, 1000000},
                                                 (flow_point_t){0, 0}));
    expect("motion due: a value whose lists found no room is not due every frame",
           flow_motion_due(lm.tree, (os64_gui_rect_t){0, 0, 800, 1000000}, (flow_point_t){0, 0}) == 2000,
           detail);
    moving_close(&lm);
    free(html);

    // No room for either keyframe's list where it is resolved, and room for
    // the animation's own: the budget spent by elements whose lists are a
    // quarter of a keyframe's, down to between a half and three quarters
    // of one. The value is dropped as a mix with no room is — left as the
    // base has it, and due at its end — never mixed against the element's
    // own list in the keyframes' place, which here, an empty list mixed
    // with itself, needs no room and would pass for a value.
    size_t quarter = dropped_left(0, false, 16) - dropped_left(1, false, 16);
    divs = quarter > 0 ? (int)((F_STYLE_LISTS_BUDGET - 2 * quarter) / quarter) : 0;
    html = dropped_page(divs, false, 16);
    moving_open(&lm, html, 1000);
    fs = f_style_build(lm.p.doc, lm.p.page, &lm.p.env);
    ln = motion_find(lm.p.doc->document, "long");
    st = ln != NULL && fs != NULL ? f_style_of(fs, ln) : NULL;
    expect("motion: a keyframe whose list finds no room leaves its value dropped",
           st != NULL && st->motion != NULL && st->style.ntransform == 0 &&
               f_props_has(&st->motion->anims[0].dropped, GARB_TRANSFORM), NULL);
    f_style_free(fs);
    uint64_t refused = flow_motion_due(lm.tree, (os64_gui_rect_t){0, 0, 800, 1000000}, (flow_point_t){0, 0});
    snprintf(detail, sizeof(detail), "due %llu", (unsigned long long)refused);
    expect("motion due: and it is due at its end", refused == 2000, detail);
    moving_close(&lm);
    free(html);

    // Two animations of one property, the earlier's mix one function and
    // the later's 64, with room left for the earlier's alone (calibrated
    // as above): the later's is dropped, and the earlier's, which the
    // style shows, moves — so the element is due now, not at the end.
    left0 = dropped_left(0, true, 64);
    each = left0 - dropped_left(1, true, 64);
    animated = F_STYLE_LISTS_BUDGET - left0;
    divs = each > 0 ? (int)((left0 + animated - animated * 7 / 10) / each) : 0;
    html = dropped_page(divs, true, 64);
    moving_open(&lm, html, 1000);
    fs = f_style_build(lm.p.doc, lm.p.page, &lm.p.env);
    ln = motion_find(lm.p.doc->document, "long");
    st = ln != NULL && fs != NULL ? f_style_of(fs, ln) : NULL;
    bool split = st != NULL && st->motion != NULL && st->motion->nanims == 2 &&
                 declared_items(&st->motion->anims[1].frames[0], GARB_TRANSFORM) == 64 &&
                 st->style.ntransform == 1 &&
                 !f_props_has(&st->motion->anims[0].dropped, GARB_TRANSFORM) &&
                 f_props_has(&st->motion->anims[1].dropped, GARB_TRANSFORM);
    expect("motion: the page leaves room for the earlier animation's mix and none for the later's", split,
           NULL);
    f_style_free(fs);
    uint64_t due = flow_motion_due(lm.tree, (os64_gui_rect_t){0, 0, 800, 1000000}, (flow_point_t){0, 0});
    snprintf(detail, sizeof(detail), "due %llu", (unsigned long long)due);
    expect("motion due: what one animation dropped leaves another's motion of it due", due == 1000, detail);
    moving_close(&lm);
    free(html);

    // The other way round: a 64-function list that stands still replaces a
    // one-function list moving under it, and the tree shows it still: its
    // two keyframes write one declaration, so nothing is due until the end.
    size_t cap = 64 * 17 * 2 + 1024, at = 0;
    html = malloc(cap);
    at += (size_t)snprintf(html + at, cap - at, MOTION_STYLE
                           "@keyframes mv{from{transform:translateX(0px)}to{transform:translateX(10px)}}"
                           "@keyframes st{from{transform:");
    for (int i = 0; i < 64; i++)
        at += (size_t)snprintf(html + at, cap - at, "translateX(1px) ");
    at += (size_t)snprintf(html + at, cap - at, "}to{transform:");
    for (int i = 0; i < 64; i++)
        at += (size_t)snprintf(html + at, cap - at, "translateX(1px) ");
    (void)snprintf(html + at, cap - at, "}}</style><div style='animation:mv 1s linear,st 1s linear'>x</div>");
    moving_open(&lm, html, 1000);
    due = flow_motion_due(lm.tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0});
    snprintf(detail, sizeof(detail), "due %llu", (unsigned long long)due);
    expect("motion due: a still list over a moving one is due at its end", due == 2000, detail);
    moving_close(&lm);
    free(html);
}

// A time past the domain is held at its limit (values.h): a 1e308s
// iteration begun -5e307s ago is over — its delay and its duration both
// held at 1e12 ms — as Chrome's, held to a float's seconds, is over too.
// Its fill is none, so the value is the base's, and nothing is due.
static void motion_held_time_cases(void)
{
    Moving m;
    moving_open(&m, MOTION_STYLE "@keyframes k{from{opacity:0}to{opacity:1}}"
                "</style><div id=x style='animation:k 1e308s linear -5e307s paused'>x</div>", 1000);
    const flow_style_t *s = moving_style(&m, "x");
    bool held = s != NULL && s->animations.nduration == 1 &&
                s->animations.duration[0] == GARB_TIME_MS_MAX && s->animations.ndelay == 1 &&
                s->animations.delay[0] == -GARB_TIME_MS_MAX;
    expect("motion: a time past the domain is held at its limit", held, NULL);
    // A count past the domain is held at its limit, finite, never read as
    // `infinite`: 1e308 iterations of 1e-12 ms are held to 1e15, a second
    // in all, so 1.5 s in it is over where an infinite one still runs.
    {
        Moving c;
        moving_open(&c, MOTION_STYLE "@keyframes h{from{opacity:0}to{opacity:.5}}</style>"
                    "<div id=n style='animation:h 1e-15s linear 1e308 -1.5s paused'>x</div>"
                    "<div id=f style='animation:h 1e-15s linear infinite -1.5s paused'>x</div>",
                    1000);
        const flow_style_t *n = moving_style(&c, "n"), *f = moving_style(&c, "f");
        expect("motion: a count past the domain is held at its limit, and ends",
               n != NULL && n->animations.niterations == 1 &&
                   n->animations.iterations[0] == GARB_ITERATIONS_MAX && n->opacity == 1000 &&
                   f != NULL && f->opacity < 1000,
               NULL);
        moving_close(&c);
    }
    char detail[64];
    snprintf(detail, sizeof(detail), "opacity %d", s != NULL ? (int)s->opacity : -1);
    expect("motion: an iteration held at the domain's longest time is over",
           s != NULL && s->opacity == 1000, detail);
    expect("motion due: an animation held at the domain's longest time is due for nothing",
           flow_motion_due(m.tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0}) == FLOW_NEVER,
           NULL);
    moving_close(&m);
}

// An incomplete layout styles the whole page and stops before laying it
// all out, so what its styles no longer name is carried on: only a
// complete layout may drop it. Here #a's animation, started at 1000, is
// named by no rule of a cascade the page is laid out with at 3000 in too
// small a budget; that tree shown, a layout at 5000 back on the page's own
// cascade finds the animation 4 s into its 10 — and with each of the
// incomplete layout's last allocations failed in turn, where it carries
// the record on, a tree comes back only with the record carried.
static void motion_incomplete_cases(void)
{
    size_t cap = 8192;
    char *html = malloc(cap);
    size_t at = (size_t)snprintf(html, cap, MOTION_STYLE
        "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "#a{animation:mv 10s linear}"
        "@media (prefers-reduced-motion:reduce){#a{animation:none}}"
        "</style><div id=a>a</div>");
    for (int i = 0; i < 40; i++)
        at += (size_t)snprintf(html + at, cap - at, "<b>");
    for (int i = 0; i < 100; i++)
        at += (size_t)snprintf(html + at, cap - at, "x<div></div>");
    size_t tried = 0, shown = 0;
    bool failed = false;
    for (size_t back = 0; back <= 8 && !failed; back++) {
        Moving m;
        moving_open(&m, html, 1000);
        garb_sheet_in_t in[16];
        for (int32_t k = 0; k < m.p.n; k++)
            in[k] = (garb_sheet_in_t){.sheet = &m.p.sheets[k]};
        garb_cascade_t *calm =
            garb_cascade(in, m.p.n, m.p.doc, (garb_env_t){.width = 800, .height = 600, .reduced_motion = true});
        m.p.env.cascade = calm;
        m.p.env.max_arena_bytes = 4096;
        m.p.env.now_ms = 3000;
        allocations = 0;
        flow_tree_t *probe = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
        size_t count = allocations;
        bool incomplete = probe != NULL && flow_incomplete(probe);
        flow_free(probe);
        if (back == 0)
            expect("motion: a layout in too small a budget is incomplete", incomplete, NULL);
        failed |= !incomplete;
        if (back < count) {
            allocations = 0;
            fail_single = true;
            fail_at = back > 0 ? count + 1 - back : 0;
            flow_tree_t *tree = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
            fail_at = 0;
            fail_single = false;
            tried++;
            if (tree != NULL) {
                shown++;
                flow_free(m.tree);
                m.tree = tree;
                flow_timeline_commit(&m.tl, tree);
                m.p.env.timeline = m.tl;
            }
        }
        m.p.env.cascade = m.p.cascade;
        m.p.env.max_arena_bytes = 0;
        moving_again(&m, 5000);
        double g[16];
        bool ran = moving_matrix(&m, "a", g) && fabs(g[12] - 40) <= 1.0 / 128 + 1e-4;
        expect("motion: what an incomplete layout's styles no longer name is carried on", ran, NULL);
        failed |= !ran;
        garb_cascade_free(calm);
        moving_close(&m);
    }
    free(html);
    printf("libflow motion: an incomplete layout carried its records on, %zu of its last allocations failed "
           "(%zu trees shown)\n", tried, shown);
}

// ── Ticks (PILE3.md § Animations in place, slice S8b) ──────────────────

// A page of every kind of value the tick set moves, each element's
// animations repeating for ever so no phase edge falls inside the times it
// is asked at: transforms in percentages and `em`, lists that pair and
// lists mixed as matrices, an origin and the individual three, opacity over
// a value underneath and in steps, colours with currentColor at one end,
// background positions of 2 and 3 items mixed to 6, shadows in
// currentColor, a hold between equal keyframes, an element inside a
// turned one, and an inline-block turning on its line, whose frame is its
// atom's. `w`, last on the page, moves by layouts.
static const char kTickPage[] = MOTION_STYLE
    "@keyframes spin{from{transform:rotate(0deg) translateX(10%)}"
    "to{transform:rotate(360deg) translateX(2em)}}"
    "@keyframes mixed{0%{transform:translate(10px,5px)}40%{transform:scale(2) rotate(30deg)}"
    "to{transform:skewX(20deg) translateY(-3em) perspective(400px) rotateY(40deg)}}"
    "@keyframes fade{from{opacity:0}50%{opacity:.8}to{opacity:.2}}"
    "@keyframes over{to{opacity:.5}}"
    "@keyframes hue{from{background-color:red;border-color:currentColor}"
    "to{background-color:rgba(0,0,255,.5);border-color:green}}"
    "@keyframes pos{from{background-position:0 0,10px 20%}to{background-position:50% 3em,1px 2px,30% 40%}}"
    "@keyframes glow{from{box-shadow:0 0 0 red}"
    "to{box-shadow:5px 5px 10px 2px currentColor,inset 0 0 4px blue}}"
    "@keyframes origin{from{transform-origin:0 0}"
    "to{transform-origin:100% 50%;translate:10px 1em;rotate:45deg;scale:1.5}}"
    "@keyframes hold{0%,40%{transform:translateX(0)}60%,to{transform:translateX(50px)}}"
    "@keyframes wide{to{width:300px}}"
    "#a{font-size:20px;animation:spin 1.3s linear infinite}"
    "#b{animation:mixed 2s ease-in-out infinite alternate}"
    "#c{opacity:.3;animation:fade 700ms steps(4) infinite,over 900ms infinite}"
    "#d{color:orange;border:3px solid;animation:hue 1.1s cubic-bezier(.3,1.6,.6,-.4) infinite}"
    "#e{background-image:linear-gradient(red,blue),linear-gradient(green,white);"
    "animation:pos 1.7s infinite}"
    "#f{color:teal;animation:glow 1s infinite reverse}"
    "#g{font-size:12px;animation:origin 1.5s infinite}"
    "#h{animation:hold 1s ease infinite}"
    "#wrap{transform:rotate(10deg)}"
    "#in{animation:fade 800ms infinite,hue 600ms infinite}"
    "#ib{display:inline-block;width:60px;animation:spin 1.1s linear infinite}"
    "#w{animation:wide 1s linear infinite}"
    "</style><div id=a></div><div id=b></div><div id=c></div><div id=d></div><div id=e></div>"
    "<div id=f></div><div id=g></div><div id=h></div><div id=wrap><div id=in></div></div>"
    "<p>an <span id=ib>atom</span> on its line</p><div id=w></div>";
static const char *const kTickIds[] = {"a", "b", "c", "d", "e", "f", "g", "h", "in", "ib"};

static const flow_frame_t *own_frame(const flow_box_t *b)
{
    return b->frame != NULL && b->frame->box == b && b->frame->kind == F_FRAME_TRANSFORM ? b->frame : NULL;
}

// What first differs between two styles in the tick set, to the bit, or
// NULL: a transform as the matrix it makes of a 100 by 40 box.
static const char *tick_differs(const flow_style_t *a, const flow_style_t *b)
{
    if (a->opacity != b->opacity)
        return "opacity";
    if (a->has_background != b->has_background || a->background != b->background)
        return "the background's colour";
    if (a->current_colours != b->current_colours)
        return "what is in currentColor";
    if (memcmp(a->border_color, b->border_color, sizeof(a->border_color)) != 0)
        return "the border colours";
    if (memcmp(a->transform_origin, b->transform_origin, sizeof(a->transform_origin)) != 0 ||
        a->transform_origin_z != b->transform_origin_z)
        return "transform-origin";
    const flow_backgrounds_t *x = &a->backgrounds, *y = &b->backgrounds;
    if (x->nx != y->nx || x->ny != y->ny ||
        (x->nx > 0 && memcmp(x->x, y->x, (size_t)x->nx * sizeof(*x->x)) != 0) ||
        (x->ny > 0 && memcmp(x->y, y->y, (size_t)x->ny * sizeof(*x->y)) != 0))
        return "the background's position";
    if (a->nbox_shadows != b->nbox_shadows ||
        (a->nbox_shadows > 0 &&
         memcmp(a->box_shadows, b->box_shadows, (size_t)a->nbox_shadows * sizeof(*a->box_shadows)) != 0))
        return "box-shadow";
    double ma[16], mb[16];
    f_transform_matrix(a, 0, 0, 100 * 64, 40 * 64, ma);
    f_transform_matrix(b, 0, 0, 100 * 64, 40 * 64, mb);
    if (memcmp(ma, mb, sizeof(ma)) != 0)
        return "the transform";
    return NULL;
}

// What first differs between two boxes' own transform frames, or NULL.
static const char *frame_differs(const flow_frame_t *a, const flow_frame_t *b)
{
    if ((a == NULL) != (b == NULL))
        return "whether there is a frame";
    if (a == NULL)
        return NULL;
    if (memcmp(a->matrix, b->matrix, sizeof(a->matrix)) != 0)
        return "the frame's matrix";
    if (a->by.x != b->by.x || a->by.y != b->by.y)
        return "the frame's move";
    if (a->through != b->through || a->upright != b->upright ||
        memcmp(a->affine, b->affine, sizeof(a->affine)) != 0)
        return "how the frame is drawn";
    return NULL;
}

// A box `t` ticked to a time and `l` laid out at it: the same style and
// frame, and ink no less; `why` says what was not.
static bool box_ticks_as_laid(const flow_box_t *t, const flow_box_t *l, char *why, size_t cap)
{
    const char *d = t == NULL || l == NULL ? "a box" : tick_differs(t->style, l->style);
    if (d == NULL)
        d = frame_differs(own_frame(t), own_frame(l));
    for (int k = 0; d == NULL && k < 4; k++)
        if (t->ink[k] < l->ink[k])
            d = "ink less than the layout's";
    if (d != NULL)
        snprintf(why, cap, "%s", d);
    return d == NULL;
}

static const os64_gui_rect_t kTickView = {0, 0, 800, 600};

// A tick at a time draws what a layout at that time draws, to the bit, at
// every time asked, one tick after another — and two trees, each ticked,
// keep their lists apart (ASan reads every one of them each time).
static void motion_tick_layout_cases(void)
{
    Moving tick, lay;
    moving_open_for(&tick, kTickPage, 0, true);
    moving_open(&lay, kTickPage, 0);
    expect("tick: a page whose values tick is due for ticks at once",
           flow_motion_tick_due(tick.tree, kTickView, (flow_point_t){0, 0}) == 0, NULL);
    expect("tick: a face that does not tick is due for no tick",
           flow_motion_tick_due(lay.tree, kTickView, (flow_point_t){0, 0}) == FLOW_NEVER, NULL);
    int compared = 0, bad = 0, refused = 0;
    for (uint64_t t = 1; t <= 4000; t += 37) {
        refused += !flow_motion_tick(tick.tree, t, (flow_point_t){0, 0}, NULL, NULL, NULL);
        moving_again(&lay, t);
        for (size_t i = 0; i < sizeof(kTickIds) / sizeof(kTickIds[0]); i++) {
            char why[96];
            compared++;
            if (!box_ticks_as_laid(moving_box(&tick, kTickIds[i]), moving_box(&lay, kTickIds[i]), why,
                                   sizeof(why))) {
                char detail[160];
                snprintf(detail, sizeof(detail), "#%s at %llu ms: %s", kTickIds[i], (unsigned long long)t,
                         why);
                expect("tick: a tick at a time draws what a layout at that time draws", false, detail);
                if (++bad > 8)
                    t = UINT64_MAX - 37;
            }
        }
    }
    expect("tick: every tick was taken", refused == 0, NULL);
    // `w` moves by layouts: a tick leaves it where the layout put it.
    const flow_style_t *w = moving_style(&tick, "w");
    expect("tick: what moves by layouts stands where the layout left it",
           w != NULL && w->width.value == 100 * 64, NULL);
    printf("libflow ticks: %d values ticked, each the layout's to the bit\n", compared);
    moving_close(&tick);
    moving_close(&lay);
}

// What the schedule makes of `css` and `body` laid out at 100 ms, with an
// element animated for ever: 't' when it ticks — due for ticks and not
// for layouts — 'l' when it moves by layouts, '?' for anything else.
static char page_moves(const char *css, const char *body, bool ticks)
{
    char html[2048];
    snprintf(html, sizeof(html), MOTION_STYLE "%s</style>%s", css, body);
    Moving m;
    moving_open_for(&m, html, 100, ticks);
    uint64_t lay = flow_motion_due(m.tree, kTickView, (flow_point_t){0, 0});
    uint64_t tick = flow_motion_tick_due(m.tree, kTickView, (flow_point_t){0, 0});
    moving_close(&m);
    return lay == FLOW_NEVER && tick == 100 ? 't' : lay == 100 && tick == FLOW_NEVER ? 'l' : '?';
}

// What makes an element tick, and each thing that keeps it from it.
static void motion_tick_condition_cases(void)
{
    static const struct {
        const char *name, *css, *body;
        bool ticks;
        char expect;
    } kCases[] = {
        {"a transform ticks",
         "@keyframes k{to{transform:rotate(90deg)}}#x{animation:k 1s infinite}", "<div id=x></div>", true,
         't'},
        {"every property of the tick set ticks together",
         "@keyframes k{to{transform:scale(2);translate:1px;rotate:5deg;scale:2;transform-origin:0 0;"
         "opacity:.5;background-color:red;background-position:1px 2px;border-color:blue;"
         "box-shadow:1px 1px red}}#x{animation:k 1s infinite}",
         "<div id=x></div>", true, 't'},
        {"a face that does not tick lays it out",
         "@keyframes k{to{transform:rotate(90deg)}}#x{animation:k 1s infinite}", "<div id=x></div>", false,
         'l'},
        {"a width is laid out",
         "@keyframes k{to{width:10px}}#x{animation:k 1s infinite}", "<div id=x></div>", true, 'l'},
        {"a colour is laid out, inherited",
         "@keyframes k{to{color:red;transform:none}}#x{animation:k 1s infinite}", "<div id=x></div>", true,
         'l'},
        {"a child's inherit takes the ticks away",
         "@keyframes k{to{background-color:red}}#x{animation:k 1s infinite}#y{background-color:inherit}",
         "<div id=x><div id=y></div></div>", true, 'l'},
        {"a child's keyframe inherit takes the ticks away",
         "@keyframes k{to{background-color:red}}@keyframes c{to{background-color:inherit;color:red}}"
         "#x{animation:k 1s infinite}#y{animation:c 1s infinite}",
         "<div id=x><div id=y></div></div>", true, 'l'},
        {"a child's inherit of another property leaves them",
         "@keyframes k{to{background-color:red}}#x{animation:k 1s infinite}#y{border-color:inherit}",
         "<div id=x><div id=y></div></div>", true, 't'},
        {"a background cut to the text is laid out",
         "@keyframes k{to{background-color:red}}#x{background-clip:text;animation:k 1s infinite}",
         "<div id=x>text</div>", true, 'l'},
    };
    // Each half asks its own elements: a hold that ticks is due at its far
    // end however the width beside it moves.
    static const char kBoth[] = MOTION_STYLE
        "@keyframes hold{0%,40%{transform:none}60%,to{transform:rotate(9deg)}}@keyframes w{to{width:9px}}"
        "#t{animation:hold 1s linear infinite}#w{animation:w 1s linear infinite}"
        "</style><div id=t></div><div id=w></div>";
    Moving both;
    moving_open_for(&both, kBoth, 100, true);
    expect("tick: a laid-out value is due for layouts, a held tick at its far end",
           flow_motion_due(both.tree, kTickView, (flow_point_t){0, 0}) == 100 &&
               flow_motion_tick_due(both.tree, kTickView, (flow_point_t){0, 0}) == 500, NULL);
    moving_close(&both);
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char got = page_moves(kCases[i].css, kCases[i].body, kCases[i].ticks);
        char detail[160];
        snprintf(detail, sizeof(detail), "%s: '%c', not '%c'", kCases[i].name, got, kCases[i].expect);
        expect("tick: what ticks, and what keeps it from it", got == kCases[i].expect, detail);
    }
}

// A phase edge is the layout's: one past it is not ticked across, and stays
// as the last tick before it left it; and a hold is due at its far end.
static void motion_tick_edge_cases(void)
{
    static const char kEdge[] = MOTION_STYLE
        "@keyframes k{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "@keyframes hold{0%,40%{opacity:0}60%,to{opacity:1}}"
        "#x{animation:k 1s linear 500ms both}#h{animation:hold 1s linear infinite}"
        "</style><div id=x></div><div id=h></div>";
    const flow_point_t o = {0, 0};
    Moving m, lay;
    moving_open_for(&m, kEdge, 0, true);
    moving_open(&lay, kEdge, 0);
    expect("tick: the start of an active phase is the layout's",
           flow_motion_due(m.tree, kTickView, o) == 500, NULL);
    // At 100 the hold's first interval holds still (0 to 40%): due at 400.
    (void)flow_motion_tick(m.tree, 100, o, NULL, NULL, NULL);
    expect("tick: a hold is due at its far end", flow_motion_tick_due(m.tree, kTickView, o) == 400, NULL);
    moving_again(&m, 600);
    expect("tick: the end of an active phase is the layout's",
           flow_motion_due(m.tree, kTickView, o) == 1500, NULL);
    expect("tick: a value in its active phase is due for a tick",
           flow_motion_tick_due(m.tree, kTickView, o) == 600, NULL);
    (void)flow_motion_tick(m.tree, 1200, o, NULL, NULL, NULL);
    (void)flow_motion_tick(m.tree, 1700, o, NULL, NULL, NULL);
    moving_again(&lay, 1200);
    char why[96];
    expect("tick: past its edge, a value stays as the last tick before it left it",
           box_ticks_as_laid(moving_box(&m, "x"), moving_box(&lay, "x"), why, sizeof(why)), why);
    moving_again(&lay, 1700);
    expect("tick: other values tick on past one element's edge",
           box_ticks_as_laid(moving_box(&m, "h"), moving_box(&lay, "h"), why, sizeof(why)), why);
    moving_close(&m);
    moving_close(&lay);
    // A tick's holds are what it found, asked at its own time: laid out at
    // 500, between the hold's two ends, and ticked to 700, where it holds
    // to the iteration's end.
    static const char kHold[] = MOTION_STYLE
        "@keyframes hold{0%,40%{opacity:0}60%,to{opacity:1}}#h{animation:hold 1s linear infinite}"
        "</style><div id=h></div>";
    moving_open_for(&m, kHold, 0, true);
    moving_again(&m, 500);
    (void)flow_motion_tick(m.tree, 700, o, NULL, NULL, NULL);
    expect("tick: a tick's holds are its own, asked at its time",
           flow_motion_tick_due(m.tree, kTickView, o) == 1000, NULL);
    moving_close(&m);
    static const char kAlone[] = MOTION_STYLE
        "@keyframes k{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "#x{animation:k 1s linear 500ms both}</style><div id=x></div>";
    moving_open_for(&m, kAlone, 0, true);
    moving_again(&m, 600);
    (void)flow_motion_tick(m.tree, 1700, o, NULL, NULL, NULL);
    expect("tick: past its edge an element is due for no tick, but for the layout",
           flow_motion_tick_due(m.tree, kTickView, o) == FLOW_NEVER &&
               flow_motion_due(m.tree, kTickView, o) == 1500, NULL);
    moving_close(&m);
}

typedef struct {
    os64_gui_rect_t r[64];
    const flow_box_t *box[64];
    int n;
} TickMarks;

static void tick_mark(void *ctx, const flow_box_t *box, os64_gui_rect_t r)
{
    TickMarks *k = ctx;
    if (k->n < 64) {
        k->r[k->n] = r;
        k->box[k->n++] = box;
    }
}

// Whether `r` lies inside the marks handed for `box`, joined.
static bool marked(const TickMarks *k, const flow_box_t *box, os64_gui_rect_t r)
{
    int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
    for (int i = 0; i < k->n; i++)
        if (k->box[i] == box) {
            x0 = k->r[i].x < x0 ? k->r[i].x : x0;
            y0 = k->r[i].y < y0 ? k->r[i].y : y0;
            x1 = (int64_t)k->r[i].x + k->r[i].w > x1 ? (int64_t)k->r[i].x + k->r[i].w : x1;
            y1 = (int64_t)k->r[i].y + k->r[i].h > y1 ? (int64_t)k->r[i].y + k->r[i].h : y1;
        }
    return x0 <= r.x && y0 <= r.y && (int64_t)r.x + r.w <= x1 && (int64_t)r.y + r.h <= y1;
}

static bool marks_box(const TickMarks *k, const flow_box_t *box)
{
    for (int i = 0; i < k->n; i++)
        if (k->box[i] == box)
            return true;
    return false;
}

// `r` grown by `by` on every side.
static os64_gui_rect_t tick_grown(os64_gui_rect_t r, int32_t by)
{
    return (os64_gui_rect_t){r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by};
}

// What a tick marks: where a turning element was and is, inside a turned
// one too; a shadow's reach where no group holds it; the root, whose
// background is the canvas's; nothing for a value holding still; and
// whether a frame moved.
static void motion_tick_mark_cases(void)
{
    static const char kMarks[] = MOTION_STYLE
        "@keyframes spin{to{transform:rotate(360deg)}}"
        "@keyframes glow{from{box-shadow:none}to{box-shadow:0 0 0 20px red}}"
        "@keyframes hold{0%,50%{opacity:.5}to{opacity:1}}"
        "@keyframes bg{to{background-color:blue}}"
        "html{animation:bg 1s infinite}"
        "#s{animation:spin 1s linear infinite}#g{animation:glow 1s linear infinite}"
        "#h{animation:hold 1s linear infinite}#wrap{transform:rotate(30deg)}"
        "#in{animation:spin 1s linear infinite}#far{position:absolute;left:400px;top:0;width:10px;height:10px}"
        "#gin{animation:glow 1s linear infinite}"
        "</style><div id=s><div id=far></div></div><div id=gw><div id=g></div></div><div id=h></div>"
        "<div id=wrap><div id=in></div><div id=gin></div></div>";
    const flow_point_t o = {0, 0};
    Moving m;
    moving_open_for(&m, kMarks, 0, true);
    const flow_box_t *s = moving_box(&m, "s"), *g = moving_box(&m, "g"), *in = moving_box(&m, "in");
    const flow_box_t *far = moving_box(&m, "far"), *gw = moving_box(&m, "gw"), *gin = moving_box(&m, "gin");
    os64_gui_rect_t s_was = flow_box_doc_rect(s, o), in_was = flow_box_doc_rect(in, o);
    os64_gui_rect_t far_was = flow_box_doc_rect(far, o);
    TickMarks k = {.n = 0};
    bool moved = false;
    (void)flow_motion_tick(m.tree, 125, o, tick_mark, &k, &moved);
    expect("tick marks: a tick that turns a frame says so", moved, NULL);
    expect("tick marks: where a turning element was", marked(&k, s, s_was), NULL);
    expect("tick marks: where it is", marked(&k, s, flow_box_doc_rect(s, o)), NULL);
    expect("tick marks: where a positioned box inside it was, the group's whole",
           marked(&k, s, far_was), NULL);
    expect("tick marks: where it is", marked(&k, s, flow_box_doc_rect(far, o)), NULL);
    expect("tick marks: where one inside a turned one was, through its matrix", marked(&k, in, in_was), NULL);
    expect("tick marks: where it is, through its matrix", marked(&k, in, flow_box_doc_rect(in, o)), NULL);
    expect("tick marks: a shadow's reach, with no group to hold it",
           marked(&k, g, tick_grown(flow_box_doc_rect(g, o), 2)), NULL);
    expect("tick marks: and inside a turned one, through its matrix", marked(&k, gin, flow_box_doc_rect(gin, o)),
           NULL);
    expect("tick marks: the root, by its own box", marks_box(&k, flow_root(m.tree)), NULL);
    expect("tick marks: nothing for a value holding still", !marks_box(&k, moving_box(&m, "h")), NULL);
    // A shadow grows its ink, and its ink does not shrink back.
    int32_t ink = g->ink[FLOW_TOP];
    expect("tick marks: a growing shadow grows its box's ink", ink >= 2, NULL);
    expect("tick marks: and hands it up", gw->ink[FLOW_TOP] >= ink, NULL);
    (void)flow_motion_tick(m.tree, 1010, o, NULL, NULL, NULL);
    expect("tick marks: a box's ink only grows", g->ink[FLOW_TOP] >= ink, NULL);
    moving_close(&m);
    static const char kColours[] = MOTION_STYLE
        "@keyframes bg{to{background-color:blue}}#x{animation:bg 1s infinite}</style><div id=x></div>";
    moving_open_for(&m, kColours, 0, true);
    moved = true;
    (void)flow_motion_tick(m.tree, 500, o, NULL, NULL, &moved);
    expect("tick marks: a tick of colours moves no frame", !moved, NULL);
    moving_close(&m);
}

// The page laid out again at `now` beside the tree on screen, not shown:
// what a face does before it hands the old tree's parts over.
static flow_tree_t *moving_beside(Moving *m, uint64_t now)
{
    m->p.env.now_ms = now;
    return flow_layout(m->p.doc, m->p.page, 800, &m->p.env);
}

// Whether a mark was handed for a box of `node`, in either tree, and the
// marks for its boxes joined.
static bool marks_node(const TickMarks *k, const os64_html_node_t *node, os64_gui_rect_t *joined)
{
    bool any = false;
    int64_t x0 = INT64_MAX, y0 = INT64_MAX, x1 = INT64_MIN, y1 = INT64_MIN;
    for (int i = 0; i < k->n; i++)
        if (k->box[i]->node == node) {
            any = true;
            x0 = k->r[i].x < x0 ? k->r[i].x : x0;
            y0 = k->r[i].y < y0 ? k->r[i].y : y0;
            x1 = (int64_t)k->r[i].x + k->r[i].w > x1 ? (int64_t)k->r[i].x + k->r[i].w : x1;
            y1 = (int64_t)k->r[i].y + k->r[i].h > y1 ? (int64_t)k->r[i].y + k->r[i].h : y1;
        }
    if (any && joined != NULL)
        *joined = (os64_gui_rect_t){(int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0)};
    return any;
}

// What a layout for the animations alone changed (flow_layout_parts): the
// element whose values are drawn in place, where it was and is, and the
// boxes drawn outside it that take what changed — by the rule the view's
// question asks (stacked_from); nothing for a value holding still; and no
// parts at all where a layout placed something, the canvas may take the
// background, or the element has no box of its own — each refusal said
// with its reason (`why`), and the element and property that refused.
static void motion_layout_part_cases(void)
{
    static const struct {
        const char *name, *style, *body, *id;
        bool bounded, marks;
        flow_parts_why_t why;
    } kParts[] = {
        {"layout parts: a text shadow marks its element", "@keyframes a{to{text-shadow:0 -10px 0 red}}",
         "<div id=x style='animation:a 1s linear'>x</div>", "x", true, true, FLOW_PARTS_MARKED},
        {"layout parts: a colour marks its element", "@keyframes a{to{color:red}}",
         "<div id=x style='animation:a 1s linear'>x</div>", "x", true, true, FLOW_PARTS_MARKED},
        {"layout parts: a turning element marks its group, what it holds drawn outside it included",
         "@keyframes a{to{transform:rotate(90deg)}}",
         "<div id=x style='animation:a 1s linear'><p style='position:absolute;left:400px;margin:0'>x</p></div>",
         "x", true, true, FLOW_PARTS_MARKED},
        {"layout parts: a fading element marks its group, what it holds drawn outside it included",
         "@keyframes a{to{opacity:.3}}",
         "<div id=x style='animation:a 1s linear'><p style='position:absolute;left:400px;margin:0'>x</p></div>",
         "x", true, true, FLOW_PARTS_MARKED},
        {"layout parts: a fixed box inside takes an inherited colour",
         "@keyframes a{to{color:red}}",
         "<div style='margin-top:200px;animation:a 1s linear'><p id=x style='position:fixed;top:0;margin:0'>x</p></div>",
         "x", true, true, FLOW_PARTS_MARKED},
        {"layout parts: a positioned box inside takes a background by keyword",
         "@keyframes a{to{background-color:blue}}",
         "<div style='position:relative;margin-top:200px;animation:a 1s linear'>"
         "<p id=x style='position:absolute;top:-150px;margin:0;background:inherit'>x</p></div>",
         "x", true, true, FLOW_PARTS_MARKED},
        {"layout parts: one with a background of its own takes nothing",
         "@keyframes a{to{background-color:blue}}",
         "<div style='position:relative;margin-top:200px;animation:a 1s linear'>"
         "<p id=x style='position:absolute;top:-150px;margin:0;background:green'>x</p></div>",
         "x", true, false, FLOW_PARTS_MARKED},
        {"layout parts: nothing for a value holding still", "@keyframes a{to{color:red}}",
         "<div id=x style='animation:a 1s steps(1,end)'>x</div>", "x", true, false, FLOW_PARTS_MARKED},
        {"layout parts: none where what changed moved the element's box", "@keyframes a{to{width:300px}}",
         "<div id=x style='animation:a 1s linear'>x</div>", "x", false, false, FLOW_PARTS_MOVED},
        {"layout parts: none where a layout places what changed, though nothing moved yet",
         "@keyframes a{to{z-index:9}}",
         "<div id=x style='position:relative;z-index:1;animation:a 1s linear'>x</div>", "x", false, false,
         FLOW_PARTS_PLACED},
        {"layout parts: none for the background the canvas takes",
         "@keyframes a{to{background-color:blue}}body{animation:a 1s linear}",
         "<div id=x>x</div>", "x", false, false, FLOW_PARTS_CANVAS},
        {"layout parts: none for an element with no box of its own", "@keyframes a{to{color:red}}",
         "<div id=x style='display:contents;animation:a 1s linear'><span>x</span></div>", "x", false, false,
         FLOW_PARTS_NO_BOX},
        {"layout parts: none for an element with no box of its own around a bare text",
         "@keyframes a{to{color:red}}",
         "<div id=x style='display:contents;animation:a 1s linear'>x</div>", "x", false, false, FLOW_PARTS_NO_BOX},
        {"layout parts: an element inside a button, which its widget draws instead, has nothing to mark",
         "@keyframes a{to{transform:rotate(90deg)}}",
         "<button><span id=x style='display:block;animation:a 1s linear'>x</span></button>", "x", true, false,
         FLOW_PARTS_MARKED},
        {"layout parts: an element under display:none has nothing to mark", "@keyframes a{to{color:red}}",
         "<div style='display:none'><p id=x style='animation:a 1s linear'>x</p></div>", "x", true, false,
         FLOW_PARTS_MARKED},
    };
    // Two trees of the page at two widths, its only change a colour drawn
    // in place: the boxes moved, so the compare refuses whatever changed.
    {
        Moving m;
        moving_open(&m, MOTION_STYLE "@keyframes a{to{color:red}}</style>"
                        "<div style='animation:a 1s linear'>a line long enough to wrap at one width and not "
                        "at the other one, which it does</div>", 1000);
        m.p.env.now_ms = 1300;
        flow_tree_t *narrow = flow_layout(m.p.doc, m.p.page, 300, &m.p.env);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        expect("layout parts: none when the boxes moved, said so with a box that moved",
               narrow != NULL && !flow_layout_parts(m.tree, narrow, (flow_point_t){0, 0}, tick_mark, &k, &said) &&
                   said.why == FLOW_PARTS_MOVED && said.node != NULL && said.prop == -1,
               NULL);
        flow_free(narrow);
        moving_close(&m);
    }
    // A tree missing is a layout that stopped short, and a caller that asks
    // no reason is answered all the same.
    {
        Moving m;
        moving_open(&m, MOTION_STYLE "@keyframes a{to{color:red}}</style><div style='animation:a 1s linear'>x</div>",
                    1000);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        bool refused = !flow_layout_parts(NULL, m.tree, (flow_point_t){0, 0}, tick_mark, &k, &said) &&
                       said.why == FLOW_PARTS_INCOMPLETE;
        flow_tree_t *now = moving_beside(&m, 1300);
        expect("layout parts: none for a tree missing, said as stopped short; NULL asks no reason",
               refused && now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, NULL),
               NULL);
        flow_free(now);
        moving_close(&m);
    }
    // One element scrolled in the old tree and not in the new: the boxes are
    // alike but not where they show, said as scrolled, with the scroller.
    {
        Moving m;
        moving_open(&m, MOTION_STYLE "@keyframes a{to{color:red}}</style>"
                        "<div id=s style='overflow:auto;height:40px'><div style='height:400px;animation:a 1s linear'>"
                        "x</div></div>", 1000);
        const os64_html_node_t *s = motion_find(m.p.doc->document, "s");
        int32_t i = flow_scroller_for(m.tree, s);
        if (i >= 0)
            flow_scroll_set(m.tree, i, (flow_point_t){0, 30});
        flow_tree_t *now = moving_beside(&m, 1300);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        bool bounded = now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, &said);
        char detail[64];
        snprintf(detail, sizeof(detail), "scroller %d bounded %d why %d", i, bounded, said.why);
        expect("layout parts: none when a box scrolls elsewhere, said so with its scroller",
               i >= 0 && now != NULL && !bounded && said.why == FLOW_PARTS_SCROLLED && said.node == s, detail);
        flow_free(now);
        moving_close(&m);
    }
    // A box that moves first in a box the page makes for no element (the
    // anonymous block around a text beside a block) is named by the element
    // holding it.
    {
        Moving m;
        moving_open(&m, MOTION_STYLE "@keyframes a{to{font-size:40px}}</style>"
                        "<div id=x style='height:300px'>a <span style='animation:a 1s linear'>w</span>"
                        "<p>b</p></div>", 1000);
        const os64_html_node_t *x = motion_find(m.p.doc->document, "x");
        flow_tree_t *now = moving_beside(&m, 1300);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        bool bounded = now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, &said);
        char detail[64];
        snprintf(detail, sizeof(detail), "bounded %d why %d named %d", bounded, said.why, said.node == x);
        expect("layout parts: a box made for no element that moved is named by the element holding it",
               x != NULL && now != NULL && !bounded && said.why == FLOW_PARTS_MOVED && said.node == x, detail);
        flow_free(now);
        moving_close(&m);
    }
    // A box that becomes positioned halfway (position is discrete) is a
    // stacked box the old tree did not have: the trees paint in another
    // number of layers.
    {
        Moving m;
        moving_open(&m, MOTION_STYLE "@keyframes a{from{position:static}to{position:relative}}</style>"
                        "<div style='animation:a 1s linear'>x</div>", 1000);
        flow_tree_t *now = moving_beside(&m, 1800);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        bool bounded = now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, &said);
        char detail[48];
        snprintf(detail, sizeof(detail), "bounded %d why %d", bounded, said.why);
        expect("layout parts: none when the trees paint in another number of layers, said so",
               now != NULL && !bounded && said.why == FLOW_PARTS_LAYERS, detail);
        flow_free(now);
        moving_close(&m);
    }
    // Two stacked boxes whose order an animated z-index swaps: the layers
    // differ.
    {
        Moving m;
        moving_open(&m, MOTION_STYLE "@keyframes a{to{z-index:9}}</style>"
                        "<div style='position:relative;z-index:1;animation:a 1s linear'>x</div>"
                        "<div style='position:relative;z-index:3'>y</div>", 1000);
        flow_tree_t *now = moving_beside(&m, 1800);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        bool bounded = now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, &said);
        char detail[48];
        snprintf(detail, sizeof(detail), "bounded %d why %d", bounded, said.why);
        expect("layout parts: none when the stacking changed, said as other layers",
               now != NULL && !bounded && said.why == FLOW_PARTS_LAYERS && said.node == NULL, detail);
        flow_free(now);
        moving_close(&m);
    }
    for (size_t i = 0; i < sizeof(kParts) / sizeof(*kParts); i++) {
        char html[1024];
        snprintf(html, sizeof(html), MOTION_STYLE "%s</style>%s", kParts[i].style, kParts[i].body);
        Moving m;
        moving_open(&m, html, 1000);
        flow_tree_t *now = moving_beside(&m, 1300);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        bool bounded = now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, &said);
        const os64_html_node_t *x = motion_find(m.p.doc->document, kParts[i].id);
        os64_gui_rect_t joined = {0, 0, 0, 0};
        bool marks = marks_node(&k, x, &joined);
        char detail[128];
        snprintf(detail, sizeof(detail), "bounded %d marks %d (%d marks, %dx%d at %d,%d) why %d prop %d", bounded,
                 marks, k.n, joined.w, joined.h, joined.x, joined.y, said.why, said.prop);
        bool ok = bounded == kParts[i].bounded && (!kParts[i].bounded || marks == kParts[i].marks);
        // What refused is said: the reason, the element whose change it was,
        // and the property a layout places.
        const os64_html_node_t *refuser = kParts[i].why == FLOW_PARTS_MARKED  ? NULL
                                          : kParts[i].why == FLOW_PARTS_CANVAS ? said.node
                                                                               : x;
        ok = ok && said.why == kParts[i].why && said.node == refuser &&
             said.prop == (kParts[i].why == FLOW_PARTS_PLACED ? (int32_t)GARB_Z_INDEX : -1);
        if (ok && kParts[i].why == FLOW_PARTS_CANVAS)
            ok = said.node != NULL && said.node->tag == OS64_HTML_TAG_BODY;
        // The parts of an element the size of a line are that size, not the view.
        if (ok && marks)
            ok = joined.h < 400;
        // A group's parts reach what it draws outside its own box: the
        // positioned box 400 px across.
        if (ok && strstr(kParts[i].name, "group") != NULL)
            ok = (int64_t)joined.x + joined.w > 400;
        expect(kParts[i].name, ok, detail);
        flow_free(now);
        moving_close(&m);
    }
}

// The parts against the trees themselves: after a layout whose parts are
// handed, every box that paints otherwise in the new tree than in the old —
// any longhand of its style — lies inside one of the marks, with its ink,
// where it was and where it is. A box's ink alone is no test: it holds what
// the boxes inside it reach, so an ancestor's grows with a child's shadow
// and paints nothing new. What the rule handed is not trusted here: the
// trees are compared box by box.
static bool inside_a_mark(const TickMarks *k, os64_gui_rect_t r)
{
    for (int i = 0; i < k->n; i++)
        if (k->r[i].x <= r.x && k->r[i].y <= r.y && (int64_t)k->r[i].x + k->r[i].w >= (int64_t)r.x + r.w &&
            (int64_t)k->r[i].y + k->r[i].h >= (int64_t)r.y + r.h)
            return true;
    return r.w <= 0 || r.h <= 0;
}

static os64_gui_rect_t inked_doc_rect(const flow_box_t *b)
{
    os64_gui_rect_t r = flow_box_doc_rect(b, (flow_point_t){0, 0});
    return (os64_gui_rect_t){r.x - b->ink[FLOW_LEFT], r.y - b->ink[FLOW_TOP],
                             r.w + b->ink[FLOW_LEFT] + b->ink[FLOW_RIGHT],
                             r.h + b->ink[FLOW_TOP] + b->ink[FLOW_BOTTOM]};
}

// The stacked boxes' walk is held to a budget (PARTS_WALK_MAX): a page whose
// stacked boxes nest hundreds deep inside the changed element would make it
// cost more than repainting, so it refuses; one a few deep is bounded.
static void motion_layout_part_depth_cases(void)
{
    static const int kDepth[] = {20, 500};
    for (int d = 0; d < 2; d++) {
        static char html[64000];
        size_t n = (size_t)snprintf(html, sizeof(html), MOTION_STYLE
            "@keyframes a{to{color:red}}#e{animation:a 1s linear}div{position:relative}</style><div id=e>");
        for (int i = 0; i < kDepth[d]; i++)
            n += (size_t)snprintf(html + n, sizeof(html) - n, "<div>");
        n += (size_t)snprintf(html + n, sizeof(html) - n, "x");
        for (int i = 0; i <= kDepth[d]; i++)
            n += (size_t)snprintf(html + n, sizeof(html) - n, "</div>");
        Moving m;
        moving_open(&m, html, 1000);
        flow_tree_t *now = moving_beside(&m, 1300);
        TickMarks k = {.n = 0};
        flow_parts_said_t said = {FLOW_PARTS_COUNT, NULL, 0};
        bool bounded = now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, &said);
        expect(d == 0 ? "layout parts: stacked boxes nested a few deep are bounded"
                      : "layout parts: stacked boxes nested hundreds deep refuse, past the walk's budget, said so",
               now != NULL && bounded == (d == 0) && said.why == (d == 0 ? FLOW_PARTS_MARKED : FLOW_PARTS_DEEP),
               NULL);
        flow_free(now);
        moving_close(&m);
    }
}

static void motion_layout_part_oracle_cases(void)
{
    static const char *const kPages[] = {
        "@keyframes a{to{text-shadow:0 -6px 0 red}}#e{animation:a 1s linear}</style>"
        "<div id=e>one <span>two <b>three</b></span></div>",
        "@keyframes a{to{color:red}}#e{position:relative;animation:a 1s linear}</style>"
        "<div id=e>x<p style='position:absolute;left:300px;top:40px;margin:0'>y</p>"
        "<p style='position:fixed;top:300px;margin:0'>z</p><span>w</span></div>",
        "@keyframes a{to{background-color:blue}}#e{position:relative;animation:a 1s linear}</style>"
        "<div id=e>x<p style='position:absolute;left:300px;margin:0;background:inherit'>y</p></div>",
        "@keyframes a{to{opacity:.4}}#e{animation:a 1s linear}</style>"
        "<div id=e>x<p style='position:absolute;left:300px;margin:0'>y</p></div>",
        "@keyframes a{to{transform:rotate(30deg)}}#e{animation:a 1s linear}</style>"
        "<div id=e>x<p style='position:absolute;left:300px;margin:0'>y</p></div>",
        "@keyframes a{to{border-top-color:red}}#e{border:3px solid blue;animation:a 1s linear}</style>"
        "<div id=e>x</div>",
        "@keyframes a{to{box-shadow:10px 10px 0 4px red}}#e{animation:a 1s linear}</style><div id=e>x</div>",
        "@keyframes a{to{text-shadow:0 -30px 0 red}}#e{animation:a 1s linear}</style>"
        "<div>one <span id=e>two</span> three</div>",
        "@keyframes a{to{color:red}}#e{animation:a 1s linear}</style>"
        "<div><span id=e><div>BLOCK</div>after</span></div>",
    };
    for (size_t i = 0; i < sizeof(kPages) / sizeof(*kPages); i++) {
        char html[1024];
        snprintf(html, sizeof(html), MOTION_STYLE "%s", kPages[i]);
        Moving m;
        moving_open(&m, html, 1000);
        flow_tree_t *now = moving_beside(&m, 1300);
        TickMarks k = {.n = 0};
        bool bounded = now != NULL && flow_layout_parts(m.tree, now, (flow_point_t){0, 0}, tick_mark, &k, NULL);
        int differ = 0, outside = 0;
        const flow_box_t *x = bounded ? flow_root(m.tree) : NULL, *y = bounded ? flow_root(now) : NULL;
        while (x != NULL && y != NULL) {
            if (!f_styles_same(x->style, y->style)) {
                differ++;
                outside += !inside_a_mark(&k, inked_doc_rect(x)) || !inside_a_mark(&k, inked_doc_rect(y));
            }
            if (x->first != NULL) {
                x = x->first;
                y = y->first;
                continue;
            }
            while (x != NULL && x->next == NULL) {
                x = x->parent;
                y = y->parent;
            }
            x = x != NULL ? x->next : NULL;
            y = y != NULL ? y->next : NULL;
        }
        char name[96], detail[96];
        snprintf(name, sizeof(name), "layout parts: every box that paints otherwise is marked (page %zu)", i);
        snprintf(detail, sizeof(detail), "bounded %d, %d boxes differ, %d outside the marks", bounded, differ,
                 outside);
        expect(name, bounded && differ > 0 && outside == 0, detail);
        flow_free(now);
        moving_close(&m);
    }
}

// A tick with each of its allocations failed in turn is not taken, leaves
// the tree as the tick before it left it, and leaks nothing; the next one
// is the layout's.
static void motion_tick_sweep_cases(void)
{
    const flow_point_t o = {0, 0};
    Moving m, lay;
    size_t base_live = live, tried = 0;
    moving_open_for(&m, kTickPage, 0, true);
    moving_open(&lay, kTickPage, 0);
    (void)flow_motion_tick(m.tree, 300, o, NULL, NULL, NULL);
    moving_again(&lay, 300);
    int bad = 0;
    for (size_t at = 1;; at++) {
        TickMarks k = {.n = 0};
        allocations = 0;
        fail_at = at;
        bool taken = flow_motion_tick(m.tree, 300 + (uint64_t)at, o, tick_mark, &k, NULL);
        fail_at = 0;
        bool reached = allocations >= at;
        if (!reached && taken)
            break;
        tried++;
        if (taken) {
            moving_again(&lay, 300 + (uint64_t)at);
            continue;
        }
        for (size_t i = 0; i < sizeof(kTickIds) / sizeof(kTickIds[0]); i++) {
            char why[96];
            if (!box_ticks_as_laid(moving_box(&m, kTickIds[i]), moving_box(&lay, kTickIds[i]), why,
                                   sizeof(why)) && bad++ < 4)
                expect("tick sweep: a tick not taken leaves the tree as it was", false, why);
        }
    }
    (void)flow_motion_tick(m.tree, 5000, o, NULL, NULL, NULL);
    moving_again(&lay, 5000);
    for (size_t i = 0; i < sizeof(kTickIds) / sizeof(kTickIds[0]); i++) {
        char why[96];
        if (!box_ticks_as_laid(moving_box(&m, kTickIds[i]), moving_box(&lay, kTickIds[i]), why, sizeof(why)))
            expect("tick sweep: the tick after a failed one is the layout's", false, why);
    }
    printf("libflow ticks: each of %zu allocations failed in turn, and every one after it\n", tried);
    moving_close(&m);
    moving_close(&lay);
    expect("tick sweep: failed ticks leak nothing", live == base_live, NULL);
    // A tick not taken leaves the holds as the tick before it found them:
    // `h` holds still at 100 (due at 400) and moves at 500; `t` holds one
    // list for ever (due at its iteration's end), worked after `h`, and
    // its list finds no memory.
    static const char kHolds[] = MOTION_STYLE
        "@keyframes hold{0%,40%{opacity:0}60%,to{opacity:1}}"
        "@keyframes t{from,to{transform:translateX(9px) rotate(9deg)}}"
        "#h{animation:hold 1s linear infinite}#t{animation:t 1s linear infinite}"
        "</style><div id=h></div><div id=t></div>";
    moving_open_for(&m, kHolds, 0, true);
    (void)flow_motion_tick(m.tree, 100, o, NULL, NULL, NULL);
    allocations = 0;
    fail_at = 1;
    bool taken = flow_motion_tick(m.tree, 500, o, NULL, NULL, NULL);
    fail_at = 0;
    const flow_box_t *hb = moving_box(&m, "h");
    expect("tick sweep: a tick not taken leaves the holds as they were",
           !taken && hb != NULL && hb->style->opacity == 0 &&
               flow_motion_tick_due(m.tree, kTickView, o) == 400, NULL);
    moving_close(&m);
}


// A page of `coarse` filler elements that each spend 32 transform
// functions of the lists' budget and `fine` that each spend one background
// position, then #p, animated by `keyframes` for ever.
static char *budget_page(int coarse, int fine, const char *keyframes)
{
    size_t cap = (size_t)(coarse + fine) * 24 + strlen(keyframes) + 32 * 17 + 1024, at = 0;
    char *html = malloc(cap);
    at += (size_t)snprintf(html + at, cap - at, MOTION_STYLE ".s{transform:");
    for (int i = 0; i < 32; i++)
        at += (size_t)snprintf(html + at, cap - at, "translateX(1px) ");
    at += (size_t)snprintf(html + at, cap - at,
                           "}.f{background-position-x:1px}%s#p{animation:k 1s linear infinite}</style>",
                           keyframes);
    for (int i = 0; i < coarse; i++)
        at += (size_t)snprintf(html + at, cap - at, "<div class=s></div>");
    for (int i = 0; i < fine; i++)
        at += (size_t)snprintf(html + at, cap - at, "<div class=f></div>");
    (void)snprintf(html + at, cap - at, "<div id=p></div>");
    return html;
}

// What styling budget_page leaves of #p for a face that ticks, at 100 ms:
// whether it animates at all (its `animation` lists are the budget's too),
// whether it ticks, whether its mix dropped a list, what styling left of
// the budget and what #p's own mix spent — a tick may spend the two.
typedef struct {
    bool animates, ticks, dropped;
    size_t left, mixed;
} BudgetSeen;

static BudgetSeen budget_of(int coarse, int fine, const char *keyframes)
{
    char *html = budget_page(coarse, fine, keyframes);
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    flow_timeline_t *tl = flow_timeline_new(p.doc);
    p.env.timeline = tl;
    p.env.now_ms = 100;
    p.env.ticks = true;
    FStyles *fs = f_style_build(p.doc, p.page, &p.env);
    const FStyled *st = fs != NULL ? f_style_of(fs, motion_find(p.doc->document, "p")) : NULL;
    BudgetSeen seen = {false, false, false, 0, 0};
    if (st != NULL && st->motion != NULL) {
        seen.animates = true;
        seen.ticks = st->motion->ticks;
        seen.dropped = st->motion->anims[0].dropped.w[GARB_BACKGROUND_POSITION_X / 64] != 0;
        seen.left = fs->lists_left;
        seen.mixed = st->motion->mixed;
    }
    f_style_free(fs);
    flow_timeline_free(tl);
    sheeted_close(&p);
    free(html);
    return seen;
}

// Whether `s` is on the near side of the edge budget_edge looks for: #p
// ticks (`ticks`), or it animates and its mix is whole, with at least
// `left` of the budget left besides.
static bool budget_near(BudgetSeen s, bool ticks, size_t left)
{
    return (ticks ? s.ticks : s.animates && !s.dropped) && s.left >= left;
}

// The most filler, coarse and then fine, that leaves `ticks` (or, when it
// is false, that leaves #p's mix whole); false when more than a page holds
// never does. The coarse filler is held to leaving 64KB, more than one of
// it spends: past where a filler's own list finds no room it spends
// nothing, and more of it would change nothing.
static bool budget_edge(const char *keyframes, bool ticks, int *coarse, int *fine)
{
    int at[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        size_t left = k == 0 ? (size_t)64 << 10 : 0;
        int lo = 0, hi = 1;
        for (;;) {
            at[k] = hi;
            BudgetSeen s = budget_of(at[0], at[1], keyframes);
            if (!budget_near(s, ticks, left))
                break;
            lo = hi;
            hi *= 2;
            if (hi > (1 << 17))
                return false;
        }
        while (hi - lo > 1) {
            at[k] = lo + (hi - lo) / 2;
            BudgetSeen s = budget_of(at[0], at[1], keyframes);
            if (budget_near(s, ticks, left))
                lo = at[k];
            else
                hi = at[k];
        }
        at[k] = lo;
    }
    *coarse = at[0];
    *fine = at[1];
    return true;
}

// A tick's lists are held to what the layout's budget left them, and what
// its own mix spent: at the most filler that leaves #p ticking, a tick in
// the first half — a list of eight, as the layout mixed — is taken, and one
// in the second — 1000, 8 and 125 items mixed to their least common
// multiple, which the layout never made — is not, and the tree is as it
// was. And a list the layout's mix dropped keeps its element from ticking.
static void motion_tick_room_cases(void)
{
    static const char kFar[] = "@keyframes k{0%{background-position-x:1px,1px,1px,1px,1px,1px,1px,1px}"
                               "50%{background-position-x:2px,2px,2px,2px,2px,2px,2px,2px}"
                               "to{background-position-x:0px";
    size_t cap = sizeof(kFar) + 125 * 4 + 8;
    char *keyframes = malloc(cap);
    size_t at = (size_t)snprintf(keyframes, cap, "%s", kFar);
    for (int i = 1; i < 125; i++)
        at += (size_t)snprintf(keyframes + at, cap - at, ",%dpx", i % 10);
    (void)snprintf(keyframes + at, cap - at, "}}");
    int coarse = 0, fine = 0;
    bool found = budget_edge(keyframes, true, &coarse, &fine);
    BudgetSeen seen = budget_of(coarse, fine, keyframes);
    size_t room = seen.left + seen.mixed, need = 1000 * sizeof(flow_length_t);
    char detail[128];
    snprintf(detail, sizeof(detail), "%d and %d filler leave %zu, its mix spent %zu", coarse, fine, seen.left,
             seen.mixed);
    expect("tick room: filler that leaves #p ticking with less than its own mix besides",
           found && seen.ticks && seen.left < seen.mixed, detail);
    char *html = budget_page(coarse, fine, keyframes);
    Moving m;
    moving_open_for(&m, html, 100, true);
    const flow_point_t o = {0, 0};
    // Far below the filler, and a position waits out of view: the whole page.
    const os64_gui_rect_t page = {0, 0, 800, INT32_MAX / 2};
    expect("tick room: it ticks", flow_motion_tick_due(m.tree, page, o) == 100, NULL);
    expect("tick room: a tick needing what the layout's mix spent is taken",
           flow_motion_tick(m.tree, 300, o, NULL, NULL, NULL), NULL);
    const flow_style_t *s = moving_style(&m, "p");
    flow_length_t at300 = s != NULL && s->backgrounds.nx == 8 ? s->backgrounds.x[0] : (flow_length_t){0};
    expect("tick room: a tick that needs more room than the layout left is not taken",
           room < need && !flow_motion_tick(m.tree, 700, o, NULL, NULL, NULL), NULL);
    s = moving_style(&m, "p");
    expect("tick room: and the tree is as the last tick left it",
           s != NULL && s->backgrounds.nx == 8 && memcmp(&s->backgrounds.x[0], &at300, sizeof(at300)) == 0, NULL);
    // One item of filler more and the keyframe of 125 finds no room where
    // the layout resolves it, though the mix does: laid out.
    BudgetSeen past = budget_of(coarse, fine + 1, keyframes);
    expect("tick room: a keyframe whose list finds no room keeps its element from ticking",
           past.animates && !past.dropped && !past.ticks, NULL);
    printf("libflow ticks: %d and %d filler elements leave a tick %zu bytes\n", coarse, fine, room);
    moving_close(&m);
    free(html);
    free(keyframes);
    // Two keyframes of 2 and 3 items, both resolved where they are mixed,
    // mixed as 6: one item of filler more than leaves the mix whole drops
    // it, and the element is laid out.
    static const char kSix[] = "@keyframes k{from{background-position-x:1px,2px}"
                               "to{background-position-x:3px,4px,5px}}";
    found = budget_edge(kSix, false, &coarse, &fine);
    BudgetSeen whole = budget_of(coarse, fine, kSix), dropped = budget_of(coarse, fine + 1, kSix);
    expect("tick room: a list the layout's mix drops keeps its element from ticking",
           found && whole.ticks && dropped.dropped && !dropped.ticks, NULL);
}

static void motion_tick_cases(void)
{
    motion_tick_layout_cases();
    motion_tick_condition_cases();
    motion_tick_edge_cases();
    motion_tick_mark_cases();
    motion_tick_sweep_cases();
    motion_tick_room_cases();
}

static void motion_sweep(void)
{
    motion_sweep_in(false, false);
    motion_sweep_in(true, false);
    motion_sweep_in(false, true);
    motion_sweep_failed();
    motion_incomplete_cases();
}

static void motion_no_visit(void *ctx, const flow_box_t *b)
{
    (void)ctx;
    (void)b;
}

static void motion_count_ring(void *ctx, const flow_box_t *b)
{
    (void)b;
    (*(int32_t *)ctx)++;
}

// Slice 4's longhands under @keyframes (MODERN_WEB_BRIEFS.md § Slice 4):
// each animatable by its computed value mixes halfway as its type does —
// a length, a colour, a position, a <ratio> by its logarithm (Values 4 §
// 5.7.1) — and a keyword is discrete, the first value until halfway.
// Paused at half of a linear second.
static void motion_batch_a_cases(void)
{
    static const char kPage[] =
        "<!doctype html><style>div { animation: m 1s linear -500ms paused }"
        " #k { animation-delay: -250ms }"
        " @keyframes m { from { letter-spacing: 0; outline: 2px solid #ff0000;"
        " outline-offset: 0; object-position: 0% 0%; aspect-ratio: 1; cursor: pointer;"
        " word-break: normal }"
        " to { letter-spacing: 10px; outline: 6px solid #0000ff; outline-offset: 4px;"
        " object-position: 100% 100%; aspect-ratio: 4; cursor: wait; word-break: break-all } }"
        "</style><div id=h>a</div><div id=k>b</div>";
    Moving m;
    moving_open(&m, kPage, 0);
    const flow_style_t *h = moving_style(&m, "h"), *k = moving_style(&m, "k");
    expect("animation: letter-spacing halfway, 5px", h != NULL && h->letter_spacing == 5 * 64, NULL);
    expect("animation: outline-width and -offset halfway",
           h != NULL && h->outline_width == 4 * 64 && h->outline_offset == 2 * 64 &&
               h->outline_style == FLOW_BORDER_SOLID, NULL);
    expect("animation: outline-color halfway",
           h != NULL && h->outline_color == 0x800080u, NULL);
    expect("animation: object-position halfway, 50% 50%",
           h != NULL && h->object_position[0].kind == FLOW_LENGTH_PERCENT &&
               h->object_position[0].value == 50 * 64 && h->object_position[1].value == 50 * 64,
           NULL);
    expect("animation: aspect-ratio halfway by its logarithm, 2",
           h != NULL && fabs(h->aspect_ratio - 2) < 1e-9, NULL);
    expect("animation: a keyword is the second value from halfway",
           h != NULL && h->cursor == FLOW_CURSOR_WAIT && h->word_break == FLOW_WORD_BREAK_BREAK_ALL,
           NULL);
    expect("animation: and the first before it",
           k != NULL && k->cursor == FLOW_CURSOR_POINTER && k->word_break == FLOW_WORD_BREAK_NORMAL &&
               k->letter_spacing == 160, NULL);
    moving_close(&m);
    // An outline whose style is none has no width, whatever is animating
    // the width; a style animated to solid brings the width's own, medium
    // when nothing declares one.
    moving_open(&m, "<!doctype html><style>div { animation: n 1s linear -750ms paused }"
                    " @keyframes n { from { outline-style: none } to { outline-style: solid } }"
                    "</style><div id=s>a</div>", 0);
    const flow_style_t *s = moving_style(&m, "s");
    expect("animation: an outline style animated to solid draws medium",
           s != NULL && s->outline_style == FLOW_BORDER_SOLID && s->outline_width == 3 * 64, NULL);
    moving_close(&m);
    // A tick grows the ink of what it changed and leaves standing what the
    // layout found below it: an outline under an element whose shadow
    // ticks is still handed to the outline step after the tick.
    moving_open_for(&m, MOTION_STYLE "@keyframes s{from{box-shadow:0 0 0 red}"
                        "to{box-shadow:0 0 10px red}}#p{animation:s 1s linear}"
                        "#c{outline:1px solid;height:10px}</style><div id=p><div id=c></div></div>",
                    0, true);
    bool ticked = flow_motion_tick(m.tree, 500, (flow_point_t){0, 0}, NULL, NULL, NULL);
    int32_t rings = 0;
    flow_visitor_t vis = {motion_no_visit, NULL, NULL, &rings, motion_count_ring};
    flow_visit_groups(m.tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0}, &vis);
    expect("tick: an outline below a ticked shadow is still painted", ticked && rings == 1, NULL);
    moving_close(&m);
}

static void motion_cases(void)
{
    motion_batch_a_cases();
    motion_easing_cases();
    motion_chrome_cases();
    motion_sparse_cases();
    motion_important_budget_cases();
    motion_cascade_cases();
    motion_effect_cases();
    motion_timeline_cases();
    motion_due_cases();
    motion_fuzz_cases();
    motion_list_case_cases();
    motion_dropped_cases();
    motion_held_time_cases();
    motion_tick_cases();
    motion_reach_budget_cases();
    motion_layout_part_cases();
    motion_layout_part_depth_cases();
    motion_layout_part_oracle_cases();
    motion_sweep();
}
