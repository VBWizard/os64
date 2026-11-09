/* I3 uses the production fetch, cache, PNG decoder and window provider. */
static void image_ownership_http(bool cached,bool refused)
{
    size_t baseline=live,len=0;
    char *png=memory_fixture_read("image-memory.png",&len);
    if(!png)return;
    fake_reset();fake_fetch_reset();
    const char *url="https://fixture.test/owned.png";
    fake_answer_t *answer=fake_answer("GET",url,200,
        "Content-Type: image/png\r\nCache-Control: max-age=600\r\n",png);
    answer->body_len=len;
    probe_page("<!doctype html><img id=i src='owned.png' width=160 height=90>"
        "<canvas id=c width=1 height=1></canvas><p id=out>waiting</p><script>"
        "var i=document.getElementById('i'),loads=0;"
        "i.onload=function(){loads++;var c=document.getElementById('c').getContext('2d');"
        "c.drawImage(i,0,0,1,1);var p=c.getImageData(0,0,1,1).data;"
        "document.getElementById('out').textContent=(loads===1&&i.naturalWidth===1920&&i.complete&&"
        "p[0]===32&&p[1]===96&&p[2]===192&&p[3]===255)?'PASS':'FAIL';};</script>",true);
    way_cache_t *cache=cached?way_cache_open("/tmp/i3-cache",16u<<20):NULL;
    g.cache=cache;
    data_pool_open();pictures_start(&g.page);script_turn();
    if(refused)g.page.kept_bytes=PICTURES_KEPT_MAX-10000;
    check(sizes_run(),"image ownership: selected HTTP response arrives");
    Picture *pic=&g.page.pics[0];
    check(g.page.npics==1 && nfake_requests==1 && !pic->full && pic->encoded &&
          pic->encoded->length==len,"image ownership: one request retains exact encoded backing");
    if(refused) {
        os64_dom_image_view_t view;
        check(pic->state==PIC_NOT_KEPT && canvas_source(g.page.way.doc,probe_id("i"),false,&view)==OS64_DOM_IMAGE_READY &&
              view.surface.width==1920,"image ownership: raster refusal preserves successful metadata");
        canvas_source_release(g.page.way.doc,&view);
        check(canvas_source(g.page.way.doc,probe_id("i"),true,&view)==OS64_DOM_IMAGE_REFUSED &&
              pic->encoded && !pic->full,"image ownership: admission refusal preserves reconstruction");
        canvas_source_release(g.page.way.doc,&view);
        g.page.kept_bytes=pic->cost; /* Release the fixture's simulated competing owners. */
    }
    if(cache) {
        way_cache_key_t key={"https fixture.test",true,url};
        way_cache_drop(cache,&key);way_cache_enable(cache,false);
    }
    /* A new response could not reproduce the completed request. */
    answer->body="broken replacement";answer->body_len=strlen(answer->body);
    for(int t=0;t<4;t++)script_turn();
    check(probe_text_is("out","PASS") && nfake_requests==1 && g.page.image_promotions==1,
          "image ownership: one onload draw reads the original PNG with cache cleared/disabled and changed server bytes");
    os64_dom_image_view_t first,second;
    const os64_html_node_t *node=probe_id("i");
    check(canvas_source(g.page.way.doc,node,true,&first)==OS64_DOM_IMAGE_READY &&
          canvas_source(g.page.way.doc,node,true,&second)==OS64_DOM_IMAGE_READY &&
          first.surface.pixels==second.surface.pixels && g.page.image_promotions==1,
          "image ownership: repeated draws borrow the retained promotion");
    canvas_source_release(g.page.way.doc,&second);
    PictureMemory memory=picture_memory(&g.page);
    check(memory.display+memory.native+memory.encoded+memory.sequence+memory.svg==g.page.kept_bytes,
          "image ownership: retained subdivisions reconcile after promotion");
    data_pool_close();g.cache=NULL;way_cache_close(cache);probe_drop();
    check(((uint32_t *)first.surface.pixels)[0]==0xff2060c0 && !strcmp(first.current_src,url),
          "image ownership: pixel and currentSrc loans survive document teardown");
    canvas_source_release(NULL,&first);
    fake_fetch_reset();fake_reset();free(png);
    check(live==baseline,"image ownership: response, cache, catalog and outstanding loans release once");
}

