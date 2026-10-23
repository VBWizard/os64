/* SLICE 1b, THE WINDOW'S VIEW, THE SCROLL AND THE HISTORY (MODERN_WEB.md
 * § Slice 1b; dom.h § THE VIEW, GETCOMPUTEDSTYLE, HISTORY), on fake
 * providers, a case per sentence of the contract: the scroll and size
 * members, scrollingElement, the view's events, matchMedia and its lists,
 * getComputedStyle's declaration, and the history's state updates,
 * fragment navigations and traversal events; each name absent and nothing
 * dispatched with the switch off; and every binding allocation the new
 * operations make failed in turn. */

#include "garb/values.h"

#define VIEW_ASSERT "function assert(x,m){if(!x)throw Error(m||'assertion');}" \
    "function throws(n,f){let caught=false;try{f()}catch(e){caught=true;assert(e.name===n,'wanted '+n+' got '+e.name)}assert(caught,'no '+n);}"

typedef struct {
    os64_dom_view_t view;
    unsigned reads, scrolls;
    double asked_x, asked_y;
    bool refuse;
} ViewFake;

static void fake_view_read(void *opaque, os64_dom_view_t *out)
{
    ViewFake *f = opaque;
    f->reads++;
    *out = f->view;
}

/* Clamped as a host clamps: to 0, and to 1000 by 2000. */
static bool fake_view_scroll(void *opaque, double x, double y, os64_dom_geometry_stats_t *cost)
{
    ViewFake *f = opaque;
    f->scrolls++;
    f->asked_x = x;
    f->asked_y = y;
    cost->layouts = 1;
    cost->elapsed_us = 7;
    if (f->refuse) return false;
    f->view.scroll_x = x < 0 ? 0 : x > 1000 ? 1000 : x;
    f->view.scroll_y = y < 0 ? 0 : y > 2000 ? 2000 : y;
    return true;
}

typedef struct {
    unsigned calls;
    bool refuse, spill;
    const char *root_overflow, *body_overflow, *body_display, *root_display;
    const os64_html_node_t *last;
    const os64_html_document_t *document;
} ComputedFake;

/* Every property `p<index>`, but those a case reads: `width` counts the
 * calls, so a held declaration is seen to ask again. */
static bool fake_computed(void *opaque, const os64_html_node_t *element, os64_dom_computed_t *out)
{
    ComputedFake *f = opaque;
    f->calls++;
    f->last = element;
    os64_dom_computed_cost(out, 1, 3);
    if (f->refuse) return false;
    if (element->parent == NULL) return true;           // detached: no style
    bool body = element->tag == OS64_HTML_TAG_BODY, root = element->tag == OS64_HTML_TAG_HTML;
    for (int32_t i = 0; i < GARB_NPROPS; i++) {
        char text[64];
        if (i == GARB_WIDTH) snprintf(text, sizeof(text), "%upx", f->calls);
        else if (i == GARB_COLOR) snprintf(text, sizeof(text), "rgb(1, 2, 3)");
        else if (i == GARB_DISPLAY)
            snprintf(text, sizeof(text), "%s", body && f->body_display ? f->body_display
                                                : root && f->root_display ? f->root_display : "block");
        else if (i == GARB_OVERFLOW_X || i == GARB_OVERFLOW_Y)
            snprintf(text, sizeof(text), "%s", body && f->body_overflow ? f->body_overflow
                                                : root && f->root_overflow ? f->root_overflow : "visible");
        else snprintf(text, sizeof(text), "p%d", (int)i);
        size_t length = strlen(text);
        if (f->spill && i == GARB_FLOAT) {
            static char big[1024 * 1024];
            memset(big, 'z', sizeof(big));
            if (!os64_dom_computed_put(out, i, big, sizeof(big))) return false;
            continue;
        }
        if (!os64_dom_computed_put(out, i, text, length)) return false;
    }
    return true;
}

typedef struct {
    char url[OS64_DOM_URL_MAX], state[OS64_DOM_STATE_MAX + 1], fragment[OS64_DOM_URL_MAX];
    bool has_state, push, replace, refuse;
    unsigned updates, fragments;
    uint32_t length;
} HistoryFake;

static bool fake_history_update(void *opaque, const char *url, const char *state, size_t length, bool push)
{
    HistoryFake *h = opaque;
    h->updates++;
    if (h->refuse) return false;
    snprintf(h->url, sizeof(h->url), "%s", url);
    h->has_state = state != NULL;
    memcpy(h->state, state != NULL ? state : "", state != NULL ? length : 0);
    h->state[state != NULL ? length : 0] = '\0';
    check(state == NULL || strlen(h->state) == length, "history: the state's length is its text's");
    h->push = push;
    if (push) h->length++;
    return true;
}

static bool fake_history_fragment(void *opaque, const char *url, bool replace)
{
    HistoryFake *h = opaque;
    h->fragments++;
    if (h->refuse) return false;
    snprintf(h->fragment, sizeof(h->fragment), "%s", url);
    h->replace = replace;
    if (!replace) h->length++;
    return true;
}

static uint32_t fake_history_length(void *opaque)
{
    return ((HistoryFake *)opaque)->length;
}

/* A fixture whose document has an address, for location and history. */
static DomFixture view_fixture(bool modern, const char *url)
{
    os64_dom_options_t options = os64_dom_default_options();
    options.modern = modern;
    options.url = url;
    return fixture_new(&options);
}

static bool view_step(DomFixture *f, const char *what)
{
    os64_js_outcome_t outcome;
    bool ran = os64_dom_view_step(f->binding, &outcome);
    if (ran && outcome.status != OS64_JS_OK) fprintf(stderr, "%s: %s\n", what, outcome.message);
    check(!ran || outcome.status == OS64_JS_OK, what);
    return ran;
}

