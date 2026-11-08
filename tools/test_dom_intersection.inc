/* INTERSECTIONOBSERVER AND DOMRECT (libdom/intersection.c, rect.c;
 * INTERSECTION_OBSERVER.md): the interface and its conversions, then the
 * standard's arithmetic against a fake batch whose answers are a table kept
 * by element id, over a fake view 800 by 600. The step and the delivery
 * task are the host's calls (os64_dom_intersections_update, _fire), made
 * here between scripts; `L` is each case's log. */

#define IO_ASSERT STORAGE_ASSERT \
    "function near(a,b){return Math.abs(a-b)<1e-9}" \
    "function rect(r,l,t,rr,b){return near(r.left,l)&&near(r.top,t)&&near(r.right,rr)&&near(r.bottom,b)}"

typedef struct {
    const char *id;
    bool boxed, contained, root_boxed;
    double rect[4], clip[4], root_rect[4];
} IoFake;

static IoFake io_fakes[16];
static size_t io_nfakes, io_calls, io_asked;
static bool io_refuse;
static const os64_html_node_t *io_last_root;

static IoFake *io_fake(const char *id)
{
    for (size_t i = 0; i < io_nfakes; i++)
        if (!strcmp(io_fakes[i].id, id)) return &io_fakes[i];
    if (io_nfakes == sizeof(io_fakes) / sizeof(io_fakes[0])) abort();
    IoFake *f = &io_fakes[io_nfakes++];
    *f = (IoFake){.id = id, .boxed = true, .contained = true, .root_boxed = true,
                  .clip = {-OS64_DOM_UNCLIPPED, -OS64_DOM_UNCLIPPED, OS64_DOM_UNCLIPPED, OS64_DOM_UNCLIPPED}};
    return f;
}

static void io_place(const char *id, double l, double t, double r, double b)
{
    IoFake *f = io_fake(id);
    f->rect[0] = l; f->rect[1] = t; f->rect[2] = r; f->rect[3] = b;
}

static const char *io_id(const os64_html_node_t *node)
{
    const os64_html_attr_t *id = node != NULL ? os64_html_attr(node, "id") : NULL;
    return id != NULL ? id->value : "";
}

static bool io_provider(void *opaque, os64_dom_intersection_t *items, size_t count, os64_dom_geometry_stats_t *cost)
{
    (void)opaque;
    io_calls++;
    io_asked += count;
    cost->layouts = 0;
    if (io_refuse) return false;
    for (size_t i = 0; i < count; i++) {
        IoFake *t = NULL;
        for (size_t k = 0; k < io_nfakes; k++)
            if (!strcmp(io_fakes[k].id, io_id(items[i].target))) t = &io_fakes[k];
        items[i].boxed = t != NULL && t->boxed;
        items[i].contained = t != NULL && t->contained;
        for (int s = 0; s < 4 && t != NULL; s++) {
            items[i].rect[s] = t->rect[s];
            items[i].clip[s] = t->clip[s];
        }
        io_last_root = items[i].root;
        if (items[i].root != NULL) {
            IoFake *r = NULL;
            for (size_t k = 0; k < io_nfakes; k++)
                if (!strcmp(io_fakes[k].id, io_id(items[i].root))) r = &io_fakes[k];
            items[i].root_boxed = r != NULL && r->root_boxed;
            for (int s = 0; s < 4 && r != NULL; s++) items[i].root_rect[s] = r->root_rect[s];
        }
    }
    return true;
}

static void io_reset(void)
{
    io_nfakes = io_calls = io_asked = 0;
    io_refuse = false;
    io_last_root = NULL;
}

static ViewFake io_view;

/* A script of assertions in a block of its own, with the case's helpers. */
static int io_script(DomFixture *f, const char *body, const char *name)
{
    size_t length = strlen(IO_ASSERT) + strlen(body) + 3;
    char *source = malloc(length);
    if (source == NULL) abort();
    snprintf(source, length, "%s{%s}", IO_ASSERT, body);
    int result = dom_script(f, source, name);
    free(source);
    return result;
}

static DomFixture io_fixture(void)
{
    io_reset();
    p2b_ms = 1000;
    p2b_us = 1000000;
    os64_dom_options_t options = p2b_options(P2B_URL);
    DomFixture f = fixture_new(&options);
    if (!fixture_ready(&f)) return f;
    io_view = (ViewFake){.view = {.width = 800, .height = 600}};
    os64_dom_view_provider_t view = {fake_view_read, fake_view_scroll, &io_view, NULL};
    os64_dom_set_view(f.binding, &view);
    os64_dom_set_intersections(f.binding, io_provider, NULL);
    return f;
}

/* The rendering step and then the delivery task, as the host runs them:
 * whether the step left the task due, and the task's outcome OK. */
static bool io_frame(DomFixture *f, const char *what)
{
    bool due = os64_dom_intersections_update(f->binding);
    check(due == os64_dom_intersections_due(f->binding), what);
    if (!due) return false;
    os64_js_outcome_t out;
    bool ran = os64_dom_intersections_fire(f->binding, &out);
    if (ran && out.status != OS64_JS_OK) fprintf(stderr, "%s: %s\n", what, out.message);
    check(ran && out.status == OS64_JS_OK && !os64_dom_intersections_due(f->binding), what);
    return ran;
}

