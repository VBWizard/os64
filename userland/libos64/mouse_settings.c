#include "os64/os64.h"
#include "os64/conf.h"
#include "os64/mouse_settings.h"

int os64_mouse_read(os64_mouse_snapshot_t *out)
{
    if(!out) return -1;
    int64_t fd=os64_open(OS64_MOUSE_PATH,"r");
    if(fd<0) return -1;
    size_t got=0;
    while(got<sizeof(*out)) {
        int64_t n=os64_read(fd,(uint8_t *)out+got,sizeof(*out)-got);
        if(n<=0) break;
        got+=(size_t)n;
    }
    uint8_t extra;
    int64_t end=os64_read(fd,&extra,1);
    os64_close(fd);
    if(got!=sizeof(*out) || end!=0 || out->version!=OS64_MOUSE_VERSION || out->count>OS64_MOUSE_DEVICES) return -1;
    for(unsigned i=0;i<out->count;i++) {
        if(!os64_mouse_setting_valid(&out->devices[i].setting) ||
           out->devices[i].name[OS64_MOUSE_NAME-1] || out->devices[i].connected>1) return -1;
    }
    return 0;
}
int os64_mouse_apply(const os64_mouse_command_t *command)
{
    int64_t fd=os64_open(OS64_MOUSE_PATH,"w");
    if(fd<0) return -1;
    int64_t n=os64_write(fd,command,sizeof(*command));
    os64_close(fd);
    return n==(int64_t)sizeof(*command)?0:-1;
}

typedef struct { os64_mouse_command_t *command; bool valid; } mouse_parse_t;
static bool mouse_pair(const char *key,const char *value,void *ctx)
{
    mouse_parse_t *parse=ctx;
    os64_mouse_setting_t s={0};
    if(!key || os64_strcopy(s.key,sizeof(s.key),key)>=sizeof(s.key)) goto bad;
    if(*value<'0' || *value>'9') goto bad;
    while(*value>='0' && *value<='9') {
        s.speed=s.speed*10+(unsigned)(*value++-'0');
        if(s.speed>OS64_MOUSE_MAX_SPEED) goto bad;
    }
    if(*value!=' ' && *value!='\t') goto bad;
    while(*value==' ' || *value=='\t') value++;
    if((*value!='0' && *value!='1') || value[1]) goto bad;
    s.right_primary=*value-'0';
    if(!os64_mouse_setting_valid(&s)) goto bad;
    unsigned i=0;
    while(i<parse->command->count && os64_strcmp(parse->command->settings[i].key,s.key)) i++;
    if(i==OS64_MOUSE_DEVICES) goto bad;
    if(i==parse->command->count) parse->command->count++;
    parse->command->settings[i]=s;
    return true;
bad:
    parse->valid=false; return false;
}
bool os64_mouse_config_parse(const char *text,size_t length,os64_mouse_command_t *out)
{
    *out=(os64_mouse_command_t){.version=OS64_MOUSE_VERSION};
    mouse_parse_t parse={out,true};
    return os64_conf_parse(text,length,mouse_pair,&parse)>=0 && parse.valid;
}
static bool mouse_validate(const char *text,size_t length,void *ctx)
{
    (void)ctx; os64_mouse_command_t command;
    return os64_mouse_config_parse(text,length,&command);
}
int os64_mouse_save(const os64_mouse_setting_t *setting)
{
    if(!os64_mouse_setting_valid(setting)) return -1;
    char value[24];
    os64_snprintf(value,sizeof(value),"%u %u",setting->speed,setting->right_primary);
    os64_conf_pair_t pair={setting->key,value};
    return (int)os64_conf_write_checked("mouse.conf",&pair,1,mouse_validate,NULL);
}
int os64_mouse_startup(void)
{
    os64_mouse_snapshot_t snapshot;
    if(os64_mouse_read(&snapshot)) return -1;
    if(snapshot.generation) return 0;
    char *text=NULL; size_t length=0;
    int64_t result=os64_conf_find_bytes("mouse.conf",&text,&length);
    if(result==OS64_CONF_NO_FILE) return 0;
    if(result<0) return -1;
    os64_mouse_command_t command;
    bool valid=os64_mouse_config_parse(text,length,&command);
    os64_free(text);
    if(!valid) return -1;
    return command.count?os64_mouse_apply(&command):0;
}
