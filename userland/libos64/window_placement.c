#include "os64/window_placement.h"
#include "os64/conf.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/str.h"
#include "os64/fmt.h"

static bool number(const char **p, bool signed_value, int64_t *out)
{
    const char *s=*p;
    bool negative=signed_value && *s=='-';
    if (negative) ++s;
    if (*s<'0' || *s>'9') return false;
    uint64_t value=0, limit=signed_value ? (uint64_t)INT32_MAX+negative : OS64_WINDOW_PREFERENCE_MAX;
    while (*s>='0' && *s<='9') {
        unsigned digit=(unsigned)(*s++-'0');
        if (value>(limit-digit)/10) return false;
        value=value*10+digit;
    }
    *out=negative ? -(int64_t)value : (int64_t)value;
    *p=s;
    return true;
}
static bool parse_geometry(const char *s, os64_window_remember_mode_t mode,
    os64_window_geometry_t *out)
{
    os64_window_geometry_t g=*out;
    int64_t x,y,w,h;
    if (mode==OS64_WINDOW_REMEMBER_POSITION_AND_SIZE) {
        if (s[0]!='1' || s[1]!=',') return false;
        s+=2;
        if (!number(&s,true,&x) || *s++!=',' || !number(&s,true,&y) || *s++!=',') return false;
        g.x=(int32_t)x; g.y=(int32_t)y;
    }
    if (!number(&s,false,&w) || *s++!=',' || !number(&s,false,&h) || *s || !w || !h) return false;
    g.width=(uint32_t)w; g.height=(uint32_t)h;
    *out=g;
    return true;
}
typedef struct {
    os64_window_remember_mode_t mode;
    os64_window_geometry_t fallback, value;
    bool syntax_ok;
} placement_parse_t;
static bool setting(const char *key, const char *value, void *user)
{
    placement_parse_t *p=user;
    if (!key) { p->syntax_ok=false; return false; }
    if (os64_streq_nocase(key,p->mode==OS64_WINDOW_REMEMBER_SIZE ? "size" : "geometry")) {
        p->value=p->fallback; // a later invalid duplicate must not revive an earlier value
        (void)parse_geometry(value,p->mode,&p->value);
    }
    return true;
}
static bool validate(const char *text, size_t length, void *user)
{
    (void)user;
    placement_parse_t p={.syntax_ok=true};
    return os64_conf_parse(text,length,setting,&p)>=0 && p.syntax_ok;
}
static int64_t read_preference(const os64_window_placement_t *p,
    os64_window_geometry_t *g, char source[OS64_CONF_PATH_MAX])
{
    if (os64_conf_find(p->config,source,OS64_CONF_PATH_MAX)!=0) return OS64_CONF_NO_FILE;
    uint8_t *bytes=NULL; size_t length=0;
    os64_slurp_status_t rc=os64_slurp(source,OS64_CONF_MAX-1,&bytes,&length);
    if (rc!=OS64_SLURP_OK) return rc==OS64_SLURP_TOO_BIG ? OS64_CONF_TRUNCATED :
        rc==OS64_SLURP_NO_MEMORY ? OS64_CONF_NO_MEMORY : OS64_CONF_IO_ERROR;
    placement_parse_t parse={.mode=p->mode,.fallback=*g,.value=*g,.syntax_ok=true};
    int64_t result=os64_conf_parse((char *)bytes,length,setting,&parse);
    os64_free(bytes);
    if (result<0) return result;
    if (!parse.syntax_ok) return OS64_CONF_BAD_SETTING;
    *g=parse.value;
    return 0;
}
bool os64_window_placement_init(os64_window_placement_t *p,
    os64_window_remember_t remember, os64_window_geometry_t defaults)
{
    if (!p) return false;
    *p=(os64_window_placement_t){.preferred=defaults};
    if (!defaults.width || !defaults.height || defaults.width>OS64_WINDOW_PREFERENCE_MAX ||
        defaults.height>OS64_WINDOW_PREFERENCE_MAX) return false;
    if (!remember.config) return true;
    if (remember.mode!=OS64_WINDOW_REMEMBER_SIZE && remember.mode!=OS64_WINDOW_REMEMBER_POSITION_AND_SIZE)
        return false;
    size_t n=os64_strlen(remember.config);
    if (!n || n>=sizeof(p->config) || os64_streq(remember.config,".") || os64_streq(remember.config,"..")) return false;
    for (size_t i=0;i<n;++i)
        if ((unsigned char)remember.config[i]<33 || remember.config[i]=='/' || remember.config[i]==127) return false;
    os64_strcopy(p->config,sizeof(p->config),remember.config);
    p->mode=remember.mode;
    char source[OS64_CONF_PATH_MAX];
    p->status=read_preference(p,&p->preferred,source);
    return true;
}
os64_window_geometry_t os64_window_placement_prepare(const os64_window_placement_t *p)
{
    os64_window_geometry_t g=p->preferred;
    uint32_t w,h;
    if (os64_gui_screen_info(&w,&h)!=0) w=h=OS64_GUI_WINDOW_DIM_MAX;
    if (g.width>w) g.width=w;
    if (g.height>h) g.height=h;
    if (g.width<OS64_GUI_MIN_CONTENT) g.width=OS64_GUI_MIN_CONTENT;
    if (g.height<OS64_GUI_MIN_CONTENT) g.height=OS64_GUI_MIN_CONTENT;
    return g;
}
bool os64_window_placement_event(os64_window_placement_t *p, const os64_gui_event_t *e)
{
    if (!p || !e || !p->mode || e->type!=OS64_GUI_EVENT_WINDOW_PLACEMENT) return false;
    uint32_t mask=e->placement.changed & (p->mode==OS64_WINDOW_REMEMBER_SIZE ?
        OS64_GUI_PLACEMENT_WIDTH|OS64_GUI_PLACEMENT_HEIGHT : OS64_GUI_PLACEMENT_ALL);
    if (!mask) return true;
    if (((mask&OS64_GUI_PLACEMENT_WIDTH) && (!e->placement.width || e->placement.width>OS64_WINDOW_PREFERENCE_MAX)) ||
        ((mask&OS64_GUI_PLACEMENT_HEIGHT) && (!e->placement.height || e->placement.height>OS64_WINDOW_PREFERENCE_MAX))) {
        p->status=OS64_CONF_BAD_SETTING; return true;
    }
    char source[OS64_CONF_PATH_MAX], destination[OS64_CONF_PATH_MAX], value[96];
    os64_window_geometry_t g=p->preferred;
    int64_t rc=read_preference(p,&g,source);
    if (rc && rc!=OS64_CONF_NO_FILE) { p->status=rc; return true; }
    if (mask&OS64_GUI_PLACEMENT_X) g.x=e->placement.x;
    if (mask&OS64_GUI_PLACEMENT_Y) g.y=e->placement.y;
    if (mask&OS64_GUI_PLACEMENT_WIDTH) g.width=e->placement.width;
    if (mask&OS64_GUI_PLACEMENT_HEIGHT) g.height=e->placement.height;
    os64_conf_pair_t pair;
    if (p->mode==OS64_WINDOW_REMEMBER_SIZE) {
        os64_snprintf(value,sizeof(value),"%u,%u",g.width,g.height); pair.key="size";
    } else {
        os64_snprintf(value,sizeof(value),"1,%d,%d,%u,%u",g.x,g.y,g.width,g.height); pair.key="geometry";
    }
    pair.value=value;
    p->status=os64_conf_target(p->config,destination,sizeof(destination));
    if (p->status) return true;
    p->status=rc==OS64_CONF_NO_FILE ? os64_conf_create_checked(destination,&pair,1,validate,NULL) :
        os64_conf_merge_checked(source,destination,&pair,1,validate,NULL);
    if (!p->status) p->preferred=g;
    return true;
}
