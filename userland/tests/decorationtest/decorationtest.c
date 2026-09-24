/* Real-WM fixture for prepared fonts, geometry, and publication. --hold keeps
 * the windows available for pointer/VT/maximize checks; q exits the fixture. */
#include "os64/os64.h"
#include "os64/draw.h"
#include "os64/decoration_prepare.h"
#include "os64/font_config.h"

static unsigned failures;
static unsigned composition=1,close_requests,client_down,client_up;
#define CHECK(x) do { if (!(x)) { ++failures; os64_printf("decorationtest: FAIL %d: %s\n",__LINE__,#x); } } while (0)
static void *allocate(void *u,size_t n) {(void)u;return os64_malloc(n);}
static void release(void *u,void *p,size_t n) {(void)u;(void)n;os64_free(p);}

typedef struct {
    const void *bytes;
    size_t length;
    uint64_t generation;
    int64_t descriptor;
    volatile uint32_t ready;
} race_t;
static int64_t publish_racer(void *arg)
{
    race_t *r=arg;
    while (!__atomic_load_n(&r->ready,__ATOMIC_ACQUIRE)) os64_yield();
    if (r->descriptor>=0) return os64_write(r->descriptor,r->bytes,r->length);
    return os64_decor_apply(r->bytes,r->length,r->generation);
}
static void race(race_t *r, int64_t success)
{
    r->ready=0;
    int64_t a=os64_thread(publish_racer,r),b=os64_thread(publish_racer,r);
    __atomic_store_n(&r->ready,1,__ATOMIC_RELEASE);
    CHECK(a>=0 && b>=0);
    int64_t first=-2,second=-2;
    if (a>=0) CHECK(os64_thread_join((int32_t)a,&first)==0);
    if (b>=0) CHECK(os64_thread_join((int32_t)b,&second)==0);
    CHECK((first==success && second<0) || (second==success && first<0));
}

static void refused_staging(uint64_t expected)
{
    int64_t fd=os64_open("/sys/decorations","w");
    CHECK(fd>=0);
    if (fd<0) return;
    os64_decor_command_t begin={.command=OS64_DECOR_BEGIN,
        .total_bytes=sizeof(os64_decor_header_t),.expected_generation=expected};
    race_t r={.bytes=&begin,.length=sizeof(begin),.descriptor=fd};
    race(&r,sizeof(begin));
    struct {os64_decor_command_t command;os64_decor_header_t invalid;} packet={0};
    packet.command=begin;packet.command.command=OS64_DECOR_DATA;
    packet.command.data_bytes=sizeof(packet.invalid);packet.command.offset=1;
    CHECK(os64_write(fd,&packet,sizeof(packet))<0);
    packet.command.offset=0;
    CHECK(os64_write(fd,&packet,sizeof(packet))==(int64_t)sizeof(packet));
    begin.command=OS64_DECOR_COMMIT;begin.offset=sizeof(packet.invalid);
    CHECK(os64_write(fd,&begin,sizeof(begin))<0);
    os64_close(fd);
    uint64_t actual;
    CHECK(os64_decor_generation(&actual)==0 && actual==expected);
    fd=os64_open("/sys/decorations","w");
    CHECK(fd>=0);
    if (fd>=0) {
        begin.command=OS64_DECOR_BEGIN;begin.offset=0;
        CHECK(os64_write(fd,&begin,sizeof(begin))==(int64_t)sizeof(begin));
        os64_close(fd);
        CHECK(os64_decor_generation(&actual)==0 && actual==expected);
    }
}

