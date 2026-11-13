#include "../userland/libdom/internal.h"

/* Public mark/finish owns the boundary between host activity and retained DOM. */
typedef struct {
    FakeStore store;
    FakeNet net;
    os64_dom_t *binding;
    bool armed, freed;
    unsigned callbacks, cancellations;
} DepartureProbe;

static void departure_reentry(DepartureProbe *p)
{
    os64_js_outcome_t out;
    check(!p->freed, "departure: provider storage still alive during callback");
    check(os64_dom_depart(p->binding, &out) == OS64_JS_OK, "departure: reentrant mark is idempotent");
    check(os64_dom_depart_finish(p->binding, &out) == OS64_JS_BUSY,
          "departure: finish refuses a borrowed native/engine stack");
    p->callbacks++;
}

static void departure_alert(void *opaque, const char *text, size_t length)
{
    (void)text; (void)length;
    departure_reentry(opaque);
}

static uint64_t departure_clock(void *opaque)
{
    DepartureProbe *p = opaque;
    if (p->armed) departure_reentry(p);
    return 1000;
}

static uint64_t departure_start(void *opaque, const os64_dom_request_t *request, uint32_t token)
{
    DepartureProbe *p = opaque;
    check(!p->freed, "departure: live request producer");
    return net_start(&p->net, request, token);
}

static void departure_cancel(void *opaque, uint64_t id)
{
    DepartureProbe *p = opaque;
    departure_reentry(p);
    p->cancellations++;
    net_cancel(&p->net, id);
}

static os64_dom_storage_status_t departure_commit(void *opaque, const char *origin,
                                                 const void *changes, size_t length)
{
    DepartureProbe *p = opaque;
    departure_reentry(p);
    return fake_commit(&p->store, origin, changes, length);
}

