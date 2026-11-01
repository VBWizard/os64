#include "internal.h"
#include <draw2d/stroke.h>
#include <float.h>

#define PI 3.14159265358979323846264338327950288
static const os64_geometry_map_t identity = {1,0,0,1,0,0};
typedef os64_geometry_point_t Point;
typedef os64_geometry_command_t Command;
typedef os64_geometry_map_t Map;

void c_clip_release(canvas_context_t *c, Clip *clip)
{
    if (clip && --clip->references == 0) c_release(c, clip, clip->bytes);
}
void c_path_release(canvas_context_t *c, Path *p)
{
    c_release(c, p->commands, p->capacity * sizeof(Command));
    *p = (Path){0};
}
static bool finite_map(const Map *m)
{
    return isfinite(m->a) && isfinite(m->b) && isfinite(m->c) && isfinite(m->d) &&
           isfinite(m->tx) && isfinite(m->ty);
}
static Point mapped(const Map *m, Point p)
{ return (Point){m->a*p.x + m->c*p.y + m->tx, m->b*p.x + m->d*p.y + m->ty}; }
static bool same(Point a, Point b) { return a.x == b.x && a.y == b.y; }
static unsigned points(Command v)
{ return v.verb == OS64_GEOMETRY_CLOSE ? 0 : v.verb == OS64_GEOMETRY_CUBIC ? 3 : v.verb == OS64_GEOMETRY_QUADRATIC ? 2 : 1; }
static Point current(const Path *p)
{
    Command v = p->commands[p->count-1];
    return v.p[points(v)-1]; /* Builders leave an explicit MOVE after CLOSE. */
}
static canvas_status_t reserve(canvas_context_t *c, Path *p, size_t extra)
{
    size_t limit = c->options.max_commands < OS64_GEOMETRY_COMMAND_MAX ?
        c->options.max_commands : OS64_GEOMETRY_COMMAND_MAX;
    if (p->count > limit || extra > limit-p->count) return CANVAS_LIMIT;
    size_t n = p->count + extra;
    if (n <= p->capacity) return CANVAS_OK;
    size_t capacity = p->capacity * 2;
    if (capacity < 8) capacity = 8;
    if (capacity < n) capacity = n;
    if (capacity > limit) capacity = limit;
    Command *commands;
    canvas_status_t status = c_allocate(c, capacity*sizeof(*commands), (void **)&commands);
    if (status != CANVAS_OK) return status;
    for (size_t i=0;i<p->count;++i) commands[i]=p->commands[i];
    c_release(c,p->commands,p->capacity*sizeof(*commands));
    p->commands=commands;p->capacity=capacity;
    return CANVAS_OK;
}
static canvas_status_t append(canvas_context_t *c,Path *p,Command command,const Map *map)
{
    if (command.verb < OS64_GEOMETRY_MOVE || command.verb > OS64_GEOMETRY_CLOSE) return CANVAS_BAD_ARGUMENT;
    unsigned n=points(command);
    for(unsigned i=0;i<n;++i) {
        if(!isfinite(command.p[i].x)||!isfinite(command.p[i].y)) return CANVAS_OK;
    }
    for(unsigned i=0;i<n;++i) {
        command.p[i]=mapped(map,command.p[i]);
        if(!isfinite(command.p[i].x)||!isfinite(command.p[i].y)) return CANVAS_LIMIT;
    }
    if(command.verb==OS64_GEOMETRY_CLOSE) {
        if(!p->count || p->commands[p->count-1].verb==OS64_GEOMETRY_MOVE) return CANVAS_OK;
        size_t start=p->count-1;
        while(start && p->commands[start].verb!=OS64_GEOMETRY_MOVE)--start;
        canvas_status_t status=reserve(c,p,2);
        if(status!=CANVAS_OK)return status;
        p->commands[p->count++]=command;
        p->commands[p->count++]=(Command){.verb=OS64_GEOMETRY_MOVE,.p={p->commands[start].p[0]}};
        return CANVAS_OK;
    }
    bool start=!p->count && command.verb!=OS64_GEOMETRY_MOVE;
    canvas_status_t status=reserve(c,p,start?2:1);
    if(status!=CANVAS_OK)return status;
    if(start) {
        p->commands[p->count++]=(Command){.verb=OS64_GEOMETRY_MOVE,.p={command.p[0]}};
        if(command.verb==OS64_GEOMETRY_LINE)return CANVAS_OK;
    }
    p->commands[p->count++]=command;
    return CANVAS_OK;
}
canvas_status_t canvas_begin_path(canvas_context_t *c)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    c->path.count=0;return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_path_command(canvas_context_t *c,const Command *v)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    return c_leave(c,v?append(c,&c->path,*v,&c->state.transform):CANVAS_BAD_ARGUMENT);
}
static canvas_status_t rectangle_path(canvas_context_t *c,Path *p,double x,double y,double w,double h)
{
    if(!isfinite(x)||!isfinite(y)||!isfinite(w)||!isfinite(h))return CANVAS_OK;
    Command v[6]={
        {.verb=OS64_GEOMETRY_MOVE,.p={{x,y}}},
        {.verb=OS64_GEOMETRY_LINE,.p={{x+w,y}}},
        {.verb=OS64_GEOMETRY_LINE,.p={{x+w,y+h}}},
        {.verb=OS64_GEOMETRY_LINE,.p={{x,y+h}}},
        {.verb=OS64_GEOMETRY_CLOSE},
        {.verb=OS64_GEOMETRY_MOVE,.p={{x,y}}}};
    for(unsigned i=0;i<6;++i)if(points(v[i])) {
        v[i].p[0]=mapped(&c->state.transform,v[i].p[0]);
        if(!isfinite(v[i].p[0].x)||!isfinite(v[i].p[0].y))return CANVAS_LIMIT;
    }
    canvas_status_t s=reserve(c,p,6);if(s!=CANVAS_OK)return s;
    for(unsigned i=0;i<6;++i)p->commands[p->count++]=v[i];
    return CANVAS_OK;
}
canvas_status_t canvas_path_rect(canvas_context_t *c,double x,double y,double w,double h)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    return c_leave(c,rectangle_path(c,&c->path,x,y,w,h));
}
static bool inverse(const Map *m,Map *out)
{
    double scale=fmax(fmax(fabs(m->a),fabs(m->b)),fmax(fabs(m->c),fabs(m->d)));
    if(!scale || !isfinite(scale))return false;
    double a=m->a/scale,b=m->b/scale,c=m->c/scale,d=m->d/scale,det=a*d-b*c;
    if(!det || !isfinite(det))return false;
    *out=(Map){d/det/scale,-b/det/scale,-c/det/scale,a/det/scale,0,0};
    out->tx=-(out->a*m->tx+out->c*m->ty);out->ty=-(out->b*m->tx+out->d*m->ty);
    return finite_map(out);
}
canvas_status_t canvas_transform(canvas_context_t *c,const Map *m,bool replace)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(!m)return c_leave(c,CANVAS_BAD_ARGUMENT);
    if(!finite_map(m))return c_leave(c,CANVAS_OK);
    Map a=c->state.transform, r=*m;
    if(!replace)r=(Map){a.a*m->a+a.c*m->b,a.b*m->a+a.d*m->b,
        a.a*m->c+a.c*m->d,a.b*m->c+a.d*m->d,
        a.a*m->tx+a.c*m->ty+a.tx,a.b*m->tx+a.d*m->ty+a.ty};
    if(!finite_map(&r))return c_leave(c,CANVAS_LIMIT);
    c->state.transform=r;return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_set_stroke(canvas_context_t *c,uint32_t rgb,double a)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(!isfinite(a)||a<0||a>1)return c_leave(c,CANVAS_BAD_ARGUMENT);
    canvas_gradient_release(c->state.stroke_gradient);c->state.stroke_gradient=NULL;
    c->state.stroke_argb=((uint32_t)(a*255+.5)<<24)|(rgb&0xffffff);c->state.stroke_alpha=a;
    return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_set_line(canvas_context_t *c,const os64_geometry_stroke_t *line)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(!line)return c_leave(c,CANVAS_BAD_ARGUMENT);
    if(isfinite(line->width)&&line->width>0&&isfinite(line->miter_limit)&&line->miter_limit>0 &&
       line->cap>=OS64_GEOMETRY_BUTT&&line->cap<=OS64_GEOMETRY_SQUARE&&
       (line->join==OS64_GEOMETRY_MITER||line->join==OS64_GEOMETRY_ROUND_JOIN||line->join==OS64_GEOMETRY_BEVEL))
        c->state.line=*line;
    return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_set_dash(canvas_context_t *c,const double *v,size_t count)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(count && !v)return c_leave(c,CANVAS_BAD_ARGUMENT);
    if(count>256)return c_leave(c,CANVAS_LIMIT);
    for(size_t i=0;i<count;++i)if(!isfinite(v[i])||v[i]<0)return c_leave(c,CANVAS_OK);
    if(count%2 && count>128)return c_leave(c,CANVAS_LIMIT);
    c->state.dash_count=count*(count%2?2:1);
    for(size_t i=0;i<c->state.dash_count;++i)c->state.dash[i]=v[i%count];
    return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_set_dash_offset(canvas_context_t *c,double v)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(isfinite(v))c->state.dash_offset=v;
    return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_set_composite(canvas_context_t *c,draw2d_operator_t op)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(op<DRAW2D_SOURCE_OVER || op>DRAW2D_LIGHTER || op==DRAW2D_CLEAR)return c_leave(c,CANVAS_BAD_ARGUMENT);
    c->state.composite=op;return c_leave(c,CANVAS_OK);
}
/* Cubic Hermite interpolation bounds each ellipse coordinate by its fourth
 * derivative times step^4/384. Subdivision is chosen in device space. */
