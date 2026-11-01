#include "internal.h"

#define PI 3.14159265358979323846264338327950288
/* Classify in user space, independently of raster quantization. Splitting at
 * coordinate extrema makes each ray crossing monotone; bisection then needs
 * neither a flattened path nor an unbounded recursion stack. */
typedef struct {
    Point p[4];
    unsigned degree;
    bool arc;
    SvgArc ellipse;
} Curve;
typedef struct { int winding; bool boundary; } Hit;
static Point at(const Curve *c, double t)
{
    if (c->arc) {
        if(t==0)return c->p[0];
        if(t==1)return c->p[1];
        double angle = c->ellipse.angle + t * c->ellipse.sweep;
        return (Point){c->ellipse.center.x + c->ellipse.u.x*cos(angle) + c->ellipse.v.x*sin(angle),
                       c->ellipse.center.y + c->ellipse.u.y*cos(angle) + c->ellipse.v.y*sin(angle)};
    }
    Point p[4];
    for (unsigned i=0;i<=c->degree;i++) p[i]=c->p[i];
    for (unsigned n=c->degree;n;n--)
        for (unsigned i=0;i<n;i++)
            p[i]=(Point){(1-t)*p[i].x+t*p[i+1].x,(1-t)*p[i].y+t*p[i+1].y};
    return p[0];
}
static void insert(double *v, unsigned *n, double t)
{
    if (!(t>0 && t<1)) return;
    unsigned i=*n;
    while (i && v[i-1]>t) {v[i]=v[i-1];i--;}
    v[i]=t;(*n)++;
}
static void extrema(const Curve *c, unsigned axis, double *v, unsigned *n)
{
    if (!c->arc) {
        double roots[2];
        unsigned count=svg_bezier_extrema(c->p,c->degree,axis,roots);
        for(unsigned i=0;i<count;i++) insert(v,n,roots[i]);
    } else {
        double a=atan2(axis?c->ellipse.v.y:c->ellipse.v.x,
                       axis?c->ellipse.u.y:c->ellipse.u.x);
        for(int k=-4;k<=4;k++)
            insert(v,n,(a+k*PI-c->ellipse.angle)/c->ellipse.sweep);
    }
}
static bool crossing(Context *ctx, const Curve *curve, Point point, Hit *hit)
{
    /* Normalize before subtraction, including the query. Finite input values
     * can otherwise overflow differences and derivative coefficients. */
    Curve c=*curve;
    double scale=fmax(fabs(point.x),fabs(point.y));
    if(c.arc) {
        Point p[]={c.ellipse.center,c.ellipse.u,c.ellipse.v};
        for(unsigned i=0;i<3;i++) scale=fmax(scale,fmax(fabs(p[i].x),fabs(p[i].y)));
    } else for(unsigned i=0;i<=c.degree;i++)
        scale=fmax(scale,fmax(fabs(c.p[i].x),fabs(c.p[i].y)));
    if(!scale) scale=1;
    point.x/=scale;point.y/=scale;
    if(c.arc) {
        c.ellipse.center.x/=scale;c.ellipse.center.y/=scale;
        c.ellipse.u.x/=scale;c.ellipse.u.y/=scale;
        c.ellipse.v.x/=scale;c.ellipse.v.y/=scale;
    }
    for(unsigned i=0;i<=c.degree;i++) {c.p[i].x/=scale;c.p[i].y/=scale;}
    double cuts[12]={0};unsigned count=1;
    /* X extrema also split horizontal curves, whose entire span is boundary. */
    extrema(&c,0,cuts,&count);extrema(&c,1,cuts,&count);cuts[count++]=1;
    const double epsilon=64*DBL_EPSILON;
    for(unsigned i=1;i<count;i++) {
        if(!svg_spend(ctx,1)) return false;
        double lo=cuts[i-1],hi=cuts[i];Point a=at(&c,lo),b=at(&c,hi);
        if(fabs(a.x-point.x)<=epsilon && fabs(a.y-point.y)<=epsilon)hit->boundary=true;
        if(fabs(b.x-point.x)<=epsilon && fabs(b.y-point.y)<=epsilon)hit->boundary=true;
        if(a.y==b.y) {
            if(fabs(a.y-point.y)<=epsilon && point.x>=fmin(a.x,b.x)-epsilon &&
               point.x<=fmax(a.x,b.x)+epsilon)hit->boundary=true;
            continue;
        }
        if(point.y<fmin(a.y,b.y)||point.y>fmax(a.y,b.y))continue;
        bool up=b.y>a.y;
        for(unsigned j=0;j<60;j++) {
            if(!svg_spend(ctx,1))return false;
            double mid=lo+(hi-lo)/2;
            if(mid==lo||mid==hi)break;
            Point p=at(&c,mid);
            if((p.y<point.y)==up)lo=mid;else hi=mid;
        }
        Point p=at(&c,lo+(hi-lo)/2);
        if(fabs(p.x-point.x)<=epsilon)hit->boundary=true;
        /* Half-open Y ranges count a shared vertex once; opposite tangencies
         * cancel rather than changing winding. */
        if(point.y<fmax(a.y,b.y) && p.x>point.x)hit->winding+=up?1:-1;
    }
    return true;
}
static bool line(Context *c,Point a,Point b,Point p,Hit *hit)
{
    return crossing(c,&(Curve){.p={a,b},.degree=1},p,hit);
}
Status svg_path_contains(Context *c,const svg_path_t *path,Point point,bool even_odd,bool *out)
{
    if(!out)return SVG_BAD_ARGUMENT;
    *out=false;
    if(!c||!path||path->owner!=c)return SVG_BAD_ARGUMENT;
    Status status=svg_enter(c);if(status!=SVG_OK)return status;
    if(!svg_point_valid(point))return svg_leave(c,SVG_OK);
    Hit hit={0};Point current={0},start={0};bool drawn=false;
    for(size_t i=0;i<path->count;i++) {
        if(!svg_spend(c,1))break;
        const svg_command_t *cmd=&path->commands[i];
        if(cmd->verb==SVG_MOVE) {
            if(drawn&&!line(c,current,start,point,&hit))break;
            current=start=cmd->p[0];drawn=false;continue;
        }
        if(cmd->verb==SVG_CLOSE) {
            if(drawn&&!line(c,current,start,point,&hit))break;
            current=start;continue;
        }
        Curve curve={.p={current},.degree=cmd->verb==SVG_CUBIC?3:cmd->verb==SVG_QUADRATIC?2:1};
        for(unsigned j=0;j<curve.degree;j++)curve.p[j+1]=cmd->p[j];
        if(cmd->verb==SVG_ARC && cmd->rx && cmd->ry) {
            if(current.x==cmd->p[0].x&&current.y==cmd->p[0].y)continue;
            if(!svg_arc_center(c,current,cmd,&curve.ellipse))break;
            curve.arc=true;
        }
        if(!crossing(c,&curve,point,&hit))break;
        current=cmd->p[curve.degree-1];drawn=true;
    }
    if(c->why==SVG_OK && drawn)line(c,current,start,point,&hit);
    if(c->why==SVG_OK)*out=hit.boundary||(even_odd?(hit.winding%2!=0):(hit.winding!=0));
    return svg_leave(c,c->why);
}
