#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <svg/svg.h>

#define PI 3.14159265358979323846
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            abort();                                                                               \
        }                                                                                          \
        ++checks;                                                                                  \
    } while (0)
static size_t checks;
static const os64_geometry_map_t identity = {1, 0, 0, 1, 0, 0};
typedef struct {
    size_t live, peak, allocations, fail, polls, cancel;
    bool admission, probe;
    svg_context_t *context;
} Ledger;
typedef union {
    max_align_t align;
    struct {
        size_t bytes;
        Ledger *owner;
    } info;
} Header;
static os64_geometry_status_t allocate(void *user, size_t bytes, void **out)
{
    Ledger *l = user;
    *out = NULL;
    ++l->allocations;
    if (l->fail == l->allocations)
        return l->admission ? OS64_GEOMETRY_LIMIT : OS64_GEOMETRY_NO_MEMORY;
    Header *h = malloc(sizeof(*h) + bytes);
    CHECK(h);
    h->info.bytes = bytes;
    h->info.owner = l;
    l->live += bytes;
    if (l->peak < l->live)
        l->peak = l->live;
    *out = h + 1;
    return OS64_GEOMETRY_OK;
}
static void release(void *user, void *data, size_t bytes)
{
    Ledger *l = user;
    Header *h = (Header *)data - 1;
    CHECK(h->info.owner == l && h->info.bytes == bytes && l->live >= bytes);
    l->live -= bytes;
    free(h);
}
static bool cancelled(void *user)
{
    Ledger *l = user;
    if (l->probe && l->context) {
        svg_stats_t stats;
        CHECK(svg_stats(l->context, &stats) == SVG_BUSY);
        CHECK(svg_destroy(l->context) == SVG_BUSY);
    }
    return ++l->polls == l->cancel;
}
static svg_options_t options(Ledger *l)
{
    return (svg_options_t){.memory = {l, allocate, release},
                           .cancel_context = l,
                           .cancelled = cancelled,
                           .limits = svg_default_limits()};
}
static svg_context_t *context(Ledger *l)
{
    svg_context_t *c = NULL;
    svg_options_t o = options(l);
    CHECK(svg_create(&o, &c) == SVG_OK && c);
    l->context = c;
    return c;
}
static svg_path_t *parse(svg_context_t *c, const char *text, svg_status_t expected)
{
    svg_path_t *p = NULL;
    svg_parse_result_t r;
    CHECK(svg_path_parse(c, (svg_string_t){text, strlen(text)}, &p, &r) == expected);
    CHECK(r.status == expected);
    CHECK((p != NULL) == (expected == SVG_OK || expected == SVG_MALFORMED));
    if (expected == SVG_OK)
        CHECK(r.offset == strlen(text));
    return p;
}
static void near(double actual, double expected)
{
    CHECK(fabs(actual - expected) <= 1e-12 * fmax(1, fabs(expected)));
}
static void grammar(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    const char *cases[] = {"",
                           " \r\n\t\f ",
                           "M0 0",
                           "m1 2 3 4",
                           "M.6.5L1-2",
                           "M1e1-2E+1l.5.25",
                           "M0,0H3 5h2V9v-1ZzL2 3",
                           "M0 0C1 2 3 4 5 6S7 8 9 10",
                           "M0 0q1 2 3 4t5 6",
                           "M0 0A10 20 30 0110 20a-10-20-30 10-10-20",
                           "M1. 2.L3. 4.",
                           "M-0e999999999 0"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        svg_path_t *p = parse(c, cases[i], SVG_OK);
        CHECK(svg_path_release(c, p) == SVG_OK);
    }
    svg_path_t *p = parse(c, "m1 2 3 4h2v3q1 2 3 4t5 6c1 2 3 4 5 6s7 8 9 10z l2 3", SVG_OK);
    size_t count;
    const svg_command_t *cmd = svg_path_commands(p, &count);
    CHECK(count == 10);
    near(cmd[1].p[0].x, 4);
    near(cmd[1].p[0].y, 6);
    near(cmd[3].p[0].x, 6);
    near(cmd[3].p[0].y, 9);
    near(cmd[4].p[0].x, 7);
    near(cmd[4].p[1].y, 13);
    near(cmd[5].p[0].x, 11);
    near(cmd[5].p[0].y, 15);
    near(cmd[5].p[1].x, 14);
    near(cmd[5].p[1].y, 19);
    near(cmd[7].p[0].x, 21);
    near(cmd[7].p[0].y, 27);
    near(cmd[9].p[0].x, 3);
    near(cmd[9].p[0].y, 5);
    svg_geometry_t *g = NULL;
    CHECK(svg_path_lower(c, p, &identity, 1.0 / 64, &g) == SVG_OK);
    const os64_geometry_command_t *gc = svg_geometry_commands(g, &count);
    CHECK(count == 11 && gc[9].verb == OS64_GEOMETRY_MOVE);
    near(gc[9].p[0].x, 1);
    near(gc[9].p[0].y, 2);
    CHECK(svg_destroy(c) == SVG_BUSY);
    CHECK(svg_geometry_release(c, g) == SVG_OK);
    CHECK(svg_path_release(c, p) == SVG_OK);
    p = parse(c, "M0 0C1 1 2 2 3 3L4 4S5 5 6 6Q7 7 8 8A1 1 0 0 1 8 8T9 9", SVG_OK);
    cmd = svg_path_commands(p, &count);
    near(cmd[3].p[0].x, 4);
    near(cmd[6].p[0].x, 8);
    CHECK(svg_path_release(c, p) == SVG_OK);
    const struct {
        const char *text;
        size_t prefix;
    } bad[] = {{"L1 2", 0},
               {"M", 0},
               {"M,0 0", 0},
               {"M0 0,", 1},
               {"M0 0,,1 2", 1},
               {"M0 0L1 2 3", 2},
               {"M0 0C1 2 3 4 5", 1},
               {"M0 0A1 1 0 2 0 2 2", 1},
               {"M0 0A1 1 0 0 +1 2 2", 1},
               {"M0 0L1e 2", 1},
               {"M0 0X1 2", 1},
               {"M0 0z1 2", 2},
               {"M0 0LNaN 3", 1},
               {"M0 0Linf 3", 1},
               {"M0 0A1 1 0.0110 20", 1},
               {"M0 0\vL1 2", 1}};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); ++i) {
        p = parse(c, bad[i].text, SVG_MALFORMED);
        svg_path_commands(p, &count);
        CHECK(count == bad[i].prefix);
        CHECK(svg_path_release(c, p) == SVG_OK);
    }
    svg_parse_result_t result;
    const char embedded[] = {'M', '0', ' ', '0', 0, 'L', '1', ' ', '1'};
    CHECK(svg_path_parse(c, (svg_string_t){embedded, sizeof(embedded)}, &p, &result) ==
          SVG_MALFORMED);
    CHECK(result.offset == 4);
    CHECK(svg_path_release(c, p) == SVG_OK);
    parse(c, "M1e99999 0", SVG_LIMIT);
    parse(c, "M1e-99999 0", SVG_LIMIT);
    parse(c, "M1e308 0l1e308 0", SVG_LIMIT);
    char *long_number = malloc(1000040);
    CHECK(long_number);
    memcpy(long_number, "M1", 2);
    memset(long_number + 2, '0', 1000000);
    strcpy(long_number + 1000002, "e-100000000000 0");
    parse(c, long_number, SVG_LIMIT);
    memcpy(long_number, "M0.", 3);
    memset(long_number + 3, '0', 1000000);
    strcpy(long_number + 1000003, "1e100000000000 0");
    parse(c, long_number, SVG_LIMIT);
    free(long_number);
    p = parse(c, "M1e-320 1.234567890123456789e12", SVG_OK);
    cmd = svg_path_commands(p, &count);
    CHECK(cmd[0].p[0].x > 0);
    near(cmd[0].p[0].y, 1234567890123.456789);
    CHECK(svg_path_release(c, p) == SVG_OK);
    l.probe = true;
    p = parse(c, "M1 2L3 4", SVG_OK);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && l.live == 0);
}
static os64_geometry_point_t ellipse(double cx, double cy, double rx, double ry, double rotation,
                                     double angle)
{
    double cp = cos(rotation), sp = sin(rotation), ct = cos(angle), st = sin(angle);
    return (os64_geometry_point_t){cx + rx * ct * cp - ry * st * sp,
                                   cy + rx * ct * sp + ry * st * cp};
}
static os64_geometry_point_t cubic(os64_geometry_point_t start, const os64_geometry_command_t *c,
                                   double t)
{
    double u = 1 - t;
    return (os64_geometry_point_t){start.x * u * u * u + 3 * c->p[0].x * u * u * t +
                                       3 * c->p[1].x * u * t * t + c->p[2].x * t * t * t,
                                   start.y * u * u * u + 3 * c->p[0].y * u * u * t +
                                       3 * c->p[1].y * u * t * t + c->p[2].y * t * t * t};
}
static void arc_accuracy(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    const os64_geometry_map_t maps[] = {identity,
                                        {4, 0, 0, 1, 0, 0},
                                        {-2, .6, 1.3, 3, 4, 5},
                                        {16384, 0, 0, 16384, 0, 0},
                                        {0, 0, 0, 0, 0, 0}};
    double worst = 0;
    for (size_t m = 0; m < sizeof(maps) / sizeof(*maps); ++m) {
        for (int k = 0; k < 12; ++k) {
            double angle = .31, sweep = (k % 2 ? -1 : 1) * (.2 + (k / 2) * 1.1);
            double cx = 100, cy = 90, rx = 35, ry = k % 3 == 0 ? .3 : 19, rotation = .74;
            os64_geometry_point_t from = ellipse(cx, cy, rx, ry, rotation, angle);
            os64_geometry_point_t to = ellipse(cx, cy, rx, ry, rotation, angle + sweep);
            char text[256];
            snprintf(text, sizeof(text), "M%.17g %.17gA%.17g %.17g %.17g %d %d %.17g %.17g", from.x,
                     from.y, rx, ry, rotation * 180 / PI, fabs(sweep) > PI, sweep > 0, to.x, to.y);
            svg_path_t *p = parse(c, text, SVG_OK);
            svg_geometry_t *g = NULL;
            double error = 1.0 / 64;
            CHECK(svg_path_lower(c, p, &maps[m], error, &g) == SVG_OK);
            size_t count;
            const os64_geometry_command_t *cmd = svg_geometry_commands(g, &count);
            CHECK(count > 1 && cmd[0].verb == OS64_GEOMETRY_MOVE);
            os64_geometry_point_t start = from;
            for (size_t j = 1; j < count; ++j) {
                CHECK(cmd[j].verb == OS64_GEOMETRY_CUBIC);
                for (int sample = 0; sample <= 32; ++sample) {
                    double t = sample / 32.0;
                    os64_geometry_point_t a = cubic(start, &cmd[j], t);
                    os64_geometry_point_t b = ellipse(cx, cy, rx, ry, rotation,
                                                      angle + sweep * ((j - 1) + t) / (count - 1));
                    double dx = a.x - b.x, dy = a.y - b.y;
                    double observed = fmax(fabs(maps[m].a * dx + maps[m].c * dy),
                                           fabs(maps[m].b * dx + maps[m].d * dy));
                    if (observed > worst)
                        worst = observed;
                    CHECK(observed <= error);
                }
                start = cmd[j].p[2];
            }
            near(start.x, to.x);
            near(start.y, to.y);
            CHECK(svg_geometry_release(c, g) == SVG_OK);
            CHECK(svg_path_release(c, p) == SVG_OK);
        }
    }
    svg_path_t *p = parse(c, "M0 0A0 1 0 0 1 3 4A1 1 0 0 1 3 4zL5 6", SVG_OK);
    svg_geometry_t *g = NULL;
    CHECK(svg_path_lower(c, p, &identity, .01, &g) == SVG_OK);
    size_t count;
    const os64_geometry_command_t *cmd = svg_geometry_commands(g, &count);
    CHECK(count == 5 && cmd[1].verb == OS64_GEOMETRY_LINE && cmd[3].verb == OS64_GEOMETRY_MOVE);
    CHECK(svg_geometry_release(c, g) == SVG_OK);
    CHECK(svg_path_release(c, p) == SVG_OK);
    p = parse(c, "M0 0A1 1 0 0 1 10 0", SVG_OK);
    CHECK(svg_path_lower(c, p, &identity, .01, &g) == SVG_OK);
    cmd = svg_geometry_commands(g, &count);
    /* Corrected radii form a radius-five lower semicircle. */
    near(cmd[(count - 1) / 2].p[2].x, 5);
    near(cmd[(count - 1) / 2].p[2].y, -5);
    CHECK(svg_geometry_release(c, g) == SVG_OK);
    CHECK(svg_path_release(c, p) == SVG_OK);
    p = parse(c, "M1e20 0A1e20 1e20 0 0 1 0 1e20", SVG_OK);
    CHECK(svg_path_lower(c, p, &identity, .01, &g) == SVG_LIMIT && !g);
    CHECK(svg_path_release(c, p) == SVG_OK);
    p = parse(c, "M20 0A20 20 0 0 1 -20 0", SVG_OK);
    os64_geometry_map_t extreme = {16384, 0, 0, 16384, 0, 0};
    /* The near-tangent center conversion cannot certify this tight budget at
     * this zoom. Refusal is explicit; it does not publish coarse geometry. */
    CHECK(svg_path_lower(c, p, &extreme, .01, &g) == SVG_LIMIT && !g);
    CHECK(svg_path_lower(c, p, &identity, .01, &g) == SVG_OK);
    CHECK(svg_geometry_release(c, g) == SVG_OK);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    printf("Arc positional samples: maximum device-axis error %.9g (limit 1/64)\n", worst);
}

