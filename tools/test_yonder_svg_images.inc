/* Real picture worker/arrival/painter with fixture transport and raster codecs. */
static const char *picture_svg_source =
    "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32' viewBox='0 0 64 32'>"
    "<style>rect{fill:blue}</style><rect width='64' height='32'/></svg>";
static size_t picture_polls, picture_cancel_at;
static bool picture_cancel(void *context)
{
    (void)context;
    return ++picture_polls == picture_cancel_at;
}
static yonder_picture_job_t *picture_svg_job(const char *url)
{
    yonder_picture_job_t *job = os64_calloc(1,sizeof(*job));
    check(job != NULL,"SVG image job allocated");
    job->kind=YONDER_JOB_PICTURE; job->svg_enabled=true;
    job->generation=g.page_serial; job->index=0;
    os64_strcopy(job->url,sizeof(job->url),url);
    return job;
}
static void picture_svg_worker_cases(void)
{
    size_t baseline=live;
    slurp_path="/svg-image.svg"; slurp_bytes=picture_svg_source; slurp_length=0;
    yonder_picture_job_t *job=picture_svg_job("file:///svg-image.svg");
    void *out=NULL;
    picture_polls=picture_cancel_at=0;
    size_t before=attempts;
    check(yonder_picture_run(job,picture_cancel,NULL,&out)==1,"SVG file worker succeeds");
    size_t allocations=attempts-before, polls=picture_polls;
    yonder_picture_t *product=out;
    check(product && product->kind==YONDER_PICTURE_SVG && product->svg &&
          !product->image.pixels && !product->sequence && product->width==64 && product->height==32 &&
          product->cost==yonder_svg_document_cost(product->svg),"SVG tagged product owns document and natural size");
    yonder_picture_release(job,product);
    check(live==baseline,"SVG worker release owns the unclaimed document");
    for(size_t i=1;i<=allocations;i++) {
        job=picture_svg_job("file:///svg-image.svg");out=NULL;fail_at=attempts+i;
        (void)yonder_picture_run(job,picture_cancel,NULL,&out);fail_at=0;
        yonder_picture_release(job,out);
        check(live==baseline,"SVG worker allocation cut releases partial ownership");
    }
    for(size_t i=1;i<=polls;i++) {
        job=picture_svg_job("file:///svg-image.svg");out=NULL;picture_polls=0;picture_cancel_at=i;
        check(yonder_picture_run(job,picture_cancel,NULL,&out)!=1,"SVG worker cancellation refuses product");
        yonder_picture_release(job,out);
        check(live==baseline,"SVG cancelled worker releases its document");
    }
    picture_cancel_at=0;
    slurp_path=NULL;slurp_bytes=NULL;
    picture_http_bytes=picture_svg_source;picture_http_type="image/svg+xml";picture_http_status=200;
    picture_http_failure=false; picture_fetches=0;
    job=picture_svg_job("https://fixture.test/logo");out=NULL;
    check(yonder_picture_run(job,picture_cancel,NULL,&out)==1 && picture_fetches==1,
          "SVG HTTP worker recognizes MIME without relying on a URL suffix");
    yonder_picture_release(job,out);
    picture_http_type="application/octet-stream";
    job=picture_svg_job("https://fixture.test/logo");out=NULL;
    check(yonder_picture_run(job,picture_cancel,NULL,&out)==1,"SVG byte candidate is confirmed by XML root identity");
    yonder_picture_release(job,out);
    picture_http_type="image/svg+xml";picture_http_status=404;
    job=picture_svg_job("https://fixture.test/logo");out=NULL;
    check(yonder_picture_run(job,picture_cancel,NULL,&out)==0 &&
          ((yonder_picture_t *)out)->svg==NULL,"HTTP error SVG body does not become a picture");
    yonder_picture_release(job,out);picture_http_status=200;
    const char *bad[]={"<html><svg/></html>","<!-- <svg -->not XML", "<svg/>",
        "<svg xmlns='http://www.w3.org/2000/svg'><rect></svg>"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        picture_http_bytes=bad[i];job=picture_svg_job("https://fixture.test/logo");out=NULL;
        check(yonder_picture_run(job,picture_cancel,NULL,&out)==0 && !((yonder_picture_t*)out)->svg,
              "SVG format detection refuses wrappers, comments, wrong namespaces and malformed XML");
        yonder_picture_release(job,out);
    }
    picture_http_bytes=picture_svg_source;job=picture_svg_job("https://fixture.test/logo");
    job->svg_enabled=false;out=NULL;int raster_before=picture_raster_decodes;
    check(yonder_picture_run(job,picture_cancel,NULL,&out)==0 && picture_raster_decodes==raster_before+1,
          "Modern Web off retains raster-only image dispatch");
    yonder_picture_release(job,out);
    picture_http_bytes="RASTER";picture_http_type="text/plain";
    job=picture_svg_job("https://fixture.test/logo");out=NULL;
    check(yonder_picture_run(job,picture_cancel,NULL,&out)==1 &&
          ((yonder_picture_t*)out)->kind==YONDER_PICTURE_RASTER,
          "ordinary pictures retain signature-based raster dispatch");
    yonder_picture_release(job,out);
    picture_http_bytes=NULL;picture_http_type=NULL;
    check(live==baseline,"SVG worker cases leave no retained allocation");
    printf("SVG picture worker: %zu allocation and %zu cancellation positions pass\n",allocations,polls);
}
static void picture_svg_arrive(const char *html, const char *xml)
{
    probe_page(html,false);
    pictures_start(&g.page);css_pictures(&g.page);
    check(g.page.npics==1,"SVG uses share a single address entry");
    g.page.pics[0].state=PIC_WAITING;g.page.waiting=g.page.in_flight=1;
    yonder_picture_job_t *job=picture_svg_job("file:///svg-image.svg");
    slurp_path="/svg-image.svg";slurp_bytes=xml;slurp_length=0;
    void *out=NULL;
    check(yonder_picture_run(job,NULL,NULL,&out)==1,"SVG gallery product parses on worker");
    picture_arrived(job,out);
    check(g.page.pics[0].svg && !((yonder_picture_t*)out)->svg &&
          g.page.kept_bytes==((yonder_picture_t*)out)->cost,"SVG arrival transfers ownership and charges retained bytes");
    yonder_picture_release(job,out);slurp_path=NULL;slurp_bytes=NULL;
    check(page_lay_out(&g.page,800,600,g.zoom,&g.page),"SVG arrival relayout succeeds");
}
static void picture_svg_window_cases(void)
{
    picture_svg_worker_cases();
    const char *html="<!doctype html><style>body{margin:0;background:white}rect{fill:red!important}"
        "img{display:block}</style><img id=one src=file:///svg-image.svg>"
        "<img src=file:///svg-image.svg width=128 height=64>";
    picture_svg_arrive(html,picture_svg_source);
    uint32_t *px=stand_in_shot();
    check((px[16*800+32]&0xffffff)==0xff && (px[64*800+100]&0xffffff)==0xff &&
          (px[16*800+80]&0xffffff)==0xffffff,"SVG images use natural/explicit sizes and isolated blue stylesheet");
    os64_free(px);
    g.zoom=4000;
    check(page_lay_out(&g.page,800,600,g.zoom,&g.page),"SVG image zoom relayout succeeds");
    px=stand_in_shot();
    check((px[64*800+250]&0xffffff)==0xff && (px[64*800+260]&0xffffff)==0xffffff,
          "SVG image rerasterizes at 400 percent");
    os64_free(px);check(g.page.svg_bytes==0,"SVG image painting returns its complete pixel/geometry ledger");
    yonder_svg_pixels_t refused;
    g.page.svg_bytes=YONDER_SVG_MEMORY_MAX;
    check(!svg_picture_pixels(&g.page.pics[0],(os64_gui_rect_t){0,0,256,128},&refused) &&
          !refused.pixels && g.page.svg_bytes==YONDER_SVG_MEMORY_MAX,
          "SVG image exhausted render ledger refuses without stealing another owner's bytes");
    g.page.svg_bytes=0;
    probe_drop();
    picture_svg_arrive("<!doctype html><style>body{margin:0;background:white}"
        "img{display:block;width:128px;height:128px;object-fit:contain}</style>"
        "<img src=file:///svg-image.svg>",picture_svg_source);
    px=stand_in_shot();
    check((px[16*800+64]&0xffffff)==0xffffff && (px[64*800+64]&0xffffff)==0xff &&
          (px[112*800+64]&0xffffff)==0xffffff,"SVG object-fit contain centers its viewport");
    os64_free(px);probe_drop();
    picture_svg_arrive("<!doctype html><style>body{margin:0;background:white}"
        "div{width:128px;height:64px;background-image:url(file:///svg-image.svg);"
        "background-size:64px 32px;background-repeat:no-repeat}</style><div></div>",picture_svg_source);
    px=stand_in_shot();
    check((px[16*800+32]&0xffffff)==0xff && (px[48*800+96]&0xffffff)==0xffffff,
          "SVG background uses the resolved tile viewport");
    os64_free(px);probe_drop();
    // A generation mismatch leaves ownership with the completed worker.
    picture_svg_arrive(html,picture_svg_source);
    yonder_picture_job_t *job=picture_svg_job("file:///svg-image.svg");
    yonder_picture_t *product=os64_calloc(1,sizeof(*product));
    check(yonder_svg_document_parse(picture_svg_source,strlen(picture_svg_source),NULL,NULL,
                                     &product->svg,NULL)==SVG_OK,"SVG stale product parses");
    product->kind=YONDER_PICTURE_SVG;product->status=OS64_IMAGE_OK;job->generation--;
    yonder_svg_document_t *kept=g.page.pics[0].svg;
    picture_arrived(job,product);
    check(product->svg && g.page.pics[0].svg==kept,"departed SVG worker cannot replace the active page's picture");
    yonder_picture_release(job,product);probe_drop();
}
