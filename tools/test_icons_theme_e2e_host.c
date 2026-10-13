#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "image/icon_theme.h"
#include "os64/conf.h"
#include "os64/io.h"
#include "os64/slurp.h"
#include "png/png.h"

typedef struct { size_t bytes; } allocation_t;
static size_t live;
static size_t allocation_calls, fail_after=SIZE_MAX;
static char config_path[512];
static char selected_icon_path[512], default_folder_path[512];
static bool open_fail_once;
static size_t default_folder_reads;
void *os64_malloc(size_t n)
{
    if (allocation_calls++ >= fail_after) return NULL;
    allocation_t *p=malloc(sizeof(*p)+n);
    if (!p) return NULL;
    p->bytes=n; live+=n+sizeof(*p); return p+1;
}
void os64_free(void *v)
{
    if (!v) return;
    allocation_t *p=(allocation_t *)v-1;
    assert(live>=p->bytes+sizeof(*p));
    live-=p->bytes+sizeof(*p); free(p);
}
size_t os64_malloc_size(const void *v)
{ return ((const allocation_t *)v-1)->bytes; }
void *os64_memset(void *p,int c,size_t n) { return memset(p,c,n); }
void *os64_memcpy(void *d,const void *s,size_t n) { return memcpy(d,s,n); }
int os64_memcmp(const void *a,const void *b,size_t n) { return memcmp(a,b,n); }
bool os64_streq(const char *a,const char *b) { return strcmp(a,b)==0; }
size_t os64_strcopy(char *out,size_t cap,const char *s)
{
    size_t n=strlen(s),copy=n<cap?n:cap?cap-1:0;
    if (cap) { memcpy(out,s,copy); out[copy]=0; }
    return n;
}
int32_t os64_hprintf(int32_t h,const char *fmt,...)
{
    (void)h;
    va_list ap; va_start(ap,fmt);
    int result=vfprintf(stderr,fmt,ap);
    va_end(ap); return result;
}
os64_slurp_status_t os64_slurp(const char *path,size_t cap,uint8_t **out,size_t *len)
{
    *out=NULL; *len=0;
    if (!strcmp(path,default_folder_path)) ++default_folder_reads;
    if (open_fail_once && !strcmp(path,selected_icon_path)) {
        open_fail_once=false; return OS64_SLURP_NO_FILE;
    }
    FILE *f=fopen(path,"rb");
    if (!f) return OS64_SLURP_NO_FILE;
    assert(fseek(f,0,SEEK_END)==0);
    long end=ftell(f);
    assert(end>=0 && fseek(f,0,SEEK_SET)==0);
    if ((unsigned long)end>cap) { fclose(f); return OS64_SLURP_TOO_BIG; }
    uint8_t *p=os64_malloc(cap+1);
    if (!p) { fclose(f); return OS64_SLURP_NO_MEMORY; }
    if (fread(p,1,(size_t)end,f)!=(size_t)end) {
        fclose(f); os64_free(p); return OS64_SLURP_IO_ERROR;
    }
    fclose(f); p[end]=0; *out=p; *len=(size_t)end;
    return OS64_SLURP_OK;
}
int64_t os64_stat(const char *path,os64_dirent_t *entry)
{
    struct stat st;
    if (stat(path,&st)) return -1;
    memset(entry,0,sizeof(*entry)); entry->size=(uint64_t)st.st_size;
    return 0;
}
typedef struct { char *out; size_t cap; bool found; } setting_t;
static bool setting(const char *key,const char *value,void *user)
{
    setting_t *s=user;
    if (key && !strcmp(key,"set")) {
        s->found=os64_strcopy(s->out,s->cap,value)<s->cap;
    }
    return true;
}
int64_t os64_conf_get(const char *name,const char *key,char *out,size_t cap)
{
    assert(!strcmp(name,"icons.conf") && !strcmp(key,"set"));
    uint8_t *bytes; size_t len;
    if (os64_slurp(config_path,4096,&bytes,&len)!=OS64_SLURP_OK)
        return OS64_CONF_NO_FILE;
    setting_t s={out,cap,false};
    int64_t rc=os64_conf_parse((const char *)bytes,len,setting,&s);
    os64_free(bytes);
    return rc<0?rc:s.found?0:OS64_CONF_NO_KEY;
}
static bool lease_matches_png(const os64_ui_icon_lease_t *lease,const char *path)
{
    uint8_t *bytes; size_t len;
    assert(os64_slurp(path,65536,&bytes,&len)==OS64_SLURP_OK);
    os64_png_image_t decoded={0};
    assert(os64_png_decode(bytes,len,4096,&decoded)==OS64_PNG_OK);
    bool same=lease->width==decoded.width && lease->height==decoded.height &&
        lease->pitch==decoded.width &&
        !memcmp(lease->pixels,decoded.pixels,
                (size_t)decoded.width*decoded.height*sizeof(uint32_t));
    os64_png_free(&decoded); os64_free(bytes);
    return same;
}
static os64_ui_icon_status_t acquire(const os64_ui_icon_provider_t *p,
    const char *key,os64_ui_icon_lease_t *lease)
{ return p->acquire(p->user,key,24,lease); }
static void release(const os64_ui_icon_provider_t *p,os64_ui_icon_lease_t *lease)
{ p->release(p->user,lease); }
static void write_text(const char *path,const char *text)
{
    FILE *f=fopen(path,"wb"); assert(f);
    size_t n=strlen(text);
    assert(fwrite(text,1,n,f)==n && fclose(f)==0);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    char selected[512],fallback[512],manifest[512],staged[512],root[512];
    snprintf(config_path,sizeof(config_path),"%s/icons.conf",argv[1]);
    snprintf(selected,sizeof(selected),"%s/selected/folder-24.png",argv[1]);
    snprintf(selected_icon_path,sizeof(selected_icon_path),"%s",selected);
    snprintf(fallback,sizeof(fallback),"%s/default/file-24.png",argv[1]);
    snprintf(default_folder_path,sizeof(default_folder_path),
             "%s/default/folder-24.png",argv[1]);
    snprintf(manifest,sizeof(manifest),"%s/selected/theme.conf",argv[1]);
    snprintf(staged,sizeof(staged),"%s/selected/staged.conf",argv[1]);
    snprintf(root,sizeof(root),"%s/default",argv[1]);
    os64_icon_theme_t *theme=NULL;
    fail_after=allocation_calls;
    assert(os64_icon_theme_create(&theme)==OS64_UI_NO_MEMORY && !theme);
    fail_after=SIZE_MAX;
    fail_after=allocation_calls+1;
    assert(os64_icon_theme_create(&theme)==OS64_UI_NO_MEMORY && !theme);
    fail_after=SIZE_MAX;
    assert(live==0);
    assert(os64_icon_theme_create(&theme)==OS64_UI_OK);
    assert(!strcmp(os64_icon_theme_name(theme),"Selected fixture"));
    const os64_ui_icon_provider_t *p=os64_icon_theme_provider(theme);
    os64_ui_icon_lease_t chosen={0},generic={0};
    size_t before=live;
    fail_after=allocation_calls;
    assert(acquire(p,"folder",&chosen)==OS64_UI_ICON_NO_MEMORY);
    fail_after=SIZE_MAX;
    assert(live==before);
    size_t fallback_reads=default_folder_reads;
    open_fail_once=true;
    assert(acquire(p,"folder",&chosen)==OS64_UI_ICON_IO);
    assert(!chosen.pixels && default_folder_reads==fallback_reads);
    assert(acquire(p,"folder",&chosen)==OS64_UI_ICON_OK);
    assert(lease_matches_png(&chosen,selected));
    assert(acquire(p,"file",&generic)==OS64_UI_ICON_OK);
    assert(lease_matches_png(&generic,fallback));
    os64_ui_icon_lease_t invalid={0};
    assert(acquire(p,"file/../folder",&invalid)==OS64_UI_ICON_INVALID);
    assert(os64_icon_theme_destroy(theme)==OS64_UI_BUSY);
    release(p,&chosen); release(p,&generic);
    assert(os64_icon_theme_destroy(theme)==OS64_UI_OK);

    assert(rename(manifest,staged)==0);
    assert(os64_icon_theme_create(&theme)==OS64_UI_OK);
    assert(!strcmp(os64_icon_theme_path(theme),root));
    assert(os64_icon_theme_destroy(theme)==OS64_UI_OK);
    assert(rename(staged,manifest)==0);
    write_text(config_path,"set = relative/path\n");
    assert(os64_icon_theme_create(&theme)==OS64_UI_OK);
    assert(!strcmp(os64_icon_theme_path(theme),root));
    assert(os64_icon_theme_destroy(theme)==OS64_UI_OK);
    char setting_line[600];
    snprintf(setting_line,sizeof(setting_line),"set = %s/selected\n",argv[1]);
    write_text(config_path,setting_line);

    FILE *f=fopen(selected,"wb"); assert(f);
    assert(fwrite("broken",1,6,f)==6); assert(fclose(f)==0);
    assert(os64_icon_theme_create(&theme)==OS64_UI_OK);
    p=os64_icon_theme_provider(theme);
    assert(acquire(p,"folder",&chosen)==OS64_UI_ICON_MALFORMED);
    assert(chosen.pixels==NULL);
    assert(os64_icon_theme_destroy(theme)==OS64_UI_OK);
    assert(live==0);
    puts("icon theme/catalog/PNG end-to-end: PASS");
    return 0;
}
