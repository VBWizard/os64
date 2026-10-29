#include "internal.h"
#include <draw2d/stroke.h>
typedef os64_geometry_command_t Command;
static svg_geometry_t *geometry(Context *c)
{
    svg_geometry_t *g = svg_alloc(c, sizeof(*g));
    if (g) {
        *g = (svg_geometry_t){.owner = c};
        ++c->stats.geometries;
    }
    return g;
}
static bool emit(Context *c, svg_geometry_t *g, Command command)
{
    for (size_t i = 0; i < 3; ++i)
        if (!svg_point_valid(command.p[i])) {
            c->why = SVG_LIMIT;
            return false;
        }
    size_t limit = c->options.limits.geometry_commands;
    if (limit > OS64_GEOMETRY_COMMAND_MAX)
        limit = OS64_GEOMETRY_COMMAND_MAX;
    if (!svg_spend(c, 1))
        return false;
    g->commands = svg_grow(c, g->commands, &g->capacity, g->count, sizeof(*g->commands), limit);
    if (c->why != SVG_OK)
        return false;
    g->commands[g->count++] = command;
    return true;
}
static bool simple(Context *c, svg_geometry_t *g, os64_geometry_verb_t verb, Point p)
{
    return emit(c, g, (Command){.verb = verb, .p = {p}});
}
static bool cap(Context *c, svg_geometry_t *caps, Point p, const svg_style_t *style, const Map *map,
                double error)
{
    if (style->stroke_style.cap == OS64_GEOMETRY_BUTT)
        return true;
    double radius = style->stroke_style.width * .5;
    if (style->stroke_style.cap == OS64_GEOMETRY_SQUARE) {
        // A zero-length subpath has no direction; its effective tangent is +x.
        // Zero-length dash squares use the same user-axis convention as Chromium.
        return simple(c, caps, OS64_GEOMETRY_MOVE, (Point){p.x - radius, p.y - radius}) &&
               simple(c, caps, OS64_GEOMETRY_LINE, (Point){p.x + radius, p.y - radius}) &&
               simple(c, caps, OS64_GEOMETRY_LINE, (Point){p.x + radius, p.y + radius}) &&
               simple(c, caps, OS64_GEOMETRY_LINE, (Point){p.x - radius, p.y + radius}) &&
               simple(c, caps, OS64_GEOMETRY_CLOSE, (Point){0});
    }
    svg_shape_t shape = {.kind = SVG_SHAPE_ELLIPSE, .x = p.x, .y = p.y, .rx = radius, .ry = radius};
    svg_path_t *path = NULL;
    svg_geometry_t *circle = NULL;
    svg_parse_result_t result;
    Status status = svg_make_shape(c, &shape, &path, &result);
    if (status == SVG_OK)
        status = svg_lower(c, path, map, error, &circle);
    if (status == SVG_OK)
        for (size_t i = 0; i < circle->count; ++i)
            if (!emit(c, caps, circle->commands[i]))
                break;
    svg_free_geometry(c, circle);
    svg_free_path(c, path);
    if (status != SVG_OK && c->why == SVG_OK)
        c->why = status;
    return c->why == SVG_OK;
}

typedef struct { Context *c; svg_geometry_t *center, *caps; const svg_style_t *style;
    const Map *map; double error; } Cut;
static os64_geometry_status_t status(Context *c)
{
    return c->why==SVG_OK?OS64_GEOMETRY_OK:c->why==SVG_NO_MEMORY?OS64_GEOMETRY_NO_MEMORY:
        c->why==SVG_CANCELLED?OS64_GEOMETRY_CANCELLED:OS64_GEOMETRY_LIMIT;
}
static os64_geometry_status_t cut_spend(void *p,uint64_t n)
{ Cut *a=p; svg_spend(a->c,n);return status(a->c); }
static os64_geometry_status_t cut_grow(void *p,void **v,size_t *capacity,size_t count,size_t size,size_t limit)
{ Cut *a=p;*v=svg_grow(a->c,*v,capacity,count,size,limit);return status(a->c); }
static void cut_free(void *p,void *v) { Cut *a=p;svg_free(a->c,v); }
static os64_geometry_status_t cut_emit(void *p,bool caps,const Command *v)
{ Cut *a=p;emit(a->c,caps?a->caps:a->center,*v);return status(a->c); }
static os64_geometry_status_t cut_cap(void *p,Point point,Point tangent)
{ (void)tangent; Cut *a=p;cap(a->c,a->caps,point,a->style,a->map,a->error);return status(a->c); }
Status svg_stroke_geometry(Context *c, const svg_geometry_t *source, const Map *map, double error,
                           const svg_style_t *style, svg_geometry_t **centerline,
                           svg_geometry_t **cap_geometry)
{
    *centerline=*cap_geometry=NULL;
    Cut a={c,geometry(c),geometry(c),style,map,error};
    if (a.center && a.caps) {
        draw2d_stroke_ops_t ops={&a,c->options.limits.geometry_commands,
            cut_spend,cut_grow,cut_free,cut_emit,cut_cap};
        draw2d_stroke_style_t s={{style->dash.values,style->dash.count,style->dash.offset}};
        os64_geometry_status_t result=draw2d_stroke_cut(&ops,source->commands,source->count,map,error,&s);
        if (result!=OS64_GEOMETRY_OK && c->why==SVG_OK) c->why=SVG_LIMIT;
    }
    if(c->why==SVG_OK) { *centerline=a.center;*cap_geometry=a.caps; }
    else { svg_free_geometry(c,a.center);svg_free_geometry(c,a.caps); }
    return c->why;
}
