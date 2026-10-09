#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os64/os64.h"
#include "os64/conf.h"
#include "os64/mouse_settings.h"
static os64_mouse_snapshot_t live={.version=1};
static os64_mouse_command_t applied;
static size_t read_at;
static bool read_failure,write_failure,save_failure;
static int saved;
static const char *config;
void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }
int64_t os64_open(const char *path,const char *mode)
{ assert(!strcmp(path,OS64_MOUSE_PATH)); read_at=0; return *mode=='r'?1:2; }
int64_t os64_close(int32_t fd) { (void)fd; return 0; }
int64_t os64_read(int32_t fd,void *out,size_t n)
{
    assert(fd==1);
    if(read_failure && read_at>=17) return -1;
    if(n>17) n=17;
    if(n>sizeof(live)-read_at) n=sizeof(live)-read_at;
    memcpy(out,(char *)&live+read_at,n); read_at+=n; return n;
}
int64_t os64_write(int32_t fd,const void *in,size_t n)
{
    assert(fd==2 && n==sizeof(applied));
    const os64_mouse_command_t *c=in;
    if(write_failure || c->expected_generation!=live.generation) return -1;
    applied=*c; live.generation++; return n;
}
int64_t __wrap_os64_conf_find_bytes(const char *name,char **out,size_t *length)
{
    assert(!strcmp(name,"mouse.conf"));
    if(!config) return OS64_CONF_NO_FILE;
    *length=strlen(config); *out=malloc(*length+1); memcpy(*out,config,*length+1); return 0;
}
int64_t __wrap_os64_conf_write_checked(const char *name,const os64_conf_pair_t *pairs,size_t count,
    bool (*validate)(const char *,size_t,void *),void *ctx)
{
    assert(!strcmp(name,"mouse.conf") && count==1);
    char text[256]; snprintf(text,sizeof(text),"%s = %s\n",pairs[0].key,pairs[0].value);
    assert(validate(text,strlen(text),ctx));
    assert(!validate("bad = 999 0\n",12,ctx));
    if(save_failure) return -1;
    saved++; return 0;
}
int main(void)
{
    os64_mouse_command_t c;
    const char *good="bt-1-aabbccddeeff = 200 0\nusb-1 = 25 1\nbt-1-aabbccddeeff = 300 1\n";
    assert(os64_mouse_config_parse(good,strlen(good),&c) && c.count==2);
    assert(c.settings[0].speed==300 && c.settings[0].right_primary==1);
    const char *bad[]={"x = 0 0", "x = 401 0", "x = 100 2", "x = 100 0 junk", "x = 1000",
        "x 100 0", "X = 100 0", "x = -25 0", "x = 100 00", "x = 999999999999999999999 0"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!os64_mouse_config_parse(bad[i],strlen(bad[i]),&c));
    const char embedded[]="x = 100 0\n\0y = 100 0";
    assert(!os64_mouse_config_parse(embedded,sizeof(embedded)-1,&c));
    char many[1024]={0};
    for(unsigned i=0;i<17;i++) { char line[40]; snprintf(line,sizeof(line),"mouse-%u = 100 0\n",i); strcat(many,line); }
    assert(!os64_mouse_config_parse(many,strlen(many),&c));
    assert(!os64_mouse_startup() && !live.generation);
    config="x = 999 0"; assert(os64_mouse_startup()<0 && !live.generation);
    config=good; read_failure=true; assert(os64_mouse_startup()<0 && !live.generation);
    read_failure=false; write_failure=true; assert(os64_mouse_startup()<0 && !live.generation);
    write_failure=false; assert(!os64_mouse_startup() && live.generation==1 && applied.count==2);
    assert(!os64_mouse_startup() && live.generation==1); // Restarting desktop cannot overwrite Apply.
    assert(!os64_mouse_save(&applied.settings[0]) && saved==1);
    save_failure=true; assert(os64_mouse_save(&applied.settings[0])<0 && saved==1);
    puts("PASS: complete config validation, duplicate replacement, partial I/O, startup CAS and save failure reporting");
}
