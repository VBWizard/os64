#include "internal.h"
struct canvas_gradient {
    os64_geometry_memory_t memory;
    size_t references,count;
    unsigned kind;
    double coordinates[6];
    draw2d_gradient_stop_t stops[DRAW2D_GRADIENT_STOPS_MAX];
};
canvas_status_t canvas_gradient_create(os64_geometry_memory_t memory,unsigned kind,const double *v,canvas_gradient_t **out)
{
    if(!out)return CANVAS_BAD_ARGUMENT;
    *out=NULL;
    if(!memory.alloc||!memory.free||!v||kind>2)return CANVAS_BAD_ARGUMENT;
    unsigned n=kind==0?4:kind==1?6:3;
    for(unsigned i=0;i<n;++i)if(!isfinite(v[i]))return CANVAS_BAD_ARGUMENT;
    if(kind==1&&(v[2]<0||v[5]<0))return CANVAS_BAD_ARGUMENT;
    void *storage=NULL;os64_geometry_status_t s=memory.alloc(memory.context,sizeof(canvas_gradient_t),&storage);
    if(s!=OS64_GEOMETRY_OK||!storage)return s==OS64_GEOMETRY_LIMIT?CANVAS_LIMIT:CANVAS_NO_MEMORY;
    canvas_gradient_t *g=storage;*g=(canvas_gradient_t){.memory=memory,.references=1,.kind=kind};
    for(unsigned i=0;i<n;++i)g->coordinates[i]=v[i];
    *out=g;return CANVAS_OK;
}
void canvas_gradient_retain(canvas_gradient_t *g){if(g)++g->references;}
void canvas_gradient_release(canvas_gradient_t *g)
{ if(g&&!--g->references)g->memory.free(g->memory.context,g,sizeof(*g)); }
void c_style_release(canvas_state_t *s)
{ canvas_gradient_release(s->fill_gradient);canvas_gradient_release(s->stroke_gradient); }
canvas_status_t canvas_gradient_add_stop(canvas_gradient_t *g,double offset,uint32_t color)
{
    if(!g||!isfinite(offset)||offset<0||offset>1)return CANVAS_BAD_ARGUMENT;
    if(g->count==DRAW2D_GRADIENT_STOPS_MAX)return CANVAS_LIMIT;
    size_t at=g->count;
    while(at&&g->stops[at-1].offset>offset){g->stops[at]=g->stops[at-1];--at;}
    g->stops[at]=(draw2d_gradient_stop_t){offset,color};++g->count;return CANVAS_OK;
}
canvas_status_t canvas_set_gradient(canvas_context_t *c,canvas_gradient_t *g,bool stroke)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    canvas_gradient_t **slot=stroke?&c->state.stroke_gradient:&c->state.fill_gradient;
    canvas_gradient_retain(g);canvas_gradient_release(*slot);*slot=g;
    return c_leave(c,CANVAS_OK);
}
canvas_status_t c_gradient_prepare(canvas_gradient_t *g,const os64_geometry_map_t *m,GradientProgram *p)
{
    *p=(GradientProgram){.kind=g->kind,.gradient=g};
    const double *v=g->coordinates;
    draw2d_status_t s=DRAW2D_OK;
    if(g->kind==0) {
        if(v[0]==v[2]&&v[1]==v[3])return CANVAS_OK;
        draw2d_linear_gradient_t d={.x1=v[0],.y1=v[1],.x2=v[2],.y2=v[3],
            .transform={1,0,0,1,0,0},.stops=g->stops,.count=g->count};
        s=draw2d_linear_gradient_prepare(&d,m,NULL,&p->linear);
    } else if(g->kind==1) {
        draw2d_radial_gradient_t d={.fx=v[0],.fy=v[1],.fr=v[2],.cx=v[3],.cy=v[4],.r=v[5],
            .transform={1,0,0,1,0,0},.stops=g->stops,.count=g->count};
        s=draw2d_radial_gradient_prepare(&d,m,NULL,&p->radial);
    } else {
        double det=m->a*m->d-m->b*m->c;
        if(!isfinite(det))return CANVAS_LIMIT;
        if(!det){p->gradient=NULL;return CANVAS_OK;}
        p->inverse=(os64_geometry_map_t){m->d/det,-m->b/det,-m->c/det,m->a/det,0,0};
        p->inverse.tx=-p->inverse.a*m->tx-p->inverse.c*m->ty;
        p->inverse.ty=-p->inverse.b*m->tx-p->inverse.d*m->ty;
        const double values[]={p->inverse.a,p->inverse.b,p->inverse.c,p->inverse.d,p->inverse.tx,p->inverse.ty};
        for(unsigned i=0;i<6;++i)if(!isfinite(values[i]))return CANVAS_LIMIT;
    }
    return s==DRAW2D_OK?CANVAS_OK:s==DRAW2D_LIMIT?CANVAS_LIMIT:CANVAS_BAD_ARGUMENT;
}
uint32_t c_gradient_sample(void *opaque,double x,double y)
{
    GradientProgram *p=opaque;
    if(p->kind==0)return draw2d_linear_gradient_sample(&p->linear,x,y);
    if(p->kind==1)return draw2d_radial_gradient_sample(&p->radial,x,y);
    canvas_gradient_t *g=p->gradient;if(!g)return 0;
    const os64_geometry_map_t *m=&p->inverse;
    double px=m->a*x+m->c*y+m->tx-g->coordinates[1],py=m->b*x+m->d*y+m->ty-g->coordinates[2];
    double t=fmod(atan2(py,px)-fmod(g->coordinates[0],6.283185307179586),6.283185307179586)/6.283185307179586;
    if(t<0)t+=1;
    return draw2d_gradient_stops_sample(g->stops,g->count,DRAW2D_SPREAD_PAD,t);
}