typedef struct {
    Ledger *ledger;
    svg_node_t nodes[10];
    size_t acquired, released;
    size_t chain;
    bool suppress, hidden, unsupported, cyclic, pending, malformed, shaped;
    uint32_t color;
} Provider;
static svg_status_t acquire(void *user, svg_mode_t mode, void **snapshot, uint64_t *root)
{
    Provider *p = user;
    CHECK(mode == SVG_MODE_INLINE || mode == SVG_MODE_IMAGE);
    ++p->acquired;
    *snapshot = p;
    *root = 1;
    return SVG_OK;
}
static void unpin(void *user, void *snapshot)
{
    Provider *p = user;
    CHECK(snapshot == p);
    ++p->released;
    if (p->ledger->context)
        CHECK(svg_destroy(p->ledger->context) == SVG_BUSY);
}
static svg_status_t node(void *user, void *snapshot, uint64_t source, svg_node_t *out)
{
    Provider *p = user;
    CHECK(snapshot == p && source > 0);
    if (p->chain) {
        CHECK(source <= p->chain);
        *out = (svg_node_t){source == p->chain ? (p->shaped ? SVG_NODE_SHAPE : SVG_NODE_PATH) : SVG_NODE_GROUP,
                            source == p->chain ? 0 : source + 1, 0};
        return SVG_OK;
    }
    CHECK(source < 10);
    *out = p->nodes[source];
    if (source == 2 && p->unsupported)
        out->kind = SVG_NODE_UNSUPPORTED;
    if (source == 2 && p->cyclic)
        out->first_child = 1;
    return SVG_OK;
}
static svg_status_t style(void *user, void *snapshot, const svg_style_request_t *r, svg_style_t *s,
                          svg_string_t *d)
{
    Provider *p = user;
    CHECK(snapshot == p);
    if (p->pending && !p->shaped && r->source == 3)
        return SVG_PENDING;
    *s = svg_initial_style();
    if (r->parent_style)
        s->fill = r->parent_style->fill;
    if (r->source == 1) {
        CHECK(r->parent_instance == 0 && !r->parent_style);
        s->opacity = .5;
        s->visible = !p->hidden;
        s->transform = (os64_geometry_map_t){2, 0, 0, 3, 5, 6};
    }
    if (r->source == 2) {
        s->fill.argb = p->color;
        s->display = !p->suppress;
        s->transform.tx = 7;
    }
    if (r->source == 4)
        s->fill.argb = 0xff0000ff;
    if (r->source == 3 || (p->chain && r->source == p->chain)) {
        CHECK(r->parent_instance != 0);
        const char *text = p->malformed ? "M0 0L10 0 3" : "M0 0L10 0L10 10Z";
        *d = (svg_string_t){text, strlen(text)};
    }
    return SVG_OK;
}
static Provider provider(Ledger *l)
{
    return (Provider){.ledger = l,
                      .color = 0xffff0000,
                      .nodes = {{0},
                                {SVG_NODE_GROUP, 2, 999},
                                {SVG_NODE_GROUP, 3, 4},
                                {SVG_NODE_PATH, 0, 0},
                                {SVG_NODE_GROUP, 3, 0}}};
}
static svg_status_t provide_shape(void *user, void *snapshot, const svg_style_request_t *r,
                                   svg_shape_t *out)
{
    Provider *p = user;
    CHECK(snapshot == p && r->parent_style && r->parent_instance);
    CHECK(r->parent_style->fill.argb == 0xff0000ff);
    if (p->pending) return SVG_PENDING;
    *out = (svg_shape_t){.kind=SVG_SHAPE_RECT,.width=20,.height=10};
    return SVG_OK;
}
static svg_provider_t callbacks(Provider *p)
{
    return (svg_provider_t){p, acquire, unpin, node, style, NULL, NULL, NULL};
}
static void scenes(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    Provider p = provider(&l);
    svg_provider_t cb = callbacks(&p);
    svg_scene_t *scene = NULL;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_OK);
    CHECK(p.acquired == 1 && p.released == 0);
    size_t count;
    const svg_scene_item_t *items = svg_scene_items(scene, &count);
    CHECK(count == 5 && items[0].parent == SIZE_MAX);
    CHECK(items[2].source == 3 && items[4].source == 3 && items[2].instance != items[4].instance);
    CHECK(items[2].style.fill.argb == 0xffff0000 && items[4].style.fill.argb == 0xff0000ff);
    CHECK(items[0].style.opacity == .5 && items[2].style.opacity == 1);
    near(items[2].user_to_viewport.tx, 19);
    near(items[2].user_to_viewport.d, 3);
    CHECK(svg_path_release(c, (svg_path_t *)items[2].path) == SVG_BUSY);
    p.color = 0xff00ff00;
    p.hidden = p.malformed = true;
    svg_scene_t *fresh = NULL;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_IMAGE, &identity, &fresh) == SVG_OK);
    CHECK(items[2].style.fill.argb == 0xffff0000);
    items = svg_scene_items(fresh, &count);
    CHECK(!items[0].style.visible && items[2].style.visible);
    CHECK(items[2].style.fill.argb == 0xff00ff00 && items[2].path_result.status == SVG_MALFORMED);
    CHECK(svg_scene_release(c, fresh) == SVG_OK);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    p.suppress = p.unsupported = true;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_OK);
    svg_scene_items(scene, &count);
    CHECK(count == 3);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    p.suppress = false;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_UNSUPPORTED &&
          !scene);
    p.unsupported = false;
    p.cyclic = true;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_PROVIDER_ERROR &&
          !scene);
    p.cyclic = false;
    p.pending = true;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_PENDING && !scene);
    p.pending = false;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_OK);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    // Exercise a shape callback after growing the scene array, with a live
    // borrowed parent style. The provider also gets the image processing mode.
    p.chain = 33; p.shaped = true;
    CHECK(svg_scene_prepare(c,&cb,SVG_MODE_IMAGE,&identity,&scene)==SVG_PROVIDER_ERROR && !scene);
    cb.shape = provide_shape;
    CHECK(svg_scene_prepare(c,&cb,SVG_MODE_IMAGE,&identity,&scene)==SVG_OK);
    items=svg_scene_items(scene,&count);
    CHECK(count==33 && items[32].kind==SVG_NODE_SHAPE && items[32].path);
    CHECK(svg_scene_release(c,scene)==SVG_OK);
    p.pending = true;
    CHECK(svg_scene_prepare(c,&cb,SVG_MODE_IMAGE,&identity,&scene)==SVG_PENDING && !scene);
    CHECK(p.acquired == p.released);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static svg_status_t use_reference(void *user, void *snapshot, const svg_style_request_t *r,
                                  uint64_t *source)
{
    Provider *p = user;
    CHECK(snapshot == p && r->parent_style && r->parent_instance);
    CHECK(r->mode == SVG_MODE_INLINE || r->mode == SVG_MODE_IMAGE);
    if (p->pending)
        return SVG_PENDING;
    *source = p->hidden ? 1 : 3;
    return SVG_OK;
}
static void instances(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    Provider p = provider(&l);
    p.nodes[2] = (svg_node_t){SVG_NODE_USE, 0, 4};
    p.nodes[4] = (svg_node_t){SVG_NODE_USE, 0, 0};
    p.nodes[3].next_sibling = 999; // A reference must not visit source siblings.
    svg_provider_t cb = callbacks(&p);
    svg_scene_t *scene = NULL;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_PROVIDER_ERROR);
    cb.reference = use_reference;
    size_t calls = l.allocations, polls = l.polls, count;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_IMAGE, &identity, &scene) == SVG_OK);
    calls = l.allocations - calls;
    polls = l.polls - polls;
    const svg_scene_item_t *items = svg_scene_items(scene, &count);
    CHECK(count == 5 && items[1].kind == SVG_NODE_USE && items[3].kind == SVG_NODE_USE);
    CHECK(items[2].source == 3 && items[4].source == 3 && items[2].instance != items[4].instance);
    CHECK(items[2].style.fill.argb == 0xffff0000 && items[4].style.fill.argb == 0xff0000ff);
    near(items[2].user_to_viewport.tx, 19);
    near(items[4].user_to_viewport.tx, 5);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    for (size_t i = 1; i <= calls; ++i) {
        l.fail = l.allocations + i;
        CHECK(svg_scene_prepare(c, &cb, SVG_MODE_IMAGE, &identity, &scene) == SVG_NO_MEMORY && !scene);
        l.fail = 0;
    }
    for (size_t i = 1; i <= polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_scene_prepare(c, &cb, SVG_MODE_IMAGE, &identity, &scene) == SVG_CANCELLED && !scene);
        l.cancel = 0;
    }
    p.pending = true;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_PENDING && !scene);
    p.pending = false;
    p.hidden = true;
    // A reference to an active ancestor is empty; ordinary child cycles remain errors.
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_OK);
    svg_scene_items(scene, &count);
    CHECK(count == 3);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    CHECK(p.acquired == p.released);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static svg_status_t cycle_reference(void *user, void *snapshot, const svg_style_request_t *r,
                                    uint64_t *source)
{
    CHECK(user == snapshot);
    CHECK(r->source == 2 || r->source == 4 || r->source == 5);
    *source = r->source + 1;
    return SVG_OK;
}
static svg_status_t cycle_style(void *user, void *snapshot, const svg_style_request_t *r,
                                svg_style_t *s, svg_string_t *d)
{
    svg_status_t status = style(user, snapshot, r, s, d);
    if (r->source >= 7)
        *d = (svg_string_t){"M0 0H8V8Z", 9};
    return status;
}
static void descendant_cycles(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    Provider p = provider(&l);
    p.nodes[1] = (svg_node_t){SVG_NODE_GROUP, 2, 0};
    p.nodes[2] = (svg_node_t){SVG_NODE_USE, 0, 9};
    p.nodes[3] = (svg_node_t){SVG_NODE_GROUP, 4, 8};
    p.nodes[4] = (svg_node_t){SVG_NODE_USE, 0, 0};
    p.nodes[5] = (svg_node_t){SVG_NODE_USE, 0, 0};
    p.nodes[6] = (svg_node_t){SVG_NODE_GROUP, 7, 0};
    p.nodes[7] = (svg_node_t){SVG_NODE_PATH, 0, 3};
    p.nodes[8] = (svg_node_t){SVG_NODE_PATH, 0, 0};
    p.nodes[9] = (svg_node_t){SVG_NODE_PATH, 0, 0};
    svg_provider_t cb = callbacks(&p);
    cb.style = cycle_style;
    cb.reference = cycle_reference;
    svg_scene_t *scene = NULL;
    size_t before = l.allocations, polls = l.polls, count;
    // 3 -> 4 -> 5 -> 6 -> 3 closes on an ordinary child edge. Keep the
    // geometry before/after that edge and the unrelated outer sibling.
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_OK);
    size_t calls = l.allocations - before;
    polls = l.polls - polls;
    const svg_scene_item_t *items = svg_scene_items(scene, &count);
    CHECK(count == 9);
    for (size_t i = 0; i < count; ++i)
        CHECK(items[i].source == i + 1);
    CHECK(items[6].parent == 5 && items[7].parent == 5 && items[8].parent == 0);
    CHECK(items[6].path && items[7].path && items[8].path);
    CHECK(svg_scene_release(c, scene) == SVG_OK);
    for (size_t i = 1; i <= calls; ++i) {
        l.fail = l.allocations + i;
        CHECK(svg_scene_prepare(c, &cb, SVG_MODE_IMAGE, &identity, &scene) == SVG_NO_MEMORY && !scene);
        l.fail = 0;
    }
    for (size_t i = 1; i <= polls; ++i) {
        l.cancel = l.polls + i;
        CHECK(svg_scene_prepare(c, &cb, SVG_MODE_IMAGE, &identity, &scene) == SVG_CANCELLED && !scene);
        l.cancel = 0;
    }
    // A reference above a purely ordinary child loop must not conceal a
    // malformed provider graph: the loop itself contains no reference edge.
    p.nodes[7] = (svg_node_t){SVG_NODE_GROUP, 7, 0};
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_PROVIDER_ERROR && !scene);
    CHECK(p.acquired == p.released);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void boundaries(void)
{
    Ledger l = {0};
    svg_options_t o = options(&l);
    o.limits.input_bytes = 3;
    svg_context_t *c = NULL;
    CHECK(svg_create(&o, &c) == SVG_OK);
    parse(c, "M0 0", SVG_LIMIT);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    o = options(&l);
    o.limits.work = 1024;
    CHECK(svg_create(&o, &c) == SVG_OK);
    char spaces[1101];
    memset(spaces, ' ', sizeof(spaces) - 1);
    spaces[1100] = 0;
    parse(c, spaces, SVG_LIMIT);
    svg_stats_t stats;
    CHECK(svg_stats(c, &stats) == SVG_OK && stats.last_work <= 1024 && !stats.paths);
    svg_path_t *p = parse(c, "M0 0", SVG_OK);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    o = options(&l);
    o.limits.work = UINT64_MAX;
    CHECK(svg_create(&o, &c) == SVG_OK);
    char *text = malloc(4 * 4097 + 1);
    CHECK(text);
    memcpy(text, "M0 0", 4);
    for (size_t i = 1; i < 4097; ++i)
        memcpy(text + 4 * i, "L1 1", 4);
    text[4 * 4097] = 0;
    p = parse(c, text, SVG_OK);
    free(text);
    size_t count;
    svg_path_commands(p, &count);
    CHECK(count == 4097);
    svg_geometry_t *g = NULL;
    CHECK(svg_path_lower(c, p, &identity, .01, &g) == SVG_LIMIT && !g);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    o = options(&l);
    o.limits.scene_nodes = 8;
    CHECK(svg_create(&o, &c) == SVG_OK);
    l.context = c;
    Provider fake = provider(&l);
    fake.nodes[2].next_sibling = 2;
    fake.suppress = true;
    svg_provider_t cb = callbacks(&fake);
    svg_scene_t *scene = NULL;
    /* Suppressed siblings still consume source work/node admission. */
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_LIMIT && !scene);
    CHECK(fake.acquired == fake.released);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    c = context(&l);
    Ledger other = {0};
    svg_context_t *foreign = context(&other);
    p = parse(c, "M1 2", SVG_OK);
    CHECK(svg_path_release(foreign, p) == SVG_BAD_ARGUMENT);
    CHECK(svg_path_lower(foreign, p, &identity, .01, &g) == SVG_BAD_ARGUMENT && !g);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(foreign) == SVG_OK && !other.live);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}

