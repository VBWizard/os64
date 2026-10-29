#define YONDER_HOST_FAKES_ONLY
#include "test_yonder_scripts_host.c"

static void dump_canvas(const char *directory,const char *name,const char *suffix)
{
    canvas_bitmap_t bitmap;
    check(yonder_scripts_canvas_bitmap(g.page.scripts,probe_id(name),&bitmap),"gallery retains canvas bitmap");
    if(!bitmap.pixels)return;
    char path[1024];snprintf(path,sizeof(path),"%s/%s%s.rgba",directory,name,suffix);
    FILE *out=fopen(path,"wb");check(out!=NULL,"gallery dump opens");if(!out)return;
    for(uint32_t y=0;y<bitmap.height;++y)for(uint32_t x=0;x<bitmap.width;++x) {
        uint32_t p=bitmap.pixels[(size_t)y*bitmap.stride/4+x];unsigned a=p>>24;
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
    os64_text_options_t options={.memory={.alloc=probe_alloc,.free=probe_free},
        .backend=flow_test_backend(),.memory_cap=8*1024*1024};
    check(os64_text_create(&options,&probe_text)==OS64_FONT_OK,"text context");
    check(os64_text_font_bitmap(probe_text,&probe_font)==OS64_FONT_OK,"bitmap face");
    probe_page(html,true);free(html);script_turn();
    const char *names[]={"route","turn","stencil","scratch"},*buttons[]={"march","spin","slide","reveal"};
    for(unsigned i=0;i<4;++i)dump_canvas(argv[2],names[i],"");
    for(unsigned i=0;i<4;++i) {
        os64_dom_event_t event={.type="click",.bubbles=true,.cancelable=true};
        bool prevented=false;os64_js_outcome_t outcome;
        check(yonder_scripts_dispatch(g.page.scripts,probe_id(buttons[i]),&event,&prevented,&outcome)==OS64_JS_OK,"gallery button executes");
        dump_canvas(argv[2],names[i],"-clicked");
    }
    probe_drop();os64_text_font_release(probe_font);
    check(os64_text_destroy(probe_text)==OS64_FONT_OK,"gallery text released");
    check(live==0,"gallery heap released");
    printf("Canvas gallery: %u failed\n",failures);return failures?1:0;
}