static void *prepare(uint32_t size, size_t *length)
{
    os64_text_context_t *text=NULL;
    os64_text_options_t options={.memory={NULL,allocate,release}};
    if (os64_font_context_create(&options,&text)!=OS64_FONT_OK) return NULL;
    os64_font_config_t config;
    os64_font_config_defaults(&config);
    os64_strcopy(config.roles[0].face[0],sizeof(config.roles[0].face[0]),"/tests/fonts/DejaVuSans.ttf");
    config.roles[0].size=size;
    os64_font_set_t *set=NULL;
    os64_font_config_error_t error;
    void *bundle=NULL;
    if (os64_font_config_prepare(text,&config,&set,&error)==OS64_FONT_CONFIG_OK) {
        os64_font_role_view_t role;
        if (os64_font_set_view(set,OS64_FONT_ROLE_UI,&role)==OS64_FONT_OK) {
            os64_decor_header_t style;os64_decor_defaults(&style);
            style.symbols[OS64_DECOR_CLOSE-1]=(os64_decor_symbol_t){0xff102030,0xff405060};
            if (composition==2) {
                style.button_count=1;os64_memset(style.buttons,0,sizeof(style.buttons));
                style.buttons[0]=(os64_decor_button_t){OS64_DECOR_CLOSE,1,OS64_DECOR_ROUND,0};
            } else if (composition==3) {
                style.buttons[0]=(os64_decor_button_t){OS64_DECOR_CLOSE,0,OS64_DECOR_ROUND,0};
                style.buttons[1]=(os64_decor_button_t){OS64_DECOR_MAXIMIZE,0,OS64_DECOR_SQUARE,0};
                style.buttons[2]=(os64_decor_button_t){OS64_DECOR_MINIMIZE,0,OS64_DECOR_BARE,0};
            } else if (composition==4) {
                style.button_count=0;os64_memset(style.buttons,0,sizeof(style.buttons));
            } else if (composition==5) {
                style.button_count=4;
                style.buttons[3]=(os64_decor_button_t){OS64_DECOR_PIN,0,OS64_DECOR_ROUND,0};
            }
            os64_font_status_t status=os64_decor_prepare(&role,&style,&bundle,length);
            if (status) os64_printf("decorationtest: prepare refused %d\n",status);
        }
    } else os64_printf("decorationtest: font config refused %d/%d\n",error.status,error.font_status);
    os64_font_set_release(set);
    CHECK(os64_text_destroy(text)==OS64_FONT_OK);
    return bundle;
}

static void paint(os64_draw_ctx_t *ctx, const char *title)
{
    CHECK(os64_draw_ctx_refresh(ctx)==0);
    os64_draw_fill_rect(&ctx->surf,(os64_gui_rect_t){0,0,(int32_t)ctx->surf.width,(int32_t)ctx->surf.height},0xff18222e);
    os64_draw_text(&ctx->surf,20,25,title,os64_strlen(title),0xffece9e2,0xff18222e);
    const char *help="1-5 layout; +/- font; q exits";
    os64_draw_text(&ctx->surf,20,55,help,os64_strlen(help),0xffd5a650,0xff18222e);
    ctx->surf.pixels[100*ctx->surf.pitch_px+100]=0xffaabbcc;
    CHECK(os64_gui_window_publish(ctx->win,NULL)==0);
}

