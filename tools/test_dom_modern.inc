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

/* The shape of the binding's prototype chains, for nodes of every kind it
 * tells apart: for each, each level down to Object.prototype, as the names
 * the binding put there, leaving out what the table's rows add to them
 * (each prototype's `constructor` and Node's constants, the interface
 * row's; onscroll, onresize and the document's scrollingElement, the
 * view's; its defaultView, getComputedStyle's; P1-Da's members of a
 * document's prototype, which the document row puts there and window
 * never sees, left out of a document's levels alone, since an element's
 * hidden, dir and title are its own and every page's; Symbol.toStringTag
 * is a symbol, which no list of names holds).
 * malloc'd; NULL when the script did not run. */
static char *modern_chain_shape(DomFixture *f)
{
    if (!dom_script(f, MODERN_ASSERT
        "var holder=document.createElement('div');holder.innerHTML='<svg><circle/></svg>';"
        "var nodes=[document,document.body,document.createElement('div'),document.createElement('input'),"
        "document.createElement('a'),document.createElement('script'),holder.firstChild,"
        "document.createTextNode('t'),document.createComment('c'),document.createDocumentFragment(),"
        "document.doctype,document.implementation,document.implementation.createHTMLDocument('')];"
        "var row=/(^constructor$|_NODE$|^DOCUMENT_POSITION_|^onscroll$|^onresize$|^scrollingElement$|^defaultView$)/;"
        "var documentRow=/^(readyState|onreadystatechange|visibilityState|hidden|onvisibilitychange|referrer|"
        "activeElement|hasFocus|currentScript|title|prerendering|onprerenderingchange|domain|URL|documentURI|"
        "wasDiscarded|compatMode|dir|characterSet|charset|inputEncoding|contentType|lastModified|rootElement|"
        "clear)$/;"
        "globalThis.chainShape=nodes.map(function(n){var levels=[];"
        "for(var p=Object.getPrototypeOf(n);p&&p!==Object.prototype;p=Object.getPrototypeOf(p))"
        "levels.push(Object.getOwnPropertyNames(p).filter(function(k){"
        "return !row.test(k)&&!(n.nodeType===9&&documentRow.test(k))}).sort().join(','));"
        "return levels.join(' / ')}).join('\\n');", "modern-chain-shape"))
        return NULL;
    os64_js_outcome_t outcome;
    JSContext *ctx = os64_js_context(f->runtime, OS64_JS_ABI_ID, &outcome);
    if (ctx == NULL) return NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue shape = JS_GetPropertyStr(ctx, global, "chainShape");
    const char *text = JS_ToCString(ctx, shape);
    char *copy = text != NULL ? strdup(text) : NULL;
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, shape);
    JS_FreeValue(ctx, global);
    return copy;
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
        /* slice 4's names: longhands, shorthands, aliases, flow-relative names */
        "assert('aspectRatio' in s&&'outline' in s&&'outline-offset' in s&&'wordWrap' in s);"
        "assert('marginInlineStart' in s&&'margin-inline' in s&&'borderBlockEnd' in s);"
        "s.marginInlineStart='3px';assert(s.getPropertyValue('margin-inline-start')==='3px'&&s.marginLeft==='');"
        "s.setProperty('animation-name','spin');assert(s.getPropertyValue('animation-name')==='spin');",
        "modern-on-style-member");
    dom_script(&f, MODERN_ASSERT
        "function tag(o){return Object.prototype.toString.call(o)}"
        "assert(tag(document.body)==='[object HTMLElement]'&&tag(document)==='[object Document]');"
        "assert(tag(document.createElement('input'))==='[object HTMLInputElement]');"
        "assert(tag(document.createTextNode('t'))==='[object CharacterData]');"
        "assert(tag(new DocumentFragment())==='[object DocumentFragment]');"
        "var d=Object.getOwnPropertyDescriptor(HTMLElement.prototype,Symbol.toStringTag);"
        "assert(d.value==='HTMLElement'&&d.configurable&&!d.writable&&!d.enumerable);"
        "assert(!Object.prototype.hasOwnProperty.call(HTMLElement,Symbol.toStringTag));",
        "modern-on-string-tag");
    check(on != NULL, "modern: window's names with the switch on");
    char *chain_on = modern_chain_shape(&f);
    fixture_free(&f);

    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) { modern_names_free(on, non); free(chain_on); return; }
    size_t noff = 0;
    char **off = modern_global_names(&f, &noff);
    check(off != NULL, "modern: window's names with the switch off");
    dom_script(&f, MODERN_ASSERT
        "assert(typeof Element==='undefined'&&!('Element' in window)&&!self.hasOwnProperty('HTMLElement'));"
        "var r;try{Node}catch(e){r=e.name}assert(r==='ReferenceError');"
        "var b=document.body;assert(b.tagName==='BODY'&&b.constructor===Object);"
        "assert(Object.getPrototypeOf(b)!==Object.prototype&&typeof b.getAttribute==='function');"
        "assert(b.ELEMENT_NODE===undefined&&!('ELEMENT_NODE' in b));"
        "assert(Object.prototype.toString.call(b)==='[object Object]'&&"
        "Object.getOwnPropertySymbols(Object.getPrototypeOf(b)).indexOf(Symbol.toStringTag)<0);"
        /* tools/test_dom_host.sh marks animation-name as the arc's */
        "assert(!('animationName' in b.style)&&!('animation-name' in b.style)&&'color' in b.style);"
        /* the shorthand is still read, for the longhands that are not marked */
        "assert('animation' in b.style);"
        "assert(!('aspectRatio' in b.style)&&!('outline' in b.style)&&!('wordWrap' in b.style)&&"
        "!('marginInlineStart' in b.style)&&!('margin-inline' in b.style)&&!('cursor' in b.style));"
        "b.style.setProperty('margin-inline-start','3px');assert(b.style.getPropertyValue('margin-inline-start')==='');"
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
    /* The switch takes away the table's names, not the binding's own
     * structure: the prototype chains have the same levels, holding the
     * same members, either way. */
    char *chain_off = modern_chain_shape(&f);
    check(chain_on != NULL && chain_off != NULL && strcmp(chain_on, chain_off) == 0 &&
          strstr(chain_on, " / ") != NULL,
          "modern: the binding's prototype chains are the same with the switch on and off");
    free(chain_on);
    free(chain_off);
    modern_names_free(on, non);
    modern_names_free(off, noff);
    fixture_free(&f);
}
#undef MODERN_ASSERT
