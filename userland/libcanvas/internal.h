#ifndef CANVAS_INTERNAL_H
#define CANVAS_INTERNAL_H
#include <canvas/canvas.h>
#include <math.h>
typedef struct Clip { size_t references,bytes; uint8_t pixels[]; } Clip;
typedef struct Path { os64_geometry_command_t *commands; size_t count,capacity; } Path;
typedef struct Saved {
    struct Saved *next;
    canvas_state_t state;
    Clip *clip;
} Saved;
struct canvas_context {
    canvas_options_t options;
    canvas_stats_t stats;
    draw2d_surface_t surface;
    draw2d_damage_t damage;
    canvas_state_t state;
    Saved *saved;
    Clip *clip;
    Path path;
    bool busy, origin_clean;
};

typedef struct {
    unsigned kind;
    draw2d_linear_sampler_t linear;
    draw2d_radial_sampler_t radial;
    canvas_gradient_t *gradient;
    os64_geometry_map_t inverse;
} GradientProgram;
void c_style_release(canvas_state_t *);
canvas_status_t c_gradient_prepare(canvas_gradient_t *,const os64_geometry_map_t *,GradientProgram *);
uint32_t c_gradient_sample(void *,double,double);
canvas_status_t c_enter(canvas_context_t *);
canvas_status_t c_leave(canvas_context_t *,canvas_status_t);
canvas_status_t c_allocate(canvas_context_t *,size_t,void **);
void c_release(canvas_context_t *,void *,size_t);
void c_damage_union(canvas_context_t *,draw2d_damage_t);
void c_clip_release(canvas_context_t *,Clip *);
void c_path_release(canvas_context_t *,Path *);
canvas_status_t c_rectangle(canvas_context_t *,double,double,double,double,int mode);
#endif
