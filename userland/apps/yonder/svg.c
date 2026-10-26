#include "svg.h"
#include <math.h>
#include <os64/mem.h>
#include <os64/str.h>

#define SVG_ATTR_MAX 4096
#define SVG_DEPTH_MAX 128
#define SVG_VISITS_MAX 4096
#define SVG_ADAPTER_WORK 4000000
#define PI 3.14159265358979323846

typedef os64_geometry_map_t Map;
typedef struct {
    svg_style_t s;
    uint32_t color;
    bool current_fill, current_stroke, dashes, vector, linear;
} Computed;
typedef struct {
    const os64_html_document_t *doc;
    const garb_cascade_t *cascade;
    const os64_html_node_t *root;
    uint64_t pin, work;
    double width, height;
    uint32_t root_color, initial_color;
} Adapter;
typedef struct {
    const char *p;
    size_t left;
} Scanner;
static const Map identity = {1, 0, 0, 1, 0, 0};
static bool eq(const char *a, const char *b)
{
    return a && os64_streq(a, b);
}
static const char *attr(const os64_html_node_t *n, const char *name)
{
    const os64_html_attr_t *a = os64_html_attr(n, name);
    return a && a->value ? a->value : "";
}
static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
static void ws(Scanner *s)
{
    while (s->left && space(*s->p)) {
        ++s->p;
        --s->left;
    }
}
static bool scanner(const char *text, Scanner *s)
{
    size_t len = 0;
    while (text[len] && len <= SVG_ATTR_MAX)
        ++len;
    *s = (Scanner){text, len};
    return len <= SVG_ATTR_MAX;
}
static bool digit(char c)
{
    return c >= '0' && c <= '9';
}
// Bounded decimal conversion for viewport/transform attributes. SVG paths use
// libsvg's scanner. Range loss is refused before a matrix can reach the backend.
static bool number(Scanner *s, double *out)
{
    ws(s);
    bool neg = false;
    if (s->left && (*s->p == '+' || *s->p == '-')) {
        neg = *s->p == '-';
        ++s->p;
        --s->left;
    }
    double mant = 0;
    int exp = 0, kept = 0;
    bool any = false, dot = false, significant = false;
    while (s->left && (digit(*s->p) || (!dot && *s->p == '.'))) {
        char ch = *s->p++;
        --s->left;
        if (ch == '.') {
            dot = true;
            continue;
        }
        any = true;
        if (ch != '0')
            significant = true;
        if (significant && kept < 19) {
            mant = mant * 10 + ch - '0';
            ++kept;
            if (dot)
                --exp;
        } else if (significant) {
            if (!dot)
                ++exp;
        } else if (dot)
            --exp;
    }
    if (!any)
        return false;
    if (s->left && (*s->p == 'e' || *s->p == 'E')) {
        ++s->p;
        --s->left;
        bool minus = false;
        if (s->left && (*s->p == '+' || *s->p == '-')) {
            minus = *s->p == '-';
            ++s->p;
            --s->left;
        }
        if (!s->left || !digit(*s->p))
            return false;
        int e = 0;
        while (s->left && digit(*s->p)) {
            if (e < 10000)
                e = e * 10 + *s->p - '0';
            ++s->p;
            --s->left;
        }
        exp += minus ? -e : e;
    }
    if (!mant) {
        *out = 0;
        return true;
    }
    if (exp < -323 || exp > 308)
        return false;
    double v = mant * pow(10, exp);
    if (!isfinite(v) || !v)
        return false;
    *out = neg ? -v : v;
    return true;
}
static bool sep(Scanner *s)
{
    size_t before = s->left;
    ws(s);
    if (s->left && *s->p == ',') {
        ++s->p;
        --s->left;
        ws(s);
        return s->left != 0;
    }
    return before != s->left;
}
static bool numbers(const char *text, double *v, size_t n)
{
    Scanner s;
    if (!scanner(text, &s))
        return false;
    for (size_t i = 0; i < n; ++i)
        if ((i && !sep(&s)) || !number(&s, &v[i]))
            return false;
    ws(&s);
    return !s.left;
}
static Map multiply(Map a, Map b)
{
    return (Map){a.a * b.a + a.c * b.b,          a.b * b.a + a.d * b.b,
                 a.a * b.c + a.c * b.d,          a.b * b.c + a.d * b.d,
                 a.a * b.tx + a.c * b.ty + a.tx, a.b * b.tx + a.d * b.ty + a.ty};
}
static bool finite_map(Map m)
{
    return isfinite(m.a) && isfinite(m.b) && isfinite(m.c) && isfinite(m.d) && isfinite(m.tx) &&
           isfinite(m.ty);
}
static bool function(const char *name, const double *v, size_t n, Map *m)
{
    *m = identity;
    if (eq(name, "matrix") && n == 6) {
        *m = (Map){v[0], v[1], v[2], v[3], v[4], v[5]};
        return true;
    }
    if (eq(name, "translate") && (n == 1 || n == 2)) {
        m->tx = v[0];
        m->ty = n == 2 ? v[1] : 0;
        return true;
    }
    if (eq(name, "scale") && (n == 1 || n == 2)) {
        m->a = v[0];
        m->d = n == 2 ? v[1] : v[0];
        return true;
    }
    if (eq(name, "rotate") && (n == 1 || n == 3)) {
        double r = v[0] * PI / 180;
        m->a = m->d = cos(r);
        m->b = sin(r);
        m->c = -m->b;
        if (n == 3) {
            m->tx = v[1] - m->a * v[1] - m->c * v[2];
            m->ty = v[2] - m->b * v[1] - m->d * v[2];
        }
        return finite_map(*m);
    }
    if ((eq(name, "skewX") || eq(name, "skewx")) && n == 1) {
        m->c = tan(v[0] * PI / 180);
        return isfinite(m->c);
    }
    if ((eq(name, "skewY") || eq(name, "skewy")) && n == 1) {
        m->b = tan(v[0] * PI / 180);
        return isfinite(m->b);
    }
    return false;
}
static bool transform_attribute(const char *text, Map *out)
{
    Scanner s;
    if (!scanner(text, &s))
        return false;
    *out = identity;
    ws(&s);
    size_t count = 0;
    while (s.left) {
        if (++count > 64)
            return false;
        char name[16];
        size_t len = 0;
        while (s.left && ((*s.p >= 'a' && *s.p <= 'z') || (*s.p >= 'A' && *s.p <= 'Z'))) {
            if (len == sizeof(name) - 1)
                return false;
            name[len++] = *s.p++;
            --s.left;
        }
        name[len] = 0;
        ws(&s);
        if (!s.left || *s.p != '(')
            return false;
        ++s.p;
        --s.left;
        double v[6];
        size_t n = 0;
        ws(&s);
        while (s.left && *s.p != ')') {
            if (n == 6 || !number(&s, &v[n++]))
                return false;
            size_t before = s.left;
            ws(&s);
            if (s.left && *s.p == ')')
                break;
            bool separated = before != s.left;
            if (s.left && *s.p == ',') {
                ++s.p;
                --s.left;
                ws(&s);
                separated = true;
                if (s.left && *s.p == ')')
                    return false;
            }
            if (!separated)
                return false;
        }
        if (!s.left || *s.p != ')')
            return false;
        ++s.p;
        --s.left;
        Map m;
        if (!function(name, v, n, &m))
            return false;
        *out = multiply(*out, m);
        if (!finite_map(*out))
            return false;
        ws(&s);
        if (s.left && *s.p == ',') {
            ++s.p;
            --s.left;
            ws(&s);
            if (!s.left)
                return false;
        }
    }
    return true;
}
static bool viewbox(const os64_html_node_t *root, double v[4])
{
    return numbers(attr(root, "viewBox"), v, 4) && v[2] >= 0 && v[3] >= 0;
}
static bool dimension(const char *text, double *out)
{
    Scanner s;
    if (!scanner(text, &s) || !number(&s, out) || *out < 0)
        return false;
    if (s.left >= 2 && s.p[0] == 'p' && s.p[1] == 'x') {
        s.p += 2;
        s.left -= 2;
    }
    ws(&s);
    return !s.left;
}
bool yonder_svg_size(const os64_html_node_t *n, int32_t *w, int32_t *h)
{
    if (!flow_svg_root(n))
        return false;
    double v[4], width = 300, height = 150;
    bool wb = dimension(attr(n, "width"), &width), hb = dimension(attr(n, "height"), &height);
    if (viewbox(n, v) && v[2] > 0 && v[3] > 0) {
        double ratio = v[2] / v[3];
        if (wb && !hb)
            height = width / ratio;
        else if (hb && !wb)
            width = height * ratio;
        else if (!wb && !hb) {
            if (ratio > 2)
                height = 300 / ratio;
            else
                width = 150 * ratio;
        }
    }
    if (!isfinite(width) || !isfinite(height) || width > 1000000 || height > 1000000)
        return false;
    *w = (int32_t)(width + .5);
    *h = (int32_t)(height + .5);
    return true;
}
static svg_status_t viewport(Adapter *a, int32_t w, int32_t h, uint32_t zoom, Map *m)
{
    double v[4];
    *m = identity;
    a->width = w * 1000.0 / zoom;
    a->height = h * 1000.0 / zoom;
    if (!*attr(a->root, "viewBox")) {
        m->a = m->d = zoom / 1000.0;
        return SVG_OK;
    }
    if (!viewbox(a->root, v))
        return SVG_MALFORMED;
    if (!v[2] || !v[3])
        return SVG_PENDING; // A zero viewport has no painted content.
    a->width = v[2];
    a->height = v[3];
    double sx = w / v[2], sy = h / v[3];
    const char *par = attr(a->root, "preserveAspectRatio");
    Scanner s;
    if (!scanner(par, &s))
        return SVG_LIMIT;
    ws(&s);
    char align[16] = {0};
    size_t n = 0;
    while (s.left && !space(*s.p)) {
        if (n == 15)
            return SVG_MALFORMED;
        align[n++] = *s.p++;
        --s.left;
    }
    if (!n)
        os64_memcpy(align, "xMidYMid", 9);
    ws(&s);
    while (s.left && space(s.p[s.left - 1]))
        --s.left;
    bool slice = false;
    if (s.left) {
        if (s.left == 5 && !os64_memcmp(s.p, "slice", 5))
            slice = true;
        else if (s.left != 4 || os64_memcmp(s.p, "meet", 4))
            return SVG_MALFORMED;
    }
    if (eq(align, "none")) {
        *m = (Map){sx, 0, 0, sy, -v[0] * sx, -v[1] * sy};
        return SVG_OK;
    }
    int ax = -1, ay = -1;
    const char *xs[] = {"xMin", "xMid", "xMax"}, *ys[] = {"YMin", "YMid", "YMax"};
    if (os64_strlen(align) == 8)
        for (int i = 0; i < 3; ++i) {
            if (!os64_memcmp(align, xs[i], 4))
                ax = i;
            if (!os64_memcmp(align + 4, ys[i], 4))
                ay = i;
        }
    if (ax < 0 || ay < 0)
        return SVG_MALFORMED;
    double scale = slice ? fmax(sx, sy) : fmin(sx, sy);
    *m = (Map){scale,
               0,
               0,
               scale,
               -v[0] * scale + (w - v[2] * scale) * ax / 2,
               -v[1] * scale + (h - v[3] * scale) * ay / 2};
    return finite_map(*m) ? SVG_OK : SVG_LIMIT;
}
static const garb_val_t *winner(const garb_cascade_t *c, const os64_html_node_t *n,
                                garb_prop_t prop)
{
    garb_style_t s = garb_style_for(c, n);
    for (int32_t i = 0; i < s.n; ++i)
        if (s.sets[i].prop == prop)
            return &s.sets[i].value;
    return NULL;
}
static bool keyword(const garb_val_t *v, const char *word)
{
    return v && v->kind == GARB_V_KEYWORD && eq(v->keyword, word);
}
static bool value(const garb_val_t *v, double percent, double *out)
{
    if (v->kind == GARB_V_NUMBER || v->kind == GARB_V_ANGLE)
        *out = v->number;
    else if (v->kind == GARB_V_PERCENTAGE)
        *out = v->number * percent / 100;
    else if (v->kind == GARB_V_LENGTH && v->unit == GARB_U_PX)
        *out = v->number;
    else
        return false;
    return isfinite(*out);
}
static uint32_t color(garb_color_t c, uint32_t current)
{
    if (c.current)
        return current;
    return ((uint32_t)(fmin(1, fmax(0, c.a)) * 255 + .5) << 24) |
           ((uint32_t)(fmin(255, fmax(0, c.r)) + .5) << 16) |
           ((uint32_t)(fmin(255, fmax(0, c.g)) + .5) << 8) |
           (uint32_t)(fmin(255, fmax(0, c.b)) + .5);
}
static Computed initial(uint32_t ink)
{
    return (Computed){.s = svg_initial_style(), .color = ink};
}
static void copy_property(Computed *d, const Computed *s, garb_prop_t p)
{
    switch (p) {
    case GARB_COLOR:
        d->color = s->color;
        break;
    case GARB_FILL:
        d->s.fill = s->s.fill;
        d->current_fill = s->current_fill;
        break;
    case GARB_STROKE:
        d->s.stroke = s->s.stroke;
        d->current_stroke = s->current_stroke;
        break;
    case GARB_FILL_OPACITY:
        d->s.fill_opacity = s->s.fill_opacity;
        break;
    case GARB_STROKE_OPACITY:
        d->s.stroke_opacity = s->s.stroke_opacity;
        break;
    case GARB_OPACITY:
        d->s.opacity = s->s.opacity;
        break;
    case GARB_VISIBILITY:
        d->s.visible = s->s.visible;
        break;
    case GARB_DISPLAY:
        d->s.display = s->s.display;
        break;
    case GARB_FILL_RULE:
        d->s.even_odd = s->s.even_odd;
        break;
    case GARB_STROKE_WIDTH:
        d->s.stroke_style.width = s->s.stroke_style.width;
        break;
    case GARB_STROKE_MITERLIMIT:
        d->s.stroke_style.miter_limit = s->s.stroke_style.miter_limit;
        break;
    case GARB_STROKE_LINECAP:
        d->s.stroke_style.cap = s->s.stroke_style.cap;
        break;
    case GARB_STROKE_LINEJOIN:
        d->s.stroke_style.join = s->s.stroke_style.join;
        break;
    case GARB_STROKE_DASHARRAY:
        d->dashes = s->dashes;
        break;
    case GARB_VECTOR_EFFECT:
        d->vector = s->vector;
        break;
    case GARB_COLOR_INTERPOLATION:
        d->linear = s->linear;
        break;
    case GARB_TRANSFORM:
        d->s.transform = s->s.transform;
        break;
    default:
        break;
    }
}
static svg_status_t css_transform(const garb_val_t *v, Map *out)
{
    *out = identity;
    if (keyword(v, "none"))
        return SVG_OK;
    if (v->kind != GARB_V_FUNCTIONS)
        return SVG_UNSUPPORTED;
    for (int32_t i = 0; i < v->nitems; ++i) {
        const garb_val_t *f = &v->items[i];
        double args[6];
        if (f->nitems > 6)
            return SVG_UNSUPPORTED;
        for (int32_t j = 0; j < f->nitems; ++j) {
            const garb_val_t *a = &f->items[j];
            if (a->kind == GARB_V_PERCENTAGE || !value(a, 1, &args[j]))
                return SVG_UNSUPPORTED;
            if (a->kind == GARB_V_ANGLE) {
                if (a->unit == GARB_U_RAD)
                    args[j] *= 180 / PI;
                else if (a->unit == GARB_U_GRAD)
                    args[j] *= .9;
                else if (a->unit == GARB_U_TURN)
                    args[j] *= 360;
            }
        }
        Map m;
        if (!function(f->keyword, args, (size_t)f->nitems, &m))
            return SVG_UNSUPPORTED;
        *out = multiply(*out, m);
        if (!finite_map(*out))
            return SVG_LIMIT;
    }
    return SVG_OK;
}
static svg_status_t apply(Adapter *a, Computed *c, const Computed *parent, garb_prop_t prop,
                          const garb_val_t *v)
{
    Computed defaults = initial(a->initial_color);
    if (v->kind == GARB_V_WIDE) {
        bool inherit =
            eq(v->keyword, "inherit") || (!eq(v->keyword, "initial") && garb_prop_inherited(prop));
        copy_property(c, inherit ? parent : &defaults, prop);
        return SVG_OK;
    }
    double n = 0;
    switch (prop) {
    case GARB_COLOR:
        c->color = color(v->color, parent->color);
        break;
    case GARB_FILL:
    case GARB_STROKE: {
        svg_paint_t *p = prop == GARB_FILL ? &c->s.fill : &c->s.stroke;
        bool *current = prop == GARB_FILL ? &c->current_fill : &c->current_stroke;
        *current = v->kind == GARB_V_COLOR && v->color.current;
        *p = keyword(v, "none")        ? (svg_paint_t){SVG_PAINT_NONE, 0}
             : v->kind == GARB_V_COLOR ? (svg_paint_t){SVG_PAINT_COLOR, color(v->color, c->color)}
                                       : (svg_paint_t){SVG_PAINT_UNSUPPORTED, 0};
        break;
    }
    case GARB_FILL_OPACITY:
    case GARB_STROKE_OPACITY:
    case GARB_OPACITY:
        if (!value(v, 1, &n))
            return SVG_UNSUPPORTED;
        n = fmax(0, fmin(1, n));
        if (prop == GARB_OPACITY)
            c->s.opacity = n;
        else if (prop == GARB_FILL_OPACITY)
            c->s.fill_opacity = n;
        else
            c->s.stroke_opacity = n;
        break;
    case GARB_DISPLAY:
        c->s.display = !keyword(v, "none");
        break;
    case GARB_VISIBILITY:
        c->s.visible = keyword(v, "visible");
        break;
    case GARB_FILL_RULE:
        c->s.even_odd = keyword(v, "evenodd");
        break;
    case GARB_STROKE_WIDTH:
        if (!value(v, hypot(a->width, a->height) / sqrt(2), &n))
            return SVG_UNSUPPORTED;
        c->s.stroke_style.width = fmax(0, n);
        break;
    case GARB_STROKE_MITERLIMIT:
        if (!value(v, 1, &n))
            return SVG_UNSUPPORTED;
        c->s.stroke_style.miter_limit = fmax(1, n);
        break;
    case GARB_STROKE_LINECAP:
        c->s.stroke_style.cap = keyword(v, "round")    ? OS64_GEOMETRY_ROUND_CAP
                                : keyword(v, "square") ? OS64_GEOMETRY_SQUARE
                                                       : OS64_GEOMETRY_BUTT;
        break;
    case GARB_STROKE_LINEJOIN:
        c->s.stroke_style.join = keyword(v, "round")        ? OS64_GEOMETRY_ROUND_JOIN
                                 : keyword(v, "bevel")      ? OS64_GEOMETRY_BEVEL
                                 : keyword(v, "arcs")       ? OS64_GEOMETRY_ARCS
                                 : keyword(v, "miter-clip") ? OS64_GEOMETRY_MITER_CLIP
                                                            : OS64_GEOMETRY_MITER;
        break;
    case GARB_STROKE_DASHARRAY:
        c->dashes = !keyword(v, "none");
        break;
    case GARB_VECTOR_EFFECT:
        c->vector = !keyword(v, "none");
        break;
    case GARB_COLOR_INTERPOLATION:
        c->linear = keyword(v, "linearrgb");
        break;
    case GARB_TRANSFORM:
        return css_transform(v, &c->s.transform);
    default:
        break;
    }
    return SVG_OK;
}
static bool ignored(const os64_html_node_t *n)
{
    return n->ns == OS64_HTML_NS_SVG &&
           (eq(n->name, "defs") || eq(n->name, "title") || eq(n->name, "desc") ||
            eq(n->name, "metadata") || eq(n->name, "style"));
}
static svg_status_t compute(Adapter *a, const os64_html_node_t *n, Computed *c)
{
    const os64_html_node_t *chain[SVG_DEPTH_MAX];
    size_t depth = 0;
    for (const os64_html_node_t *p = n; p && p->kind == OS64_HTML_ELEMENT; p = p->parent) {
        if (depth == SVG_DEPTH_MAX)
            return SVG_LIMIT;
        chain[depth++] = p;
    }
    *c = initial(a->initial_color);
    while (depth) {
        if (++a->work > SVG_ADAPTER_WORK)
            return SVG_LIMIT;
        const os64_html_node_t *p = chain[--depth];
        Computed parent = *c;
        c->s.opacity = 1;
        c->s.transform = identity;
        c->s.display = true;
        c->vector = false;
        garb_style_t style = garb_style_for(a->cascade, p);
        // Resolve color first. An inherited currentColor paint stays symbolic
        // until the descendant's own color has been computed.
        const garb_val_t *cv = winner(a->cascade, p, GARB_COLOR);
        if (cv) {
            svg_status_t s = apply(a, c, &parent, GARB_COLOR, cv);
            if (s != SVG_OK)
                return s;
        }
        for (int32_t j = 0; j < style.n; ++j) {
            if (++a->work > SVG_ADAPTER_WORK)
                return SVG_LIMIT;
            garb_prop_t prop = style.sets[j].prop;
            if (prop == GARB_COLOR)
                continue;
            // HTML ancestors contribute inherited paint/color, not their box
            // transforms. The outer SVG's CSS transform is painted by flow.
            if (prop == GARB_TRANSFORM && p->ns != OS64_HTML_NS_SVG)
                continue;
            svg_status_t s = apply(a, c, &parent, prop, &style.sets[j].value);
            if (s != SVG_OK)
                return s;
        }
        // The outer CSS box includes HTML presentation hints and the face
        // ink; its resolved color is the SVG subtree's inheritance boundary.
        if (p == a->root)
            c->color = a->root_color;
        if (p->ns == OS64_HTML_NS_SVG && !winner(a->cascade, p, GARB_TRANSFORM)) {
            Scanner scan;
            if (!scanner(attr(p, "transform"), &scan))
                return SVG_LIMIT;
            a->work += scan.left;
            if (a->work > SVG_ADAPTER_WORK)
                return SVG_LIMIT;
            if (!transform_attribute(scan.p, &c->s.transform))
                return SVG_MALFORMED;
        }
    }
    if (c->current_fill)
        c->s.fill.argb = c->color;
    if (c->current_stroke)
        c->s.stroke.argb = c->color;
    return SVG_OK;
}
static svg_status_t acquire(void *ctx, svg_mode_t mode, void **snapshot, uint64_t *root)
{
    Adapter *a = ctx;
    if (mode != SVG_MODE_INLINE)
        return SVG_UNSUPPORTED;
    a->pin = os64_html_pin(a->doc);
    if (!a->pin)
        return SVG_LIMIT;
    *snapshot = a;
    *root = (uintptr_t)a->root;
    return SVG_OK;
}
static void release(void *ctx, void *snapshot)
{
    Adapter *a = ctx;
    (void)snapshot;
    os64_html_unpin(a->doc, a->pin);
    a->pin = 0;
}
static const os64_html_node_t *element(Adapter *a, const os64_html_node_t *n)
{
    while (n && n->kind != OS64_HTML_ELEMENT) {
        if (++a->work > SVG_ADAPTER_WORK)
            return NULL;
        n = n->next;
    }
    return n;
}
static svg_status_t node(void *ctx, void *snapshot, uint64_t source, svg_node_t *out)
{
    Adapter *a = ctx;
    (void)snapshot;
    const os64_html_node_t *n = (const void *)(uintptr_t)source;
    bool group = n == a->root || (n->ns == OS64_HTML_NS_SVG && eq(n->name, "g"));
    out->kind = group                                              ? SVG_NODE_GROUP
                : n->ns == OS64_HTML_NS_SVG && eq(n->name, "path") ? SVG_NODE_PATH
                                                                   : SVG_NODE_UNSUPPORTED;
    out->first_child = group ? (uintptr_t)element(a, n->first_child) : 0;
    out->next_sibling = (uintptr_t)element(a, n->next);
    return a->work > SVG_ADAPTER_WORK ? SVG_LIMIT : SVG_OK;
}
static svg_status_t style(void *ctx, void *snapshot, const svg_style_request_t *r, svg_style_t *out,
                          svg_string_t *d)
{
    Adapter *a = ctx;
    (void)snapshot;
    const os64_html_node_t *n = (const void *)(uintptr_t)r->source;
    if (ignored(n)) {
        *out = svg_initial_style();
        out->display = false;
        return SVG_OK;
    }
    Computed c;
    svg_status_t status = compute(a, n, &c);
    if (status != SVG_OK)
        return status;
    *out = c.s;
    // The CSS box owns root opacity and its CSS transform. Keep their computed
    // values available to an explicit inherit on descendants, but paint once.
    if (n == a->root) {
        out->opacity = 1;
        out->transform = identity;
    }
    if (!out->display)
        return SVG_OK;
    bool path = n->ns == OS64_HTML_NS_SVG && eq(n->name, "path");
    if (path && out->visible && ((c.dashes || c.vector) && out->stroke.kind != SVG_PAINT_NONE))
        return SVG_UNSUPPORTED;
    if (c.linear)
        return SVG_UNSUPPORTED;
    // Features absent from the prepared-scene profile must not masquerade as
    // ordinary solid artwork. CSS declarations not read by garb are also
    // recorded by the page's existing skipped-style diagnostics.
    const char *unsupported[] = {"clip-path",  "mask",       "filter",     "marker-start",
                                 "marker-mid", "marker-end", "paint-order"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        const char *v = attr(n, unsupported[i]);
        if (*v && !eq(v, "none") && !eq(v, "normal"))
            return SVG_UNSUPPORTED;
    }
    if (n != a->root) {
        const garb_prop_t unsupported_props[] = {GARB_TRANSFORM_ORIGIN, GARB_TRANSLATE, GARB_ROTATE,
                                                 GARB_SCALE};
        for (size_t i = 0; i < sizeof(unsupported_props) / sizeof(unsupported_props[0]); ++i)
            if (winner(a->cascade, n, unsupported_props[i]))
                return SVG_UNSUPPORTED;
    }
    if (!path) {
        out->fill.kind = SVG_PAINT_NONE;
        out->stroke.kind = SVG_PAINT_NONE;
    }
    const char *data = attr(n, "d");
    size_t len = 0;
    while (len <= 1048576 && data[len])
        ++len;
    if (len > 1048576)
        return SVG_LIMIT;
    *d = (svg_string_t){data, len};
    return SVG_OK;
}
void yonder_svg_pixels_free(const os64_geometry_memory_t *m, yonder_svg_pixels_t *out)
{
    if (out->pixels)
        m->free(m->context, out->pixels, out->bytes);
    *out = (yonder_svg_pixels_t){0};
}
svg_status_t yonder_svg_render(const os64_html_document_t *doc, const garb_cascade_t *cascade,
                               const flow_box_t *box, os64_gui_rect_t content, os64_gui_rect_t clip,
                               uint32_t zoom, uint32_t initial_color,
                               const os64_geometry_memory_t *memory,
                               const os64_geometry_backend_t *backend, yonder_svg_pixels_t *out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = (yonder_svg_pixels_t){0};
    if (!doc || !cascade || !box || !flow_svg_root(box->node) || !memory || !memory->alloc ||
        !memory->free || !zoom)
        return SVG_BAD_ARGUMENT;
    if (garb_cascade_env(cascade).modern_off)
        return SVG_UNSUPPORTED;
    if (garb_cascade_incomplete(cascade))
        return SVG_LIMIT;
    int64_t x = fmax(content.x, clip.x), y = fmax(content.y, clip.y);
    int64_t x1 = fmin((int64_t)content.x + content.w, (int64_t)clip.x + clip.w);
    int64_t y1 = fmin((int64_t)content.y + content.h, (int64_t)clip.y + clip.h);
    if (content.w <= 0 || content.h <= 0 || x1 <= x || y1 <= y)
        return SVG_OK;
    uint64_t width = (uint64_t)(x1 - x), height = (uint64_t)(y1 - y);
    if (width > 16384 || height > 16384 || width * height > 8388608)
        return SVG_LIMIT;
    Adapter a = {.doc = doc,
                 .cascade = cascade,
                 .root = box->node,
                 .root_color = box->style->color ^ 0xff000000u,
                 .initial_color = initial_color};
    Map map;
    svg_status_t status = viewport(&a, content.w, content.h, zoom, &map);
    if (status == SVG_PENDING)
        return SVG_OK;
    if (status != SVG_OK)
        return status;
    if (!winner(cascade, box->node, GARB_TRANSFORM)) {
        Map root_transform;
        if (!transform_attribute(attr(box->node, "transform"), &root_transform))
            return SVG_MALFORMED;
        // The root transform sits outside viewBox, in CSS user units.
        // Convert its translation to the zoomed box coordinate system.
        root_transform.tx *= zoom / 1000.0;
        root_transform.ty *= zoom / 1000.0;
        map = multiply(root_transform, map);
        if (!finite_map(map))
            return SVG_LIMIT;
    }
    svg_context_t *c = NULL;
    svg_scene_t *scene = NULL;
    svg_options_t options = {.memory = *memory, .limits = svg_default_limits()};
    options.limits.scene_nodes = SVG_VISITS_MAX;
    status = svg_create(&options, &c);
    if (status != SVG_OK)
        return status;
    svg_provider_t provider = {&a, acquire, release, node, style};
    status = svg_scene_prepare(c, &provider, SVG_MODE_INLINE, &map, &scene);
    if (status == SVG_OK) {
        out->bytes = (size_t)(width * height * 4);
        void *pixels = NULL;
        os64_geometry_status_t allocation = memory->alloc(memory->context, out->bytes, &pixels);
        if (allocation != OS64_GEOMETRY_OK || !pixels)
            status = allocation == OS64_GEOMETRY_LIMIT ? SVG_LIMIT : SVG_NO_MEMORY;
        else {
            out->pixels = pixels;
            out->rect = (os64_gui_rect_t){(int32_t)x, (int32_t)y, (int32_t)width, (int32_t)height};
            svg_render_target_t target = {.origin_x = (double)(x - content.x),
                                          .origin_y = (double)(y - content.y),
                                          .width = (uint32_t)width,
                                          .height = (uint32_t)height,
                                          .stride = (size_t)width * 4,
                                          .capacity = out->bytes,
                                          .pixels = out->pixels};
            svg_render_options_t render = svg_render_options_default(backend);
            status = svg_scene_render(c, scene, &render, &target, NULL);
        }
    }
    if (scene)
        svg_scene_release(c, scene);
    svg_destroy(c);
    if (status != SVG_OK)
        yonder_svg_pixels_free(memory, out);
    return status;
}
