/* Native context identity is independent of a binding or its document URL.
 * Raw globals are exchanged by authority_open solely for the host proof. */
static unsigned context_provider_calls;
static uint64_t context_clock(void *opaque)
{
    (void)opaque;context_provider_calls++;return 4000;
}

static os64_dom_child_t context_candidate_prepare(CeFixture *f, size_t cut, os64_js_outcome_t *out)
{
    DomFixture doc=child_install_document();
    os64_dom_options_t options=p2b_options("https://document-b.test/replacement");
    options.now_ms=context_clock;
    os64_dom_installation_t installation={.engine_headroom=OS64_DOM_CHILD_ENGINE_HEADROOM,
        .now_ms=4000,.now_us=4000000};
    for(unsigned i=0;i<OS64_DOM_TIMING_COUNT;i++)installation.timing.at[i]=OS64_DOM_TIMING_NOT_YET;
    way_origin_t origin;way_origin_of("https://authority-a.test/",0,&origin);
    context_provider_calls=0;
    dom_fail_at=cut?dom_attempts+cut:0;
    os64_dom_child_t child=d_create_child_with_origin(f->cluster,doc.document,doc.state,
        &options,&installation,&origin,out);
    dom_fail_at=0;
    check(context_provider_calls==0,"contexts: candidate setup never calls document providers");
    child_install_unconsumed(&doc,child);
    return child;
}