static void image_ownership_shared(void)
{
    size_t baseline=live;
    sizes_open("<!doctype html><img id=i src='data:image/png,SIZED%20400x200'>","RASTER");
    pictures_start(&g.page);responsive_drain();
    const os64_html_node_t *node=probe_id("i");Picture *pic=&g.page.pics[0];
    int decodes=picture_raster_decodes;
    os64_dom_image_view_t view;
    check(canvas_source(g.page.way.doc,node,true,&view)==OS64_DOM_IMAGE_READY &&
          view.surface.pixels==pic->image.pixels && !pic->encoded &&
          picture_raster_decodes==decodes && g.page.image_promotions==0,
          "image ownership: full display raster lends pixels without another allocation or decode");
    uint32_t *original=view.surface.pixels;
    pic->need_w=100;pic->need_h=50;pictures_judge(&g.page);pictures_feed(&g.page);
    check(sizes_run() && pic->image.width==100 && pic->full->image.pixels==original &&
          original[101]==0xff650040,"image ownership: a live loan and immutable source survive display resampling");
    canvas_source_release(g.page.way.doc,&view);
    pic->need_w=400;pic->need_h=200;pictures_judge(&g.page);pictures_feed(&g.page);
    check(!sizes_run() && pic->image.pixels==original && pic->cost==320000,
          "image ownership: growth reuses the retained full source without a duplicate grid");
    sizes_close();check(live==baseline,"image ownership: shared full source released once");
}

static void image_ownership_failure(void)
{
    for(unsigned cut=1;cut<=3;cut++) {
        size_t baseline=live;
        sizes_open("<!doctype html><img id=i src='data:image/png,SIZED%20400x200' width=100 height=50>","RASTER");
        pictures_start(&g.page);responsive_drain();
        Picture *pic=&g.page.pics[0];size_t kept=g.page.kept_bytes;
        fail_at=attempts+cut;os64_dom_image_view_t view;
        os64_dom_image_status_t status=canvas_source(g.page.way.doc,probe_id("i"),true,&view);fail_at=0;
        check(status==OS64_DOM_IMAGE_REFUSED && !pic->encoded && !pic->full && g.page.kept_bytes==kept,
              "image ownership: loan, owner and decode allocation failures preserve backing and metadata");
        canvas_source_release(g.page.way.doc,&view);
        check(canvas_source(g.page.way.doc,probe_id("i"),true,&view)==OS64_DOM_IMAGE_READY,
              "image ownership: a later draw can retry a refused promotion synchronously");
        canvas_source_release(g.page.way.doc,&view);sizes_close();
        check(live==baseline,"image ownership: promotion allocation cut leaks nothing");
    }
}

