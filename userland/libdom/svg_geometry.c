#include "internal.h"
#include <math.h>

bool d_svg_geometry_node(const os64_html_node_t *n)
{
    if(!n || n->kind!=OS64_HTML_ELEMENT || n->ns!=OS64_HTML_NS_SVG)return false;
    const char *names[]={"path","rect","circle","ellipse","line","polyline","polygon"};
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)if(os64_streq(n->name,names[i]))return true;
    return false;
}
JSValueConst d_svg_prototype(os64_dom_t *dom,const os64_html_node_t *n)
{
    if(dom->svg_geometry_proto && d_svg_geometry_node(n))return dom->svg_geometry_proto->value;
    if(dom->svg_graphics_proto && n->kind==OS64_HTML_ELEMENT && n->ns==OS64_HTML_NS_SVG) {
        const char *names[]={"svg","g","use","image","text","foreignObject"};
        for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)
            if(os64_streq(n->name,names[i]))return dom->svg_graphics_proto->value;
    }
    return dom->prototypes[d_node_kind(n)]->value;
}
void os64_dom_set_svg_hit(os64_dom_t *dom,os64_dom_svg_hit_provider_t provider,void *opaque)
{
    if(!dom||dom->closed)return;
    dom->svg_hit=provider;dom->svg_hit_opaque=opaque;
}
static uint64_t add_sat(uint64_t a,uint64_t b) {return b>UINT64_MAX-a?UINT64_MAX:a+b;}
static bool cancelled(void *opaque)
{
    os64_dom_t *dom=opaque;os64_js_outcome_t out;
    return os64_js_check_budget(dom->runtime,OS64_JS_ABI_ID,&out)!=OS64_JS_OK;
}
static JSValue contains(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)magic;
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);if(!dom)return JS_EXCEPTION;
    const os64_html_node_t *n=d_node(dom,ctx,self);if(!n)return JS_EXCEPTION;
    if(!d_svg_geometry_node(n))return JS_ThrowTypeError(ctx,"Expected SVG geometry");
    double p[4];if(d_point_dictionary(ctx,argc?argv[0]:JS_UNDEFINED,p)<0)return JS_EXCEPTION;
    if(!isfinite(p[0])||!isfinite(p[1]))return JS_FALSE;
    if(!dom->svg_hit)return d_error(ctx,"InvalidStateError","No SVG geometry provider");
    if(cancelled(dom))return d_error(ctx,"InvalidStateError","SVG query requires a live script budget");
    os64_geometry_memory_t memory={dom,d_canvas_allocate,d_canvas_release};
    os64_dom_geometry_stats_t cost={0};bool inside=false;
    os64_geometry_status_t result=dom->svg_hit(dom->svg_hit_opaque,n,p[0],p[1],
        &memory,cancelled,dom,&inside,&cost);
    dom->geometry_stats.layouts=add_sat(dom->geometry_stats.layouts,cost.layouts);
    dom->geometry_stats.elapsed_us=add_sat(dom->geometry_stats.elapsed_us,cost.elapsed_us);
    if(cancelled(dom))return d_error(ctx,"InvalidStateError","SVG query requires a live script budget");
    if(result!=OS64_GEOMETRY_OK)return d_error(ctx,
        result==OS64_GEOMETRY_LIMIT||result==OS64_GEOMETRY_NO_MEMORY?"QuotaExceededError":"InvalidStateError",
        "SVG fill query refused");
    return JS_NewBool(ctx,inside);
}
int d_svg_geometry_prototypes(os64_dom_t *dom,JSContext *ctx)
{
    JSValue graphics=JS_NewObjectProto(ctx,dom->prototypes[D_PROTO_SVG]->value);
    dom->svg_graphics_proto=d_retain(dom,ctx,graphics);
    if(!dom->svg_graphics_proto)return -1;
    JSValue geometry=JS_NewObjectProto(ctx,graphics);
    dom->svg_geometry_proto=d_retain(dom,ctx,geometry);
    return dom->svg_geometry_proto?0:-1;
}
int d_svg_geometry_install(os64_dom_t *dom,JSContext *ctx,JSValueConst global)
{
    JSValue parent=JS_GetPropertyStr(ctx,global,"SVGElement");
    if(JS_IsException(parent))return -1;
    for(unsigned i=0;i<2;i++) {
        JSValue proto=i?dom->svg_geometry_proto->value:dom->svg_graphics_proto->value;
        const char *name=i?"SVGGeometryElement":"SVGGraphicsElement";
        JSValue constructor=d_interface_new(ctx,name,0,NULL,proto,parent);
        JS_FreeValue(ctx,parent);
        if(JS_IsException(constructor))return -1;
        if(JS_DefinePropertyValueStr(ctx,global,name,JS_DupValue(ctx,constructor),JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE)<0) {
            JS_FreeValue(ctx,constructor);return -1;
        }
        parent=constructor;
    }
    JS_FreeValue(ctx,parent);
    return d_method(dom,ctx,dom->svg_geometry_proto->value,"isPointInFill",contains,0,0);
}