static void dom_view_scroll_cases(void)
{
    DomFixture f = view_fixture(true, "http://view.test/page.html");
    if (!fixture_ready(&f)) return;
    dom_script(&f, VIEW_ASSERT
        "assert(scrollX===0&&scrollY===0&&pageXOffset===0&&pageYOffset===0,'no view reads zeros');"
        "assert(innerWidth===0&&innerHeight===0&&outerWidth===0&&outerHeight===0);"
        "assert(scrollTo(5,5)===undefined&&scrollY===0,'no view scrolls nowhere');",
        "view: without a provider");
    ViewFake v = {.view = {.width = 800, .height = 600, .outer_width = 820, .outer_height = 700,
                           .scroll_x = 3, .scroll_y = 40.5}};
    os64_dom_view_provider_t provider = {fake_view_read, fake_view_scroll, &v};
    os64_dom_set_view(f.binding, &provider);
    dom_script(&f, VIEW_ASSERT
        "assert(scrollX===3&&pageXOffset===3&&scrollY===40.5&&pageYOffset===40.5,'the scroll');"
        "assert(innerWidth===800&&innerHeight===600&&outerWidth===820&&outerHeight===700,'the sizes');"
        "scrollTo(10,20);assert(scrollX===10&&scrollY===20,'two numbers');"
        "scroll(11,21);assert(scrollX===11&&scrollY===21,'scroll is scrollTo');"
        "scrollTo({top:30});assert(scrollX===11&&scrollY===30,'a missing member keeps its axis');"
        "scrollTo({left:1,top:2,behavior:'smooth'});assert(scrollX===1&&scrollY===2,'smooth scrolls at once');"
        "scrollTo({left:4,behavior:'instant'});scrollTo({left:5,behavior:'auto'});assert(scrollX===5);"
        "scrollTo();scrollTo(undefined);scrollTo(null);scrollTo({});assert(scrollX===5&&scrollY===2,'nothing asked moves nothing');"
        "scrollBy(1,3);assert(scrollX===6&&scrollY===5,'scrollBy adds');"
        "scrollBy({top:10});assert(scrollX===6&&scrollY===15,'scrollBy one axis');"
        "scrollTo(NaN,Infinity);assert(scrollX===0&&scrollY===0,'non-finite is 0');"
        "assert(focus()===undefined&&blur()===undefined&&document.defaultView.focus===focus&&focus.name==='focus','window focus and blur do nothing');"
        "scrollTo(5000,-9);assert(scrollX===1000&&scrollY===0,'the host clamps');"
        "throws('TypeError',()=>scrollTo(5));throws('TypeError',()=>scrollBy(5));throws('TypeError',()=>scroll('x'));"
        "throws('TypeError',()=>scrollTo({behavior:''}));throws('TypeError',()=>scrollTo({left:1,behavior:'abcd'}));"
        "throws('TypeError',()=>scrollTo({behavior:200}));"
        "var order=[];scrollTo({get top(){order.push('top');return 1},get left(){order.push('left');return 1},"
        "get behavior(){order.push('behavior');return 'auto'}});assert(order.join()==='behavior,left,top','WebIDL member order');"
        "var bad={valueOf(){throw Error('valueOf')}};var r;try{scrollTo(bad,1)}catch(e){r=e.message}assert(r==='valueOf');"
        "innerWidth=5;assert(innerWidth===5&&Object.getOwnPropertyDescriptor(window,'innerWidth').value===5,'[Replaceable]');"
        "scrollY='mine';assert(scrollY==='mine');"
        "assert(scrollTo.name==='scrollTo'&&scroll.name==='scroll'&&scrollBy.name==='scrollBy','named as WebIDL names them');",
        "view: scroll and size");
    check(v.scrolls >= 10, "view: every scroll asks the host");
    os64_dom_geometry_stats_t before = os64_dom_geometry_stats(f.binding, true);
    dom_script(&f, "scrollTo(1,1);scrollBy(1,1);", "view: cost");
    os64_dom_geometry_stats_t cost = os64_dom_geometry_stats(f.binding, false);
    check(before.layouts > 0 && cost.layouts == 2 && cost.elapsed_us == 14,
          "view: what a scroll's layout cost is counted as geometry's");
    v.refuse = true;
    dom_script(&f, VIEW_ASSERT "throws('InvalidStateError',()=>scrollTo(1,1));", "view: a refused scroll");
    v.refuse = false;
    // A scroll whose layout runs out the task's time stops the script, as
    // a geometry read does.
    dom_clock_step = 600000;
    os64_js_outcome_t outcome;
    const char *source = "for(;;){try{scrollTo(1,1)}catch(e){}}";
    check(os64_js_run(f.runtime, source, strlen(source), "view-overrun", &outcome) == OS64_JS_LIMIT,
          "view: a scroll past the budget is a sticky limit");
    dom_clock_step = 0;
    fixture_free(&f);
}

static void dom_view_scrolling_element_cases(void)
{
    DomFixture f = view_fixture(true, "http://view.test/page.html");
    if (!fixture_ready(&f)) return;
    dom_script(&f, VIEW_ASSERT
        "assert(document.scrollingElement===document.documentElement,'standards: the root');"
        "var d=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(document),'scrollingElement')||"
        "Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Object.getPrototypeOf(document)),'scrollingElement');"
        "assert(d&&d.get&&!d.set,'read-only');throws('TypeError',()=>d.get.call(document.body));",
        "scrollingElement: standards mode");
    fixture_free(&f);

    // A quirks document: the body, unless it and the root both scroll.
    DomFixture q = {0};
    os64_html_options_t html_options = os64_html_options_default();
    html_options.charset = "utf-8";
    os64_html_parser_t *parser = os64_html_parser_new(&html_options);
    static const char quirks[] = "<html><body><p>quirks</p></body></html>";
    os64_html_parser_feed(parser, quirks, sizeof(quirks) - 1);
    q.document = os64_html_parser_finish(parser);
    q.state = os64_page_state_create(q.document, 0);
    os64_js_config_t config = fixture_config();
    q.runtime = create_fixture(&config);
    os64_js_outcome_t outcome;
    q.binding = os64_dom_create(q.runtime, q.document, q.state, NULL, &outcome);
    if (!fixture_ready(&q)) return;
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===document.body,'quirks: the body, with no style to ask');",
               "scrollingElement: quirks without a provider");
    ComputedFake c = {.document = q.document};
    os64_dom_set_computed(q.binding, fake_computed, &c);
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===document.body);", "scrollingElement: nothing scrolls");
    c.root_overflow = "clip";
    c.body_overflow = "auto";
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===null,'a clip root counts as hidden');",
               "scrollingElement: both scroll");
    c.body_overflow = "clip";
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===document.body,'a clip body does not');",
               "scrollingElement: the body clips");
    c.root_overflow = "visible";
    c.body_overflow = "scroll";
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===document.body,'the root does not scroll');",
               "scrollingElement: only the body");
    c.root_overflow = "scroll";
    c.body_display = "none";
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===document.body,'a body with no box');",
               "scrollingElement: an undisplayed body");
    c.body_display = "contents";
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===document.body,'a contents body has no box');",
               "scrollingElement: a contents body");
    c.body_display = NULL;
    c.body_overflow = "auto";
    c.root_display = "none";
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===document.body,'a root with no box: nor has the body');",
               "scrollingElement: an undisplayed root");
    c.root_display = "contents";
    dom_script(&q, VIEW_ASSERT "assert(document.scrollingElement===null,'a contents root leaves the body its box');",
               "scrollingElement: a contents root");
    c.root_display = NULL;
    dom_script(&q, VIEW_ASSERT "document.documentElement.removeChild(document.body);"
               "assert(document.scrollingElement===null,'no body');", "scrollingElement: no body");
    fixture_free(&q);
}

