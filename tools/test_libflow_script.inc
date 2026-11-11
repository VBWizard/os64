// A script's effects (garb_effect_t, WEB_ANIMATIONS.md § 2): laid
// over an element's CSS animations by the environment's script_effects,
// on clocks of their own, read back as getComputedStyle reads them.
// Included by test_libflow_host.c.

typedef struct {
    const os64_html_node_t *target;
    const garb_effect_t *const *effects;
    int32_t n;
} ScriptEffects;

static int32_t script_effects_hook(void *ctx, const os64_html_node_t *node, const garb_effect_t *const **out)
{
    const ScriptEffects *x = ((Sheeted *)ctx)->script;
    if (x == NULL || node != x->target)
        return 0;
    *out = x->effects;
    return x->n;
}

// `text` read as a declaration block into longhands, as libdom reads a
// script's keyframe: each into `out`, how many.
static int32_t script_sets(garb_parsed_t *owner, const char *text, garb_set_t *out, int32_t cap)
{
    if (garb_parse_block(text, strlen(text), owner) != GARB_OK)
        return 0;
    int32_t n = 0;
    for (int32_t i = 0; i < owner->nitems; i++) {
        if (owner->items[i].kind != GARB_ITEM_DECL)
            continue;
        garb_set_t sets[64];
        int32_t k = garb_read_declaration(owner, &owner->items[i].decl, (garb_reading_t){0}, sets);
        for (int32_t j = 0; j < k && n < cap; j++)
            out[n++] = sets[j];
    }
    return n;
}

/* One script effect of two keyframes on `#k` in `html`, its composite and
 * iterationComposite as given, held at `hold` ms of a 1000 ms iteration
 * run `iterations` times; `prop` read as getComputedStyle reads it. */
static void composite_case(const char *name, const char *html, const char *from, const char *to,
                           garb_composite_t op, bool iteration_accumulate, double iterations, double hold,
                           garb_prop_t prop, const char *want)
{
    garb_parsed_t a = {0}, b = {0};
    garb_set_t sa[8], sb[8];
    int32_t na = script_sets(&a, from, sa, 8), nb = script_sets(&b, to, sb, 8);
    garb_effect_keyframe_t frames[2] = {{0, false, {0}, sa, na, op}, {1, false, {0}, sb, nb, op}};
    garb_effect_t fx;
    memset(&fx, 0, sizeof(fx));
    fx.frames = frames;
    fx.nframes = 2;
    fx.timing.duration_ms = 1000;
    fx.timing.iterations = iterations;
    fx.held = true;
    fx.hold_ms = hold;
    fx.rate = 1;
    fx.iteration_accumulate = iteration_accumulate;
    const garb_effect_t *list[1] = {&fx};
    Sheeted p;
    sheeted_open(&p, html, 800, 600);
    ScriptEffects se = {computed_find(p.doc->document, "k"), list, 1};
    p.script = &se;
    p.env.script_effects = script_effects_hook;
    p.env.ctx = &p;
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    flow_computed_t *c = tree != NULL ? flow_computed_new(tree, &p.env, se.target, NULL) : NULL;
    computed_is(c, name, prop, want);
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);
    garb_free(&a);
    garb_free(&b);
}

/* Composite (Web Animations 1 § 5.4): a keyframe that adds meets the value
 * underneath before the interval is mixed. */
