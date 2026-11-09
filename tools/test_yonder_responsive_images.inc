#include <float.h>

static void source_choice(const char *set,const char *src,double size,double dpr,
    const char *url,double density)
{
    os64_page_source_t out;
    os64_page_source_status_t status=os64_page_source_select(set,src,size,dpr,&out);
    check(status==OS64_PAGE_SOURCE_OK && out.length==strlen(url) &&
        !memcmp(out.url,url,out.length) && out.density==density,"responsive: parsed source choice and density");
}

static void responsive_parser_cases(void)
{
    source_choice("a 1x, b 2x","fallback",400,1,"a",1);
    source_choice("a 1x, b 2x","fallback",400,1.25,"b",2);
    source_choice("a 1x, b 2x","fallback",400,4,"b",2);
    source_choice("b 2x","fallback",400,1,"fallback",1);
    source_choice("a 400w, b 800w","fallback",400,2,"b",2);
    source_choice("a 400w, b 800w","fallback",800,1,"b",1);
    source_choice("a 1x, duplicate 1x, b 2x",NULL,400,1,"a",1);
    source_choice("a 1x 2x, b -1x, c 0w, d 10h, good 2x",NULL,400,1,"good",2);
    source_choice("data:image/png,a,b 1x, c 2x",NULL,400,1,"data:image/png,a,b",1);
    source_choice("a,b",NULL,400,1,"a,b",1);
    source_choice("a,,, b 2x",NULL,400,1,"a",1);
    source_choice("a 400w 200h",NULL,200,1,"a",2);
    source_choice("a .5x, b 1e0x",NULL,200,0.75,"b",1);
    source_choice("bad calc(1, 2)x, good 1x",NULL,200,1,"good",1);
    source_choice("bad 1.x, bad +1x, good 0x",NULL,200,1,"good",0);
    source_choice("bad -1x"," fallback ",400,1,"fallback",1);
    source_choice("huge 9e99999999999999999999999x",NULL,400,1,"huge",DBL_MAX);
    os64_page_source_t out;
    check(os64_page_source_select("a 1w 1x",NULL,400,1,&out)==OS64_PAGE_SOURCE_EMPTY,
          "responsive: conflicting descriptor rejects its candidate");
    char many[1600];many[0]=0;for(int i=0;i<129;i++)strcat(many,"a 1x, ");
    check(os64_page_source_select(many,NULL,400,1,&out)==OS64_PAGE_SOURCE_LIMIT && !out.url,
          "responsive: candidate limit refuses the complete set");
    char descriptor[160];memset(descriptor,'1',sizeof(descriptor));
    descriptor[0]='a';descriptor[1]=' ';descriptor[158]='x';descriptor[159]=0;
    check(os64_page_source_select(descriptor,NULL,400,1,&out)==OS64_PAGE_SOURCE_LIMIT,
          "responsive: overlong descriptor is an explicit resource refusal");
    garb_env_t env={.width=800,.height=600,.resolution=2};
    check(garb_image_source_size("auto, 400px",env)==400,"responsive sizes: ineligible auto falls through");
    check(garb_image_source_size("auto",env)==800,"responsive sizes: exhausted list uses viewport");
    check(garb_image_source_size("(max-width: 700px) 100vw, 50vw",env)==400,
          "responsive sizes: media conditions select a CSS viewport length");
    check(garb_image_source_size("calc(100vw - 40px)",env)==760,"responsive sizes: CSS calc lengths");
    check(garb_image_source_size("min(500px, 60vw)",env)==480,"responsive sizes: CSS min lengths");
    check(garb_image_source_size("clamp(100px, 50vw, 300px)",env)==300,"responsive sizes: CSS clamp lengths");
    check(garb_image_source_size("50%, -2px, var(--size), 10em",env)==160,
          "responsive sizes: unsupported or invalid entries fall through to initial-font lengths");
}

static void responsive_drain(void)
{
    for(int i=0;i<20;i++) {
        pictures_feed(&g.page);
        if(!pool_work.job)break;
        check(sizes_run(),"responsive: selected picture worker completes");
    }
}

static void responsive_serial_pool(void)
{
    /* This fixture pool has one picture slot. Reserve the other admission
     * slots so simultaneous display/native requests queue rather than hitting
     * its deliberate submission-refusal door. The real pool has all slots. */
    g.page.in_flight=PICTURES_AT_ONCE-1;
}