static void dom_view_event_cases(void)
{
    DomFixture f = view_fixture(true, "http://view.test/page.html");
    if (!fixture_ready(&f)) return;
    os64_js_outcome_t outcome;
    check(!os64_dom_view_step(f.binding, &outcome), "view step: nothing owed without a view");
    ViewFake v = {.view = {.width = 800, .height = 600}};
    os64_dom_view_provider_t provider = {fake_view_read, fake_view_scroll, &v};
    os64_dom_set_view(f.binding, &provider);
    check(!view_step(&f, "view step: unmoved"), "view step: a view that has not moved owes nothing");
    v.view.scroll_y = 10;
    check(!view_step(&f, "view step: scroll heard by nothing"), "view step: a scroll nothing hears costs no task");
    dom_script(&f, "var heard=[];"
        "addEventListener('resize',e=>heard.push('resize:'+e.bubbles+':'+(e.target===window)+':'+e.isTrusted));"
        "document.addEventListener('scroll',e=>heard.push('doc:'+e.bubbles+':'+(e.target===document)));"
        "addEventListener('scroll',e=>heard.push('win:'+(e.currentTarget===window)));"
        "onscroll=e=>heard.push('onscroll');", "view step: listeners");
    v.view.width = 640;
    v.view.scroll_y = 20;
    check(view_step(&f, "view step: resize"), "view step: a resize runs");
    check(view_step(&f, "view step: scroll"), "view step: then the scroll");
    check(!view_step(&f, "view step: done"), "view step: then nothing");
    dom_script(&f, VIEW_ASSERT
        "assert(heard.join()==='resize:false:true:true,doc:true:true,win:true,onscroll','resize at window, scroll at the document bubbling to window: '+heard.join());",
        "view step: what was heard");
    // matchMedia and the lists' change events.
    dom_script(&f, VIEW_ASSERT
        "assert(typeof matchMedia==='function');"
        "var wide=matchMedia('(min-width: 700px)'),narrow=matchMedia('(max-width:700PX)');"
        "assert(wide instanceof MediaQueryList&&!wide.matches&&narrow.matches,'judged at the view now');"
        "assert(wide.media==='(min-width: 700px)'&&narrow.media==='(max-width: 700px)','serialised');"
        "assert(matchMedia('').media===''&&matchMedia('').matches,'an empty list');"
        "assert(matchMedia('::').media==='not all'&&!matchMedia('::').matches,'not a query');"
        "assert(matchMedia('all and (min-width:1px)').media==='(min-width: 1px)');"
        "throws('TypeError',()=>matchMedia());throws('TypeError',()=>new MediaQueryList());"
        "assert(Object.prototype.toString.call(wide)==='[object MediaQueryList]');"
        "var changes=[];wide.addEventListener('change',e=>changes.push('wide:'+e.matches+':'+e.media+':'+(e instanceof MediaQueryListEvent)+':'+(e.target===wide)+':'+e.bubbles+e.cancelable));"
        "narrow.onchange=e=>changes.push('narrow:'+e.matches);"
        "var gone=matchMedia('(min-width: 700px)');gone.addListener(e=>changes.push('gone'));"
        "var cb=e=>changes.push('removed');gone.addListener(cb);gone.removeListener(cb);gone.addListener(null);"
        "var silent=matchMedia('(min-width: 700px)');"
        "var dark=matchMedia('(prefers-color-scheme: dark)');dark.addEventListener('change',e=>changes.push('dark:'+e.matches));",
        "matchMedia: lists");
    v.view.width = 900;
    check(view_step(&f, "media: the resize"), "media: the resize comes before the lists");
    check(view_step(&f, "media: first"), "media: the first list that changed");
    check(view_step(&f, "media: second"), "media: the second, a call later");
    check(view_step(&f, "media: third"), "media: the third");
    check(!view_step(&f, "media: done"), "media: a list nothing hears owes no task");
    dom_script(&f, VIEW_ASSERT
        "assert(changes.join()==='wide:true:(min-width: 700px):true:true:falsefalse,narrow:false,gone','in the order made: '+changes.join());"
        "assert(wide.matches&&!narrow.matches&&silent.matches);changes.length=0;",
        "media: what was heard");
    v.view.width = 500;
    v.view.width = 900;
    check(!view_step(&f, "media: undone"), "media: a change undone before a step owes nothing");
    v.view.dark = true;
    check(view_step(&f, "media: dark"), "media: the person's scheme");
    check(!view_step(&f, "media: dark done"), "media: and nothing after");
    dom_script(&f, VIEW_ASSERT "assert(changes.join()==='dark:true'&&dark.matches,changes.join());"
        "var e=new MediaQueryListEvent('test');assert(e.type==='test'&&e.media===''&&e.matches===false&&!e.bubbles&&!e.cancelable);"
        "e=new MediaQueryListEvent('change',{media:'x',matches:1,bubbles:true,cancelable:true});"
        "assert(e.media==='x'&&e.matches===true&&e.bubbles&&e.cancelable&&e instanceof Event);"
        "throws('TypeError',()=>MediaQueryListEvent('change'));"
        "assert(Object.getPrototypeOf(MediaQueryListEvent)===Event);"
        "var n=0;wide.dispatchEvent(new Event('change'));wide.onchange=()=>n++;wide.dispatchEvent(new Event('change'));"
        "assert(n===1,'a script dispatches at a list');wide.onchange=null;"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(MediaQueryList.prototype,'media').get.call({}));",
        "media: events and the constructor");
    // A list nobody keeps is collected and its record freed at the next
    // matchMedia; one a listener holds is kept.
    size_t base = os64_dom_bytes(f.binding);
    dom_script(&f, "for(var i=0;i<200;i++)matchMedia('(min-width: '+i+'px)');", "media: lists dropped");
    size_t grown = os64_dom_bytes(f.binding);
    dom_script(&f, "matchMedia('(color)');", "media: one more");
    size_t swept = os64_dom_bytes(f.binding);
    check(grown < base + 512 && swept < base + 512,
          "media: a list nobody keeps is freed by the next matchMedia, so dropped lists cost nothing");
    dom_script(&f, "changes.length=0;", "media: forget");
    v.view.width = 100;
    check(view_step(&f, "media: held resize"), "media: the resize");
    check(view_step(&f, "media: held"), "media: a list a listener holds still hears its change");
    dom_script(&f, VIEW_ASSERT "assert(changes[0]==='wide:false:(min-width: 700px):true:true:falsefalse',changes.join());",
               "media: held, heard");
    // A list is held only while something listens on it (the fourth cold
    // read): an add then a remove, a read of onchange, a remove from a list
    // nothing was added to, a handler set and cleared, and a dispatch at a
    // list nothing hears each leave the binding where it was once the lists
    // are dropped and the next step sweeps; and the list a listener still
    // holds keeps hearing.
    for (int i = 0; i < 8 && view_step(&f, "media: settle"); i++) {}
    dom_script(&f, "changes.length=0;", "media: forget what settling heard");
    size_t entries = os64_dom_registry_count(f.binding);
    dom_script(&f, VIEW_ASSERT "var kept=matchMedia('(color)');assert(kept.onchange===null);"
        "kept.removeEventListener('change',()=>0);kept.onchange=null;", "media: a read and a remove");
    check(os64_dom_registry_count(f.binding) == entries,
          "media: reading onchange, removing a listener and clearing the handler make no entry");
    dom_script(&f, "kept=null;", "media: drop it");
    size_t before = os64_dom_bytes(f.binding), held = os64_dom_registry_count(f.binding);
    dom_script(&f, VIEW_ASSERT "var h=()=>0;"
        "for(var i=0;i<200;i++){var m=matchMedia('(min-width: '+i+'px)');m.addEventListener('change',h);"
        "m.removeEventListener('change',h);}"
        "for(var i=0;i<50;i++){var r=matchMedia('(color)');assert(r.onchange===null);r.removeEventListener('change',h);"
        "r.onchange=null;}"
        "for(var i=0;i<50;i++){var s=matchMedia('(color)');s.onchange=h;s.onchange=null;s.dispatchEvent(new Event('change'));}"
        "m=r=s=null;", "media: lists listened to and let go");
    check(!view_step(&f, "media: swept"), "media: the step that sweeps owes nothing");
    size_t after = os64_dom_bytes(f.binding);
    if (after > before + 512 || os64_dom_registry_count(f.binding) != held)
        fprintf(stderr, "media release: %zu bytes before, %zu after; registry %zu before, %zu after\n", before, after,
                held, os64_dom_registry_count(f.binding));
    check(after <= before + 512 && os64_dom_registry_count(f.binding) == held,
          "media: a list nothing listens on any more is let go and swept, whatever was done to it");
    dom_script(&f, "changes.length=0;", "media: forget again");
    v.view.width = 900;
    check(view_step(&f, "media: held again resize"), "media: the resize back");
    check(view_step(&f, "media: held again"), "media: the held list still hears");
    dom_script(&f, VIEW_ASSERT "assert(changes[0]==='wide:true:(min-width: 700px):true:true:falsefalse',changes.join());",
               "media: held, heard again");
    fixture_free(&f);
}