static canvas_status_t ellipse_path(canvas_context_t *c,Path *out,const Map *map,const Map *precision_map,double x,double y,
    double rx,double ry,double rotation,double start,double end,bool ccw)
{
    double args[]={x,y,rx,ry,rotation,start,end};
    for(unsigned i=0;i<7;++i)if(!isfinite(args[i]))return CANVAS_OK;
    if(rx<0||ry<0)return CANVAS_BAD_ARGUMENT;
    double sweep=end-start;
    if((!ccw&&sweep>=2*PI)||(ccw&&sweep<=-2*PI))sweep=ccw?-2*PI:2*PI;
    else { sweep=fmod(end,2*PI)-fmod(start,2*PI);
        if(!ccw&&sweep<0)sweep+=2*PI;
        if(ccw&&sweep>0)sweep-=2*PI;
        if(sweep==0 && start!=end)sweep=ccw?-2*PI:2*PI;
    }
    start=fmod(start,2*PI);
    double cs=cos(rotation),sn=sin(rotation);
    Point u={rx*cs,rx*sn},v={-ry*sn,ry*cs};
    double bound=fmax(fabs(precision_map->a*u.x+precision_map->c*u.y)+fabs(precision_map->a*v.x+precision_map->c*v.y),
        fabs(precision_map->b*u.x+precision_map->d*u.y)+fabs(precision_map->b*v.x+precision_map->d*v.y));
    if(!isfinite(bound))return CANVAS_LIMIT;
    size_t n=1;
    while(fabs(sweep)/n>PI/2 || bound*pow(fabs(sweep)/n,4)/384>1.0/128) {
        if(n>=2048)return CANVAS_LIMIT;
        n*=2;
    }
    canvas_status_t s=reserve(c,out,n+1);if(s!=CANVAS_OK)return s;
    size_t original=out->count;
    Point first={x+u.x*cos(start)+v.x*sin(start),y+u.y*cos(start)+v.y*sin(start)};
    Command move={.verb=out->count?OS64_GEOMETRY_LINE:OS64_GEOMETRY_MOVE,.p={first}};
    if(!isfinite(first.x)||!isfinite(first.y)){s=CANVAS_LIMIT;goto fail;}
    s=append(c,out,move,map);
    if(s!=CANVAS_OK)goto fail;
    for(size_t i=0;i<n && sweep!=0;++i) {
        double a=start+sweep*i/n,b=start+sweep*(i+1)/n,k=(b-a)/3;
        Point pa={x+u.x*cos(a)+v.x*sin(a),y+u.y*cos(a)+v.y*sin(a)};
        Point pb={x+u.x*cos(b)+v.x*sin(b),y+u.y*cos(b)+v.y*sin(b)};
        Point da={-u.x*sin(a)+v.x*cos(a),-u.y*sin(a)+v.y*cos(a)};
        Point db={-u.x*sin(b)+v.x*cos(b),-u.y*sin(b)+v.y*cos(b)};
        Command cmd={.verb=OS64_GEOMETRY_CUBIC,.p={{pa.x+k*da.x,pa.y+k*da.y},{pb.x-k*db.x,pb.y-k*db.y},pb}};
        for(unsigned j=0;j<3;++j)if(!isfinite(cmd.p[j].x)||!isfinite(cmd.p[j].y)){s=CANVAS_LIMIT;goto fail;}
        s=append(c,out,cmd,map);if(s!=CANVAS_OK)goto fail;
    }
    return CANVAS_OK;
fail: out->count=original;return s;
}
canvas_status_t canvas_ellipse(canvas_context_t *c,double x,double y,double rx,double ry,
    double rotation,double start,double end,bool ccw)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    return c_leave(c,ellipse_path(c,&c->path,&c->state.transform,&c->state.transform,x,y,rx,ry,rotation,start,end,ccw));
}
canvas_status_t canvas_arc_to(canvas_context_t *c,double x1,double y1,double x2,double y2,double radius)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(!isfinite(x1)||!isfinite(y1)||!isfinite(x2)||!isfinite(y2)||!isfinite(radius))return c_leave(c,CANVAS_OK);
    Command line={.verb=OS64_GEOMETRY_LINE,.p={{x1,y1}}};
    if(radius<0) {
        if(!c->path.count) {
            s=append(c,&c->path,line,&c->state.transform);
            if(s!=CANVAS_OK)return c_leave(c,s);
        }
        return c_leave(c,CANVAS_BAD_ARGUMENT);
    }
    Map inv;
    if(!c->path.count)return c_leave(c,append(c,&c->path,line,&c->state.transform));
    if(!inverse(&c->state.transform,&inv))return c_leave(c,CANVAS_OK);
    Point p=mapped(&inv,current(&c->path)),a={p.x-x1,p.y-y1},b={x2-x1,y2-y1};
    double la=hypot(a.x,a.y),lb=hypot(b.x,b.y);
    if(!la||!lb||!radius)return c_leave(c,append(c,&c->path,line,&c->state.transform));
    a.x/=la;a.y/=la;b.x/=lb;b.y/=lb;
    double cross=a.x*b.y-a.y*b.x,dot=a.x*b.x+a.y*b.y;
    if(!cross)return c_leave(c,append(c,&c->path,line,&c->state.transform));
    double distance=radius*(1+dot)/fabs(cross);
    Point t={x1+distance*a.x,y1+distance*a.y};
    double sign=cross>0?1:-1;
    Point center={t.x-sign*a.y*radius,t.y+sign*a.x*radius};
    Point last={x1+distance*b.x,y1+distance*b.y};
    if(!isfinite(center.x)||!isfinite(center.y)||!isfinite(distance))return c_leave(c,CANVAS_LIMIT);
    s=ellipse_path(c,&c->path,&c->state.transform,&c->state.transform,center.x,center.y,radius,radius,0,
        atan2(t.y-center.y,t.x-center.x),atan2(last.y-center.y,last.x-center.x),cross>0);
    return c_leave(c,s);
}
canvas_status_t c_geometry_status(os64_geometry_status_t s)
{
    return s==OS64_GEOMETRY_OK?CANVAS_OK:s==OS64_GEOMETRY_NO_MEMORY?CANVAS_NO_MEMORY:
        s==OS64_GEOMETRY_LIMIT?CANVAS_LIMIT:s==OS64_GEOMETRY_UNSUPPORTED?CANVAS_UNSUPPORTED:CANVAS_BAD_ARGUMENT;
}
static os64_geometry_status_t geometry_alloc(void *p,size_t n,void **out)
{
    canvas_status_t s=c_allocate(p,n,out);
    return s==CANVAS_OK?OS64_GEOMETRY_OK:s==CANVAS_LIMIT?OS64_GEOMETRY_LIMIT:OS64_GEOMETRY_NO_MEMORY;
}
static void geometry_free(void *p,void *v,size_t n) { c_release(p,v,n); }
typedef union { max_align_t align; size_t bytes; } Scratch;
typedef struct { canvas_context_t *c; Path center,caps; uint64_t work; canvas_status_t status; } Cut;
static os64_geometry_status_t cut_spend(void *p,uint64_t n)
{
    Cut *cut=p;
    if(n>cut->c->options.max_geometry_work-cut->work){cut->status=CANVAS_LIMIT;return OS64_GEOMETRY_LIMIT;}
    cut->work+=n;return OS64_GEOMETRY_OK;
}
static os64_geometry_status_t cut_grow(void *p,void **v,size_t *capacity,size_t count,size_t size,size_t limit)
{
    Cut *cut=p;
    if(count>=limit)return OS64_GEOMETRY_LIMIT;
    if(count<*capacity)return OS64_GEOMETRY_OK;
    size_t n=*capacity?*capacity*2:8;if(n>limit)n=limit;
    if(n>(SIZE_MAX-sizeof(Scratch))/size)return OS64_GEOMETRY_LIMIT;
    size_t bytes=sizeof(Scratch)+n*size;
    os64_geometry_status_t s=cut_spend(p,bytes);if(s!=OS64_GEOMETRY_OK)return s;
    Scratch *out;
    s=geometry_alloc(cut->c,bytes,(void **)&out);if(s!=OS64_GEOMETRY_OK)return s;
    out->bytes=bytes;
    for(size_t i=0;i<count*size;++i)((unsigned char *)(out+1))[i]=((unsigned char *)*v)[i];
    if(*v){Scratch *old=(Scratch *)*v-1;c_release(cut->c,old,old->bytes);}
    *v=out+1;*capacity=n;return OS64_GEOMETRY_OK;
}
static void cut_free(void *p,void *v)
{ if(v){Cut *cut=p;Scratch *a=(Scratch *)v-1;c_release(cut->c,a,a->bytes);} }
static os64_geometry_status_t cut_emit(void *p,bool caps,const Command *v)
{
    Cut *cut=p;Path *out=caps?&cut->caps:&cut->center;
    os64_geometry_status_t s=cut_spend(p,1);if(s!=OS64_GEOMETRY_OK)return s;
    cut->status=reserve(cut->c,out,1);
    if(cut->status!=CANVAS_OK)return cut->status==CANVAS_LIMIT?OS64_GEOMETRY_LIMIT:OS64_GEOMETRY_NO_MEMORY;
    out->commands[out->count++]=*v;return OS64_GEOMETRY_OK;
}
static os64_geometry_status_t cut_cap(void *p,Point at,Point tangent)
{
    Cut *cut=p;canvas_context_t *c=cut->c;
    if(c->state.line.cap==OS64_GEOMETRY_BUTT)return OS64_GEOMETRY_OK;
    double r=c->state.line.width*.5;
    size_t old=cut->caps.count;
    if(c->state.line.cap==OS64_GEOMETRY_ROUND_CAP) {
        /* Each cap is its own contour; ellipse_path would otherwise connect it. */
        Path circle={0};
        cut->status=ellipse_path(c,&circle,&identity,&c->state.transform,at.x,at.y,r,r,0,0,2*PI,false);
        if(cut->status==CANVAS_OK)for(size_t i=0;i<circle.count;++i)
            if(cut_emit(p,true,&circle.commands[i])!=OS64_GEOMETRY_OK)break;
        c_path_release(c,&circle);
    } else {
        double length=hypot(tangent.x,tangent.y);
        Point along=length?(Point){tangent.x/length*r,tangent.y/length*r}:(Point){r,0};
        Point normal={-along.y,along.x};
        Command square[4]={{.verb=OS64_GEOMETRY_MOVE,.p={{at.x-along.x-normal.x,at.y-along.y-normal.y}}},
            {.verb=OS64_GEOMETRY_LINE,.p={{at.x+along.x-normal.x,at.y+along.y-normal.y}}},
            {.verb=OS64_GEOMETRY_LINE,.p={{at.x+along.x+normal.x,at.y+along.y+normal.y}}},
            {.verb=OS64_GEOMETRY_LINE,.p={{at.x-along.x+normal.x,at.y-along.y+normal.y}}}};
        for(unsigned i=0;i<4;++i)if(cut_emit(p,true,&square[i])!=OS64_GEOMETRY_OK)break;
    }
    if(cut->status!=CANVAS_OK){cut->caps.count=old;return cut->status==CANVAS_LIMIT?OS64_GEOMETRY_LIMIT:OS64_GEOMETRY_NO_MEMORY;}
    Command close={.verb=OS64_GEOMETRY_CLOSE};return cut_emit(p,true,&close);
}
/* The expanded outline bounds include stroke joins and caps. Keep the same
 * two-pixel fringe as SVG for quantization and the changed raster origin. */
