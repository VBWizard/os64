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
    // Borrowed from the live cascade; evaluate when SVG paints these values.
    const garb_val_t *transform_css, *opacity_css, *dash_css, *dashoffset_css, *clip_css;
    const garb_val_t *fill_css, *stroke_css;
    uint32_t color;
    bool current_fill, current_stroke, vector, linear, clip_even_odd;
    int overflow[2]; // visible, hidden, scroll, auto, clip
} Computed;
typedef struct {
    const os64_html_node_t *source;
    uint64_t parent;
    bool instanced;
    const garb_val_t *clip_css;
    double width, height; // Descendant percentage basis in viewport user units.
} Instance;
typedef struct {
    const os64_html_document_t *doc;
    const garb_cascade_t *cascade;
    const os64_html_node_t *root;
    uint64_t pin, work;
    double width, height;
    double dash_values[SVG_DASH_MAX];
    svg_linear_gradient_t gradients[2];
    svg_radial_gradient_t radial_gradients[2];
    svg_gradient_stop_t gradient_stops[2][SVG_GRADIENT_STOPS_MAX];
    uint32_t root_color, initial_color;
    os64_geometry_memory_t memory;
    Instance *instances;
    size_t capacity;
    bool base_checked, clip_geometry, clip_style;
    svg_mode_t mode;
    svg_viewport_t image_viewport;
    svg_status_t stopped;
    void *cancel_context;
    bool (*cancelled)(void *);
} Adapter;
typedef struct {
    const char *p;
    size_t left;
} Scanner;
static const Map identity = {1, 0, 0, 1, 0, 0};
static bool adapter_spend(Adapter *a, uint64_t amount)
{
    if (a->stopped != SVG_OK)
        return false;
    if (a->cancelled && a->cancelled(a->cancel_context))
        a->stopped = SVG_CANCELLED;
    else if (amount > SVG_ADAPTER_WORK - a->work)
        a->stopped = SVG_LIMIT;
    else
        a->work += amount;
    return a->stopped == SVG_OK;
}
static bool same_word(const char *a, const char *b)
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
    if (same_word(name, "matrix") && n == 6) {
        *m = (Map){v[0], v[1], v[2], v[3], v[4], v[5]};
        return true;
    }
    if (same_word(name, "translate") && (n == 1 || n == 2)) {
        m->tx = v[0];
        m->ty = n == 2 ? v[1] : 0;
        return true;
    }
    if (same_word(name, "scale") && (n == 1 || n == 2)) {
        m->a = v[0];
        m->d = n == 2 ? v[1] : v[0];
        return true;
    }
    if (same_word(name, "rotate") && (n == 1 || n == 3)) {
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
    if ((same_word(name, "skewX") || same_word(name, "skewx")) && n == 1) {
        m->c = tan(v[0] * PI / 180);
        return isfinite(m->c);
    }
    if ((same_word(name, "skewY") || same_word(name, "skewy")) && n == 1) {
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
    double parsed;
    if (!scanner(text, &s) || !number(&s, &parsed) || parsed < 0)
        return false;
    if (s.left >= 2 && s.p[0] == 'p' && s.p[1] == 'x') {
        s.p += 2;
        s.left -= 2;
    }
    ws(&s);
    if (s.left)
        return false;
    *out = parsed;
    return true;
}
static bool natural_size(const os64_html_node_t *n, double width, double height,
                         bool wb, bool hb, int32_t *w, int32_t *h)
{
    double v[4];
    if (!wb) width = 300;
    if (!hb) height = 150;
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
bool yonder_svg_size(const os64_html_node_t *n, int32_t *w, int32_t *h)
{
    if (!flow_svg_root(n))
        return false;
    double width = 300, height = 150;
    bool wb = dimension(attr(n, "width"), &width), hb = dimension(attr(n, "height"), &height);
    return natural_size(n, width, height, wb, hb, w, h);
}
static svg_status_t viewport_map(const os64_html_node_t *node, double w, double h,
                                 double *user_width, double *user_height, Map *m)
{
    double v[4];
    *m = identity;
    *user_width = w;
    *user_height = h;
    if (!*attr(node, "viewBox"))
        return SVG_OK;
    if (!viewbox(node, v))
        return SVG_MALFORMED;
    if (!v[2] || !v[3])
        return SVG_PENDING; // A zero viewport has no painted content.
    *user_width = v[2];
    *user_height = v[3];
    double sx = w / v[2], sy = h / v[3];
    const char *par = attr(node, "preserveAspectRatio");
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
    if (same_word(align, "none")) {
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
static svg_status_t viewport(Adapter *a, int32_t w, int32_t h, uint32_t zoom, Map *m)
{
    svg_status_t status =
        viewport_map(a->root, w * 1000.0 / zoom, h * 1000.0 / zoom, &a->width, &a->height, m);
    if (status == SVG_OK)
        *m = multiply((Map){zoom / 1000.0, 0, 0, zoom / 1000.0, 0, 0}, *m);
    return status;
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
    return v && v->kind == GARB_V_KEYWORD && same_word(v->keyword, word);
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
static bool natural_dimension(const garb_val_t *v, double *out)
{
    if (!v || v->kind != GARB_V_LENGTH || v->number < 0)
        return false;
    double factor;
    switch (v->unit) {
    case GARB_U_PX: factor = 1; break;
    case GARB_U_IN: factor = 96; break;
    case GARB_U_CM: factor = 96 / 2.54; break;
    case GARB_U_MM: factor = 96 / 25.4; break;
    case GARB_U_Q: factor = 96 / 101.6; break;
    case GARB_U_PT: factor = 96 / 72.0; break;
    case GARB_U_PC: factor = 16; break;
    default: return false;
    }
    *out = v->number * factor;
    return isfinite(*out);
}
bool yonder_svg_natural_size(const os64_html_node_t *n, const garb_cascade_t *c,
                              int32_t *w, int32_t *h)
{
    if (!flow_svg_root(n) || !c || garb_cascade_incomplete(c))
        return false;
    double width = 300, height = 150;
    bool wb = natural_dimension(winner(c, n, GARB_WIDTH), &width);
    bool hb = natural_dimension(winner(c, n, GARB_HEIGHT), &height);
    return natural_size(n, width, height, wb, hb, w, h);
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
    case GARB_OVERFLOW_X:
    case GARB_OVERFLOW_Y:
        d->overflow[p == GARB_OVERFLOW_Y] = s->overflow[p == GARB_OVERFLOW_Y];
        break;
    case GARB_COLOR:
        d->color = s->color;
        break;
    case GARB_FILL:
        d->s.fill = s->s.fill;
        d->current_fill = s->current_fill;
        d->fill_css = s->fill_css;
        break;
    case GARB_STROKE:
        d->s.stroke = s->s.stroke;
        d->current_stroke = s->current_stroke;
        d->stroke_css = s->stroke_css;
        break;
    case GARB_FILL_OPACITY:
        d->s.fill_opacity = s->s.fill_opacity;
        break;
    case GARB_STROKE_OPACITY:
        d->s.stroke_opacity = s->s.stroke_opacity;
        break;
    case GARB_OPACITY:
        d->s.opacity = s->s.opacity;
        d->opacity_css = s->opacity_css;
        break;
    case GARB_VISIBILITY:
        d->s.visible = s->s.visible;
        break;
    case GARB_DISPLAY:
        d->s.display = s->s.display;
        break;
    case GARB_CLIP_PATH:
        d->clip_css = s->clip_css;
        break;
    case GARB_CLIP_RULE:
        d->clip_even_odd = s->clip_even_odd;
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
        d->dash_css = s->dash_css;
        break;
    case GARB_STROKE_DASHOFFSET:
        d->dashoffset_css = s->dashoffset_css;
        break;
    case GARB_VECTOR_EFFECT:
        d->vector = s->vector;
        break;
    case GARB_COLOR_INTERPOLATION:
        d->linear = s->linear;
        break;
    case GARB_TRANSFORM:
        d->s.transform = s->s.transform;
        d->transform_css = s->transform_css;
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
        bool inherit = same_word(v->keyword, "inherit") ||
                       (!same_word(v->keyword, "initial") && garb_prop_inherited(prop));
        copy_property(c, inherit ? parent : &defaults, prop);
        return SVG_OK;
    }
    double n = 0;
    switch (prop) {
    case GARB_CLIP_PATH:
        c->clip_css = v;
        break;
    case GARB_CLIP_RULE:
        c->clip_even_odd = keyword(v, "evenodd");
        break;
    case GARB_OVERFLOW_X:
    case GARB_OVERFLOW_Y:
        c->overflow[prop == GARB_OVERFLOW_Y] = keyword(v, "hidden")   ? 1
                                               : keyword(v, "scroll") ? 2
                                               : keyword(v, "auto")   ? 3
                                               : keyword(v, "clip")   ? 4
                                                                      : 0;
        break;
    case GARB_COLOR:
        c->color = color(v->color, parent->color);
        break;
    case GARB_FILL:
    case GARB_STROKE: {
        svg_paint_t *p = prop == GARB_FILL ? &c->s.fill : &c->s.stroke;
        bool *current = prop == GARB_FILL ? &c->current_fill : &c->current_stroke;
        *current = v->kind == GARB_V_COLOR && v->color.current;
        if (prop == GARB_FILL)
            c->fill_css = v;
        else
            c->stroke_css = v;
        *p = keyword(v, "none") ? (svg_paint_t){.kind = SVG_PAINT_NONE}
             : v->kind == GARB_V_COLOR
                 ? (svg_paint_t){.kind = SVG_PAINT_COLOR, .argb = color(v->color, c->color)}
                 : (svg_paint_t){.kind = SVG_PAINT_UNSUPPORTED};
        break;
    }
    case GARB_OPACITY:
        c->opacity_css = v;
        break;
    case GARB_FILL_OPACITY:
    case GARB_STROKE_OPACITY:
        if (!value(v, 1, &n))
            return SVG_UNSUPPORTED;
        n = fmax(0, fmin(1, n));
        if (prop == GARB_FILL_OPACITY)
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
        c->dash_css = v;
        break;
    case GARB_STROKE_DASHOFFSET:
        c->dashoffset_css = v;
        break;
    case GARB_VECTOR_EFFECT:
        c->vector = !keyword(v, "none");
        break;
    case GARB_COLOR_INTERPOLATION:
        c->linear = keyword(v, "linearrgb");
        break;
    case GARB_TRANSFORM:
        c->transform_css = v;
        break;
    default:
        break;
    }
    return SVG_OK;
}
static bool image_inert(const Adapter *a, const os64_html_node_t *n)
{
    return a->mode == SVG_MODE_IMAGE && n->ns == OS64_HTML_NS_SVG &&
           (same_word(n->name, "script") || same_word(n->name, "animate") ||
            same_word(n->name, "animateMotion") || same_word(n->name, "animateTransform") ||
            same_word(n->name, "set") || same_word(n->name, "discard"));
}
static bool ignored(const os64_html_node_t *n)
{
    return n->ns == OS64_HTML_NS_SVG &&
           (same_word(n->name, "defs") || same_word(n->name, "title") ||
            same_word(n->name, "desc") || same_word(n->name, "metadata") ||
            same_word(n->name, "style") || same_word(n->name, "clipPath") ||
            same_word(n->name, "linearGradient") || same_word(n->name, "radialGradient") ||
            same_word(n->name, "stop"));
}
// Instance IDs come from libsvg's bounded visit sequence. Records preserve
// inheritance without cloning the DOM or retaining pointers into a growable array.
static svg_status_t remember(Adapter *a, const svg_style_request_t *r)
{
    if (!r->instance || r->instance > SVG_VISITS_MAX || r->parent_instance >= r->instance)
        return SVG_PROVIDER_ERROR;
    if (r->instance >= a->capacity) {
        size_t capacity = a->capacity ? a->capacity * 2 : 16;
        while (capacity <= r->instance)
            capacity *= 2;
        if (capacity > SVG_VISITS_MAX + 1)
            capacity = SVG_VISITS_MAX + 1;
        void *p = NULL;
        os64_geometry_status_t status =
            a->memory.alloc(a->memory.context, capacity * sizeof(Instance), &p);
        if (status != OS64_GEOMETRY_OK || !p)
            return status == OS64_GEOMETRY_LIMIT ? SVG_LIMIT : SVG_NO_MEMORY;
        os64_memset(p, 0, capacity * sizeof(Instance));
        if (a->instances) {
            os64_memcpy(p, a->instances, a->capacity * sizeof(Instance));
            a->memory.free(a->memory.context, a->instances, a->capacity * sizeof(Instance));
        }
        a->instances = p;
        a->capacity = capacity;
    }
    Instance parent = a->instances[r->parent_instance];
    if (r->parent_instance && !parent.source)
        return SVG_PROVIDER_ERROR;
    a->instances[r->instance] = (Instance){
        .source = (const void *)(uintptr_t)r->source,
        .parent = r->parent_instance,
        .width = parent.source ? parent.width : a->width,
        .height = parent.source ? parent.height : a->height,
        .instanced = parent.instanced || (parent.source && parent.source->ns == OS64_HTML_NS_SVG &&
                                          same_word(parent.source->name, "use"))};
    return SVG_OK;
}
static svg_status_t compute(Adapter *a, uint64_t instance, Computed *c)
{
    const os64_html_node_t *chain[SVG_DEPTH_MAX];
    size_t depth = 0;
    const os64_html_node_t *p = NULL;
    while (instance) {
        Instance record = a->instances[instance];
        p = record.source;
        if (depth == SVG_DEPTH_MAX)
            return SVG_LIMIT;
        // Source-selector matching is reusable, DOM-resolved custom properties
        // are not. Instance substitution is outside this profile (SVG_INSTANCES.md).
        if (record.instanced && garb_style_needs_substitution(a->cascade, p))
            return SVG_UNSUPPORTED;
        chain[depth++] = p;
        instance = record.parent;
    }
    for (p = p ? p->parent : NULL; p && p->kind == OS64_HTML_ELEMENT; p = p->parent) {
        if (depth == SVG_DEPTH_MAX)
            return SVG_LIMIT;
        chain[depth++] = p;
    }
    *c = initial(a->initial_color);
    while (depth) {
        if (!adapter_spend(a, 1))
            return a->stopped;
        const os64_html_node_t *p = chain[--depth];
        Computed parent = *c;
        c->s.opacity = 1;
        c->opacity_css = NULL;
        c->clip_css = NULL;
        c->s.transform = identity;
        c->transform_css = NULL;
        c->s.display = true;
        c->vector = false;
        // SVG's UA rule clips nested svg and symbol viewports by default.
        c->overflow[0] = c->overflow[1] =
            p->ns == OS64_HTML_NS_SVG && (same_word(p->name, "svg") || same_word(p->name, "symbol"))
                ? 1
                : 0;
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
            if (!adapter_spend(a, 1))
                return a->stopped;
            garb_prop_t prop = style.sets[j].prop;
            if (prop == GARB_COLOR)
                continue;
            // Clip silhouettes have no fill, stroke, opacity or color-space
            // paint semantics; even unsupported paint values cannot alter them.
            if (a->clip_style && prop != GARB_DISPLAY && prop != GARB_VISIBILITY &&
                prop != GARB_TRANSFORM && prop != GARB_CLIP_PATH && prop != GARB_CLIP_RULE)
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
        if (p == a->root && a->mode == SVG_MODE_INLINE)
            c->color = a->root_color;
        if (p->ns == OS64_HTML_NS_SVG && !winner(a->cascade, p, GARB_TRANSFORM)) {
            Scanner scan;
            if (!scanner(attr(p, "transform"), &scan))
                return SVG_LIMIT;
            if (!adapter_spend(a, scan.left))
                return a->stopped;
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
    if (mode != a->mode)
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
    if (a->instances)
        a->memory.free(a->memory.context, a->instances, a->capacity * sizeof(Instance));
    a->instances = NULL;
    a->capacity = 0;
}
static const os64_html_node_t *element(Adapter *a, const os64_html_node_t *n)
{
    while (n && n->kind != OS64_HTML_ELEMENT) {
        if (!adapter_spend(a, 1))
            return NULL;
        n = n->next;
    }
    return n;
}
static bool basic_shape(const os64_html_node_t *n)
{
    return n->ns == OS64_HTML_NS_SVG &&
           (same_word(n->name, "rect") || same_word(n->name, "circle") ||
            same_word(n->name, "ellipse") || same_word(n->name, "line") ||
            same_word(n->name, "polyline") || same_word(n->name, "polygon"));
}
// Geometry is non-inherited. Follow ancestry only for an explicit inherit,
// keeping percentages symbolic until the consuming shape's viewport is known.
static svg_status_t geometry_value(Adapter *a, uint64_t instance, garb_prop_t prop, double percent,
                                   double initial_value, double *out)
{
    *out = initial_value;
    const os64_html_node_t *n = a->instances[instance].source;
    size_t depth = 0;
    while (n && n->kind == OS64_HTML_ELEMENT) {
        if (++depth > SVG_DEPTH_MAX)
            return SVG_LIMIT;
        if (!adapter_spend(a, 1))
            return a->stopped;
        const garb_val_t *v = winner(a->cascade, n, prop);
        if (!v || keyword(v, "auto"))
            return SVG_OK;
        if (v->kind == GARB_V_WIDE) {
            if (!same_word(v->keyword, "inherit"))
                return SVG_OK;
            uint64_t parent = instance ? a->instances[instance].parent : 0;
            n = parent ? a->instances[parent].source : n->parent;
            instance = parent;
            continue;
        }
        if (a->clip_geometry && v->kind == GARB_V_PERCENTAGE)
            return SVG_UNSUPPORTED; // Resource percentage bases need their own viewport contract.
        return value(v, percent, out) ? SVG_OK : SVG_UNSUPPORTED;
    }
    return SVG_OK;
}
static svg_status_t nested_viewport(Adapter *a, const svg_style_request_t *r, const Computed *c,
                                    svg_style_t *out)
{
    const os64_html_node_t *n = (const void *)(uintptr_t)r->source;
    uint64_t parent = r->parent_instance;
    const os64_html_node_t *host = parent ? a->instances[parent].source : NULL;
    bool referenced = host && host->ns == OS64_HTML_NS_SVG && same_word(host->name, "use");
    if (same_word(n->name, "symbol") && (*attr(n, "refX") || *attr(n, "refY")))
        return SVG_UNSUPPORTED;
    double w = NAN, h = NAN, x, y;
    svg_status_t status = SVG_OK;
    if (referenced) {
        status = geometry_value(a, parent, GARB_WIDTH, a->width, NAN, &w);
        if (status == SVG_OK)
            status = geometry_value(a, parent, GARB_HEIGHT, a->height, NAN, &h);
    }
    if (status == SVG_OK && isnan(w))
        status = geometry_value(a, r->instance, GARB_WIDTH, a->width, a->width, &w);
    if (status == SVG_OK && isnan(h))
        status = geometry_value(a, r->instance, GARB_HEIGHT, a->height, a->height, &h);
    if (status == SVG_OK)
        status = geometry_value(a, r->instance, GARB_X, a->width, 0, &x);
    if (status == SVG_OK)
        status = geometry_value(a, r->instance, GARB_Y, a->height, 0, &y);
    if (status != SVG_OK)
        return status;
    if (w <= 0 || h <= 0) {
        out->display = false;
        return SVG_OK;
    }
    Map content;
    double uw, uh;
    status = viewport_map(n, w, h, &uw, &uh, &content);
    if (status == SVG_PENDING) {
        out->display = false;
        return SVG_OK;
    }
    if (status != SVG_OK)
        return status;
    // Scroll/auto require a scrolling or visibility policy beyond this static profile.
    if (c->overflow[0] == 2 || c->overflow[0] == 3 || c->overflow[1] == 2 || c->overflow[1] == 3)
        return SVG_UNSUPPORTED;
    int overflow[2] = {c->overflow[0], c->overflow[1]};
    for (int i = 0; i < 2; ++i)
        if (c->overflow[1 - i] != 0 && c->overflow[1 - i] != 4) {
            if (overflow[i] == 0)
                overflow[i] = 3;
            if (overflow[i] == 4)
                overflow[i] = 1;
        }
    if ((overflow[0] != 0) != (overflow[1] != 0))
        return SVG_UNSUPPORTED; // One-axis clip/visible needs an unbounded strip mask.
    out->viewport = (svg_viewport_t){true, overflow[0] != 0, w, h, content};
    out->transform = multiply(out->transform, (Map){1, 0, 0, 1, x, y});
    if (!finite_map(out->transform))
        return SVG_LIMIT;
    a->instances[r->instance].width = uw;
    a->instances[r->instance].height = uh;
    return SVG_OK;
}
static svg_status_t line_coordinate(Adapter *a, const os64_html_node_t *n, const char *name,
                                    double percent, double *out)
{
    Scanner s;
    *out = 0;
    if (!scanner(attr(n, name), &s))
        return SVG_LIMIT;
    if (!adapter_spend(a, s.left))
        return a->stopped;
    double parsed;
    if (!number(&s, &parsed))
        return SVG_OK; // Invalid coordinates use the attribute's initial zero.
    if (s.left && *s.p == '%') {
        if (a->clip_geometry)
            return SVG_UNSUPPORTED;
        parsed *= percent / 100;
        ++s.p;
        --s.left;
    } else if (s.left >= 2 && s.p[0] == 'p' && s.p[1] == 'x') {
        s.p += 2;
        s.left -= 2;
    }
    ws(&s);
    if (s.left)
        return SVG_UNSUPPORTED;
    if (!isfinite(parsed))
        return SVG_LIMIT;
    *out = parsed;
    return SVG_OK;
}
static svg_status_t shape(void *ctx, void *snapshot, const svg_style_request_t *r, svg_shape_t *out)
{
    Adapter *a = ctx;
    (void)snapshot;
    const os64_html_node_t *n = (const void *)(uintptr_t)r->source;
    *out = (svg_shape_t){0};
    if (same_word(n->name, "polyline") || same_word(n->name, "polygon")) {
        out->kind = same_word(n->name, "polygon") ? SVG_SHAPE_POLYGON : SVG_SHAPE_POLYLINE;
        const char *points = attr(n, "points");
        size_t len = 0;
        while (len <= 1048576 && points[len])
            ++len;
        if (len > 1048576)
            return SVG_LIMIT;
        out->points = (svg_string_t){points, len};
        return SVG_OK;
    }
    if (same_word(n->name, "line")) {
        out->kind = SVG_SHAPE_LINE;
        const char *names[] = {"x1", "y1", "x2", "y2"};
        double *coords[] = {&out->x, &out->y, &out->x2, &out->y2};
        for (size_t i = 0; i < 4; ++i) {
            svg_status_t status =
                line_coordinate(a, n, names[i], i % 2 ? a->height : a->width, coords[i]);
            if (status != SVG_OK)
                return status;
        }
        return SVG_OK;
    }
    bool rect = same_word(n->name, "rect"), circle = same_word(n->name, "circle");
    out->kind = rect ? SVG_SHAPE_RECT : SVG_SHAPE_ELLIPSE;
    const garb_prop_t props[] = {rect ? GARB_X : GARB_CX,
                                 rect ? GARB_Y : GARB_CY,
                                 GARB_WIDTH,
                                 GARB_HEIGHT,
                                 GARB_RX,
                                 GARB_RY,
                                 GARB_R};
    double *coords[] = {&out->x, &out->y, &out->width, &out->height, &out->rx, &out->ry, &out->rx};
    for (size_t i = 0; i < 7; ++i) {
        if ((!rect && (i == 2 || i == 3)) || (circle && (i == 4 || i == 5)) || (!circle && i == 6))
            continue;
        double percent = i == 6  ? hypot(a->width, a->height) / sqrt(2)
                         : i % 2 ? a->height
                                 : a->width;
        svg_status_t status =
            geometry_value(a, r->instance, props[i], percent, i == 4 || i == 5 ? -1 : 0, coords[i]);
        if (status != SVG_OK)
            return status;
    }
    if (circle)
        out->ry = out->rx;
    // SVG2 auto copies the other used radius. Both auto gives zero.
    if (out->rx < 0)
        out->rx = fmax(0, out->ry);
    if (out->ry < 0)
        out->ry = out->rx;
    return SVG_OK;
}
static svg_status_t node(void *ctx, void *snapshot, uint64_t source, svg_node_t *out)
{
    Adapter *a = ctx;
    (void)snapshot;
    const os64_html_node_t *n = (const void *)(uintptr_t)source;
    bool group =
        n->ns == OS64_HTML_NS_SVG &&
        (same_word(n->name, "g") || same_word(n->name, "svg") || same_word(n->name, "symbol"));
    out->kind = group                                                     ? SVG_NODE_GROUP
                : n->ns == OS64_HTML_NS_SVG && same_word(n->name, "path") ? SVG_NODE_PATH
                : n->ns == OS64_HTML_NS_SVG && same_word(n->name, "use")  ? SVG_NODE_USE
                : basic_shape(n)                                          ? SVG_NODE_SHAPE
                                                                          : SVG_NODE_UNSUPPORTED;
    out->first_child = group ? (uintptr_t)element(a, n->first_child) : 0;
    out->next_sibling = (uintptr_t)element(a, n->next);
    return a->stopped;
}
static svg_status_t gradient_paint(Adapter *, const garb_val_t *, uint32_t, unsigned,
                                   svg_paint_t *);
static svg_status_t style(void *ctx, void *snapshot, const svg_style_request_t *r, svg_style_t *out,
                          svg_string_t *d)
{
    Adapter *a = ctx;
    (void)snapshot;
    const os64_html_node_t *n = (const void *)(uintptr_t)r->source;
    const os64_html_node_t *parent =
        r->parent_instance ? a->instances[r->parent_instance].source : NULL;
    bool symbol = n->ns == OS64_HTML_NS_SVG && same_word(n->name, "symbol");
    if (image_inert(a, n) || ignored(n) ||
        (symbol && !(parent && parent->ns == OS64_HTML_NS_SVG && same_word(parent->name, "use")))) {
        *out = svg_initial_style();
        out->display = false;
        return SVG_OK;
    }
    svg_status_t status = remember(a, r);
    if (status != SVG_OK)
        return status;
    a->width = a->instances[r->instance].width;
    a->height = a->instances[r->instance].height;
    Computed c;
    status = compute(a, r->instance, &c);
    if (status != SVG_OK)
        return status;
    *out = c.s;
    // Inline roots leave their outer box effects to flow. Image roots own
    // these effects, including opacity of the complete image contents.
    bool inline_root = n == a->root && a->mode == SVG_MODE_INLINE;
    if (inline_root) {
        out->opacity = 1;
        out->transform = identity;
    }
    if (!out->display)
        return SVG_OK;
    if (!inline_root && c.opacity_css) {
        double opacity;
        if (!value(c.opacity_css, 1, &opacity))
            return SVG_UNSUPPORTED;
        out->opacity = fmax(0, fmin(1, opacity));
    }
    if (!inline_root && c.transform_css) {
        status = css_transform(c.transform_css, &out->transform);
        if (status != SVG_OK)
            return status;
    }
    if (n == a->root && a->mode == SVG_MODE_IMAGE) {
        // The standalone root's default transform origin is its viewport
        // center, outside viewBox. Explicit origins require a separate resolver.
        double x = a->image_viewport.width / 2, y = a->image_viewport.height / 2;
        out->transform = multiply((Map){1, 0, 0, 1, x, y},
                                  multiply(out->transform, (Map){1, 0, 0, 1, -x, -y}));
        out->viewport = a->image_viewport;
    }
    if (n->ns == OS64_HTML_NS_SVG && same_word(n->name, "use")) {
        double x, y;
        status = geometry_value(a, r->instance, GARB_X, a->width, 0, &x);
        if (status == SVG_OK)
            status = geometry_value(a, r->instance, GARB_Y, a->height, 0, &y);
        if (status != SVG_OK)
            return status;
        out->transform = multiply(out->transform, (Map){1, 0, 0, 1, x, y});
        if (!finite_map(out->transform))
            return SVG_LIMIT;
    }
    if (n != a->root && n->ns == OS64_HTML_NS_SVG && (symbol || same_word(n->name, "svg"))) {
        status = nested_viewport(a, r, &c, out);
        if (status != SVG_OK || !out->display)
            return status;
    }
    a->instances[r->instance].clip_css = c.clip_css;
    bool path = n->ns == OS64_HTML_NS_SVG && same_word(n->name, "path");
    bool geometry = path || basic_shape(n);
    if (geometry && out->visible && out->stroke.kind != SVG_PAINT_NONE) {
        if (c.vector)
            return SVG_UNSUPPORTED;
        double percent = hypot(a->width, a->height) / sqrt(2);
        if (c.dash_css && !keyword(c.dash_css, "none")) {
            if (c.dash_css->nitems <= 0 || c.dash_css->nitems > (int32_t)SVG_DASH_MAX)
                return SVG_UNSUPPORTED;
            bool nonzero = false;
            for (int32_t i = 0; i < c.dash_css->nitems; ++i) {
                if (!value(&c.dash_css->items[i], percent, &a->dash_values[i]))
                    return SVG_UNSUPPORTED;
                nonzero |= a->dash_values[i] > 0;
            }
            if (nonzero) {
                out->dash.values = a->dash_values;
                out->dash.count = (size_t)c.dash_css->nitems;
                // Calibrated path lengths need their own metric contract.
                // Refuse an active calibration rather than silently ignoring it.
                if (*attr(n, "pathLength"))
                    return SVG_UNSUPPORTED;
            }
        }
        if (out->dash.count && c.dashoffset_css &&
            !value(c.dashoffset_css, percent, &out->dash.offset))
            return SVG_UNSUPPORTED;
    }
    if (geometry && out->visible) {
        status = gradient_paint(a, c.fill_css, c.color, 0, &out->fill);
        if (status == SVG_OK)
            status = gradient_paint(a, c.stroke_css, c.color, 1, &out->stroke);
        if (status != SVG_OK)
            return status;
    }
    if (c.linear)
        return SVG_UNSUPPORTED;
    // Features absent from the prepared-scene profile must not masquerade as
    // ordinary solid artwork. CSS declarations not read by garb are also
    // recorded by the page's existing skipped-style diagnostics.
    const char *unsupported[] = {"mask",       "filter",     "marker-start",
                                 "marker-mid", "marker-end", "paint-order"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        const char *v = attr(n, unsupported[i]);
        if (*v && !same_word(v, "none") && !same_word(v, "normal"))
            return SVG_UNSUPPORTED;
    }
    if (!inline_root) {
        const garb_prop_t unsupported_props[] = {GARB_TRANSFORM_ORIGIN, GARB_TRANSLATE, GARB_ROTATE,
                                                 GARB_SCALE};
        for (size_t i = 0; i < sizeof(unsupported_props) / sizeof(unsupported_props[0]); ++i)
            if (winner(a->cascade, n, unsupported_props[i]))
                return SVG_UNSUPPORTED;
    }
    // Hidden shapes still supply geometry for bounds, but their unresolved
    // paint URLs must not reach the scene's supported-paint validation.
    if (!geometry || !out->visible) {
        out->fill.kind = SVG_PAINT_NONE;
        out->stroke.kind = SVG_PAINT_NONE;
    }
    if (basic_shape(n) && same_word(n->name, "line"))
        out->fill.kind = SVG_PAINT_NONE;
    const char *data = path ? attr(n, "d") : "";
    size_t len = 0;
    while (len <= 1048576 && data[len])
        ++len;
    if (len > 1048576)
        return SVG_LIMIT;
    *d = (svg_string_t){data, len};
    return SVG_OK;
}
static int hex(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static svg_status_t fragment_target(Adapter *a, const char *text, uint64_t *source)
{
    Scanner scan;
    *source = 0;
    if (!scanner(text ? text : "", &scan))
        return SVG_LIMIT;
    ws(&scan);
    while (scan.left && space(scan.p[scan.left - 1]))
        --scan.left;
    if (!scan.left)
        return SVG_OK;
    if (*scan.p != '#')
        // Image mode treats blocked external references as failed resources.
        // Embedded data SVG targets are outside this profile too; no resolver.
        return a->mode == SVG_MODE_IMAGE ? SVG_OK : SVG_UNSUPPORTED;
    char id[SVG_ATTR_MAX + 1];
    size_t len = 0;
    for (size_t i = 1; i < scan.left; ++i) {
        char c = scan.p[i];
        if (c == '%' && i + 2 < scan.left && hex(scan.p[i + 1]) >= 0 && hex(scan.p[i + 2]) >= 0) {
            c = (char)(hex(scan.p[i + 1]) * 16 + hex(scan.p[i + 2]));
            i += 2;
        }
        if (!c)
            return SVG_OK;
        id[len++] = c;
    }
    id[len] = 0;
    if (!len)
        return SVG_OK;
    // Search document order, including other SVG roots. Template contents are
    // separate trees. The first ID wins even when it names a non-SVG element.
    const os64_html_node_t *n = a->doc->document, *target = NULL;
    while (n) {
        if (!adapter_spend(a, 1))
            return a->stopped;
        if (n->kind == OS64_HTML_ELEMENT) {
            // Fragment-only lookup requires a document-local base. A URL
            // resolver is needed before base-changing documents can use it.
            if (!a->base_checked &&
                ((n->ns == OS64_HTML_NS_HTML && same_word(n->name, "base") && *attr(n, "href")) ||
                 *attr(n, "xml:base")))
                return SVG_UNSUPPORTED;
            const char *candidate = attr(n, "id");
            size_t i = 0;
            while (i < len && candidate[i] && candidate[i] == id[i])
                ++i;
            if (!adapter_spend(a, i))
                return a->stopped;
            if (!target && i == len && !candidate[i]) {
                target = n;
                if (a->base_checked)
                    break;
            }
        }
        if (n->first_child)
            n = n->first_child;
        else {
            while (n && !n->next) {
                if (!adapter_spend(a, 1))
                    return a->stopped;
                n = n->parent;
            }
            if (n)
                n = n->next;
        }
    }
    a->base_checked = true;
    if (!target || target->ns != OS64_HTML_NS_SVG)
        return SVG_OK;
    *source = (uintptr_t)target;
    return SVG_OK;
}
static svg_status_t href_target(Adapter *a, const os64_html_node_t *use, uint64_t *source)
{
    const os64_html_attr_t *href = os64_html_attr(use, "href");
    // An unqualified href, including an empty one, precedes XLink href.
    if (!href) {
        for (const os64_html_attr_t *v = use->attrs; v; v = v->next) {
            if (!adapter_spend(a, 1))
                return a->stopped;
            if (!same_word(v->ns, "http://www.w3.org/1999/xlink"))
                continue;
            const char *local = v->name;
            for (const char *p = v->name; *p; ++p)
                if (*p == ':')
                    local = p + 1;
            if (same_word(local, "href")) {
                href = v;
                break;
            }
        }
    }
    return fragment_target(a, href ? href->value : "", source);
}
static svg_status_t reference(void *ctx, void *snapshot, const svg_style_request_t *r,
                              uint64_t *source)
{
    Adapter *a = ctx;
    (void)snapshot;
    const os64_html_node_t *use = (const void *)(uintptr_t)r->source;
    svg_status_t status = href_target(a, use, source);
    if (status != SVG_OK || !*source)
        return status;
    size_t depth = 0;
    for (const os64_html_node_t *p = use; p; p = p->parent) {
        if (++depth > SVG_DEPTH_MAX)
            return SVG_LIMIT;
        if (!adapter_spend(a, 1))
            return a->stopped;
        if ((uintptr_t)p == *source) {
            *source = 0;
            break;
        }
    }
    return SVG_OK;
}
typedef struct {
    uint32_t color, stop;
    double opacity;
    bool current, linear;
} StopStyle;
static svg_status_t stop_style_step(Adapter *a, const os64_html_node_t *node,
                                    const StopStyle *parent, StopStyle *out)
{
    *out = (StopStyle){
        .color = parent->color, .stop = 0xff000000, .opacity = 1, .linear = parent->linear};
    const garb_prop_t props[] = {GARB_COLOR, GARB_STOP_COLOR, GARB_STOP_OPACITY,
                                 GARB_COLOR_INTERPOLATION};
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); ++i) {
        if (!adapter_spend(a, 1))
            return a->stopped;
        const garb_val_t *v = winner(a->cascade, node, props[i]);
        if (!v)
            continue;
        bool inherit =
            v->kind == GARB_V_WIDE &&
            (same_word(v->keyword, "inherit") ||
             (!same_word(v->keyword, "initial") && garb_prop_inherited(props[i])));
        if (v->kind == GARB_V_WIDE) {
            if (props[i] == GARB_COLOR)
                out->color = inherit ? parent->color : a->initial_color;
            if (props[i] == GARB_STOP_COLOR && inherit) {
                out->stop = parent->stop;
                out->current = parent->current;
            }
            if (props[i] == GARB_STOP_OPACITY && inherit)
                out->opacity = parent->opacity;
            if (props[i] == GARB_COLOR_INTERPOLATION)
                out->linear = inherit && parent->linear;
        } else if (props[i] == GARB_COLOR)
            out->color = color(v->color, parent->color);
        else if (props[i] == GARB_STOP_COLOR) {
            out->stop = color(v->color, out->color);
            out->current = v->color.current;
        } else if (props[i] == GARB_STOP_OPACITY) {
            if (!value(v, 1, &out->opacity))
                return SVG_UNSUPPORTED;
            out->opacity = fmin(1, fmax(0, out->opacity));
        } else
            out->linear = keyword(v, "linearrgb");
    }
    return SVG_OK;
}
static svg_status_t gradient_style(Adapter *a, const os64_html_node_t *node, StopStyle *out)
{
    const os64_html_node_t *chain[SVG_DEPTH_MAX];
    size_t depth = 0;
    for (; node && node->kind == OS64_HTML_ELEMENT; node = node->parent) {
        if (depth == SVG_DEPTH_MAX)
            return SVG_LIMIT;
        chain[depth++] = node;
    }
    *out = (StopStyle){.color = a->initial_color, .stop = 0xff000000, .opacity = 1};
    while (depth) {
        StopStyle parent = *out;
        svg_status_t st = stop_style_step(a, chain[--depth], &parent, out);
        if (st != SVG_OK)
            return st;
    }
    return SVG_OK;
}
static svg_status_t gradient_number(Adapter *a, const char *text, double percent, bool offset,
                                    double *out)
{
    Scanner s;
    if (!scanner(text, &s))
        return SVG_LIMIT;
    if (!adapter_spend(a, s.left))
        return a->stopped;
    double n;
    if (!number(&s, &n))
        return SVG_MALFORMED;
    double factor = 1;
    if (s.left && *s.p == '%') {
        factor = percent / 100;
        ++s.p;
        --s.left;
    } else if (!offset && s.left && !space(*s.p)) {
        const struct {
            const char *unit;
            double factor;
        } units[] = {{"px", 1},         {"in", 96},        {"cm", 96 / 2.54}, {"mm", 96 / 25.4},
                     {"Q", 96 / 101.6}, {"pt", 96 / 72.0}, {"pc", 16}};
        bool matched = false;
        for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); ++i) {
            size_t len = os64_strlen(units[i].unit);
            if (s.left >= len && !os64_memcmp(s.p, units[i].unit, len)) {
                factor = units[i].factor;
                s.p += len;
                s.left -= len;
                matched = true;
                break;
            }
        }
        if (!matched)
            return SVG_UNSUPPORTED;
    }
    ws(&s);
    if (s.left)
        return SVG_MALFORMED;
    *out = n * factor;
    return isfinite(*out) ? SVG_OK : SVG_LIMIT;
}
static svg_status_t gradient_paint(Adapter *a, const garb_val_t *paint, uint32_t ink, unsigned part,
                                   svg_paint_t *out)
{
    if (!paint || paint->kind != GARB_V_URL)
        return SVG_OK;
    *out = (svg_paint_t){.kind = SVG_PAINT_NONE};
    if (paint->nitems && paint->items[0].kind == GARB_V_COLOR)
        *out = (svg_paint_t){.kind = SVG_PAINT_COLOR, .argb = color(paint->items[0].color, ink)};
    if (paint->len > SVG_ATTR_MAX)
        return SVG_LIMIT;
    char url[SVG_ATTR_MAX + 1];
    os64_memcpy(url, paint->text, paint->len);
    url[paint->len] = 0;
    uint64_t target = 0;
    svg_status_t status = fragment_target(a, url, &target);
    if (status != SVG_OK || !target)
        return status;
    const os64_html_node_t *resource = (const void *)(uintptr_t)target;
    if (same_word(resource->name, "pattern"))
        return SVG_UNSUPPORTED;
    bool radial = same_word(resource->name, "radialGradient");
    if (!radial && !same_word(resource->name, "linearGradient"))
        return SVG_OK;
    const char *names[] = {"gradientUnits",
                           "spreadMethod",
                           "gradientTransform",
                           "x1",
                           "y1",
                           "x2",
                           "y2",
                           "cx",
                           "cy",
                           "r",
                           "fx",
                           "fy",
                           "fr"};
    const char *attrs[13] = {0};
    const os64_html_node_t *seen[SVG_DEPTH_MAX], *stops = NULL, *node = resource;
    size_t depth = 0;
    while (node) {
        if (!adapter_spend(a, 1))
            return a->stopped;
        if (depth == SVG_DEPTH_MAX)
            return SVG_LIMIT;
        if (!adapter_spend(a, depth))
            return a->stopped;
        for (size_t i = 0; i < depth; ++i)
            if (seen[i] == node)
                return SVG_OK;
        seen[depth++] = node;
        bool template_radial = same_word(node->name, "radialGradient");
        for (size_t i = 0; i < 13; ++i) {
            // Different gradient types share stop content and common attributes,
            // but geometric attributes belong to their own element type.
            if (i >= 3 && (i >= 7) != template_radial)
                continue;
            const os64_html_attr_t *v = os64_html_attr(node, names[i]);
            if (!attrs[i] && v && !v->ns)
                attrs[i] = v->value;
        }
        if (!stops) {
            for (const os64_html_node_t *child = node->first_child; child; child = child->next) {
                if (!adapter_spend(a, 1))
                    return a->stopped;
                if (child->kind != OS64_HTML_ELEMENT)
                    continue;
                if (child->ns == OS64_HTML_NS_SVG && same_word(child->name, "stop")) {
                    stops = node;
                    break;
                }
                if (!image_inert(a, child) && !ignored(child))
                    return SVG_UNSUPPORTED;
            }
        }
        uint64_t next = 0;
        status = href_target(a, node, &next);
        if (status != SVG_OK)
            return status;
        node = (const void *)(uintptr_t)next;
        if (node && !same_word(node->name, "linearGradient") &&
            !same_word(node->name, "radialGradient"))
            node = NULL;
    }
    svg_linear_gradient_t *g = &a->gradients[part];
    *g = (svg_linear_gradient_t){
        .x2 = 1, .transform = identity, .object_bbox = true, .stops = a->gradient_stops[part]};
    if (attrs[0]) {
        if (same_word(attrs[0], "userSpaceOnUse"))
            g->object_bbox = false;
        else if (!same_word(attrs[0], "objectBoundingBox"))
            return SVG_UNSUPPORTED;
    }
    if (attrs[1]) {
        if (same_word(attrs[1], "repeat"))
            g->spread = SVG_SPREAD_REPEAT;
        else if (same_word(attrs[1], "reflect"))
            g->spread = SVG_SPREAD_REFLECT;
        else if (!same_word(attrs[1], "pad"))
            return SVG_UNSUPPORTED;
    }
    svg_radial_gradient_t *rg = &a->radial_gradients[part];
    *rg = (svg_radial_gradient_t){0};
    double *coords[] = {&g->x1,  &g->y1, &g->x2,  &g->y2,  &rg->cx,
                        &rg->cy, &rg->r, &rg->fx, &rg->fy, &rg->fr};
    const char *defaults[] = {"0%", "0%", "100%", "0%", "50%", "50%", "50%", NULL, NULL, "0%"};
    for (size_t i = radial ? 4 : 0; i < (radial ? 10 : 4); ++i) {
        const char *text = attrs[i + 3] ? attrs[i + 3] : defaults[i];
        if (!text) {
            *coords[i] = i == 7 ? rg->cx : rg->cy;
            continue;
        }
        bool radius = i == 6 || i == 9;
        bool vertical = i == 1 || i == 3 || i == 5 || i == 8;
        double percent = g->object_bbox ? 1
                         : radius       ? hypot(a->width, a->height) / sqrt(2)
                         : vertical     ? a->height
                                        : a->width;
        status = gradient_number(a, text, percent, false, coords[i]);
        if (status != SVG_OK)
            return status;
        if (radius && *coords[i] < 0)
            return SVG_MALFORMED;
    }
    const garb_val_t *transform = winner(a->cascade, resource, GARB_TRANSFORM);
    if (transform)
        status = css_transform(transform, &g->transform);
    else if (attrs[2]) {
        Scanner scan;
        if (!scanner(attrs[2], &scan))
            return SVG_LIMIT;
        if (!adapter_spend(a, scan.left))
            return a->stopped;
        if (!transform_attribute(attrs[2], &g->transform))
            status = SVG_MALFORMED;
    }
    if (status != SVG_OK)
        return status;
    const garb_prop_t extra[] = {GARB_TRANSFORM_ORIGIN, GARB_TRANSLATE, GARB_ROTATE, GARB_SCALE};
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
        if (winner(a->cascade, resource, extra[i]))
            return SVG_UNSUPPORTED;
    StopStyle host;
    status = gradient_style(a, resource, &host);
    if (status != SVG_OK)
        return status;
    if (host.linear)
        return SVG_UNSUPPORTED;
    if (stops)
        for (const os64_html_node_t *child = stops->first_child; child; child = child->next) {
            if (!adapter_spend(a, 1))
                return a->stopped;
            if (child->kind != OS64_HTML_ELEMENT)
                continue;
            if (child->ns != OS64_HTML_NS_SVG || !same_word(child->name, "stop")) {
                if (image_inert(a, child) || ignored(child))
                    continue;
                return SVG_UNSUPPORTED;
            }
            if (g->count == SVG_GRADIENT_STOPS_MAX)
                return SVG_LIMIT;
            if (stops != resource && garb_style_needs_substitution(a->cascade, child))
                return SVG_UNSUPPORTED;
            StopStyle stop;
            status = stop_style_step(a, child, &host, &stop);
            if (status != SVG_OK)
                return status;
            double offset = 0;
            const char *text = attr(child, "offset");
            if (*text)
                status = gradient_number(a, text, 1, true, &offset);
            if (status != SVG_OK)
                return status;
            offset = fmin(1, fmax(g->count ? g->stops[g->count - 1].offset : 0, offset));
            uint32_t rgba = stop.current ? stop.color : stop.stop;
            rgba = (rgba & 0xffffff) | (uint32_t)((rgba >> 24) * stop.opacity + .5) << 24;
            a->gradient_stops[part][g->count++] = (svg_gradient_stop_t){offset, rgba};
        }
    if (radial) {
        rg->transform = g->transform;
        rg->object_bbox = g->object_bbox;
        rg->spread = g->spread;
        rg->stops = g->stops;
        rg->count = g->count;
        *out = (svg_paint_t){.kind = SVG_PAINT_RADIAL, .radial = rg};
    } else
        *out = (svg_paint_t){.kind = SVG_PAINT_LINEAR, .linear = g};
    return SVG_OK;
}
// A clip source inherits through its own DOM ancestors. Temporarily using the
// consumer's record avoids a second ancestry mechanism; restore it on every exit.
static svg_status_t clip_computed(Adapter *a, uint64_t instance, const os64_html_node_t *n,
                                  Computed *out)
{
    Instance saved = a->instances[instance];
    a->instances[instance] = (Instance){.source = n};
    a->clip_style = true;
    svg_status_t status = compute(a, instance, out);
    a->clip_style = false;
    a->instances[instance] = saved;
    if (status == SVG_OK && out->transform_css)
        status = css_transform(out->transform_css, &out->s.transform);
    if (status != SVG_OK)
        return status;
    if (out->clip_css && !keyword(out->clip_css, "none"))
        return SVG_UNSUPPORTED;
    const garb_prop_t transforms[] = {GARB_TRANSFORM_ORIGIN, GARB_TRANSLATE, GARB_ROTATE,
                                      GARB_SCALE};
    for (size_t i = 0; i < sizeof(transforms) / sizeof(transforms[0]); ++i)
        if (winner(a->cascade, n, transforms[i]))
            return SVG_UNSUPPORTED;
    return SVG_OK;
}
static svg_status_t clip_source(void *ctx, void *snapshot, const svg_style_request_t *r,
                                svg_clip_source_t *out)
{
    Adapter *a = ctx;
    (void)snapshot;
    *out = (svg_clip_source_t){0};
    a->width = a->instances[r->instance].width;
    a->height = a->instances[r->instance].height;
    svg_status_t status;
    const garb_val_t *v = a->instances[r->instance].clip_css;
    if (!v || keyword(v, "none"))
        return SVG_OK;
    if (v->kind != GARB_V_URL)
        return SVG_UNSUPPORTED;
    if (v->len > SVG_ATTR_MAX)
        return SVG_LIMIT;
    char url[SVG_ATTR_MAX + 1];
    for (size_t i = 0; i < v->len; ++i)
        url[i] = v->text[i];
    url[v->len] = 0;
    uint64_t source;
    status = fragment_target(a, url, &source);
    if (status != SVG_OK || !source)
        return status;
    const os64_html_node_t *resource = (const void *)(uintptr_t)source;
    if (!same_word(resource->name, "clipPath"))
        return SVG_OK;
    const char *units = attr(resource, "clipPathUnits");
    bool object_bbox = same_word(units, "objectBoundingBox");
    if (*units && !same_word(units, "userSpaceOnUse") && !object_bbox)
        return SVG_UNSUPPORTED;
    Computed container;
    status = clip_computed(a, r->instance, resource, &container);
    if (status != SVG_OK)
        return status;
    *out = (svg_clip_source_t){.active = true, .kind = SVG_NODE_PATH,
                               .transform = identity, .object_bbox = object_bbox,
                               .outer_transform = container.s.transform};
    bool found = false;
    for (const os64_html_node_t *n = resource->first_child; n; n = n->next) {
        if (!adapter_spend(a, 1))
            return a->stopped;
        if (n->kind != OS64_HTML_ELEMENT)
            continue;
        if (n->ns != OS64_HTML_NS_SVG)
            return SVG_UNSUPPORTED;
        // Scripts and animation do not contribute clip silhouettes in image mode.
        if (image_inert(a, n) || same_word(n->name, "title") || same_word(n->name, "desc") ||
            same_word(n->name, "metadata") || same_word(n->name, "style"))
            continue;
        Computed child;
        status = clip_computed(a, r->instance, n, &child);
        if (status != SVG_OK)
            return status;
        if (!child.s.display || !child.s.visible)
            continue;
        if (found)
            return SVG_UNSUPPORTED; // Multiple silhouettes need a geometric union.
        bool path = same_word(n->name, "path");
        if (!path && !basic_shape(n))
            return SVG_UNSUPPORTED;
        found = true;
        out->even_odd = child.clip_even_odd;
        out->transform = object_bbox ? child.s.transform
                                     : multiply(container.s.transform, child.s.transform);
        if (!finite_map(out->transform))
            return SVG_LIMIT;
        if (path) {
            const char *d = attr(n, "d");
            size_t len = 0;
            while (len <= 1048576 && d[len])
                ++len;
            if (len > 1048576)
                return SVG_LIMIT;
            out->d = (svg_string_t){d, len};
        } else {
            out->kind = SVG_NODE_SHAPE;
            Instance saved = a->instances[r->instance];
            a->instances[r->instance] = (Instance){.source = n};
            svg_style_request_t req = *r;
            req.source = (uintptr_t)n;
            a->clip_geometry = true;
            status = shape(a, snapshot, &req, &out->shape);
            a->clip_geometry = false;
            a->instances[r->instance] = saved;
            if (status != SVG_OK)
                return status;
        }
    }
    if (a->mode == SVG_MODE_IMAGE && r->source == (uintptr_t)a->root) {
        // The outer image root has a CSS viewport box. Its clip uses that box
        // and viewport units, outside viewBox; the scene consumes content units.
        Map content = a->image_viewport.content_transform;
        if (!content.a || !content.d)
            return SVG_LIMIT;
        Map inverse = {1 / content.a, 0, 0, 1 / content.d,
                       -content.tx / content.a, -content.ty / content.d};
        Map clip = out->transform;
        if (object_bbox) {
            Map box = {a->image_viewport.width, 0, 0, a->image_viewport.height, 0, 0};
            clip = multiply(out->outer_transform, multiply(box, clip));
        }
        out->transform = multiply(inverse, clip);
        out->outer_transform = identity;
        out->object_bbox = false;
        if (!finite_map(out->transform))
            return SVG_LIMIT;
    }
    return SVG_OK;
}
void yonder_svg_pixels_free(const os64_geometry_memory_t *m, yonder_svg_pixels_t *out)
{
    if (out->pixels)
        m->free(m->context, out->pixels, out->bytes);
    *out = (yonder_svg_pixels_t){0};
}
static svg_status_t render_adapter(Adapter *a, const Map *map, os64_gui_rect_t content,
                                    int64_t x, int64_t y, uint64_t width, uint64_t height,
                                    const os64_geometry_backend_t *backend,
                                    yonder_svg_pixels_t *out)
{
    svg_context_t *c = NULL;
    svg_scene_t *scene = NULL;
    svg_options_t options = {.memory = a->memory, .cancel_context = a->cancel_context,
                             .cancelled = a->cancelled, .limits = svg_default_limits()};
    options.limits.scene_nodes = SVG_VISITS_MAX;
    svg_status_t status = svg_create(&options, &c);
    if (status != SVG_OK)
        return status;
    svg_provider_t provider = {a, acquire, release, node, style, shape, reference, clip_source};
    status = svg_scene_prepare(c, &provider, a->mode, map, &scene);
    if (status == SVG_OK) {
        out->bytes = (size_t)(width * height * 4);
        void *pixels = NULL;
        os64_geometry_status_t allocation = a->memory.alloc(a->memory.context, out->bytes, &pixels);
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
        yonder_svg_pixels_free(&a->memory, out);
    return status;
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
                 .initial_color = initial_color,
                 .memory = *memory};
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
    return render_adapter(&a, &map, content, x, y, width, height, backend, out);
}

svg_status_t yonder_svg_render_image_tree(const os64_html_document_t *doc,
                                         const garb_cascade_t *cascade,
                                         const yonder_svg_image_view_t *view,
                                         const os64_geometry_memory_t *memory,
                                         const os64_geometry_backend_t *backend,
                                         yonder_svg_pixels_t *out)
{
    if (!out)
        return SVG_BAD_ARGUMENT;
    *out = (yonder_svg_pixels_t){0};
    if (!doc || !doc->is_xml || !cascade || !view || !memory || !memory->alloc || !memory->free)
        return SVG_BAD_ARGUMENT;
    const os64_html_node_t *root = os64_html_document_element(doc);
    if (!root || root->ns != OS64_HTML_NS_SVG || !same_word(root->name, "svg"))
        return SVG_UNSUPPORTED;
    if (garb_cascade_incomplete(cascade))
        return SVG_LIMIT;
    if ((!view->pixel_width) != (!view->pixel_height))
        return SVG_BAD_ARGUMENT;
    if (!isfinite(view->width) || !isfinite(view->height) ||
        view->width < 0 || view->height < 0 || view->width > 16384 || view->height > 16384)
        return SVG_LIMIT;
    uint32_t pw = view->pixel_width ? view->pixel_width : (uint32_t)ceil(view->width);
    uint32_t ph = view->pixel_height ? view->pixel_height : (uint32_t)ceil(view->height);
    if (pw > 16384 || ph > 16384 ||
        (uint64_t)pw * ph > 8388608)
        return SVG_LIMIT;
    if (!view->width || !view->height)
        return SVG_OK;
    Adapter a = {.doc = doc, .cascade = cascade, .root = root, .initial_color = 0xff000000,
                 .memory = *memory, .mode = SVG_MODE_IMAGE,
                 .cancel_context = view->cancel_context, .cancelled = view->cancelled};
    Map content_map;
    svg_status_t status = viewport(&a, view->width, view->height, 1000, &content_map);
    if (status == SVG_PENDING)
        return SVG_OK;
    if (status != SVG_OK)
        return status;
    a.image_viewport = (svg_viewport_t){.active = true, .width = view->width,
                                       .height = view->height, .content_transform = content_map};
    Map output = {(double)pw / view->width, 0, 0, (double)ph / view->height, 0, 0};
    return render_adapter(&a, &output, (os64_gui_rect_t){0, 0, pw, ph},
                          0, 0, pw, ph, backend, out);
}
