#include "internal.h"
#include <svg/svg.h>

/* Retained SVG geometry is immutable. Copies share its bounded native owner;
 * the DOM registry releases each wrapper during document teardown. */
typedef struct DPath2D {
    size_t references;
    svg_context_t *owner;
    svg_path_t *path;
} DPath2D;
static JSClassID path2d_slot;
static JSValue result(JSContext *ctx,svg_status_t status)
{
    return d_error(ctx,status==SVG_LIMIT||status==SVG_NO_MEMORY?"QuotaExceededError":
        "InvalidStateError","Path2D operation refused");
}
static void release(os64_dom_t *dom,DPath2D *p)
{
    if(!p||--p->references)return;
    if(p->path)svg_path_release(p->owner,p->path);
    if(p->owner)svg_destroy(p->owner);
    d_canvas_release(dom,p,sizeof(*p));
}
void d_path2d_free(DValue *value)
{
    release(value->dom,value->canvas_path2d);value->canvas_path2d=NULL;
}
static JSValue construct(JSContext *ctx,JSValueConst new_target,int argc,JSValueConst *argv)
{
    os64_dom_t *dom=d_binding(ctx);if(!dom)return JS_EXCEPTION;
    DValue *source=argc?JS_GetOpaque(argv[0],dom->path2d_class):NULL;
    if(source&&source->dom!=dom)return JS_ThrowTypeError(ctx,"Path2D belongs to another document");
    size_t length=0;const char *text=NULL;
    if(!source&&argc&&!JS_IsUndefined(argv[0])) {
        text=JS_ToCStringLen(ctx,&length,argv[0]);if(!text)return JS_EXCEPTION;
    }
    JSValue prototype=JS_GetPropertyStr(ctx,new_target,"prototype");
    if(JS_IsException(prototype)){JS_FreeCString(ctx,text);return JS_EXCEPTION;}
    if(!JS_IsObject(prototype)) {
        JS_FreeValue(ctx,prototype);prototype=JS_GetClassProto(ctx,dom->path2d_class);
    }
    JSValue object=JS_NewObjectProtoClass(ctx,prototype,dom->path2d_class);JS_FreeValue(ctx,prototype);
    if(JS_IsException(object)){JS_FreeCString(ctx,text);return object;}
    DPath2D *path=NULL;
    svg_status_t status=SVG_OK;
    if(source) {path=source->canvas_path2d;path->references++;}
    else {
        os64_geometry_status_t allocated=d_canvas_allocate(dom,sizeof(*path),(void **)&path);
        if(allocated!=OS64_GEOMETRY_OK)status=allocated==OS64_GEOMETRY_LIMIT?SVG_LIMIT:SVG_NO_MEMORY;
        if(status==SVG_OK) {
            *path=(DPath2D){.references=1};
            svg_options_t options={.memory={dom,d_canvas_allocate,d_canvas_release},.limits=svg_default_limits()};
            options.limits.input_bytes=65536;options.limits.path_commands=4096;
            options.limits.geometry_commands=4096;options.limits.work=16u*1024*1024;
            status=svg_create(&options,&path->owner);
            svg_parse_result_t parsed;
            if(status==SVG_OK)status=svg_path_parse(path->owner,(svg_string_t){text?text:"",length},&path->path,&parsed);
            /* SVG syntax errors retain the complete prefix. Resource refusal
             * publishes no partial path and is distinct from malformed data. */
            if(status==SVG_MALFORMED)status=SVG_OK;
        }
    }
    JS_FreeCString(ctx,text);
    if(status!=SVG_OK){release(dom,path);JS_FreeValue(ctx,object);return result(ctx,status);}
    DValue *held=d_retain(dom,ctx,object);
    if(!held){release(dom,path);return JS_EXCEPTION;}
    held->dom=dom;held->canvas_path2d=path;JS_SetOpaque(object,held);
    return JS_DupValue(ctx,object);
}
JSValue d_path2d_draw(JSContext *ctx,DValue *context,DValue *value,canvas_path_operation_t operation,bool even_odd)
{
    canvas_state_t state;canvas_status_t status=canvas_state(context->canvas,&state);
    if(status!=CANVAS_OK)return d_canvas_result(ctx,status);
    DPath2D *path=value->canvas_path2d;svg_geometry_t *geometry=NULL;
    svg_status_t lowered=svg_path_lower(path->owner,path->path,&state.transform,1.0/128,&geometry);
    if(lowered!=SVG_OK)return result(ctx,lowered);
    size_t count=0;const os64_geometry_command_t *commands=svg_geometry_commands(geometry,&count);
    status=canvas_draw_path(context->canvas,commands,count,operation,even_odd);
    svg_geometry_release(path->owner,geometry);
    return d_canvas_result(ctx,status);
}
int d_path2d_install(os64_dom_t *dom,JSContext *ctx,JSValueConst global)
{
    dom->path2d_class=os64_js_class_id(&path2d_slot);
    const JSClassDef definition={.class_name="Path2D"};
    if(JS_NewClass(dom->engine,dom->path2d_class,&definition)<0)return -1;
    JSValue prototype=JS_NewObject(ctx);if(!d_retain(dom,ctx,prototype))return -1;
    JS_SetClassProto(ctx,dom->path2d_class,JS_DupValue(ctx,prototype));
    JSValue constructor=d_interface_new(ctx,"Path2D",0,construct,prototype,JS_UNDEFINED);
    if(JS_IsException(constructor))return -1;
    return JS_DefinePropertyValueStr(ctx,global,"Path2D",constructor,JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE);
}
