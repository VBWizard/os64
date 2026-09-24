/* Build the included collection through the production preparation, container
 * and painting paths. The Python driver supplies the pinned font backend. */
#include "../userland/apps/framestudio/storage.h"
#include "os64/mem.h"
#include "os64/str.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }
static void *allocate(void *u,size_t n) { (void)u;return malloc(n); }
static void release(void *u,void *p,size_t n) { (void)u;(void)n;free(p); }

static const char *names[]={"Blue-Hour","Orchard","Porcelain","Copperline","Moss-and-Linen","Signal"};
static const char *titles[]={"Blue Hour","Orchard","Porcelain","Copperline","Moss and Linen","Signal"};

static void palette(os64_decor_header_t *h,uint32_t face,uint32_t face2,
    uint32_t inactive,uint32_t inactive2,uint32_t text,uint32_t dim,uint32_t edge,uint32_t dim_edge)
{
    h->active_face=0xff000000|face;h->active_face2=0xff000000|face2;
    h->inactive_face=0xff000000|inactive;h->inactive_face2=0xff000000|inactive2;
    h->active_text=0xff000000|text;h->inactive_text=0xff000000|dim;
    h->active_border=h->active_border2=0xff000000|edge;
    h->inactive_border=h->inactive_border2=0xff000000|dim_edge;
}
static frame_draft_t composition(unsigned i)
{
    frame_draft_t d={0};os64_decor_defaults(&d.style);
    strcpy(d.font.face[0],"/etc/fonts/DejaVuSans.ttf");d.font.size=24;
    os64_decor_header_t *h=&d.style;
    h->padding_x=10;h->padding_y=5;h->button_size=24;h->button_gap=6;
    h->buttons[2].shape=OS64_DECOR_ROUND;
    switch(i){
    case 0:
        palette(h,0x162b4d,0x3f6dab,0x253140,0x394656,0xe7f1ff,0xa4b0c0,0x4f78ae,0x394656);
        h->finish=OS64_DECOR_GRAIN;h->strength=13;h->relief=1;
        h->button_count=4;h->buttons[3]=(os64_decor_button_t){OS64_DECOR_PIN,0,OS64_DECOR_BARE,0};
        break;
    case 1:
        palette(h,0xf4f3f1,0xd7d6d3,0xe8e7e5,0xdeddd9,0x25282d,0x73767a,0xa5a7aa,0xbabbbd);
        d.font.size=22;h->align=1;h->finish=OS64_DECOR_GRADIENT;
        h->button_size=20;h->button_gap=7;h->padding_x=12;h->padding_y=6;
        h->buttons[0]=(os64_decor_button_t){OS64_DECOR_CLOSE,0,OS64_DECOR_ROUND,0xffff6259};
        h->buttons[1]=(os64_decor_button_t){OS64_DECOR_MINIMIZE,0,OS64_DECOR_ROUND,0xffffbd2e};
        h->buttons[2]=(os64_decor_button_t){OS64_DECOR_MAXIMIZE,0,OS64_DECOR_ROUND,0xff2dca46};
        for(unsigned k=0;k<3;++k)h->symbols[k]=(os64_decor_symbol_t){0xff202420,0xff424640};
        break;
    case 2:
        palette(h,0xf3f0e7,0xe5dfd2,0xe5e2db,0xdad7d0,0x284e46,0x68736b,0x708f7a,0xa5ada2);
        h->finish=OS64_DECOR_SOLID;h->padding_y=6;
        for(unsigned k=0;k<3;++k)h->buttons[k].shape=OS64_DECOR_BARE;
        break;
    case 3:
        palette(h,0x272b31,0x3c424a,0x303238,0x383a40,0xf4dbc3,0xb4aaa2,0xc1845c,0x665b54);
        h->finish=OS64_DECOR_GRADIENT;h->border=2;h->padding_y=6;
        h->buttons[2].face=0xffa86e4c;h->symbols[0]=(os64_decor_symbol_t){0xff181a1e,0xff282a2e};
        break;
    case 4:
        palette(h,0x46564b,0x91a080,0x4b514c,0x737a6d,0xf0ead8,0xb9beaf,0xa7ae8e,0x737c6d);
        h->finish=OS64_DECOR_GRAIN;h->strength=12;h->relief=1;h->border=2;
        h->buttons[2].face=0xffc7bd9f;h->symbols[0]=(os64_decor_symbol_t){0xff303e34,0xff465044};
        break;
    case 5:
        palette(h,0x132b3c,0x1c4258,0x293640,0x34434d,0xecf8fa,0xacbcc4,0x55c8d1,0x567783);
        h->finish=OS64_DECOR_SOLID;h->border=3;h->relief=1;h->button_size=26;
        for(unsigned k=0;k<3;++k)h->buttons[k].shape=OS64_DECOR_SQUARE;
        h->buttons[2].face=0xff55c8d1;h->symbols[0]=(os64_decor_symbol_t){0xff102735,0xff233b47};
        break;
    }
    return d;
}
static void write_file(const char *path,const void *data,size_t size)
{
    FILE *f=fopen(path,"wb");if(!f){perror(path);exit(1);}
    if(fwrite(data,1,size,f)!=size || fclose(f)){perror(path);exit(1);}
}
static void preview(os64_decor_surface_t *surface,const os64_decor_view_t *view,unsigned i)
{
    int x=28+(int)(i%2)*700,y=24+(int)(i/2)*330;
    for(unsigned state=0;state<2;++state){
        os64_decor_rect_t r={x,y+(int)state*148,652,124};
        os64_decor_insets_t in=os64_decor_insets(view->header,true);
        for(int yy=r.y+in.top;yy<r.y+r.h-in.bottom;++yy)
            for(int xx=r.x+in.left;xx<r.x+r.w-in.right;++xx)
                surface->pixels[(size_t)yy*surface->pitch+xx]=state?0xffd9dcddu:0xffeceeeeu;
        char title[96];snprintf(title,sizeof(title),"%s / %s",titles[i],state?"Inactive":"Caf\xc3\xa9 & R\xc3\xa9sum\xc3\xa9");
        os64_decor_state_t controls={0};
        assert(os64_decor_paint(view,surface,r,(os64_decor_rect_t){0,0,(int)surface->width,(int)surface->height},
            true,!state,false,title,strlen(title),false,&controls));
    }
}
int main(int argc,char **argv)
{
    if(argc!=4){fprintf(stderr,"usage: frame_collection FONT OUTDIR PREVIEW.ppm\n");return 2;}
    FILE *font=fopen(argv[1],"rb");if(!font){perror(argv[1]);return 1;}
    assert(!fseek(font,0,SEEK_END));long length=ftell(font);assert(length>0);rewind(font);
    uint8_t *source=malloc((size_t)length);assert(source);
    assert(fread(source,1,(size_t)length,font)==(size_t)length);assert(!fclose(font));
    os64_text_context_t *text=NULL;os64_text_options_t options={.memory={NULL,allocate,release}};
    assert(os64_font_context_create(&options,&text)==OS64_FONT_OK);
    os64_decor_surface_t surface={.width=1400,.height=1000,.pitch=1400};
    surface.pixels=malloc((size_t)surface.width*surface.height*4);assert(surface.pixels);
    for(size_t p=0;p<(size_t)surface.width*surface.height;++p)surface.pixels[p]=0xff18212a;
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i){
        frame_draft_t draft=composition(i);
        os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT]={0};
        specs[0].pixel_height=draft.font.size;
        specs[0].primary=(os64_font_source_t){OS64_FONT_SOURCE_OUTLINE,source,(size_t)length};
        os64_font_set_t *set=NULL;os64_font_role_view_t role;
        assert(os64_font_set_prepare(text,specs,&set)==OS64_FONT_OK);
        assert(os64_font_set_view(set,OS64_FONT_ROLE_UI,&role)==OS64_FONT_OK);
        void *bundle=NULL,*file=NULL,*decoded=NULL;size_t bytes=0,file_bytes=0,decoded_bytes=0;
        assert(os64_decor_prepare(&role,&draft.style,&bundle,&bytes)==OS64_FONT_OK);
        os64_font_set_release(set);
        assert(frame_encode(&draft,bundle,bytes,&file,&file_bytes)==FRAME_STORE_OK);
        frame_draft_t roundtrip;
        assert(frame_decode(file,file_bytes,&roundtrip,&decoded,&decoded_bytes)==FRAME_STORE_OK);
        assert(frame_same(&draft,&roundtrip) && bytes==decoded_bytes && !memcmp(bundle,decoded,bytes));
        os64_decor_view_t view;assert(os64_decor_validate(decoded,decoded_bytes,&view));
        preview(&surface,&view,i);
        char path[4096];int n=snprintf(path,sizeof(path),"%s/%s.frame",argv[2],names[i]);
        assert(n>0 && (size_t)n<sizeof(path));write_file(path,file,file_bytes);
        printf("%s: %zu bytes; prepared, decoded and painted\n",names[i],file_bytes);
        free(decoded);free(file);free(bundle);
    }
    FILE *ppm=fopen(argv[3],"wb");if(!ppm){perror(argv[3]);return 1;}
    fprintf(ppm,"P6\n%u %u\n255\n",surface.width,surface.height);
    for(size_t p=0;p<(size_t)surface.width*surface.height;++p){
        uint32_t c=surface.pixels[p];uint8_t rgb[]={(uint8_t)(c>>16),(uint8_t)(c>>8),(uint8_t)c};
        assert(fwrite(rgb,1,3,ppm)==3);
    }
    assert(!fclose(ppm));free(surface.pixels);free(source);
    assert(os64_text_destroy(text)==OS64_FONT_OK);return 0;
}
