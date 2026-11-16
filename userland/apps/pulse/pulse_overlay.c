#include "pulse_overlay.h"
#include "os64/os64.h"
void pulse_overlay_init(pulse_overlay_t *p)
{*p=(pulse_overlay_t){.target=-1,.shown=-1};}
void pulse_overlay_cancel(pulse_overlay_t *p)
{p->target=p->shown=-1;p->deadline=0;p->keyboard=false;}
void pulse_overlay_release(pulse_overlay_t *p)
{for(unsigned i=0;i<10;++i){os64_ui_run_release(p->runs[i]);p->runs[i]=NULL;}}
void pulse_overlay_hover(pulse_overlay_t *p,int target,int64_t now)
{
    if(target==p->target && !p->keyboard)return;
    pulse_overlay_cancel(p);p->target=target;
    if(target>=0 && now>=0 && now<=INT64_MAX-500000)p->deadline=now+500000;
}
void pulse_overlay_focus(pulse_overlay_t *p,int target)
{pulse_overlay_cancel(p);p->shown=target;p->keyboard=target>=0;}
bool pulse_overlay_due(pulse_overlay_t *p,int64_t now)
{
    if(p->target<0 || !p->deadline || now<p->deadline)return false;
    p->shown=p->target;p->deadline=0;return true;
}
bool pulse_overlay_paint(pulse_overlay_t *p,os64_ui_t *ui,os64_gui_surface_t *surface,
    os64_gui_rect_t bounds,os64_gui_rect_t anchor,const char *text)
{
    int32_t row=os64_ui_font_row_height(ui,OS64_FONT_ROLE_DOCUMENT);
    if(row<1 || bounds.w<32 || bounds.h<row+16)return false;
    int32_t width=bounds.w-8;if(width>440)width=440;
    int32_t limit=width-16;unsigned maximum=(unsigned)((bounds.h-24)/row);
    if(maximum>10)maximum=10;
    if(!maximum)return false;
    char lines[10][256];unsigned count=0;const char *s=text;
    while(*s && count<maximum){
        size_t n=0,last_space=0;int32_t measured;
        while(s[n] && s[n]!='\n' && n<sizeof(lines[0])-4){
            size_t next=n+1;
            while(((unsigned char)s[next]&0xc0)==0x80)++next;
            if(next>sizeof(lines[0])-4)break;
            if(os64_ui_text_measure(ui,OS64_FONT_ROLE_DOCUMENT,s,next,&measured))return false;
            if(measured>limit)break;
            if(s[n]==' ')last_space=n;
            n=next;
        }
        if(!n && *s!='\n')return false;
        if(s[n] && s[n]!='\n' && last_space)n=last_space;
        os64_memcpy(lines[count],s,n);lines[count][n]=0;s+=n;
        while(*s==' ')++s;
        if(*s=='\n')++s;
        if(count+1==maximum && *s){
            for(;;){
                os64_memcpy(lines[count]+n,"...",4);
                if(os64_ui_text_measure(ui,OS64_FONT_ROLE_DOCUMENT,lines[count],n+3,&measured))return false;
                if(measured<=limit)break;
                if(!n)return false;
                --n;
                while(n && ((unsigned char)lines[count][n]&0xc0)==0x80)--n;
            }
        }
        ++count;
    }
    int32_t height=(int32_t)count*row+16;
    os64_gui_rect_t box={anchor.x,anchor.y+anchor.h+6,width,height};
    if(box.x<bounds.x+4)box.x=bounds.x+4;
    if(box.x+width>bounds.x+bounds.w-4)box.x=bounds.x+bounds.w-width-4;
    if(box.y+height>bounds.y+bounds.h-4)box.y=anchor.y-height-6;
    if(box.y<bounds.y+4)box.y=bounds.y+4;
    const uint32_t bg=0xff182738,fg=0xffedf5ff;
    os64_draw_fill_rect(surface,box,bg);os64_draw_rect(surface,box,0xff87b9e9);
    os64_gui_rect_t clip={box.x+8,box.y+8,limit,height-16};
    for(unsigned i=0;i<count;++i)
        if(os64_ui_draw_text(ui,&p->runs[i],OS64_FONT_ROLE_DOCUMENT,surface,clip,
            clip.x,clip.y+(int32_t)i*row,lines[i],os64_strlen(lines[i]),fg,bg)<0)return false;
    for(unsigned i=count;i<10;++i){os64_ui_run_release(p->runs[i]);p->runs[i]=NULL;}
    return true;
}