static void responsive_window_cases(void)
{
    sizes_open("<!doctype html><picture><source type='image/avif' srcset='bad.avif'>"
        "<source id=s media='(min-width: 600px)' type='image/png' srcset='data:image/png,SIZED%20400x200 2x'>"
        "<img id=pic src='data:image/png,SIZED%2080x40'><source srcset='after.png'></picture>","RASTER");
    responsive_serial_pool();pictures_start(&g.page);responsive_drain();
    const os64_html_node_t *node=probe_id("pic");
    ImageSource *source=image_source(&g.page,node);
    check(source && strstr(source->url,"400x200") && source->density==2,
          "responsive picture: preceding supported matching source wins");
    int32_t w=0,h=0;
    check(replaced_size(&g.page,node,&w,&h) && w==200 && h==100,
          "responsive layout: 2x natural size is density corrected");
    os64_dom_image_view_t view;
    check(canvas_source(g.page.way.doc,node,false,&view)==OS64_DOM_IMAGE_READY,
          "responsive: metadata shares the completed display selection");
    responsive_drain();
    check(canvas_source(g.page.way.doc,node,true,&view)==OS64_DOM_IMAGE_READY &&
          view.surface.width==400 && view.density==2 && strstr(view.current_src,"400x200"),
          "responsive Canvas: selected raster and density are distinct");
    canvas_source_release(g.page.way.doc,&view);
    uint64_t before=source->generation;
    image_sources_environment(&g.page,500,600,1000);responsive_drain();
    check(source->generation>before && strstr(source->url,"80x40") && source->density==1,
          "responsive picture: resize changes the matching source");
    check(canvas_source(g.page.way.doc,node,false,&view)==OS64_DOM_IMAGE_READY && view.surface.width==80,
          "responsive resize: display and native selection converge");
    canvas_source_release(g.page.way.doc,&view);
    sizes_close();

    sizes_open("<!doctype html><base href='https://a.test/images/'><img id=pic srcset='small.png 400w, large.png 800w' sizes='auto, 400px'>",
        "SIZED 400x200");
    responsive_serial_pool();pictures_start(&g.page);source=image_source(&g.page,probe_id("pic"));
    check(os64_page_nimages(page_model(&g.page))==1 && source &&
          !strcmp(source->url,"https://a.test/images/small.png") && source->density==1,
          "responsive: srcset-only images are cataloged and resolve against base");
    responsive_drain();
    image_sources_environment(&g.page,800,600,2000);
    check(!strcmp(source->url,"https://a.test/images/large.png") && source->density==2,
          "responsive: effective DPR chooses 2x without multiplying viewport zoom twice");
    responsive_drain();sizes_close();

    sizes_open("<!doctype html><img id=pic srcset='data:image/png,SIZED%20256x256 10000x'>","RASTER");
    responsive_serial_pool();pictures_start(&g.page);responsive_drain();
    check(replaced_size(&g.page,probe_id("pic"),&w,&h) && w==0 && h==0,
          "responsive: loaded subpixel natural size does not become an unknown placeholder");
    sizes_close();
}

static void responsive_set(const os64_html_node_t *node,const char *name,const char *value)
{
    check(os64_html_set_attr(g.page.way.doc,(os64_html_node_t *)node,name,value,strlen(value))==OS64_HTML_OK,
          "responsive: attribute mutation accepted");
}