static void dom_rect_cases(void)
{
    DomFixture f = io_fixture();
    if (!fixture_ready(&f)) return;
    io_script(&f,
        "assert(DOMRect.prototype.__proto__===DOMRectReadOnly.prototype&&DOMRect.__proto__===DOMRectReadOnly);"
        "assert(DOMRectReadOnly.length===0&&DOMRect.length===0);"
        "let r=new DOMRectReadOnly(10,20,-30,40);"
        "assert(r.x===10&&r.y===20&&r.width===-30&&r.height===40,'the numbers kept as given');"
        "assert(r.left===-20&&r.right===10&&r.top===20&&r.bottom===60,'the edges from a negative width');"
        "r.x=5;assert(r.x===10,'a read-only rect ignores a sloppy write');"
        "throws('TypeError',()=>{'use strict';Object.getOwnPropertyDescriptor(DOMRect.prototype,'x').set.call(r,1)});"
        "let w=new DOMRect(1,2,3,4);w.x=7;w.width=NaN;"
        "assert(w.x===7&&isNaN(w.width)&&isNaN(w.right)&&isNaN(w.left)&&w.top===2,'a NaN width makes NaN edges');"
        "let j=new DOMRect(1,2,3,4).toJSON();"
        "assert(JSON.stringify(j)==='{\"x\":1,\"y\":2,\"width\":3,\"height\":4,\"top\":2,\"right\":4,\"bottom\":6,\"left\":1}',"
        "JSON.stringify(j));"
        "let order=[];let init={get x(){order.push('x');return 1},get y(){order.push('y');return 2},"
        "get width(){order.push('width');return 3},get height(){order.push('height');return 4}};"
        "let fr=DOMRect.fromRect(init);"
        "assert(order.join()==='height,width,x,y','DOMRectInit read in its order: '+order);"
        "assert(fr instanceof DOMRect&&fr.right===4&&DOMRectReadOnly.fromRect() instanceof DOMRectReadOnly);"
        "assert(!(DOMRectReadOnly.fromRect() instanceof DOMRect));"
        "throws('TypeError',()=>DOMRect.fromRect(1));throws('TypeError',()=>DOMRect(1,2,3,4));"
        "throws('TypeError',()=>DOMRectReadOnly.prototype.x);"
        "throws('TypeError',()=>Object.getOwnPropertyDescriptor(DOMRect.prototype,'x').get.call(new DOMRectReadOnly(5)));"
        "assert(Object.getOwnPropertyDescriptor(DOMRectReadOnly.prototype,'x').get.call(new DOMRect(5))===5,"
        "'a DOMRect is a DOMRectReadOnly');"
        "class Mine extends DOMRect{};assert(new Mine(1,1,1,1) instanceof Mine);",
        "rect: DOMRectReadOnly and DOMRect");
    fixture_free(&f);

    os64_dom_options_t options = os64_dom_default_options();
    options.modern = false;
    f = fixture_new(&options);
    if (!fixture_ready(&f)) return;
    dom_script(&f, "if(typeof DOMRect!=='undefined'||typeof IntersectionObserver!=='undefined')throw Error('modern');",
               "rect: neither exists with the switch off");
    fixture_free(&f);
}