static canvas_status_t coverage_box(const canvas_context_t *c,os64_geometry_bounds_t b,draw2d_damage_t *box)
{
    *box=(draw2d_damage_t){0};
    if(b.empty)return CANVAS_OK;
    if(!isfinite(b.x_min)||!isfinite(b.y_min)||!isfinite(b.x_max)||!isfinite(b.y_max)||
       b.x_min>b.x_max||b.y_min>b.y_max)return CANVAS_BAD_ARGUMENT;
    double x0=fmax(0,floor(b.x_min-2)),y0=fmax(0,floor(b.y_min-2));
    double x1=fmin(c->surface.width,ceil(b.x_max+2)),y1=fmin(c->surface.height,ceil(b.y_max+2));
    if(x1>x0 && y1>y0)*box=(draw2d_damage_t){x0,y0,x1,y1};
    return CANVAS_OK;
}
/* Lines, Beziers and implicit closing edges stay within their control hull.
 * Commands are already in device space. A common outside half-plane proves
 * an empty fill without asking the fixed-point backend to admit far-away
 * coordinates. Retain the coverage box's two-pixel quantization fringe. */
static bool fill_outside(const canvas_context_t *c,const Path *path)
{
    bool left=true,right=true,top=true,bottom=true,any=false;
    for(size_t i=0;i<path->count;++i) for(unsigned j=0;j<points(path->commands[i]);++j) {
        Point p=path->commands[i].p[j];
        if(!isfinite(p.x)||!isfinite(p.y))return false;
        any=true;
        left &= p.x < -2;right &= p.x > (double)c->surface.width+2;
        top &= p.y < -2;bottom &= p.y > (double)c->surface.height+2;
        if(!left&&!right&&!top&&!bottom)return false;
    }
    return any && (left||right||top||bottom);
}
/* Geometry and coverage are scratch until preparation succeeds. Neither a
 * backend refusal nor a failed clip allocation can partially paint a bitmap. */
