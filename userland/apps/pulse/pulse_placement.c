#include "pulse_placement.h"
#include "os64/os64.h"
#include "os64/conf.h"
static void apply(pulse_config_t *c,unsigned mode,const os64_gui_event_t *e)
{
    unsigned mask=e->placement.changed;
    if(mask&OS64_GUI_PLACEMENT_X)c->x=e->placement.x;
    if(mask&OS64_GUI_PLACEMENT_Y)c->y=e->placement.y;
    if(mask&OS64_GUI_PLACEMENT_WIDTH)c->size[mode].w=e->placement.width;
    if(mask&OS64_GUI_PLACEMENT_HEIGHT)c->size[mode].h=e->placement.height;
}
static bool valid_line(const char *key,const char *value,void *user)
{(void)value;if(!key)*(bool *)user=false;return key!=NULL;}
static bool valid_config(const char *text,size_t length,void *user)
{(void)user;bool valid=true;return os64_conf_parse(text,length,valid_line,&valid)>=0 && valid;}
int64_t pulse_record_placement(pulse_config_t *c,const os64_gui_event_t *e)
{
    if(e->type!=OS64_GUI_EVENT_WINDOW_PLACEMENT)return 0;
    unsigned mask=e->placement.changed & OS64_GUI_PLACEMENT_ALL;
    if(!mask)return 0;
    if(((mask&OS64_GUI_PLACEMENT_WIDTH) && (!e->placement.width || e->placement.width>32767)) ||
       ((mask&OS64_GUI_PLACEMENT_HEIGHT) && (!e->placement.height || e->placement.height>32767)))return OS64_CONF_BAD_SETTING;
    unsigned mode=pulse_presentation(c);apply(c,mode,e);
    pulse_config_t saved;pulse_config_defaults(&saved);
    int64_t rc=os64_conf_find_read("pulse.conf",pulse_config_line,&saved,NULL,0);
    if(rc<0 && rc!=OS64_CONF_NO_FILE)return rc;
    apply(&saved,mode,e);
    char geometry[96],size[48];os64_conf_pair_t pairs[2];unsigned n=0;
    if(mode==PULSE_CLASSIC_SIZE || (mask&(OS64_GUI_PLACEMENT_X|OS64_GUI_PLACEMENT_Y))){
        os64_snprintf(geometry,sizeof(geometry),"1,%d,%d,%u,%u",saved.x,saved.y,saved.size[0].w,saved.size[0].h);
        pairs[n++]=(os64_conf_pair_t){"geometry",geometry};
    }
    if(mode!=PULSE_CLASSIC_SIZE && (mask&(OS64_GUI_PLACEMENT_WIDTH|OS64_GUI_PLACEMENT_HEIGHT))){
        os64_snprintf(size,sizeof(size),"%u,%u",saved.size[mode].w,saved.size[mode].h);
        pairs[n++]=(os64_conf_pair_t){mode==PULSE_COMPACT_SIZE?"instruments_compact_size":"instruments_size",size};
    }
    return os64_conf_write_checked("pulse.conf",pairs,n,valid_config,NULL);
}