static void dom_view_computed_cases(void)
{
    DomFixture f = view_fixture(true, "http://view.test/page.html");
    if (!fixture_ready(&f)) return;
    dom_script(&f, VIEW_ASSERT
        "var cs=getComputedStyle(document.body);throws('InvalidStateError',()=>cs.color);"
        "assert(getComputedStyle(document.body,'::before').color==='','a pseudo-element answers empty with no provider');",
        "computed: without a provider");
    ComputedFake c = {.document = f.document};
    os64_dom_set_computed(f.binding, fake_computed, &c);
    dom_script(&f, VIEW_ASSERT
        "var b=document.body,cs=getComputedStyle(b);assert(document.defaultView===window);"
        "assert(typeof CSSStyleDeclaration==='function'&&cs instanceof CSSStyleDeclaration&&b.style instanceof CSSStyleDeclaration,"
        "'one interface, both declarations');throws('TypeError',()=>new CSSStyleDeclaration());"
        "assert(Object.prototype.toString.call(cs)==='[object CSSStyleDeclaration]'&&CSSStyleDeclaration.prototype.constructor===CSSStyleDeclaration);"
        "assert(cs.setProperty!==b.style.setProperty&&cs.color!==undefined,'the computed members shadow the style object\\'s');"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(Object.getPrototypeOf(document),'defaultView').get.call(b));"
        "assert(cs.color==='rgb(1, 2, 3)'&&cs.getPropertyValue('COLOR')==='rgb(1, 2, 3)','camel case, any case');"
        "assert(cs.backgroundColor===cs['background-color']&&cs.backgroundColor==='p2','dashed attributes');"
        "assert(cs.WebkitTextFillColor===cs.webkitTextFillColor&&cs['-webkit-text-fill-color']===cs.webkitTextFillColor,'webkit-cased');"
        "assert(cs.cssFloat===cs.float&&cs.cssFloat===cs.getPropertyValue('float'));"
        "assert(cs.getPropertyValue('--custom')===''&&cs.getPropertyValue('nonsense')===''&&cs.getPropertyPriority('color')==='');"
        "var a=cs.width,z=cs.width;assert(a!==z,'every read asks again: live');"
        "assert(cs.cssText===''&&cs.parentRule===null);"
        "assert(cs.length>100&&cs.item(0)===cs[0]&&cs.item(cs.length)===''&&cs[cs.length]===undefined);"
        "var names=[...cs];assert(names.length===cs.length&&names.indexOf('color')>=0);"
        "var sorted=names.slice().sort((x,y)=>(x[0]==='-')!==(y[0]==='-')?(x[0]==='-'?1:-1):(x<y?-1:1));"
        "assert(names.join()===sorted.join(),'lexicographic, prefixed names last');"
        "assert(names[names.length-1]==='-webkit-text-fill-color');"
        "throws('NoModificationAllowedError',()=>cs.setProperty('color','red'));"
        "throws('NoModificationAllowedError',()=>cs.removeProperty('color'));"
        "throws('NoModificationAllowedError',()=>{cs.color='red'});"
        "throws('NoModificationAllowedError',()=>{cs.cssText='color:red'});"
        "assert(getComputedStyle(b)!==cs,'a new declaration each call');"
        "var p=getComputedStyle(b,'::after');assert(p.color===''&&p.length===0,'a pseudo-element answers empty');"
        "assert(getComputedStyle(b,'after').color==='rgb(1, 2, 3)','no colon: the element');"
        "assert(getComputedStyle(b,null).color==='rgb(1, 2, 3)'&&getComputedStyle(b,'').color==='rgb(1, 2, 3)');"
        "throws('TypeError',()=>getComputedStyle());throws('TypeError',()=>getComputedStyle(document));"
        "throws('TypeError',()=>getComputedStyle({}));throws('TypeError',()=>getComputedStyle(document.createTextNode('t')));"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(Object.getPrototypeOf(cs),'color').get.call(b.style));"
        "var loose=document.createElement('div'),lcs=getComputedStyle(loose);"
        "assert(lcs.color===''&&lcs.length===0&&lcs.item(0)===''&&[...lcs].length===0,'a detached element has no style');"
        "document.body.appendChild(loose);assert(lcs.color==='rgb(1, 2, 3)'&&lcs.length>0,'connected, it has');"
        "loose.remove();assert(lcs.length===0);",
        "computed: the declaration");
    check(c.last != NULL && c.last->kind == OS64_HTML_ELEMENT, "computed: the provider is asked for the element");
    os64_dom_geometry_stats(f.binding, true);
    unsigned calls = c.calls;
    dom_script(&f, "getComputedStyle(document.body).color;getComputedStyle(document.body).display;", "computed: cost");
    check(c.calls == calls + 2 && os64_dom_geometry_stats(f.binding, false).layouts == 2,
          "computed: each read's style costs what the provider said");
    c.refuse = true;
    dom_script(&f, VIEW_ASSERT "throws('InvalidStateError',()=>getComputedStyle(document.body).color);", "computed: refused");
    c.refuse = false;
    size_t base = os64_dom_bytes(f.binding);
    dom_script(&f, "for(var i=0;i<500;i++)getComputedStyle(document.body).color;", "computed: in a loop");
    check(os64_dom_bytes(f.binding) == base, "computed: a declaration is engine memory, the snapshot one buffer");
    fixture_free(&f);
    // A snapshot past the binding's budget is refused whole.
    os64_dom_options_t small = os64_dom_default_options();
    small.max_bytes = 512 * 1024;
    DomFixture tight = fixture_new(&small);
    if (fixture_ready(&tight)) {
        ComputedFake spill = {.spill = true, .document = tight.document};
        os64_dom_set_computed(tight.binding, fake_computed, &spill);
        size_t before = os64_dom_bytes(tight.binding);
        for (int i = 0; i < 4; i++)
            dom_script(&tight, VIEW_ASSERT "throws('QuotaExceededError',()=>getComputedStyle(document.body).float);",
                       "computed: a snapshot past the budget");
        spill.spill = false;
        dom_script(&tight, VIEW_ASSERT "assert(getComputedStyle(document.body).color==='rgb(1, 2, 3)');",
                   "computed: a snapshot that fits, after one that did not");
        check(os64_dom_bytes(tight.binding) <= before + 64 * 1024,
              "computed: a refused snapshot keeps nothing it grew for");
        fixture_free(&tight);
    }
}

