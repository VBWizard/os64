// Browser callers and producer revisions, including a hidden measurement that
// precedes the displayed page's model/state bookkeeping.
static void graft_browser_cases(void)
{
    char html[5000];strcpy(html,"<!doctype html><style>p{margin:4px}</style><p id=changed>first</p>"
        "<p id=kept>kept</p><input id=field value=before><canvas id=canvas width=40 height=30>");
    strcat(html,"</canvas><p><img id=picture src=https://fixture.test/kept.png></p>");
    for(int i=0;i<20;i++)strcat(html,"<p>stable paragraph with words</p>");
    probe_page(html,true);
    os64_html_document_t *doc=g.page.way.doc;
    os64_page_state_t *state=os64_page_shared_state(page_model(&g.page));
    check(!os64_html_set_text(doc,probe_id("changed")->first_child,"a longer first paragraph",24) && script_rebuild(),
          "graft browser: changed paragraph publishes");
    // Form widgets establish their initial shared state after the fixture's
    // first layout. The next update starts from that established state.
    check(!os64_html_set_text(doc,probe_id("changed")->first_child,"another changed paragraph",25) && script_rebuild(),
          "graft browser: second paragraph update publishes");
    check(last_graft_profile.reused>0 && last_graft_profile.measures<10,
          "graft browser: displayed updates copy stable paragraph geometry");
    check(measured_layout(&g.page,state,g.page.way.url,800,600),"graft browser: hidden measurement");
    uint64_t layouts=geometry_layout_total;
    check(measured_layout(&g.page,state,g.page.way.url,800,600) && geometry_layout_total==layouts,
          "graft browser: unchanged hidden read reuses its complete answer");
    uint64_t dom_version=os64_html_version(doc),shown_state=g.page.state_version;
    check(!os64_page_node_set_value(state,probe_id("field"),"typed without an attribute",26) &&
          os64_html_version(doc)==dom_version && g.page.state_version==shown_state,
          "graft browser: form state can advance before displayed bookkeeping");
    check(measured_layout(&g.page,state,g.page.way.url,800,600) && geometry_layout_total>layouts &&
          !last_graft_profile.reused && flow_reuse_stats(s_measured.tree).refused[FLOW_REUSE_INPUTS_CHANGED],
          "graft browser: hidden cache and geometry both reject changed shared form state");
    layouts=geometry_layout_total;
    uint64_t font_revision=s_layout_resources.fonts;
    webfonts_move();s_web.moved=false;
    check(s_layout_resources.fonts>font_revision && measured_layout(&g.page,state,g.page.way.url,800,600) &&
          geometry_layout_total>layouts && !last_graft_profile.reused,
          "graft browser: font producer invalidates even after the batching flag clears");

    pictures_start(&g.page);
    int picture=picture_find(&g.page,"https://fixture.test/kept.png");
    check(picture>=0,"graft browser: picture catalog entry");
    if(picture>=0) {
        g.page.pics[picture].state=PIC_WAITING;g.page.waiting=g.page.in_flight=1;
        yonder_picture_job_t job={.kind=YONDER_JOB_PICTURE,.index=picture,.generation=g.page_serial};
        yonder_picture_t product={.status=OS64_IMAGE_OK,.width=73,.height=29,
            .image={.width=73,.height=29,.pixels=os64_calloc(73*29,4)}};
        uint64_t revision=s_layout_resources.pictures;layouts=geometry_layout_total;
        picture_arrived(&job,&product);g.pictures_moved=false;
        check(s_layout_resources.pictures>revision && measured_layout(&g.page,state,g.page.way.url,800,600) &&
              geometry_layout_total>layouts && !last_graft_profile.reused,
              "graft browser: picture arrival invalidates before any displayed settle");
        const flow_box_t *box=flow_box_for(s_measured.tree,probe_id("picture"));
        check(box && box->rect.w==73 && box->rect.h==29,"graft browser: hidden image gets arrived dimensions");
        int64_t status;os64_html_node_t *new_image=os64_html_create_element(doc,OS64_HTML_NS_HTML,"img",&status);
        os64_html_set_attr(doc,new_image,"src","https://fixture.test/new.png",strlen("https://fixture.test/new.png"));
        os64_html_insert(doc,probe_id("picture")->parent,new_image,probe_id("picture"));
        check(measured_layout(&g.page,state,g.page.way.url,800,600),"graft browser: hidden model inserts earlier image");
        box=flow_box_for(s_measured.tree,probe_id("picture"));
        check(box && box->rect.w==73 && box->rect.h==29,
              "graft browser: fresh image indices resolve by URL rather than stale displayed map");
    }
    check(!os64_html_set_attr(doc,probe_id("canvas"),"width","90",2) &&
          measured_layout(&g.page,state,g.page.way.url,800,600),"graft browser: canvas dimension mutation measures");
    const flow_box_t *canvas=flow_box_for(s_measured.tree,probe_id("canvas"));
    check(canvas && canvas->rect.w==90,"graft browser: canvas resize changes layout through native DOM history");
    bool dark=s_dark,reduce=s_reduce_motion;
    layouts=geometry_layout_total;s_dark=!dark;
    check(measured_layout(&g.page,state,g.page.way.url,800,600) && geometry_layout_total>layouts &&
          !last_graft_profile.reused,"graft browser: hidden cache rejects appearance changes");
    layouts=geometry_layout_total;s_reduce_motion=!reduce;
    check(measured_layout(&g.page,state,g.page.way.url,800,600) && geometry_layout_total>layouts &&
          !last_graft_profile.reused,"graft browser: hidden cache rejects reduced-motion changes");
    s_dark=dark;s_reduce_motion=reduce;
    uint64_t revision=s_layout_resources.pictures;
    s_layout_resources.pictures=UINT64_MAX;layout_revision(&s_layout_resources.pictures);
    check(!layout_inputs(&g.page,state).resource_owner,"graft browser: epoch overflow disables reuse");
    s_layout_resources.pictures=revision;s_layout_resources.exhausted=false;
    probe_drop();

    probe_page("<!doctype html><style>div{width:40px}div:has(.on){width:90px}</style>"
               "<div id=box><p id=child>text</p></div>",true);
    doc=g.page.way.doc;
    state=os64_page_shared_state(page_model(&g.page));
    check(!flow_reuse_stats(g.page.tree).metadata_peak,
          "graft fallback: broad displayed cascade allocates no geometry snapshot");
    check(measured_layout(&g.page,state,g.page.way.url,800,600) &&
          !flow_reuse_stats(s_measured.tree).metadata_peak,
          "graft fallback: broad hidden cascade allocates no geometry snapshot");
    layouts=geometry_layout_total;
    check(measured_layout(&g.page,state,g.page.way.url,800,600) && geometry_layout_total==layouts,
          "graft fallback: unchanged hidden answer remains cached without a snapshot");
    check(!os64_html_set_attr(doc,probe_id("child"),"class","on",2) &&
          measured_layout(&g.page,state,g.page.way.url,800,600),
          "graft fallback: broad selector mutation still rebuilds hidden layout");
    const flow_box_t *broad=flow_box_for(s_measured.tree,probe_id("box"));
    check(broad && broad->rect.w==90 && !flow_reuse_stats(s_measured.tree).metadata_peak,
          "graft fallback: fresh hidden geometry reflects changed descendant");
    check(script_rebuild(),"graft fallback: broad selector mutation publishes");
    broad=flow_box_for(g.page.tree,probe_id("box"));
    check(broad && broad->rect.w==90 && !flow_reuse_stats(g.page.tree).metadata_peak,
          "graft fallback: fresh displayed geometry reflects changed descendant");
    probe_drop();
}