static svg_status_t pipeline(Ledger *l)
{
    svg_options_t o = options(l);
    svg_context_t *c = NULL;
    svg_status_t s = svg_create(&o, &c);
    if (s != SVG_OK)
        return s;
    l->context = c;
    svg_path_t *p = NULL;
    svg_geometry_t *g = NULL;
    svg_scene_t *scene = NULL;
    svg_parse_result_t r;
    const char *d = "M0 0A40 10 30 1 1 20 30zL10 10q4 5 6 7t8 9";
    s = svg_path_parse(c, (svg_string_t){d, strlen(d)}, &p, &r);
    if (s == SVG_OK)
        s = svg_path_lower(c, p, &identity, .01, &g);
    Provider fake = provider(l);
    svg_provider_t cb = callbacks(&fake);
    if (s == SVG_OK)
        s = svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene);
    svg_scene_t *deep = NULL;
    fake.chain = 24;
    if (s == SVG_OK)
        s = svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &deep);
    if (deep)
        CHECK(svg_scene_release(c, deep) == SVG_OK);
    if (scene)
        CHECK(svg_scene_release(c, scene) == SVG_OK);
    if (g)
        CHECK(svg_geometry_release(c, g) == SVG_OK);
    if (p)
        CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(fake.acquired == fake.released);
    svg_stats_t stats;
    CHECK(svg_stats(c, &stats) == SVG_OK);
    CHECK(!stats.paths && !stats.geometries && !stats.scenes);
    CHECK(stats.live_bytes == l->live);
    CHECK(svg_destroy(c) == SVG_OK && !l->live);
    l->context = NULL;
    return s;
}
static void failures(void)
{
    Ledger success = {0};
    CHECK(pipeline(&success) == SVG_OK);
    for (size_t n = 1; n <= success.allocations; ++n) {
        Ledger l = {.fail = n};
        CHECK(pipeline(&l) == SVG_NO_MEMORY && !l.live);
        l = (Ledger){.fail = n, .admission = true};
        CHECK(pipeline(&l) == SVG_LIMIT && !l.live);
    }
    for (size_t n = 1; n <= success.polls; ++n) {
        Ledger l = {.cancel = n};
        CHECK(pipeline(&l) == SVG_CANCELLED && !l.live);
    }
    printf("Failure sweeps: %zu allocation cuts in both refusal modes, %zu cancellation cuts; peak "
           "%zu bytes\n",
           success.allocations, success.polls, success.peak);
    Ledger l = {0};
    svg_options_t o = options(&l);
    o.limits.work = 0;
    svg_context_t *c = NULL;
    CHECK(svg_create(&o, &c) == SVG_LIMIT && !c && !l.live);
    o = options(&l);
    o.limits.path_commands = 1;
    CHECK(svg_create(&o, &c) == SVG_OK);
    parse(c, "M0 0L1 1", SVG_LIMIT);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    o = options(&l);
    o.limits.geometry_commands = 2;
    CHECK(svg_create(&o, &c) == SVG_OK);
    svg_path_t *p = parse(c, "M20 0A20 20 0 1 1 0 20", SVG_OK);
    svg_geometry_t *g = NULL;
    CHECK(svg_path_lower(c, p, &identity, .01, &g) == SVG_LIMIT && !g);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
    o = options(&l);
    o.limits.depth = 2;
    CHECK(svg_create(&o, &c) == SVG_OK);
    l.context = c;
    Provider fake = provider(&l);
    svg_provider_t cb = callbacks(&fake);
    svg_scene_t *scene = NULL;
    CHECK(svg_scene_prepare(c, &cb, SVG_MODE_INLINE, &identity, &scene) == SVG_LIMIT && !scene);
    CHECK(fake.acquired == fake.released);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
typedef struct {
    unsigned maximum;
    double total;
} Difference;
static Difference difference(const uint8_t *pixels, const uint8_t *oracle)
{
    Difference d = {0};
    for (size_t i = 0; i < 64 * 64; ++i) {
        unsigned delta = (unsigned)abs((int)pixels[i] - oracle[i]);
        if (delta > d.maximum)
            d.maximum = delta;
        d.total += delta / 255.0;
    }
    return d;
}
static void backend(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    svg_path_t *p = parse(c, "M52 32A20 20 0 1 1 12 32a20 20 0 1 1 40 0Z", SVG_OK);
    const os64_geometry_backend_t *b = os64_freetype_geometry_v1();
    os64_geometry_context_t *engine = NULL;
    os64_geometry_options_t o = {.memory = {&l, allocate, release}, .max_work = 4000000};
    CHECK(b->create(&o, &engine) == OS64_GEOMETRY_OK);
    uint8_t oracle[64 * 64], pixels[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 64; ++sy)
                for (int sx = 0; sx < 64; ++sx) {
                    double dx = x + (sx + .5) / 64 - 32, dy = y + (sy + .5) / 64 - 32;
                    inside += dx * dx + dy * dy < 400;
                }
            oracle[y * 64 + x] = (uint8_t)((inside * 255 + 2048) / 4096);
        }
    os64_geometry_target_t target = {
        .width = 64, .height = 64, .stride = 64, .capacity = sizeof(pixels), .pixels = pixels};
    const double tolerances[] = {1.0 / 64, 1.0 / 1024, 1.0 / 4096};
    size_t first_count = 0;
    for (size_t precision = 0; precision < 3; ++precision) {
        svg_geometry_t *g = NULL;
        CHECK(svg_path_lower(c, p, &identity, tolerances[precision], &g) == SVG_OK);
        size_t count;
        const os64_geometry_command_t *commands = svg_geometry_commands(g, &count);
        os64_geometry_outline_t *outline = NULL;
        CHECK(b->path(engine, commands, count, &identity, 1.0 / 64, &outline) == OS64_GEOMETRY_OK);
        memset(pixels, 0xcc, sizeof(pixels));
        CHECK(b->raster(engine, outline, &identity, &target) == OS64_GEOMETRY_OK);
        Difference d = difference(pixels, oracle);
        printf("Parsed circle, arc tolerance %.9g, %zu commands: analytic coverage max %u/255, L1 "
               "%.6f\n",
               tolerances[precision], count, d.maximum, d.total);
        CHECK(d.maximum <= 40 && d.total <= 10);
        CHECK(pixels[32 * 64 + 32] == 255 && pixels[0] == 0);
        if (!precision)
            first_count = count;
        if (precision == 2)
            CHECK(count > first_count);
        const os64_geometry_map_t wrong[] = {{1, 0, 0, 1, 1, 0}, {1.1, 0, 0, 1.1, -3.2, -3.2}};
        for (size_t i = 0; i < 2; ++i) {
            CHECK(b->raster(engine, outline, &wrong[i], &target) == OS64_GEOMETRY_OK);
            Difference bad = difference(pixels, oracle);
            CHECK(bad.maximum > 40 && bad.total > 10);
        }
        CHECK(b->release(engine, outline) == OS64_GEOMETRY_OK);
        os64_geometry_map_t map = {4, 0, .25, 1, 0, 0};
        os64_geometry_stroke_t stroke = {2, 4, OS64_GEOMETRY_ROUND_CAP, OS64_GEOMETRY_ROUND_JOIN};
        CHECK(b->stroke(engine, commands, count, &map, 1.0 / 64, &stroke, &outline) ==
              OS64_GEOMETRY_OK);
        CHECK(b->release(engine, outline) == OS64_GEOMETRY_OK);
        CHECK(svg_geometry_release(c, g) == SVG_OK);
    }
    /* An independent dense line polygon checks the coverage oracle and
     * separates cubic raster approximation from arc parameter conversion. */
    os64_geometry_command_t polygon[514] = {0};
    for (size_t i = 0; i <= 512; ++i) {
        polygon[i].verb = i ? OS64_GEOMETRY_LINE : OS64_GEOMETRY_MOVE;
        polygon[i].p[0] = ellipse(32, 32, 20, 20, 0, 2 * PI * i / 512);
    }
    polygon[513].verb = OS64_GEOMETRY_CLOSE;
    os64_geometry_outline_t *reference = NULL;
    CHECK(b->path(engine, polygon, 514, &identity, 1.0 / 64, &reference) == OS64_GEOMETRY_OK);
    CHECK(b->raster(engine, reference, &identity, &target) == OS64_GEOMETRY_OK);
    Difference dense = difference(pixels, oracle);
    printf("Independent 512-edge circle: analytic coverage max %u/255, L1 %.6f\n", dense.maximum,
           dense.total);
    CHECK(dense.maximum <= 8 && dense.total <= 2);
    CHECK(b->release(engine, reference) == OS64_GEOMETRY_OK);
    CHECK(b->destroy(engine) == OS64_GEOMETRY_OK);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void byte_fuzz(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    unsigned state = 0x12345678;
    const char alphabet[] = "MmLlHhVvCcSsQqTtAaZz0123456789+-.eE ,\n\t\0x";
    for (size_t n = 0; n < 4000; ++n) {
        char input[192];
        size_t len = n % sizeof(input);
        for (size_t i = 0; i < len; ++i) {
            state = state * 1664525u + 1013904223u;
            input[i] = alphabet[state % (sizeof(alphabet) - 1)];
        }
        if (len > 4)
            memcpy(input, "M0 0", 4);
        svg_path_t *p = NULL;
        svg_parse_result_t result;
        svg_status_t s = svg_path_parse(c, (svg_string_t){input, len}, &p, &result);
        CHECK(s == SVG_OK || s == SVG_MALFORMED || s == SVG_LIMIT);
        CHECK(result.offset <= len);
        if (p) {
            svg_geometry_t *g = NULL;
            s = svg_path_lower(c, p, &identity, .01, &g);
            CHECK(s == SVG_OK || s == SVG_LIMIT);
            if (g)
                CHECK(svg_geometry_release(c, g) == SVG_OK);
            CHECK(svg_path_release(c, p) == SVG_OK);
        }
    }
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void shapes(void)
{
    static const struct {
        svg_shape_t shape;
        const char *path;
    } cases[] = {
        {{.kind = SVG_SHAPE_RECT, .x = 2, .y = 3, .width = 20, .height = 10},
         "M2 3L22 3L22 13L2 13Z"},
        {{.kind = SVG_SHAPE_RECT, .x = 2, .y = 3, .width = 20, .height = 10, .rx = 30, .ry = 2},
         "M12 3L12 3A10 2 0 0 1 22 5L22 11A10 2 0 0 1 12 13L12 13A10 2 0 0 1 2 11L2 5A10 2 0 0 1 "
         "12 3Z"},
        {{.kind = SVG_SHAPE_ELLIPSE, .x = 30, .y = 20, .rx = 12, .ry = 6},
         "M42 20A12 6 0 0 1 30 26A12 6 0 0 1 18 20A12 6 0 0 1 30 14A12 6 0 0 1 42 20Z"},
        {{.kind = SVG_SHAPE_LINE, .x = -2, .y = 3, .x2 = 22, .y2 = 13}, "M-2 3L22 13"},
        {{.kind = SVG_SHAPE_POLYLINE, .points = {"2,3 22-3 2,13", 13}}, "M2 3L22-3L2 13"},
        {{.kind = SVG_SHAPE_POLYGON, .points = {"2,3 22-3 2,13", 13}}, "M2 3L22-3L2 13Z"},
        {{.kind = SVG_SHAPE_RECT, .width = 0, .height = 12}, ""},
        {{.kind = SVG_SHAPE_ELLIPSE, .rx = 0, .ry = 12}, ""},
    };
    Ledger l = {0};
    svg_context_t *c = context(&l);
    size_t calls = 0, polls = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        svg_path_t *p = NULL;
        svg_parse_result_t result;
        size_t before = l.allocations, polling = l.polls;
        CHECK(svg_path_from_shape(c, &cases[i].shape, &p, &result) == SVG_OK && p);
        if (i == 1) {
            calls = l.allocations - before;
            polls = l.polls - polling;
        }
        svg_path_t *expected = parse(c, cases[i].path, SVG_OK);
        size_t n, m;
        const svg_command_t *a = svg_path_commands(p, &n), *b = svg_path_commands(expected, &m);
        CHECK(n == m);
        for (size_t j = 0; j < n; ++j) {
            CHECK(a[j].verb == b[j].verb);
            near(a[j].p[0].x, b[j].p[0].x);
            near(a[j].p[0].y, b[j].p[0].y);
            near(a[j].rx, b[j].rx);
            near(a[j].ry, b[j].ry);
            CHECK(a[j].sweep == b[j].sweep && a[j].large == b[j].large);
        }
        CHECK(svg_path_release(c, p) == SVG_OK);
        CHECK(svg_path_release(c, expected) == SVG_OK);
    }
    for (size_t i = 1; i <= calls; ++i) {
        for (int admission = 0; admission < 2; ++admission) {
            l.fail = l.allocations + i;
            l.admission = admission;
            svg_path_t *p = NULL;
            svg_parse_result_t result;
            CHECK(svg_path_from_shape(c, &cases[1].shape, &p, &result) ==
                  (admission ? SVG_LIMIT : SVG_NO_MEMORY));
            CHECK(!p);
            l.fail = 0;
        }
    }
    for (size_t i = 1; i <= polls; ++i) {
        l.cancel = l.polls + i;
        svg_path_t *p = NULL;
        svg_parse_result_t result;
        CHECK(svg_path_from_shape(c, &cases[1].shape, &p, &result) == SVG_CANCELLED && !p);
        l.cancel = 0;
    }
    const char *bad[] = {"2,3 22,3 5", "2,3 22,3 nope", "2,3 22,3,,5,6"};
    for (size_t i = 0; i < 3; ++i) {
        svg_shape_t shape = {.kind = SVG_SHAPE_POLYGON, .points = {bad[i], strlen(bad[i])}};
        svg_path_t *p = NULL;
        svg_parse_result_t result;
        CHECK(svg_path_from_shape(c, &shape, &p, &result) == SVG_MALFORMED && p);
        size_t count;
        const svg_command_t *cmd = svg_path_commands(p, &count);
        CHECK(count == 3 && cmd[2].verb == SVG_CLOSE);
        CHECK(svg_path_release(c, p) == SVG_OK);
    }
    svg_shape_t invalid[] = {{.kind = SVG_SHAPE_RECT, .width = -1},
                             {.kind = SVG_SHAPE_RECT, .x = INFINITY},
                             {.kind = SVG_SHAPE_RECT, .x = 1e308, .width = 1e308, .height = 1}};
    for (size_t i = 0; i < 3; ++i) {
        svg_path_t *p = NULL;
        svg_parse_result_t result;
        CHECK(svg_path_from_shape(c, &invalid[i], &p, &result) ==
              (i ? SVG_LIMIT : SVG_BAD_ARGUMENT));
        CHECK(!p);
    }
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
static void geometric_bounds(void)
{
    Ledger l = {0};
    svg_context_t *c = context(&l);
    static const struct {
        const char *d;
        os64_geometry_map_t map;
        double x0, y0, x1, y1;
    } cases[] = {
        {"M10 20", {1, 0, 0, 1, 0, 0}, 10, 20, 10, 20},
        {"M10 20L10 20Z", {1, 0, 0, 1, 0, 0}, 10, 20, 10, 20},
        {"M0 0L10 10M100 100", {1, 0, 0, 1, 0, 0}, 0, 0, 100, 100},
        {"M0 0Q10 20 20 0", {1, 0, 0, 1, 0, 0}, 0, 0, 20, 10},
        {"M0 0C0 40 40 40 40 0", {1, 0, 0, 1, 0, 0}, 0, 0, 40, 30},
        {"M0 0C0 40 40 40 40 0", {0, 2, -3, 0, 7, 11}, -83, 11, 7, 91},
        {"M0 0C0 40 40 -40 40 0",
         {1, 0, 0, 1, 0, 0},
         0,
         -11.547005383792516,
         40,
         11.547005383792516},
        {"M0 0Q10 20 20 0", {1, 0, 1, 1, 0, 0}, 0, 0, 22.5, 10},
        {"M-10 0A10 20 0 1 0 10 0A10 20 0 1 0 -10 0Z", {1, 0, 0, 1, 0, 0}, -10, -20, 10, 20},
        {"M-10 0A10 20 0 1 0 10 0A10 20 0 1 0 -10 0Z",
         {1, 0, 1, 1, 3, 7},
         -19.360679774997898,
         -13,
         25.360679774997898,
         27},
        {"M-10 0A10 10 0 0 1 10 0", {1, 0, 0, 1, 0, 0}, -10, -10, 10, 0},
        {"M-10 0A10 10 0 0 0 10 0", {1, 0, 0, 1, 0, 0}, -10, 0, 10, 10},
        {"M-10 0A2 2 0 0 1 10 0", {1, 0, 0, 1, 0, 0}, -10, -10, 10, 0},
        {"M0 0A0 10 0 0 1 20 30", {1, 0, 0, 1, 0, 0}, 0, 0, 20, 30},
        {"M0 0C0 40 40 40 40 0", {0, 0, 0, 0, 7, 9}, 7, 9, 7, 9},
        {"M0 0C10 20 20 20 30 0", {1, 0, 0, 1, 0, 0}, 0, 0, 30, 15},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        svg_path_t *p = parse(c, cases[i].d, SVG_OK);
        svg_bounds_t b;
        size_t allocations = l.allocations, before = l.polls;
        CHECK(svg_path_bounds(c, p, &cases[i].map, &b) == SVG_OK && !b.empty);
        size_t polls = l.polls - before;
        near(b.min_x, cases[i].x0);
        near(b.min_y, cases[i].y0);
        near(b.max_x, cases[i].x1);
        near(b.max_y, cases[i].y1);
        CHECK(l.allocations == allocations);
        for (size_t j = 1; j <= polls; ++j) {
            l.cancel = l.polls + j;
            CHECK(svg_path_bounds(c, p, &cases[i].map, &b) == SVG_CANCELLED && b.empty);
            l.cancel = 0;
        }
        CHECK(svg_path_release(c, p) == SVG_OK);
    }
    svg_bounds_t b;
    svg_path_t *p = parse(c, "", SVG_OK);
    CHECK(svg_path_bounds(c, p, &identity, &b) == SVG_OK && b.empty);
    CHECK(svg_path_bounds(c, p, NULL, &b) == SVG_BAD_ARGUMENT && b.empty);
    CHECK(svg_path_release(c, p) == SVG_OK);
    p = parse(c, "M1e308 0L1e308 1", SVG_OK);
    os64_geometry_map_t huge = {2, 0, 0, 1, 0, 0};
    CHECK(svg_path_bounds(c, p, &huge, &b) == SVG_LIMIT && b.empty);
    CHECK(svg_path_release(c, p) == SVG_OK);
    CHECK(svg_destroy(c) == SVG_OK && !l.live);
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    geometric_bounds();
    shapes();
    grammar();
    arc_accuracy();
    scenes();
    instances();
    descendant_cycles();
    failures();
    boundaries();
    backend();
    byte_fuzz();
    printf("SVG core: %zu checks passed\n", checks);
    return 0;
}
