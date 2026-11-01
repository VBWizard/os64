#include "internal.h"
#include "dom/canvas.h"
#include "garb/garb.h"
#include "garb/values.h"
#include "os64/fmt.h"
#include <math.h>
#include <stddef.h>

/* Native pixels have a separate page ledger: the small DOM-record quota is
 * not a bitmap quota. The aligned header preserves geometry's allocator ABI. */
#define CANVAS_PAGE_BYTES ((size_t)128 * 1024 * 1024)
typedef union { max_align_t alignment; size_t bytes; } CanvasAllocation;
static JSClassID canvas_slot, gradient_slot;
os64_geometry_status_t d_canvas_allocate(void *opaque, size_t size, void **out)
{
    os64_dom_t *dom = opaque;
    *out = NULL;
    if (size > SIZE_MAX - sizeof(CanvasAllocation)) return OS64_GEOMETRY_LIMIT;
    size_t total = size + sizeof(CanvasAllocation);
    if (total > CANVAS_PAGE_BYTES - dom->canvas_bytes) return OS64_GEOMETRY_LIMIT;
    CanvasAllocation *p = os64_malloc(total);
    if (!p) return OS64_GEOMETRY_NO_MEMORY;
    p->bytes = total;
    dom->canvas_bytes += total;
    *out = p + 1;
    return OS64_GEOMETRY_OK;
}
void d_canvas_release(void *opaque, void *pointer, size_t size)
{
    (void)size;
    if (!pointer) return;
    os64_dom_t *dom = opaque;
    CanvasAllocation *p = (CanvasAllocation *)pointer - 1;
    dom->canvas_bytes -= p->bytes;
    os64_free(p);
}
static uint32_t dimension(const os64_html_node_t *node, const char *name, uint32_t fallback)
{
    const os64_html_attr_t *a = os64_html_attr(node, name);
    int64_t n = a ? d_non_negative(a->value) : -1;
    return n >= 0 && n <= INT32_MAX ? (uint32_t)n : fallback;
}
static canvas_status_t create(DValue *e)
{
    canvas_options_t options = canvas_options_default((os64_geometry_memory_t){
        .context = e->dom, .alloc = d_canvas_allocate, .free = d_canvas_release});
    options.geometry = os64_freetype_geometry_v1();
    return canvas_create(&options, dimension(e->node, "width", 300),
                         dimension(e->node, "height", 150), &e->canvas);
}
void d_canvas_sync(DValue *e)
{
    if (!e->canvas_reset_pending) return;
    e->canvas_reset_pending = false;
    canvas_status_t status = e->canvas ? canvas_reset(e->canvas,
        dimension(e->node, "width", 300), dimension(e->node, "height", 150)) : create(e);
    /* A failed DOM resize cannot keep displaying pixels at the old size.
     * Drop the bitmap; the stable JS context is lost until a later resize
     * can allocate it. Reads of a lost context produce transparent pixels. */
    if (status != CANVAS_OK) {
        canvas_destroy(e->canvas);
        e->canvas = NULL;
    }
}
canvas_status_t d_canvas_prepare(DValue *e)
{
    d_canvas_sync(e);
    return e->canvas ? CANVAS_OK : e->canvas_object ? CANVAS_NO_MEMORY : create(e);
}
static void changed(os64_dom_t *dom, const os64_html_change_t *c)
{
    d_image_changed(dom,c);
    if (c->kind != OS64_HTML_CHANGE_ATTR || c->attr_ns ||
        (os64_strcmp(c->attr_local, "width") && os64_strcmp(c->attr_local, "height"))) return;
    DValue *e = d_find(dom, c->target);
    if (e && (e->canvas_object || e->canvas)) e->canvas_reset_pending = true;
}
void d_canvas_free(DValue *e)
{
    if (e->canvas) canvas_destroy(e->canvas);
    e->canvas = NULL;
    canvas_gradient_release(e->canvas_gradient);
    e->canvas_gradient = NULL;
}
JSValue d_canvas_result(JSContext *ctx, canvas_status_t status)
{
    if (status == CANVAS_OK) return JS_UNDEFINED;
    return d_error(ctx, status == CANVAS_LIMIT || status == CANVAS_NO_MEMORY ?
        "QuotaExceededError" : status == CANVAS_UNSUPPORTED ? "NotSupportedError" : "InvalidStateError", "Canvas operation refused");
}
/* The unsupported creation profiles are refused, so callers do not mistake
 * a transparent sRGB bitmap for an opaque or wide-gamut one. */
