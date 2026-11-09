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

static void script_effect_cases(void)
{
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
    garb_effect_keyframe_t frames[2] = {{0, true, ease, from_s, nfrom}, {1, true, ease, to_s, nto}};
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
    garb_effect_keyframe_t fade[2] = {{0, false, {0}, s0, n0}, {1, false, {0}, s1, n1}};
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
    } kRuns[] = {
        {"script effects: a running clock is (now - start) at rate 1", "", "0.5", 5000, 1, 4000, GARB_FILL_NONE},
        {"script effects: at rate 2, 500 ms in is half way", "", "0.5", 4500, 2, 4000, GARB_FILL_NONE},
        {"script effects: and 1000 ms in is its end, filled forwards its last", "", "1", 5000, 2, 4000,
         GARB_FILL_FORWARDS},
        {"script effects: past its end, unfilled, it shows nothing", "", "1", 7000, 1, 4000, GARB_FILL_NONE},
        {"script effects: past its end, filled forwards, its last", "#k{opacity:.3}", "1", 7000, 1, 4000,
         GARB_FILL_FORWARDS},
        {"script effects: before its start it shows nothing, the rule's value",
         "#k{opacity:.3}", "0.3", 3000, 1, 4000, GARB_FILL_NONE},
        {"script effects: backwards from its end, a quarter run", "", "0.75", 4500, -1, 6000, GARB_FILL_NONE},
        {"script effects: laid after a CSS animation of the same property, it wins",
         "@keyframes a{from{opacity:.1}to{opacity:.1}}#k{animation:a 10s}", "0.5", 5000, 1, 4000, GARB_FILL_NONE},
        {"script effects: an !important rule beats it, as it beats a CSS animation",
         "#k{opacity:.2 !important}", "0.2", 5000, 1, 4000, GARB_FILL_NONE},
    };
    for (size_t i = 0; i < sizeof(kRuns) / sizeof(kRuns[0]); i++) {
        char html[256];
        snprintf(html, sizeof(html), "<!doctype html><style>%s</style><div id=k>x</div>", kRuns[i].css);
        run.rate = kRuns[i].rate;
        run.start_ms = kRuns[i].start;
        run.timing.fill = kRuns[i].fill;
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
    garb_free(&o0);
    garb_free(&o1);
    garb_free(&from_p);
    garb_free(&to_p);
}