// Two keyframes of one value, the second adding: their composited ends
// differ, so the interval moves and a running effect is due at once, not
// at its end.
static void composite_due_case(void)
{
    garb_parsed_t a = {0}, b = {0};
    garb_set_t sa[2], sb[2];
    int32_t na = script_sets(&a, "margin-left: 10px", sa, 2), nb = script_sets(&b, "margin-left: 10px", sb, 2);
    garb_effect_keyframe_t frames[2] = {{0, false, {0}, sa, na, GARB_COMPOSITE_REPLACE},
                                        {1, false, {0}, sb, nb, GARB_COMPOSITE_ADD}};
    garb_effect_t fx;
    memset(&fx, 0, sizeof(fx));
    fx.frames = frames;
    fx.nframes = 2;
    fx.timing.duration_ms = 1000;
    fx.timing.iterations = 1;
    fx.start_ms = 4000;
    fx.rate = 1;
    const garb_effect_t *list[1] = {&fx};
    Sheeted p;
    sheeted_open(&p, "<!doctype html><style>#k{margin-left:100px}</style><div id=k>x</div>", 800, 600);
    ScriptEffects se = {computed_find(p.doc->document, "k"), list, 1};
    p.script = &se;
    p.env.script_effects = script_effects_hook;
    p.env.ctx = &p;
    p.env.now_ms = 4500;
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    flow_computed_t *c = tree != NULL ? flow_computed_new(tree, &p.env, se.target, NULL) : NULL;
    computed_is(c, "composite: equal keyframes, one adding, move", GARB_MARGIN_LEFT, "60px");
    flow_computed_free(c);
    uint64_t due = tree != NULL ? flow_motion_due(tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0}) : 0;
    expect("composite: so they are due at once", due == 4500, NULL);
    flow_free(tree);
    sheeted_close(&p);
    garb_free(&a);
    garb_free(&b);
}

// A paused effect that adds, over a running one: the running one still
// shows through it, so the property is due at once. And an accumulation
// whose duration is so small that its iteration count is infinite at a
// finite time ends.
static void composite_under_cases(void)
{
    garb_parsed_t a = {0}, b = {0}, c = {0};
    garb_set_t sa[2], sb[2], sc[2];
    int32_t na = script_sets(&a, "margin-left: 0px", sa, 2), nb = script_sets(&b, "margin-left: 100px", sb, 2);
    int32_t nc = script_sets(&c, "margin-left: 10px", sc, 2);
    garb_effect_keyframe_t run_frames[2] = {{0, false, {0}, sa, na, GARB_COMPOSITE_REPLACE},
                                            {1, false, {0}, sb, nb, GARB_COMPOSITE_REPLACE}};
    garb_effect_keyframe_t add_frames[2] = {{0, false, {0}, sc, nc, GARB_COMPOSITE_ADD},
                                            {1, false, {0}, sc, nc, GARB_COMPOSITE_ADD}};
    garb_effect_t run, add;
    memset(&run, 0, sizeof(run));
    run.frames = run_frames;
    run.nframes = 2;
    run.timing.duration_ms = 1000;
    run.timing.iterations = 1;
    run.start_ms = 4000;
    run.rate = 1;
    add = run;
    add.frames = add_frames;
    add.held = true;
    add.hold_ms = 500;
    const garb_effect_t *list[2] = {&run, &add};
    Sheeted p;
    sheeted_open(&p, "<!doctype html><div id=k>x</div>", 800, 600);
    ScriptEffects se = {computed_find(p.doc->document, "k"), list, 2};
    p.script = &se;
    p.env.script_effects = script_effects_hook;
    p.env.ctx = &p;
    p.env.now_ms = 4500;
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    flow_computed_t *cc = tree != NULL ? flow_computed_new(tree, &p.env, se.target, NULL) : NULL;
    computed_is(cc, "composite: a held effect adds to a running one", GARB_MARGIN_LEFT, "60px");
    flow_computed_free(cc);
    uint64_t due = tree != NULL ? flow_motion_due(tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0}) : 0;
    expect("composite: the running one under it is still due", due == 4500, NULL);
    flow_free(tree);
    run.timing.duration_ms = 1e-320;
    run.timing.iterations = INFINITY;
    run.held = true;
    run.hold_ms = 1;
    run.iteration_accumulate = true;
    se.n = 1;
    tree = flow_layout(p.doc, p.page, 800, &p.env);
    expect("composite: an infinite iteration count at a finite time ends", tree != NULL, NULL);
    flow_free(tree);
    sheeted_close(&p);
    garb_free(&a);
    garb_free(&b);
    garb_free(&c);
}