static void responsive_mutation_cases(void)
{
    sizes_open("<!doctype html><img id=pic src='data:image/png,SIZED%2080x40'>","RASTER");
    responsive_serial_pool();pictures_start(&g.page);responsive_drain();
    const os64_html_node_t *node=probe_id("pic");os64_dom_image_view_t view;
    canvas_source(g.page.way.doc,node,false,&view);responsive_drain();
    int32_t count=g.page.npics;
    responsive_set(node,"srcset","data:image/png,SIZED%20120x60 1x");
    check(canvas_source(g.page.way.doc,node,false,&view)==OS64_DOM_IMAGE_READY && view.pending && view.surface.width==80,
          "responsive mutation: old complete image remains available while replacement waits");
    responsive_set(node,"srcset","data:image/png,SIZED%20160x80 1x");
    canvas_source(g.page.way.doc,node,false,&view);
    check(g.page.npics==count && !pool_work.job,"responsive mutation: related changes coalesce before submission");
    pictures_feed(&g.page);
    check(g.page.npics==count+1 && picture_find(&g.page,"data:image/png,SIZED%20120x60")<0,
          "responsive mutation: skipped intermediate source never enters fetch catalog");
    responsive_set(node,"srcset","data:image/png,SIZED%20200x100 1x");
    responsive_drain();
    check(canvas_source(g.page.way.doc,node,false,&view)==OS64_DOM_IMAGE_READY && !view.pending &&
          view.surface.width==200 && strstr(view.current_src,"200x100"),
          "responsive mutation: obsolete completions cannot replace newer selection");
    responsive_set(node,"srcset","data:image/png,broken 1x, data:image/png,SIZED%20240x120 2x");
    pictures_feed(&g.page);
    for(int i=0;i<10 && pool_work.job;i++)(void)sizes_run();
    check(canvas_source(g.page.way.doc,node,false,&view)==OS64_DOM_IMAGE_BROKEN &&
          picture_find(&g.page,"data:image/png,SIZED%20240x120")<0,
          "responsive failure: broken selected candidate does not download alternatives");
    responsive_set(node,"src","");responsive_set(node,"srcset","bad -1x");
    check(canvas_source(g.page.way.doc,node,false,&view)==OS64_DOM_IMAGE_BROKEN && !view.pending,
          "responsive: invalid source set without fallback settles as broken");
    responsive_set(node,"srcset","");
    canvas_source(g.page.way.doc,node,false,&view);
    check(view.empty && !view.pending,"responsive: clearing sources completes without an image");
    sizes_close();
}

static void responsive_script_cases(void)
{
    probe_page("<!doctype html><img id=pic srcset='data:image/png,SIZED%20400x200 2x'>"
        "<canvas id=c width=220 height=110></canvas><p id=out>waiting</p>"
        "<script>var loads=0,i=document.getElementById('pic'),c=document.getElementById('c').getContext('2d');"
        "i.onload=function(){if(++loads>1){document.getElementById('out').textContent='duplicate';return;}"
        "c.imageSmoothingEnabled=false;c.drawImage(i,0,0);"
        "if(i.clientWidth!==200 || i.clientHeight!==100){document.getElementById('out').textContent='stale geometry';return;}"
        "var edge=c.getImageData(200,0,1,1).data[3];"
        "c.clearRect(0,0,220,110);c.drawImage(i,50,20,1,1,0,0,1,1);"
        "var p=c.getImageData(0,0,1,1).data;"
        "document.getElementById('out').textContent=i.naturalWidth+','+i.naturalHeight+','+edge+','+p[0]+','+p[1]+','+(i.currentSrc.indexOf('400x200')>=0);};"
        "</script>",true);
    data_pool_open();responsive_serial_pool();pictures_start(&g.page);script_turn();
    for(int t=0;t<8;t++){responsive_drain();script_turn();}
    /* Nearest-neighbor samples the center of the 2x2 source crop. */
    check(probe_text_is("out","200,100,0,101,41,true"),
          "responsive Canvas: natural size, default destination and cropped source use CSS dimensions");
    uint64_t generation=image_source(&g.page,probe_id("pic"))->generation;
    responsive_set(probe_id("pic"),"sizes","200px");
    script_turn();responsive_drain();script_turn();
    check(image_source(&g.page,probe_id("pic"))->generation==generation &&
          probe_text_is("out","200,100,0,101,41,true"),
          "responsive: irrelevant sizes change keeps the completed density selection");
    data_pool_close();probe_drop();

    probe_page("<!doctype html><p id=out>waiting</p><script>"
        "var i=new Image(),s=document.createElement('source');s.srcset='a 1x';s.sizes='20px';s.media='all';s.type='image/png';"
        "i.onload=function(){document.getElementById('out').textContent=i.naturalWidth+','+i.complete+','+s.srcset+','+(s instanceof HTMLSourceElement);};"
        "i.srcset='data:image/png,SIZED%2080x40 2x';i.sizes='auto, 400px';"
        "</script>",true);
    data_pool_open();responsive_serial_pool();script_turn();
    for(int t=0;t<8;t++){responsive_drain();script_turn();}
    check(probe_text_is("out","40,true,a 1x,true"),
          "responsive detached Image: srcset-only load event and reflected source properties");
    data_pool_close();probe_drop();
}

