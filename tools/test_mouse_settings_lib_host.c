#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "os64/os64.h"
#include "os64/conf.h"
#include "os64/mouse_settings.h"
static os64_mouse_snapshot_t live={.version=OS64_MOUSE_VERSION};
static os64_mouse_command_t applied;
static size_t read_at;
static bool read_failure,write_failure,save_failure,sync_failure,rename_failure,reconnect_on_forget;
static char path[256];
void *os64_malloc(size_t n) { return malloc(n); }
void os64_free(void *p) { free(p); }
uint64_t os64_taskid(void) { return (uint64_t)getpid(); }
uint64_t os64_syscall6(uint64_t nr,uint64_t name,uint64_t out,uint64_t cap,
    uint64_t from,uint64_t any,uint64_t unused)
{
    (void)unused; assert(nr==SYSCALL_CONF_RESOLVE && !strcmp((char *)name,"mouse.conf"));
    if(from || strlen(path)>=cap) return (uint64_t)-1;
    strcpy((char *)out,path);
    return any || !access(path,F_OK)?1:(uint64_t)-1;
}
int64_t os64_open(const char *name,const char *mode)
{
    if(!strcmp(name,OS64_MOUSE_PATH)) { read_at=0; return *mode=='r'?10000:10001; }
    return open(name,*mode=='r'?O_RDONLY:O_WRONLY|O_CREAT|O_TRUNC,0600);
}
int64_t os64_close(int32_t fd) { return fd>=10000?0:close(fd); }
int64_t os64_read(int32_t fd,void *out,size_t n)
{
    if(fd!=10000) return read(fd,out,n>17?17:n);
    if(read_failure && read_at>=17) return -1;
    if(n>17) n=17;
    if(n>sizeof(live)-read_at) n=sizeof(live)-read_at;
    memcpy(out,(char *)&live+read_at,n); read_at+=n; return n;
}
int64_t os64_write(int32_t fd,const void *in,size_t n)
{
    if(fd!=10001) return save_failure?-1:write(fd,in,n>23?23:n);
    assert(n==sizeof(applied));
    const os64_mouse_command_t *c=in;
    if(write_failure || c->expected_generation!=live.generation) return -1;
    if(c->operation==OS64_MOUSE_FORGET) {
        assert(c->count==1);
        unsigned i=0;
        while(i<live.count && strcmp(live.devices[i].setting.key,c->settings[0].key)) i++;
        if(i==live.count) return -1;
        if(reconnect_on_forget) live.devices[i].connected=1;
        if(live.devices[i].connected) return -1;
        memmove(&live.devices[i],&live.devices[i+1],(live.count-i-1)*sizeof(live.devices[0]));
        live.count--;
    } else for(unsigned j=0;j<c->count;j++) {
        unsigned i=0;
        while(i<live.count && strcmp(live.devices[i].setting.key,c->settings[j].key)) i++;
        assert(i<OS64_MOUSE_DEVICES);
        if(i==live.count) { live.count++; live.devices[i]=(os64_mouse_device_t){0}; }
        live.devices[i].setting=c->settings[j];
    }
    applied=*c; live.generation++; return n;
}
int64_t os64_sync(int32_t fd) { return sync_failure?-1:fsync(fd); }
int64_t os64_unlink(const char *name) { return unlink(name); }
int64_t os64_rename(const char *from,const char *to) { return rename(from,to); }
int64_t os64_rename_with_flags(const char *from,const char *to,uint64_t flags)
{ assert(flags==OS64_RENAME_REQUIRE_ATOMIC_REPLACE); return rename_failure?-1:rename(from,to); }
static void config(const char *text)
{
    FILE *f=fopen(path,"w"); assert(f);
    assert(fwrite(text,1,strlen(text),f)==strlen(text)); assert(!fclose(f));
}
static void saved_config(os64_mouse_command_t *out)
{
    char *text; size_t length;
    assert(!os64_conf_find_bytes("mouse.conf",&text,&length));
    assert(os64_mouse_config_parse(text,length,out)); free(text);
}
int main(int argc,char **argv)
{
    assert(argc==2 && snprintf(path,sizeof(path),"%s/mouse.conf",argv[1])<(int)sizeof(path));
    os64_mouse_command_t c;
    const char *good="bt-1-aabbccddeeff = 200 0\nusb-1 = 25 1\nbt-1-aabbccddeeff = 300 1\n";
    assert(os64_mouse_config_parse(good,strlen(good),&c) && c.count==2);
    assert(c.settings[0].speed==300 && c.settings[0].right_primary==1);
    const char *bad[]={"x = 0 0", "x = 401 0", "x = 100 2", "x = 100 0 junk", "x = 1000",
        "x 100 0", "X = 100 0", "x = -25 0", "x = 100 00", "x = 999999999999999999999 0"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!os64_mouse_config_parse(bad[i],strlen(bad[i]),&c));
    const char embedded[]="x = 100 0\n\0y = 100 0";
    assert(!os64_mouse_config_parse(embedded,sizeof(embedded)-1,&c));
    char many[2048]="# Saved mice\n";
    for(unsigned i=0;i<17;i++) { char line[40]; snprintf(line,sizeof(line),"mouse-%u = 200 0\n",i); strcat(many,line); }
    assert(!os64_mouse_config_parse(many,strlen(many),&c));
    assert(!os64_mouse_startup() && !live.generation);
    config("x = 999 0"); assert(os64_mouse_startup()<0 && !live.generation);
    config(good); read_failure=true; assert(os64_mouse_startup()<0 && !live.generation);
    read_failure=false; write_failure=true; assert(os64_mouse_startup()<0 && !live.generation);
    write_failure=false; assert(!os64_mouse_startup() && live.generation==1 && applied.count==2);
    assert(!os64_mouse_startup() && live.generation==1); // Restarting desktop cannot overwrite Apply.
    assert(!os64_mouse_save(&applied.settings[0])); saved_config(&c); assert(c.count==2 && c.settings[0].speed==300);
    save_failure=true; assert(os64_mouse_save(&applied.settings[0])<0); save_failure=false;
    // Reproduce 16 saved offline identities blocking a seventeenth Save.
    *strstr(many,"mouse-16 =")=0; strcat(many,"mouse-0 = 250 1\n"); config(many);
    live=(os64_mouse_snapshot_t){.version=OS64_MOUSE_VERSION};
    assert(!os64_mouse_startup() && live.count==16);
    os64_mouse_setting_t old=live.devices[0].setting,new={.key="new-mouse",.speed=300,.right_primary=1};
    assert(os64_mouse_save(&new)<0); saved_config(&c); assert(c.count==16);
    live.devices[0].connected=1;
    assert(os64_mouse_forget(&old,live.generation)<0); live.devices[0].connected=0;
    assert(os64_mouse_forget(&old,live.generation-1)<0);
    // A failed disk commit leaves the live record available to retry.
    uint64_t generation=live.generation;
    for(unsigned failure=0;failure<3;failure++) {
        save_failure=failure==0; sync_failure=failure==1; rename_failure=failure==2;
        assert(os64_mouse_forget(&old,generation)<0 && live.generation==generation && live.count==16);
        save_failure=sync_failure=rename_failure=false;
        saved_config(&c); assert(c.count==16 && c.settings[0].speed==250);
    }
    assert(!os64_mouse_forget(&old,generation) && live.count==15 && live.generation==generation+1);
    saved_config(&c); assert(c.count==15);
    char *text; size_t length; assert(!os64_conf_find_bytes("mouse.conf",&text,&length));
    assert(strstr(text,"# Saved mice") && !strstr(text,"mouse-0 =")); free(text);
    assert(!os64_mouse_save(&new)); saved_config(&c); assert(c.count==16 && c.settings[15].speed==300);
    live=(os64_mouse_snapshot_t){.version=OS64_MOUSE_VERSION};
    assert(!os64_mouse_startup() && live.count==16 && !strcmp(live.devices[15].setting.key,"new-mouse"));
    for(unsigned i=0;i<live.count;i++) assert(strcmp(live.devices[i].setting.key,old.key));
    // If reconnection races the file commit, report partial success and keep live settings.
    old=live.devices[0].setting; generation=live.generation; reconnect_on_forget=true;
    assert(os64_mouse_forget(&old,generation)==OS64_MOUSE_FORGET_SAVED_ONLY);
    assert(live.count==16 && live.generation==generation && live.devices[0].connected);
    saved_config(&c); assert(c.count==15);
    assert(!unlink(path));
    puts("PASS: parsing, startup CAS, real checked Save/Forget, 16-to-17 device recovery, reboot restore and disk/reconnect failures");
}
