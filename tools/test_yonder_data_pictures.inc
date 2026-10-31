/* data: pictures (DATA_URL_PICTURES_BACKPORT.md): the real worker decodes
 * the address with fetch_job's data: processor, and the page's catalog,
 * feed, arrival and painter take one from markup or a sheet. The network,
 * the disk and the raster codecs are the fixture's (picture_http_bytes,
 * os64_slurp, os64_image_decode's "RASTER"), so a count of their calls
 * proves none was asked. */

static char *data_b64(const char *bytes, size_t n)
{
    static const char a[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out=os64_malloc((n+2)/3*4+1);
    if(!out) return NULL;
    size_t o=0;
    for(size_t i=0;i<n;i+=3) {
        uint32_t v=(uint32_t)(uint8_t)bytes[i]<<16;
        if(i+1<n) v|=(uint32_t)(uint8_t)bytes[i+1]<<8;
        if(i+2<n) v|=(uint8_t)bytes[i+2];
        out[o++]=a[v>>18&63]; out[o++]=a[v>>12&63];
        out[o++]=i+1<n?a[v>>6&63]:'='; out[o++]=i+2<n?a[v&63]:'=';
    }
    out[o]=0;
    return out;
}

/* An SVG 64 by 32 whose comment pads it to at least `pad` bytes, as a
 * base64 data: address (os64_malloc'd). */
static char *data_svg_url(size_t pad)
{
    char *svg=os64_malloc(pad+256);
    if(!svg) return NULL;
    int n=snprintf(svg,pad+256,"<svg xmlns='http://www.w3.org/2000/svg' width='64' height='32'>"
        "<rect width='64' height='32' fill='blue'/><!--");
    while((size_t)n<pad) svg[n++]='x';
    n+=snprintf(svg+n,pad+256-(size_t)n,"--></svg>");
    char *b=data_b64(svg,(size_t)n);
    os64_free(svg);
    if(!b) return NULL;
    size_t len=strlen(b)+32;
    char *url=os64_malloc(len);
    if(url) snprintf(url,len,"data:image/svg+xml;base64,%s",b);
    os64_free(b);
    return url;
}

static bool data_has(const char *text, const char *needle) { return strstr(text,needle)!=NULL; }

#define DATA_RASTER "data:image/png;base64,UkFTVEVS"      /* "RASTER" */
#define DATA_SVG "data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='64' height='32'%3E" \
    "%3Crect width='64' height='32' fill='blue'/%3E%3C/svg%3E"

static yonder_picture_job_t *data_job(const char *url, bool svg)
{
    yonder_picture_job_t *job=os64_calloc(1,sizeof(*job));
    check(job!=NULL,"data: job allocated");
    job->kind=YONDER_JOB_PICTURE; job->svg_enabled=svg;
    job->generation=g.page_serial; job->index=0;
    job->data_url=os64_malloc(strlen(url)+1);
    check(job->data_url!=NULL,"data: job address allocated");
    memcpy(job->data_url,url,strlen(url)+1);
    return job;
}

/* Runs a job and hands back its product; no fetch and no read either way. */
static yonder_picture_t *data_run(yonder_picture_job_t *job, int64_t *verdict)
{
    int fetches=picture_fetches; void *out=NULL;
    const char *slurp_was=slurp_path; slurp_path="/never-read";
    *verdict=yonder_picture_run(job,picture_cancel,NULL,&out);
    slurp_path=slurp_was;
    check(picture_fetches==fetches,"data: a data: picture asks the network nothing");
    return out;
}

static void data_worker_cases(void)
{
    size_t baseline=live; int64_t v;
    picture_polls=picture_cancel_at=0;
    picture_http_bytes="RASTER"; picture_http_type="image/png"; picture_http_status=200; picture_http_failure=false;

    yonder_picture_job_t *job=data_job(DATA_RASTER,false);
    yonder_picture_t *p=data_run(job,&v);
    check(v==1 && p && p->kind==YONDER_PICTURE_RASTER && p->image.width==1 && p->image.height==1 &&
          p->image.pixels && p->image.pixels[0]==0xff123456,
          "data: a base64 raster decodes to its pixels with Modern Web off");
    yonder_picture_release(job,p);

    job=data_job("DaTa:image/png;BASE64,UkFT VEVS",false);
    p=data_run(job,&v);
    check(v==1 && p && p->image.width==1,"data: the scheme and ;base64 in any case, base64 forgiving of spaces");
    yonder_picture_release(job,p);

    job=data_job(DATA_SVG,true);
    p=data_run(job,&v);
    check(v==1 && p && p->kind==YONDER_PICTURE_SVG && p->svg && p->width==64 && p->height==32,
          "data: a percent-encoded SVG is a document at its own size");
    yonder_picture_release(job,p);

    job=data_job(DATA_SVG,false);
    int raster=picture_raster_decodes;
    p=data_run(job,&v);
    check(v==0 && p && !p->svg && picture_raster_decodes==raster+1,
          "data: with Modern Web off an SVG is no picture, as one fetched is not");
    yonder_picture_release(job,p);

    char *remote=os64_malloc(1024);
    snprintf(remote,1024,"data:image/svg+xml,%%3Csvg xmlns='http://www.w3.org/2000/svg' "
        "xmlns:x='http://www.w3.org/1999/xlink' width='8' height='8'%%3E"
        "%%3Cimage href='https://far.test/a.png' width='8' height='8'/%%3E"
        "%%3Cimage x:href='file:///etc/passwd' width='8' height='8'/%%3E%%3C/svg%%3E");
    job=data_job(remote,true);
    p=data_run(job,&v);
    check(p!=NULL,"data: an SVG naming other resources settles, nothing fetched (data_run)");
    yonder_picture_release(job,p); os64_free(remote);

    char *longer=data_svg_url(OS64_FETCH_URL_MAX);
    check(longer && strlen(longer)>OS64_FETCH_URL_MAX,"data: the long address is past a job's fixed one");
    job=data_job(longer,true);
    p=data_run(job,&v);
    check(v==1 && p && p->svg && p->width==64 && p->height==32,
          "data: an address past OS64_FETCH_URL_MAX decodes whole");
    yonder_picture_release(job,p);

    static const struct { const char *url; os64_image_status_t st; const char *why; } bad[]={
        {"data:image/png;base64,@@@@",OS64_IMAGE_MALFORMED,"its data: address is malformed, too big, or no memory"},
        {"data:image/png;base64,A",OS64_IMAGE_MALFORMED,"its data: address is malformed, too big, or no memory"},
        {"data:image/png;base64",OS64_IMAGE_MALFORMED,"its data: address is malformed, too big, or no memory"},
        {"data:image/png;base64,",OS64_IMAGE_NO_FILE,"its data: address is empty"},
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        job=data_job(bad[i].url,true);
        p=data_run(job,&v);
        check(v==0 && p && p->status==bad[i].st && os64_streq(p->why,bad[i].why) && !p->svg && !p->image.pixels,
              "data: a malformed or empty address is a failed picture, named so");
        yonder_picture_release(job,p);
    }
    job=data_job("data:image/png;base64,Tk9UQVBJQ1RVUkU=",true);     /* "NOTAPICTURE" */
    p=data_run(job,&v);
    check(v==0 && p && p->status==OS64_IMAGE_UNKNOWN_FORMAT,"data: bytes no decoder takes are not a picture");
    yonder_picture_release(job,p);

    /* Past libimage's cap: refused by the data: processor, decoder unasked. */
    size_t big=(size_t)OS64_IMAGE_CAP_DEFAULT+16;
    char *huge=os64_malloc(big+8);
    check(huge!=NULL,"data: a huge address is built");
    memcpy(huge,"data:,",6); memset(huge+6,'A',big); huge[6+big]=0;
    job=data_job(huge,true); os64_free(huge);
    raster=picture_raster_decodes;
    p=data_run(job,&v);
    check(v==0 && p && p->status==OS64_IMAGE_MALFORMED && picture_raster_decodes==raster,
          "data: a body past the picture cap is refused before any decoder");
    yonder_picture_release(job,p);

    /* Every allocation the worker makes, cut in turn, and every poll of
     * the cancel, each through the real release: the long address for the
     * allocations, the short one for the polls (a parse polls per byte). */
    job=data_job(longer,true);
    size_t before=attempts;
    p=data_run(job,&v);
    size_t allocations=attempts-before;
    yonder_picture_release(job,p);
    picture_polls=0;
    job=data_job(DATA_SVG,true);
    p=data_run(job,&v);
    size_t polls=picture_polls;
    yonder_picture_release(job,p);
    for(size_t i=1;i<=allocations;i++) {
        job=data_job(longer,true); void *out=NULL;
        fail_at=attempts+i;
        int64_t r=yonder_picture_run(job,picture_cancel,NULL,&out);
        fail_at=0;
        check(r!=1 || (out && ((yonder_picture_t *)out)->svg),"data: a cut allocation is no half picture");
        yonder_picture_release(job,out);
        check(live==baseline+1,"data: a cut allocation releases everything through the worker's release");
    }
    for(size_t i=1;i<=polls;i++) {
        job=data_job(DATA_SVG,true); void *out=NULL; picture_polls=0; picture_cancel_at=i;
        check(yonder_picture_run(job,picture_cancel,NULL,&out)!=1,"data: a cancelled picture is refused");
        yonder_picture_release(job,out);
        check(live==baseline+1,"data: a cancelled picture releases its address and document");
    }
    picture_cancel_at=0;
    os64_free(longer);
    picture_http_bytes=NULL; picture_http_type=NULL;
    check(live==baseline,"data: the worker cases leave nothing behind");
    printf("data: picture worker: %zu allocation and %zu cancellation positions pass\n",allocations,polls);
}

/* The page side, against the fixture pool's one slot. */
static void data_pool_open(void)
{
    pool_open=true; g.pool=(os64_work_pool_t *)&pool_fake; memset(&pool_work,0,sizeof(pool_work));
}
static void data_pool_close(void)
{
    if(pool_work.job!=NULL) { pool_work.release(pool_work.job,NULL); memset(&pool_work,0,sizeof(pool_work)); }
    pool_open=false; g.pool=NULL;
}
/* The job in the pool, run and arrived as reaped() would: true when it
 * made a picture. */
static bool data_run_held(void)
{
    yonder_picture_job_t *job=pool_work.job;
    if(job==NULL || job->kind!=YONDER_JOB_PICTURE) return false;
    memset(&pool_work,0,sizeof(pool_work));
    int64_t v; yonder_picture_t *p=data_run(job,&v);
    picture_arrived(job,p);
    yonder_picture_release(job,p);
    return v==1;
}

static void data_page_cases(void)
{
    int fetches=picture_fetches;
    /* Markup, Modern Web off: two spellings of one address are one picture,
     * the job carries its own copy, and it arrives as pixels. */
    probe_modern_off=true;
    probe_page("<img src='DaTa:image/png;base64,UkFTVEVS'><img src=' " DATA_RASTER "'>",false);
    probe_modern_off=false;
    data_pool_open();
    pictures_start(&g.page);
    const yonder_picture_job_t *job=pool_work.job;
    check(g.page.npics==1 && g.page.pic_of[0]==0 && g.page.pic_of[1]==0 &&
          os64_streq(g.page.pics[0].url,DATA_RASTER) && g.page.pics[0].url_len==strlen(DATA_RASTER),
          "data: a markup data: picture is one catalog entry however its scheme is written");
    check(job && job->kind==YONDER_JOB_PICTURE && job->data_url && job->data_url!=g.page.pics[0].url &&
          os64_streq(job->data_url,DATA_RASTER) && job->url[0]==0 && !job->svg_enabled,
          "data: the job carries its own copy of the address");
    check(data_run_held() && g.page.pics[0].state==PIC_SHOWN && g.page.pics[0].image.width==1 &&
          g.page.waiting==0 && g.page.in_flight==0 && picture_fetches==fetches,
          "data: a raster data: picture arrives and settles the page, nothing fetched");
    /* An address that begins another is still its own picture — asked
     * where the index probes the longer one first, so only the lengths
     * can tell them apart. */
    char longer_key[64]; const char *prefix="data:,shared";
    size_t cap=g.page.pic_slot_cap;
    for(int i=0;;i++) {
        snprintf(longer_key,sizeof(longer_key),"%s%d",prefix,i);
        if(((url_hash(longer_key,strlen(longer_key))^url_hash(prefix,strlen(prefix)))&(cap-1))==0) break;
    }
    check(picture_for(&g.page,longer_key)==1 && g.page.pic_slot_cap==cap &&
          picture_find(&g.page,prefix)==-1 && picture_find(&g.page,DATA_RASTER)==0,
          "data: a key is the whole address, not a prefix of one");
    data_pool_close(); probe_drop();

    /* A sheet and the markup name one SVG; the painter finds the sheet's. */
    const char *html="<!doctype html><style>body{margin:0;background:white}"
        "div{width:64px;height:32px;background-repeat:no-repeat;"
        "background-image:url(\"DATA:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='64' height='32'%3E"
        "%3Crect width='64' height='32' fill='blue'/%3E%3C/svg%3E#frag\")}"
        "img{display:block}</style><div></div><img src=\"" DATA_SVG "\">";
    probe_page(html,false);
    data_pool_open();
    pictures_start(&g.page); css_pictures(&g.page);
    check(g.page.npics==1 && os64_streq(g.page.pics[0].url,DATA_SVG),
          "data: a sheet's data: picture (scheme folded, fragment dropped) is the markup's");
    check(data_run_held() && g.page.pics[0].kind==YONDER_PICTURE_SVG && g.page.waiting==0,
          "data: an SVG data: picture arrives as a document");
    check(page_lay_out(&g.page,800,600,g.zoom,&g.page),"data: relayout after arrival");
    uint32_t *px=stand_in_shot();
    check((px[16*800+32]&0xffffff)==0xff && (px[48*800+32]&0xffffff)==0xff,
          "data: the sheet's background and the markup's picture are painted");
    os64_free(px);
    data_pool_close(); probe_drop();

    /* A sheet's data: picture past OS64_FETCH_URL_MAX reaches the worker whole. */
    char *longer=data_svg_url(OS64_FETCH_URL_MAX);
    size_t hl=strlen(longer)+256;
    char *page=os64_malloc(hl);
    snprintf(page,hl,"<!doctype html><style>body{margin:0;background:white}div{width:64px;height:32px;"
        "background-image:url(%s)}</style><div></div>",longer);
    probe_page(page,false);
    data_pool_open();
    css_pictures(&g.page);
    job=pool_work.job;
    check(g.page.npics==1 && g.page.pics[0].url_len==strlen(longer) && job && job->data_url &&
          os64_streq(job->data_url,longer),"data: a long sheet data: address is kept and handed over whole");
    check(data_run_held() && g.page.pics[0].kind==YONDER_PICTURE_SVG && g.page.pics[0].width==64,
          "data: the long one decodes to its picture");
    check(page_lay_out(&g.page,800,600,g.zoom,&g.page),"data: long relayout");
    px=stand_in_shot();
    check((px[16*800+32]&0xffffff)==0xff,"data: the long sheet picture is painted");
    os64_free(px);
    data_pool_close(); probe_drop(); os64_free(page);

    /* Malformed: a failed picture on the record, the page settled. */
    probe_page("<img src='data:image/png;base64,@@@@'>",false);
    g.page.diag=diag_new_claimed("http://d.test/",1);
    data_pool_open();
    pictures_start(&g.page);
    check(!data_run_held() && g.page.pics[0].state==PIC_FAILED && g.page.waiting==0 && g.page.in_flight==0,
          "data: a malformed data: picture fails and settles the page");
    char text[4096]; yonder_diag_render(g.page.diag,text,sizeof(text));
    check(data_has(text,"FAILED picture-fetch: data:image/png;base64,@@@@: its data: address is malformed"),
          "data: a malformed data: picture is on the record by its address");
    data_pool_close(); yonder_diag_free(g.page.diag); g.page.diag=NULL; probe_drop();

    /* Long failures keep their reason: the record names the address short. */
    char *bad=os64_malloc(strlen(longer)+8);
    snprintf(bad,strlen(longer)+8,"%s@@",longer);
    snprintf(page=os64_malloc(hl+8),hl+8,"<!doctype html><style>div{width:4px;height:4px;"
        "background-image:url(%s)}</style><div></div>",bad);
    probe_page(page,false);
    g.page.diag=diag_new_claimed("http://d.test/",2);
    data_pool_open();
    css_pictures(&g.page);
    check(!data_run_held() && g.page.waiting==0,"data: a long malformed sheet picture settles");
    yonder_diag_render(g.page.diag,text,sizeof(text));
    char want[160]; snprintf(want,sizeof(want),"bytes): its data: address is malformed, too big, or no memory");
    check(data_has(text,"FAILED picture-fetch: data:image/svg+xml;base64,") && data_has(text,want),
          "data: a long data: address is named short, its reason whole");
    data_pool_close(); yonder_diag_free(g.page.diag); g.page.diag=NULL; probe_drop();
    os64_free(page); os64_free(bad);

    /* Past the addresses a page may name: refused before the table, and on
     * the record as a failed picture, not as a scheme nothing fetches. */
    probe_page("<img src='" DATA_RASTER "'>",false);
    g.page.diag=diag_new_claimed("http://d.test/",4);
    data_pool_open();
    g.page.picture_url_bytes=4u*1024u*1024u-8;
    pictures_start(&g.page);
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(g.page.npics==0 && pool_work.job==NULL && g.page.waiting==0 && !data_has(text,"picture-scheme") &&
          data_has(text,"FAILED picture: " DATA_RASTER ": past the pictures a page may name, or no memory"),
          "data: a data: picture past the catalog's budget is a failed picture by its address");
    g.page.picture_url_bytes=0;
    data_pool_close(); yonder_diag_free(g.page.diag); g.page.diag=NULL; probe_drop();

    /* Markup past an address's length (a search page's inline thumbnails):
     * the model keeps a picture's data: address whole, and it is shown. */
    check(strlen(longer)>OS64_URL_REF_MAX,"data: the long address is past the model's address limit");
    char *tall=os64_malloc(strlen(longer)+64);
    snprintf(tall,strlen(longer)+64,"<img src='DATA%s'>",longer+4);
    probe_page(tall,false);
    g.page.diag=diag_new_claimed("http://d.test/",3);
    data_pool_open();
    pictures_start(&g.page);
    job=pool_work.job;
    check(g.page.npics==1 && os64_streq(g.page.pics[0].url,longer) && job && job->data_url &&
          os64_streq(job->data_url,longer),"data: a markup data: address past OS64_URL_REF_MAX is handed over whole");
    check(data_run_held() && g.page.pics[0].kind==YONDER_PICTURE_SVG && g.page.pics[0].width==64 &&
          g.page.waiting==0,"data: and decodes to its picture");
    yonder_diag_render(g.page.diag,text,sizeof(text));
    check(!data_has(text,"FAILED picture"),"data: nothing on the record for it");
    data_pool_close(); yonder_diag_free(g.page.diag); g.page.diag=NULL; probe_drop(); os64_free(tall);

    /* No memory for the job's copy, a pool that will not take it, the
     * page left with it out, and a page gone before it arrives: each
     * settles, and the job's address goes through its release. */
    probe_page("<img src='" DATA_RASTER "'><img src='" DATA_SVG "'>",false);
    data_pool_open();
    check(picture_for(&g.page,DATA_RASTER)==0 && picture_for(&g.page,DATA_SVG)==1,"data: two entries");
    size_t at=live;
    fail_at=attempts+2;             /* the job, then its address */
    pictures_feed(&g.page);
    fail_at=0;
    check(g.page.pics[0].state==PIC_FAILED && g.page.waiting==1 && g.page.in_flight==1 &&
          pool_work.job!=NULL && ((yonder_picture_job_t *)pool_work.job)->index==1,
          "data: no memory for its address fails the one picture and feeds the next");
    check(live==at+2,"data: the job whose address failed was released, the next one holds two blocks");
    data_pool_close(); probe_drop();

    probe_page("<img src='" DATA_RASTER "'><img src='" DATA_SVG "'>",false);
    data_pool_open();
    check(picture_for(&g.page,DATA_RASTER)==0 && picture_for(&g.page,DATA_SVG)==1,"data: two entries again");
    at=live;
    pictures_feed(&g.page);         /* the fixture pool keeps one picture */
    check(g.page.pics[1].state==PIC_FAILED && g.page.waiting==1 && pool_work.job!=NULL,
          "data: a picture the pool refuses fails");
    check(live==at+2,"data: the refused job and its address went through the release");
    check(data_run_held() && g.page.waiting==0 && g.page.in_flight==0,"data: the one taken settles the page");
    data_pool_close(); probe_drop();

    probe_page("<img src='" DATA_RASTER "'>",false);
    data_pool_open();
    check(picture_for(&g.page,DATA_RASTER)==0,"data: one entry");
    at=live;
    pictures_feed(&g.page);
    check(pool_work.job!=NULL,"data: handed over");
    pictures_leave(&g.page);
    check(pool_work.job==NULL && live==at && g.page.waiting==0,
          "data: leaving the page cancels the job, its address released with it");
    data_pool_close(); probe_drop();

    probe_page("<img src='" DATA_RASTER "'>",false);
    data_pool_open();
    check(picture_for(&g.page,DATA_RASTER)==0,"data: one entry, to be left");
    pictures_feed(&g.page);
    g.page_serial++;                /* another page is on screen when it lands */
    check(data_run_held() && g.page.pics[0].state==PIC_WAITING && g.page.pics[0].image.pixels==NULL,
          "data: a picture for a page gone is let go, not shown");
    data_pool_close(); probe_drop();

    os64_free(longer);
    check(picture_fetches==fetches,"data: no page case fetched anything");
}

static void data_picture_cases(void)
{
    data_worker_cases();
    data_page_cases();
}