static void dom_intersection_interface_cases(void)
{
    DomFixture f = io_fixture();
    if (!fixture_ready(&f)) return;
    io_script(&f,
        "assert(IntersectionObserver.length===1&&IntersectionObserverEntry.length===1);"
        "throws('TypeError',()=>new IntersectionObserver());throws('TypeError',()=>new IntersectionObserver(1));"
        "throws('TypeError',()=>IntersectionObserver(()=>{}));"
        "throws('TypeError',()=>new IntersectionObserver(()=>{},1));"
        "let o=new IntersectionObserver(()=>{});"
        "assert(o.root===null&&o.rootMargin==='0px 0px 0px 0px'&&o.scrollMargin==='0px 0px 0px 0px');"
        "assert(o.thresholds.length===1&&o.thresholds[0]===0&&o.thresholds===o.thresholds,'[0], the same object');"
        "assert(Object.isFrozen(o.thresholds),'a FrozenArray');"
        "assert(o.delay===0&&o.trackVisibility===false);"
        "o=new IntersectionObserver(()=>{},{rootMargin:' ',threshold:[]});"
        "assert(o.rootMargin==='0px 0px 0px 0px'&&o.thresholds.join()==='0','blank margin, empty list');"
        "let box=document.getElementById('box');"
        "o=new IntersectionObserver(()=>{},{root:box,threshold:[1,0.25,0,0.5],rootMargin:'10% 20px'});"
        "assert(o.root===box&&o.thresholds.join()==='0,0.25,0.5,1'&&o.rootMargin==='10% 20px 10% 20px');"
        "assert(new IntersectionObserver(()=>{},{root:document}).root===document,'a Document root');"
        "assert(new IntersectionObserver(()=>{},{threshold:0.5}).thresholds.join()==='0.5','one number');"
        "assert(new IntersectionObserver(()=>{},{threshold:{valueOf(){return 0.75}}}).thresholds.join()==='0.75',"
        "'an object with no iterator is a number');"
        "assert(new IntersectionObserver(()=>{},{threshold:new Set([0.5,0.1])}).thresholds.join()==='0.1,0.5',"
        "'any iterable');"
        "throws('RangeError',()=>new IntersectionObserver(()=>{},{threshold:[1.1]}));"
        "throws('RangeError',()=>new IntersectionObserver(()=>{},{threshold:-0.1}));"
        "throws('TypeError',()=>new IntersectionObserver(()=>{},{threshold:['foo']}));"
        "throws('TypeError',()=>new IntersectionObserver(()=>{},{threshold:Infinity}));"
        "for(const bad of ['1','2em','auto','calc(1px + 2px)','1px !important','1px 1px 1px 1px 1px','1px,2px','%','px'])"
        "{throws('SyntaxError',()=>new IntersectionObserver(()=>{},{rootMargin:bad}));"
        "throws('SyntaxError',()=>new IntersectionObserver(()=>{},{scrollMargin:bad}));}"
        "const units={px:'10px',cm:'377px',mm:'37px',q:'9px',Q:'9px',in:'960px',pt:'13px',pc:'160px','%':'10%'};"
        "for(const u in units)assert(new IntersectionObserver(()=>{},{rootMargin:'10'+u}).rootMargin===units[u]+' '"
        "+units[u]+' '+units[u]+' '+units[u],u);"
        "assert(new IntersectionObserver(()=>{},{rootMargin:'-1.5px 2.5% +3px'}).rootMargin==='-2px 2.5% 3px 2.5%',"
        "'signs, fractions and the three-value form');"
        "assert(new IntersectionObserver(()=>{},{rootMargin:'1e1px -0px'}).rootMargin==='10px 0px 10px 0px');"
        "let sm=new IntersectionObserver(()=>{},{rootMargin:'1px',scrollMargin:'10px 5%'});"
        "assert(sm.scrollMargin==='10px 5% 10px 5%'&&sm.rootMargin==='1px 1px 1px 1px','each margin its own');"
        "throws('TypeError',()=>new IntersectionObserver(()=>{},{root:1}));"
        "throws('TypeError',()=>new IntersectionObserver(()=>{},{root:document.createTextNode('t')}));"
        "let tv=new IntersectionObserver(()=>{},{trackVisibility:true,delay:20});"
        "assert(tv.trackVisibility===true&&tv.delay===100,'trackVisibility raises the delay to 100');"
        "let order=[];new IntersectionObserver(()=>{},{get delay(){order.push('delay')},get root(){order.push('root')},"
        "get rootMargin(){order.push('rootMargin')},get scrollMargin(){order.push('scrollMargin')},"
        "get threshold(){order.push('threshold')},get trackVisibility(){order.push('trackVisibility')}});"
        "assert(order.join()==='delay,root,rootMargin,scrollMargin,threshold,trackVisibility',"
        "'IntersectionObserverInit in its order: '+order);"
        "throws('Error',()=>new IntersectionObserver(()=>{},{rootMargin:'x',get threshold(){throw Error('t')}}));"
        "throws('SyntaxError',()=>new IntersectionObserver(()=>{},{rootMargin:'x',threshold:[2]}));"
        "let gets=0;new IntersectionObserver(()=>{},{threshold:{get [Symbol.iterator](){gets++;return [].values.bind([0.5])}}});"
        "assert(gets===1,'@@iterator read once: '+gets);"
        "throws('TypeError',()=>o.observe('foo'));throws('TypeError',()=>o.observe(document.createTextNode('t')));"
        "throws('TypeError',()=>o.observe());throws('TypeError',()=>o.unobserve(1));"
        "throws('TypeError',()=>IntersectionObserver.prototype.observe.call({},box));"
        "throws('TypeError',()=>new IntersectionObserverEntry());"
        "throws('TypeError',()=>new IntersectionObserverEntry({}));"
        "let e=new IntersectionObserverEntry({time:5,rootBounds:null,boundingClientRect:{x:1,y:2,width:3,height:4},"
        "intersectionRect:{},isIntersecting:1,isVisible:0,intersectionRatio:0.5,target:box});"
        "assert(e.time===5&&e.rootBounds===null&&e.boundingClientRect instanceof DOMRectReadOnly&&"
        "e.boundingClientRect.bottom===6&&e.intersectionRect.width===0&&e.isIntersecting===true&&"
        "e.isVisible===false&&e.intersectionRatio===0.5&&e.target===box,'an entry from its init');"
        "throws('TypeError',()=>new IntersectionObserverEntry({time:5,rootBounds:null,boundingClientRect:{},"
        "intersectionRect:{},isIntersecting:1,isVisible:0,intersectionRatio:0.5,target:document}));",
        "intersection: the interface");
    fixture_free(&f);
}

