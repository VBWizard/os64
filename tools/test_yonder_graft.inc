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
}