static canvas_status_t render(canvas_context_t *c,const Path *path,int mode,bool even_odd,const draw2d_image_t *image)
{
    const os64_geometry_backend_t *b=c->options.geometry;
    if(!b || b->revision!=OS64_GEOMETRY_BACKEND_REVISION ||
       b->struct_size<offsetof(os64_geometry_backend_t,stroke_with_caps)+sizeof(b->stroke_with_caps) ||
       !b->create||!b->destroy||!b->path||!b->stroke_with_caps||!b->raster||!b->release)return CANVAS_UNSUPPORTED;
    GradientProgram program;draw2d_shader_t shader={&program,c_gradient_sample};
    canvas_gradient_t *gradient=mode==2?c->state.stroke_gradient:c->state.fill_gradient;
    if(image||mode==1||mode==3)gradient=NULL;
    if(gradient){canvas_status_t prepared=c_gradient_prepare(gradient,&c->state.transform,&program);if(prepared!=CANVAS_OK)return prepared;}
    size_t pixels=(size_t)c->surface.width*c->surface.height;
    if(!pixels)return CANVAS_OK;
    if(pixels>c->options.max_draw_pixels)return CANVAS_LIMIT;
    /* Stroke expansion and shifted/blurred shadows can enter the bitmap.
     * Clip replacement and operators such as copy also affect pixels outside
     * the fill, so those operations must keep their normal preparation. */
    if((mode==0||mode==1) && (mode==1||!c_shadows(c)) &&
       draw2d_zero_coverage_preserves_destination(mode==1?DRAW2D_CLEAR:c->state.composite) &&
       fill_outside(c,path))return CANVAS_OK;
    Map map=identity,inv;
    Path converted={0};Cut cut={.c=c,.status=CANVAS_OK};
    canvas_status_t status=CANVAS_OK;
    if(mode==2) {
        map=c->state.transform;
        if(!inverse(&map,&inv))return CANVAS_OK;
        status=reserve(c,&converted,path->count);if(status!=CANVAS_OK)goto done;
        /* Remove zero-length line segments before tracing, per Canvas. SVG
         * keeps its own degenerate-cap rule in the shared cutter's adapter. */
        Point last={0};
        for(size_t i=0;i<path->count;++i) {
            Command cmd=path->commands[i];
            for(unsigned j=0;j<points(cmd);++j)cmd.p[j]=mapped(&inv,cmd.p[j]);
            bool zero=cmd.verb!=OS64_GEOMETRY_MOVE && points(cmd)>0;
            for(unsigned j=0;j<points(cmd);++j)zero &= same(last,cmd.p[j]);
            if(zero)continue;
            if(cmd.verb==OS64_GEOMETRY_CLOSE && converted.count &&
               converted.commands[converted.count-1].verb==OS64_GEOMETRY_MOVE)continue;
            if(points(cmd))last=cmd.p[points(cmd)-1];
            converted.commands[converted.count++]=cmd;
        }
        draw2d_stroke_ops_t ops={&cut,c->options.max_commands,cut_spend,cut_grow,cut_free,cut_emit,cut_cap};
        draw2d_stroke_style_t style={{c->state.dash,c->state.dash_count,c->state.dash_offset}};
        status=c_geometry_status(draw2d_stroke_cut(&ops,converted.commands,converted.count,&map,1.0/64,&style));
        if(status!=CANVAS_OK)goto done;
    }
    os64_geometry_context_t *geometry=NULL;
    os64_geometry_options_t options={.memory={c,geometry_alloc,geometry_free},.max_work=c->options.max_geometry_work};
    status=c_geometry_status(b->create(&options,&geometry));if(status!=CANVAS_OK)goto done;
    os64_geometry_outline_t *outline=NULL;
    if(mode==2 && (cut.center.count||cut.caps.count)) {
        os64_geometry_stroke_t line=c->state.line;
        /* Canvas admits positive miter limits below one; those joins bevel. */
        if(line.miter_limit<1){line.miter_limit=1;if(line.join==OS64_GEOMETRY_MITER)line.join=OS64_GEOMETRY_BEVEL;}
        status=c_geometry_status(b->stroke_with_caps(geometry,cut.center.commands,cut.center.count,
            cut.caps.commands,cut.caps.count,&map,1.0/64,&line,&outline));
    } else if(mode!=2 && path->count)
        status=c_geometry_status(b->path(geometry,path->commands,path->count,&map,1.0/64,&outline));
    if(status==CANVAS_OK && mode!=1 && mode!=3 && c_shadows(c)) {
        status=c_shadow_paint(c,geometry,outline,&map,even_odd,
            (draw2d_solid_t){mode==2?c->state.stroke_argb:c->state.fill_argb},gradient?&shader:NULL,image);
        if(outline)b->release(geometry,outline);
        b->destroy(geometry);
        goto done;
    }
    draw2d_damage_t box={0,0,c->surface.width,c->surface.height};
    draw2d_operator_t op=mode==1?DRAW2D_CLEAR:c->state.composite;
    /* Solid paints can use a sub-surface without changing source coordinates.
     * Clip creation and source/image shaders retain the full target; operators
     * that clear beyond the shape must also keep that complete coverage. */
    bool compact=mode!=3 && !gradient && !image && b->bounds &&
        draw2d_zero_coverage_preserves_destination(op);
    if(status==CANVAS_OK && compact) {
        os64_geometry_bounds_t bounds={.empty=true};
        if(outline)status=c_geometry_status(b->bounds(geometry,outline,&map,0,0,&bounds));
        if(status==CANVAS_OK)status=coverage_box(c,bounds,&box);
    }
    uint32_t width=box.x1-box.x0,height=box.y1-box.y0;
    size_t covered=(size_t)width*height;
    uint8_t *coverage=NULL,*clip_copy=NULL;
    const uint8_t *clip=c->clip?c->clip->pixels:NULL;
    if(status==CANVAS_OK && covered)status=c_allocate(c,covered,(void **)&coverage);
    if(status==CANVAS_OK && covered) {
        for(size_t i=0;i<covered;++i)coverage[i]=0;
        os64_geometry_target_t target={box.x0,box.y0,width,height,width,covered,coverage,even_odd};
        if(outline)status=c_geometry_status(b->raster(geometry,outline,&map,&target));
    }
    if(outline)b->release(geometry,outline);
    b->destroy(geometry);
    if(status==CANVAS_OK && !covered)goto done;
    if(status==CANVAS_OK && compact && clip) {
        status=c_allocate(c,covered,(void **)&clip_copy);
        if(status==CANVAS_OK) {
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x)
                clip_copy[(size_t)y*width+x]=clip[(size_t)(box.y0+y)*c->surface.width+box.x0+x];
            clip=clip_copy;
        }
    }
    draw2d_surface_t surface=c->surface;
    if(status==CANVAS_OK) {
        size_t offset=(size_t)box.y0*surface.stride+(size_t)box.x0*sizeof(uint32_t);
        surface.pixels=(uint8_t *)surface.pixels+offset;
        surface.capacity-=offset;surface.width=width;surface.height=height;
    }
    if(status==CANVAS_OK && mode==3) {
        Clip *clip;
        status=c_allocate(c,sizeof(*clip)+pixels,(void **)&clip);
        if(status==CANVAS_OK) {
            clip->references=1;clip->bytes=sizeof(*clip)+pixels;
            for(size_t i=0;i<pixels;++i)clip->pixels[i]=c->clip?
                ((unsigned)coverage[i]*c->clip->pixels[i]+127)/255:coverage[i];
            c_clip_release(c,c->clip);c->clip=clip;
        }
    } else if(status==CANVAS_OK) {
        draw2d_damage_t damage;
        draw2d_status_t result=gradient?draw2d_paint_shader(&surface,coverage,clip,covered,&shader,
            (draw2d_composite_t){c->state.composite,(uint16_t)(c->state.global_alpha*65535+.5)},c->options.max_draw_pixels,&damage):image?draw2d_paint_image(&surface,coverage,clip,covered,image,
            (draw2d_composite_t){c->state.composite,(uint16_t)(c->state.global_alpha*65535+.5)},
            c->options.max_draw_pixels,&damage):draw2d_paint_mask(&surface,coverage,clip,covered,
            (draw2d_solid_t){mode==2?c->state.stroke_argb:c->state.fill_argb},
            (draw2d_composite_t){mode==1?DRAW2D_CLEAR:c->state.composite,(uint16_t)(c->state.global_alpha*65535+.5)},
            c->options.max_draw_pixels,&damage);
        status=result==DRAW2D_OK?CANVAS_OK:CANVAS_BAD_ARGUMENT;
        if(status==CANVAS_OK && damage.x1>damage.x0 && damage.y1>damage.y0) {
            damage.x0+=box.x0;damage.x1+=box.x0;damage.y0+=box.y0;damage.y1+=box.y0;
            c_damage_union(c,damage);
        }
    }
    c_release(c,clip_copy,covered);
    c_release(c,coverage,covered);
