/* CSS snapshot loans and native activity cleanup, without destroying exported
 * Animation/KeyframeEffect records or acknowledging outstanding layout trees. */
void d_web_animations_cancel(os64_dom_t *dom);
void d_web_animations_drain(os64_dom_t *dom);
enum { AR_GET_COUNT, AR_GET_AT, AR_GET_DRIVE, AR_STEP_VERSION, AR_STEP_COUNT,
       AR_STEP_AT, AR_DUE_VERSION, AR_DRIVE, AR_REPLACE, AR_EFFECTS,
       AR_ANIMATE_CONVERT, AR_RATE_CONVERT, AR_START_CONVERT, AR_COUNT };
enum { AR_COUNT_CALL, AR_AT_CALL, AR_DRIVE_CALL, AR_VERSION_CALL };
typedef struct {
    os64_dom_t *binding, *sibling;
    os64_js_realm_t *realm;
    os64_dom_css_animation_t records[2];
    unsigned mode, calls[4], replaced;
    bool armed, alive;
} AnimationRetirement;

static bool animation_poison_at(void *opaque, int32_t i, os64_dom_css_animation_t *out)
{
    (void)i; (void)out;
    AnimationRetirement *p = opaque; p->replaced++; return false;
}

static void animation_provider_entry(AnimationRetirement *p, unsigned kind)
{
    p->calls[kind]++;
    check(p->alive, "animation retirement: detached provider is never used");
    bool busy = d_provider_busy(p->binding) && d_provider_busy(p->sibling);
    check(busy, "animation retirement: native CSS loan visible across cluster");
    if (busy) {
        check(os64_dom_web_animations_next(p->binding) == UINT64_MAX &&
              os64_dom_web_animations_next(p->sibling) == UINT64_MAX,
              "animation retirement: provider reentry cannot query another CSS snapshot");
        d_web_animations_cancel(p->binding);
        d_web_animations_drain(p->binding);
    }
    if (!p->armed) return;
    if (p->mode == AR_REPLACE && kind == AR_COUNT_CALL) {
        os64_dom_css_animations_t replacement = {.at=animation_poison_at,.opaque=p};
        os64_dom_set_css_animations(p->binding, &replacement);
        return;
    }
    bool retire = (kind == AR_COUNT_CALL && (p->mode == AR_GET_COUNT || p->mode == AR_STEP_COUNT)) ||
        (kind == AR_AT_CALL && (p->mode == AR_GET_AT || p->mode == AR_STEP_AT)) ||
        (kind == AR_DRIVE_CALL && (p->mode == AR_GET_DRIVE || p->mode == AR_DRIVE)) ||
        (kind == AR_VERSION_CALL && (p->mode == AR_STEP_VERSION || p->mode == AR_DUE_VERSION));
    if (retire) {
        os64_js_outcome_t out;
        check(os64_js_realm_retire(p->realm, OS64_JS_ABI_ID, &out) == OS64_JS_OK,
              "animation retirement: CSS callback marks its document");
        if (busy) d_web_animations_cancel(p->binding);
        check(d_provider_busy(p->binding) && !d_provider_enter(p->binding),
              "animation retirement: mark preserves loan and refuses new work");
    }
}
static int32_t animation_retiring_count(void *opaque, bool current)
{
    (void)current; animation_provider_entry(opaque, AR_COUNT_CALL); return 2;
}
static bool animation_retiring_at(void *opaque, int32_t i, os64_dom_css_animation_t *out)
{
    AnimationRetirement *p = opaque;
    animation_provider_entry(p, AR_AT_CALL);
    if (i < 0 || i >= 2) return false;
    *out = p->records[i]; return true;
}
static bool animation_retiring_drive(void *opaque, uint64_t id, bool paused, bool canceled,
                                      double start, double hold, double rate)
{
    (void)id; (void)paused; (void)canceled; (void)start; (void)hold; (void)rate;
    animation_provider_entry(opaque, AR_DRIVE_CALL); return true;
}
static uint64_t animation_retiring_version(void *opaque)
{
    animation_provider_entry(opaque, AR_VERSION_CALL); return 1;
}
static void dom_animation_retirement_cases(void)
{
    for (int policy = OS64_JS_TEARDOWN_FATAL; policy <= OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode = 0; mode < AR_COUNT; ++mode) {
        p2b_ms=1000; p2b_us=1000000;
        os64_js_config_t config=fixture_config(); config.limits.memory_bytes=64u<<20;
        os64_js_runtime_t *runtime=NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "animation retirement: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_dom_cluster_t *cluster=os64_dom_cluster_create(runtime,&limits,&out);
        if (!cluster) {check(false,"animation retirement: cluster created");os64_js_destroy(runtime);continue;}
        os64_js_realm_t *realms[2]={os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out),NULL};
        check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&realms[1],&out)==OS64_JS_OK,
              "animation retirement: child realm created");
        os64_dom_t *bindings[2]={0}; os64_html_document_t *documents[2]={0};
        for (unsigned i=0;i<2&&realms[i];++i) {
            DomFixture f=fixture_configured(0,0,&config);os64_js_destroy(f.runtime);
            os64_dom_options_t options=p2b_options("https://animation.example/");
            os64_dom_admission_t a=os64_dom_create_in_realm(cluster,realms[i],f.document,f.state,&options,&out);
            bindings[i]=a.binding;documents[i]=f.document;
            check(a.binding&&a.consumed,"animation retirement: binding installed");
            if (!a.consumed) {os64_page_state_free(f.state);os64_html_document_free(f.document);}
        }
        if (bindings[0]&&bindings[1]) {
            retirement_realms[1]=realms[1];
            AnimationRetirement p={.binding=bindings[1],.sibling=bindings[0],.realm=realms[1],.mode=mode,.alive=true};
            for (unsigned i=0;i<2;++i) p.records[i]=(os64_dom_css_animation_t){.id=i+1,.element=documents[1]->body,
                .name="retained",.name_len=8,.paused=true,.style_paused=true,.hold_ms=20,.rate=1,
                .duration_ms=1000,.iterations=1,.fill=GARB_FILL_BOTH};
            os64_dom_css_animations_t host={animation_retiring_count,animation_retiring_at,
                animation_retiring_drive,animation_retiring_version,&p};
            os64_dom_set_css_animations(bindings[1],&host);
            for (unsigned i=0;i<2;++i) {
                JSContext *ctx=os64_js_realm_context(realms[i],OS64_JS_ABI_ID,&out);
                JSValue global=JS_GetGlobalObject(ctx);
                for (unsigned j=0;j<2;++j) {
                    JSContext *other=os64_js_realm_context(realms[j],OS64_JS_ABI_ID,&out);
                    check(JS_SetPropertyStr(ctx,global,j?"B":"A",JS_GetGlobalObject(other))>=0,
                          "animation retirement: trusted references exported");
                }
                check(JS_SetPropertyStr(ctx,global,"retireChild",JS_NewCFunctionMagic(ctx,retirement_mark,
                      "retireChild",0,JS_CFUNC_generic_magic,1))>=0,"animation retirement: native mark exported");
                JS_FreeValue(ctx,global);
                realm_script(realms[i],STORAGE_ASSERT,"animation retirement: assertions installed");
            }
            bool retained=mode==AR_DUE_VERSION||mode==AR_DRIVE||mode==AR_EFFECTS||
                mode==AR_RATE_CONVERT||mode==AR_START_CONVERT;
            if (retained) {
                realm_script(realms[1],"globalThis.css=document.getAnimations();globalThis.cssEffect=css[0].effect;",
                             "animation retirement: CSS identities retained");
                check(os64_dom_web_animations_update(bindings[1],1000,&out),
                      "animation retirement: initial CSS bookkeeping settles");
            }
            const struct garb_effect *borrowed=NULL;const struct garb_effect *const *effects=NULL;
            const os64_html_node_t *moving=NULL;uint64_t mark=0;
            if (mode==AR_EFFECTS) {
                realm_script(realms[1],"globalThis.moving=document.createElement('div');moving.id='motion';"
                    "document.body.appendChild(moving);globalThis.motion=moving.animate({opacity:[0,1]},1000);"
                    "globalThis.effect=motion.effect;", "animation retirement: running script effect exported");
                p2b_ms+=20;p2b_us+=20000;os64_dom_web_animations_update(bindings[1],p2b_ms,&out);
                p2b_ms+=40;p2b_us+=40000;os64_dom_web_animations_update(bindings[1],p2b_ms,&out);
                moving=native_id(documents[1]->document,"motion");
                check(os64_dom_script_effects(bindings[1],moving,&effects)==1,"animation retirement: source layout borrows effect");
                borrowed=effects?effects[0]:NULL;mark=os64_dom_effects_mark(bindings[1]);
                check(borrowed&&!borrowed->held,"animation retirement: source effect clock is running");
                realm_script(realms[0],"document.body.appendChild(B.moving);","animation retirement: live document adopts animated target");
                (void)os64_dom_effects_mark(bindings[0]);
            }
            memset(p.calls,0,sizeof(p.calls));p.armed=true;
            if (mode<=AR_GET_DRIVE)
                realm_script(realms[0],"var refused=false;try{B.document.getAnimations()}catch(e){refused=e.name==='InvalidStateError'}"
                    "assert(refused);", "animation retirement: interrupted CSS query refuses partial result");
            else if (mode<=AR_STEP_AT) {
                realm_script(realms[1],"addEventListener('animationstart',()=>{throw Error('late CSS event')});",
                             "animation retirement: CSS event listener installed");
                check(os64_dom_web_animations_update(bindings[1],1000,&out)&&out.status==OS64_JS_OK,
                      "animation retirement: CSS step stops after callback mark");
            } else if (mode==AR_DUE_VERSION)
                check(os64_dom_web_animations_next(bindings[1])==UINT64_MAX,"animation retirement: version retirement leaves no scheduled work");
            else if (mode==AR_DRIVE)
                realm_script(realms[0],"B.css[0].currentTime=30;","animation retirement: retained clock push can mark departure");
            else if (mode==AR_REPLACE)
                realm_script(realms[0],"assert(B.document.getAnimations().length===2);assert(B.document.getAnimations().length===2);",
                             "animation retirement: snapshot and installed table survive provider replacement attempt");
            else if (mode==AR_ANIMATE_CONVERT || mode==AR_RATE_CONVERT || mode==AR_START_CONVERT)
                realm_script(realms[0],mode==AR_ANIMATE_CONVERT ?
                    "var refused=false;try{B.document.body.animate(null,{get duration(){retireChild();return 100}})}"
                    "catch(e){refused=e.name==='InvalidStateError'}assert(refused);" : mode==AR_RATE_CONVERT ?
                    "var refused=false;try{B.css[0].updatePlaybackRate({valueOf(){retireChild();return 2}})}"
                    "catch(e){refused=e.name==='InvalidStateError'}assert(refused);" :
                    "var refused=false;try{B.css[0].startTime={valueOf(){retireChild();return 2}}}"
                    "catch(e){refused=e.name==='InvalidStateError'}assert(refused);",
                    "animation retirement: conversion retirement refuses timeline work");
            if (mode==AR_GET_COUNT||mode==AR_STEP_COUNT)
                check(p.calls[AR_AT_CALL]==0,"animation retirement: count retirement prevents record query");
            if (mode==AR_GET_AT||mode==AR_STEP_AT)
                check(p.calls[AR_AT_CALL]==1&&p.calls[AR_DRIVE_CALL]==0,
                      "animation retirement: record retirement prevents consumption and later query");
            if (mode==AR_GET_DRIVE) check(p.calls[AR_AT_CALL]==1,"animation retirement: wrap drive retirement stops snapshot");
            if (mode==AR_STEP_VERSION) check(p.calls[AR_COUNT_CALL]==0,"animation retirement: version retirement prevents count");
            check(p.replaced==0&&!d_provider_busy(bindings[0]),"animation retirement: original producer and balanced loans");
            check(os64_js_realm_retire(realms[1],OS64_JS_ABI_ID,&out)==OS64_JS_OK,"animation retirement: document marked");
            d_web_animations_cancel(bindings[1]);d_web_animations_cancel(bindings[1]);p.alive=false;
            unsigned before=0;for(unsigned i=0;i<4;++i)before+=p.calls[i];
            realm_script(realms[0],"var refused=false;try{B.document.getAnimations()}catch(e){refused=e.name==='InvalidStateError'}"
                "assert(refused);assert(document.getAnimations() instanceof Array);", "animation retirement: inactive query refuses while sibling stays usable");
            if(retained) realm_script(realms[0],"assert(B.css[0].effect===B.cssEffect&&B.css[0].animationName==='retained');"
                "B.css[0].currentTime=70;assert(B.css[0].currentTime===70);var refused=false;"
                "try{B.css[0].play()}catch(e){refused=e.name==='InvalidStateError'}assert(refused);",
                "animation retirement: retained CSS identity and pure seeks survive cleanup");
            if(retained) {
                size_t bytes=os64_dom_cluster_bytes(cluster).dom_bytes;
                realm_script(realms[0],"for(let i=0;i<16;i++){B.css[0].currentTime=1;B.css[0].cancel()}",
                             "animation retirement: retained cancellation creates no departed event tasks");
                check(os64_dom_cluster_bytes(cluster).dom_bytes<=bytes,
                      "animation retirement: late cancellation retains no event payloads");
            }
            check(os64_dom_web_animations_next(bindings[1])==UINT64_MAX&&
                  !os64_dom_web_animations_update(bindings[1],2000,&out),"animation retirement: no departed scheduler entry");
            unsigned after=0;for(unsigned i=0;i<4;++i)after+=p.calls[i];
            check(after==before,"animation retirement: late methods never touch detached CSS provider");
            if(mode==AR_EFFECTS) {
                check(borrowed&&borrowed->held&&borrowed->hold_ms==40&&
                      os64_dom_script_effects(bindings[0],moving,&effects)==1,
                      "animation retirement: departed timeline freezes adopted effect without unlinking it");
                realm_script(realms[0],"globalThis.departedFinished=0;"
                    "B.motion.finished.then(()=>{departedFinished++});B.motion.currentTime=1000;",
                    "animation retirement: retained seek reaches effect end");
                realm_script(realms[0],"assert(departedFinished===0);",
                    "animation retirement: retained seek schedules no departed finish notification");
                realm_script(realms[0],"assert(B.motion.effect===B.effect);var refused=false;"
                    "try{B.motion.startTime=0}catch(e){refused=e.name==='InvalidStateError'}assert(refused);"
                    "B.motion.currentTime=40;B.effect.setKeyframes({opacity:[1,0]});",
                             "animation retirement: retained effect mutates after creator departure");
                os64_dom_effects_release(bindings[0],UINT64_MAX);
                size_t bytes=os64_dom_cluster_bytes(cluster).dom_bytes;
                os64_dom_effects_release(bindings[1],mark);
                check(borrowed&&borrowed->nframes==2&&os64_dom_cluster_bytes(cluster).dom_bytes==bytes,
                      "animation retirement: cleanup preserves outstanding source layout generation");
                os64_dom_effects_release(bindings[1],UINT64_MAX);
                check(os64_dom_cluster_bytes(cluster).dom_bytes<bytes,"animation retirement: final layout acknowledgement releases old data");
                check(os64_dom_script_effects(bindings[0],moving,&effects)==1,
                      "animation retirement: destination borrows replacement effect");
                borrowed=effects?effects[0]:NULL;
                (void)os64_dom_effects_mark(bindings[0]);
                realm_script(realms[0],"B.motion=null;B.effect=null;",
                             "animation retirement: script drops its last animation and effect references");
                JSContext *ctx=os64_js_realm_context(realms[0],OS64_JS_ABI_ID,&out);
                JS_RunGC(JS_GetRuntime(ctx));
                check(borrowed&&borrowed->nframes==2&&os64_dom_script_effects(bindings[0],moving,&effects)==0,
                      "animation retirement: collected animation leaves layout-borrowed data alive");
                bytes=os64_dom_cluster_bytes(cluster).dom_bytes;
                os64_dom_effects_release(bindings[1],UINT64_MAX);
                check(borrowed&&borrowed->nframes==2&&os64_dom_cluster_bytes(cluster).dom_bytes==bytes,
                      "animation retirement: creator acknowledgement cannot release destination tree data");
                os64_dom_effects_release(bindings[0],UINT64_MAX);
                check(os64_dom_cluster_bytes(cluster).dom_bytes<bytes,
                      "animation retirement: destination tree release reclaims collected effect data");
            }
        }
        os64_dom_cluster_drain(cluster);os64_js_destroy(runtime);os64_dom_cluster_free(cluster);
        check(live==0&&dom_live==0&&!dom_allocations,"animation retirement: native and engine holds released");
    }
}