static void composite_cases(void)
{
    const char *box = "<!doctype html><style>#k{margin-left:100px;opacity:0.25;color:rgb(10,20,30);"
                      "transform:translateX(10px)}</style><div id=k>x</div>";
    composite_case("composite: replace mixes the keyframes alone", box, "margin-left: 0px", "margin-left: 50px",
                   GARB_COMPOSITE_REPLACE, false, 1, 500, GARB_MARGIN_LEFT, "25px");
    composite_case("composite: add meets the value underneath", box, "margin-left: 0px", "margin-left: 50px",
                   GARB_COMPOSITE_ADD, false, 1, 500, GARB_MARGIN_LEFT, "125px");
    composite_case("composite: an added number is clamped", box, "opacity: 0.5", "opacity: 1",
                   GARB_COMPOSITE_ADD, false, 1, 500, GARB_OPACITY, "1");
    composite_case("composite: an added colour, channel by channel", box, "color: rgb(10, 10, 10)",
                   "color: rgb(10, 10, 10)", GARB_COMPOSITE_ADD, false, 1, 500, GARB_COLOR, "rgb(20, 30, 40)");
    composite_case("composite: an added transform follows the one underneath", box, "transform: translateX(0px)",
                   "transform: translateX(20px)", GARB_COMPOSITE_ADD, false, 1, 500, GARB_TRANSFORM,
                   "matrix(1, 0, 0, 1, 20, 0)");
    composite_case("composite: an accumulated scale builds from 1", "<!doctype html><style>#k{transform:scale(2)}"
                   "</style><div id=k>x</div>", "transform: scale(3)", "transform: scale(3)",
                   GARB_COMPOSITE_ACCUMULATE, false, 1, 500, GARB_TRANSFORM, "matrix(4, 0, 0, 4, 0, 0)");
    composite_case("composite: an added scale multiplies", "<!doctype html><style>#k{transform:scale(2)}"
                   "</style><div id=k>x</div>", "transform: scale(3)", "transform: scale(3)",
                   GARB_COMPOSITE_ADD, false, 1, 500, GARB_TRANSFORM, "matrix(6, 0, 0, 6, 0, 0)");
    composite_case("composite: an added scale property multiplies", "<!doctype html><style>#k{scale:2}"
                   "</style><div id=k>x</div>", "scale: 3", "scale: 3", GARB_COMPOSITE_ADD, false, 1, 500,
                   GARB_SCALE, "6");
    composite_case("composite: lists that do not pair accumulate as the keyframe's alone", box,
                   "transform: scale(2)", "transform: scale(2)", GARB_COMPOSITE_ACCUMULATE, false, 1, 500,
                   GARB_TRANSFORM, "matrix(2, 0, 0, 2, 0, 0)");
    composite_case("composite: a kind that does not add replaces", box, "visibility: hidden", "visibility: hidden",
                   GARB_COMPOSITE_ADD, false, 1, 500, GARB_VISIBILITY, "hidden");
    composite_case("composite: an added number past its range mixes, then is held", box, "opacity: 0.5",
                   "opacity: 1", GARB_COMPOSITE_ADD, false, 1, 250, GARB_OPACITY, "0.875");
    composite_case("composite: each finished iteration builds on the last", box, "margin-left: 0px",
                   "margin-left: 10px", GARB_COMPOSITE_REPLACE, true, 3, 2500, GARB_MARGIN_LEFT, "25px");
    composite_case("composite: without iterationComposite each iteration starts over", box, "margin-left: 0px",
                   "margin-left: 10px", GARB_COMPOSITE_REPLACE, false, 3, 2500, GARB_MARGIN_LEFT, "5px");
    composite_case("composite: what iterations build is held to the property's range", box, "opacity: 0.5",
                   "opacity: 0.5", GARB_COMPOSITE_REPLACE, true, 3, 2500, GARB_OPACITY, "1");
    composite_case("composite: iterations build past a thousand", box, "margin-left: 0px", "margin-left: 1px",
                   GARB_COMPOSITE_REPLACE, true, 2000, 1001500, GARB_MARGIN_LEFT, "1001.5px");
    composite_case("composite: added rotations about two axes compose", "<!doctype html><style>#k{rotate:x 90deg}"
                   "</style><div id=k>x</div>", "rotate: y 90deg", "rotate: y 90deg", GARB_COMPOSITE_ADD, false, 1,
                   500, GARB_ROTATE, "0.57735 0.57735 0.57735 120deg");
    composite_case("composite: an added colour past a channel's top mixes as the sum",
                   "<!doctype html><style>#k{color:rgb(128,128,128)}</style><div id=k>x</div>", "color: rgb(255, 0, 0)",
                   "color: rgb(0, 0, 255)", GARB_COMPOSITE_ADD, false, 1, 500, GARB_COLOR, "rgb(255, 128, 255)");
    composite_case("composite: an accumulated colour at an iteration's start", box, "color: rgb(0, 0, 0)",
                   "color: rgb(120, 120, 120)", GARB_COMPOSITE_REPLACE, true, 10, 2000, GARB_COLOR, "rgb(240, 240, 240)");
    composite_case("composite: an accumulated colour past a channel's top mixes as the sum", box, "color: rgb(0, 0, 0)",
                   "color: rgb(120, 120, 120)", GARB_COMPOSITE_REPLACE, true, 10, 2500, GARB_COLOR, "rgb(255, 255, 255)");
    composite_case("composite: each endpoint accumulates before it is composited",
                   "<!doctype html><style>#k{scale:2}</style><div id=k>x</div>", "scale: 3", "scale: 3",
                   GARB_COMPOSITE_ADD, true, 3, 1500, GARB_SCALE, "10");
    composite_case("composite: an implicit first keyframe accumulates onto the value underneath", box, "",
                   "margin-left: 10px", GARB_COMPOSITE_REPLACE, true, 3, 1500, GARB_MARGIN_LEFT, "65px");
    composite_case("composite: a font size's iterations build on its last keyframe", box, "font-size: 10px",
                   "font-size: 20px", GARB_COMPOSITE_REPLACE, true, 3, 1500, GARB_FONT_SIZE, "35px");
    composite_case("composite: a colour's iterations build on its last keyframe", box, "color: rgb(0, 0, 0)",
                   "color: rgb(100, 0, 0)", GARB_COMPOSITE_REPLACE, true, 3, 1500, GARB_COLOR, "rgb(150, 0, 0)");
    composite_case("composite: an implicit first keyframe accumulates the neutral value, then adds",
                   "<!doctype html><style>#k{scale:2}</style><div id=k>x</div>", "", "scale: 3",
                   GARB_COMPOSITE_ADD, true, 3, 1500, GARB_SCALE, "8");
    composite_case("composite: an implicit keyframe of a kind that does not add is the value underneath", box,
                   "", "visibility: hidden", GARB_COMPOSITE_ADD, true, 3, 1250, GARB_VISIBILITY, "visible");
    composite_case("composite: an implicit last keyframe adds nothing to the iterations", box, "margin-left: 0px",
                   "", GARB_COMPOSITE_REPLACE, true, 3, 1500, GARB_MARGIN_LEFT, "50px");
    composite_due_case();
    composite_under_cases();
    composite_case("composite: an added currentColor is a colour of its own",
                   "<!doctype html><style>#k{color:rgb(10,20,30);background-color:rgb(10,20,30)}</style>"
                   "<div id=k>x</div>", "background-color: currentColor", "background-color: currentColor",
                   GARB_COMPOSITE_ADD, false, 1, 500, GARB_BACKGROUND_COLOR, "rgb(20, 40, 60)");
    composite_case("composite: a colour added onto currentColor is a colour of its own",
                   "<!doctype html><style>#k{color:rgb(10,20,30);background-color:currentColor}</style>"
                   "<div id=k>x</div>", "background-color: rgb(10, 20, 30)", "background-color: rgb(10, 20, 30)",
                   GARB_COMPOSITE_ADD, false, 1, 500, GARB_BACKGROUND_COLOR, "rgb(20, 40, 60)");
}

