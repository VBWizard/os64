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
    static const flow_easing_t kEase = {FLOW_EASE_CUBIC, {0.25, 0.1, 0.25, 1}, 0, 0};
    static const flow_easing_t kOver = {FLOW_EASE_CUBIC, {0.2, 0.86, 0.32, 1.22}, 0, 0};
    // On [0, 1], y at a point whose x is within 1e-7 of the input: so the
    // answer is within the curve's slope times 1e-7 of the exact y — a
    // slope at most a few here.
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
    // Outside [0, 1], the tangent at the nearer end (§ 2.2.1): through
    // P0 and P1 below, P2 and P3 above.
    expect("easing: below 0, the line through P0 and P1",
           fabs(f_ease(&kOver, -0.5, false) - 0.86 / 0.2 * -0.5) < 1e-12, NULL);
    expect("easing: above 1, the line through P2 and P3",
           fabs(f_ease(&kOver, 1.5, false) - (1 + (1.22 - 1) / (0.32 - 1) * 0.5)) < 1e-12, NULL);
    // With x1 at 0, below 0 the line through P0 and P2; with both at 0,
    // and above 1 with both at 1, no line: the end's own value.
    static const flow_easing_t kX2 = {FLOW_EASE_CUBIC, {0, 0, 0.5, 0.25}, 0, 0};
    static const flow_easing_t kLow = {FLOW_EASE_CUBIC, {0, 0.3, 0, 0.6}, 0, 0};
    static const flow_easing_t kHigh = {FLOW_EASE_CUBIC, {1, 0.3, 1, 0.6}, 0, 0};
    expect("easing: below 0 with x1 at 0, the line through P0 and P2",
           f_ease(&kX2, -1, false) == -0.5, NULL);
    expect("easing: with both x at an end, past it the end's value",
           f_ease(&kLow, -1, false) == 0 && f_ease(&kHigh, 2, false) == 1, NULL);
    // Steps (§ 2.3.1): the examples' table.
    static const flow_easing_t kEnd = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_END};
    static const flow_easing_t kStart = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_START};
    static const flow_easing_t kNone = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_NONE};
    static const flow_easing_t kBoth = {FLOW_EASE_STEPS, {0}, 4, FLOW_JUMP_BOTH};
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
    os64_html_document_t *doc = parse("<!doctype html><p id=x>x</p><p id=y>y</p>");
    const os64_html_node_t *x = motion_find(doc->document, "x");
    const os64_html_node_t *y = motion_find(doc->document, "y");
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
    // A paused animation holds its time — nothing is due while it is — and
    // resumed, it runs from there: started paused at 5000, resumed at
    // 9000, its one second ends at 10000.
    FAnimation p;
    memset(&p, 0, sizeof(p));
    p.timing.name = (flow_name_t){"p", 1};
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
    };
    for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
        char html[512];
        snprintf(html, sizeof(html), MOTION_STYLE
                 "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
                 "@keyframes ff{from{font-family:serif}to{font-family:monospace}}"
                 "</style><div style='animation:%s'>x</div>", kCases[i].animation);
        Moving m;
        moving_open(&m, html, 1000);
        uint64_t due = flow_motion_due(m.tree, kView, (flow_point_t){0, 0});
        char detail[64];
        snprintf(detail, sizeof(detail), "got %llu", (unsigned long long)due);
        expect(kCases[i].name, due == kCases[i].due, due == kCases[i].due ? NULL : detail);
        moving_close(&m);
    }
    // Out of view: nothing is due.
    Moving m;
    moving_open(&m, MOTION_STYLE
        "@keyframes mv{from{transform:translateX(0)}to{transform:translateX(100px)}}"
        "</style><div style='margin-top:2000px;animation:mv 1s linear'>x</div>", 1000);
    expect("motion due: out of view, never",
           flow_motion_due(m.tree, kView, (flow_point_t){0, 0}) == FLOW_NEVER, NULL);
    moving_close(&m);
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

static void motion_sweep(void)
{
    motion_sweep_in(false);
    motion_sweep_in(true);
}

static void motion_cases(void)
{
    motion_easing_cases();
    motion_chrome_cases();
    motion_cascade_cases();
    motion_effect_cases();
    motion_timeline_cases();
    motion_due_cases();
    motion_sweep();
}
