#include "internal.h"
#include <math.h>

enum { P_BEGIN,P_CLOSE,P_MOVE,P_LINE,P_QUAD,P_CUBIC,P_RECT,P_ARC,P_ELLIPSE,P_ARC_TO,
    P_FILL,P_STROKE,P_CLIP,P_STROKE_RECT,P_TRANSLATE,P_SCALE,P_ROTATE,P_TRANSFORM,P_SET_TRANSFORM,
    P_RESET_TRANSFORM,P_WIDTH,P_CAP,P_JOIN,P_MITER,P_DASH_OFFSET,P_DASH_SET,P_DASH_GET,P_COMPOSITE };
static const struct { const char *name; draw2d_operator_t op; } composites[]={
    {"source-over",DRAW2D_SOURCE_OVER},{"destination-out",DRAW2D_DESTINATION_OUT},
    {"source-in",DRAW2D_SOURCE_IN},{"source-out",DRAW2D_SOURCE_OUT},{"source-atop",DRAW2D_SOURCE_ATOP},
    {"destination-over",DRAW2D_DESTINATION_OVER},{"destination-in",DRAW2D_DESTINATION_IN},
    {"destination-atop",DRAW2D_DESTINATION_ATOP},{"copy",DRAW2D_COPY},{"xor",DRAW2D_XOR},{"lighter",DRAW2D_LIGHTER}};
