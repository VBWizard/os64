#include "pulse_instruments.h"
#include "os64/geometry_backend.h"
#include "os64/os64.h"
#include <limits.h>

#define DRAW_BUDGET (8u * 1024u * 1024u)
#define TILE_W 128u
#define TILE_H 64u
#define BG 0xff0c1422u
#define FACE 0xff131f30u
#define TRACK 0xff2b3b50u
#define INK 0xffedf4ffu
#define MUTED 0xffa9bad0u
#define CAPTION OS64_FONT_ROLE_DOCUMENT
static const uint32_t colors[PULSE_DIAL_COUNT]={0xff51c9efu,0xffab98ffu,0xffedb668u,0xff409bffu,0xffed6868u};
static const char *const labels[PULSE_DIAL_COUNT]={"CPU","RAM","DISK","DOWNLOAD","UPLOAD"};
static const os64_geometry_map_t identity={1,0,0,1,0,0};
typedef struct {uint32_t *pixels;int32_t w,h;bool compact;} dial_cache_t;
struct pulse_instruments {
    os64_ui_t *ui;
    const os64_geometry_backend_t *api;
    os64_geometry_context_t *geometry;
    size_t live,peak;
    bool cache_enabled;
    uint8_t *coverage;
    uint32_t *rgba;
    dial_cache_t faces[PULSE_DIAL_COUNT];
    void *text[PULSE_DIAL_COUNT][4],*scale_run;
};
static void *reserve(pulse_instruments_t *p,size_t bytes)
{
    if(bytes>DRAW_BUDGET-p->live)return NULL;
    void *q=os64_malloc(bytes);
    if(q){p->live+=bytes;if(p->live>p->peak)p->peak=p->live;}
    return q;
}
static void give_back(pulse_instruments_t *p,void *q,size_t bytes)
{if(q){os64_free(q);p->live-=bytes;}}
static os64_geometry_status_t geometry_alloc(void *u,size_t bytes,void **out)
{
    pulse_instruments_t *p=u;*out=NULL;
    if(bytes>DRAW_BUDGET-p->live)return OS64_GEOMETRY_LIMIT;
    *out=reserve(p,bytes);return *out?OS64_GEOMETRY_OK:OS64_GEOMETRY_NO_MEMORY;
}
static void geometry_free(void *u,void *q,size_t bytes){give_back(u,q,bytes);}
static void clear_faces(pulse_instruments_t *p)
{
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i){
        dial_cache_t *c=&p->faces[i];give_back(p,c->pixels,(size_t)c->w*c->h*4);*c=(dial_cache_t){0};
    }
}
void pulse_instruments_invalidate(pulse_instruments_t *p)
{
    if(!p)return;
    clear_faces(p);
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i)for(unsigned j=0;j<4;++j){
        os64_ui_run_release(p->text[i][j]);p->text[i][j]=NULL;
    }
    os64_ui_run_release(p->scale_run);p->scale_run=NULL;
}
pulse_instruments_t *pulse_instruments_create(os64_ui_t *ui)
{
    if(!ui)return NULL;
    pulse_instruments_t *p=os64_malloc(sizeof(*p));if(!p)return NULL;
    *p=(pulse_instruments_t){.ui=ui,.api=os64_freetype_geometry_v1(),.cache_enabled=true,.live=sizeof(*p),.peak=sizeof(*p)};
    if(!p->api || p->api->revision!=OS64_GEOMETRY_BACKEND_REVISION)goto fail;
    os64_geometry_options_t options={.memory={p,geometry_alloc,geometry_free},.max_work=OS64_GEOMETRY_WORK_DEFAULT};
    if(p->api->create(&options,&p->geometry))goto fail;
    p->coverage=reserve(p,TILE_W*TILE_H);p->rgba=reserve(p,TILE_W*TILE_H*4);
    if(!p->coverage || !p->rgba)goto fail;
    return p;
fail:
    pulse_instruments_destroy(p);return NULL;
}
void pulse_instruments_destroy(pulse_instruments_t *p)
{
    if(!p)return;
    pulse_instruments_invalidate(p);
    give_back(p,p->coverage,TILE_W*TILE_H);give_back(p,p->rgba,TILE_W*TILE_H*4);
    if(p->geometry)(void)p->api->destroy(p->geometry);
    os64_free(p);
}
void pulse_instruments_cache(pulse_instruments_t *p,bool enabled)
{if(p){p->cache_enabled=enabled;if(!enabled)clear_faces(p);}}
size_t pulse_instruments_live_bytes(const pulse_instruments_t *p){return p?p->live:0;}
size_t pulse_instruments_peak_bytes(const pulse_instruments_t *p){return p?p->peak:0;}
static bool safe_rect(os64_gui_rect_t r)
{return r.x>=0 && r.y>=0 && r.w>0 && r.h>0 && r.x<=INT32_MAX-r.w && r.y<=INT32_MAX-r.h;}
static bool inside(os64_gui_rect_t r,int32_t x,int32_t y)
{return x>=r.x && y>=r.y && (int64_t)x< (int64_t)r.x+r.w && (int64_t)y<(int64_t)r.y+r.h;}
static int32_t largest(int32_t a,int32_t b){return a>b?a:b;}
static int32_t smallest(int32_t a,int32_t b){return a<b?a:b;}
typedef struct {
    int32_t primary_row,caption_row,percent_w,network_w,cell_w,cell_h;
} instrument_metrics_t;
static bool measure(os64_ui_t *ui,instrument_metrics_t *out)
{
    if(!ui || !out)return false;
    instrument_metrics_t m={.primary_row=os64_ui_font_row_height(ui,OS64_FONT_ROLE_UI),
        .caption_row=os64_ui_font_row_height(ui,CAPTION)};
    if(m.primary_row<=0 || m.caption_row<=0)return false;
    int32_t number,label,rate;
    if(os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,"100%",4,&number) ||
       os64_ui_text_measure(ui,CAPTION,"DOWNLOAD",8,&label) ||
       os64_ui_text_measure(ui,OS64_FONT_ROLE_UI,"9.9e20",6,&rate))return false;
    int32_t unit=0;
    for(unsigned i=0;i<PULSE_UNITS;++i){int32_t w;
        if(os64_ui_text_measure(ui,CAPTION,pulse_unit_names[i],os64_strlen(pulse_unit_names[i]),&w))return false;
        unit=largest(unit,w);
    }
    m.percent_w=largest(number+12,88);
    m.network_w=largest(largest(rate,unit),label)+12;
    m.cell_w=largest(2*m.network_w+24,220);
    m.cell_h=110+m.primary_row+3*m.caption_row+24;
    *out=m;return true;
}
static int32_t compact_row_height(const instrument_metrics_t *m,bool network)
{return 60+m->primary_row+m->caption_row+(network?m->caption_row:0);}
bool pulse_instruments_min_size(os64_ui_t *ui,const pulse_config_t *c,bool compact,
                                os64_gui_rect_t *out)
{
    instrument_metrics_t m;
    if(!c || !out || !measure(ui,&m))return false;
    unsigned top=0;for(unsigned i=0;i<3;++i)if(c->show[i])++top;
    bool net=c->show[PULSE_NET];unsigned count=top+net;
    os64_gui_rect_t size={0,0,64,32};
    if(count && compact){
        unsigned rows=(top?1:0)+net;
        if(top)size.w=largest(size.w,(int32_t)top*m.percent_w+((int32_t)top+1)*8);
        if(net)size.w=largest(size.w,2*m.network_w+24);
        size.h=(int32_t)rows*compact_row_height(&m,net)+((int32_t)rows+1)*8+(net?m.caption_row+8:0);
    }else if(count){
        // Both dimensions can reach their floors together. Reserve two
        // columns for multiple cards; the renderer's
        // single-column layout remains available to taller surfaces.
        unsigned cols=count>1?2:1,rows=(count+cols-1)/cols;
        size.w=(int32_t)cols*m.cell_w+((int32_t)cols+1)*8;
        size.h=(int32_t)rows*m.cell_h+((int32_t)rows+1)*8;
    }
    *out=size;return true;
}
bool pulse_instruments_layout(os64_ui_t *ui,const pulse_config_t *c,bool compact,
                              os64_gui_rect_t b,pulse_instrument_layout_t *out)
{
    instrument_metrics_t m;
    if(!c || !out || !safe_rect(b) || !measure(ui,&m))return false;
    pulse_instrument_layout_t l={.bounds=b,.compact=compact,
        .primary_row=m.primary_row,.caption_row=m.caption_row};
    int32_t percent_w=m.percent_w,network_w=m.network_w;
    unsigned count=0;for(unsigned i=0;i<PULSE_METERS;++i)if(c->show[i])++count;
    if(!count){*out=l;return true;}
    int32_t gap=8,footer=c->show[PULSE_NET]?l.caption_row+gap:0;
    if(compact){
        unsigned count_top=0;for(unsigned i=0;i<3;++i)if(c->show[i])++count_top;
        unsigned rows=(count_top?1:0)+(c->show[PULSE_NET]?1:0);
        int32_t available=b.h-footer-(int32_t)(rows+1)*gap;
        if(available<=0)return false;
        int32_t height=available/(int32_t)rows;
        int32_t min_h=compact_row_height(&m,c->show[PULSE_NET]);
        if(height<min_h)return false;
        unsigned slot=0;
        for(unsigned i=0;i<3;++i)if(c->show[i]){
            int32_t width=(b.w-((int32_t)count_top+1)*gap)/(int32_t)count_top;
            if(width<percent_w)return false;
            l.dials[i]=(os64_gui_rect_t){b.x+gap+(int32_t)slot*(width+gap),b.y+gap,width,height};
            l.visible[i]=true;++slot;
        }
        if(c->show[PULSE_NET]){
            int32_t width=(b.w-3*gap)/2;if(width<network_w)return false;
            for(unsigned n=0;n<2;++n){unsigned i=PULSE_DIAL_DOWN+n;
                l.dials[i]=(os64_gui_rect_t){b.x+gap+(int32_t)n*(width+gap),b.y+gap+(count_top?height+gap:0),width,height};l.visible[i]=true;
            }
        }
    }else{
        int32_t cell_w=m.cell_w;
        unsigned cols=b.w>=2*cell_w+3*gap && count>1?2:1;
        unsigned rows=(count+cols-1)/cols;
        int32_t width=(b.w-(int32_t)(cols+1)*gap)/(int32_t)cols;
        int32_t height=(b.h-(int32_t)(rows+1)*gap)/(int32_t)rows;
        if(width<cell_w || height<m.cell_h)return false;
        unsigned slot=0;
        for(unsigned i=0;i<PULSE_METERS;++i)if(c->show[i]){
            os64_gui_rect_t card={b.x+gap+(int32_t)(slot%cols)*(width+gap),b.y+gap+(int32_t)(slot/cols)*(height+gap),width,height};
            l.cards[i]=card;++slot;
            if(i<PULSE_NET){l.dials[i]=card;l.visible[i]=true;}
            else for(unsigned n=0;n<2;++n){unsigned d=PULSE_DIAL_DOWN+n;
                l.dials[d]=(os64_gui_rect_t){card.x+8+(int32_t)n*(width/2-4),card.y+l.caption_row+12,width/2-12,height-2*l.caption_row-20};l.visible[d]=true;
            }
        }
    }
    if(c->show[PULSE_NET]){
        os64_gui_rect_t net=compact?b:l.cards[PULSE_NET];
        l.scale=(os64_gui_rect_t){net.x+8,net.y+net.h-l.caption_row-8,net.w-16,l.caption_row};
    }
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i)if(l.visible[i]){
        os64_gui_rect_t r=l.dials[i];
        if(!safe_rect(r) || !inside(b,r.x,r.y) || !inside(b,r.x+r.w-1,r.y+r.h-1))return false;
    }
    *out=l;return true;
}
int pulse_instruments_hit(const pulse_instrument_layout_t *l,int32_t x,int32_t y)
{
    if(!l)return -1;
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i)if(l->visible[i] && inside(l->dials[i],x,y))return (int)i;
    return -1;
}
// A 270-degree sweep. Range-reduced sine avoids a runtime math dependency;
// the degree-15 polynomial's error is below a pixel at supported window sizes.
static double sine(double x)
{
    const double pi=3.14159265358979323846;
    while(x>pi)x-=2*pi;
    while(x< -pi)x+=2*pi;
    double term=x,sum=x,xx=x*x;
    for(unsigned n=1;n<=7;++n){term*= -xx/((2*n)*(2*n+1));sum+=term;}
    return sum;
}
static os64_geometry_point_t point(double x,double y,double r,double fraction)
{
    const double pi=3.14159265358979323846;
    double a=(135+270*fraction)*pi/180;
    return (os64_geometry_point_t){x+r*sine(a+pi/2),y+r*sine(a)};
}
static os64_geometry_command_t command(os64_geometry_verb_t verb,os64_geometry_point_t p)
{return (os64_geometry_command_t){.verb=verb,.p={p}};}
static bool stroke(pulse_instruments_t *p,os64_gui_surface_t *dst,os64_gui_rect_t clip,
                   const os64_geometry_command_t *cmd,size_t count,double width,uint32_t color)
{
    os64_gui_rect_t screen={0,0,(int32_t)dst->width,(int32_t)dst->height};
    if(!os64_rect_intersect(clip,screen,&clip))return true;
    os64_geometry_stroke_t style={width,4,OS64_GEOMETRY_ROUND_CAP,OS64_GEOMETRY_ROUND_JOIN};
    os64_geometry_outline_t *outline=NULL;
    if(p->api->stroke(p->geometry,cmd,count,&identity,1.0/64,&style,&outline))return false;
    bool ok=true;
    for(int32_t y=clip.y;ok && y<clip.y+clip.h;){
        int32_t h=smallest(TILE_H,clip.y+clip.h-y);
        for(int32_t x=clip.x;ok && x<clip.x+clip.w;){
            int32_t w=smallest(TILE_W,clip.x+clip.w-x);size_t n=(size_t)w*h;
            os64_memset(p->coverage,0,n);
            os64_geometry_target_t target={x,y,(uint32_t)w,(uint32_t)h,(size_t)w,n,p->coverage,false};
            if(p->api->raster(p->geometry,outline,&identity,&target)){ok=false;break;}
            for(size_t i=0;i<n;++i)p->rgba[i]=(color&0x00ffffffu)|((uint32_t)p->coverage[i]<<24);
            os64_draw_blend(dst,x,y,p->rgba,(uint32_t)w,(uint32_t)h,(size_t)w);
            x+=w;
        }
        y+=h;
    }
    (void)p->api->release(p->geometry,outline);return ok;
}
static bool arc(pulse_instruments_t *p,os64_gui_surface_t *dst,os64_gui_rect_t clip,
                double x,double y,double radius,double fraction,double width,uint32_t color)
{
    if(fraction<=0)return true;
    if(fraction>1)fraction=1;
    os64_geometry_command_t path[101];unsigned segments=100;
    for(unsigned i=0;i<=segments;++i)path[i]=command(i?OS64_GEOMETRY_LINE:OS64_GEOMETRY_MOVE,point(x,y,radius,fraction*i/segments));
    return stroke(p,dst,clip,path,segments+1,width,color);
}
static bool rule(pulse_instruments_t *p,os64_gui_surface_t *dst,os64_gui_rect_t clip,
                 os64_geometry_point_t a,os64_geometry_point_t b,double width,uint32_t color)
{
    os64_geometry_command_t cmd[]={command(OS64_GEOMETRY_MOVE,a),command(OS64_GEOMETRY_LINE,b)};
    return stroke(p,dst,clip,cmd,2,width,color);
}
typedef struct {double x,y,r;int32_t value_y,detail_y;} face_geometry_t;
static face_geometry_t face_geometry(os64_gui_rect_t b,const pulse_instrument_layout_t *l,unsigned i)
{
    bool net=i>=PULSE_DIAL_DOWN;
    int32_t bottom=l->compact?(net?l->caption_row:0):(!net?2*l->caption_row+8:l->caption_row);
    double r=smallest((b.w-16)/2,(int32_t)((b.h-l->caption_row-8-l->primary_row-bottom)/1.8));
    if(r<12)r=12;
    double cy=b.y+l->caption_row+8+r;
    return (face_geometry_t){b.x+b.w/2.0,cy,r,(int32_t)(cy+r*.45),b.y+b.h-bottom};
}
static bool static_face(pulse_instruments_t *p,os64_gui_surface_t *dst,os64_gui_rect_t b,
                        const pulse_instrument_layout_t *l,unsigned i)
{
    face_geometry_t f=face_geometry(b,l,i);
    if(!arc(p,dst,b,f.x,f.y,f.r,1,l->compact?5:8,TRACK))return false;
    os64_geometry_command_t ticks[42];
    for(unsigned n=0;n<=20;++n){double t=n/20.0;
        ticks[2*n]=command(OS64_GEOMETRY_MOVE,point(f.x,f.y,f.r-9,t));
        ticks[2*n+1]=command(OS64_GEOMETRY_LINE,point(f.x,f.y,f.r-(n%5?12:17),t));
    }
    return stroke(p,dst,b,ticks,42,1,0xff8fa2b9u);
}
static bool background(pulse_instruments_t *p,os64_gui_surface_t *dst,os64_gui_rect_t b,
                       const pulse_instrument_layout_t *l,unsigned i)
{
    dial_cache_t *c=&p->faces[i];
    if(c->w!=b.w || c->h!=b.h || c->compact!=l->compact){
        give_back(p,c->pixels,(size_t)c->w*c->h*4);*c=(dial_cache_t){0};
    }
    if(!c->pixels && p->cache_enabled){
        size_t bytes=(size_t)b.w*b.h*4;
        // Leave headroom for transient geometry allocations on an uncached pass.
        if(bytes<=DRAW_BUDGET/2 && p->live+bytes<DRAW_BUDGET*3/4){
            c->pixels=reserve(p,bytes);
            if(c->pixels){
                c->w=b.w;c->h=b.h;c->compact=l->compact;
                os64_gui_surface_t face={.pixels=c->pixels,.width=(uint32_t)b.w,.height=(uint32_t)b.h,.pitch_px=(uint32_t)b.w};
                os64_gui_rect_t box={0,0,b.w,b.h};os64_draw_fill_rect(&face,box,l->compact?BG:FACE);
                if(!static_face(p,&face,box,l,i)){
                    give_back(p,c->pixels,bytes);*c=(dial_cache_t){0};return false;
                }
            }
        }
    }
    if(c->pixels){os64_draw_blit(dst,b.x,b.y,c->pixels,(uint32_t)b.w,(uint32_t)b.h,(size_t)b.w);return true;}
    os64_draw_fill_rect(dst,b,l->compact?BG:FACE);return static_face(p,dst,b,l,i);
}
static bool centered(pulse_instruments_t *p,os64_gui_surface_t *dst,os64_gui_rect_t b,
                     int32_t y,const char *text,os64_font_role_t role,uint32_t color,uint32_t bg,void **run)
{
    int32_t width;
    if(os64_ui_run_width(p->ui,run,role,text,os64_strlen(text),&width))return false;
    if(width>b.w)return false;
    (void)os64_ui_draw_run(p->ui,*run,role,dst,b,b.x+(b.w-width)/2,y,text,os64_strlen(text),color,bg);
    return true;
}
static double fraction(uint64_t value,uint64_t max)
{return !max?0:value>=max?1:(double)value/(double)max;}
static void short_rate(char *out,size_t cap,uint64_t rate,unsigned units)
{
    char full[64];pulse_format_rate(full,sizeof(full),rate,units);
    char *space=full;while(*space && *space!=' ')++space;*space=0;
    if(full[0]>='0' && full[0]<='9' && os64_strlen(full)<=5){os64_strcopy(out,cap,full);return;}
    // Scientific notation keeps the selected unit and two significant digits.
    // Conversion through double also handles bit/s values beyond uint64_t.
    static const double divisor[PULSE_UNITS]={1,1024,1048576,1,1000,1000000};
    double number=(double)rate/divisor[units]*(units>=PULSE_BITS?8:1);
    unsigned exponent=0;
    while(number>=10){number/=10;++exponent;}
    unsigned whole=(unsigned)number,tenth=(unsigned)((number-whole)*10);
    os64_snprintf(out,cap,"%u.%ue%u",whole,tenth,exponent);
}
bool pulse_instruments_paint(pulse_instruments_t *p,os64_gui_surface_t *dst,
                            const pulse_instrument_layout_t *l,const pulse_config_t *config,
                            const pulse_instrument_readings_t *readings)
{
    if(!p || !dst || !dst->pixels || !l || !config || !readings || !safe_rect(l->bounds) ||
       dst->width>INT32_MAX || dst->height>INT32_MAX || dst->pitch_px<dst->width)return false;
    if(l->bounds.x+l->bounds.w>(int32_t)dst->width || l->bounds.y+l->bounds.h>(int32_t)dst->height)return false;
    const pulse_sample_t *s=&readings->current;
    bool valid[]={s->cpu_valid,s->ram_valid,s->disk_valid,s->net_valid,s->net_valid};
    uint64_t value[]={s->cpu_percent,pulse_percent(s->ram_used,s->ram_total),pulse_percent(s->disk_used,s->disk_total),s->download,s->upload};
    unsigned units=config->units<PULSE_UNITS?config->units:PULSE_BYTES;
    os64_draw_fill_rect(dst,l->bounds,BG);
    for(unsigned i=0;i<PULSE_METERS;++i)if(!l->compact && l->cards[i].w){
        os64_draw_fill_round_rect(dst,l->cards[i],10,FACE);
        os64_draw_round_rect(dst,l->cards[i],10,TRACK,TRACK);
    }
    for(unsigned i=0;i<PULSE_DIAL_COUNT;++i){
        if(!l->visible[i]){dial_cache_t *c=&p->faces[i];give_back(p,c->pixels,(size_t)c->w*c->h*4);*c=(dial_cache_t){0};continue;}
        os64_gui_rect_t b=l->dials[i];
        // Inset card-sized dials so the cached rectangle preserves rounded chrome.
        if(!l->compact && i<PULSE_DIAL_DOWN){b.x+=8;b.y+=8;b.w-=16;b.h-=16;}
        if(!background(p,dst,b,l,i))return false;
        face_geometry_t f=face_geometry(b,l,i);uint64_t max=i<PULSE_DIAL_DOWN?100:s->network_scale;
        uint32_t bg=l->compact?BG:FACE;
        if(valid[i] && max){
            double v=fraction(value[i],max);
            if(!arc(p,dst,b,f.x,f.y,f.r,v,l->compact?5:8,colors[i]))return false;
            if(i==PULSE_DIAL_CPU || i>=PULSE_DIAL_DOWN){
                if(!rule(p,dst,b,(os64_geometry_point_t){f.x,f.y},point(f.x,f.y,f.r-20,v),2.5,INK))return false;
                if(!arc(p,dst,b,f.x,f.y,3,1,5,INK))return false;
            }
        }
        if(i!=PULSE_DIAL_DISK && readings->peak_valid[i] && max){
            double peak=fraction(readings->peak[i],max);
            if(!rule(p,dst,b,point(f.x,f.y,f.r-5,peak),point(f.x,f.y,f.r+5,peak),2.5,INK))return false;
        }
        char main[40],first[64]="",second[64]="";
        if(!valid[i])os64_strcopy(main,sizeof(main),"--");
        else if(i<PULSE_DIAL_DOWN)os64_snprintf(main,sizeof(main),"%lu%%",value[i]);
        else short_rate(main,sizeof(main),value[i],units);
        if(i>=PULSE_DIAL_DOWN)os64_strcopy(first,sizeof(first),pulse_unit_names[units]);
        else if(!l->compact){
            if(i==PULSE_DIAL_CPU)os64_strcopy(first,sizeof(first),valid[i]?"Overall utilization":"Waiting for sample");
            else if(valid[i]){
                char a[32],btext[32];os64_format_binary_size(i==PULSE_DIAL_RAM?s->ram_used:s->disk_used,a,sizeof(a));
                os64_format_binary_size(i==PULSE_DIAL_RAM?s->ram_available:s->disk_available,btext,sizeof(btext));
                os64_snprintf(first,sizeof(first),"%s used",a);os64_snprintf(second,sizeof(second),"%s available",btext);
            }else os64_strcopy(first,sizeof(first),"Unavailable");
        }
        if(!centered(p,dst,b,b.y,labels[i],CAPTION,colors[i],bg,&p->text[i][0]) ||
           !centered(p,dst,b,f.value_y,main,OS64_FONT_ROLE_UI,valid[i]?INK:MUTED,bg,&p->text[i][1]))return false;
        if(*first && !centered(p,dst,b,f.detail_y,first,CAPTION,MUTED,bg,&p->text[i][2]))return false;
        if(*second && !centered(p,dst,b,f.detail_y+l->caption_row,second,CAPTION,MUTED,bg,&p->text[i][3]))return false;
    }
    if(l->scale.w){
        char rate[64],label[96];unsigned scale_units=units;
        short_rate(rate,sizeof(rate),s->network_scale,scale_units);
        if(!os64_strcmp(rate,"0.0") && s->network_scale){
            scale_units=units>=PULSE_BITS?PULSE_KBIT:PULSE_KIB;
            short_rate(rate,sizeof(rate),s->network_scale,scale_units);
        }
        os64_snprintf(label,sizeof(label),"Scale: %s %s",rate,pulse_unit_names[scale_units]);
        if(!centered(p,dst,l->scale,l->scale.y,label,CAPTION,MUTED,l->compact?BG:FACE,&p->scale_run))return false;
    }
    return os64_ui_font_status(p->ui)==OS64_FONT_OK;
}
