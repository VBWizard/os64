#include "internal.h"
#include "os64/fmt.h"

/* Handles increase without reuse. A frame snapshots the last issued handle;
 * cancellation can remove later callbacks in that frame, while newly requested
 * callbacks wait for the next one. The whole batch shares one runtime budget,
 * with a microtask/error checkpoint between callbacks. */
struct DAnimation {
    DAnimation *next;
    JSValue callback;
    uint32_t handle, script;
};

#define FRAME_INTERVAL_MS 20
#define FRAME_CALLBACKS_MAX 1024

static uint64_t frame_after(const os64_dom_t *dom)
{
    uint64_t now=dom->options.now_ms?dom->options.now_ms(dom->options.host_opaque):0;
    return now>=UINT64_MAX-FRAME_INTERVAL_MS?UINT64_MAX-1:now+FRAME_INTERVAL_MS;
}

static void remove_entry(os64_dom_t *dom,DAnimation **link,DAnimation *previous)
{
    DAnimation *entry=*link;
    *link=entry->next;
    if(dom->animation_last==entry)dom->animation_last=previous;
    dom->animation_count--;
    JS_FreeValueRT(dom->engine,entry->callback);
    d_free(dom,entry);
}

static JSValue animation_method(JSContext *ctx,JSValueConst self,int argc,
                                JSValueConst *argv,int magic,JSValue *data)
{
    (void)self;
    os64_dom_t *dom=d_callback(ctx,data,OS64_JS_ABI_ID);
    if(!dom)return JS_EXCEPTION;
    if(magic) {
        if(argc<1)return JS_ThrowTypeError(ctx,"cancelAnimationFrame needs a handle");
        uint32_t handle;
        if(JS_ToUint32(ctx,&handle,argv[0])<0)return JS_EXCEPTION;
        DAnimation *previous=NULL;
        for(DAnimation **link=&dom->animations;*link;previous=*link,link=&(*link)->next) {
            if((*link)->handle==handle){remove_entry(dom,link,previous);break;}
        }
        return JS_UNDEFINED;
    }
    if(argc<1||!JS_IsFunction(ctx,argv[0]))
        return JS_ThrowTypeError(ctx,"requestAnimationFrame needs a function");
    uint32_t script=d_callback_script(dom,ctx,argv[0]);
    if(script==D_SCRIPT_FAILED)return JS_EXCEPTION;
    if(dom->animation_count>=FRAME_CALLBACKS_MAX||dom->animation_handle==UINT32_MAX)
        return d_error(ctx,"QuotaExceededError","Too many animation callbacks");
    DAnimation *entry=d_alloc(dom,sizeof(*entry));
    if(!entry)return d_error(ctx,"QuotaExceededError","DOM animation callback quota exceeded");
    *entry=(DAnimation){.callback=JS_DupValue(ctx,argv[0]),.handle=++dom->animation_handle,.script=script};
    if(dom->animation_last)dom->animation_last->next=entry;
    else {dom->animations=entry;dom->animation_due=frame_after(dom);}
    dom->animation_last=entry;dom->animation_count++;
    return JS_NewUint32(ctx,entry->handle);
}

uint64_t os64_dom_animation_next(const os64_dom_t *dom)
{
    return dom&&!dom->closed&&dom->animations?dom->animation_due:UINT64_MAX;
}

bool os64_dom_animation_fire(os64_dom_t *dom,uint64_t now,os64_js_outcome_t *out)
{
    if(out)os64_memset(out,0,sizeof(*out));
    if(!out||!dom||dom->closed||dom->animation_running||!dom->animations||dom->animation_due>now)return false;
    char name[OS64_JS_SOURCE_NAME_CAP];
    os64_snprintf(name,sizeof(name),"%.*s#animation-frame",(int)(sizeof(name)-20),dom->url?dom->url:"about:blank");
    uint32_t last=dom->animation_handle;
    double timestamp=d_performance_now(dom);
    dom->animation_running=true;
    if(os64_js_task_begin(dom->runtime,OS64_JS_ABI_ID,name,out)==OS64_JS_OK) {
        os64_js_outcome_t access;
        JSContext *ctx=os64_js_context(dom->runtime,OS64_JS_ABI_ID,&access);
        if(!ctx)d_outcome_fold(out,&access);
        while(ctx&&!dom->closed&&dom->animations&&dom->animations->handle<=last) {
            DAnimation *entry=dom->animations;
            JSValue callback=JS_DupValueRT(dom->engine,entry->callback);
            uint32_t script=entry->script;
            remove_entry(dom,&dom->animations,NULL);
            JSValue arg=JS_NewFloat64(ctx,timestamp);
            os64_js_outcome_t call;
            DScripts before=d_script_enter(dom,script);
            os64_js_call(dom->runtime,OS64_JS_ABI_ID,callback,dom->window->value,1,&arg,NULL,&call);
            d_script_leave(dom,&before,true);
            JS_FreeValueRT(dom->engine,callback);
            d_outcome_fold(out,&call);
            if(call.status==OS64_JS_EXCEPTION)d_errors_fire(dom,out);
            d_task_settle(dom,out);
            if(out->status!=OS64_JS_OK&&out->status!=OS64_JS_EXCEPTION&&
               out->status!=OS64_JS_UNHANDLED_REJECTION)break;
        }
        d_task_finish(dom,out);
    }
    dom->animation_running=false;
    dom->animation_due=frame_after(dom);
    d_task_close(dom,name,out);
    return true;
}

void d_animation_drain(os64_dom_t *dom)
{
    while(dom->animations)remove_entry(dom,&dom->animations,NULL);
}

int d_animation_install(os64_dom_t *dom,JSContext *ctx,JSValueConst global)
{
    if(d_method_named(dom,ctx,global,"requestAnimationFrame",animation_method,1,0)<0)return -1;
    return d_method_named(dom,ctx,global,"cancelAnimationFrame",animation_method,1,1);
}