int main(int argc,char **argv)
{
    bool hold=argc==2 && os64_streq(argv[1],"--hold");
    uint32_t sw,sh;
    if (os64_gui_screen_info(&sw,&sh)<0) {os64_printf("decorationtest: SKIP no GUI\n");return 0;}
    int64_t window=os64_gui_window_create_content("Frame Studio: Caf\xc3\xa9 / R\xc3\xa9sum\xc3\xa9",160,180,500,240,0);
    int64_t other=os64_gui_window_create_content("Inactive title - readable too",710,250,420,200,OS64_GUI_WINDOW_START_UNFOCUSED);
    if (window<=0 || other<=0) return 1;
    os64_draw_ctx_t ctx,second;
    CHECK(os64_draw_ctx_init(&ctx,window)==0);
    CHECK(os64_draw_ctx_init(&second,other)==0);
    if (failures) goto done;
    paint(&ctx,"Drawable contents retain their dimensions.");
    paint(&second,"The title font belongs to the decoration.");
    os64_gui_window_state_t before,after;
    CHECK(os64_gui_window_get_state(window,&before)==0);
    uint32_t *pixels=ctx.surf.pixels;
    size_t length=0;uint32_t size=24;
    void *bundle=prepare(size,&length);
    CHECK(bundle!=NULL);
    if (!bundle) goto done;
    uint64_t generation,installed;
    /* Publish real V4/V5 layouts to exercise compatibility at the WM boundary. */
    for(unsigned version=4;version<=5;++version){
        size_t delta=sizeof(os64_decor_header_t)-OS64_DECOR_LEGACY_HEADER_BYTES;
        size_t legacy_length=length-delta;
        uint8_t *legacy=os64_malloc(legacy_length);CHECK(legacy!=NULL);
        if(!legacy)continue;
        os64_memcpy(legacy,bundle,OS64_DECOR_LEGACY_HEADER_BYTES);
        os64_memcpy(legacy+OS64_DECOR_LEGACY_HEADER_BYTES,(uint8_t *)bundle+sizeof(os64_decor_header_t),length-sizeof(os64_decor_header_t));
        os64_decor_header_t *old=(void *)legacy;
        old->version=version;old->bytes=(uint32_t)legacy_length;
        old->glyph_offset-=(uint32_t)delta;old->pair_offset-=(uint32_t)delta;
        old->mask_offset-=(uint32_t)delta;old->tile_offset-=(uint32_t)delta;
        CHECK(os64_decor_generation(&generation)==0);
        CHECK(os64_decor_apply(legacy,legacy_length,generation)==0);
        os64_decor_status_t legacy_status;
        CHECK(os64_decor_current(&legacy_status)==0 && legacy_status.generation==generation+1 &&
            legacy_status.bytes==legacy_length && legacy_status.fingerprint==os64_decor_fingerprint(legacy,legacy_length));
        os64_free(legacy);
    }
    CHECK(os64_decor_generation(&generation)==0);
    os64_decor_status_t initial,current;
    CHECK(os64_decor_current(&initial)==0 && initial.generation==generation);
    refused_staging(generation);
    CHECK(os64_decor_current(&current)==0 && current.generation==initial.generation &&
        current.fingerprint==initial.fingerprint && current.bytes==initial.bytes);
    CHECK(os64_decor_apply(bundle,length,generation)==0);
    CHECK(os64_decor_generation(&installed)==0 && installed==generation+1);
    CHECK(os64_decor_current(&current)==0 && current.generation==installed &&
        current.bytes==length && current.fingerprint==os64_decor_fingerprint(bundle,length));
    CHECK(os64_decor_apply(bundle,length,generation)<0);
    CHECK(os64_decor_current(&initial)==0 && initial.generation==current.generation &&
        initial.fingerprint==current.fingerprint && initial.bytes==current.bytes);
    CHECK(os64_decor_generation(&generation)==0 && generation==installed);
    race_t publication={.bytes=bundle,.length=length,.generation=generation,.descriptor=-1};
    race(&publication,0);
    CHECK(os64_decor_generation(&installed)==0 && installed==generation+1);
    CHECK(os64_draw_ctx_refresh(&ctx)==0);
    CHECK(ctx.surf.width==500 && ctx.surf.height==240 && ctx.surf.pixels==pixels);
    CHECK(ctx.surf.pixels[100*ctx.surf.pitch_px+100]==0xffaabbcc);
    CHECK(os64_gui_window_get_state(window,&after)==0);
    CHECK(before.y+(int32_t)before.height==after.y+(int32_t)after.height);
    os64_decor_view_t view;CHECK(os64_decor_validate(bundle,length,&view));
    os64_decor_insets_t insets=os64_decor_insets(view.header,true);
    CHECK(after.height==240+(uint32_t)(insets.top+insets.bottom));
    int64_t newest=os64_gui_window_create_content("Created after Apply",35,500,300,160,OS64_GUI_WINDOW_START_UNFOCUSED);
    CHECK(newest>0);
    if (newest>0) {
        os64_draw_ctx_t fresh;CHECK(os64_draw_ctx_init(&fresh,newest)==0);
        CHECK(fresh.surf.width==300 && fresh.surf.height==160);
        CHECK(os64_gui_window_destroy(newest)==0);
    }
    /* A close-only narrow window must make a wider composition fail without
     * changing its contents or publication generation. */
    os64_decor_header_t *header=bundle;
    os64_decor_header_t saved=*header;
    header->button_count=1;os64_memset(header->buttons,0,sizeof(header->buttons));
    header->buttons[0]=(os64_decor_button_t){OS64_DECOR_CLOSE,1,OS64_DECOR_ROUND,0};
    CHECK(os64_decor_generation(&generation)==0);
    CHECK(os64_decor_apply(bundle,length,generation)==0);
    int64_t narrow=os64_gui_window_create_content("Narrow",20,550,70,100,OS64_GUI_WINDOW_START_UNFOCUSED);
    CHECK(narrow>0);
    CHECK(os64_decor_current(&initial)==0 && initial.fingerprint==os64_decor_fingerprint(bundle,length));
    *header=saved;
    CHECK(os64_decor_generation(&generation)==0);
    CHECK(os64_decor_apply(bundle,length,generation)<0);
    CHECK(os64_decor_generation(&installed)==0 && installed==generation);
    CHECK(os64_decor_current(&current)==0 && current.generation==initial.generation &&
        current.fingerprint==initial.fingerprint && current.bytes==initial.bytes);
    if (narrow>0) CHECK(os64_gui_window_destroy(narrow)==0);
    CHECK(os64_decor_apply(bundle,length,generation)==0);
    CHECK(os64_decor_generation(&installed)==0);
    os64_free(bundle);
    os64_printf("decorationtest: %s font=%upx screen=%ux%u generation=%lu frame=%ux%u content=%ux%u\n",
        failures?"FAIL":"PASS",size,sw,sh,installed,after.width,after.height,ctx.surf.width,ctx.surf.height);
    while (hold && !failures) {
        os64_gui_event_t event;
        bool quit=false;
        int64_t wins[2]={window,other};
        for (unsigned i=0;i<2;++i) while (os64_gui_event_poll(wins[i],&event)>0) {
            if (event.type==OS64_GUI_EVENT_WINDOW_CLOSE) {
                ++close_requests;os64_printf("decorationtest: close request %u window=%u (kept open)\n",close_requests,i);
            }
            if (event.type==OS64_GUI_EVENT_MOUSE_BUTTON_DOWN) ++client_down;
            if (event.type==OS64_GUI_EVENT_MOUSE_BUTTON_UP) ++client_up;
            if (event.type==OS64_GUI_EVENT_WINDOW_RESIZE) paint(i?&second:&ctx,"Contents resized by the window manager.");
            if (event.type==OS64_GUI_EVENT_KEY_DOWN) {
                if (event.key.ascii=='q') quit=true;
                if (event.key.ascii=='d' && i==0) {
                    CHECK(os64_gui_window_destroy(window)==0);
                    window=os64_gui_window_create_content("Replacement after capture loss",160,180,500,240,0);
                    CHECK(window>0 && os64_draw_ctx_init(&ctx,window)==0);
                    if (window>0) paint(&ctx,"Destroyed the pressed window; release must be consumed.");
                    os64_printf("decorationtest: replaced window during input test\n");
                }
                if (event.key.ascii=='l' || event.key.ascii=='r') {
                    os64_draw_ctx_t *current=i?&second:&ctx;
                    CHECK(os64_draw_ctx_refresh(current)==0);
                    bool lock=event.key.ascii=='l';
                    CHECK(os64_gui_window_set_min_size(wins[i],lock?current->surf.width:64,
                        lock?current->surf.height:32)==0);
                    os64_printf("decorationtest: content minimum %s\n",lock?"fixed at current size":"released");
                }
                if (event.key.ascii=='v') {
                    os64_draw_ctx_t *current=i?&second:&ctx;
                    CHECK(os64_draw_ctx_refresh(current)==0);
                    CHECK(os64_gui_window_get_state(wins[i],&after)==0);
                    os64_printf("decorationtest: state frame=%d,%d %ux%u content=%ux%u flags=%u\n",
                        after.x,after.y,after.width,after.height,current->surf.width,current->surf.height,after.flags);
                    os64_printf("decorationtest: client edges down=%u up=%u close=%u layout=%u\n",
                        client_down,client_up,close_requests,composition);
                }
                if (event.key.ascii=='+' || event.key.ascii=='-' ||
                    (event.key.ascii>='1' && event.key.ascii<='5')) {
                    uint32_t previous=composition;
                    if (event.key.ascii>='1' && event.key.ascii<='5') composition=(unsigned)(event.key.ascii-'0');
                    uint32_t next=event.key.ascii=='+'?size+4:event.key.ascii=='-'?(size>12?size-4:size):size;
                    if (next>64) continue;
                    bundle=prepare(next,&length);
                    if (bundle) {
                        if (os64_decor_generation(&generation)==0 && os64_decor_apply(bundle,length,generation)==0) size=next;
                        else {composition=previous;os64_printf("decorationtest: Apply refused; old appearance retained\n");}
                        os64_free(bundle);
                    }
                }
            }
        }
        if (quit) break;
        os64_sleep(10);
    }
done:
    os64_gui_window_destroy(other);
    os64_gui_window_destroy(window);
    return failures?1:0;
}
