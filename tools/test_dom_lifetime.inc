/* Selected native host proof. Raw binding creation remains compatibility
 * admission; this fixture does not select a public checked DOM profile. */
static bool lifetime_controls;
static void lifetime_assert(bool ok,const char *message)
{check(ok,message);if(!ok && lifetime_controls)exit(1);}
typedef struct {
    os64_js_runtime_t *runtime;
    os64_dom_cluster_t *cluster;
    os64_dom_cluster_t *other;
    os64_js_runtime_t *other_runtime;
    os64_dom_t *binding[4];
    os64_js_realm_t *realm[4];
    JSContext *context[4];
    unsigned mode, maps, facts;
    JSOwnerId original;
} LifetimeFixture;
static bool lifetime_exhaust_context;
JSOwnerStatus d_lifetime_observed_context_reserve(JSOwnerIncarnationIssuer *issuer, uint64_t *out)
{
    if (lifetime_exhaust_context) __atomic_store_n(&issuer->_last, UINT64_MAX, __ATOMIC_RELAXED);
    return js_owner_incarnation_reserve(issuer, out);
}
static bool lifetime_host_describe(void *opaque, os64_js_runtime_t *runtime, uint64_t *out)
{
    LifetimeFixture *f=opaque;JSOwnerRuntimeMetadata metadata;
    if(runtime!=f->runtime || !js_owner_runtime_metadata(runtime,&metadata))return false;
    *out=metadata.incarnation;
    if(f->mode==5)os64_dom_cluster_drain(f->cluster);
    return true;
}
static bool lifetime_host_map(void *opaque, const os64_js_realm_t *realm, JSOwnerId *out)
{
    LifetimeFixture *f=opaque;f->maps++;
    if(f->mode==1)return false;
    if(f->mode==2) {
        os64_js_outcome_t outcome;JSOwnerId id={91,92};DLifetimeSnapshot facts;memset(&facts,0xa7,sizeof facts);
        DLifetimeSnapshot saved=facts;
        lifetime_assert(d_lifetime_register(f->cluster,(os64_dom_t *)(uintptr_t)1,&id,&outcome)==OS64_JS_BUSY && id.serial==92 &&
            d_lifetime_observe(f->cluster,f->original,1,&facts,&outcome)==OS64_JS_BUSY && !memcmp(&facts,&saved,sizeof facts),
            "C3 lifetimes: bridge reentry refuses before native identity dereference");
        lifetime_assert(!d_browsing_context_create(f->cluster,NULL,f->binding[2],"reentry",&outcome) && outcome.status==OS64_JS_BUSY,
            "C3 lifetimes: bridge borrow excludes native publication");
    }
    JSOwnerTicket ticket;JSOwnerRealmSnapshot facts;
    if(js_owner_runtime_realm_ticket(f->runtime,realm,&ticket)!=JS_OWNER_OK)return false;
    JSOwnerStatus mapped=js_owner_runtime_owner(f->runtime,ticket,&facts);
    JSOwnerStatus closed=js_owner_runtime_close(f->runtime,ticket);
    if(f->mode==3)f->cluster->poisoned=true;
    if(f->mode==4)os64_dom_cluster_drain(f->cluster);
    if(mapped!=JS_OWNER_OK || closed!=JS_OWNER_OK)return false;
    *out=facts.owner;return true;
}
static bool lifetime_host_facts(void *opaque, JSOwnerId owner, JSOwnerRealmSnapshot *out)
{
    LifetimeFixture *f=opaque;f->facts++;
    bool copied=js_owner_runtime_owner_snapshot(f->runtime,owner,out)==JS_OWNER_OK;
    if(f->mode==7)os64_dom_cluster_drain(f->cluster);
    return copied;
}
static bool lifetime_host_context(void *opaque, JSOwnerContextId *out)
{
    LifetimeFixture *f=opaque;bool issued=js_owner_runtime_context_id(f->runtime,out)==JS_OWNER_OK;
    if(f->mode==6)os64_dom_cluster_drain(f->cluster);
    return issued;
}
static DLifetimeHost lifetime_host(LifetimeFixture *f)
{return (DLifetimeHost){f,lifetime_host_describe,lifetime_host_map,lifetime_host_facts,lifetime_host_context};}
static os64_dom_t *lifetime_new_binding(os64_dom_cluster_t *cluster,os64_js_realm_t *realm,const os64_js_config_t *config,unsigned origin)
{
    DomFixture source=fixture_configured(0,0,config);os64_js_destroy(source.runtime);
    os64_dom_options_t options=p2b_options("https://c3.test/document");way_origin_t principal;
    way_origin_of(origin?"https://other.test/":"https://c3.test/",0,&principal);os64_js_outcome_t out;
    os64_dom_admission_t admission=d_create_in_realm_with_origin(cluster,realm,source.document,source.state,&options,&principal,&out);
    if(!admission.consumed){os64_page_state_free(source.state);os64_html_document_free(source.document);}
    return admission.binding;
}
static bool lifetime_open(LifetimeFixture *f,int policy,bool selected)
{
    *f=(LifetimeFixture){0};os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
    os64_js_outcome_t out;
    os64_js_status_t result=selected?js_owner_runtime_create(&config,policy,OS64_JS_ABI_ID,(JSOwnerLimits){8,16,8},&f->runtime,&out)
        :os64_js_create_with_teardown(&config,policy,OS64_JS_ABI_ID,&f->runtime,&out);
    if(result!=OS64_JS_OK)return false;
    os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
    f->cluster=os64_dom_cluster_create_with_browser_jobs(f->runtime,&limits,&out);if(!f->cluster)return false;
    for(unsigned i=0;i<4;i++) {
        if(!i)f->realm[i]=os64_js_primary_realm(f->runtime,OS64_JS_ABI_ID,&out);
        else if(os64_js_realm_create(f->runtime,OS64_JS_ABI_ID,&f->realm[i],&out)!=OS64_JS_OK)return false;
        f->context[i]=os64_js_realm_context(f->realm[i],OS64_JS_ABI_ID,&out);
        f->binding[i]=lifetime_new_binding(f->cluster,f->realm[i],&config,(i==1 || i==2));if(!f->binding[i])return false;
    }
    return true;
}
static void lifetime_close(LifetimeFixture *f)
{
    os64_dom_cluster_drain(f->cluster);DLifetimeSummary counts;bool selected=d_lifetime_summary(f->cluster,&counts);
    os64_dom_cluster_drain(f->other);
    if(selected)lifetime_assert(counts.closed,"C3 lifetimes: entry gate closes before engine destruction");
    os64_js_teardown_report_t report;os64_js_destroy_report(f->runtime,&report);
    lifetime_assert(!report.leaked,"C3 lifetimes: grouped drain balances engine roots under each teardown policy");
    if(f->other_runtime) {
        os64_js_destroy_report(f->other_runtime,&report);
        lifetime_assert(!report.leaked,"C3 lifetimes: independent cluster runtime root cleanup balances");
    }
    if(selected) {
        DLifetimeHost host=lifetime_host(f);os64_js_outcome_t out;JSOwnerId id={91,92};DLifetimeSnapshot facts;
        memset(&facts,0x97,sizeof facts);DLifetimeSnapshot saved=facts;
        lifetime_assert(d_lifetime_summary(f->cluster,&counts) && counts.closed &&
            d_lifetime_select(f->cluster,1,&host,&out)==OS64_JS_ADMISSION_REFUSED &&
            d_lifetime_register(f->cluster,(os64_dom_t *)(uintptr_t)1,&id,&out)==OS64_JS_ADMISSION_REFUSED && id.serial==92 &&
            d_lifetime_observe(f->cluster,f->original,1,&facts,&out)==OS64_JS_ADMISSION_REFUSED && !memcmp(&facts,&saved,sizeof facts),
            "C3 lifetimes: closed adapter never consults destroyed runtime or stale native identity");
    }
    os64_dom_cluster_free(f->cluster);os64_dom_cluster_free(f->other);f->cluster=NULL;f->runtime=NULL;
    lifetime_assert(!live && !dom_live && !dom_allocations,"C3 lifetimes: native adapter ledger and grouped storage balance");
}
static void lifetime_unchanged(LifetimeFixture *f,JSOwnerId id,uint64_t generation,os64_js_status_t expected,const char *message)
{
    DLifetimeSnapshot facts;memset(&facts,0xc7,sizeof facts);DLifetimeSnapshot saved=facts;os64_js_outcome_t out;
    lifetime_assert(d_lifetime_observe(f->cluster,id,generation,&facts,&out)==expected && !memcmp(&facts,&saved,sizeof facts),message);
}
static LifetimeFixture *lifetime_finalizer_fixture;
static unsigned lifetime_finalizers;
static JSClassID lifetime_finalizer_slot;
static void lifetime_finalizer(JSRuntime *runtime,JSValue value)
{
    (void)runtime;(void)value;LifetimeFixture *f=lifetime_finalizer_fixture;DLifetimeSummary counts;
    lifetime_finalizers++;
    lifetime_assert(d_lifetime_summary(f->cluster,&counts)&&counts.closed,
        "C3 lifetimes: native value finalizer sees closed diagnostic storage");
    lifetime_unchanged(f,f->original,1,OS64_JS_ADMISSION_REFUSED,
        "C3 lifetimes: drain finalizer cannot observe detached native references");
}
static void dom_lifetime_cases(void)
{
    JSOwnerId previous_owner={0};JSOwnerContextId previous_context={0};
    for(int policy=OS64_JS_TEARDOWN_FATAL;policy<=OS64_JS_TEARDOWN_RECLAIM;policy++) {
        LifetimeFixture f;os64_js_outcome_t out;DLifetimeSummary counts;DLifetimeSnapshot facts;
        lifetime_assert(lifetime_open(&f,policy,false),"C3 lifetimes: default host fixture");DLifetimeHost host=lifetime_host(&f);
        lifetime_assert(d_lifetime_select(f.cluster,4,&host,&out)==OS64_JS_ADMISSION_REFUSED && !d_lifetime_summary(f.cluster,&counts),
            "C3 lifetimes: default owner arena cannot fabricate checked identity");lifetime_close(&f);

        lifetime_assert(lifetime_open(&f,policy,true),"C3 lifetimes: selected host fixture");host=lifetime_host(&f);
        DBrowsingContext *root=d_browsing_context_create(f.cluster,NULL,f.binding[0],"root",&out);
        DBrowsingContext *child=d_browsing_context_create(f.cluster,root,f.binding[1],"child",&out);
        lifetime_assert(root&&child,"C3 lifetimes: original contexts created");size_t before=f.cluster->bytes;
        dom_fail_at=dom_attempts+1;
        lifetime_assert(d_lifetime_select(f.cluster,4,&host,&out)==OS64_JS_ADMISSION_REFUSED && f.cluster->bytes==before,
            "C3 lifetimes: adapter allocation refusal publishes no partial charge");dom_fail_at=0;
        size_t limit=f.cluster->limits.dom_bytes;f.cluster->limits.dom_bytes=before;
        lifetime_assert(d_lifetime_select(f.cluster,4,&host,&out)==OS64_JS_ADMISSION_REFUSED,"C3 lifetimes: aggregate native limit includes adapter");
        f.cluster->limits.dom_bytes=limit;
        lifetime_assert(d_lifetime_select(f.cluster,SIZE_MAX,&host,&out)==OS64_JS_BAD_ARGUMENT &&
            d_lifetime_select(f.cluster,4,&host,&out)==OS64_JS_OK && d_lifetime_summary(f.cluster,&counts) &&
            counts.capacity==4 && counts.charged_bytes==f.cluster->bytes-before,
            "C3 lifetimes: bounded arena is selected and charged before registration");
        JSOwnerId ids[4];ids[1]=(JSOwnerId){91,92};
        lifetime_assert(d_lifetime_register(f.cluster,f.binding[1],&ids[1],&out)==OS64_JS_ADMISSION_REFUSED && ids[1].serial==92,
            "C3 lifetimes: missing parent map refuses unchanged output");
        f.mode=1;ids[0]=(JSOwnerId){91,92};
        lifetime_assert(d_lifetime_register(f.cluster,f.binding[0],&ids[0],&out)==OS64_JS_ADMISSION_REFUSED && ids[0].serial==92 &&
            d_lifetime_summary(f.cluster,&counts) && !counts.bindings && !counts.contexts,
            "C3 lifetimes: refused original-owner map publishes neither owner nor context");f.mode=0;
        lifetime_assert(d_lifetime_register(f.cluster,f.binding[0],&ids[0],&out)==OS64_JS_OK &&
            d_lifetime_register(f.cluster,f.binding[1],&ids[1],&out)==OS64_JS_OK &&
            d_lifetime_register(f.cluster,f.binding[2],&ids[2],&out)==OS64_JS_OK,"C3 lifetimes: fallible maps register original bindings");
        f.original=ids[1];
        JSOwnerRuntimeMetadata metadata;lifetime_assert(js_owner_runtime_metadata(f.runtime,&metadata)&&metadata.summary.unknown_escape && !metadata.summary.tickets,
            "C3 lifetimes: compatibility conversion keeps raw veto and closes temporary holders");
        lifetime_assert(d_lifetime_observe(f.cluster,ids[2],0,&facts,&out)==OS64_JS_OK && !facts.associated && !facts.discarded && !facts.context.serial,
            "C3 lifetimes: unassociated binding differs from discarded context");
        JSOwnerContextId stable;
        lifetime_assert(d_lifetime_observe(f.cluster,ids[1],1,&facts,&out)==OS64_JS_OK && facts.associated && facts.has_active && facts.active_eligible &&
            facts.has_relevant_origin && !strcmp(facts.relevant_origin.host,"other.test") && facts.context.serial && facts.parent.serial && facts.context.serial!=facts.parent.serial && facts.escaped_unknown && facts.graph_unknown && facts.native_borrows_unknown,
            "C3 lifetimes: context publication and original owner are separate from unknown exposure census");stable=facts.context;
        if(previous_owner.serial) {
            lifetime_assert(stable.incarnation!=previous_context.incarnation,"C3 lifetimes: a recreated runtime has a distinct context namespace");
            lifetime_unchanged(&f,previous_owner,1,OS64_JS_ADMISSION_REFUSED,"C3 lifetimes: copied old owner cannot resolve a new runtime binding");
        }
        previous_owner=ids[1];previous_context=stable;
        lifetime_unchanged(&f,ids[1],2,OS64_JS_ADMISSION_REFUSED,"C3 lifetimes: stale publication cannot observe current target");
        f.mode=2;lifetime_assert(d_lifetime_register(f.cluster,f.binding[3],&ids[3],&out)==OS64_JS_OK,
            "C3 lifetimes: host map can test reentry without partial publication");f.mode=0;
        f.cluster->provider_borrows++;
        lifetime_unchanged(&f,ids[1],1,OS64_JS_BUSY,"C3 lifetimes: cluster provider borrow refuses scalar observation");f.cluster->provider_borrows--;
        f.cluster->dispatching++;
        lifetime_unchanged(&f,ids[1],1,OS64_JS_BUSY,"C3 lifetimes: cluster dispatch refuses scalar observation");f.cluster->dispatching--;
        f.binding[2]->departure_finishing=true;
        lifetime_unchanged(&f,ids[1],1,OS64_JS_BUSY,"C3 lifetimes: sibling native departure excludes observation");f.binding[2]->departure_finishing=false;
        lifetime_assert(os64_js_realm_task_begin(f.realm[0],OS64_JS_ABI_ID,"C3 busy",&out)==OS64_JS_OK,"C3 lifetimes: open task barrier fixture");
        lifetime_unchanged(&f,ids[1],1,OS64_JS_BUSY,"C3 lifetimes: wrapper task refuses observation");
        lifetime_assert(os64_js_task_end(f.runtime,OS64_JS_ABI_ID,&out)==OS64_JS_OK,"C3 lifetimes: task barrier releases");
        f.cluster->poisoned=true;lifetime_unchanged(&f,ids[1],1,OS64_JS_FAILED_RUNTIME,"C3 lifetimes: poison is unavailable, never zero borrows");f.cluster->poisoned=false;
        JSValue proxy=d_window_proxy(f.context[0],child);JSValue global=JS_GetGlobalObject(f.context[0]);
        lifetime_assert(JS_SetPropertyStr(f.context[0],global,"keptProxy",proxy)>=0,"C3 lifetimes: actual proxy can escape its native caller");JS_FreeValue(f.context[0],global);
        realm_script(f.realm[0],STORAGE_ASSERT "globalThis.keptClose=keptProxy.close;globalThis.keptHref=Object.getOwnPropertyDescriptor(keptProxy.location,'href').set;assert(typeof keptClose==='function'&&typeof keptHref==='function');",
            "C3 lifetimes: actual target caches export retained functions");
        lifetime_assert(d_window_identity_prepare(f.context[1],f.binding[1],child)==0 &&
            d_window_identity_prepare(f.context[3],f.binding[3],child)==0,"C3 lifetimes: private Window identity preparation");
        lifetime_assert(d_lifetime_observe(f.cluster,ids[1],1,&facts,&out)==OS64_JS_OK && facts.known.window_proxy_roots==1 &&
            facts.known.window_descriptor_roots && facts.known.location_backing_roots==1 && facts.known.location_descriptor_roots && facts.known.window_identity_aliases==1,
            "C3 lifetimes: original binding cache roots and Window alias are distinct from escaped exposure census");
#ifdef DOM_C3_F4_NAMES
        lifetime_assert(d_browsing_context_rename(child,1,f.binding[1],"renamed",7) &&
            d_lifetime_observe(f.cluster,ids[1],1,&facts,&out)==OS64_JS_OK && facts.context.serial==stable.serial,
            "C3 lifetimes: F4 rename changes neither context identity nor publication");
#endif
        lifetime_assert(os64_js_realm_retire(f.realm[1],OS64_JS_ABI_ID,&out)==OS64_JS_OK &&
            d_lifetime_observe(f.cluster,ids[1],1,&facts,&out)==OS64_JS_OK && !facts.active_eligible && facts.active.retired && facts.generation==1,
            "C3 lifetimes: retirement changes eligibility without context publication");
        lifetime_assert(d_browsing_context_replace(child,1,f.binding[3],&out)==OS64_JS_OK &&
            d_lifetime_register(f.cluster,f.binding[3],&ids[3],&out)==OS64_JS_OK &&
            d_lifetime_observe(f.cluster,ids[1],2,&facts,&out)==OS64_JS_OK && facts.context.serial==stable.serial &&
            facts.relevant.owner.serial==ids[1].serial && facts.active.owner.serial==ids[3].serial && facts.active_eligible && facts.has_active_origin &&
            !strcmp(facts.relevant_origin.host,"other.test") && !strcmp(facts.active_origin.host,"c3.test") &&
            facts.known.location_descriptor_roots && facts.escaped_unknown,
            "C3 lifetimes: replacement retains context and relevant Location owner while target changes");
        lifetime_assert(d_browsing_context_discard(child,2,&out)==OS64_JS_OK &&
            d_lifetime_observe(f.cluster,ids[1],3,&facts,&out)==OS64_JS_OK && facts.discarded && !facts.has_active && facts.has_last_window &&
            facts.last_window.owner.serial==ids[3].serial && facts.context.serial==stable.serial,
            "C3 lifetimes: discard preserves context and last Window independently of active target");
        os64_dom_cluster_limits_t limits={64u<<20,16u<<20,8u<<20,256u<<20,128u<<20};
        os64_js_config_t config=fixture_config();config.limits.memory_bytes=64u<<20;
        lifetime_assert(!os64_dom_cluster_create_with_browser_jobs(f.runtime,&limits,&out),"C3 lifetimes: existing one-cluster-per-runtime admission stays enforced");
        lifetime_assert(js_owner_runtime_create(&config,policy,OS64_JS_ABI_ID,(JSOwnerLimits){2,4,2},&f.other_runtime,&out)==OS64_JS_OK,
            "C3 lifetimes: independent selected runtime for second cluster");
        f.other=os64_dom_cluster_create_with_browser_jobs(f.other_runtime,&limits,&out);
        os64_js_realm_t *extra=os64_js_primary_realm(f.other_runtime,OS64_JS_ABI_ID,&out);
        LifetimeFixture second={.runtime=f.other_runtime,.cluster=f.other};DLifetimeHost second_host=lifetime_host(&second);
        lifetime_assert(f.other&&extra,"C3 lifetimes: second cluster follows existing runtime admission");
        os64_dom_t *binding=lifetime_new_binding(f.other,extra,&config,0);JSOwnerId other;
        DBrowsingContext *other_context=d_browsing_context_create(f.other,NULL,binding,"other",&out);
        lifetime_assert(binding&&other_context&&d_lifetime_select(f.other,1,&second_host,&out)==OS64_JS_OK &&
            d_lifetime_register(f.other,binding,&other,&out)==OS64_JS_OK && d_lifetime_observe(f.other,other,1,&facts,&out)==OS64_JS_OK &&
            facts.context.incarnation!=stable.incarnation && facts.context.serial && !facts.parent.serial,
            "C3 lifetimes: second cluster context has a distinct runtime-scoped identity");
        lifetime_unchanged(&second,ids[1],1,OS64_JS_ADMISSION_REFUSED,"C3 lifetimes: foreign cluster owner cannot resolve by matching serial");
        JSClassDef definition={.class_name="C3 drain probe",.finalizer=lifetime_finalizer};
        JSClassID slot=os64_js_class_id(&lifetime_finalizer_slot);lifetime_assert(JS_NewClass(JS_GetRuntime(f.context[0]),slot,&definition)==0,"C3 lifetimes: native drain finalizer class");
        lifetime_finalizer_fixture=&f;lifetime_finalizers=0;
        lifetime_assert(d_retain(f.binding[0],f.context[0],JS_NewObjectClass(f.context[0],slot))!=NULL,"C3 lifetimes: root native finalizer fixture");
        lifetime_close(&f);lifetime_assert(lifetime_finalizers==1,"C3 lifetimes: drain finalizer actually ran");lifetime_finalizer_fixture=NULL;

        for(unsigned mode=0;mode<8;mode++) {
            lifetime_assert(lifetime_open(&f,policy,true),"C3 lifetimes: capacity/issuer/bridge fault fixture");host=lifetime_host(&f);
            root=d_browsing_context_create(f.cluster,NULL,f.binding[0],"root",&out);
            child=d_browsing_context_create(f.cluster,root,f.binding[1],"child",&out);
            if(mode==4) {
                f.mode=5;
                lifetime_assert(d_lifetime_select(f.cluster,4,&host,&out)==OS64_JS_ADMISSION_REFUSED && !d_lifetime_summary(f.cluster,&counts),
                    "C3 lifetimes: drain during host description prevents adapter publication");lifetime_close(&f);continue;
            }
            lifetime_assert(root&&child&&d_lifetime_select(f.cluster,mode?4:1,&host,&out)==OS64_JS_OK,"C3 lifetimes: fault arena selection");
            ids[0]=(JSOwnerId){91,92};
            if(mode==1)lifetime_exhaust_context=true;
            if(mode==2 || mode==3)f.mode=mode+1;
            if(mode==5)f.mode=6;
            os64_js_status_t result=d_lifetime_register(f.cluster,f.binding[0],&ids[0],&out);lifetime_exhaust_context=false;
            if(!mode) {
                lifetime_assert(result==OS64_JS_OK,"C3 lifetimes: capacity-one first registration");f.original=ids[0];
                ids[1]=(JSOwnerId){91,92};
                lifetime_assert(d_lifetime_register(f.cluster,f.binding[1],&ids[1],&out)==OS64_JS_ADMISSION_REFUSED && ids[1].serial==92 &&
                    d_lifetime_observe(f.cluster,ids[0],1,&facts,&out)==OS64_JS_OK,
                    "C3 lifetimes: full registry refuses with old identity intact and no eviction");
            } else if(mode==6) {
                lifetime_assert(result==OS64_JS_OK,"C3 lifetimes: facts drain observation fixture");f.original=ids[0];f.mode=7;
                lifetime_unchanged(&f,ids[0],1,OS64_JS_ADMISSION_REFUSED,"C3 lifetimes: drain during scalar bridge prevents snapshot publication");
            } else if(mode==7) {
                lifetime_assert(result==OS64_JS_OK,"C3 lifetimes: unmapped replacement fixture");f.original=ids[0];
                lifetime_assert(d_lifetime_observe(f.cluster,ids[0],1,&facts,&out)==OS64_JS_OK,"C3 lifetimes: original target mapped");JSOwnerContextId original=facts.context;
                lifetime_assert(d_browsing_context_replace(root,1,f.binding[2],&out)==OS64_JS_OK,"C3 lifetimes: publish an unregistered target");
#ifdef DOM_C3_F4_NAMES
                lifetime_assert(d_browsing_context_name_length(root)==0,"C3 lifetimes: F4 cross-origin root publication resets name without replacing context");
#endif
                lifetime_unchanged(&f,ids[0],2,OS64_JS_ADMISSION_REFUSED,"C3 lifetimes: missing target owner map refuses unchanged snapshot");
                lifetime_assert(d_lifetime_register(f.cluster,f.binding[2],&ids[2],&out)==OS64_JS_OK &&
                    d_lifetime_observe(f.cluster,ids[0],2,&facts,&out)==OS64_JS_OK && facts.context.serial==original.serial && facts.active.owner.serial==ids[2].serial,
                    "C3 lifetimes: mapping new target does not reassign retained context identity");
            } else lifetime_assert(result==OS64_JS_ADMISSION_REFUSED && ids[0].serial==92 && d_lifetime_summary(f.cluster,&counts) && !counts.bindings && !counts.contexts,
                "C3 lifetimes: issuer exhaustion or bridge poison/drain publishes no partial identity");
            lifetime_close(&f);
        }
    }
}
