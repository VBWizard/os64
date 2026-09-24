#include "model.h"
#include "os64/str.h"
#include "os64/mem.h"

void frame_preset(frame_draft_t *d,unsigned preset)
{
    /* Presets replace the frame recipe; typography remains composition-owned. */
    os64_decor_defaults(&d->style);
    os64_decor_header_t *h=&d->style;
    h->strength=16;h->finish=OS64_DECOR_GRAIN;h->button_count=4;
    h->buttons[3]=(os64_decor_button_t){OS64_DECOR_PIN,0,OS64_DECOR_ROUND,0};
    if(preset==1) {
        h->active_face=0xffdeded8;h->inactive_face=0xffa9adae;
        h->active_text=0xff202630;h->inactive_text=0xff454a50;
        h->active_border=0xff303943;h->inactive_border=0xff798087;
        h->active_face2=0xff8e939a;h->inactive_face2=0xff707982;
        h->active_border2=0xff596573;h->inactive_border2=0xff454e58;
        h->finish=OS64_DECOR_STRIPES;h->strength=8;h->direction=1;
        h->button_count=3;os64_memset(&h->buttons[3],0,sizeof(h->buttons[3]));
        for(unsigned i=0;i<3;++i)h->buttons[i].shape=OS64_DECOR_BARE;
    } else if(preset==2) {
        h->active_face=0xff96968e;h->inactive_face=0xff777d79;
        h->active_face2=0xff74746c;h->inactive_face2=0xff555b57;
        h->active_text=0xfffcf7e8;h->inactive_text=0xffdedbcc;
        h->active_border=0xffd1bd89;h->inactive_border=0xff434942;
        h->active_border2=0xffb09c68;h->inactive_border2=0xff656b64;
        h->border=3;h->relief=2;h->finish=OS64_DECOR_GRADIENT;h->strength=18;
        h->button_size=28;
        for(unsigned i=0;i<h->button_count;++i)h->buttons[i].shape=OS64_DECOR_SQUARE;
    }
}
bool frame_same(const frame_draft_t *a,const frame_draft_t *b)
{
    if(a->font.size!=b->font.size)return false;
    for(unsigned i=0;i<3;++i)if(!os64_streq(a->font.face[i],b->font.face[i]))return false;
    const unsigned char *x=(const void *)&a->style,*y=(const void *)&b->style;
    for(size_t i=0;i<sizeof(a->style);++i)if(x[i]!=y[i])return false;
    return true;
}