static void dom_browsing_context_cases(void)
{
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        CeFixture f;if(!authority_open(&f,policy,0)){check(false,"contexts: fixture");ce_close(&f);continue;}
        os64_js_outcome_t out;
        size_t before=f.cluster->bytes;
        char name[]="root";
        DBrowsingContext *root=d_browsing_context_create(f.cluster,NULL,f.bindings[0],name,&out);
        name[0]='X';
        check(root&&out.status==OS64_JS_OK&&f.cluster->bytes==before+sizeof(*root),
              "contexts: record and bounded mutable name storage charged exactly once");
        if(!root){ce_close(&f);continue;}
        DBrowsingContext *left=d_browsing_context_create(f.cluster,root,f.bindings[1],"duplicate",&out);
        DBrowsingContext *right=d_browsing_context_create(f.cluster,root,f.bindings[2],"duplicate",&out);
        DBrowsingContext *leaf=d_browsing_context_create(f.cluster,left,f.bindings[3],"leaf",&out);
        check(left&&right&&leaf&&d_browsing_context_parent(leaf)==left&&d_browsing_context_top(leaf)==root,
              "contexts: parent/top relations use stable identities");
        check(!strcmp(d_browsing_context_name(root),"root")&&d_browsing_context_child(root,0)==left&&
              d_browsing_context_child(root,1)==right&&!d_browsing_context_child(root,2),
              "contexts: copied names do not collapse duplicate-name sibling order");
        check(!d_browsing_context_create(f.cluster,root,f.bindings[0],"reuse",&out),
              "contexts: a binding cannot publish into two identities");
        size_t reserved=f.cluster->bytes, count=f.cluster->context_count;
        dom_fail_at=dom_attempts+1;
        check(!d_browsing_context_create(f.cluster,root,f.bindings[4],"refused",&out)&&
              out.status==OS64_JS_ADMISSION_REFUSED,"contexts: record allocation refusal");
        dom_fail_at=0;
        check(f.cluster->bytes==reserved&&f.cluster->context_count==count&&!f.bindings[4]->browsing_context,
              "contexts: refused reservation consumes neither identity nor charge");
        size_t cap=f.cluster->limits.dom_bytes;f.cluster->limits.dom_bytes=reserved;
        check(!d_browsing_context_create(f.cluster,root,f.bindings[4],"cap",&out)&&out.status==OS64_JS_ADMISSION_REFUSED,
              "contexts: aggregate cap includes record and name");
        f.cluster->limits.dom_bytes=cap;
        char long_name[D_CONTEXT_NAME_MAX+2];memset(long_name,'n',sizeof(long_name));long_name[sizeof(long_name)-1]=0;
        check(!d_browsing_context_create(f.cluster,root,f.bindings[4],long_name,&out)&&out.status==OS64_JS_BAD_ARGUMENT,
              "contexts: oversized name refuses before allocation");
        /* Exercise the finite registry boundary without constructing 65 DOMs. */
        f.cluster->context_count=D_REALMS;
        check(!d_browsing_context_create(f.cluster,root,f.bindings[4],"cap",&out)&&out.status==OS64_JS_REALM_LIMIT,
              "contexts: cumulative context cap refuses new identity");
        f.cluster->context_count=count;
        DBrowsingContext foreign_parent={0};os64_dom_t foreign_binding={0};
        check(!d_browsing_context_create(f.cluster,&foreign_parent,f.bindings[4],"foreign",&out)&&
              !d_browsing_context_create(f.cluster,root,&foreign_binding,"foreign",&out),
              "contexts: foreign parent/binding cannot enter the registry");
        uint64_t generation=d_browsing_context_generation(left);
        check(d_browsing_context_replace(left,generation,f.bindings[2],&out)==OS64_JS_BAD_ARGUMENT&&
              d_browsing_context_replace(left,generation,NULL,&out)==OS64_JS_BAD_ARGUMENT&&
              d_browsing_context_replace(left,generation,f.bindings[4],NULL)==OS64_JS_BAD_ARGUMENT&&
              !os64_js_realm_is_retired(ce_realms[1]),
              "contexts: associated/missing candidates and missing outcome refuse before departure");
        check(d_browsing_context_replace(left,generation+1,f.bindings[4],&out)==OS64_JS_BAD_ARGUMENT&&
              d_browsing_context_target(left,generation)==f.bindings[1]&&!os64_js_realm_is_retired(ce_realms[1]),
              "contexts: stale replacement preserves old target and eligibility");
        d_provider_hold(f.bindings[2]);
        check(d_browsing_context_replace(left,generation,f.bindings[4],&out)==OS64_JS_BUSY,
              "contexts: sibling provider loan blocks publication");
        d_provider_leave(f.bindings[2]);
        f.bindings[2]->departure_finishing=true;
        check(d_browsing_context_discard(left,generation,&out)==OS64_JS_BUSY,
              "contexts: sibling departure callback excludes reentrant discard");
        f.bindings[2]->departure_finishing=false;
        check(os64_js_realm_task_begin(ce_realms[0],OS64_JS_ABI_ID,"context busy",&out)==OS64_JS_OK,
              "contexts: begin task for publication exclusion");
        check(d_browsing_context_replace(left,generation,f.bindings[4],&out)==OS64_JS_BUSY,
              "contexts: engine task excludes publication");
        check(os64_js_task_end(ce_runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"contexts: busy task closes");
        leaf->generation=UINT64_MAX;
        check(d_browsing_context_replace(left,generation,f.bindings[4],&out)==OS64_JS_ADMISSION_REFUSED&&
              !os64_js_realm_is_retired(ce_realms[1])&&!os64_js_realm_is_retired(ce_realms[3]),
              "contexts: descendant generation exhaustion refuses the whole transaction");
        leaf->generation=1;
        left->generation=UINT64_MAX;
        check(d_browsing_context_discard(left,UINT64_MAX,&out)==OS64_JS_ADMISSION_REFUSED,
              "contexts: target generation cannot wrap");
        left->generation=generation;
        size_t allocations=dom_attempts;
        check(d_browsing_context_replace(left,generation,f.bindings[4],&out)==OS64_JS_OK,
              "contexts: replacement commits");
        check(dom_attempts==allocations&&context_provider_calls==0,
              "contexts: commit allocates no binding storage and calls no provider");
        check(d_browsing_context_target(left,generation)==NULL&&
              d_browsing_context_target(left,generation+1)==f.bindings[4]&&
              f.bindings[1]->browsing_context==left&&f.bindings[4]->browsing_context==left,
              "contexts: old references retain context identity, stale generation loses target");
        check(os64_js_realm_is_retired(ce_realms[1])&&os64_js_realm_is_retired(ce_realms[3])&&
              !os64_js_realm_is_retired(ce_realms[2])&&!d_browsing_context_target(leaf,2)&&
              !d_browsing_context_child(left,0)&&d_browsing_context_child(root,1)==right,
              "contexts: old document descendants discard without disturbing sibling");
        check(d_browsing_context_top(leaf)==root&&!strcmp(d_browsing_context_name(leaf),"leaf")&&
              !d_browsing_context_create(f.cluster,leaf,f.bindings[1],"invalid",&out),
              "contexts: tombstones retain ancestry/name but cannot gain children");
        realm_script(ce_realms[0],"denied(()=>B.status);assert(C.document)",
              "contexts: retired Window member refuses and sibling remains usable");
        check(os64_dom_depart_finish(f.bindings[1],&out)==OS64_JS_OK&&
              os64_dom_depart_finish(f.bindings[3],&out)==OS64_JS_OK,
              "contexts: old provider cleanup completes behind departure barrier");
        check(d_browsing_context_discard(left,generation+1,&out)==OS64_JS_OK&&
              d_browsing_context_child(root,0)==right&&!d_browsing_context_child(root,1)&&
              !d_browsing_context_target(left,generation+2),
              "contexts: discard unlinks sibling and retains unusable tombstone");
        check(d_browsing_context_discard(left,generation+2,&out)==OS64_JS_BAD_ARGUMENT&&
              f.cluster->context_count==count&&f.cluster->bytes<=reserved,
              "contexts: tombstones consume their cumulative identity slot");
        check(d_browsing_context_discard(root,1,&out)==OS64_JS_OK&&
              os64_js_realm_is_retired(ce_realms[0])&&os64_js_realm_is_retired(ce_realms[2]),
              "contexts: root discard retires remaining descendants");
        ce_close(&f);
    }
    const size_t cuts[]={1,2,3,5,17,0};
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++)
    for(size_t i=0;i<sizeof(cuts)/sizeof(cuts[0]);i++) {
        CeFixture f;if(!authority_open(&f,policy,0)){check(false,"contexts: preparation fixture");ce_close(&f);continue;}
        os64_js_outcome_t out;
        DBrowsingContext *root=d_browsing_context_create(f.cluster,NULL,f.bindings[0],"root",&out);
        size_t before=f.cluster->bytes;
        if(i==0) {
            size_t limit=f.cluster->limits.dom_bytes;
            f.cluster->limits.dom_bytes=before+sizeof(os64_dom_t)+sizeof(way_origin_t);
            os64_dom_child_t capped=context_candidate_prepare(&f,0,&out);
            check(!capped.consumed&&capped.state==OS64_DOM_CHILD_REFUSED&&
                  out.status==OS64_JS_ADMISSION_REFUSED&&f.cluster->bytes==before,
                  "contexts: candidate quota preflights shell plus charged origin header");
            f.cluster->limits.dom_bytes=limit;
        }
        os64_dom_child_t child=context_candidate_prepare(&f,cuts[i],&out);
        check(root&&d_browsing_context_target(root,1)==f.bindings[0]&&!os64_js_realm_is_retired(ce_realms[0]),
              "contexts: preparation leaves old target, generation and eligibility intact");
        realm_script(ce_realms[0],"assert(document&&location)","contexts: original page works after candidate preparation");
        if(cuts[i]) {
            check(child.state!=OS64_DOM_CHILD_ACTIVE&&child.state!=OS64_DOM_CHILD_RUNTIME_FAILED,
                  "contexts: sampled native refusal is local");
            check(cuts[i]<=2?(!child.consumed&&f.cluster->bytes==before):child.consumed,
                  "contexts: refusal observes pre/post ownership-transfer boundary");
        } else {
            check(child.state==OS64_DOM_CHILD_ACTIVE&&child.consumed&&child.binding->binding_origin,
                  "contexts: staged origin-bearing candidate installs and activates privately");
            size_t peak=f.cluster->bytes;
            check(d_browsing_context_replace(root,1,child.binding,&out)==OS64_JS_OK&&
                  d_browsing_context_target(root,2)==child.binding,
                  "contexts: staged candidate publishes into retained identity");
            realm_script(child.realm,"if(!document||location.href!=='https://document-b.test/replacement')throw Error('new page')",
                  "contexts: new active document executes with its own URL");
            check(os64_dom_depart_finish(f.bindings[0],&out)==OS64_JS_OK&&
                  f.bindings[0]->options.now_ms==NULL&&child.binding->options.now_ms==context_clock,
                  "contexts: old provider detaches while replacement provider remains routed");
            printf("Context replacement policy %d: before=%zu peak=%zu after-finish=%zu retained-contexts=%zu\n",
                policy,before,peak,f.cluster->bytes,f.cluster->context_count);
        }
        ce_close(&f);
    }
}
