#include "os64/ui_icon.h"
bool os64_ui_icon_key_valid(const char *s)
{
    if(!s||!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z'))) return false;
    size_t n=0;
    for(;s[n]&&n<48;++n) if(!((s[n]>='a'&&s[n]<='z')||(s[n]>='A'&&s[n]<='Z')||
        (s[n]>='0'&&s[n]<='9')||s[n]=='-')) return false;
    return n>0&&n<48;
}
uint32_t os64_ui_icon_menu_size(int32_t h) { return h<=16?16:h<=24?24:32; }
os64_ui_icon_status_t os64_ui_icon_acquire(const os64_ui_icon_provider_t *p,
    const char *key,uint32_t size,os64_ui_icon_lease_t *out)
{
    if(!out) return OS64_UI_ICON_INVALID;
    *out=(os64_ui_icon_lease_t){0};
    if(!os64_ui_icon_key_valid(key)||!(size==16||size==24||size==32||size==48||size==64))
        return OS64_UI_ICON_INVALID;
    if(!p) return OS64_UI_ICON_MISSING;
    if(!p->acquire||!p->release) return OS64_UI_ICON_INVALID;
    os64_ui_icon_lease_t lease={0};
    os64_ui_icon_status_t rc=p->acquire(p->user,key,size,&lease);
    if(rc!=OS64_UI_ICON_OK) return rc;
    if(!lease.pixels||!lease.width||!lease.height||lease.width>size||lease.height>size||
        lease.pitch<lease.width||lease.pitch>64) {
        p->release(p->user,&lease); return OS64_UI_ICON_MALFORMED;
    }
    *out=lease; return rc;
}
void os64_ui_icon_release(const os64_ui_icon_provider_t *p,os64_ui_icon_lease_t *l)
{
    if(p&&p->release&&l&&l->pixels) p->release(p->user,l);
    if(l) *l=(os64_ui_icon_lease_t){0};
}
void os64_ui_icon_draw(os64_gui_surface_t *dst,os64_gui_rect_t slot,
    const os64_ui_icon_lease_t *l,bool disabled)
{
    if(!dst||!l||!l->pixels||!l->width||!l->height||l->width>64||l->height>64||
        l->pitch<l->width||l->pitch>64) return;
    os64_gui_rect_t clip;
    if(!os64_rect_intersect(slot,(os64_gui_rect_t){0,0,(int32_t)dst->width,(int32_t)dst->height},&clip)) return;
    int64_t x=(int64_t)slot.x+(slot.w-(int64_t)l->width)/2;
    int64_t y=(int64_t)slot.y+(slot.h-(int64_t)l->height)/2;
    uint32_t row[64];
    for(uint32_t j=0;j<l->height;++j) {
        if(y+j<clip.y||y+j>=(int64_t)clip.y+clip.h) continue;
        for(uint32_t i=0;i<l->width;++i) {
            uint32_t c=l->pixels[j*l->pitch+i];
            if(disabled) c=(c&0xffffffu)|((((c>>24)+1)/2)<<24);
            row[i]=c;
        }
        int64_t lo=x>clip.x?x:clip.x,hi=x+l->width;
        if(hi>(int64_t)clip.x+clip.w) hi=(int64_t)clip.x+clip.w;
        if(hi>lo) os64_draw_blend(dst,(int32_t)lo,(int32_t)(y+j),row+(lo-x),(uint32_t)(hi-lo),1,(uint32_t)(hi-lo));
    }
}