done:
    c_path_release(c,&converted);c_path_release(c,&cut.center);c_path_release(c,&cut.caps);
    return status;
}
static canvas_status_t draw(canvas_context_t *c,int mode,bool even_odd)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    return c_leave(c,render(c,&c->path,mode,even_odd,NULL));
}
canvas_status_t canvas_fill(canvas_context_t *c,bool even_odd){return draw(c,0,even_odd);}
canvas_status_t canvas_stroke(canvas_context_t *c){return draw(c,2,false);}
canvas_status_t canvas_clip(canvas_context_t *c,bool even_odd){return draw(c,3,even_odd);}
canvas_status_t c_rectangle(canvas_context_t *c,double x,double y,double w,double h,int mode)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if(!isfinite(x)||!isfinite(y)||!isfinite(w)||!isfinite(h) || (mode!=2 && (!w||!h)) || (!w&&!h))
        return c_leave(c,CANVAS_OK);
    Map m=c->state.transform;
    if(mode!=2 && (mode==1||!c_shadows(c)) && (mode==1||!c->state.fill_gradient) && !c->clip && m.a==1&&m.b==0&&m.c==0&&m.d==1 &&
       (mode==1 || c->state.composite==DRAW2D_SOURCE_OVER)) {
        draw2d_damage_t damage;
        draw2d_status_t result=draw2d_paint_rect(&c->surface,x+m.tx,y+m.ty,w,h,
            (draw2d_solid_t){c->state.fill_argb},
            (draw2d_composite_t){mode==1?DRAW2D_CLEAR:DRAW2D_SOURCE_OVER,(uint16_t)(c->state.global_alpha*65535+.5)},
            c->options.max_draw_pixels,&damage);
        if(result==DRAW2D_OK)c_damage_union(c,damage);
        return c_leave(c,result==DRAW2D_OK?CANVAS_OK:result==DRAW2D_LIMIT?CANVAS_LIMIT:CANVAS_BAD_ARGUMENT);
    }
    Path path={0};
    s=rectangle_path(c,&path,x,y,w,h);
    if(s==CANVAS_OK)s=render(c,&path,mode,false,NULL);
    c_path_release(c,&path);return c_leave(c,s);
}