// Both catalog entries are already loaded: changing the resolution base must
// invalidate geometry without relying on a subsequent resource arrival.
static void graft_base_cases(void)
{
    for(int via_base=0;via_base<3;via_base++) {
        char html[4000];strcpy(html,"<!doctype html><base id=base href='https://fixture.test/'>"
            "<p><img id=relative src=kept.png></p>"
            "<p><img src=https://fixture.test/other/kept.png></p><p id=changed>first</p>");
        if(!via_base) {
            // No base element: history's new document URL is the resolver base.
            strcpy(html,"<!doctype html><p><img id=relative src=kept.png></p>"
                "<p><img src=https://fixture.test/other/kept.png></p><p id=changed>first</p>");
        }
        for(int i=0;i<20;i++)strcat(html,"<p>stable words</p>");
        probe_page(html,true);
        pictures_start(&g.page);
        const char *urls[]={"https://fixture.test/kept.png","https://fixture.test/other/kept.png"};
        for(int i=0;i<2;i++) {
            int k=picture_find(&g.page,urls[i]);
            check(k>=0,"graft base: both image URLs enter catalog");
            if(k<0)continue;
            int width=i?91:37;
            g.page.pics[k].state=PIC_WAITING;g.page.waiting=g.page.in_flight=1;
            yonder_picture_job_t job={.kind=YONDER_JOB_PICTURE,.index=k,.generation=g.page_serial};
            yonder_picture_t product={.status=OS64_IMAGE_OK,.width=width,.height=29,
                .image={.width=width,.height=29,.pixels=os64_calloc(width*29,4)}};
            picture_arrived(&job,&product);
        }
        check(page_lay_out(&g.page,800,600,g.zoom,&g.page) &&
              page_lay_out(&g.page,800,600,g.zoom,&g.page) && last_graft_profile.reused>0,
              "graft base: loaded image geometry is eligible for reuse");
        os64_page_state_t *state=os64_page_shared_state(page_model(&g.page));
        check(measured_layout(&g.page,state,g.page.way.url,800,600),"graft base: prime hidden measurement");
        const flow_box_t *box=flow_box_for(s_measured.tree,probe_id("relative"));
        check(box && box->rect.w==37,"graft base: original resolved dimensions");
        uint64_t layouts=geometry_layout_total;
        if(via_base==1)
            os64_html_set_attr(g.page.way.doc,probe_id("base"),"href","https://fixture.test/other/",
                              strlen("https://fixture.test/other/"));
        else {
            os64_strcopy(g.page.way.url,sizeof(g.page.way.url),"https://fixture.test/other/page");
            page_rebase(&g.page);
        }
        check(measured_layout(&g.page,state,g.page.way.url,800,600) && geometry_layout_total>layouts,
              "graft base: hidden read invalidates after URL or base changes");
        box=flow_box_for(s_measured.tree,probe_id("relative"));
        check(box && box->rect.w==(via_base==2?37:91),
              "graft base: hidden image uses the current resolution base");
        if(via_base==2) {
            // History moved under an absolute base. Removing it later must
            // use the new document URL, not the cached model's original URL.
            os64_html_remove_attr(g.page.way.doc,probe_id("base"),"href");
            check(measured_layout(&g.page,state,g.page.way.url,800,600),
                  "graft base: measure after absolute base removal");
            box=flow_box_for(s_measured.tree,probe_id("relative"));
            check(box && box->rect.w==91,"graft base: removed base exposes new history URL");
        }
        os64_html_set_text(g.page.way.doc,probe_id("changed")->first_child,"unrelated update",16);
        check(script_rebuild(),"graft base: unrelated displayed edit publishes");
        box=flow_box_for(g.page.tree,probe_id("relative"));
        check(box && box->rect.w==91,"graft base: displayed image uses the new resolution base");
        probe_drop();
    }
}
