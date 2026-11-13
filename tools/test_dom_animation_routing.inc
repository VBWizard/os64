/* Creation realm and timeline ownership are independent after adoption. */
static void dom_animation_routing_cases(void)
{
    static const char *const create[] = {
        "document.body.appendChild(B.node);globalThis.anim=B.node.animate({opacity:[0,1]},1000);",
        "globalThis.anim=B.node.animate({opacity:[0,1]},{get duration(){document.body.appendChild(B.node);return 1000}});",
        "globalThis.anim=B.node.animate({get opacity(){document.body.appendChild(B.node);return [0,1]}},1000);",
        "document.body.appendChild(B.node);globalThis.anim=Element.prototype.animate.call(B.node,{opacity:[0,1]},1000);",
        "document.body.appendChild(B.node);globalThis.anim=new B.Animation(new B.KeyframeEffect(B.node,{opacity:[0,1]},1000),document.timeline);anim.play();",
        "globalThis.anim=B.node.animate({opacity:[0,1]},{duration:1000,timeline:document.timeline});document.body.appendChild(B.node);"
    };
    for (int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;++policy)
    for (unsigned mode=0;mode<sizeof(create)/sizeof(*create);++mode)
    for (unsigned early=0;early<2;++early) {
        p2b_ms=1000;p2b_us=1000000;
        os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
        os64_js_runtime_t *runtime=NULL;os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "animation routing: runtime created");
        if(!runtime)continue;
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_dom_cluster_t *cluster=os64_dom_cluster_create(runtime,&limits,&out);
        if(!cluster){check(false,"animation routing: cluster created");os64_js_destroy(runtime);continue;}
        os64_js_realm_t *realms[2]={os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out),NULL};
        check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&realms[1],&out)==OS64_JS_OK,
              "animation routing: child created");
        os64_dom_t *bindings[2]={0};os64_html_document_t *documents[2]={0};
        for(unsigned i=0;i<2&&realms[i];++i){
            DomFixture f=fixture_configured(0,0,&config);os64_js_destroy(f.runtime);
            os64_dom_options_t options=p2b_options("https://animation.example/");
            os64_dom_admission_t a=os64_dom_create_in_realm(cluster,realms[i],f.document,f.state,&options,&out);
            bindings[i]=a.binding;documents[i]=f.document;
            check(a.binding&&a.consumed,"animation routing: binding installed");
            if(!a.consumed){os64_page_state_free(f.state);os64_html_document_free(f.document);}
        }
        if(bindings[0]&&bindings[1]){
            JSContext *ctx=os64_js_realm_context(realms[0],OS64_JS_ABI_ID,&out);
            JSContext *child=os64_js_realm_context(realms[1],OS64_JS_ABI_ID,&out);
            JSValue global=JS_GetGlobalObject(ctx);
            check(JS_SetPropertyStr(ctx,global,"B",JS_GetGlobalObject(child))>=0,
                  "animation routing: child references exported");JS_FreeValue(ctx,global);
            realm_script(realms[0],STORAGE_ASSERT "B.node=B.document.createElement('div');B.node.id='routed';"
                "B.document.body.appendChild(B.node);", "animation routing: target created in child");
            realm_script(realms[1],"globalThis.seek=a=>{a.currentTime=1000};",
                "animation routing: retained ordinary function exported");
            if(early)check(os64_js_realm_retire(realms[1],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                           "animation routing: creator retired before creation");
            char label[100];snprintf(label,sizeof(label),"animation routing: create mode %u early %u",mode,early);
            bool made=realm_script(realms[0],create[mode],label);
            if(made){
                realm_script(realms[0],"assert(anim instanceof B.Animation&&!(anim instanceof Animation));"
                    "assert(anim.effect instanceof B.KeyframeEffect&&!(anim.effect instanceof KeyframeEffect));"
                    "assert(anim.timeline===document.timeline&&anim.effect.target===B.node);"
                    "assert(anim.ready instanceof B.Promise&&anim.finished instanceof B.Promise);"
                    "globalThis.empty=B.node.animate(null,{duration:1,timeline:null});"
                    "assert(empty.timeline===null);empty.cancel();"
                    "var bad=false;try{B.node.animate(null,{id:'discard',timeline:{}})}catch(e){bad=e.name==='TypeError'}assert(bad);"
                    "globalThis.finished=0;globalThis.events=0;anim.finished.then(()=>finished++);"
                    "anim.onfinish=e=>{assert(e instanceof B.AnimationPlaybackEvent);events++};",
                    "animation routing: receiver prototypes and current document timeline");
                check(os64_js_realm_retire(realms[1],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                      "animation routing: creator retired after export");
                d_web_animations_cancel(bindings[1]);
                realm_script(realms[0],"var refused=false;try{B.node.animate(null,{timeline:B.document.timeline})}"
                    "catch(e){refused=e.name==='InvalidStateError'}assert(refused);"
                    "var idle=new B.Animation(null,B.document.timeline);refused=false;"
                    "try{idle.play()}catch(e){refused=e.name==='InvalidStateError'}assert(refused);",
                    "animation routing: explicit departed timeline cannot be revived");
                check(os64_dom_web_animations_update(bindings[0],p2b_ms,&out)&&out.status==OS64_JS_OK,
                      "animation routing: live timeline starts foreign animation");
                p2b_ms+=40;p2b_us+=40000;
                check(os64_dom_web_animations_update(bindings[0],p2b_ms,&out)&&out.status==OS64_JS_OK,
                      "animation routing: live timeline advances foreign animation");
                realm_script(realms[0],"assert(anim.currentTime===40&&!anim.pending);"
                    "assert(Object.getOwnPropertyDescriptor(B.AnimationTimeline.prototype,'currentTime').get.call(document.timeline)===40);"
                    "assert(Object.getOwnPropertyDescriptor(B.Document.prototype,'timeline').get.call(document)===document.timeline);"
                    "assert(document.getAnimations().includes(anim));B.seek(anim);",
                    "animation routing: departed method seeks on live timeline");
                realm_script(realms[0],"assert(finished===1);",
                    "animation routing: finish job belongs to live timeline");
                p2b_ms+=20;p2b_us+=20000;
                check(os64_dom_web_animations_update(bindings[0],p2b_ms,&out)&&out.status==OS64_JS_OK,
                      "animation routing: live timeline delivers finish event");
                realm_script(realms[0],"assert(events===1);Animation.prototype.pause.call(anim);"
                    "assert(anim.ready instanceof B.Promise);anim.currentTime=50;"
                    "assert(anim.finished instanceof B.Promise);",
                    "animation routing: live callback receives creation-realm event");
                const garb_effect_t *const *effects=NULL;
                const os64_html_node_t *node=native_id(documents[0]->document,"routed");
                check(os64_dom_script_effects(bindings[0],node,&effects)==1,
                      "animation routing: active document discovers foreign effect");
                check(os64_js_realm_retire(realms[0],OS64_JS_ABI_ID,&out)==OS64_JS_OK,
                      "animation routing: timeline retired");
                d_web_animations_cancel(bindings[0]);
                check(effects&&effects[0]->held&&effects[0]->hold_ms==50&&
                      os64_dom_web_animations_next(bindings[0])==UINT64_MAX,
                      "animation routing: timeline departure freezes foreign effect");
            }
        }
        os64_dom_cluster_drain(cluster);os64_js_destroy(runtime);os64_dom_cluster_free(cluster);
        check(live==0&&dom_live==0&&!dom_allocations,"animation routing: all owners release native data");
    }
}