static int settings(JSContext *ctx, JSValueConst object)
{
    if (JS_IsUndefined(object) || JS_IsNull(object)) return 1;
    if (!JS_IsObject(object)) return 1;
    const char *names[] = {"alpha", "colorSpace", "colorType", "desynchronized", "willReadFrequently"};
    bool supported = true;
    for (unsigned i = 0; i < 5; ++i) {
        JSValue v = JS_GetPropertyStr(ctx, object, names[i]);
        if (JS_IsException(v)) return -1;
        if (!JS_IsUndefined(v)) {
            if (i == 1 || i == 2) {
                size_t n; const char *s = JS_ToCStringLen(ctx, &n, v);
                if (!s) { JS_FreeValue(ctx, v); return -1; }
                const char *yes = i == 1 ? "srgb" : "unorm8";
                const char *no = i == 1 ? "display-p3" : "float16";
                bool primary = n == os64_strlen(yes) && !os64_memcmp(s, yes, n);
                bool secondary = n == os64_strlen(no) && !os64_memcmp(s, no, n);
                JS_FreeCString(ctx, s);
                if (!primary && !secondary) {
                    JS_FreeValue(ctx, v); JS_ThrowTypeError(ctx, "Invalid Canvas %s", names[i]); return -1;
                }
                supported &= primary;
            } else if (i == 0) supported &= JS_ToBool(ctx, v) != 0;
        }
        JS_FreeValue(ctx, v);
    }
    return supported;
}
static JSValue get_context(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                            int magic, JSValue *data)
{
    (void)magic;
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (!dom) return JS_EXCEPTION;
    const os64_html_node_t *node = d_node(dom, ctx, self);
    if (!node || d_node_kind(node) != D_PROTO_CANVAS)
        return JS_ThrowTypeError(ctx, "Expected an HTMLCanvasElement");
    if (!argc) return JS_ThrowTypeError(ctx, "getContext requires a context type");
    size_t n; const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) return JS_EXCEPTION;
    bool two = n == 2 && !os64_memcmp(s, "2d", 2);
    JS_FreeCString(ctx, s);
    if (!two) return JS_NULL;
    DValue *e = d_find(dom, node);
    if (e->canvas_object) return JS_DupValue(ctx, e->canvas_object->value);
    int supported = settings(ctx, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (supported < 0) return JS_EXCEPTION;
    /* A dictionary getter may itself have created the context. */
    if (e->canvas_object) return JS_DupValue(ctx, e->canvas_object->value);
    if (!supported || d_canvas_prepare(e) != CANVAS_OK) return JS_NULL;
    JSValue object = JS_NewObjectClass(ctx, dom->canvas_class);
    if (JS_IsException(object)) { d_canvas_free(e); return object; }
    DValue *held = d_retain(dom, ctx, object);
    if (!held) { d_canvas_free(e); return JS_EXCEPTION; }
    JS_SetOpaque(object, e);
    e->canvas_object = held;
    return JS_DupValue(ctx, object);
}
static int integer(JSContext *ctx, JSValueConst v, int64_t *out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v) < 0) return -1;
    d = trunc(d);
    if (!isfinite(d) || d < INT32_MIN || d > INT32_MAX) {
        JS_ThrowTypeError(ctx, "Canvas pixel coordinate is outside the long range"); return -1;
    }
    *out = (int64_t)d;
    return 0;
}
static uint8_t byte(double v) { return (uint8_t)(v <= 0 ? 0 : v >= 255 ? 255 : floor(v + .5)); }
static int color(JSContext *ctx, JSValueConst value, uint32_t *out, bool *current, double *alpha)
{
    size_t n; const char *s = JS_ToCStringLen(ctx, &n, value);
    if (!s) return -1;
    garb_parsed_t parsed = {0}; garb_color_t c;
    garb_status_t status = n <= 65536 ? garb_parse_values_limited(s, n, 2u * 1024 * 1024, &parsed) : GARB_TOO_BIG;
    bool valid = status == GARB_OK && !parsed.incomplete && garb_read_color(parsed.values, parsed.nvalues, &c);
    JS_FreeCString(ctx, s);
    *current = valid && c.current;
    if (valid) *alpha = c.current ? 1 : c.a;
    if (valid) *out = c.current ? 0xff000000 : ((uint32_t)byte(c.a * 255) << 24) |
        ((uint32_t)byte(c.r) << 16) | ((uint32_t)byte(c.g) << 8) | byte(c.b);
    bool refused = status == GARB_NO_MEMORY || status == GARB_TOO_BIG || parsed.incomplete;
    garb_free(&parsed);
    if (refused) { d_error(ctx, "QuotaExceededError", "Canvas color parse refused"); return -1; }
    return valid;
}
static JSValue gradient_method(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);
    if(!dom)return JS_EXCEPTION;
    if(magic==3) {
        DValue *g=JS_GetOpaque(self,dom->gradient_class);
        if(!g||g->dom!=dom)return JS_ThrowTypeError(ctx,"Expected a CanvasGradient");
        if(argc<2)return JS_ThrowTypeError(ctx,"addColorStop requires offset and color");
        double offset;if(JS_ToFloat64(ctx,&offset,argv[0])<0)return JS_EXCEPTION;
        if(!isfinite(offset))return JS_ThrowTypeError(ctx,"Gradient offset must be finite");
        /* WebIDL converts both arguments before the range check. */
        size_t n;const char *string=JS_ToCStringLen(ctx,&n,argv[1]);if(!string)return JS_EXCEPTION;
        JSValue value=JS_NewStringLen(ctx,string,n);JS_FreeCString(ctx,string);
        if(JS_IsException(value))return value;
        if(offset<0||offset>1){JS_FreeValue(ctx,value);return d_error(ctx,"IndexSizeError","Gradient offset is outside [0,1]");}
        uint32_t argb;bool current;double alpha;int valid=color(ctx,value,&argb,&current,&alpha);JS_FreeValue(ctx,value);
        if(valid<0)return JS_EXCEPTION;
        if(!valid)return d_error(ctx,"SyntaxError","Invalid gradient color");
        return d_canvas_result(ctx,canvas_gradient_add_stop(g->canvas_gradient,offset,argb));
    }
    DValue *e=JS_GetOpaque(self,dom->canvas_class);
    if(!e||e->dom!=dom)return JS_ThrowTypeError(ctx,"Expected a CanvasRenderingContext2D");
    unsigned count=magic==0?4:magic==1?6:3;
    if(argc<(int)count)return JS_ThrowTypeError(ctx,"Gradient requires coordinates");
    double v[6];for(unsigned i=0;i<count;++i) {
        if(JS_ToFloat64(ctx,&v[i],argv[i])<0)return JS_EXCEPTION;
        if(!isfinite(v[i]))return JS_ThrowTypeError(ctx,"Gradient coordinates must be finite");
    }
    if(magic==1&&(v[2]<0||v[5]<0))return d_error(ctx,"IndexSizeError","Gradient radius is negative");
    canvas_gradient_t *gradient=NULL;
    canvas_status_t status=canvas_gradient_create((os64_geometry_memory_t){dom,d_canvas_allocate,d_canvas_release},magic,v,&gradient);
    if(status!=CANVAS_OK)return d_canvas_result(ctx,status);
    JSValue object=JS_NewObjectClass(ctx,dom->gradient_class);
    if(JS_IsException(object)){canvas_gradient_release(gradient);return object;}
    DValue *held=d_retain(dom,ctx,object);
    if(!held){canvas_gradient_release(gradient);return JS_EXCEPTION;}
    held->dom=dom;held->canvas_gradient=gradient;JS_SetOpaque(object,held);
    return JS_DupValue(ctx,object);
}
static int gradients_install(os64_dom_t *dom,JSContext *ctx,JSValueConst global,JSValueConst context)
{
    dom->gradient_class=os64_js_class_id(&gradient_slot);
    const JSClassDef definition={.class_name="CanvasGradient"};
    if(JS_NewClass(dom->engine,dom->gradient_class,&definition)<0)return -1;
    JSValue proto=JS_NewObject(ctx);if(!d_retain(dom,ctx,proto))return -1;
    JS_SetClassProto(ctx,dom->gradient_class,JS_DupValue(ctx,proto));
    JSValue constructor=d_interface_new(ctx,"CanvasGradient",0,NULL,proto,JS_UNDEFINED);
    if(JS_IsException(constructor)||JS_DefinePropertyValueStr(ctx,global,"CanvasGradient",constructor,JS_PROP_WRITABLE|JS_PROP_CONFIGURABLE)<0)return -1;
    if(d_method_named(dom,ctx,proto,"addColorStop",gradient_method,2,3)<0)return -1;
    const char *names[]={"createLinearGradient","createRadialGradient","createConicGradient"};
    for(unsigned i=0;i<3;++i)if(d_method_named(dom,ctx,context,names[i],gradient_method,i==0?4:i==1?6:3,i)<0)return -1;
    return 0;
}
enum { C_CANVAS, C_FILL, C_ALPHA, C_SAVE, C_RESTORE, C_FILL_RECT, C_CLEAR_RECT,
       C_CREATE_IMAGE, C_GET_IMAGE, C_PUT_IMAGE, C_LOST, C_RESET, C_STROKE, C_SHADOW_COLOR, C_SHADOW_BLUR, C_SHADOW_X, C_SHADOW_Y };