static char *responsive_replace(char *text,const char *from,const char *to)
{
    size_t count=0,n=strlen(from),m=strlen(to);
    for(char *p=text;(p=strstr(p,from));p+=n)count++;
    size_t cap=strlen(text)+count*m+1;
    char *out=malloc(cap),*at=out,*p=text,*hit;
    if(!out){free(text);return NULL;}
    while((hit=strstr(p,from))) {
        size_t part=(size_t)(hit-p);memcpy(at,p,part);at+=part;
        memcpy(at,to,m);at+=m;p=hit+n;
    }
    strcpy(at,p);free(text);return out;
}

static void responsive_gallery_case(void)
{
    size_t len;char *html=memory_fixture_read("image-responsive.html",&len);
    if(!html)return;
    for(int scale=1;scale<=2;scale++) {
        char name[64];snprintf(name,sizeof(name),"image-responsive-%dx.png",scale);
        char *png=memory_fixture_read(name,&len);
        char *base=png?data_b64(png,len):NULL;free(png);
        if(!base){free(html);return;}
        char *url=malloc(strlen(base)+32);
        if(!url){os64_free(base);free(html);return;}
        sprintf(url,"data:image/png;base64,%s",base);os64_free(base);
        html=responsive_replace(html,name,url);free(url);if(!html)return;
    }
    probe_page(html,true);free(html);
    data_pool_open();responsive_serial_pool();pictures_start(&g.page);
    for(int turn=0;turn<10;turn++){script_turn();responsive_drain();}
    const char *ids[]={"fixed-result","width-result","picture-result","detached-result"};
    for(unsigned i=0;i<4;i++)
        check(!strncmp(probe_text_of(ids[i]),"PASS:",5),"responsive gallery: real PNG, delivered script and immediate Canvas pass");
    ImageSource *width=image_source(&g.page,probe_id("width-image"));
    check(width && width->density==1,"responsive gallery: 100% chooses the 160w source");
    g.zoom=2000;image_sources_environment(&g.page,800,600,g.zoom);
    for(int turn=0;turn<8;turn++){responsive_drain();script_turn();}
    check(width && width->density==2 && !strncmp(probe_text_of("width-result"),"PASS:",5),
          "responsive gallery: 200% chooses 320w and keeps natural dimensions");
    os64_dom_event_t click={.type="click"};os64_js_outcome_t outcome;
    check(yonder_scripts_dispatch(g.page.scripts,probe_id("replace"),&click,NULL,&outcome)==OS64_JS_OK,
          "responsive gallery: replacement button executes");
    for(int turn=0;turn<8;turn++){responsive_drain();script_turn();}
    check(!strncmp(probe_text_of("detached-result"),"PASS:",5),"responsive gallery: detached replacement completes");
    data_pool_close();probe_drop();
}

static void responsive_cors_case(void)
{
    size_t len;char *png=memory_fixture_read("image-responsive-2x.png",&len);
    if(!png)return;
    fake_reset();fake_fetch_reset();
    const char *url="https://b.test/responsive.png";
    fake_answer_t *answer=fake_answer("GET",url,200,"Content-Type: image/png\r\n",png);
    answer->body_len=len;
    for(unsigned attempt=0;attempt<2;attempt++) {
        if(attempt)answer->headers="Content-Type: image/png\r\nAccess-Control-Allow-Origin: *\r\n";
        yonder_picture_job_t *job=sizes_job(80,45);
        os64_strcopy(job->url,sizeof(job->url),url);
        os64_strcopy(job->hooks.referrer,sizeof(job->hooks.referrer),"https://a.test/page");
        job->display_cors=2;job->svg_enabled=true;
        void *out=NULL;yonder_picture_run(job,NULL,NULL,&out);
        yonder_picture_t *product=out;
        check(product && (attempt ? product->status==OS64_IMAGE_OK && product->origin_clean &&
              product->width==320 && product->image.width==80 : product->status!=OS64_IMAGE_OK),
              "responsive CORS display: enforce access policy while retaining display reduction");
        yonder_picture_release(job,product);
    }
    fake_fetch_reset();fake_reset();free(png);
}

static void responsive_image_cases(void)
{
    responsive_parser_cases();responsive_window_cases();responsive_mutation_cases();responsive_script_cases();
    responsive_gallery_case();responsive_cors_case();
}
