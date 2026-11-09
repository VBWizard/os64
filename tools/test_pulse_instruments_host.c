/* Real geometry, font and blending paths; guest I/O is absent from this renderer. */
#define main pulse_instruments_font_suite_main
#include "test_ui_text_host.c"
#undef main
#include "os64/ui_menu.h"
os64_ui_status_code_t os64_ui_menu_open_list(os64_ui_menu_session_t *s,const os64_ui_menu_list_t *l,os64_gui_rect_t a)
{(void)s;(void)l;(void)a;return OS64_UI_ERROR;}
void os64_ui_menu_list_detach(os64_ui_menu_session_t *s,void *u){(void)s;(void)u;}
#include "../userland/apps/pulse/pulse_instruments.h"

int64_t os64_open(const char *p,const char *m){(void)p;(void)m;return -1;}
int64_t os64_read(int32_t f,void *p,size_t n){(void)f;(void)p;(void)n;return -1;}
int64_t os64_write(int32_t f,const void *p,size_t n){(void)f;(void)p;(void)n;return -1;}
int64_t os64_close(int32_t f){(void)f;return 0;}

static pulse_instrument_readings_t example(void)
{
    return (pulse_instrument_readings_t){.current={
        .cpu_valid=true,.ram_valid=true,.disk_valid=true,.net_valid=true,
        .cpu_percent=37,.ram_used=3ULL<<30,.ram_available=5ULL<<30,.ram_total=8ULL<<30,
        .disk_used=120ULL<<30,.disk_available=380ULL<<30,.disk_total=500ULL<<30,
        .download=1572864,.upload=262144,.network_scale=2097152},
        .peak_valid={true,true,false,true,true},.peak={82,61,0,1900000,900000}};
}
static bool dial_font(os64_ui_t *ui,const char *dir,unsigned px)
{
    size_t length;uint8_t *bytes=slurp(dir,"DejaVuSans.ttf",&length);
    if(!bytes)return false;
    os64_font_role_spec_t specs[OS64_FONT_ROLE_COUNT]={0};
    for(unsigned i=0;i<OS64_FONT_ROLE_COUNT;++i)specs[i].pixel_height=16;
    specs[OS64_FONT_ROLE_UI]=(os64_font_role_spec_t){.pixel_height=px,
        .primary={OS64_FONT_SOURCE_OUTLINE,bytes,length}};
    specs[OS64_FONT_ROLE_DOCUMENT]=(os64_font_role_spec_t){.pixel_height=px*2/3<16?16:px*2/3,
        .primary={OS64_FONT_SOURCE_OUTLINE,bytes,length}};
    os64_font_set_t *set=NULL;
    os64_font_status_t rc=os64_font_set_prepare(os64_ui_font_context(ui),specs,&set);
    free(bytes);if(rc)return false;
    os64_font_consumer_t consumer;os64_ui_font_consumer(ui,&consumer);
    rc=os64_font_adopt(set,&consumer,1,NULL);os64_font_set_release(set);return rc==OS64_FONT_OK;
}
static void ppm(const char *dir,const char *name,os64_gui_surface_t *s,os64_gui_rect_t b)
{
    char path[512];snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    FILE *f=fopen(path,"wb");CHECK(f);if(!f)return;
    fprintf(f,"P6\n%d %d\n255\n",b.w,b.h);bool ok=true;
    for(int y=b.y;y<b.y+b.h;++y)for(int x=b.x;x<b.x+b.w;++x){
        uint32_t p=s->pixels[(size_t)y*s->pitch_px+x];unsigned char rgb[]={p>>16,p>>8,p};
        if(fwrite(rgb,1,3,f)!=3)ok=false;
    }
    CHECK(ok);CHECK(!fclose(f));
}
static void scenario(os64_ui_t *ui,const char *dir,bool compact,int w,int h,unsigned px)
{
    char name[64];snprintf(name,sizeof(name),"%s-%d-%upx",compact?"compact":"instruments",w,px);current=name;
    os64_draw_ctx_t ctx={0};ui->ctx=&ctx;
    os64_gui_surface_t *s=&ctx.surf;
    *s=(os64_gui_surface_t){.width=w+20,.height=h+20,.pitch_px=w+24};
    size_t bytes=(size_t)s->pitch_px*s->height*4;
    s->pixels=malloc(bytes);CHECK(s->pixels);if(!s->pixels)return;
    memset(s->pixels,0x5a,bytes);
    os64_gui_rect_t b={10,10,w,h};
    pulse_config_t config;pulse_config_defaults(&config);
    pulse_instrument_readings_t sample=example();
    pulse_instrument_layout_t layout;
    bool fits=pulse_instruments_layout(ui,&config,compact,b,&layout);CHECK(fits);
    if(!fits){free(s->pixels);ui->ctx=NULL;return;}
    pulse_instruments_t *p=pulse_instruments_create(ui);CHECK(p);if(!p){free(s->pixels);return;}
    CHECK(pulse_instruments_paint(p,s,&layout,&config,&sample));
    ppm(dir,name,s,b);
    uint32_t *copy=malloc(bytes);CHECK(copy);memcpy(copy,s->pixels,bytes);
    size_t cached=pulse_instruments_live_bytes(p);
    pulse_instruments_cache(p,false);
    CHECK(pulse_instruments_live_bytes(p)<cached);
    CHECK(pulse_instruments_paint(p,s,&layout,&config,&sample));
    CHECK(!memcmp(copy,s->pixels,bytes));free(copy);
    bool guard=true;
    for(unsigned y=0;y<s->height;++y)for(unsigned x=0;x<s->pitch_px;++x)
        if((x<10 || x>=(unsigned)w+10 || y<10 || y>=(unsigned)h+10) && s->pixels[(size_t)y*s->pitch_px+x]!=0x5a5a5a5a)guard=false;
    CHECK(guard);
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i){os64_gui_rect_t d=layout.dials[i];CHECK(pulse_instruments_hit(&layout,d.x+d.w/2,d.y+d.h/2)==(int)i);}
    CHECK(pulse_instruments_hit(&layout,-1,-1)==-1);
    pulse_instrument_layout_t prior=layout;
    CHECK(!pulse_instruments_layout(ui,&config,compact,(os64_gui_rect_t){0,0,20,20},&layout));
    CHECK(!memcmp(&prior,&layout,sizeof(layout)));
    // Refusing a face cache still permits the uncached draw. Refusing all
    // geometry storage reports failure and leaves a subsequent retry usable.
    pulse_instruments_cache(p,true);deny_countdown=0;
    CHECK(pulse_instruments_paint(p,s,&layout,&config,&sample));deny_countdown=-1;
    ui_test_deny_all=true;CHECK(!pulse_instruments_paint(p,s,&layout,&config,&sample));ui_test_deny_all=false;
    CHECK(pulse_instruments_paint(p,s,&layout,&config,&sample));
    for(unsigned mask=0;mask<16;++mask){
        for(unsigned i=0;i<PULSE_METERS;++i)config.show[i]=(mask&(1u<<i))!=0;
        CHECK(pulse_instruments_layout(ui,&config,compact,b,&layout));
        CHECK(pulse_instruments_paint(p,s,&layout,&config,&sample));
    }
    pulse_config_defaults(&config);CHECK(pulse_instruments_layout(ui,&config,compact,b,&layout));
    for(unsigned units=0;units<PULSE_UNITS;++units){
        config.units=units;
        for(unsigned large=0;large<3;++large){
            sample.current.download=large==0?0:large==1?1234567890123ULL:UINT64_MAX;
            sample.current.network_scale=UINT64_MAX;
            CHECK(pulse_instruments_paint(p,s,&layout,&config,&sample));
        }
    }
    sample.current=(pulse_sample_t){.network_scale=2097152};
    CHECK(pulse_instruments_paint(p,s,&layout,&config,&sample));
    if(compact){snprintf(name,sizeof(name),"unavailable-%d-%upx",w,px);ppm(dir,name,s,b);}
    printf("%s: rows %d/%d; drawing peak %zu bytes\n",name,layout.primary_row,layout.caption_row,pulse_instruments_peak_bytes(p));
    CHECK(pulse_instruments_peak_bytes(p)<=8u*1024*1024);
    pulse_instruments_destroy(p);free(s->pixels);ui->ctx=NULL;
}
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    os64_ui_t ui={0};ui_test_view_theme(&ui.theme);
    for(unsigned px=24;px<=26;px+=2){
        current="adopt primary and caption fonts";CHECK(dial_font(&ui,argv[1],px));
        scenario(&ui,argv[2],false,600,560,px);
        scenario(&ui,argv[2],false,300,1120,px);
        scenario(&ui,argv[2],true,px==24?320:360,px==24?320:360,px);
    }
    current="allocation refusal and cleanup";
    for(long fail=0;fail<12;++fail){deny_countdown=fail;pulse_instruments_t *p=pulse_instruments_create(&ui);deny_countdown=-1;pulse_instruments_destroy(p);}
    CHECK(os64_ui_font_release(&ui)==OS64_FONT_OK);
    printf("pulse instruments: %lu checks, %d failures\n",checks,failures);return failures?1:0;
}