bool canvas_origin_clean(const canvas_context_t *c) { return c && c->origin_clean; }
canvas_status_t canvas_set_smoothing(canvas_context_t *c,bool enabled,unsigned quality)
{
    canvas_status_t status=c_enter(c);if(status!=CANVAS_OK)return status;
    if(quality>2)return c_leave(c,CANVAS_BAD_ARGUMENT);
    c->state.image_smoothing=enabled;c->state.image_quality=quality;
    return c_leave(c,CANVAS_OK);
}
canvas_status_t canvas_draw_image(canvas_context_t *c,const draw2d_surface_t *source,bool clean,
    double sx,double sy,double sw,double sh,double dx,double dy,double dw,double dh)
{
    canvas_status_t status=c_enter(c);if(status!=CANVAS_OK)return status;
    double args[]={sx,sy,sw,sh,dx,dy,dw,dh};
    for(unsigned i=0;i<8;++i)if(!isfinite(args[i]))return c_leave(c,CANVAS_OK);
    if(!source||draw2d_surface_validate(source)!=DRAW2D_OK||!source->width||!source->height)
        return c_leave(c,CANVAS_BAD_ARGUMENT);
    if(!sw||!sh||!dw||!dh)return c_leave(c,CANVAS_OK);
    if(sw<0){sx+=sw;sw=-sw;}if(sh<0){sy+=sh;sh=-sh;}
    if(dw<0){dx+=dw;dw=-dw;}if(dh<0){dy+=dh;dh=-dh;}
    double right=fmin(source->width,sx+sw),bottom=fmin(source->height,sy+sh);
    double left=fmax(0,sx),top=fmax(0,sy);
    if(right<=left||bottom<=top)return c_leave(c,CANVAS_OK);
    double x=dx+(left-sx)/sw*dw,y=dy+(top-sy)/sh*dh;
    double width=(right-left)/sw*dw,height=(bottom-top)/sh*dh;
    if(!isfinite(x)||!isfinite(y)||!isfinite(width)||!isfinite(height))return c_leave(c,CANVAS_LIMIT);
    Map inv;
    if(!inverse(&c->state.transform,&inv)) {
        /* A singular transform suppresses pixels, not source-origin taint. */
        if(!clean)c->origin_clean=false;
        return c_leave(c,CANVAS_OK);
    }
    double ax=sw/dw,ay=sh/dh;
    draw2d_image_t image={*source,{ax*inv.a,ay*inv.b,ax*inv.c,ay*inv.d,
        sx+ax*(inv.tx-dx),sy+ay*(inv.ty-dy)},c->state.image_smoothing};
    /* Address ranges are checked before copying; self-draw has snapshot semantics. */
    uintptr_t a=(uintptr_t)source->pixels,b=(uintptr_t)c->surface.pixels;
    if(source->capacity>UINTPTR_MAX-a||c->surface.capacity>UINTPTR_MAX-b)return c_leave(c,CANVAS_LIMIT);
    void *copy=NULL;size_t bytes=0;
    if(a<b+c->surface.capacity&&b<a+source->capacity) {
        size_t unit=source->format==DRAW2D_ARGB16_PREMULTIPLIED?sizeof(draw2d_pixel_t):sizeof(uint32_t);
        size_t stride=(size_t)source->width*unit;
        if(source->height>SIZE_MAX/stride)return c_leave(c,CANVAS_LIMIT);
        bytes=stride*source->height;
        status=c_allocate(c,bytes,&copy);if(status!=CANVAS_OK)return c_leave(c,status);
        for(uint32_t row=0;row<source->height;++row)for(size_t col=0;col<stride;++col)
            ((unsigned char *)copy)[row*stride+col]=((const unsigned char *)source->pixels)[row*source->stride+col];
        image.surface.pixels=copy;image.surface.stride=stride;image.surface.capacity=bytes;
    }
    Path path={0};status=rectangle_path(c,&path,x,y,width,height);
    if(status==CANVAS_OK)status=render(c,&path,0,false,&image);
    if(status==CANVAS_OK&&!clean)c->origin_clean=false;
    c_path_release(c,&path);c_release(c,copy,bytes);
    return c_leave(c,status);
}

