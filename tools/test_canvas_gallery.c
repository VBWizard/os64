#define YONDER_HOST_FAKES_ONLY
#include "test_yonder_scripts_host.c"

const os64_font_backend_t *os64_freetype_real_backend_v1(void);
static uint8_t *gallery_font_bytes;
static size_t gallery_font_length;
static os64_text_font_t *gallery_font;
static uint32_t gallery_font_size;
static os64_font_status_t gallery_fonts(void *ctx,const flow_family_list_t *family,bool bold,bool italic,
    uint32_t px,os64_text_font_t *const **fonts,size_t *count,os64_font_face_info_t *info)
{
    (void)ctx;(void)family;(void)bold;(void)italic;
    if(!gallery_font||gallery_font_size!=px) {
        os64_text_font_t *fresh=NULL;os64_font_face_options_t options={px,OS64_FONT_HINT_NONE};
        os64_font_status_t status=os64_text_font_open(probe_text,gallery_font_bytes,gallery_font_length,&options,&fresh);
        if(status!=OS64_FONT_OK)return status;
        os64_text_font_release(gallery_font);gallery_font=fresh;gallery_font_size=px;
    }
    *fonts=&gallery_font;*count=1;return os64_text_font_info(gallery_font,info);
}
static os64_work_t gallery_jobs[32];
static os64_work_id_t gallery_ids[32];
static unsigned gallery_queued;
static os64_work_id_t gallery_submit(const os64_work_t *work)
{
    check(*(const uint32_t *)work->job==YONDER_JOB_PICTURE,"gallery only submits image jobs");
    if(gallery_queued==32)return 0;
    gallery_jobs[gallery_queued]=*work;gallery_ids[gallery_queued]=++pool_ids;
    return gallery_ids[gallery_queued++];
}
static void gallery_pump(void)
{
    for(unsigned turn=0;turn<100;++turn) {
        bool ran=script_turn();
        (void)yonder_scripts_images_pending(g.page.scripts);
        if(gallery_queued) {
            os64_work_t work=gallery_jobs[0];os64_work_id_t id=gallery_ids[0];
            --gallery_queued;memmove(gallery_jobs,gallery_jobs+1,gallery_queued*sizeof(*gallery_jobs));
            memmove(gallery_ids,gallery_ids+1,gallery_queued*sizeof(*gallery_ids));
            void *product=NULL;work.run(work.job,NULL,NULL,&product);reaped(id,work.job,product);ran=true;
        }
        if(!ran&&!yonder_scripts_images_pending(g.page.scripts))return;
    }
    check(false,"gallery image tasks settle");
}
static void dump_canvas(const char *directory,const char *name,const char *suffix)
{
    canvas_bitmap_t bitmap;
    check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id(name),&bitmap),"gallery retains canvas bitmap");
    if(!bitmap.pixels)return;
    char path[1024];snprintf(path,sizeof(path),"%s/%s%s.rgba",directory,name,suffix);
    FILE *out=fopen(path,"wb");check(out!=NULL,"gallery dump opens");if(!out)return;
    for(uint32_t y=0;y<bitmap.height;++y)for(uint32_t x=0;x<bitmap.width;++x) {
        uint32_t p=bitmap.pixels[(size_t)y*bitmap.stride/4+x];unsigned a=p>>24;
        check(((p>>16)&255)<=a&&((p>>8)&255)<=a&&(p&255)<=a,"gallery pixels remain premultiplied at translucent image edges");
        unsigned char rgba[]={a?(((p>>16)&255)*255+a/2)/a:0,a?(((p>>8)&255)*255+a/2)/a:0,a?((p&255)*255+a/2)/a:0,a};
        check(fwrite(rgba,1,4,out)==4,"gallery pixel written");
    }
    fclose(out);
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    FILE *file=fopen(argv[1],"rb");if(!file)return 2;
    fseek(file,0,SEEK_END);long n=ftell(file);rewind(file);
    if(n<0||n>1024*1024)return 2;
    char *html=malloc(n+1);if(!html)return 2;
    if(fread(html,1,n,file)!=(size_t)n)return 2;
    html[n]=0;fclose(file);
    bool hit=strstr(html,"YONDER / CANVAS 2D / C6D")!=NULL;
    bool retained=hit||strstr(html,"YONDER / CANVAS 2D / C6C")!=NULL;
    bool shadows=strstr(html,"YONDER / CANVAS 2D / C6B")!=NULL;
    bool paints=strstr(html,"YONDER / CANVAS 2D / C5")!=NULL;
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=(paints||shadows)?os64_freetype_real_backend_v1():flow_test_backend(),.memory_cap=32*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"bitmap face");
    bool images=strstr(html,"YONDER / CANVAS 2D / C4")!=NULL;
    probe_page(html,true);free(html);
    if(paints||shadows) {
        FILE *font=fopen("userland/libfreetype/fixtures/DejaVuSans.ttf","rb");check(font!=NULL,"gallery font opens");
        if(!font)return 2;
        fseek(font,0,SEEK_END);gallery_font_length=ftell(font);rewind(font);gallery_font_bytes=malloc(gallery_font_length);
        check(fread(gallery_font_bytes,1,gallery_font_length,font)==gallery_font_length,"gallery font reads");fclose(font);
        s_env.fonts=gallery_fonts;
    }
    if(images){pool_open=true;g.pool=(os64_work_pool_t *)1;pool_submit_hook=gallery_submit;}
    gallery_pump();
    if(retained)check(probe_text_is("status",hit?"Ready — four live geometry checks. Scripts + Modern Web; try 25% to 400% zoom.":"Four experiments, ready to turn, transform, and uncover."),"retained-path gallery initializes completely");
    const char *names[]={retained?"medals":shadows?"cast":paints?"glow":images?"stamps":"route",retained?"rings":shadows?"window":paints?"shared":images?"crop":"turn",retained?"curve":shadows?"stamp":paints?"lettering":images?"echo":"stencil",retained?"ticket":shadows?"type":paints?"fitted":images?"returned":"scratch"};
    const char *buttons[]={retained?"turn":shadows?"light":paints?"light":images?"stamp":"march",retained?"rule":shadows?"reveal":paints?"color":images?"inspect":"spin",retained?"stretch":shadows?"stamp-change":paints?"turn":images?"scoot":"slide",retained?"foil":shadows?"words":paints?"words":images?"export":"reveal"};
    for(unsigned i=0;i<4;++i) {
        dump_canvas(argv[2],names[i],"");
        if(retained) {
            canvas_bitmap_t b;check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id(names[i]),&b),"retained-path bitmap exists");
            size_t ink=0;for(uint32_t y=0;y<b.height;y++)for(uint32_t x=0;x<b.width;x++)ink+=b.pixels[(size_t)y*b.stride/4+x]>>24?1:0;
            check(ink>100,"retained-path gallery has visible ink");
        }
    }
    if(hit) {
        canvas_bitmap_t b;check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id("curve"),&b),"hit light bitmap");
        uint32_t *before=malloc(b.capacity);check(before!=NULL,"hit light snapshot");
        if(!before)return 2;
        memcpy(before,b.pixels,b.capacity);flow_tree_t *tree=g.page.tree;
        uint64_t version=os64_html_version(page_doc(&g.page));
        for(unsigned i=0;i<10;i++){now_ms+=40;clock_us+=40000;script_turn();motion_painted();}
        check(memcmp(before,b.pixels,b.capacity)!=0,"SVG queries drive moving light frames");free(before);
        check(g.page.tree==tree&&os64_html_version(page_doc(&g.page))==version,"fill queries and pixel-only frames preserve layout");
    }
    for(unsigned i=0;i<4;++i) {
        os64_dom_event_t event={.type="click",.bubbles=true,.cancelable=true};
        bool prevented=false;os64_js_outcome_t outcome;
        check(yonder_scripts_dispatch(g.page.scripts,probe_id(buttons[i]),&event,&prevented,&outcome)==OS64_JS_OK,"gallery button executes");
        gallery_pump();dump_canvas(argv[2],names[i],"-clicked");
    }
    if(retained) {
        canvas_bitmap_t bitmap;check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id("ticket"),&bitmap),"scratch bitmap exists");
        size_t before=0;for(uint32_t y=0;y<bitmap.height;y++)for(uint32_t x=0;x<bitmap.width;x++)before+=bitmap.pixels[(size_t)y*bitmap.stride/4+x]>>24?1:0;
        scroll_to_node(probe_id("ticket"));
        const flow_box_t *box=flow_box_for(g.page.tree,probe_id("ticket"));check(box!=NULL,"scratch canvas laid out");
        int x=g.view.bounds.x+box->rect.x+80-g.sx,y=g.view.bounds.y+box->rect.y+90-g.sy;
        pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_DOWN,x,y,0,1);
        for(int step=1;step<=12;step++)pointer_move(x+step*15,y,1);
        pointer_window(OS64_GUI_EVENT_MOUSE_BUTTON_UP,x+180,y,0,0);
        size_t after=0;for(uint32_t row=0;row<bitmap.height;row++)for(uint32_t col=0;col<bitmap.width;col++)after+=bitmap.pixels[(size_t)row*bitmap.stride/4+col]>>24?1:0;
        check(after+1000<before&&after>1000,"native captured pointer stroke erases foil through retained clip");
        dump_canvas(argv[2],"ticket","-scratched");
        os64_dom_event_t event={.type="click",.bubbles=true,.cancelable=true};
        bool prevented=false;os64_js_outcome_t outcome;
        check(yonder_scripts_dispatch(g.page.scripts,probe_id("foil"),&event,&prevented,&outcome)==OS64_JS_OK,"fresh foil handler executes");
        gallery_pump();
        size_t restored=0;for(uint32_t row=0;row<bitmap.height;row++)for(uint32_t col=0;col<bitmap.width;col++)restored+=bitmap.pixels[(size_t)row*bitmap.stride/4+col]>>24?1:0;
        check(restored==before,"fresh foil restores scratched pixels");
    }
    if(shadows) {
        os64_dom_event_t event={.type="click",.bubbles=true,.cancelable=true};
        bool prevented=false;os64_js_outcome_t outcome;
        check(yonder_scripts_dispatch(g.page.scripts,probe_id("animate"),&event,&prevented,&outcome)==OS64_JS_OK,"shadow animation starts");
        gallery_pump();
        flow_tree_t *tree=g.page.tree;uint64_t version=os64_html_version(page_doc(&g.page));
        canvas_bitmap_t bitmap;
        check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id("cast"),&bitmap),"animated shadow canvas exists");
        uint32_t *before=malloc(bitmap.capacity);check(before!=NULL,"animation snapshot");
        if(!before)return 2;
        memcpy(before,bitmap.pixels,bitmap.capacity);
        for(unsigned frame=0;frame<12;frame++) {
            now_ms+=40;clock_us+=40000;script_turn();motion_painted();
        }
        check(memcmp(before,bitmap.pixels,bitmap.capacity)!=0,"scheduled frames move the shadow");free(before);
        check(tree==g.page.tree&&version==os64_html_version(page_doc(&g.page)),"shadow animation preserves layout and DOM version");
        check(yonder_scripts_dispatch(g.page.scripts,probe_id("animate"),&event,&prevented,&outcome)==OS64_JS_OK,"shadow animation pauses");
        gallery_pump();
        check(yonder_scripts_animation_next(g.page.scripts)==YONDER_NEVER,"paused gallery cancels pending frame");
    }
    if(images) {
        check(g.page.waiting==0&&g.page.in_flight==0&&!gallery_queued,"gallery images all settled");
        canvas_bitmap_t original,returned,stamps;
        check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id("original"),&original)&&
              yonder_scripts_canvas_bitmap(g.page.scripts,probe_id("returned"),&returned),"export has source and returned pixels");
        check(original.capacity==returned.capacity&&!memcmp(original.pixels,returned.pixels,original.capacity),"PNG export/decode/draw preserves every premultiplied pixel");
        check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id("stamps"),&stamps)&&stamps.pixels[75*400+53]!=0,"SVG stamp loaded and drawn");
        pool_submit_hook=NULL;g.pool=NULL;pool_open=false;
    }
    probe_drop();os64_text_font_release(probe_font);
    os64_text_font_release(gallery_font);free(gallery_font_bytes);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"gallery text released");
    check(live==0,"gallery heap released");
    printf("Canvas gallery: %u failed\n",failures);return failures?1:0;
}
