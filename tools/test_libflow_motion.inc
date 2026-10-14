// tools/test_libflow_motion.inc — animations, the model (PILE3.md
// § Animations, slice S8a), included by test_libflow_host.c. Every
// expected value is headless Chrome 155's answer to the probe pages of
// 10/13 — animations paused at a negative delay, read back through
// getComputedStyle — or worked by hand from Web Animations 1, CSS
// Animations 1 and 2, Easing 1 and Transforms 1 and 2, never captured from
// the engine. A length lands on 1/64 px (the contracts), so a translation
// is held to Chrome's within half of that.

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

static void moving_open(Moving *m, const char *html, uint64_t now)
{
    sheeted_open(&m->p, html, 800, 600);
    m->tl = flow_timeline_new(m->p.doc);
    m->p.env.timeline = m->tl;
    m->p.env.now_ms = now;
    m->tree = flow_layout(m->p.doc, m->p.page, 800, &m->p.env);
}

// The page laid out again at `now`, its timeline kept.
static void moving_again(Moving *m, uint64_t now)
{
    flow_free(m->tree);
    m->p.env.now_ms = now;
    m->tree = flow_layout(m->p.doc, m->p.page, 800, &m->p.env);
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
    // 0 and 1 at 1, exactly — where the power form's rounding misses 1 by
    // 2e-10 for cubic-bezier(0, 0.1, 1, 1e6), and by more below.
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
    flow_visitor_t v = {motion_group_visit, motion_group_open, NULL, &seen};
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

// ── The timeline ────────────────────────────────────────────────────────

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

    // Animations 1 § 2's matching, and the records' lives, told directly:
    // `a` to `a, a` keeps `a` as the second and starts a first; a name
    // gone is dropped by a complete layout and not by an incomplete one;
    // the holds are let go (a document freed with one held would end the
    // program).
    os64_html_document_t *doc = parse("<!doctype html><p id=x>x</p><p id=y>y</p><p id=z>z</p>");
    const os64_html_node_t *x = motion_find(doc->document, "x");
    const os64_html_node_t *y = motion_find(doc->document, "y");
    const os64_html_node_t *z = motion_find(doc->document, "z");
    flow_timeline_t *tl = flow_timeline_new(doc);
    FAnimation an[2];
    memset(an, 0, sizeof(an));
    an[0].timing.name = an[1].timing.name = (flow_name_t){"a", 1};
    FMotion one = {.anims = an, .nanims = 1};
    f_timeline_begin(tl);
    bool ok = f_timeline_element(tl, x, &one, 1000);
    f_timeline_end(tl, true);
    FRecord *first = an[0].record;
    FMotion two = {.anims = an, .nanims = 2};
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, x, &two, 2000);
    f_timeline_end(tl, true);
    expect("motion: a to a, a keeps a as the second",
           ok && an[1].record == first && an[0].record != first && an[0].record != NULL, NULL);
    // The element styled with nothing in an incomplete layout keeps its
    // records; in a complete one they go.
    an[0].record = an[1].record = NULL;
    f_timeline_begin(tl);
    ok = f_timeline_element(tl, y, &one, 3000);
    f_timeline_end(tl, false);
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, x, &one, 4000);
    f_timeline_end(tl, true);
    expect("motion: an incomplete layout drops nothing, a complete one the rest",
           ok && an[0].record == first && f_timeline_records(tl) == 1, NULL);
    // A complete layout whose cleanup has no room drops nothing, but what
    // it did not meet is gone all the same: a name it did not meet, written
    // again later, starts anew — its one second due from the later start,
    // not over long since — and the next cleanup drops the old record.
    FAnimation g;
    memset(&g, 0, sizeof(g));
    g.timing.name = (flow_name_t){"g", 1};
    g.timing.duration_ms = 1000;
    g.timing.iterations = 1;
    f_props_add(&g.props, GARB_OPACITY);
    FMotion gm = {.anims = &g, .nanims = 1};
    flow_timeline_t *gt = flow_timeline_new(doc);
    f_timeline_begin(gt);
    ok = f_timeline_element(gt, z, &gm, 20000);
    f_timeline_end(gt, true);
    f_timeline_begin(gt);
    allocations = 0;
    fail_single = true;
    fail_at = 1;
    f_timeline_end(gt, true);
    fail_at = 0;
    fail_single = false;
    int32_t unfreed = f_timeline_records(gt);
    g.record = NULL;
    f_timeline_begin(gt);
    ok = ok && f_timeline_element(gt, z, &gm, 30000);
    f_timeline_end(gt, true);
    expect("motion: a name a cleanup had no room to drop starts anew written again",
           ok && unfreed == 1 && f_motion_next(&gm, 30000) == 31000 && f_timeline_records(gt) == 1, NULL);
    flow_timeline_free(gt);

    // An incomplete layout whose tree the face discards unshown is undone:
    // the animation it paused runs on from its start, the one it started
    // goes — and only that layout: undone once, or after a complete one,
    // there is nothing to undo.
    FAnimation ua[2];
    memset(ua, 0, sizeof(ua));
    ua[0].timing.name = (flow_name_t){"r", 1};
    ua[1].timing.name = (flow_name_t){"s", 1};
    for (int i = 0; i < 2; i++) {
        ua[i].timing.duration_ms = 1000;
        ua[i].timing.iterations = 1;
        f_props_add(&ua[i].props, GARB_OPACITY);
    }
    FMotion uone = {.anims = ua, .nanims = 1}, utwo = {.anims = ua, .nanims = 2};
    flow_timeline_t *ut = flow_timeline_new(doc);
    f_timeline_begin(ut);
    ok = f_timeline_element(ut, z, &uone, 40000);
    f_timeline_end(ut, true);
    bool after_complete = flow_timeline_undo_incomplete(ut);
    FRecord *kept_r = ua[0].record;
    ua[0].timing.paused = true;
    f_timeline_begin(ut);
    ok = ok && f_timeline_element(ut, z, &utwo, 40500);
    f_timeline_end(ut, false);
    bool discarded = flow_timeline_undo_incomplete(ut), twice = flow_timeline_undo_incomplete(ut);
    ua[0].timing.paused = false;
    expect("motion: an incomplete layout discarded unshown is undone, and only it",
           ok && !after_complete && discarded && !twice && f_timeline_records(ut) == 1 &&
               ua[0].record == kept_r && f_motion_next(&uone, 40600) == 41000,
           NULL);
    flow_timeline_free(ut);
    // A paused animation holds its time — nothing is due while it is — and
    // resumed, it runs from there: started paused at 5000, resumed at
    // 9000, its one second ends at 10000. (Each animation here sets
    // opacity, which it holds still: one that sets nothing is due for
    // nothing.)
    FAnimation p;
    memset(&p, 0, sizeof(p));
    p.timing.name = (flow_name_t){"p", 1};
    f_props_add(&p.props, GARB_OPACITY);
    p.timing.duration_ms = 1000;
    p.timing.iterations = 1;
    p.timing.paused = true;
    FMotion paused = {.anims = &p, .nanims = 1};
    f_timeline_begin(tl);
    ok = f_timeline_element(tl, y, &paused, 5000);
    f_timeline_end(tl, true);
    expect("motion: paused, nothing is due", ok && isinf(f_motion_next(&paused, 5000)), NULL);
    p.timing.paused = false;
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, y, &paused, 9000);
    f_timeline_end(tl, true);
    expect("motion: resumed, it runs from where it was held",
           ok && f_motion_next(&paused, 9000) == 10000, NULL);
    // One running before it pauses holds what it has run: started at
    // 11000, paused at 11300, nothing is due at 11500; resumed at 13000,
    // its one second ends at 13700.
    FAnimation r;
    memset(&r, 0, sizeof(r));
    r.timing.name = (flow_name_t){"r", 1};
    f_props_add(&r.props, GARB_OPACITY);
    r.timing.duration_ms = 1000;
    r.timing.iterations = 1;
    FMotion running = {.anims = &r, .nanims = 1};
    f_timeline_begin(tl);
    ok = f_timeline_element(tl, y, &running, 11000);
    f_timeline_end(tl, true);
    r.timing.paused = true;
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, y, &running, 11300);
    f_timeline_end(tl, true);
    expect("motion: paused while running, nothing is due",
           ok && isinf(f_motion_next(&running, 11500)), NULL);
    r.timing.paused = false;
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, y, &running, 13000);
    f_timeline_end(tl, true);
    expect("motion: resumed, it ends what it held short of",
           ok && f_motion_next(&running, 13000) == 13700, NULL);

    // A record made in an incomplete layout joins its element's chain
    // beside the ones it had, though no complete layout rebuilds the
    // chains after it: a, then a and b incomplete, then a again keeps the
    // first a.
    FAnimation two_names[2];
    memset(two_names, 0, sizeof(two_names));
    two_names[0].timing.name = (flow_name_t){"a", 1};
    two_names[1].timing.name = (flow_name_t){"b", 1};
    FMotion just_a = {.anims = two_names, .nanims = 1};
    FMotion a_and_b = {.anims = two_names, .nanims = 2};
    f_timeline_begin(tl);
    ok = f_timeline_element(tl, x, &just_a, 15000);
    f_timeline_end(tl, true);
    FRecord *kept_a = two_names[0].record;
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, x, &a_and_b, 16000);
    f_timeline_end(tl, false);
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, x, &just_a, 17000);
    f_timeline_end(tl, true);
    expect("motion: a record made in an incomplete layout joins its element's chain",
           ok && two_names[0].record == kept_a, NULL);

    // A layout that fails is undone: what it paused runs on from where it
    // ran, what it made goes, and the chains it grew end where they did —
    // x's at its a, z's at none, its key gone from the map — so the next
    // layout makes b and z's a again, and keeps the rest.
    FAnimation u;
    memset(&u, 0, sizeof(u));
    u.timing.name = (flow_name_t){"u", 1};
    f_props_add(&u.props, GARB_OPACITY);
    u.timing.duration_ms = 1000;
    u.timing.iterations = 1;
    FMotion undone = {.anims = &u, .nanims = 1};
    f_timeline_begin(tl);
    ok = f_timeline_element(tl, y, &undone, 18000) && f_timeline_element(tl, x, &just_a, 18000);
    f_timeline_end(tl, true);
    int32_t kept_records = f_timeline_records(tl), kept_elements = f_timeline_elements(tl);
    FRecord *kept_u = u.record;
    kept_a = two_names[0].record;
    u.timing.paused = true;
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, y, &undone, 18200) && f_timeline_element(tl, x, &a_and_b, 18200) &&
         f_timeline_element(tl, z, &one, 18200);
    bool grew = f_timeline_records(tl) == kept_records + 2;
    f_timeline_undo(tl);
    u.timing.paused = false;
    u.record = kept_u;
    expect("motion: a layout undone leaves what it paused running, and makes nothing",
           ok && grew && kept_records == 2 && f_timeline_records(tl) == kept_records &&
               kept_elements == 2 && f_timeline_elements(tl) == kept_elements &&
               f_motion_next(&undone, 18500) == 19000, NULL);
    f_timeline_begin(tl);
    ok = f_timeline_element(tl, y, &undone, 18300) && f_timeline_element(tl, x, &a_and_b, 18300) &&
         f_timeline_element(tl, z, &one, 18300);
    f_timeline_end(tl, true);
    expect("motion: after an undone layout, the chains it grew are made again",
           ok && u.record == kept_u && two_names[0].record == kept_a && two_names[1].record != NULL &&
               an[0].record != NULL && f_timeline_records(tl) == 4, NULL);
    // The undoing allocates nothing, and removing a key leaves no trace:
    // puts and removals at random over a pool of keys, every key found
    // exactly when the reference holds it, the count the reference's, and
    // no removal allocating — the map one key short of growing throughout.
    FMap map = {0};
    static int pool[48];
    bool held[48] = {false};
    int32_t holding = 0;
    bool agreed = true;
    uint32_t seed = 12345;
    fail_single = false;
    for (int step = 0; step < 20000 && agreed; step++) {
        seed = seed * 1103515245u + 12345u;
        int k = (int)((seed >> 8) % 48);
        if (held[k]) {
            allocations = 0;
            fail_at = 1;
            f_map_remove(&map, &pool[k]);
            fail_at = 0;
            held[k] = false;
            holding--;
        } else {
            agreed = f_map_put(&map, &pool[k], &pool[k]);
            held[k] = true;
            holding++;
        }
        for (int i = 0; i < 48 && agreed; i++)
            agreed = f_map_get(&map, &pool[i]) == (held[i] ? &pool[i] : NULL);
        agreed = agreed && (int32_t)map.count == holding && map.cap == 64;
    }
    expect("motion: a removed key leaves every other key found, and nothing behind", agreed, NULL);
    f_map_free(&map);

    // Matching at scale: three thousand names, a and b in turn, matched
    // as Animations 1 § 2 says — the last of each name to the last record
    // of its name — and a list that drops its first half keeps the
    // records of the half it keeps, in their places; the rest go.
    enum { kMany = 3000 };
    FAnimation *many = calloc(kMany, sizeof(*many));
    FRecord **first_records = calloc(kMany, sizeof(*first_records));
    for (int i = 0; i < kMany; i++)
        many[i].timing.name = (flow_name_t){i % 2 ? "b" : "a", 1};
    FMotion lots = {.anims = many, .nanims = kMany};
    f_timeline_begin(tl);
    ok = f_timeline_element(tl, x, &lots, 20000);
    f_timeline_end(tl, true);
    for (int i = 0; i < kMany; i++)
        first_records[i] = many[i].record;
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, x, &lots, 21000);
    f_timeline_end(tl, true);
    bool kept = ok && f_timeline_records(tl) == kMany;
    for (int i = 0; kept && i < kMany; i++)
        kept = many[i].record == first_records[i];
    FMotion half = {.anims = many + kMany / 2, .nanims = kMany / 2};
    f_timeline_begin(tl);
    ok = ok && f_timeline_element(tl, x, &half, 22000);
    f_timeline_end(tl, true);
    for (int i = 0; kept && i < kMany / 2; i++)
        kept = many[kMany / 2 + i].record == first_records[kMany / 2 + i];
    expect("motion: three thousand names matched, and half of them dropped",
           kept && f_timeline_records(tl) == kMany / 2, NULL);
    free(many);
    free(first_records);
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
        {"motion due: steps at the next edge", "mv 1s steps(4,end)", 1250},
        {"motion due: paused, never", "mv 1s linear paused", FLOW_NEVER},
        {"motion due: in its delay, its start", "mv 1s linear 500ms", 1500},
        {"motion due: a discrete value at its flip", "ff 1s linear", 1500},
        {"motion due: ended and filling, never", "mv 1s linear forwards -2s", FLOW_NEVER},
        {"motion due: hidden to visible, the millisecond after its start", "hv 1s linear", 1001},
        {"motion due: visible to hidden, its end", "vh 1s linear", 2000},
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE
                 "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
                 "@keyframes ff{from{font-family:serif}to{font-family:monospace}}"
                 "@keyframes hv{from{visibility:hidden}to{visibility:visible}}"
                 "@keyframes vh{from{visibility:visible}to{visibility:hidden}}"
                 "</style><div style='animation:%s'>x</div>", kCases[i].animation);
        Moving m;
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[64];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kCases[i].name, due == kCases[i].due, due == kCases[i].due ? NULL : detail);
        moving_close(&m);
    }
    // steps(2147483647): the next edge is worked out, not walked to — a
    // walk is two billion turns on the window's thread.
    Moving m;
    moving_open(&m, MOTION_STYLE
        "@keyframes c{to{color:red}}"
        "</style><div style='animation:c 1s steps(2147483647)'>x</div>", 1000);
    expect("motion due: the next of two billion steps, at once",
           flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) == 1001, NULL);
    moving_close(&m);

    // Running backwards the progress falls through each edge, and the
    // step below shows just past it: steps(2, end) reversed is at its
    // halfway value at 500 ms, at its first just after.
    moving_open(&m, MOTION_STYLE
        "@keyframes o{from{opacity:0}to{opacity:1}}"
        "</style><div id=o style='animation:o 1s steps(2,end) reverse'>x</div>", 1000);
    moving_again(&m, 1500);
    const flow_style_t *os = moving_style(&m, "o");
    uint64_t odue = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
    bool half = os != NULL && os->opacity == 500 && odue == 1501;
    moving_again(&m, odue);
    os = moving_style(&m, "o");
    expect("motion due: reversed, a step falls the millisecond after its edge",
           half && os != NULL && os->opacity == 0, NULL);
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
    // An animation resting on the value underneath, still where it rests
    // on its own end, replaces nothing: the one under it is due at its
    // step, where the value under the later one moves.
    moving_open(&m, MOTION_STYLE
        "@keyframes up{from{opacity:0}to{opacity:1}}@keyframes dn{to{opacity:0}}"
        "</style><div id=ov style='animation:up 1s steps(2, end),dn 1s linear'>x</div>", 1000);
    expect_u32("motion due: the one under an implicit keyframe is due for its own change",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1500);
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
    expect_u32("motion due: a transform that cannot be undone flips at one half",
               (uint32_t)flow_motion_due(m.tree, kView, (flow_point_t){0, 0}), 1500);
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

    // An easing that overshoots: visible to hidden through a curve that
    // passes 1 early turns hidden there, not at the keyframe. The crossing
    // is found here from the easing itself, scanned, and the page is due
    // when it shows: the next millisecond, as it rises through 1.
    static const struct {
        const char *name, *keyframes, *easing;
        double level;
        bool hidden_after;
    } kCross[] = {
        {"motion due: visibility hidden where an overshoot passes 1",
         "@keyframes vh{from{visibility:visible}to{visibility:hidden}}", "cubic-bezier(0,2,1,2)", 1, true},
    };
    for (size_t i = 0; i < sizeof(kCross) / sizeof(kCross[0]); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE "%s</style><div id=x style='animation:%s 1s %s'>x</div>",
                 kCross[i].keyframes, i == 0 ? "vh" : "ff", kCross[i].easing);
        flow_easing_t e = {.kind = FLOW_EASE_CUBIC};
        sscanf(kCross[i].easing, "cubic-bezier(%lf,%lf,%lf,%lf)", &e.p[0], &e.p[1], &e.p[2], &e.p[3]);
        double crossings[4];
        int nc = 0;
        double prev = f_ease(&e, 0, false) - kCross[i].level;
        for (int k = 1; k < 1000000 && nc < 4; k++) {
            double y = f_ease(&e, k / 1e6, false) - kCross[i].level;
            if ((prev < 0 && y >= 0) || (prev > 0 && y <= 0))
                crossings[nc++] = k / 1e6 * 1000;
            prev = y;
        }
        moving_open(&m, html, 1000);
        bool ok = nc == 1;
        uint64_t now = 1000;
        for (int k = 0; ok && k < nc; k++) {
            uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
            uint64_t want = (uint64_t)floor(1000 + crossings[k]) + 1;
            // The scan finds a crossing to a microsecond; the curve's own
            // root may sit the other side of a millisecond's edge.
            ok = due + 1 >= want && due <= want + 1 && due > now;
            if (ok && i == 0) {
                const flow_style_t *st = moving_style(&m, "x");
                ok = st != NULL && st->visibility == FLOW_VISIBLE;
                moving_again(&m, due);
                st = moving_style(&m, "x");
                ok = ok && st != NULL && st->visibility == FLOW_HIDDEN;
            } else if (ok) {
                moving_again(&m, due);
            }
            now = due;
        }
        char detail[96];
        snprintf(detail, sizeof(detail), "%d crossings, first at %.3f ms", nc, nc > 0 ? crossings[0] : -1.0);
        expect(kCross[i].name, ok, ok ? NULL : detail);
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
// and every one after it fail.
static void motion_sweep_in(bool single)
{
    static const char kHtml[] = MOTION_STYLE
        "@keyframes mv{from{transform:translateX(0);opacity:0}"
        "to{transform:translateX(100px) rotate(30deg);opacity:1}}"
        "@keyframes col{50%{color:red;margin-left:var(--m)}to{background-color:blue}}"
        "#a{--m:30px;animation:mv 1s linear infinite,col 2s steps(3) both}"
        "#b{animation:mv 700ms ease-in-out 200ms 3 alternate paused}"
        "#in{display:inline-block;animation:col 1s 100ms backwards}"
        "</style><div id=a>moving</div><p id=b>held <span id=in>inside</span></p>";
    Moving m;
    moving_open(&m, kHtml, 0);
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
        m.tl = m.p.env.timeline = flow_timeline_new(m.p.doc);
        allocations = 0;
        fail_at = at;
        m.tree = flow_layout(m.p.doc, m.p.page, 800, &m.p.env);
        fail_at = 0;
        // Fewer allocations than the one set to fail: none failed.
        bool reached = allocations >= at;
        flow_free(m.tree);
        m.tree = NULL;
        flow_timeline_free(m.tl);
        m.tl = m.p.env.timeline = NULL;
        made++;
        expect("motion sweep: a first layout that fails leaks nothing", live == base_live, NULL);
        failed |= live != base_live;
        if (!reached)
            break;
    }
    fail_single = false;
    printf("libflow motion sweep: each of %zu allocations failed in turn%s (%zu with the records made), "
           "nothing leaked\n", tried, single ? " alone" : ", and every one after it", made);
    free(whole);
    moving_close(&m);
}