static void dom_view_history_cases(void)
{
    DomFixture f = view_fixture(true, "http://view.test/dir/page.html?q=1#start");
    if (!fixture_ready(&f)) return;
    dom_script(&f, VIEW_ASSERT
        "assert(history.state===null&&history.length===1&&history.scrollRestoration==='auto');"
        "history.pushState({a:1,b:[1,'two',null,true],c:{d:undefined,e:'\\u2603'}},'');"
        "var s=history.state;assert(s.a===1&&s.b[1]==='two'&&s.b[2]===null&&s.c.e==='\\u2603'&&!('d' in s.c),'JSON carries it');"
        "assert(history.state===s,'the same object each read');"
        "var mine={x:1};history.replaceState(mine,'');assert(history.state!==mine&&history.state.x===1,'a fresh copy');"
        "assert(location.href==='http://view.test/dir/page.html?q=1#start','no address: the same one');"
        "history.pushState(null,'','other.html#frag');assert(location.href==='http://view.test/dir/other.html#frag'&&history.state===null);"
        "history.pushState(2,'','?x=1');assert(location.search==='?x=1'&&location.pathname==='/dir/other.html'&&history.state===2);"
        "history.pushState(undefined,'');assert(history.state===null,'undefined is null');"
        "history.pushState([undefined],'');assert(history.state[0]===null,'in an array undefined is null');"
        "assert(location.href==='http://view.test/dir/other.html?x=1','no address: unchanged');"
        "history.pushState(3,'','#keep');history.pushState(4,'','');assert(location.hash==='#keep'&&history.state===4,'an empty address is none');",
        "history: state updates");
    dom_script(&f, VIEW_ASSERT
        "throws('SecurityError',()=>history.pushState(1,'','http://elsewhere.test/'));"
        "throws('SecurityError',()=>history.pushState(1,'','https://view.test/'));"
        "throws('SecurityError',()=>history.replaceState(1,'','http://view.test:8080/'));"
        "throws('SecurityError',()=>history.pushState(1,'','javascript:1'));"
        "throws('DataCloneError',()=>history.pushState(()=>1,''));"
        "throws('DataCloneError',()=>history.pushState({f(){}},''));"
        "throws('DataCloneError',()=>history.pushState(Symbol('s'),''));"
        "throws('DataCloneError',()=>history.pushState(1n,''));"
        "throws('DataCloneError',()=>history.pushState(NaN,''));"
        "throws('DataCloneError',()=>history.pushState({n:-Infinity},''));"
        "throws('DataCloneError',()=>history.pushState(-0,''));"
        "throws('DataCloneError',()=>history.pushState(new Date(0),''));"
        "throws('DataCloneError',()=>history.pushState(new Map(),''));"
        "throws('DataCloneError',()=>history.pushState(document.body,''));"
        "throws('DataCloneError',()=>history.pushState([1,,3],''));"
        "var named=[1];named.x=2;throws('DataCloneError',()=>history.pushState(named,''));"
        "var loop={};loop.self=loop;throws('DataCloneError',()=>history.pushState(loop,''));"
        "var deep={};for(var i=0,at=deep;i<100;i++)at=at.n={};throws('DataCloneError',()=>history.pushState(deep,''));"
        "throws('QuotaExceededError',()=>history.pushState('x'.repeat(70000),''));"
        "throws('TypeError',()=>history.pushState(1));"
        "throws('TypeError',()=>history.pushState(1,Symbol('t')));"
        "var shared={k:1};history.pushState({a:shared,b:shared},'');assert(history.state.a.k===1&&history.state.b.k===1);"
        "var gets=0;history.pushState({get g(){gets++;return 5}},'');assert(gets===1&&history.state.g===5,'a getter runs once');"
        "var refused;try{history.pushState(()=>1,'','http://elsewhere.test/')}catch(e){refused=e.name}"
        "assert(refused==='DataCloneError','the state is judged before the address');"
        "assert(history.state.g===5,'a refusal changes nothing');"
        "history.scrollRestoration='manual';assert(history.scrollRestoration==='manual');"
        "history.scrollRestoration='nonsense';assert(history.scrollRestoration==='manual');"
        "history.scrollRestoration='auto';assert(history.scrollRestoration==='auto');history.scrollRestoration='manual';",
        "history: refusals");
    check(os64_dom_history_manual(f.binding), "history: the page's scroll restoration reaches the host");
    HistoryFake h = {.length = 4};
    os64_dom_history_t host = {fake_history_update, fake_history_fragment, fake_history_length, &h};
    os64_dom_set_history(f.binding, &host);
    dom_script(&f, VIEW_ASSERT
        "assert(history.length===4,'the host counts the entries');"
        "history.pushState({k:'v'},'','/next');assert(history.length===5&&location.pathname==='/next');",
        "history: through the host");
    check(h.updates == 1 && h.push && strcmp(h.url, "http://view.test/next") == 0 && h.has_state &&
          strcmp(h.state, "{\"k\":\"v\"}") == 0, "history: the host hears the entry, its address and its JSON");
    dom_script(&f, "history.replaceState(null,'');", "history: replace through the host");
    check(h.updates == 2 && !h.push && !h.has_state, "history: replace, and a null state is no state");
    h.refuse = true;
    dom_script(&f, VIEW_ASSERT
        "throws('QuotaExceededError',()=>history.pushState(9,'','/refused'));"
        "assert(location.pathname==='/next'&&history.state===null,'the host refused: nothing changed');",
        "history: the host refuses");
    h.refuse = false;
    // A fragment navigation is the host's, at once.
    unsigned fragments = h.fragments;
    dom_script(&f, VIEW_ASSERT
        "location.hash='part';assert(location.hash==='#part'&&location.href==='http://view.test/next#part','at once');"
        "assert(history.state===null);location.hash='#part';location.hash='part';location.href=location.href;",
        "history: location.hash");
    check(h.fragments == fragments + 2 && h.replace && strcmp(h.fragment, "http://view.test/next#part") == 0,
          "history: the host takes each, the hash it has already no navigation, the same address replacing");
    os64_dom_navigation_t ask;
    check(!os64_dom_take_navigation(f.binding, &ask), "history: a fragment the host took is no ask");
    dom_script(&f, "location.href='#two';location.replace('#three');", "history: href and replace");
    check(h.fragments == fragments + 4 && h.replace && strcmp(h.fragment, "http://view.test/next#three") == 0,
          "history: replace() replaces");
    dom_script(&f, "location.href='/elsewhere#x';", "history: another document");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_URL &&
              strcmp(ask.url, "http://view.test/elsewhere#x") == 0,
          "history: another address is still an ask");
    h.refuse = true;
    dom_script(&f, VIEW_ASSERT "location.hash='refused';assert(location.hash==='#three','refused: unchanged');",
               "history: a refused fragment");
    check(os64_dom_take_navigation(f.binding, &ask) && strcmp(ask.url, "http://view.test/next#refused") == 0,
          "history: a fragment the host refused is asked for instead");
    h.refuse = false;
    // A traversal, then its events.
    dom_script(&f, "var events=[];addEventListener('popstate',e=>events.push('pop:'+JSON.stringify(e.state)+':'+"
        "(e.state===history.state)+':'+(e instanceof PopStateEvent)+':'+e.bubbles+e.cancelable+e.hasUAVisualTransition));"
        "onhashchange=e=>events.push('hash:'+e.oldURL+'>'+e.newURL+':'+(e instanceof HashChangeEvent));",
        "history: listeners");
    check(os64_dom_history_moved(f.binding, "http://view.test/next#back", "{\"n\":7}", 7),
          "history: the host moves the document");
    os64_js_outcome_t outcome;
    os64_dom_history_event_t pop = {.state = "{\"n\":7}", .length = 7, .entry = os64_dom_history_entry(f.binding)};
    check(os64_dom_history_event(f.binding, &pop, &outcome) == OS64_JS_OK, "history: popstate");
    os64_dom_history_event_t hash = {true, "http://view.test/next#three", "http://view.test/next#back", NULL, 0, 0};
    check(os64_dom_history_event(f.binding, &hash, &outcome) == OS64_JS_OK, "history: hashchange");
    dom_script(&f, VIEW_ASSERT
        "assert(events.join()==='pop:{\"n\":7}:true:true:falsefalsefalse,hash:http://view.test/next#three>http://view.test/next#back:true',events.join());"
        "assert(location.href==='http://view.test/next#back'&&history.state.n===7);"
        "var e=new PopStateEvent('popstate');assert(e.state===null&&e.hasUAVisualTransition===false);"
        "var o={};e=new PopStateEvent('x',{state:o,hasUAVisualTransition:1,bubbles:true});assert(e.state===o&&e.hasUAVisualTransition===true&&e.bubbles);"
        "e=new HashChangeEvent('hashchange',{oldURL:'a',newURL:'b'});assert(e.oldURL==='a'&&e.newURL==='b');"
        "assert(new HashChangeEvent('x').oldURL==='');throws('TypeError',()=>PopStateEvent('x'));"
        "assert(Object.getPrototypeOf(PopStateEvent.prototype)===Event.prototype);",
        "history: what was heard");
    // An init dictionary that is a primitive other than undefined or null
    // is a TypeError (WebIDL § 3.2.17), for every Event interface alike;
    // undefined and null are an empty one. Codex round 2.
    dom_script(&f, VIEW_ASSERT
        "throws('TypeError',()=>new PopStateEvent('x',1));throws('TypeError',()=>new HashChangeEvent('x','bad'));"
        "throws('TypeError',()=>new MediaQueryListEvent('x',true));throws('TypeError',()=>new Event('x',0));"
        "assert(new PopStateEvent('x',null).state===null&&new HashChangeEvent('x',undefined).newURL===''&&"
        "new MediaQueryListEvent('x',null).media===''&&new Event('x',null).type==='x');"
        "var order=[];new Event({toString(){order.push('type');return 'x'}},{get bubbles(){order.push('init');return 1}});"
        "assert(order.join()==='type,init',order.join());",
        "event constructors: a primitive init is a TypeError, undefined and null are empty, the type converts first");
    check(!os64_dom_history_moved(f.binding, NULL, NULL, 0) && !os64_dom_history_moved(f.binding, "x", NULL, 3),
          "history: a move with no address, or a length with no state, is refused");
    os64_dom_history_event_t no_old = {true, NULL, "http://view.test/x", NULL, 0, 0};
    os64_dom_history_event_t no_new = {true, "http://view.test/x", NULL, NULL, 0, 0};
    os64_dom_history_event_t no_state = {false, NULL, NULL, NULL, 3, 0};
    check(os64_dom_history_event(f.binding, &no_old, &outcome) == OS64_JS_BAD_ARGUMENT &&
              os64_dom_history_event(f.binding, &no_new, &outcome) == OS64_JS_BAD_ARGUMENT,
          "history: a hashchange needs both addresses");
    check(os64_dom_history_event(f.binding, &no_state, &outcome) == OS64_JS_BAD_ARGUMENT &&
              os64_dom_history_event(f.binding, NULL, &outcome) == OS64_JS_BAD_ARGUMENT,
          "history: a popstate's length needs its state, and an event is needed");
    check(os64_dom_history_event(NULL, &pop, &outcome) == OS64_JS_BAD_ARGUMENT, "history: NULL binding");
    check(os64_dom_history_entry(NULL) == 0, "history: a NULL binding names no entry");
    os64_dom_set_history(f.binding, NULL);
    dom_script(&f, VIEW_ASSERT "assert(history.length===1,'no host: one entry');", "history: no host");
    fixture_free(&f);
}