static void dom_departure_cases(void)
{
    for (int policy=OS64_JS_TEARDOWN_FATAL; policy<=OS64_JS_TEARDOWN_RECLAIM; ++policy)
    for (unsigned mode=0; mode<3; ++mode) {
        os64_js_config_t config=fixture_config(); config.limits.memory_bytes=64u<<20;
        os64_js_runtime_t *runtime=NULL; os64_js_outcome_t out;
        check(os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&runtime,&out)==OS64_JS_OK,
              "departure: runtime created");
        if (!runtime) continue;
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_dom_cluster_t *cluster=os64_dom_cluster_create(runtime,&limits,&out);
        if (!cluster) { check(false,"departure: cluster created"); os64_js_destroy(runtime); continue; }
        os64_js_realm_t *realms[2]={os64_js_primary_realm(runtime,OS64_JS_ABI_ID,&out),NULL};
        check(os64_js_realm_create(runtime,OS64_JS_ABI_ID,&realms[1],&out)==OS64_JS_OK,
              "departure: child realm created");
        os64_dom_t *bindings[2]={0}; DepartureProbe p={0};
        for (unsigned i=0; i<2 && realms[i]; ++i) {
            DomFixture f=fixture_configured(0,0,&config); os64_js_destroy(f.runtime);
            os64_dom_options_t options=p2b_options("https://departure.example/");
            options.alert=departure_alert; options.alert_opaque=&p;
            options.now_ms=departure_clock; options.host_opaque=&p;
            os64_dom_admission_t a=os64_dom_create_in_realm(cluster,realms[i],f.document,f.state,&options,&out);
            bindings[i]=a.binding;
            check(a.binding&&a.consumed,"departure: binding installed");
            if (!a.consumed) { os64_page_state_free(f.state); os64_html_document_free(f.document); }
        }
        if (bindings[0] && bindings[1]) {
            p.binding=bindings[1];
            os64_dom_storage_t storage={fake_identity,fake_load,departure_commit,&p};
            os64_dom_network_t network={.start=departure_start,.cancel=departure_cancel,.opaque=&p};
            os64_dom_set_storage(p.binding,OS64_DOM_STORAGE_LOCAL,&storage);
            os64_dom_set_network(p.binding,&network);
            JSContext *ctx=os64_js_realm_context(realms[0],OS64_JS_ABI_ID,&out);
            JSContext *child=os64_js_realm_context(realms[1],OS64_JS_ABI_ID,&out);
            JSValue global=JS_GetGlobalObject(ctx);
            check(JS_SetPropertyStr(ctx,global,"B",JS_GetGlobalObject(child))>=0,"departure: export child");
            JS_FreeValue(ctx,global);
            realm_script(realms[0],STORAGE_ASSERT "globalThis.liveCalls=0;"
                "globalThis.mo=new MutationObserver(()=>liveCalls++);mo.observe(document.body,{childList:true});"
                "globalThis.reactions=0;customElements.define('x-depart',class extends HTMLElement{"
                "static get observedAttributes(){return ['data-pulse']}attributeChangedCallback(){reactions++}});"
                "globalThis.pulse=document.createElement('x-depart');pulse.id='pulse';document.body.appendChild(pulse);",
                "departure: sibling observer installed");
            realm_script(realms[1],STORAGE_ASSERT "globalThis.calls=0;globalThis.node=document.createElement('div');"
                "document.body.appendChild(node);globalThis.retained=()=>node;"
                "globalThis.mo=new MutationObserver(()=>calls++);mo.observe(document.body,{childList:true});"
                "globalThis.reactions=0;customElements.define('x-depart',class extends HTMLElement{"
                "static get observedAttributes(){return ['data-pulse']}attributeChangedCallback(){reactions++}});"
                "globalThis.pulse=document.createElement('x-depart');pulse.id='pulse';document.body.appendChild(pulse);"
                "for(let i=0;i<7;i++){let x=document.createElement('x-depart');x.id='extra'+i;document.body.appendChild(x)}"
                "setTimeout(()=>calls++,100);requestAnimationFrame(()=>calls++);requestIdleCallback(()=>calls++);"
                "globalThis.channel=new MessageChannel();channel.port1.postMessage({payload:123});"
                "globalThis.blob=new Blob(['kept']);globalThis.canvas=document.createElement('canvas');"
                "globalThis.paint=canvas.getContext('2d');paint.fillRect(0,0,1,1);"
                "globalThis.anim=node.animate({opacity:[0,1]},1000);"
                "fetch('/pending');localStorage.setItem('key','kept');",
                "departure: queued activity and retained objects created");
            realm_script(realms[1],"calls=0;", "departure: reset setup observer count");
            for (int i=1; i>=0; --i) {
                os64_html_document_t *doc=bindings[i]->document;
                int64_t native_status;
                os64_html_node_t *added=os64_html_create_element(doc,OS64_HTML_NS_HTML,"span",&native_status);
                check(added&&os64_html_insert(doc,doc->body,added,NULL)==OS64_HTML_OK,
                      "departure: native child-list mutation queues observer payload");
                os64_html_node_t *node=(os64_html_node_t *)native_id(doc->document,"pulse");
                check(node&&os64_html_set_attr(doc,node,"data-pulse","yes",3)==OS64_HTML_OK,
                      "departure: native mutation queues child then sibling reactions");
            }
            /* Fill the shared eight-slot backup queue, then refuse its growth
             * after the next element's payload and native record are made. */
            for (unsigned i=0; i<7; ++i) {
                char id[24]; snprintf(id,sizeof(id),"extra%u",i);
                os64_html_node_t *node=(os64_html_node_t *)native_id(p.binding->document->document,id);
                size_t cut=dom_attempts+3;
                if (i==6) dom_fail_at=cut;
                check(node&&os64_html_set_attr(p.binding->document,node,"data-pulse","yes",3)==OS64_HTML_OK,
                      "departure: native mutation survives reaction queue refusal");
                dom_fail_at=0;
                if (i==6) check(dom_attempts>=cut,"departure: reaction queue allocation refused");
            }
            check(os64_dom_depart_finish(p.binding,&out)==OS64_JS_BAD_ARGUMENT,
                  "departure: finish requires mark");
            if (mode==1) realm_script(realms[1],"alert('mark');globalThis.unwound=1;",
                                     "departure: mark inside child callback unwinds");
            else if (mode==2) realm_script(realms[0],"B.alert('mark');globalThis.unwound=1;",
                                          "departure: sibling callback marks child");
            else {
                size_t attempts=dom_attempts;
                check(os64_dom_depart(p.binding,&out)==OS64_JS_OK,"departure: idle mark");
                check(dom_attempts==attempts,"departure: mark allocates no binding storage");
            }
            check(!p.binding->departure_finished && os64_dom_fetches_pending(p.binding)==1,
                  "departure: mark leaves borrowed records intact");
            p.armed=true;
            (void)d_host_now_ms(bindings[0]); /* Native sibling provider, no engine turn. */
            p.armed=false;
            check(os64_js_realm_task_begin(realms[0],OS64_JS_ABI_ID,"departure barrier",&out)==OS64_JS_OK,
                  "departure: sibling task opens");
            os64_js_outcome_t probe;
            check(os64_dom_depart_finish(p.binding,&probe)==OS64_JS_BUSY,"departure: open sibling task blocks finish");
            check(os64_js_task_end(runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"departure: task closes");
            check(d_reactions_push(bindings[0],ctx)==0,"departure: native reaction frame opens");
            check(os64_dom_depart_finish(p.binding,&out)==OS64_JS_BUSY,"departure: shared reaction frame blocks finish");
            check(d_reactions_pop(bindings[0],ctx)==0,"departure: native reaction frame closes");
            bindings[0]->dispatching++;
            check(os64_dom_depart_finish(p.binding,&out)==OS64_JS_BUSY,"departure: sibling dispatch blocks finish");
            bindings[0]->dispatching--;
            /* A catchable sibling exception must not strand an already marked child. */
            const char *bad="throw Error('ordinary sibling error')";
            check(os64_js_realm_run(realms[0],bad,strlen(bad),"ordinary",&out)==OS64_JS_EXCEPTION,
                  "departure: ordinary sibling exception");
            size_t custom_before=os64_dom_memory(bindings[0],OS64_DOM_MEM_CUSTOM).blocks+
                                 os64_dom_memory(bindings[1],OS64_DOM_MEM_CUSTOM).blocks;
            check(os64_dom_depart_finish(p.binding,&out)==OS64_JS_OK,"departure: finish after exception");
            check(os64_dom_memory(bindings[0],OS64_DOM_MEM_CUSTOM).blocks+
                  os64_dom_memory(bindings[1],OS64_DOM_MEM_CUSTOM).blocks+16<=custom_before,
                  "departure: obsolete custom payload released before sibling delivery");
            realm_script(realms[0],"assert(B.mo.takeRecords().length===0);",
                         "departure: child observer payload discarded before sibling delivery");
            check(p.binding->departure_finished&&!p.binding->closed&&p.cancellations==1&&p.store.commits==1,
                  "departure: cancellation and final storage commit occur once without drain");
            check(!p.binding->options.host_opaque&&!p.binding->options.alert_opaque&&
                  !p.binding->network.opaque&&!p.binding->storage_providers[0].opaque&&
                  !p.binding->timer_count&&!p.binding->animations&&!p.binding->idle_count&&
                  !p.binding->messages&&os64_dom_fetches_pending(p.binding)==0,
                  "departure: providers and obsolete activity detached");
            check(os64_dom_reactions_run(bindings[0],&out)&&out.status==OS64_JS_EXCEPTION&&
                  strstr(out.message,"custom element reactions were lost"),
                  "departure: shared owed reactions remain runnable through sibling");
            realm_script(realms[0],"assert(reactions===1&&B.reactions===0&&B.mo.takeRecords().length===0);",
                         "departure: child payloads discarded and live reaction preserved");
            p.freed=true;
            check(os64_dom_depart_finish(p.binding,&out)==OS64_JS_OK&&p.cancellations==1&&p.store.commits==1,
                  "departure: repeated finish never returns to freed host");
            realm_script(realms[0],"assert(B.retained()===B.node&&B.blob.size===4);"
                "document.body.appendChild(B.node);B.node.textContent='still usable';"
                "assert(B.node.textContent==='still usable');B.paint.fillRect(1,1,1,1);"
                "B.anim.currentTime=25;B.anim.effect.setKeyframes({opacity:[1,0]});"
                "B.mo.disconnect();B.channel.port1.close();B.channel.port2.close();"
                "assert(B.calls===0);setTimeout(()=>liveCalls+=10,0);",
                "departure: retained objects and adopted node survive");
            check(os64_dom_timer_fire(bindings[0],1000,&out),"departure: sibling timer still runs");
            realm_script(realms[0],"assert(liveCalls>=11);",
                         "departure: sibling observers and timers survive cleanup");
            p.freed=false; /* Parent's provider remains valid until root teardown. */
        }
        os64_dom_cluster_drain(cluster); os64_js_destroy(runtime); os64_dom_cluster_free(cluster);
        fake_store_free(&p.store);
        check(live==0&&dom_live==0&&!dom_allocations,"departure: all retained allocations release at root teardown");
    }
    DomFixture failed=fixture_new(NULL);
    if (fixture_ready(&failed)) {
        os64_js_outcome_t out;
        os64_js_cancel(failed.runtime);
        check(os64_dom_depart(failed.binding,&out)==OS64_JS_OK,"departure: mark remains available after failure");
        check(os64_dom_depart_finish(failed.binding,&out)==OS64_JS_FAILED_RUNTIME&&!failed.binding->departure_finished,
              "departure: sticky failure cannot certify child cleanup");
    }
    fixture_free(&failed);
}