static canvas_status_t draw_commands(canvas_context_t *c,const Command *commands,size_t count,
    const Map *placement,int mode,bool even_odd)
{
    canvas_status_t s=c_enter(c);if(s!=CANVAS_OK)return s;
    if((count&&!commands)||!placement||!finite_map(placement))return c_leave(c,CANVAS_BAD_ARGUMENT);
    Path path={0};s=reserve(c,&path,count);
    bool open=false;
    for(size_t i=0;s==CANVAS_OK&&i<count;++i) {
        Command command=commands[i];
        if(command.verb<OS64_GEOMETRY_MOVE||command.verb>OS64_GEOMETRY_CLOSE||
           (!open&&command.verb!=OS64_GEOMETRY_MOVE)){s=CANVAS_BAD_ARGUMENT;break;}
        if(command.verb==OS64_GEOMETRY_MOVE)open=true;
        if(command.verb==OS64_GEOMETRY_CLOSE)open=false;
        for(unsigned k=0;k<points(command);++k) {
            command.p[k]=mapped(&c->state.transform,mapped(placement,command.p[k]));
            if(!isfinite(command.p[k].x)||!isfinite(command.p[k].y))s=CANVAS_LIMIT;
        }
        path.commands[path.count++]=command;
    }
    if(s==CANVAS_OK)s=render(c,&path,mode,even_odd,NULL);
    c_path_release(c,&path);return c_leave(c,s);
}

canvas_status_t canvas_draw_contours(canvas_context_t *c,const Command *commands,size_t count,
    const Map *placement,bool stroke)
{ return draw_commands(c,commands,count,placement,stroke?2:0,false); }
canvas_status_t canvas_draw_path(canvas_context_t *c,const Command *commands,size_t count,
    canvas_path_operation_t operation,bool even_odd)
{
    if(operation<CANVAS_PATH_FILL||operation>CANVAS_PATH_CLIP)return CANVAS_BAD_ARGUMENT;
    return draw_commands(c,commands,count,&identity,
        operation==CANVAS_PATH_FILL?0:operation==CANVAS_PATH_STROKE?2:3,even_odd);
}