static void script_effect_cases(void)
{
    composite_cases();
    // The sign-in page's animation key, as the 2026-11-08 record makes it: a box animated
    // from one colour and turn to another over 4096 ms, every keyframe
    // eased by cubic-bezier(0.23, -0.07, 0.53, 0.55), paused and set to
    // 440 ms, then read.
    garb_parsed_t from_p = {0}, to_p = {0};
    garb_set_t from_s[8], to_s[8];
    int32_t nfrom = script_sets(&from_p, "color: #62df76; transform: rotate(0deg)", from_s, 8);
    int32_t nto = script_sets(&to_p, "color: #31a6fd; transform: rotate(165deg)", to_s, 8);
    expect("script effects: the keyframes read", nfrom == 2 && nto == 2, NULL);
    garb_easing_t ease = {GARB_EASE_CUBIC, {0.23, -0.07, 0.53, 0.55}, 0, GARB_JUMP_END, false};
    garb_effect_keyframe_t frames[2] = {{0, true, ease, from_s, nfrom, GARB_COMPOSITE_REPLACE}, {1, true, ease, to_s, nto, GARB_COMPOSITE_REPLACE}};
    garb_effect_t fx;
    memset(&fx, 0, sizeof(fx));
    fx.frames = frames;
    fx.nframes = 2;
    fx.timing.duration_ms = 4096;
    fx.timing.iterations = 1;
    fx.held = true;
    fx.hold_ms = 440;
    fx.rate = 1;
    const garb_effect_t *list[1] = {&fx};

    Sheeted p;
    sheeted_open(&p, "<!doctype html><div id=key>x</div>", 800, 600);
    ScriptEffects se = {computed_find(p.doc->document, "key"), list, 1};
    p.script = &se;
    p.env.script_effects = script_effects_hook;
    p.env.ctx = &p;
    flow_tree_t *tree = flow_layout(p.doc, p.page, 800, &p.env);
    flow_computed_t *c = tree != NULL ? flow_computed_new(tree, &p.env, se.target, NULL) : NULL;
    // The want worked independently (the eased progress 0.0116461 by
    // halving the curve, each channel rounded, the turn's cosine and sine
    // to six significant digits); Chrome's own strings are the validation
    // page's to hold.
    computed_is(c, "script effects: the sign-in key's colour held at 440 of 4096", GARB_COLOR, "rgb(97, 222, 120)");
    computed_is(c, "script effects: and its turn", GARB_TRANSFORM,
                "matrix(0.999438, 0.0335322, -0.0335322, 0.999438, 0, 0)");
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);

    // Under display: none, on the element and on an ancestor, where no box
    // is made and CSS animations end: a script's effect is still its
    // computed style.
    static const char *const kHidden[] = {
        "<!doctype html><div id=key style=display:none>x</div>",
        "<!doctype html><div style=display:none><div id=key>x</div></div>",
    };
    for (int h = 0; h < 2; h++) {
        sheeted_open(&p, kHidden[h], 800, 600);
        se.target = computed_find(p.doc->document, "key");
        p.script = &se;
        p.env.script_effects = script_effects_hook;
        p.env.ctx = &p;
        tree = flow_layout(p.doc, p.page, 800, &p.env);
        c = tree != NULL ? flow_computed_new(tree, &p.env, se.target, NULL) : NULL;
        computed_is(c, h == 0 ? "script effects: on a display:none element" : "script effects: under a display:none one",
                    GARB_COLOR, "rgb(97, 222, 120)");
        flow_computed_free(c);
        flow_free(tree);
        sheeted_close(&p);
    }

    // A running clock: (now - start) * rate, here 1000 ms into 2000 of an
    // opacity from 0 to 1.
    garb_parsed_t o0 = {0}, o1 = {0};
    garb_set_t s0[2], s1[2];
    int32_t n0 = script_sets(&o0, "opacity: 0", s0, 2), n1 = script_sets(&o1, "opacity: 1", s1, 2);
    garb_effect_keyframe_t fade[2] = {{0, false, {0}, s0, n0, GARB_COMPOSITE_REPLACE}, {1, false, {0}, s1, n1, GARB_COMPOSITE_REPLACE}};
    garb_effect_t run;
    memset(&run, 0, sizeof(run));
    run.frames = fade;
    run.nframes = 2;
    run.timing.duration_ms = 2000;
    run.timing.iterations = 1;
    run.start_ms = 4000;
    run.rate = 1;
    const garb_effect_t *one[1] = {&run};
    static const struct {
        const char *what, *css, *want;
        double now, rate, start;
        garb_fill_t fill;
        bool held;
    } kRuns[] = {
        {"script effects: a running clock is (now - start) at rate 1", "", "0.5", 5000, 1, 4000, GARB_FILL_NONE, false},
        {"script effects: at rate 2, 500 ms in is half way", "", "0.5", 4500, 2, 4000, GARB_FILL_NONE, false},
        {"script effects: and 1000 ms in is its end, filled forwards its last", "", "1", 5000, 2, 4000,
         GARB_FILL_FORWARDS, false},
        {"script effects: past its end, unfilled, it shows nothing", "", "1", 7000, 1, 4000, GARB_FILL_NONE, false},
        {"script effects: past its end, filled forwards, its last", "#k{opacity:.3}", "1", 7000, 1, 4000,
         GARB_FILL_FORWARDS, false},
        {"script effects: before its start it shows nothing, the rule's value",
         "#k{opacity:.3}", "0.3", 3000, 1, 4000, GARB_FILL_NONE, false},
        {"script effects: backwards from its end, a quarter run", "", "0.75", 4500, -1, 6000, GARB_FILL_NONE, false},
        {"script effects: laid after a CSS animation of the same property, it wins",
         "@keyframes a{from{opacity:.1}to{opacity:.1}}#k{animation:a 10s}", "0.5", 5000, 1, 4000, GARB_FILL_NONE, false},
        {"script effects: an !important rule beats it, as it beats a CSS animation",
         "#k{opacity:.2 !important}", "0.2", 5000, 1, 4000, GARB_FILL_NONE, false},
        /* Held at 0 going backwards (paused or finished there): the far
         * side of its start, so unfilled it shows nothing. */
        {"script effects: a reversed one held at its start shows the rule's value", "#k{opacity:.3}", "0.3", 5000,
         -1, 4000, GARB_FILL_NONE, true},
    };
    for (size_t i = 0; i < sizeof(kRuns) / sizeof(kRuns[0]); i++) {
        char html[256];
        snprintf(html, sizeof(html), "<!doctype html><style>%s</style><div id=k>x</div>", kRuns[i].css);
        run.rate = kRuns[i].rate;
        run.start_ms = kRuns[i].start;
        run.timing.fill = kRuns[i].fill;
        run.held = kRuns[i].held;
        run.hold_ms = 0;
        sheeted_open(&p, html, 800, 600);
        ScriptEffects re = {computed_find(p.doc->document, "k"), one, 1};
        p.script = &re;
        p.env.script_effects = script_effects_hook;
        p.env.ctx = &p;
        p.env.now_ms = (uint64_t)kRuns[i].now;
        tree = flow_layout(p.doc, p.page, 800, &p.env);
        c = tree != NULL ? flow_computed_new(tree, &p.env, re.target, NULL) : NULL;
        computed_is(c, kRuns[i].what, GARB_OPACITY, kRuns[i].want);
        flow_computed_free(c);
        flow_free(tree);
        sheeted_close(&p);
    }
    run.held = false;

    // An effect easing bends progress, so an interval whose ends hold one
    // value is left early: 0 at 0 and 0.5, 1 at 1, eased out over 1000 ms,
    // leaves 0.5 at about 349 ms, where its linear far end is 500; asked at
    // 300 it is due at once.
    garb_effect_keyframe_t flat[3] = {{0, false, {0}, s0, n0, GARB_COMPOSITE_REPLACE}, {0.5, false, {0}, s0, n0, GARB_COMPOSITE_REPLACE}, {1, false, {0}, s1, n1, GARB_COMPOSITE_REPLACE}};
    garb_effect_t bent = run;
    bent.frames = flat;
    bent.nframes = 3;
    bent.timing.duration_ms = 1000;
    bent.timing.fill = GARB_FILL_NONE;
    bent.timing.easing = (garb_easing_t){GARB_EASE_CUBIC, {0, 0, 0.58, 1}, 0, GARB_JUMP_END, true};
    bent.start_ms = 4000;
    bent.rate = 1;
    const garb_effect_t *bentl[1] = {&bent};
    sheeted_open(&p, "<!doctype html><div id=k>x</div>", 800, 600);
    ScriptEffects be = {computed_find(p.doc->document, "k"), bentl, 1};
    p.script = &be;
    p.env.script_effects = script_effects_hook;
    p.env.ctx = &p;
    p.env.now_ms = 4300;
    tree = flow_layout(p.doc, p.page, 800, &p.env);
    uint64_t due = tree != NULL ? flow_motion_due(tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0}) : 0;
    expect("script effects: an eased effect's held interval is due at once, not at its linear end", due == 4300,
           NULL);
    flow_free(tree);
    sheeted_close(&p);

    // An overshooting effect easing carries a shadow past its keyframes'
    // span: from below the view, into it. Asked 1 ms in, while it has not
    // yet moved, so only the bound on where it can go puts it in view.
    garb_parsed_t t0 = {0}, t1 = {0};
    garb_set_t ts0[2], ts1[2];
    int32_t nt0 = script_sets(&t0, "text-shadow: 0 0 black", ts0, 2);
    int32_t nt1 = script_sets(&t1, "text-shadow: 0 -100px black", ts1, 2);
    garb_effect_keyframe_t lift[2] = {{0, false, {0}, ts0, nt0, GARB_COMPOSITE_REPLACE}, {1, false, {0}, ts1, nt1, GARB_COMPOSITE_REPLACE}};
    garb_effect_t over = bent;
    over.frames = lift;
    over.nframes = 2;
    over.timing.easing = (garb_easing_t){GARB_EASE_CUBIC, {0, 3, 1, 3}, 0, GARB_JUMP_END, false};
    const garb_effect_t *overl[1] = {&over};
    sheeted_open(&p, "<!doctype html><div id=k style='position:absolute;top:760px'>x</div>", 800, 600);
    ScriptEffects oe = {computed_find(p.doc->document, "k"), overl, 1};
    p.script = &oe;
    p.env.script_effects = script_effects_hook;
    p.env.ctx = &p;
    p.env.now_ms = 4001;
    tree = flow_layout(p.doc, p.page, 800, &p.env);
    due = tree != NULL ? flow_motion_due(tree, (os64_gui_rect_t){0, 0, 800, 600}, (flow_point_t){0, 0}) : 0;
    expect("script effects: a shadow an effect easing overshoots into view is asked for", due == 4001, NULL);
    flow_free(tree);
    sheeted_close(&p);
    // A transform's side effect (a containing block for fixed descendants)
    // lasts while the effect is current or in effect: held backwards at
    // its start, unfilled, it is neither; held forwards there it is.
    garb_parsed_t r0 = {0}, r1 = {0};
    garb_set_t rs0[2], rs1[2];
    int32_t nr0 = script_sets(&r0, "transform: rotate(0deg)", rs0, 2);
    int32_t nr1 = script_sets(&r1, "transform: rotate(90deg)", rs1, 2);
    garb_effect_keyframe_t turn[2] = {{0, false, {0}, rs0, nr0, GARB_COMPOSITE_REPLACE}, {1, false, {0}, rs1, nr1, GARB_COMPOSITE_REPLACE}};
    garb_effect_t spin = run;
    spin.frames = turn;
    spin.nframes = 2;
    spin.timing.duration_ms = 1000;
    spin.timing.fill = GARB_FILL_NONE;
    spin.held = true;
    spin.hold_ms = 0;
    const garb_effect_t *spinl[1] = {&spin};
    for (int dir = 0; dir < 2; dir++) {
        spin.rate = dir ? -1 : 1;
        sheeted_open(&p, "<!doctype html><style>body{margin:0}</style><div id=k style='margin:50px 0 0 200px;width:100px;"
                         "height:50px'><div id=f style='position:fixed;left:0;top:0;width:10px;height:10px'></div></div>",
                     800, 600);
        ScriptEffects ke = {computed_find(p.doc->document, "k"), spinl, 1};
        p.script = &ke;
        p.env.script_effects = script_effects_hook;
        p.env.ctx = &p;
        p.env.now_ms = 5000;
        tree = flow_layout(p.doc, p.page, 800, &p.env);
        const flow_box_t *fb = tree != NULL ? flow_box_for(tree, computed_find(p.doc->document, "f")) : NULL;
        expect(dir ? "script effects: held backwards at its start, a transform holds no fixed box"
                   : "script effects: held forwards at its start, a transform holds its fixed box",
               fb != NULL && (fb->rect.x == 200) == !dir, NULL);
        flow_free(tree);
        sheeted_close(&p);
    }
    // Below 0 with several keyframes at 0, the first alone (§ 5.3.4):
    // margin-left 100px, 200px at 0 and 300px at 1, eased below 0 by
    // cubic-bezier(0,-1,1,-1), held half way, is 100px.
    garb_parsed_t m0 = {0}, m1 = {0}, m2 = {0};
    garb_set_t ms0[2], ms1[2], ms2[2];
    int32_t nm0 = script_sets(&m0, "margin-left: 100px", ms0, 2);
    int32_t nm1 = script_sets(&m1, "margin-left: 200px", ms1, 2);
    int32_t nm2 = script_sets(&m2, "margin-left: 300px", ms2, 2);
    garb_effect_keyframe_t twice[3] = {{0, false, {0}, ms0, nm0, GARB_COMPOSITE_REPLACE}, {0, false, {0}, ms1, nm1, GARB_COMPOSITE_REPLACE}, {1, false, {0}, ms2, nm2, GARB_COMPOSITE_REPLACE}};
    garb_effect_t under = run;
    under.frames = twice;
    under.nframes = 3;
    under.timing.duration_ms = 1000;
    under.timing.fill = GARB_FILL_NONE;
    under.timing.easing = (garb_easing_t){GARB_EASE_CUBIC, {0, -1, 1, -1}, 0, GARB_JUMP_END, false};
    under.held = true;
    under.hold_ms = 500;
    under.rate = 1;
    const garb_effect_t *underl[1] = {&under};
    sheeted_open(&p, "<!doctype html><div id=k>x</div>", 800, 600);
    ScriptEffects ue = {computed_find(p.doc->document, "k"), underl, 1};
    p.script = &ue;
    p.env.script_effects = script_effects_hook;
    p.env.ctx = &p;
    tree = flow_layout(p.doc, p.page, 800, &p.env);
    c = tree != NULL ? flow_computed_new(tree, &p.env, ue.target, NULL) : NULL;
    computed_is(c, "script effects: below 0 with keyframes twice at 0, the first", GARB_MARGIN_LEFT, "100px");
    flow_computed_free(c);
    flow_free(tree);
    sheeted_close(&p);
    garb_free(&m0);
    garb_free(&m1);
    garb_free(&m2);
    garb_free(&r0);
    garb_free(&r1);
    garb_free(&t0);
    garb_free(&t1);
    garb_free(&o0);
    garb_free(&o1);
    garb_free(&from_p);
    garb_free(&to_p);
}
