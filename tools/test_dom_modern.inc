/* THE MODERN WEB'S SWITCH (libdom/modern.c, MODERN_WEB.md rule 4): with
 * options.modern clear, no name of the table's exists on window; set, each
 * row puts on window exactly the names it declares. Held against the table
 * itself, so a row a slice adds is held to its names with no case of its
 * own: a name installed outside the table is on window both ways, and one a
 * row installs without listing it is on window only one way, and either
 * fails here. */

#define MODERN_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}"

/* libdom's internal.h: the harness links the binding's own objects. */
const char *d_modern_name(size_t index);

static int modern_name_order(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* window's own string-named properties, sorted, malloc'd (each name and
 * the array); *n of them. */
static char **modern_global_names(DomFixture *f, size_t *n)
{
    *n = 0;
    os64_js_outcome_t outcome;
    JSContext *ctx = os64_js_context(f->runtime, OS64_JS_ABI_ID, &outcome);
    if (ctx == NULL) return NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSPropertyEnum *props = NULL;
    uint32_t count = 0;
    char **names = NULL;
    if (JS_GetOwnPropertyNames(ctx, &props, &count, global, JS_GPN_STRING_MASK) == 0) {
        names = malloc(sizeof(*names) * (count ? count : 1));
        if (!names) abort();
        for (uint32_t i = 0; i < count; i++) {
            const char *name = JS_AtomToCString(ctx, props[i].atom);
            names[(*n)++] = strdup(name ? name : "");
            JS_FreeCString(ctx, name);
        }
        JS_FreePropertyEnum(ctx, props, count);
        qsort(names, *n, sizeof(*names), modern_name_order);
    }
    JS_FreeValue(ctx, global);
    return names;
}

static bool modern_has(char **names, size_t n, const char *name)
{
    return bsearch(&name, names, n, sizeof(*names), modern_name_order) != NULL;
}

static void modern_names_free(char **names, size_t n)
{
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
}

static void dom_modern_cases(void)
{
    check(os64_dom_default_options().modern, "modern: the switch is on unless a host turns it off");
    size_t rows = 0;
    while (d_modern_name(rows) != NULL) rows++;
    check(rows > 0, "modern: the table has names");

    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    size_t non = 0;
    char **on = modern_global_names(&f, &non);
    dom_script(&f, MODERN_ASSERT
        "var s=document.body.style;assert('animationName' in s);"
        "s.setProperty('animation-name','spin');assert(s.getPropertyValue('animation-name')==='spin');",
        "modern-on-style-member");
    check(on != NULL, "modern: window's names with the switch on");
    fixture_free(&f);

    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) { modern_names_free(on, non); return; }
    size_t noff = 0;
    char **off = modern_global_names(&f, &noff);
    check(off != NULL, "modern: window's names with the switch off");
    dom_script(&f, MODERN_ASSERT
        "assert(typeof Element==='undefined'&&!('Element' in window)&&!self.hasOwnProperty('HTMLElement'));"
        "var r;try{Node}catch(e){r=e.name}assert(r==='ReferenceError');"
        "var b=document.body;assert(b.tagName==='BODY'&&b.constructor===Object);"
        "assert(Object.getPrototypeOf(b)!==Object.prototype&&typeof b.getAttribute==='function');"
        "assert(b.ELEMENT_NODE===undefined&&!('ELEMENT_NODE' in b));"
        /* tools/test_dom_host.sh marks animation-name as the arc's */
        "assert(!('animationName' in b.style)&&'color' in b.style);"
        "b.style.setProperty('animation-name','spin');b.style.color='red';"
        "assert(b.style.getPropertyValue('animation-name')===''&&b.style.color==='red');"
        "assert(typeof Image==='function'&&new Image().tagName==='IMG');",
        "modern-off-absent");

    bool exact = on != NULL && off != NULL;
    for (size_t i = 0; exact && i < noff; i++)
        if (!modern_has(on, non, off[i])) {
            fprintf(stderr, "modern: %s is on window only with the switch off\n", off[i]);
            exact = false;
        }
    for (size_t i = 0; exact && i < non; i++) {
        bool listed = false;
        for (size_t k = 0; k < rows && !listed; k++) listed = !strcmp(d_modern_name(k), on[i]);
        if (listed == modern_has(off, noff, on[i])) {
            fprintf(stderr, "modern: %s is %s\n", on[i], listed ? "on window with the switch off" :
                    "on window only with the switch on, and in no row of the table");
            exact = false;
        }
    }
    for (size_t k = 0; exact && k < rows; k++)
        if (!modern_has(on, non, d_modern_name(k))) {
            fprintf(stderr, "modern: the table lists %s, which is not on window\n", d_modern_name(k));
            exact = false;
        }
    check(exact, "modern: the switch adds exactly the table's names to window, and nothing else");
    modern_names_free(on, non);
    modern_names_free(off, noff);
    fixture_free(&f);
}
#undef MODERN_ASSERT
