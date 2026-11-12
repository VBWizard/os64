#include "internal.h"
#include <png/png.h>
#include <math.h>

enum { I_DRAW,I_SMOOTH,I_QUALITY,I_EXPORT,I_WIDTH,I_HEIGHT,I_COMPLETE,I_CORS,I_CURRENT };
void os64_dom_set_images(os64_dom_t *dom,const os64_dom_image_provider_t *provider)
{
    if(dom&&!dom->closed)dom->image_provider=provider?*provider:(os64_dom_image_provider_t){0};
}
typedef struct {
    os64_dom_t *owner;
    os64_dom_image_provider_t provider;
} DImageLoan;

static os64_dom_image_status_t image_acquire(os64_dom_t *dom,const os64_html_node_t *node,
                                             bool pixels,os64_dom_image_view_t *view,DImageLoan *loan)
{
    *view=(os64_dom_image_view_t){0};
    *loan=(DImageLoan){0};
    if(!dom->image_provider.acquire||!d_provider_enter(dom))return OS64_DOM_IMAGE_PENDING;
    *loan=(DImageLoan){dom,dom->image_provider};
    return loan->provider.acquire(loan->provider.opaque,node,pixels,view);
}
static void image_release(DImageLoan *loan,os64_dom_image_view_t *view)
{
    if(!loan->owner)return;
    /* Departure stops new acquisitions, but this producer still owns the
     * borrowed pixels/text until its matching release returns. */
    if(loan->provider.release)loan->provider.release(loan->provider.opaque,view);
    d_provider_leave(loan->owner);
    loan->owner=NULL;
}
static void image_update(os64_dom_t *dom)
{
    if(dom->image_provider.update&&d_provider_enter(dom)) {
        dom->image_provider.update(dom->image_provider.opaque);
        d_provider_leave(dom);
    }
}
static double image_density(const os64_dom_image_view_t *view)
{ return view->density_set?view->density:1; }
static uint32_t image_dimension(const os64_dom_image_view_t *view,bool height)
{
    double d=image_density(view), n=height?view->surface.height:view->surface.width;
    n=d>0?n/d:UINT32_MAX;
    return n>UINT32_MAX?UINT32_MAX:n>0?(uint32_t)n:0;
}
static bool image_attribute_size(const os64_html_attr_t *attr,uint32_t *out)
{
    if(!attr)return false;
    const char *s=attr->value;
    while(*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f')s++;
    bool negative=*s=='-';
    if(*s=='+'||*s=='-')s++;
    if(*s<'0'||*s>'9')return false;
    uint64_t value=0;
    while(*s>='0'&&*s<='9') {
        value=value*10+(unsigned)(*s++-'0');
        if(value>UINT32_MAX){*out=0;return !negative;}
    }
    if(negative&&value)return false;
    *out=(uint32_t)value;return true;
}
JSValue d_image_dimension(os64_dom_t *dom,JSContext *ctx,const os64_html_node_t *node,bool height)
{
    /* Texture images created with new Image() need no layout: absent or
     * invalid dimension attributes fall back to the loaded image size. */
    dom=d_owner(dom,node);
    uint32_t value;
    if(image_attribute_size(os64_html_attr(node,height?"height":"width"),&value))
        return JS_NewUint32(ctx,value);
    dom=d_owner(dom,node);
    if(!d_require_active(dom,ctx))return JS_EXCEPTION;
    os64_dom_image_view_t view;
    DImageLoan loan;
    os64_dom_image_status_t status=image_acquire(dom,node,false,&view,&loan);
    value=status==OS64_DOM_IMAGE_READY?image_dimension(&view,height):0;
    image_release(&loan,&view);
    return JS_NewUint32(ctx,value);
}
void d_image_changed(os64_dom_t *dom,const os64_html_change_t *change)
{
    if(change->kind!=OS64_HTML_CHANGE_ATTR||change->attr_ns||
       (os64_strcmp(change->attr_local,"src")&&os64_strcmp(change->attr_local,"crossorigin")&&
        os64_strcmp(change->attr_local,"srcset")&&os64_strcmp(change->attr_local,"sizes")))return;
    DValue *e=d_find(dom,change->target);
    if(e && !e->image_generation)e->image_reported=false;
}
bool os64_dom_image_event(os64_dom_t *dom,const os64_html_node_t **node,bool *failed,bool consume)
{
    if(!d_delivery_active(dom)||!dom->options.modern||!dom->image_provider.acquire)return false;
    image_update(dom);
    if(!d_delivery_active(dom))return false;
    size_t members=dom->cluster?dom->cluster->count:1;
    for(size_t i=0;i<members;++i)
    for(DValue *e=(dom->cluster?dom->cluster->members[i]:dom)->values;e;e=e->next) {
        if(e->node && d_owner(dom,e->node)!=dom)continue;
        if(!e->node||e->node->ns!=OS64_HTML_NS_HTML||e->node->tag!=OS64_HTML_TAG_IMG)continue;
        os64_dom_image_view_t view;
        DImageLoan loan;
        os64_dom_image_status_t status=image_acquire(dom,e->node,false,&view,&loan);
        bool skip=view.pending||view.empty||(e->image_reported && e->image_generation==view.generation);
        uint64_t generation=view.generation;
        image_release(&loan,&view);
        if(!d_delivery_active(dom))return false;
        if(skip||status==OS64_DOM_IMAGE_PENDING)continue;
        if(consume){e->image_reported=true;e->image_generation=generation;}
        if(node)*node=e->node;
        if(failed)*failed=status!=OS64_DOM_IMAGE_READY;
        return true;
    }
    // The probes above can discover a detached new Image whose source was
    // never read by script or layout. Submit it before an idle host sleeps.
    image_update(dom);
    return false;
}
static JSValue image_member(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);if(!dom)return JS_EXCEPTION;
    const os64_html_node_t *node=d_node(dom,ctx,self);
    if(!node||node->ns!=OS64_HTML_NS_HTML||node->tag!=OS64_HTML_TAG_IMG)
        return JS_ThrowTypeError(ctx,"Expected an HTMLImageElement");
    if((magic&~D_SET)==I_CORS) {
        if(magic&D_SET) {
            if(argc&&JS_IsNull(argv[0])) {
                int64_t status=d_set_attr(dom,node,"crossorigin",NULL,0,true);
                return status==OS64_HTML_OK?JS_UNDEFINED:d_page_error(ctx,status);
            }
            size_t n;const char *text=JS_ToCStringLen(ctx,&n,argc?argv[0]:JS_UNDEFINED);if(!text)return JS_EXCEPTION;
            int64_t status=d_set_attr(dom,node,"crossorigin",text,n,false);JS_FreeCString(ctx,text);
            return status==OS64_HTML_OK?JS_UNDEFINED:d_page_error(ctx,status);
        }
        const os64_html_attr_t *cors=os64_html_attr(node,"crossorigin");
        return !cors?JS_NULL:JS_NewString(ctx,os64_streq_nocase(cors->value,"use-credentials")?"use-credentials":"anonymous");
    }
    dom=d_owner(dom,node);
    if(!d_require_active(dom,ctx))return JS_EXCEPTION;
    os64_dom_image_view_t view;
    DImageLoan loan;
    os64_dom_image_status_t status=image_acquire(dom,node,false,&view,&loan);
    if(magic==I_CURRENT) {
        JSValue result=JS_NewString(ctx,view.current_src?view.current_src:"");
        image_release(&loan,&view);return result;
    }
    uint32_t value=status==OS64_DOM_IMAGE_READY?image_dimension(&view,magic==I_HEIGHT):0;
    bool pending=view.pending,empty=view.empty;
    image_release(&loan,&view);
    if(magic==I_COMPLETE) {
        const os64_html_attr_t *src=os64_html_attr(node,"src");
        const os64_html_attr_t *set=os64_html_attr(node,"srcset");
        return JS_NewBool(ctx,empty||(!pending && (status==OS64_DOM_IMAGE_READY||status==OS64_DOM_IMAGE_BROKEN||
            ((!src||!src->value[0])&&(!set||!set->value[0])))));
    }
    return JS_NewUint32(ctx,value);
}
static JSValue exported(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    (void)magic;
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);if(!dom)return JS_EXCEPTION;
    const os64_html_node_t *node=d_node(dom,ctx,self);
    if(!node||d_node_kind(node)!=D_PROTO_CANVAS)return JS_ThrowTypeError(ctx,"Expected an HTMLCanvasElement");
    /* Conversion may run script; PNG is also the fallback for other MIME types.
     * The optional quality is any, so a PNG export does not coerce it. */
    if(argc&&!JS_IsUndefined(argv[0])) {
        const char *type=JS_ToCString(ctx,argv[0]);if(!type)return JS_EXCEPTION;JS_FreeCString(ctx,type);
    }
    DValue *e=d_find(dom,node);
    if(d_canvas_prepare(e)!=CANVAS_OK)return JS_NewString(ctx,"data:,");
    if(!canvas_origin_clean(d_more_peek(e)->canvas))return d_error(ctx,"SecurityError","Canvas contains an image from another origin");
    canvas_bitmap_t bitmap;
    if(canvas_bitmap(d_more_peek(e)->canvas,&bitmap)!=CANVAS_OK||!bitmap.width||!bitmap.height)return JS_NewString(ctx,"data:,");
    size_t png_size;
    if(os64_png_encode_size(bitmap.width,bitmap.height,&png_size)!=OS64_PNG_OK)return JS_NewString(ctx,"data:,");
    const char prefix[]="data:image/png;base64,";
    size_t encoded=(png_size+2)/3*4,total=png_size+sizeof(prefix)-1+encoded;
    unsigned char *bytes;
    if(d_canvas_allocate(dom,total,(void **)&bytes)!=OS64_GEOMETRY_OK)return JS_NewString(ctx,"data:,");
    size_t written;
    os64_png_status_t status=os64_png_encode(bitmap.pixels,bitmap.width,bitmap.height,bitmap.stride,
        bitmap.capacity,true,bytes,png_size,&written);
    JSValue result;
    if(status!=OS64_PNG_OK)result=JS_NewString(ctx,"data:,");
    else {
        char *url=(char *)bytes+png_size;
        for(size_t i=0;i<sizeof(prefix)-1;++i)url[i]=prefix[i];
        static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t out=sizeof(prefix)-1;
        for(size_t i=0;i<written;i+=3) {
            uint32_t n=(uint32_t)bytes[i]<<16;
            if(i+1<written)n|=(uint32_t)bytes[i+1]<<8;
            if(i+2<written)n|=bytes[i+2];
            url[out++]=alphabet[n>>18];url[out++]=alphabet[(n>>12)&63];
            url[out++]=i+1<written?alphabet[(n>>6)&63]:'=';
            url[out++]=i+2<written?alphabet[n&63]:'=';
        }
        result=JS_NewStringLen(ctx,url,out);
    }
    d_canvas_release(dom,bytes,total);return result;
}
static JSValue member(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv,int magic,JSValue *data)
{
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);if(!dom)return JS_EXCEPTION;
    DValue *e=JS_GetOpaque(self,dom->canvas_class);
    if(!e||!d_same_cluster(e->dom, dom))return JS_ThrowTypeError(ctx,"Expected a CanvasRenderingContext2D");
    dom = e->dom;
    bool set=magic&D_SET;magic&=~D_SET;
    if(magic==I_SMOOTH||magic==I_QUALITY) {
        bool enabled=false;unsigned quality=0;bool valid=true;
        if(set) {
            if(magic==I_SMOOTH)enabled=JS_ToBool(ctx,argc?argv[0]:JS_UNDEFINED);
            else {
                size_t n;const char *s=JS_ToCStringLen(ctx,&n,argc?argv[0]:JS_UNDEFINED);if(!s)return JS_EXCEPTION;
                if(n==3&&!os64_memcmp(s,"low",3))quality=0;
                else if(n==6&&!os64_memcmp(s,"medium",6))quality=1;
                else if(n==4&&!os64_memcmp(s,"high",4))quality=2;
                else valid=false;
                JS_FreeCString(ctx,s);
            }
        }
        d_canvas_sync(e);canvas_state_t state={.image_smoothing=true};
        if(d_more_peek(e)->canvas)canvas_state(d_more_peek(e)->canvas,&state);
        if(set)return !valid||!d_more_peek(e)->canvas?JS_UNDEFINED:d_canvas_result(ctx,canvas_set_smoothing(d_more_peek(e)->canvas,
            magic==I_SMOOTH?enabled:state.image_smoothing,magic==I_QUALITY?quality:state.image_quality));
        return magic==I_SMOOTH?JS_NewBool(ctx,state.image_smoothing):
            JS_NewString(ctx,state.image_quality==2?"high":state.image_quality==1?"medium":"low");
    }
    if(argc<3||(argc>3&&argc<5)||(argc>5&&argc<9))return JS_ThrowTypeError(ctx,"drawImage requires three, five or nine arguments");
    const os64_html_node_t *source_node=d_node(dom,ctx,argv[0]);
    if(!source_node||source_node->ns!=OS64_HTML_NS_HTML||
       (source_node->tag!=OS64_HTML_TAG_CANVAS&&source_node->tag!=OS64_HTML_TAG_IMG))
        return JS_ThrowTypeError(ctx,"Expected a supported Canvas image source");
    unsigned count=argc>=9?8:argc>=5?4:2;double v[8];
    for(unsigned i=0;i<count;++i)if(JS_ToFloat64(ctx,&v[i],argv[i+1])<0)return JS_EXCEPTION;
    for(unsigned i=0;i<count;++i)if(!isfinite(v[i]))return JS_UNDEFINED;
    os64_dom_t *source_owner=d_owner(dom,source_node);
    os64_dom_image_view_t image={0};DImageLoan loan={0};
    if(source_node->tag==OS64_HTML_TAG_CANVAS) {
        DValue *source=d_find(dom,source_node);
        if(d_canvas_prepare(source)!=CANVAS_OK)return d_error(ctx,"InvalidStateError","Canvas source is unavailable");
        canvas_bitmap_t bitmap;
        if(canvas_bitmap(d_more_peek(source)->canvas,&bitmap)!=CANVAS_OK||!bitmap.width||!bitmap.height)
            return d_error(ctx,"InvalidStateError","Canvas source has no pixels");
        image.surface=(draw2d_surface_t){(void *)bitmap.pixels,bitmap.width,bitmap.height,bitmap.stride,bitmap.capacity,DRAW2D_ARGB8_PREMULTIPLIED};
        image.origin_clean=canvas_origin_clean(d_more_peek(source)->canvas);
    } else {
        if(!d_require_active(source_owner,ctx))return JS_EXCEPTION;
        os64_dom_image_status_t status=image_acquire(source_owner,source_node,true,&image,&loan);
        if(status!=OS64_DOM_IMAGE_READY) {
            image_release(&loan,&image);
            return status==OS64_DOM_IMAGE_PENDING?JS_UNDEFINED:
                d_error(ctx,status==OS64_DOM_IMAGE_REFUSED?"QuotaExceededError":"InvalidStateError","Image source is unavailable");
        }
    }
    double density=image_density(&image);
    double sx=0,sy=0,sw=image.surface.width,sh=image.surface.height,dx=v[0],dy=v[1],
        dw=density>0?sw/density:UINT32_MAX,dh=density>0?sh/density:UINT32_MAX;
    if(count==4){dw=v[2];dh=v[3];}
    if(count==8){sx=v[0]*density;sy=v[1]*density;sw=v[2]*density;sh=v[3]*density;dx=v[4];dy=v[5];dw=v[6];dh=v[7];}
    d_canvas_sync(e);
    canvas_status_t status=d_more_peek(e)->canvas?canvas_draw_image(d_more_peek(e)->canvas,&image.surface,image.origin_clean,sx,sy,sw,sh,dx,dy,dw,dh):CANVAS_OK;
    image_release(&loan,&image);
    return d_canvas_result(ctx,status);
}
int d_canvas_images_install(os64_dom_t *dom,JSContext *ctx,JSValueConst prototype)
{
    if(d_method_named(dom,ctx,prototype,"drawImage",member,3,I_DRAW)<0||
       d_accessor(dom,ctx,prototype,"imageSmoothingEnabled",member,I_SMOOTH,true)<0||
       d_accessor(dom,ctx,prototype,"imageSmoothingQuality",member,I_QUALITY,true)<0||
       d_method_named(dom,ctx,dom->prototypes[D_PROTO_CANVAS]->value,"toDataURL",exported,0,I_EXPORT)<0)return -1;
    JSValueConst image=dom->prototypes[D_PROTO_IMAGE]->value;
    return d_accessor(dom,ctx,image,"naturalWidth",image_member,I_WIDTH,false)<0||
        d_accessor(dom,ctx,image,"naturalHeight",image_member,I_HEIGHT,false)<0||
        d_accessor(dom,ctx,image,"complete",image_member,I_COMPLETE,false)<0||
        d_accessor(dom,ctx,image,"currentSrc",image_member,I_CURRENT,false)<0||
        d_accessor(dom,ctx,image,"crossOrigin",image_member,I_CORS,true)<0?-1:0;
}