static void dom_intersection_step_cases(void)
{
    DomFixture f = io_fixture();
    if (!fixture_ready(&f)) return;
    check(os64_dom_intersections_observed(f.binding) == 0 && !os64_dom_intersections_update(f.binding) &&
          io_calls == 0, "intersection: no targets, no batch");
    io_place("one", 10, 20, 110, 70);
    io_script(&f,
        "globalThis.L=[];globalThis.one=document.getElementById('one');"
        "globalThis.io=new IntersectionObserver(function(entries,observer){"
        "assert(this===io&&observer===io&&arguments.length===2,'this and the arguments');L.push(...entries)});"
        "io.observe(one);io.observe(one);assert(io.takeRecords().length===0,'no observation before a step');",
        "intersection: observe");
    check(os64_dom_intersections_observed(f.binding) == 1 && os64_dom_intersections_owed(f.binding),
          "intersection: one target, owed its first observation");
    check(io_frame(&f, "intersection: the first step queues, the task delivers") && io_asked == 1 &&
          io_last_root == NULL && !os64_dom_intersections_owed(f.binding),
          "intersection: one pair asked, against the viewport");
    io_script(&f,
        "assert(L.length===1,'one entry: '+L.length);let e=L[0];"
        "assert(e instanceof IntersectionObserverEntry&&e.target===one);"
        "assert(e.boundingClientRect instanceof DOMRectReadOnly&&!(e.boundingClientRect instanceof DOMRect));"
        "assert(rect(e.boundingClientRect,10,20,110,70)&&rect(e.intersectionRect,10,20,110,70));"
        "assert(rect(e.rootBounds,0,0,800,600)&&e.isIntersecting===true&&e.intersectionRatio===1&&e.isVisible===false);"
        "assert(typeof e.time==='number'&&e.time>=0&&e.time<=performance.now());"
        "assert(e.boundingClientRect===e.boundingClientRect,'the same rect each read');",
        "intersection: the first entry");
    check(!os64_dom_intersections_update(f.binding), "intersection: nothing changed, no entry");

    /* Half of it past the bottom: ratio 0.5, the index unchanged for [0]. */
    io_place("one", 10, 575, 110, 625);
    check(!os64_dom_intersections_update(f.binding), "intersection: a ratio change within one index is no entry");
    /* Wholly below, then touching the bottom edge, then a zero-area target
     * on the edge. */
    io_place("one", 10, 650, 110, 700);
    check(io_frame(&f, "intersection: out of view"), "intersection: leaving is an entry");
    io_place("one", 10, 600, 110, 650);
    check(io_frame(&f, "intersection: touching"), "intersection: an edge that touches intersects");
    io_place("one", 10, 600, 10, 600);
    check(!os64_dom_intersections_update(f.binding), "intersection: still intersecting, still index 1");
    io_script(&f,
        "assert(L.length===3,String(L.length));"
        "assert(L[1].isIntersecting===false&&L[1].intersectionRatio===0&&rect(L[1].intersectionRect,0,0,0,0));"
        "assert(rect(L[1].boundingClientRect,10,650,110,700));"
        "assert(L[2].isIntersecting===true&&L[2].intersectionRatio===0&&rect(L[2].intersectionRect,10,600,110,600),"
        "'edge-adjacent: intersecting with no area');"
        "globalThis.zero=new IntersectionObserver(es=>L.push(...es));zero.observe(one);",
        "intersection: leaving and touching");
    check(io_frame(&f, "intersection: a zero-area target"), "intersection: the zero-area target's first");
    io_script(&f, "assert(L.length===4&&L[3].intersectionRatio===1&&L[3].isIntersecting,"
               "'a zero-area target that touches has a ratio of 1');zero.disconnect();",
               "intersection: zero area");

    /* Thresholds both ways. */
    io_place("two", 0, 0, 100, 100);
    io_script(&f,
        "L=[];globalThis.two=document.getElementById('two');"
        "globalThis.th=new IntersectionObserver(es=>L.push(...es.map(e=>e.intersectionRatio)),{threshold:[0.5,0.25,1]});"
        "th.observe(two);", "intersection: thresholds");
    check(io_frame(&f, "intersection: thresholds first"), "intersection: thresholds' first");
    io_place("two", 0, 560, 100, 660);  /* 0.4 */
    check(io_frame(&f, "intersection: below one half"), "intersection: crossing 0.5 down");
    io_place("two", 0, 570, 100, 670);  /* 0.3 */
    check(!os64_dom_intersections_update(f.binding), "intersection: 0.4 to 0.3 crosses nothing");
    io_place("two", 0, 580, 100, 680);  /* 0.2 */
    check(io_frame(&f, "intersection: below one quarter"), "intersection: crossing 0.25 down");
    io_place("two", 0, 500, 100, 600);  /* 1 */
    check(io_frame(&f, "intersection: whole again"), "intersection: crossing everything up");
    io_script(&f, "assert(L.map(r=>r.toFixed(2)).join()==='1.00,0.40,0.20,1.00',L.join());th.disconnect();",
               "intersection: the crossings");
    /* Short of the smallest threshold is not intersecting, though the
     * rectangles meet and say so (the standard's issue 432). */
    io_place("two", 0, 560, 100, 660);
    io_script(&f, "L=[];globalThis.half=new IntersectionObserver(es=>L.push(...es),{threshold:0.5});half.observe(two);",
              "intersection: below the only threshold");
    check(io_frame(&f, "intersection: 0.4 against 0.5"), "intersection: the first, below the threshold");
    io_place("two", 0, 540, 100, 640);
    check(io_frame(&f, "intersection: 0.6 against 0.5"), "intersection: reaching the threshold");
    io_script(&f,
        "assert(L.length===2&&L[0].isIntersecting===false&&near(L[0].intersectionRatio,0.4)&&"
        "rect(L[0].intersectionRect,0,560,100,600),'below: not intersecting, the overlap still told');"
        "assert(L[1].isIntersecting===true&&near(L[1].intersectionRatio,0.6));half.disconnect();",
        "intersection: issue 432");

    /* rootMargin on the viewport: pixels, then percentages of its height
     * (top, bottom) and width (left, right). */
    io_place("box", 0, 650, 100, 700);
    io_script(&f,
        "L=[];globalThis.box=document.getElementById('box');"
        "globalThis.m=new IntersectionObserver(es=>L.push(...es),{rootMargin:'10px 20% 50px 5%'});m.observe(box);",
        "intersection: rootMargin");
    check(io_frame(&f, "intersection: margins"), "intersection: margins' first");
    io_script(&f,
        "assert(rect(L[0].rootBounds,-40,-10,960,650),'the grown rectangle');"
        "assert(L[0].isIntersecting&&rect(L[0].intersectionRect,0,650,100,650),'the margin reaches it');"
        "m.disconnect();", "intersection: the margin's rectangle");

    /* A clip between the target and the root cuts it, and can cut it away
     * entirely though it is in the viewport. */
    io_place("field", 0, 0, 100, 100);
    IoFake *clipped = io_fake("field");
    clipped->clip[0] = 50; clipped->clip[2] = 300;
    io_script(&f,
        "L=[];globalThis.field=document.getElementById('field');"
        "globalThis.c=new IntersectionObserver(es=>L.push(...es),{threshold:[0,1]});c.observe(field);",
        "intersection: a clip");
    check(io_frame(&f, "intersection: clipped"), "intersection: clipped first");
    clipped->clip[0] = 200;
    check(io_frame(&f, "intersection: clipped away"), "intersection: clipped away");
    io_script(&f,
        "assert(L[0].intersectionRatio===0.5&&rect(L[0].intersectionRect,50,0,100,100),'half cut');"
        "assert(!L[1].isIntersecting&&rect(L[1].intersectionRect,0,0,0,0)&&rect(L[1].boundingClientRect,0,0,100,100),"
        "'a clip that leaves nothing is not intersecting');c.disconnect();",
        "intersection: clips");

    /* No box: not intersecting, every rectangle zero, the root's too. */
    io_fake("check")->boxed = false;
    io_script(&f,
        "L=[];globalThis.chk=document.getElementById('check');"
        "globalThis.n=new IntersectionObserver(es=>L.push(...es));n.observe(chk);", "intersection: no box");
    check(io_frame(&f, "intersection: unboxed"), "intersection: unboxed first");
    io_script(&f,
        "assert(L.length===1&&!L[0].isIntersecting&&rect(L[0].boundingClientRect,0,0,0,0)&&"
        "rect(L[0].rootBounds,0,0,0,0),'every rectangle zero, rootBounds too');n.disconnect();",
        "intersection: the unboxed entry");

    /* An element root: its rectangle; a target off its chain; a root with
     * no box. */
    IoFake *root = io_fake("form");
    root->root_rect[0] = 0; root->root_rect[1] = 0; root->root_rect[2] = 50; root->root_rect[3] = 50;
    io_place("r1", 25, 25, 75, 75);
    io_place("r2", 0, 0, 10, 10);
    io_fake("r2")->contained = false;
    io_script(&f,
        "L=[];globalThis.form=document.getElementById('form');"
        "globalThis.er=new IntersectionObserver(es=>L.push(...es),{root:form,rootMargin:'10%'});"
        "er.observe(document.getElementById('r1'));er.observe(document.getElementById('r2'));",
        "intersection: an element root");
    check(io_frame(&f, "intersection: the element root") && io_last_root != NULL,
          "intersection: the root is asked for");
    root->root_boxed = false;
    io_place("r1", 25, 25, 75, 76);
    check(io_frame(&f, "intersection: the root loses its box"), "intersection: a root without a box");
    io_script(&f,
        "assert(L.length===3,String(L.length));"
        "assert(L[0].target.id==='r1'&&rect(L[0].rootBounds,-5,-5,55,55)&&rect(L[0].intersectionRect,25,25,55,55),"
        "'the root\\'s rectangle, grown by a percentage of itself');"
        "assert(L[1].target.id==='r2'&&!L[1].isIntersecting,'off the containing-block chain');"
        "assert(L[2].target.id==='r1'&&!L[2].isIntersecting,'a root with no box meets nothing');"
        "er.disconnect();", "intersection: element roots");

    /* A root in another document meets nothing, and is not asked about. */
    io_place("o1", 0, 0, 10, 10);
    io_script(&f,
        "io.disconnect();L=[];let d=document.implementation.createHTMLDocument('x');"
        "globalThis.far=new IntersectionObserver(es=>L.push(...es),{root:d.body});"
        "far.observe(document.getElementById('o1'));", "intersection: a foreign root");
    size_t asked = io_asked;
    check(io_frame(&f, "intersection: foreign root") && io_asked == asked,
          "intersection: a foreign root's targets are not asked about");
    io_script(&f, "assert(L.length===1&&!L[0].isIntersecting);far.disconnect();",
               "intersection: the foreign root's entry");

    /* takeRecords, unobserve and disconnect. */
    io_place("o2", 0, 0, 10, 10);
    io_script(&f,
        "L=[];globalThis.t=new IntersectionObserver(es=>L.push('called'));t.observe(document.getElementById('o2'));",
        "intersection: takeRecords");
    check(os64_dom_intersections_update(f.binding), "intersection: queued for takeRecords");
    io_script(&f,
        "let r=t.takeRecords();assert(r.length===1&&r[0].target.id==='o2'&&t.takeRecords().length===0);",
        "intersection: takeRecords empties the queue");
    os64_js_outcome_t out;
    check(os64_dom_intersections_fire(f.binding, &out) && out.status == OS64_JS_OK,
          "intersection: the task runs with nothing left");
    io_script(&f,
        "assert(L.length===0,'taken entries are not delivered');"
        "t.unobserve(document.getElementById('o2'));t.unobserve(document.getElementById('o1'));",
        "intersection: unobserve");
    io_place("o2", 0, 900, 10, 910);
    check(!os64_dom_intersections_update(f.binding), "intersection: an unobserved target reports nothing");

    /* One task for every observer, in the order they were made; a callback
     * that throws is reported, and the next still runs. */
    io_place("one", 0, 0, 10, 10);
    io_script(&f,
        "io.disconnect();L=[];globalThis.errors=[];window.onerror=m=>{errors.push(m);return true};"
        "globalThis.first=new IntersectionObserver(()=>{L.push('first');throw Error('observer failed')});"
        "globalThis.second=new IntersectionObserver(()=>{L.push('second');Promise.resolve().then(()=>L.push('micro'))});"
        "globalThis.third=new IntersectionObserver(()=>L.push('third'));"
        "third.observe(one);second.observe(one);first.observe(one);",
        "intersection: order");
    check(os64_dom_intersections_update(f.binding) && os64_dom_intersections_fire(f.binding, &out) &&
          !os64_dom_intersections_due(f.binding), "intersection: three observers, one task");
    io_script(&f,
        "assert(L.join()==='first,second,micro,third','made order, a checkpoint after each: '+L);"
        "assert(errors.length===1&&/observer failed/.test(errors[0]),String(errors));"
        "first.disconnect();second.disconnect();third.disconnect();",
        "intersection: ordered");

    /* A refused batch observes nothing and stays owed. */
    io_script(&f, "L=[];globalThis.w=new IntersectionObserver(es=>L.push(...es));w.observe(one);",
               "intersection: refused batch");
    io_refuse = true;
    check(!os64_dom_intersections_update(f.binding) && os64_dom_intersections_owed(f.binding),
          "intersection: a refused batch leaves the first observation owed");
    io_refuse = false;
    check(io_frame(&f, "intersection: answered at last"), "intersection: the next step answers");
    io_script(&f, "assert(L.length===1);w.disconnect();", "intersection: the owed entry");

    /* delay: a target is observed again only once that long has passed. */
    io_script(&f, "L=[];globalThis.dl=new IntersectionObserver(es=>L.push(...es),{delay:100});dl.observe(one);",
               "intersection: delay");
    check(io_frame(&f, "intersection: delay's first"), "intersection: delay's first");
    io_place("one", 0, 900, 10, 910);
    p2b_advance(50);
    check(!os64_dom_intersections_update(f.binding), "intersection: inside the delay, nothing");
    check(os64_dom_intersections_next(f.binding) == p2b_ms + 50 && !os64_dom_intersections_owed(f.binding),
          "intersection: the target passed over is owed when its delay runs out, and not before");
    p2b_advance(60);
    check(os64_dom_intersections_owed(f.binding), "intersection: its delay run, the step is owed though nothing moved");
    check(io_frame(&f, "intersection: past the delay"), "intersection: past the delay");
    io_script(&f, "assert(L.length===2&&!L[1].isIntersecting);dl.disconnect();", "intersection: delayed entry");
    /* A target passed over and then let go owes nothing: no step runs
     * with nothing observed, so its deadline must not wake the host. */
    io_script(&f, "globalThis.d2=new IntersectionObserver(()=>{},{delay:100});d2.observe(one);",
              "intersection: delayed and dropped");
    check(io_frame(&f, "intersection: its first"), "intersection: delayed and dropped, first");
    io_place("one", 0, 0, 10, 10);
    p2b_advance(10);
    (void)os64_dom_intersections_update(f.binding);
    check(os64_dom_intersections_next(f.binding) != UINT64_MAX, "intersection: passed over, a deadline");
    io_script(&f, "d2.disconnect();", "intersection: dropped");
    check(os64_dom_intersections_next(f.binding) == UINT64_MAX && !os64_dom_intersections_owed(f.binding),
          "intersection: with nothing observed, no deadline and nothing owed");

    /* A long list is sorted exactly, and in less than quadratic time. */
    io_script(&f,
        "let list=[];for(let i=0;i<20000;i++)list.push(((i*7919)%20000)/20000);"
        "let t=new IntersectionObserver(()=>{},{threshold:list}).thresholds;"
        "assert(t.length===20000);for(let i=1;i<t.length;i++)if(t[i-1]>t[i])throw Error('unsorted at '+i);",
        "intersection: thresholds sorted");

    /* Observers a page makes and drops are freed when it makes the next,
     * not kept to the page's end. */
    io_script(&f, "for(let i=0;i<200;i++)new IntersectionObserver(()=>{},{threshold:[0.1,0.2,0.3]});",
              "intersection: two hundred dropped");
    os64_js_outcome_t gc;
    JS_RunGC(JS_GetRuntime(os64_js_context(f.runtime, OS64_JS_ABI_ID, &gc)));
    size_t before = os64_dom_bytes(f.binding);
    io_script(&f, "new IntersectionObserver(()=>{});", "intersection: one more");
    check(os64_dom_bytes(f.binding) < before, "intersection: the next observer sweeps the dropped ones");
    fixture_free(&f);
}