// A layout that fails part way — each of its allocations failing in turn,
// and every one after it — is undone: the tree still shown keeps running
// what the failed layout paused, no record it made is kept, and the next
// layout finds the timeline as the shown tree left it (x, running since
// 1000, is 4 s into its 10 at 5000; paused at 3000 and resumed, it would
// be 2).
static void motion_sweep_undo(void)
{
    static const char kUndoPage[] = MOTION_STYLE
        "@keyframes k{from{opacity:0}to{opacity:1}}"
        "#x{animation:k 10s linear}"
        "@media (prefers-reduced-motion:reduce){#x{animation-play-state:paused}#y{animation:k 10s linear}}"
        "</style><div id=x>x</div><div id=y>y</div>";
    static const os64_gui_rect_t kView = {0, 0, 800, 600};
    size_t tried = 0;
    bool failed = false;
    for (size_t at = 1; !failed; at++) {
        Moving m;
        moving_open(&m, kUndoPage, 1000);
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
    printf("libflow motion sweep: %zu failed layouts undone\n", tried);
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
// its keyframes into `frames` — one to five, at offsets that may repeat,
// several at 1 among them: true when every one holds the element's own
// value, so that the animation, whatever else it does, stands still.
static bool fuzz_animation(FuzzRng *r, int kind, const flow_style_t *base, const char *name,
                           FAnimation *an, FKeyframe *frames)
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
    bool same = true;
    for (int k = 0; k < nk; k++) {
        FKeyframe *f = &frames[k];
        memset(f, 0, sizeof(*f));
        f->offset = offsets[k];
        f->style = *base;
        fuzz_set(&f->style, kind, fuzz_below(r, 3));
        same &= fuzz_get(&f->style, kind) == fuzz_get(base, kind);
        if (fuzz_below(r, 3) == 0) {
            f->has_easing = true;
            f->easing = fuzz_easing(r);
        }
        f_props_add(&f->sets, kProp[kind]);
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
                      FKeyframe frames[2][FUZZ_FRAMES], int *nanims)
{
    *kind = fuzz_below(r, FUZZ_KINDS);
    memset(base, 0, sizeof(*base));
    base->opacity = 1000;
    fuzz_set(base, *kind, fuzz_below(r, 3));
    *nanims = fuzz_below(r, 3) == 0 ? 2 : 1;
    bool still = true;
    for (int a = 0; a < *nanims; a++)
        still &= fuzz_animation(r, *kind, base, a == 0 ? "f" : "g", &an[a], frames[a]);
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
    bool still = fuzz_case(r, &kind, &base, an, frames, &nanims);
    for (int a = 0; edge && a < nanims; a++)
        fuzz_edge(r, &an[a]);
    f->pairs += nanims == 2;
    FMotion m;
    memset(&m, 0, sizeof(m));
    m.base = base;
    m.anims = an;
    m.nanims = nanims;
    flow_timeline_t *tl = flow_timeline_new(doc);
    f_timeline_begin(tl);
    bool ok = f_timeline_element(tl, x, &m, 0);
    f_timeline_end(tl, true);
    int horizon = 200;
    for (int a = 0; a < nanims; a++) {
        double end = an[a].timing.delay_ms + an[a].timing.duration_ms * an[a].timing.iterations + 200;
        int h = !(end < kHorizon) ? kHorizon : end < 200 ? 200 : (int)end;
        horizon = h > horizon ? h : horizon;
    }
    for (int t = 0; t <= horizon; t++) {
        flow_style_t out = m.base;
        (void)f_motion_style(&m, t, &out, NULL, NULL, NULL, false);
        values[t] = fuzz_get(&out, kind);
        seen[t] = false;
    }
    seen[0] = true;
    int layouts = 0;
    for (uint64_t now = 0; now <= (uint64_t)horizon;) {
        double d = f_motion_next(&m, (double)now);
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
                fprintf(stderr, "    frame %g: %u", f->offset, fuzz_get(&f->style, kind));
                if (f->has_easing)
                    fprintf(stderr, " easing %d [%g %g %g %g] steps %d jump %d", fe->kind, fe->p[0],
                            fe->p[1], fe->p[2], fe->p[3], fe->steps, fe->jump);
                fputc('\n', stderr);
            }
        }
        int at = missed >= 0 ? missed : 0;
        for (uint64_t now = 0, prev = 0; now <= (uint64_t)at;) {
            double d = f_motion_next(&m, (double)now);
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
// matrix no decomposition undoes), small enough that both animations'
// mixes fit in a probe look's lists. The change is due at its millisecond.
static void motion_probe_list_cases(void)
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
    (void)fuzz_case(&r, &kind, &base, an, frames, &nanims);
    bool shaped = kind == FUZZ_ALIGN && nanims == 2;
    for (int i = 0; i < 3; i++)
        shaped &= lists[i] != NULL && lists[i]->ntransform == 3;
    expect("motion: the probe's case is the fuzzer's, its values three lists", shaped, NULL);
    if (!shaped) {
        moving_close(&lm);
        return;
    }
    fuzz_as_list(&base, kind, lists);
    for (int a = 0; a < nanims; a++) {
        memset(&an[a].props, 0, sizeof(an[a].props));
        f_props_add(&an[a].props, GARB_TRANSFORM);
        for (int k = 0; k < an[a].nframes; k++) {
            fuzz_as_list(&frames[a][k].style, kind, lists);
            memset(&frames[a][k].sets, 0, sizeof(frames[a][k].sets));
            f_props_add(&frames[a][k].sets, GARB_TRANSFORM);
        }
    }
    FMotion m;
    memset(&m, 0, sizeof(m));
    m.base = base;
    m.anims = an;
    m.nanims = nanims;
    flow_timeline_t *tl = flow_timeline_new(lm.p.doc);
    f_timeline_begin(tl);
    bool ok = f_timeline_element(tl, motion_find(lm.p.doc->document, "v0"), &m, 0);
    f_timeline_end(tl, true);
    double due = f_motion_next(&m, 75);
    char detail[64];
    snprintf(detail, sizeof(detail), "due %.17g", due);
    expect("motion due: the fuzzer's case made of transform lists is due at its change",
           ok && due > 75 && due <= 76, detail);
    flow_timeline_free(tl);
    moving_close(&lm);
}

// A page of `divs` elements, each a 64-function transform (libgarb's most)
// out of the lists' budget, then one animating a 64-function list — and,
// when `two`, a one-function list before it, written first.
static char *dropped_page(int divs, bool two)
{
    size_t cap = (size_t)divs * 24 + 64 * 17 * 3 + 1024, at = 0;
    char *html = malloc(cap);
    at += (size_t)snprintf(html + at, cap - at, MOTION_STYLE
                           "@keyframes short{from{transform:translateX(0px)}to{transform:translateX(10px)}}"
                           ".s{transform:");
    for (int i = 0; i < 64; i++)
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
static size_t dropped_left(int divs, bool two)
{
    char *html = dropped_page(divs, two);
    Moving m;
    moving_open(&m, html, 1000);
    FStyles *fs = f_style_build(m.p.doc, m.p.page, &m.p.env);
    size_t left = fs != NULL ? fs->lists_left : 0;
    f_style_free(fs);
    moving_close(&m);
    free(html);
    return left;
}

// A property the layout's style dropped is due for nothing — not by the
// schedule's own reckoning, nor by its look at the next millisecond: an
// opacity moving from 0 to 1 for a second, dropped, is due at its end.
static void motion_dropped_cases(void)
{
    os64_html_document_t *doc = parse("<!doctype html><p id=x>x</p>");
    const os64_html_node_t *x = motion_find(doc->document, "x");
    flow_style_t base;
    memset(&base, 0, sizeof(base));
    base.opacity = 1000;
    FKeyframe frames[2];
    memset(frames, 0, sizeof(frames));
    for (int k = 0; k < 2; k++) {
        frames[k].offset = k;
        frames[k].style = base;
        frames[k].style.opacity = (uint16_t)(k * 1000);
        f_props_add(&frames[k].sets, GARB_OPACITY);
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
    flow_timeline_t *tl = flow_timeline_new(doc);
    f_timeline_begin(tl);
    bool ok = f_timeline_element(tl, x, &m, 0);
    f_timeline_end(tl, true);
    double moving = f_motion_next(&m, 100);
    f_props_add(&an.dropped, GARB_OPACITY);
    double dropped = f_motion_next(&m, 100);
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
    double nothing = f_motion_next(&m, 100);
    snprintf(detail, sizeof(detail), "due %g", nothing);
    expect("motion: a dropped list with no side effect is due for nothing", isinf(nothing), detail);
    flow_timeline_free(tl);
    os64_html_document_free(doc);

    // And through a layout: enough elements before it spend the lists'
    // budget that the animated one's keyframes find room for their lists
    // and its mix, half again as much, does not (calibrated from what one
    // element and the animated one cost, so the sizes may change). Its
    // value is left as the base has it, and it is due at its end.
    size_t left0 = dropped_left(0, false), each = left0 - dropped_left(1, false);
    size_t animated = F_STYLE_LISTS_BUDGET - left0;
    int divs = each > 0 ? (int)((left0 + animated - animated * 7 / 10) / each) : 0;
    char *html = dropped_page(divs, false);
    Moving lm;
    moving_open(&lm, html, 1000);
    FStyles *fs = f_style_build(lm.p.doc, lm.p.page, &lm.p.env);
    const os64_html_node_t *ln = motion_find(lm.p.doc->document, "long");
    const FStyled *st = ln != NULL && fs != NULL ? f_style_of(fs, ln) : NULL;
    bool keyframes = st != NULL && st->motion != NULL && st->motion->nanims == 1 &&
                     st->motion->anims[0].nframes == 2 &&
                     st->motion->anims[0].frames[0].style.ntransform == 64 &&
                     st->motion->anims[0].frames[1].style.ntransform == 64;
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

    // Two animations of one property, the earlier's mix one function and
    // the later's 64, with room left for the earlier's alone (calibrated
    // as above): the later's is dropped, and the earlier's, which the
    // style shows, moves — so the element is due now, not at the end.
    left0 = dropped_left(0, true);
    each = left0 - dropped_left(1, true);
    animated = F_STYLE_LISTS_BUDGET - left0;
    divs = each > 0 ? (int)((left0 + animated - animated * 7 / 10) / each) : 0;
    html = dropped_page(divs, true);
    moving_open(&lm, html, 1000);
    fs = f_style_build(lm.p.doc, lm.p.page, &lm.p.env);
    ln = motion_find(lm.p.doc->document, "long");
    st = ln != NULL && fs != NULL ? f_style_of(fs, ln) : NULL;
    bool split = st != NULL && st->motion != NULL && st->motion->nanims == 2 &&
                 st->motion->anims[1].frames[0].style.ntransform == 64 && st->style.ntransform == 1 &&
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
    // one-function list moving under it, and the tree shows it still. The
    // probe of the next millisecond has no room for it, and the value its
    // look leaves is the moving one's — which is not what is drawn, so
    // nothing is due until the end.
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
    expect("motion due: a list past the probe's room is not asked about", due == 2000, detail);
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

// A crossing on two days' animation, and on the domain's longest (values.h),
// wakes within the millisecond of the flip: a double resolves the time to
// a tenth of a microsecond there.
static void motion_long_crossing_cases(void)
{
    os64_html_document_t *doc = parse("<!doctype html><p id=x>x</p>");
    const os64_html_node_t *x = motion_find(doc->document, "x");
    flow_style_t base;
    memset(&base, 0, sizeof(base));
    base.opacity = 1000;
    FKeyframe frames[2];
    memset(frames, 0, sizeof(frames));
    for (int k = 0; k < 2; k++) {
        frames[k].offset = k;
        frames[k].style = base;
        frames[k].style.text_align = k ? FLOW_ALIGN_RIGHT : FLOW_ALIGN_LEFT;
        f_props_add(&frames[k].sets, GARB_TEXT_ALIGN);
    }
    static const double kDur[2] = {1e8, GARB_TIME_MS_MAX};
    for (int d = 0; d < 2; d++) {
        FAnimation an;
        memset(&an, 0, sizeof(an));
        an.timing.name = (flow_name_t){"l", 1};
        an.timing.duration_ms = kDur[d];
        an.timing.iterations = 1;
        an.frames = frames;
        an.nframes = 2;
        f_props_add(&an.props, GARB_TEXT_ALIGN);
        FMotion m;
        memset(&m, 0, sizeof(m));
        m.base = base;
        m.anims = &an;
        m.nanims = 1;
        flow_timeline_t *tl = flow_timeline_new(doc);
        f_timeline_begin(tl);
        bool ok = f_timeline_element(tl, x, &m, 0);
        f_timeline_end(tl, true);
        double half = kDur[d] / 2, wake = f_motion_next(&m, 0);
        char detail[96];
        snprintf(detail, sizeof(detail), "wake %.17g half %.17g", wake, half);
        expect(d == 0 ? "motion: two days' crossing wakes within its millisecond"
                      : "motion: the domain's longest crossing wakes within its millisecond",
               ok && wake <= half && wake >= half - 1, detail);
        flow_timeline_free(tl);
    }

    // A curve whose slope's roots lie far apart in size: cubic-bezier(1/3,
    // 1, 2/3, -1e6) rises, turns at 5e-7 and is back below 0 by 1e-6.
    // Hidden to visible over 1e12 ms, it shows at once and is hidden again
    // at 999999 ms — which is due there, the turn kept though a root that
    // much larger stands beside it, and the crossing found to the last bit.
    FKeyframe vf[2];
    memset(vf, 0, sizeof(vf));
    for (int k = 0; k < 2; k++) {
        vf[k].offset = k;
        vf[k].style = base;
        vf[k].style.visibility = k ? FLOW_VISIBLE : FLOW_HIDDEN;
        f_props_add(&vf[k].sets, GARB_VISIBILITY);
    }
    FAnimation va;
    memset(&va, 0, sizeof(va));
    va.timing.name = (flow_name_t){"v", 1};
    va.timing.duration_ms = GARB_TIME_MS_MAX;
    va.timing.iterations = 1;
    va.timing.easing =
        (flow_easing_t){FLOW_EASE_CUBIC, {1.0 / 3, 1, 2.0 / 3, -GARB_EASING_Y_MAX}, 0, 0, false};
    va.frames = vf;
    va.nframes = 2;
    f_props_add(&va.props, GARB_VISIBILITY);
    FMotion vm;
    memset(&vm, 0, sizeof(vm));
    vm.base = base;
    vm.anims = &va;
    vm.nanims = 1;
    flow_timeline_t *vt = flow_timeline_new(doc);
    f_timeline_begin(vt);
    bool vok = f_timeline_element(vt, x, &vm, 0);
    f_timeline_end(vt, true);
    flow_style_t shown = vm.base;
    (void)f_motion_style(&vm, 1, &shown, NULL, NULL, NULL, false);
    double back = f_motion_next(&vm, 1);
    char vdetail[64];
    snprintf(vdetail, sizeof(vdetail), "visible %d, due %.17g", shown.visibility == FLOW_VISIBLE, back);
    expect("motion: a turn beside a far larger root is kept, and the return due",
           vok && shown.visibility == FLOW_VISIBLE && back >= 999990 && back <= 1000001, vdetail);
    flow_timeline_free(vt);
    os64_html_document_free(doc);
}

static void motion_sweep(void)
{
    motion_sweep_in(false);
    motion_sweep_in(true);
    motion_sweep_undo();
}

static void motion_cases(void)
{
    motion_easing_cases();
    motion_chrome_cases();
    motion_cascade_cases();
    motion_effect_cases();
    motion_timeline_cases();
    motion_due_cases();
    motion_fuzz_cases();
    motion_probe_list_cases();
    motion_dropped_cases();
    motion_held_time_cases();
    motion_long_crossing_cases();
    motion_sweep();
}