static void dom_view_off_cases(void)
{
    DomFixture f = view_fixture(false, "http://view.test/page.html");
    if (!fixture_ready(&f)) return;
    ViewFake v = {.view = {.width = 800, .height = 600}};
    os64_dom_view_provider_t provider = {fake_view_read, fake_view_scroll, &v};
    os64_dom_set_view(f.binding, &provider);
    HistoryFake h = {.length = 3};
    os64_dom_history_t host = {fake_history_update, fake_history_fragment, fake_history_length, &h};
    os64_dom_set_history(f.binding, &host);
    dom_script(&f, VIEW_ASSERT
        "for(const n of ['scrollX','pageYOffset','innerWidth','outerHeight','scroll','scrollTo','scrollBy','focus','blur',"
        "'onscroll','onresize','matchMedia','MediaQueryList','MediaQueryListEvent','getComputedStyle',"
        "'onpopstate','onhashchange','PopStateEvent','HashChangeEvent','CSSStyleDeclaration'])assert(!(n in window),n);"
        "assert(!('pushState' in history)&&!('replaceState' in history)&&!('state' in history)&&!('scrollRestoration' in history));"
        "assert(!('scrollingElement' in document)&&!('defaultView' in document)&&!('onscroll' in document.body)&&!('onscroll' in document));"
        "assert(history.length===1,'the classic length');"
        "var heard=0;addEventListener('scroll',()=>heard++);addEventListener('popstate',()=>heard++);"
        "location.hash='x';",
        "modern off: the view's names are absent");
    os64_dom_navigation_t ask;
    check(os64_dom_take_navigation(f.binding, &ask) && strcmp(ask.url, "http://view.test/page.html#x") == 0 &&
              h.fragments == 0,
          "modern off: a fragment is the ask it always was");
    dom_script(&f, "history.back();history.go(-2);", "modern off: two traversals");
    check(os64_dom_take_navigation(f.binding, &ask) && ask.kind == OS64_DOM_NAVIGATE_HISTORY && ask.delta == -2,
          "modern off: the last traversal asked wins, as before the arc");
    v.view.scroll_y = 50;
    os64_js_outcome_t outcome;
    check(!os64_dom_view_step(f.binding, &outcome), "modern off: the view owes no event");
    os64_dom_history_event_t pop = {.entry = os64_dom_history_entry(f.binding)};
    check(os64_dom_history_event(f.binding, &pop, &outcome) == OS64_JS_OK, "modern off: popstate is OK");
    dom_script(&f, VIEW_ASSERT "assert(heard===0,'nothing dispatched');", "modern off: nothing heard");
    fixture_free(&f);
}

/* What a history event carries is what it was owed with, and a script's
 * fragment navigation fires its popstate inside the script: the hash
 * setter's own rule, the synchronous popstate, and an owed event read
 * after other moves came between. */