/* An observer is alive while it observes something, though nothing else
 * refers to it; one that observes nothing is collected, and the binding
 * holds nothing for it. */
static void dom_intersection_lifetime_cases(void)
{
    DomFixture f = io_fixture();
    if (!fixture_ready(&f)) return;
    /* The wrappers the cases use exist first: the count measured is the
     * observers' alone. */
    io_script(&f, "globalThis.L=[];document.getElementById('one');document.getElementById('two');",
              "intersection: the wrappers");
    size_t base = os64_dom_registry_count(f.binding);
    io_place("one", 0, 0, 10, 10);
    io_script(&f,
        "globalThis.L=[];(function(){new IntersectionObserver(es=>L.push(es[0].target.id))"
        ".observe(document.getElementById('one'))})();",
        "intersection: an observer nothing refers to");
    check(os64_dom_registry_count(f.binding) == base + 1, "intersection: observing, it is held");
    os64_js_outcome_t out;
    JSContext *ctx = os64_js_context(f.runtime, OS64_JS_ABI_ID, &out);
    JS_RunGC(JS_GetRuntime(ctx));
    check(io_frame(&f, "intersection: the unreferenced observer reports"), "intersection: it reports");
    io_script(&f, "assert(L.join()==='one',L.join());", "intersection: its callback ran");
    /* Its callback saw the observer; unobserving through it lets it go. */
    io_script(&f,
        "L=[];globalThis.keep=new IntersectionObserver(()=>{});keep.observe(document.getElementById('two'));"
        "keep.disconnect();keep=null;",
        "intersection: disconnected and dropped");
    JS_RunGC(JS_GetRuntime(ctx));
    check(os64_dom_registry_count(f.binding) == base + 1, "intersection: only the observing one is held");
    io_place("two", 0, 0, 10, 10);
    (void)os64_dom_intersections_update(f.binding);
    /* A removed target stays observed, and reports it has no box. */
    io_fake("one")->boxed = false;
    io_script(&f, "document.getElementById('one').remove();", "intersection: the target leaves");
    check(io_frame(&f, "intersection: a removed target reports"), "intersection: removed report");
    io_script(&f, "assert(L.join()==='one',L.join());", "intersection: removed, reported once");
    check(!os64_dom_intersections_update(f.binding), "intersection: and only once");
    /* Left observing with entries queued: the teardown accounts for all. */
    io_fake("one")->boxed = true;
    check(os64_dom_intersections_update(f.binding), "intersection: entries left at teardown");
    fixture_free(&f);
}