static void image_ownership_gif(bool http,bool crowded)
{
    size_t baseline=live;
    // Two 1x1 frames: red, then green, with independent LZW streams.
    const uint8_t gif[]={
        'G','I','F','8','9','a',1,0,1,0,0x80,0,0,255,0,0,0,255,0,
        0x2c,0,0,0,0,1,0,1,0,0,2,2,0x44,1,0,
        0x2c,0,0,0,0,1,0,1,0,0,2,2,0x4c,1,0,0x3b};
    char *base=data_b64((const char *)gif,sizeof(gif));char html[512];
    snprintf(html,sizeof(html),"<!doctype html><img id=i src='data:image/gif;base64,%s'>",base);
    os64_free(base);
    if(http) {
        fake_fetch_reset();fake_answer_t *answer=fake_answer("GET","https://fixture.test/moving.gif",200,"Content-Type: image/gif\r\n",(const char *)gif);
        answer->body_len=sizeof(gif);
        probe_page("<!doctype html><img id=i src='moving.gif'>",false);data_pool_open();
    } else sizes_open(html,"RASTER");
    pictures_start(&g.page);
    if(crowded)g.page.kept_bytes=PICTURES_KEPT_MAX-sizeof(gif)-4;
    responsive_drain();
    Picture *pic=&g.page.pics[0];
    check(pic->kind==YONDER_PICTURE_SEQUENCE && !pic->full &&
          (crowded?(pic->state==PIC_NOT_KEPT && pic->encoded && !pic->moving):!pic->encoded),
          "image ownership: admitted GIF uses sequence input; refused GIF can retain encoded input alone");
    if(crowded) {
        os64_dom_image_view_t view;
        check(canvas_source(g.page.way.doc,probe_id("i"),true,&view)==OS64_DOM_IMAGE_READY &&
              ((uint32_t *)view.surface.pixels)[0]==0xffff0000,
              "image ownership: a budget-refused animation still supplies its retained default frame");
        canvas_source_release(g.page.way.doc,&view);g.page.kept_bytes=pic->cost;
    }
    if(pic->moving) {
        check(os64_image_sequence_next(pic->moving)==OS64_IMAGE_OK &&
              os64_image_sequence_frame(pic->moving)->pixels[0]==0xff00ff00,
              "image ownership: GIF display has advanced to its green second frame");
        os64_dom_image_view_t view;
        check(canvas_source(g.page.way.doc,probe_id("i"),true,&view)==OS64_DOM_IMAGE_READY &&
              ((uint32_t *)view.surface.pixels)[0]==0xffff0000 &&
              os64_image_sequence_frame(pic->moving)->pixels[0]==0xff00ff00,
              "image ownership: Canvas synchronously reads the default red GIF frame without rewinding display");
        canvas_source_release(g.page.way.doc,&view);
    }
    if(http){data_pool_close();probe_drop();fake_fetch_reset();}else sizes_close();
    check(live==baseline,"image ownership: GIF sequence and default-frame promotion release");
}

static void image_ownership_taint(void)
{
    size_t baseline=live,len;char *png=memory_fixture_read("image-responsive-1x.png",&len);
    if(!png)return;
    fake_fetch_reset();
    fake_answer_t *answer=fake_answer("GET","https://b.test/p.png",200,
        "Content-Type: image/png\r\nAccess-Control-Allow-Origin: https://fixture.test\r\n"
        "Access-Control-Allow-Credentials: true\r\n",png);
    answer->body_len=len;
    fake_answer("GET","https://fixture.test/start",302,"Location: https://b.test/hop\r\n",NULL);
    fake_answer("GET","https://b.test/hop",302,"Location: https://fixture.test/end\r\n",NULL);
    answer=fake_answer("GET","https://fixture.test/end",200,"Content-Type: image/png\r\n",png);
    answer->body_len=len;
    probe_page("<!doctype html><img id=a src='https://b.test/p.png'>"
        "<img id=b crossorigin=anonymous src='https://b.test/p.png'>"
        "<img id=c crossorigin=use-credentials src='https://b.test/p.png'>"
        "<img id=d src='https://fixture.test/start'>",false);
    data_pool_open();responsive_serial_pool();pictures_start(&g.page);responsive_drain();
    const char *ids[]={"a","b","c","d"};
    for(unsigned k=0;k<4;k++) {
        os64_dom_image_view_t view;
        check(canvas_source(g.page.way.doc,probe_id(ids[k]),true,&view)==OS64_DOM_IMAGE_READY &&
              view.origin_clean==(k==1 || k==2),
              "image ownership: shared full display pixels preserve request-mode and redirect-chain taint");
        canvas_source_release(g.page.way.doc,&view);
    }
    check(g.page.npics==4 && nfake_requests==6 && g.page.image_promotions==0,
          "image ownership: metadata and pixel reads never create native fetch entries");
    data_pool_close();probe_drop();fake_fetch_reset();free(png);
    check(live==baseline,"image ownership: tainted and CORS-clean owners release once");
}