static void dom_view_owed_cases(void)
{
    DomFixture f = view_fixture(true, "http://view.test/p#a%20b");
    if (!fixture_ready(&f)) return;
    HistoryFake h = {.length = 1};
    os64_dom_history_t host = {fake_history_update, fake_history_fragment, fake_history_length, &h};
    os64_dom_set_history(f.binding, &host);
    dom_script(&f, VIEW_ASSERT
        "location.hash='a b';location.hash='#a b';location.hash='a%20b';"
        "assert(location.href==='http://view.test/p#a%20b','the fragment it has: nothing');",
        "owed: the hash setter's own rule");
    check(h.fragments == 0, "owed: a hash the address already has is no navigation (HTML's hash setter)");
    dom_script(&f, VIEW_ASSERT
        "location.hash='q\"<>`';assert(location.hash==='#q%22%3C%3E%60','the fragment percent-encode set');"
        "var log=[];addEventListener('popstate',e=>log.push('pop:'+JSON.stringify(e.state)+location.hash+':'+(e.state===history.state)));"
        "location.hash='x';log.push('after');history.pushState({n:1},'');"
        "assert(log.join()==='pop:null#x:true,after',log.join());"
        "var thrower=()=>{throw Error('a listener that throws')};addEventListener('popstate',thrower);"
        "location.hash='y';log.push('went on');removeEventListener('popstate',thrower);"
        "assert(log.join()==='pop:null#x:true,after,pop:null#y:true,went on',log.join());",
        "owed: a script's fragment navigation fires popstate inside it");
    check(h.fragments == 3, "owed: each fragment navigation reached the host");
    os64_js_outcome_t outcome;
    check(os64_dom_take_report(f.binding, &outcome) && strstr(outcome.message, "a listener that throws") != NULL,
          "owed: the throwing listener is reported, as a nested dispatch's is");
    dom_script(&f, "var heard=[];onhashchange=e=>heard.push(e.oldURL.split('#')[1]+'>'+e.newURL.split('#')[1]);"
        "addEventListener('popstate',e=>heard.push('pop:'+JSON.stringify(e.state)+':'+(e.state===history.state)));",
        "owed: listeners");
    uint64_t owed_for = os64_dom_history_entry(f.binding);
    check(os64_dom_history_moved(f.binding, "http://view.test/p#c", "{\"now\":3}", 9),
          "owed: another move comes before the owed events run");
    os64_dom_history_event_t hash = {true, "http://view.test/p#a", "http://view.test/p#b", NULL, 0, 0};
    os64_dom_history_event_t stale = {false, NULL, NULL, "{\"then\":1}", 10, owed_for};
    os64_dom_history_event_t current = {.state = "{\"now\":3}", .length = 9,
                                        .entry = os64_dom_history_entry(f.binding)};
    check(os64_dom_history_event(f.binding, &hash, &outcome) == OS64_JS_OK &&
              os64_dom_history_event(f.binding, &stale, &outcome) == OS64_JS_OK &&
              os64_dom_history_event(f.binding, &current, &outcome) == OS64_JS_OK,
          "owed: the events run");
    dom_script(&f, VIEW_ASSERT
        "assert(heard.join()==='a>b,pop:{\"then\":1}:false,pop:{\"now\":3}:true',heard.join());",
        "owed: each carries what it was owed with; only the current entry's is history.state");
    fixture_free(&f);
}

/* The arc's handler types are handlers only with the switch on, as
 * properties and as content attributes alike: with it off a
 * `<body onhashchange>` or a `<div onscroll>` is an ordinary attribute,
 * which an event the page dispatches itself does not run. */
static void dom_view_attribute_cases(void)
{
    for (int modern = 0; modern < 2; modern++) {
        DomFixture f = view_fixture(modern != 0, "http://view.test/page.html");
        if (!fixture_ready(&f)) continue;
        char source[1024];
        snprintf(source, sizeof(source), VIEW_ASSERT
            "var ran=[];document.body.setAttribute('onhashchange','ran.push(\"hash\")');"
            "document.body.setAttribute('onpopstate','ran.push(\"pop\")');"
            "var d=document.createElement('div');d.setAttribute('onscroll','ran.push(\"scroll\")');"
            "d.setAttribute('onresize','ran.push(\"resize\")');document.body.appendChild(d);"
            "dispatchEvent(new Event('hashchange'));dispatchEvent(new Event('popstate'));"
            "d.dispatchEvent(new Event('scroll'));d.dispatchEvent(new Event('resize'));"
            "assert(ran.join()===%s,ran.join());"
            "assert(typeof onhashchange===%s&&typeof d.onscroll===%s);",
            modern ? "'hash,pop,scroll,resize'" : "''", modern ? "'function'" : "'undefined'",
            modern ? "'function'" : "'undefined'");
        dom_script(&f, source, modern ? "attributes: the arc's handlers with the switch on"
                                      : "attributes: no arc handler with the switch off");
        check(os64_dom_listens(f.binding, "hashchange") == (modern != 0) &&
                  os64_dom_listens(f.binding, "scroll") == (modern != 0),
              "attributes: listens counts the arc's attributes only with the switch on");
        fixture_free(&f);
    }
}

/* Every binding allocation the slice's operations make, failed in turn:
 * the script sees QuotaExceededError (or the operation is whole), and what
 * it would have changed is as it was. */
static void dom_view_allocation_cases(void)
{
    static const char *const operations[] = {
        "history.pushState({a:[1,2,'three']},'','?n=1')",
        "history.replaceState('kept','','#r')",
        "matchMedia('(min-width: 1px)').addEventListener('change',()=>{})",
        "getComputedStyle(document.body).color",
        "location.hash='frag'",
        "addEventListener('popstate',e=>{globalThis.heard=e.state});location.hash='frag'",
        "history.state",
    };
    for (size_t op = 0; op < sizeof(operations) / sizeof(operations[0]); op++) {
        DomFixture f = view_fixture(true, "http://view.test/page.html");
        if (!fixture_ready(&f)) continue;
        ComputedFake c = {.document = f.document};
        HistoryFake h = {.length = 1};
        os64_dom_history_t host = {fake_history_update, fake_history_fragment, fake_history_length, &h};
        os64_dom_set_computed(f.binding, fake_computed, &c);
        os64_dom_set_history(f.binding, &host);
        size_t start = dom_attempts;
        dom_script(&f, operations[op], "view-allocation-baseline.js");
        size_t count = dom_attempts - start;
        fixture_free(&f);
        for (size_t cut = 1; cut <= count; cut++) {
            f = view_fixture(true, "http://view.test/page.html");
            if (!fixture_ready(&f)) break;
            c = (ComputedFake){.document = f.document};
            h = (HistoryFake){.length = 1};
            os64_dom_set_computed(f.binding, fake_computed, &c);
            os64_dom_set_history(f.binding, &host);
            char source[1024];
            snprintf(source, sizeof(source),
                     "var before=location.href;try{%s;globalThis.refused=false}catch(e){"
                     "if(e.name!=='QuotaExceededError')throw e;globalThis.refused=true}", operations[op]);
            size_t target = dom_attempts + cut;
            dom_fail_at = target;
            int ok = dom_script(&f, source, "view-allocation-cut.js");
            dom_fail_at = 0;
            dom_sweeps++;
            check(dom_attempts >= target, "view: injection point reached");
            if (ok)
                dom_script(&f, VIEW_ASSERT "assert(!refused||location.href===before,'a refusal moved the address');",
                           "view-allocation-caught.js");
            fixture_free(&f);
        }
    }
}

/* The host's half under the same sweep: a move, the two history events and
 * each kind of view step, every binding allocation each makes failed in
 * turn. A refused move answers false and leaves the address and the state
 * as they were; an event that could not be built or delivered reports
 * through its outcome; neither leaks (fixture_free counts). */
enum { HOST_MOVE, HOST_POP, HOST_POP_STALE, HOST_HASH, HOST_RESIZE, HOST_SCROLL, HOST_CHANGE, HOST_OPS };