/* A batch the DOM budget refuses is said lost once. Its targets stay owed,
 * so the host steps again whenever it runs, and a step refused again before
 * one is had queues nothing: otherwise a page at its budget is stepped and
 * told without end. The first step that has a batch observes, and a
 * refusal after that is said again. */
static void dom_intersection_starved_cases(void)
{
    DomFixture f = io_fixture();
    if (!fixture_ready(&f)) return;
    io_place("one", 0, 0, 10, 10);
    io_place("two", 0, 0, 10, 10);
    io_place("box", 0, 0, 10, 10);
    io_script(&f, "globalThis.L=[];globalThis.errors=[];window.onerror=m=>{errors.push(String(m));return true};"
        "for(const id of ['one','two','box'])new IntersectionObserver(es=>L.push(...es))"
        ".observe(document.getElementById(id));", "starved: setup");
    os64_js_outcome_t out;
    /* The step's first allocation is its batch. */
    size_t target = dom_attempts + 1;
    dom_fail_at = target;
    check(os64_dom_intersections_update(f.binding), "starved: the first refusal queues its report");
    dom_fail_at = 0;
    check(dom_attempts >= target, "starved: the batch was refused");
    (void)os64_dom_intersections_fire(f.binding, &out);
    io_script(&f, "assert(errors.length===1&&/3 intersection observations were lost/.test(errors[0])&&L.length===0,"
        "errors.join())", "starved: the loss is said");
    for (int again = 0; again < 3; again++) {
        dom_fail_at = dom_attempts + 1;
        check(!os64_dom_intersections_update(f.binding) && !os64_dom_intersections_due(f.binding),
              "starved: a refusal again queues nothing");
        dom_fail_at = 0;
    }
    check(os64_dom_intersections_owed(f.binding), "starved: its targets are still owed");
    check(os64_dom_intersections_update(f.binding), "starved: a step with a batch observes");
    (void)os64_dom_intersections_fire(f.binding, &out);
    io_script(&f, "assert(L.length===3&&errors.length===1,L.length+' '+errors.join())", "starved: observed, said once");
    /* A fourth target outgrows the batch: refused after one was had, said again. */
    io_script(&f, "new IntersectionObserver(es=>L.push(...es)).observe(document.getElementById('one'));",
              "starved: a fourth target");
    dom_fail_at = dom_attempts + 1;
    check(os64_dom_intersections_update(f.binding), "starved: a refusal after a batch is said again");
    dom_fail_at = 0;
    (void)os64_dom_intersections_fire(f.binding, &out);
    io_script(&f, "assert(errors.length===2,errors.join())", "starved: the second loss is said");
    fixture_free(&f);
}