static void image_ownership_svg(void)
{
    size_t baseline=live;
    sizes_open("<!doctype html><img id=i src='data:image/svg+xml,%3Csvg%20xmlns=%22http://www.w3.org/2000/svg%22%20width=%222%22%20height=%222%22%3E%3Crect%20width=%222%22%20height=%222%22%20fill=%22red%22/%3E%3C/svg%3E'>","RASTER");
    pictures_start(&g.page);responsive_drain();
    os64_dom_image_view_t view;
    check(canvas_source(g.page.way.doc,probe_id("i"),true,&view)==OS64_DOM_IMAGE_READY &&
          ((uint32_t *)view.surface.pixels)[0]==0xffff0000,
          "image ownership: shared SVG document supplies a secure-static Canvas render");
    sizes_close();
    check(view.surface.pixels && ((uint32_t *)view.surface.pixels)[0]==0xffff0000 &&
          view.current_src && !strncmp(view.current_src,"data:image/svg+xml,",19),
          "image ownership: rendered SVG loan survives its document and catalog");
    canvas_source_release(NULL,&view);
    check(g.page.svg_bytes==0 && live==baseline,"image ownership: retired SVG loan cannot debit the next page's ledger");
}

static void image_ownership_late_layout(bool crowded)
{
    size_t baseline=live,len=0;char *png=memory_fixture_read("image-memory.png",&len);
    if(!png)return;
    fake_reset();fake_fetch_reset();
    fake_answer_t *answer=fake_answer("GET","https://fixture.test/late.png",200,
        "Content-Type: image/png\r\nCache-Control: max-age=600\r\n",png);
    answer->body_len=len;
    probe_page("<!doctype html><style>body{margin:0}img{display:block;width:160px}</style>"
        "<img id=i src='late.png'>",false);
    way_cache_t *cache=way_cache_open("/tmp/late-image-cache",16u<<20);g.cache=cache;
    data_pool_open();pictures_start(&g.page);
    if(crowded)g.page.kept_bytes=PICTURES_KEPT_MAX-1000;
    check(sizes_run(),"image review: image arrives before intrinsic-dependent layout");
    Picture *pic=&g.page.pics[0];
    if(crowded) {
        check(pic->state==PIC_NOT_KEPT && !pic->encoded && !pic->full,
              "image review: competing owners refuse both full raster and encoded body");
        g.page.kept_bytes=PICTURES_KEPT_MAX-60000;
    } else {
        check(pic->full && pic->encoded && pic->image.width==1920,
              "image review: early whole arrival retains bytes until layout decides");
        way_cache_key_t key={"https fixture.test",true,"https://fixture.test/late.png"};
        way_cache_drop(cache,&key);way_cache_enable(cache,false);
        answer->body="changed response";answer->body_len=strlen(answer->body);
    }
    sizes_layout(1000);
    const yonder_picture_job_t *job=sizes_held();
    check(job && job->resample && (crowded?!job->source:job->source==pic->encoded),
          "image review: resample uses retained bytes or falls through to the request cache");
    check(sizes_run() && pic->state==PIC_SHOWN && pic->image.width==160 && !pic->full,
          "image review: late-layout thumbnail releases the unborrowed full raster");
    check(nfake_requests==1 && pic->cost==57600+(crowded?0:len) &&
          (crowded?!pic->encoded:pic->encoded!=NULL),
          "image review: thumbnail fits without unavailable reconstruction and does not refetch the network");
    os64_dom_image_view_t view;
    check(canvas_source(g.page.way.doc,probe_id("i"),true,&view)==
          (crowded?OS64_DOM_IMAGE_REFUSED:OS64_DOM_IMAGE_READY),
          "image review: tight-budget pixel refusal or synchronous original-response promotion");
    if(!crowded)check(((uint32_t *)view.surface.pixels)[0]==0xff2060c0,
          "image review: cache eviction and changed server cannot alter retained-response pixels");
    canvas_source_release(g.page.way.doc,&view);
    g.page.kept_bytes=pic->cost;
    data_pool_close();g.cache=NULL;way_cache_close(cache);probe_drop();fake_fetch_reset();fake_reset();free(png);
    check(live==baseline,"image review: late layout and refused-source fallback release their owners");
}

static void image_ownership_cases(void)
{
    image_ownership_late_layout(false);image_ownership_late_layout(true);
    image_ownership_http(false,false);image_ownership_http(true,false);
    image_ownership_http(true,true);image_ownership_shared();image_ownership_failure();image_ownership_gif(false,false);image_ownership_gif(true,false);image_ownership_gif(true,true);image_ownership_taint();image_ownership_svg();
}