static bool equal(const char *s,size_t n,const char *expected)
{ return n==os64_strlen(expected)&&!os64_memcmp(s,expected,n); }
static int matrix(JSContext *ctx,JSValueConst object,os64_geometry_map_t *m)
{
    if(!JS_IsObject(object)&&!JS_IsNull(object)&&!JS_IsUndefined(object)) {
        JS_ThrowTypeError(ctx,"Expected a matrix dictionary");return -1;
    }
    const char *names[]={"a","b","c","d","e","f","m11","m12","m21","m22","m41","m42"};
    double values[12]={0};bool present[12]={0};
    for(unsigned i=0;i<12;++i) {
        JSValue v=JS_IsObject(object)?JS_GetPropertyStr(ctx,object,names[i]):JS_UNDEFINED;
        if(JS_IsException(v))return -1;
        present[i]=!JS_IsUndefined(v);
        int result=present[i]?JS_ToFloat64(ctx,&values[i],v):0;
        JS_FreeValue(ctx,v);if(result<0)return -1;
    }
    for(unsigned i=0;i<6;++i) {
        if(present[i]&&present[i+6] && values[i]!=values[i+6] && !(isnan(values[i])&&isnan(values[i+6]))) {
            JS_ThrowTypeError(ctx,"Conflicting matrix aliases");return -1;
        }
        if(!present[i])values[i]=present[i+6]?values[i+6]:(i==0||i==3?1:0);
    }
    *m=(os64_geometry_map_t){values[0],values[1],values[2],values[3],values[4],values[5]};return 0;
}
static JSValue member(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);
    if(!dom)return JS_EXCEPTION;
    DValue *e=JS_GetOpaque(self,dom->canvas_class);
    if(!e||e->dom!=dom)return JS_ThrowTypeError(ctx,"Expected a CanvasRenderingContext2D");
    bool set=magic&D_SET;magic&=~D_SET;
    JSValueConst value=argc?argv[0]:JS_UNDEFINED;
    if(magic>=P_WIDTH && magic!=P_DASH_SET && magic!=P_DASH_GET) {
        double number=0;int selected=-1;
        if(set) {
            if(magic==P_CAP||magic==P_JOIN||magic==P_COMPOSITE) {
                size_t n;const char *s=JS_ToCStringLen(ctx,&n,value);if(!s)return JS_EXCEPTION;
                if(magic==P_CAP)selected=equal(s,n,"butt")?OS64_GEOMETRY_BUTT:
                    equal(s,n,"round")?OS64_GEOMETRY_ROUND_CAP:equal(s,n,"square")?OS64_GEOMETRY_SQUARE:-1;
                else if(magic==P_JOIN)selected=equal(s,n,"miter")?OS64_GEOMETRY_MITER:
                    equal(s,n,"round")?OS64_GEOMETRY_ROUND_JOIN:equal(s,n,"bevel")?OS64_GEOMETRY_BEVEL:-1;
                else for(size_t i=0;i<sizeof(composites)/sizeof(*composites);++i)
                    if(equal(s,n,composites[i].name))selected=composites[i].op;
                JS_FreeCString(ctx,s);
            } else if(JS_ToFloat64(ctx,&number,value)<0)return JS_EXCEPTION;
        }
        d_canvas_sync(e);
        canvas_state_t state={.line={1,10,OS64_GEOMETRY_BUTT,OS64_GEOMETRY_MITER}};
        if(e->canvas)canvas_state(e->canvas,&state);
        if(set) {
            if(!e->canvas)return JS_UNDEFINED;
            if(magic==P_COMPOSITE)return selected<0?JS_UNDEFINED:d_canvas_result(ctx,canvas_set_composite(e->canvas,selected));
            if(magic==P_DASH_OFFSET)return d_canvas_result(ctx,canvas_set_dash_offset(e->canvas,number));
            if(magic==P_WIDTH)state.line.width=number;
            if(magic==P_MITER)state.line.miter_limit=number;
            if(magic==P_CAP&&selected>=0)state.line.cap=selected;
            if(magic==P_JOIN&&selected>=0)state.line.join=selected;
            return d_canvas_result(ctx,canvas_set_line(e->canvas,&state.line));
        }
        if(magic==P_WIDTH)return JS_NewFloat64(ctx,state.line.width);
        if(magic==P_MITER)return JS_NewFloat64(ctx,state.line.miter_limit);
        if(magic==P_DASH_OFFSET)return JS_NewFloat64(ctx,state.dash_offset);
        if(magic==P_CAP)return JS_NewString(ctx,state.line.cap==OS64_GEOMETRY_BUTT?"butt":state.line.cap==OS64_GEOMETRY_SQUARE?"square":"round");
        if(magic==P_JOIN)return JS_NewString(ctx,state.line.join==OS64_GEOMETRY_MITER?"miter":state.line.join==OS64_GEOMETRY_BEVEL?"bevel":"round");
        for(size_t i=0;i<sizeof(composites)/sizeof(*composites);++i)
            if(composites[i].op==state.composite)return JS_NewString(ctx,composites[i].name);
        return JS_NewString(ctx,"source-over");
    }
    if(magic==P_DASH_SET||magic==P_DASH_GET) {
        double dash[256];size_t count=0;
        if(magic==P_DASH_SET) {
            if(!argc)return JS_ThrowTypeError(ctx,"setLineDash requires a sequence");
            JSValue *items;
            if(d_double_sequence(dom,ctx,value,256,&items,&count)<0)return JS_EXCEPTION;
            for(size_t i=0;i<count;++i)JS_ToFloat64(ctx,&dash[i],items[i]);
            d_sequence_free(ctx,items,count);
        }
        d_canvas_sync(e);
        if(magic==P_DASH_SET)return e->canvas?d_canvas_result(ctx,canvas_set_dash(e->canvas,dash,count)):JS_UNDEFINED;
        canvas_state_t state={0};if(e->canvas)canvas_state(e->canvas,&state);
        JSValue array=JS_NewArray(ctx);if(JS_IsException(array))return array;
        for(size_t i=0;i<state.dash_count;++i)if(JS_SetPropertyUint32(ctx,array,i,JS_NewFloat64(ctx,state.dash[i]))<0) {
            JS_FreeValue(ctx,array);return JS_EXCEPTION;
        }
        return array;
    }
    bool even_odd=false;
    if(magic==P_FILL||magic==P_CLIP) {
        if(!JS_IsUndefined(value)) {
            size_t n;const char *s=JS_ToCStringLen(ctx,&n,value);if(!s)return JS_EXCEPTION;
            bool valid=equal(s,n,"nonzero")||equal(s,n,"evenodd");even_odd=equal(s,n,"evenodd");
            JS_FreeCString(ctx,s);if(!valid)return JS_ThrowTypeError(ctx,"Invalid Canvas fill rule");
        }
    }
    unsigned required=magic==P_MOVE||magic==P_LINE||magic==P_TRANSLATE||magic==P_SCALE?2:
        magic==P_QUAD||magic==P_RECT||magic==P_STROKE_RECT?4:
        magic==P_CUBIC||magic==P_TRANSFORM?6:magic==P_ARC||magic==P_ARC_TO?5:
        magic==P_ELLIPSE?7:magic==P_ROTATE?1:0;
    os64_geometry_map_t transform={1,0,0,1,0,0};
    if(magic==P_SET_TRANSFORM) {
        if(argc<=1) { if(matrix(ctx,value,&transform)<0)return JS_EXCEPTION; }
        else required=6;
    }
    if(argc<(int)required)return JS_ThrowTypeError(ctx,"Canvas method is missing coordinates");
    double v[7]={0};bool finite=true;
    for(unsigned i=0;i<required;++i) {
        if(JS_ToFloat64(ctx,&v[i],argv[i])<0)return JS_EXCEPTION;
        finite &= isfinite(v[i]);
    }
    bool ccw=(magic==P_ARC||magic==P_ELLIPSE)&&argc>(int)required?JS_ToBool(ctx,argv[required]):false;
    d_canvas_sync(e);
    if(!finite)return JS_UNDEFINED;
    if((magic==P_ARC&&v[2]<0)||(magic==P_ELLIPSE&&(v[2]<0||v[3]<0)))
        return d_error(ctx,"IndexSizeError","Canvas arc radius is negative");
    if(magic==P_ARC_TO&&v[4]<0) {
        if(e->canvas) {
            canvas_status_t s=canvas_arc_to(e->canvas,v[0],v[1],v[2],v[3],v[4]);
            if(s!=CANVAS_BAD_ARGUMENT)return d_canvas_result(ctx,s);
        }
        return d_error(ctx,"IndexSizeError","Canvas arc radius is negative");
    }
    if(!e->canvas)return JS_UNDEFINED;
    canvas_status_t status=CANVAS_OK;
    switch(magic) {
    case P_BEGIN:status=canvas_begin_path(e->canvas);break;
    case P_CLOSE:case P_MOVE:case P_LINE:case P_QUAD:case P_CUBIC: {
        os64_geometry_command_t command={.verb=magic==P_CLOSE?OS64_GEOMETRY_CLOSE:magic==P_MOVE?OS64_GEOMETRY_MOVE:
            magic==P_LINE?OS64_GEOMETRY_LINE:magic==P_QUAD?OS64_GEOMETRY_QUADRATIC:OS64_GEOMETRY_CUBIC,
            .p={{v[0],v[1]},{v[2],v[3]},{v[4],v[5]}}};
        status=canvas_path_command(e->canvas,&command);break;
    }
    case P_RECT:status=canvas_path_rect(e->canvas,v[0],v[1],v[2],v[3]);break;
    case P_ARC:status=canvas_ellipse(e->canvas,v[0],v[1],v[2],v[2],0,v[3],v[4],ccw);break;
    case P_ELLIPSE:status=canvas_ellipse(e->canvas,v[0],v[1],v[2],v[3],v[4],v[5],v[6],ccw);break;
    case P_ARC_TO:status=canvas_arc_to(e->canvas,v[0],v[1],v[2],v[3],v[4]);break;
    case P_FILL:status=canvas_fill(e->canvas,even_odd);break;
    case P_STROKE:status=canvas_stroke(e->canvas);break;
    case P_CLIP:status=canvas_clip(e->canvas,even_odd);break;
    case P_STROKE_RECT:status=canvas_stroke_rect(e->canvas,v[0],v[1],v[2],v[3]);break;
    default:
        if(magic==P_TRANSLATE){transform.tx=v[0];transform.ty=v[1];}
        if(magic==P_SCALE){transform.a=v[0];transform.d=v[1];}
        if(magic==P_ROTATE){transform.a=transform.d=cos(v[0]);transform.b=sin(v[0]);transform.c=-transform.b;}
        if(magic==P_TRANSFORM||(magic==P_SET_TRANSFORM&&required))
            transform=(os64_geometry_map_t){v[0],v[1],v[2],v[3],v[4],v[5]};
        status=canvas_transform(e->canvas,&transform,magic==P_SET_TRANSFORM||magic==P_RESET_TRANSFORM);
    }
    return d_canvas_result(ctx,status);
}
int d_canvas_paths_install(os64_dom_t *dom,JSContext *ctx,JSValueConst prototype)
{
    const struct {const char *name;int id,length;} methods[]={
        {"beginPath",P_BEGIN,0},{"closePath",P_CLOSE,0},{"moveTo",P_MOVE,2},{"lineTo",P_LINE,2},
        {"quadraticCurveTo",P_QUAD,4},{"bezierCurveTo",P_CUBIC,6},{"rect",P_RECT,4},
        {"arc",P_ARC,5},{"ellipse",P_ELLIPSE,7},{"arcTo",P_ARC_TO,5},
        {"fill",P_FILL,0},{"stroke",P_STROKE,0},{"clip",P_CLIP,0},{"strokeRect",P_STROKE_RECT,4},
        {"translate",P_TRANSLATE,2},{"scale",P_SCALE,2},{"rotate",P_ROTATE,1},{"transform",P_TRANSFORM,6},
        {"setTransform",P_SET_TRANSFORM,0},{"resetTransform",P_RESET_TRANSFORM,0},
        {"setLineDash",P_DASH_SET,1},{"getLineDash",P_DASH_GET,0}};
    for(size_t i=0;i<sizeof(methods)/sizeof(*methods);++i)
        if(d_method_named(dom,ctx,prototype,methods[i].name,member,methods[i].length,methods[i].id)<0)return -1;
    const struct {const char *name;int id;} accessors[]={
        {"lineWidth",P_WIDTH},{"lineCap",P_CAP},{"lineJoin",P_JOIN},{"miterLimit",P_MITER},
        {"lineDashOffset",P_DASH_OFFSET},{"globalCompositeOperation",P_COMPOSITE}};
    for(size_t i=0;i<sizeof(accessors)/sizeof(*accessors);++i)
        if(d_accessor(dom,ctx,prototype,accessors[i].name,member,accessors[i].id,true)<0)return -1;
    return 0;
}