/* Every allocation the step and the task make, failed in turn: an entry
 * that could not be kept is reported once as the quota, the next step
 * goes on, and the teardown accounts for every byte. Then the interface's
 * operations through binding_sweep. */
static void dom_intersection_sweep_cases(void)
{
    const char *setup =
        "globalThis.L=[];globalThis.errors=[];window.onerror=m=>{errors.push(m);return true};"
        "for(const id of ['one','two','box'])new IntersectionObserver(es=>L.push(...es),{threshold:[0,0.5]})"
        ".observe(document.getElementById(id));";
    size_t count = 0;
    for (size_t cut = 0; cut == 0 || cut <= count; cut++) {
        DomFixture f = io_fixture();
        if (!fixture_ready(&f)) return;
        io_place("one", 0, 0, 10, 10);
        io_place("two", 0, 0, 10, 10);
        io_place("box", 0, 0, 10, 10);
        io_script(&f, setup, "intersection sweep: setup");
        size_t start = dom_attempts;
        if (cut != 0) dom_fail_at = start + cut;
        bool due = os64_dom_intersections_update(f.binding);
        os64_js_outcome_t out;
        if (due) (void)os64_dom_intersections_fire(f.binding, &out);
        dom_fail_at = 0;
        if (cut == 0) {
            count = dom_attempts - start;
            printf("intersection step and task allocation points: %zu\n", count);
        } else {
            dom_sweeps++;
        }
        /* Whatever was lost, the page goes on: a step after it observes. */
        io_place("one", 0, 900, 10, 910);
        if (os64_dom_intersections_update(f.binding)) (void)os64_dom_intersections_fire(f.binding, &out);
        io_script(&f, "assert(L.length>=1||errors.length>=1,'something reached the page')",
                   "intersection sweep: the page goes on");
        fixture_free(&f);
    }
    static const char *const kOperations[] = {
        "new IntersectionObserver(()=>{},{threshold:[0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9],rootMargin:'1px 2%'})",
        "let o=new IntersectionObserver(()=>{});o.observe(document.getElementById('one'));"
        "o.observe(document.getElementById('two'));o.unobserve(document.getElementById('one'));o.disconnect()",
        "new DOMRect(1,2,3,4).toJSON();DOMRectReadOnly.fromRect({x:1})",
        "new IntersectionObserverEntry({time:1,rootBounds:{},boundingClientRect:{},intersectionRect:{},"
        "isIntersecting:true,isVisible:false,intersectionRatio:1,target:document.body})",
    };
    os64_dom_options_t options = p2b_options(P2B_URL);
    binding_sweep("intersection", kOperations, sizeof(kOperations) / sizeof(kOperations[0]), &options);
}

static void dom_intersection_cases(void)
{
    dom_rect_cases();
    dom_intersection_interface_cases();
    dom_intersection_step_cases();
    dom_intersection_lifetime_cases();
    dom_intersection_sweep_cases();
    dom_intersection_starved_cases();
}
#undef IO_ASSERT