static JSValue member(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv,
                       int magic, JSValue *data)
{
    os64_dom_t *dom = d_callback(ctx, data, OS64_JS_ABI_ID);
    if (!dom) return JS_EXCEPTION;
    DValue *e = JS_GetOpaque(self, dom->canvas_class);
    if (!e || e->dom != dom) return JS_ThrowTypeError(ctx, "Expected a CanvasRenderingContext2D");
    bool set = magic & D_SET;
    magic &= ~D_SET;
    if (magic == C_CANVAS) return JS_DupValue(ctx, e->value);
    if (magic == C_FILL || magic == C_STROKE || magic == C_SHADOW_COLOR || magic == C_ALPHA) {
        uint32_t fill = 0; double alpha = 1;
        if (set) {
            if (magic == C_FILL || magic == C_STROKE || magic == C_SHADOW_COLOR) {
                DValue *gradient=argc?JS_GetOpaque(argv[0],dom->gradient_class):NULL;
                if(magic!=C_SHADOW_COLOR&&gradient&&gradient->dom==dom) {
                    d_canvas_sync(e);
                    return e->canvas?d_canvas_result(ctx,canvas_set_gradient(e->canvas,gradient->canvas_gradient,magic==C_STROKE)):JS_UNDEFINED;
                }
                bool current = false;
                int ok = color(ctx, argc ? argv[0] : JS_UNDEFINED, &fill, &current, &alpha);
                if (ok > 0 && current) {
                    JSValue resolved = d_computed_color(dom, ctx, e->node);
                    if (JS_IsException(resolved)) return JS_EXCEPTION;
                    ok = color(ctx, resolved, &fill, &current, &alpha);
                    JS_FreeValue(ctx, resolved);
                }
                if (ok < 0) return JS_EXCEPTION;
                if (!ok) return JS_UNDEFINED;
            } else if (JS_ToFloat64(ctx, &alpha, argc ? argv[0] : JS_UNDEFINED) < 0) return JS_EXCEPTION;
        }
        d_canvas_sync(e);
        if (set) return e->canvas ? d_canvas_result(ctx, magic == C_FILL ? canvas_set_fill_rgb_alpha(e->canvas, fill, alpha) :
                    magic == C_STROKE ? canvas_set_stroke(e->canvas, fill, alpha) :
                    magic == C_SHADOW_COLOR ? canvas_set_shadow_color(e->canvas, fill, alpha) :
                                                         canvas_set_alpha(e->canvas, alpha)) : JS_UNDEFINED;
        canvas_state_t state = {.fill_argb=0xff000000,.global_alpha=1,.fill_alpha=1, .stroke_argb=0xff000000,.stroke_alpha=1};
        if (e->canvas) canvas_state(e->canvas, &state);
        if (magic == C_ALPHA) return JS_NewFloat64(ctx, state.global_alpha);
        canvas_gradient_t *gradient=magic==C_SHADOW_COLOR?NULL:magic==C_STROKE?state.stroke_gradient:state.fill_gradient;
        if(gradient)for(DValue *held=dom->values;held;held=held->next)
            if(held->canvas_gradient==gradient)return JS_DupValue(ctx,held->value);
        char text[80]; uint32_t p = magic == C_SHADOW_COLOR ? state.shadow_argb : magic == C_STROKE ? state.stroke_argb : state.fill_argb;
        double paint_alpha = magic == C_SHADOW_COLOR ? state.shadow_alpha : magic == C_STROKE ? state.stroke_alpha : state.fill_alpha;
        if (paint_alpha == 1) os64_snprintf(text, sizeof(text), "#%06x", p & 0xffffff);
        else {
            garb_val_t value = {.kind = GARB_V_COLOR, .color = {
                (p >> 16) & 255, (p >> 8) & 255, p & 255, paint_alpha, false}};
            garb_val_dump(&value, text, sizeof(text));
        }
        return JS_NewString(ctx, text);
    }
    if(magic>=C_SHADOW_BLUR && magic<=C_SHADOW_Y) {
        double value=0;
        if(set && JS_ToFloat64(ctx,&value,argc?argv[0]:JS_UNDEFINED)<0)return JS_EXCEPTION;
        d_canvas_sync(e);
        if(set)return e->canvas?d_canvas_result(ctx,canvas_set_shadow_value(e->canvas,magic-C_SHADOW_BLUR,value)):JS_UNDEFINED;
        canvas_state_t state={0};if(e->canvas)canvas_state(e->canvas,&state);
        return JS_NewFloat64(ctx,magic==C_SHADOW_BLUR?state.shadow_blur:magic==C_SHADOW_X?state.shadow_x:state.shadow_y);
    }
    if (magic == C_FILL_RECT || magic == C_CLEAR_RECT) {
        if (argc < 4) return JS_ThrowTypeError(ctx, "Canvas rectangle requires four coordinates");
        double v[4];
        for (unsigned i = 0; i < 4; ++i) if (JS_ToFloat64(ctx, &v[i], argv[i]) < 0) return JS_EXCEPTION;
        d_canvas_sync(e);
        if (!e->canvas) return JS_UNDEFINED;
        return d_canvas_result(ctx, magic == C_FILL_RECT ? canvas_fill_rect(e->canvas, v[0], v[1], v[2], v[3]) :
                                                   canvas_clear_rect(e->canvas, v[0], v[1], v[2], v[3]));
    }
    if (magic == C_CREATE_IMAGE || magic == C_GET_IMAGE) {
        int64_t x = 0, y = 0, w = 0, h = 0;
        JSValue array; uint32_t iw, ih;
        if (magic == C_CREATE_IMAGE && argc == 1 && d_image_data_parts(argv[0], &array, &iw, &ih))
            return d_image_data_new(ctx, iw, ih);
        unsigned start = magic == C_GET_IMAGE ? 2 : 0;
        if (argc < (int)start + 2) return JS_ThrowTypeError(ctx, "Canvas pixel read requires dimensions");
        if (start && (integer(ctx, argv[0], &x) < 0 || integer(ctx, argv[1], &y) < 0)) return JS_EXCEPTION;
        if (integer(ctx, argv[start], &w) < 0 || integer(ctx, argv[start+1], &h) < 0) return JS_EXCEPTION;
        if (d_image_data_settings(ctx, argc > (int)start + 2 ? argv[start+2] : JS_UNDEFINED) < 0) return JS_EXCEPTION;
        if (w < 0) { x += w; w = -w; } if (h < 0) { y += h; h = -h; }
        if ((uint64_t)w * h > 8u * 1024 * 1024)
            return d_error(ctx, "QuotaExceededError", "Canvas pixel read exceeds its work budget");
        if(magic==C_GET_IMAGE) {
            d_canvas_sync(e);
            if(w && h && e->canvas && !canvas_origin_clean(e->canvas))
                return d_error(ctx,"SecurityError","Canvas contains an image from another origin");
        }
        JSValue image = d_image_data_new(ctx, (uint32_t)w, (uint32_t)h);
        if (JS_IsException(image)) return image;
        if (magic == C_GET_IMAGE) {
            d_canvas_sync(e);
            canvas_rgba8_t view;
            if (d_image_data_view(dom, ctx, image, &view) < 0) { JS_FreeValue(ctx, image); return JS_EXCEPTION; }
            if (e->canvas) {
                canvas_status_t status = canvas_read_pixels(e->canvas, x, y, &view);
                if (status != CANVAS_OK) { JS_FreeValue(ctx, image); return d_canvas_result(ctx, status); }
            }
        }
        return image;
    }
    if (magic == C_PUT_IMAGE) {
        if (argc < 3 || (argc > 3 && argc < 7))
            return JS_ThrowTypeError(ctx, "putImageData requires three or seven arguments");
        JSValue array; uint32_t w, h;
        if (!d_image_data_parts(argv[0], &array, &w, &h)) return JS_ThrowTypeError(ctx, "Expected an ImageData");
        int64_t v[6] = {0, 0, 0, 0, w, h};
        unsigned count = argc >= 7 ? 6 : 2;
        for (unsigned i = 0; i < count; ++i) if (integer(ctx, argv[i+1], &v[i]) < 0) return JS_EXCEPTION;
        if (v[4] < 0) { v[2] += v[4]; v[4] = -v[4]; }
        if (v[5] < 0) { v[3] += v[5]; v[5] = -v[5]; }
        int64_t x0 = v[2] > 0 ? v[2] : 0, y0 = v[3] > 0 ? v[3] : 0;
        int64_t x1 = v[2] + v[4] < w ? v[2] + v[4] : w;
        int64_t y1 = v[3] + v[5] < h ? v[3] + v[5] : h;
        d_canvas_sync(e);
        canvas_rgba8_t view;
        if (d_image_data_view(dom, ctx, argv[0], &view) < 0) return JS_EXCEPTION;
        if (!e->canvas || x1 <= x0 || y1 <= y0) return JS_UNDEFINED;
        draw2d_damage_t dirty = {(uint32_t)x0, (uint32_t)y0, (uint32_t)x1, (uint32_t)y1};
        return d_canvas_result(ctx, canvas_put_pixels(e->canvas, &view, v[0], v[1], &dirty));
    }
    if (magic == C_RESET) e->canvas_reset_pending = true;
    d_canvas_sync(e);
    if (magic == C_LOST) return JS_NewBool(ctx, !e->canvas);
    if (!e->canvas || magic == C_RESET) return JS_UNDEFINED;
    return d_canvas_result(ctx, magic == C_SAVE ? canvas_save(e->canvas) : canvas_restore(e->canvas));
}
int d_canvas_install(os64_dom_t *dom, JSContext *ctx, JSValueConst global)
{
    dom->canvas_class = os64_js_class_id(&canvas_slot);
    const JSClassDef definition = {.class_name = "CanvasRenderingContext2D"};
    if (JS_NewClass(dom->engine, dom->canvas_class, &definition) < 0) return -1;
    JSValue prototype = JS_NewObject(ctx);
    if (!d_retain(dom, ctx, prototype)) return -1;
    JS_SetClassProto(ctx, dom->canvas_class, JS_DupValue(ctx, prototype));
    JSValue constructor = d_interface_new(ctx, "CanvasRenderingContext2D", 0, NULL, prototype, JS_UNDEFINED);
    if (JS_IsException(constructor) || JS_DefinePropertyValueStr(ctx, global, "CanvasRenderingContext2D",
        constructor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) return -1;
    const char *accessors[] = {"canvas", "fillStyle", "globalAlpha"};
    for (unsigned i = 0; i < 3; ++i)
        if (d_accessor(dom, ctx, prototype, accessors[i], member, i, i != 0) < 0) return -1;
    const char *shadows[]={"shadowColor","shadowBlur","shadowOffsetX","shadowOffsetY"};
    for(unsigned i=0;i<4;i++)if(d_accessor(dom,ctx,prototype,shadows[i],member,C_SHADOW_COLOR+i,true)<0)return -1;
    if (d_accessor(dom, ctx, prototype, "strokeStyle", member, C_STROKE, true) < 0 ||
        d_canvas_paths_install(dom, ctx, prototype) < 0 ||
        d_canvas_images_install(dom,ctx,prototype) < 0 ||
        gradients_install(dom,ctx,global,prototype) < 0 ||
        d_canvas_text_install(dom,ctx,global,prototype) < 0) return -1;
    const struct { const char *name; int magic, length; } methods[] = {
        {"save", C_SAVE, 0}, {"restore", C_RESTORE, 0}, {"fillRect", C_FILL_RECT, 4},
        {"clearRect", C_CLEAR_RECT, 4}, {"createImageData", C_CREATE_IMAGE, 2},
        {"getImageData", C_GET_IMAGE, 4}, {"putImageData", C_PUT_IMAGE, 3},
        {"isContextLost", C_LOST, 0}, {"reset", C_RESET, 0}};
    for (unsigned i = 0; i < sizeof(methods)/sizeof(*methods); ++i)
        if (d_method_named(dom, ctx, prototype, methods[i].name, member, methods[i].length, methods[i].magic) < 0)
            return -1;
    if (d_method_named(dom, ctx, dom->prototypes[D_PROTO_CANVAS]->value, "getContext", get_context, 1, 0) < 0)
        return -1;
    d_mutation_register(dom, &dom->canvas_mutations, changed);
    return 0;
}

bool os64_dom_canvas_bitmap(os64_dom_t *dom, const os64_html_node_t *node, canvas_bitmap_t *out)
{
    if (!dom || dom->closed || !out) return false;
    DValue *e = d_find(dom, node);
    if (!e || !e->canvas_object) return false;
    d_canvas_sync(e);
    return e->canvas && canvas_bitmap(e->canvas, out) == CANVAS_OK;
}
bool os64_dom_canvas_dirty(os64_dom_t *dom, bool (*visible)(void *, const os64_html_node_t *), void *opaque)
{
    if (!dom || dom->closed) return false;
    for (DValue *e = dom->values; e; e = e->next) {
        if (!e->canvas_object || !d_in_window_tree(dom, e->node) || (visible && !visible(opaque, e->node)))
            continue;
        if (e->canvas_reset_pending) return true;
        draw2d_damage_t damage;
        if (e->canvas && canvas_damage(e->canvas, &damage) == CANVAS_OK &&
            damage.x0 < damage.x1 && damage.y0 < damage.y1) return true;
    }
    return false;
}
void os64_dom_canvas_visit_damage(os64_dom_t *dom,os64_dom_canvas_damage_fn visit,void *opaque)
{
    if(!dom || dom->closed || !visit)return;
    for(DValue *e=dom->values;e;e=e->next) {
        if(!e->canvas_object || !d_in_window_tree(dom,e->node))continue;
        if(e->canvas_reset_pending){visit(opaque,e->node,0,0,NULL);continue;}
        draw2d_damage_t damage;canvas_bitmap_t bitmap;
        if(e->canvas && canvas_damage(e->canvas,&damage)==CANVAS_OK &&
           damage.x0<damage.x1 && damage.y0<damage.y1 && canvas_bitmap(e->canvas,&bitmap)==CANVAS_OK)
            visit(opaque,e->node,bitmap.width,bitmap.height,&damage);
    }
}
void os64_dom_canvas_painted(os64_dom_t *dom, const os64_html_node_t *node)
{
    if (!dom || dom->closed) return;
    DValue *e = d_find(dom, node);
    if (e && e->canvas) canvas_ack_damage(e->canvas);
}