static DomFixture host_op_fixture(int op, ViewFake *v, os64_dom_view_provider_t *provider)
{
    DomFixture f = view_fixture(true, "http://view.test/page.html");
    if (!fixture_ready(&f)) return f;
    *v = (ViewFake){.view = {.width = 400, .height = 300}};
    *provider = (os64_dom_view_provider_t){fake_view_read, fake_view_scroll, v};
    os64_dom_set_view(f.binding, provider);
    dom_script(&f, "globalThis.heard=[];"
        "addEventListener('popstate',e=>heard.push('pop:'+JSON.stringify(e.state)));"
        "onhashchange=e=>heard.push('hash:'+e.newURL);"
        "addEventListener('scroll',()=>heard.push('scroll'));"
        "var mq=matchMedia('(min-width: 500px)');mq.onchange=e=>heard.push('change:'+e.matches+':'+e.media);"
        "history.replaceState({was:1},'');",
        "host op: listeners");
    if (op == HOST_RESIZE)
        dom_script(&f, "addEventListener('resize',()=>heard.push('resize'));", "host op: resize listener");
    return f;
}

static bool host_op(DomFixture *f, ViewFake *v, int op, os64_js_status_t *status)
{
    os64_js_outcome_t outcome = {0};
    *status = OS64_JS_OK;
    switch (op) {
    case HOST_MOVE:
        return os64_dom_history_moved(f->binding, "http://view.test/page.html#moved", "{\"n\":1}", 7);
    case HOST_POP: {
        os64_dom_history_event_t pop = {.entry = os64_dom_history_entry(f->binding)};
        *status = os64_dom_history_event(f->binding, &pop, &outcome);
        return true;
    }
    case HOST_POP_STALE: {
        // Owed for an entry that is no longer the current one: its state
        // is read from the JSON it carried.
        os64_dom_history_event_t pop = {.state = "{\"old\":2}", .length = 9,
                                        .entry = os64_dom_history_entry(f->binding) + 1};
        *status = os64_dom_history_event(f->binding, &pop, &outcome);
        return true;
    }
    case HOST_HASH: {
        os64_dom_history_event_t hash = {true, "http://view.test/page.html#old", "http://view.test/page.html",
                                         NULL, 0, 0};
        *status = os64_dom_history_event(f->binding, &hash, &outcome);
        return true;
    }
    case HOST_RESIZE:
        v->view.height = 320;            // the list reads only the width
        break;
    case HOST_SCROLL:
        v->view.scroll_y = 25;
        break;
    case HOST_CHANGE:
        v->view.width = 800;
        break;
    }
    bool ran = os64_dom_view_step(f->binding, &outcome);
    *status = outcome.status;
    return ran;
}

static void dom_view_host_allocation_cases(void)
{
    // What the page heard, as JS string literals.
    static const char *const heard[HOST_OPS] = {
        "''", "'pop:{\"was\":1}'", "'pop:{\"old\":2}'", "'hash:http://view.test/page.html'", "'resize'", "'scroll'",
        "'change:true:(min-width: 500px)'",
    };
    for (int op = 0; op < HOST_OPS; op++) {
        ViewFake v;
        os64_dom_view_provider_t provider;
        DomFixture f = host_op_fixture(op, &v, &provider);
        if (!fixture_ready(&f)) continue;
        os64_js_status_t status;
        size_t start = dom_attempts;
        bool done = host_op(&f, &v, op, &status);
        size_t count = dom_attempts - start;
        check(done && status == OS64_JS_OK, "host op: the baseline runs");
        char source[1024];
        snprintf(source, sizeof(source), VIEW_ASSERT "assert(heard.join()===%s,heard.join());", heard[op]);
        dom_script(&f, source, "host op: the baseline is heard");
        fixture_free(&f);
        for (size_t cut = 1; cut <= count; cut++) {
            f = host_op_fixture(op, &v, &provider);
            if (!fixture_ready(&f)) break;
            size_t target = dom_attempts + cut;
            dom_fail_at = target;
            done = host_op(&f, &v, op, &status);
            dom_fail_at = 0;
            dom_sweeps++;
            check(dom_attempts >= target, "host op: injection point reached");
            if (op == HOST_MOVE && !done)
                dom_script(&f, VIEW_ASSERT "assert(location.href==='http://view.test/page.html'&&history.state.was===1,"
                               "'a refused move changed the document');",
                           "host op: a refused move");
            else if (op == HOST_MOVE)
                dom_script(&f, VIEW_ASSERT "assert(location.hash==='#moved'&&history.state.n===1);", "host op: moved");
            // Whatever the cut cost, the binding is whole: a page task runs.
            dom_script(&f, VIEW_ASSERT "assert(Array.isArray(heard)&&mq.media==='(min-width: 500px)');",
                       "host op: the binding after a cut");
            fixture_free(&f);
        }
    }
}

/* Edges a hostile page reaches for: a query nested past libgarb's depth or
 * a megabyte long, a NUL inside a name or a query (a script string's NUL
 * is U+FFFD by the time a C string sees it, so it ends nothing), and a
 * state exactly at its cap and one byte past it. */
static void dom_view_edge_cases(void)
{
    DomFixture f = view_fixture(true, "http://view.test/page.html");
    if (!fixture_ready(&f)) return;
    ViewFake v = {.view = {.width = 800, .height = 600}};
    os64_dom_view_provider_t provider = {fake_view_read, fake_view_scroll, &v};
    os64_dom_set_view(f.binding, &provider);
    ComputedFake c = {.document = f.document};
    os64_dom_set_computed(f.binding, fake_computed, &c);
    dom_script(&f, VIEW_ASSERT
        "var deep=matchMedia('('.repeat(100000)+')'.repeat(100000));assert(!deep.matches&&deep.media==='not all');"
        "var q='(width)';for(var i=0;i<60;i++)q='('+q+')';var nested=matchMedia(q);"
        "assert(nested.matches&&nested.media===q,'nested within the bound: read once a level');"
        "var long=matchMedia('(min-width: 1px) and '.repeat(50000)+'(color)');assert(long.matches,'a long list');"
        "var big=matchMedia('x'.repeat(1<<20));assert(!big.matches&&big.media.length===1<<20,'a long unknown type');"
        "assert(!matchMedia('screen\\0, print').matches,'a NUL is part of the type, not its end');"
        "assert(matchMedia('screen\\0, print').media==='screen\\ufffd, print');"
        "assert(getComputedStyle(document.body).getPropertyValue('color\\0')==='','a NUL is part of the name');"
        "history.pushState('x'.repeat(65534),'');assert(history.state.length===65534,'JSON exactly at the cap');"
        "throws('QuotaExceededError',()=>history.pushState('x'.repeat(65535),''));"
        "assert(history.state.length===65534,'a refusal keeps the state');"
        "throws('QuotaExceededError',()=>history.pushState({k:'x'.repeat(65530)},''));",
        "edges: queries, names and the state's cap");
    fixture_free(&f);
}

static void dom_view_cases(void)
{
    dom_view_scroll_cases();
    dom_view_scrolling_element_cases();
    dom_view_event_cases();
    dom_view_computed_cases();
    dom_view_history_cases();
    dom_view_off_cases();
    dom_view_edge_cases();
    dom_view_owed_cases();
    dom_view_attribute_cases();
}
#undef VIEW_ASSERT
