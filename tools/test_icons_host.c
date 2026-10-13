#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "image/icons.h"
#include "os64/io.h"
#include "os64/slurp.h"

typedef struct { size_t bytes; } allocation_t;
static size_t live, peak, calls, fail_after = SIZE_MAX;
static bool io_once;
static bool open_fail_once;
void *os64_malloc(size_t n)
{
    if (calls++ >= fail_after) return NULL;
    allocation_t *p = malloc(sizeof(*p) + n);
    if (!p) return NULL;
    p->bytes = n;
    live += n + sizeof(*p);
    if (live > peak) peak = live;
    return p + 1;
}
void os64_free(void *v)
{
    if (!v) return;
    allocation_t *p = (allocation_t *)v - 1;
    assert(live >= p->bytes + sizeof(*p));
    live -= p->bytes + sizeof(*p);
    free(p);
}
size_t os64_malloc_size(const void *v)
{ return ((const allocation_t *)v - 1)->bytes; }
void *os64_memset(void *p, int c, size_t n) { return memset(p, c, n); }
void *os64_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
int os64_memcmp(const void *a, const void *b, size_t n) { return memcmp(a,b,n); }
bool os64_streq(const char *a, const char *b) { return strcmp(a,b)==0; }
os64_slurp_status_t os64_slurp(const char *path, size_t cap,
    uint8_t **out, size_t *len)
{
    *out=NULL; *len=0;
    if (open_fail_once && strstr(path,"/open-retry-24.png")) {
        open_fail_once=false; return OS64_SLURP_NO_FILE;
    }
    if (io_once && strstr(path,"/retry-24.png")) {
        io_once=false; return OS64_SLURP_IO_ERROR;
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
int64_t os64_stat(const char *path, os64_dirent_t *entry)
{
    struct stat st;
    if (stat(path,&st)) return -1;
    memset(entry,0,sizeof(*entry)); entry->size=(uint64_t)st.st_size;
    return 0;
}
static os64_ui_icon_status_t get(const os64_ui_icon_provider_t *p,
    const char *key, uint32_t size, os64_ui_icon_lease_t *l)
{ return p->acquire(p->user,key,size,l); }
static void put(const os64_ui_icon_provider_t *p, os64_ui_icon_lease_t *l)
{ p->release(p->user,l); }
int main(int argc, char **argv)
{
    assert(argc==2);
    os64_icon_catalog_t *c=NULL;
    assert(os64_icon_catalog_create("relative",7,&c)==OS64_UI_INVALID);
    assert(os64_icon_catalog_create(argv[1],7,&c)==OS64_UI_OK);
    const os64_ui_icon_provider_t *p=os64_icon_catalog_provider(c);
    size_t baseline=live;
    os64_ui_icon_lease_t a={0}, b={0};
    assert(get(p,"exact",24,&a)==OS64_UI_ICON_OK);
    assert(a.width==24 && a.pitch==24 && a.generation==7);
    assert(a.pixels[0]==0x7f115bcb);
    size_t cold=peak-baseline;
    assert(cold<128u*1024u);
    size_t attempts;
    os64_icon_catalog_stats_t st;
    os64_icon_catalog_stats(c,&st); attempts=st.file_attempts;
    assert(get(p,"exact",24,&b)==OS64_UI_ICON_OK && b.pixels==a.pixels);
    os64_icon_catalog_stats(c,&st); assert(st.file_attempts==attempts && st.leases==2);
    put(p,&b);
    os64_icon_catalog_t *next=NULL;
    assert(os64_icon_catalog_create(argv[1],8,&next)==OS64_UI_OK);
    const os64_ui_icon_provider_t *next_provider=os64_icon_catalog_provider(next);
    assert(get(next_provider,"exact",24,&b)==OS64_UI_ICON_OK);
    assert(b.generation==8 && a.generation==7 && b.pixels!=a.pixels);
    assert(a.pixels[0]==0x7f115bcb);
    assert(os64_icon_catalog_destroy(next)==OS64_UI_BUSY);
    put(next_provider,&b);
    assert(os64_icon_catalog_destroy(next)==OS64_UI_OK);
    assert(os64_icon_catalog_destroy(c)==OS64_UI_BUSY);
    assert(os64_icon_catalog_refresh(c)==OS64_UI_BUSY);
    assert(get(p,"small",48,&b)==OS64_UI_ICON_OK && b.width==16);
    put(p,&b);
    assert(get(p,"missing",24,&b)==OS64_UI_ICON_MISSING);
    os64_icon_catalog_stats(c,&st); attempts=st.file_attempts;
    assert(get(p,"missing",24,&b)==OS64_UI_ICON_MISSING);
    os64_icon_catalog_stats(c,&st); assert(st.file_attempts==attempts);
    assert(get(p,"bad",24,&b)==OS64_UI_ICON_MALFORMED);
    assert(get(p,"bad-size",24,&b)==OS64_UI_ICON_MALFORMED);
    assert(get(p,"large",24,&b)==OS64_UI_ICON_LIMIT);
    io_once=true;
    assert(get(p,"retry",24,&b)==OS64_UI_ICON_IO);
    assert(get(p,"retry",24,&b)==OS64_UI_ICON_OK);
    put(p,&b);
    open_fail_once=true;
    assert(get(p,"open-retry",24,&b)==OS64_UI_ICON_IO);
    assert(get(p,"open-retry",24,&b)==OS64_UI_ICON_OK);
    put(p,&b);
    size_t before_wide=live; peak=live;
    assert(get(p,"wide",64,&b)==OS64_UI_ICON_OK);
    size_t wide_peak=peak-before_wide;
    assert(wide_peak < 128u*1024u);
    put(p,&b);
    assert(get(p,"../exact",24,&b)==OS64_UI_ICON_INVALID);
    assert(get(p,"exact",17,&b)==OS64_UI_ICON_INVALID);
    for (int i=0;i<300;++i) {
        char absent[16];
        snprintf(absent,sizeof(absent),"absent%03d",i);
        assert(get(p,absent,16,&b)==OS64_UI_ICON_MISSING);
    }
    os64_icon_catalog_stats(c,&st);
    assert(st.records<=OS64_ICON_CATALOG_RECORD_LIMIT);
    size_t saved=calls;
    for (size_t failure=0; failure<4; ++failure) {
        fail_after=calls+failure;
        assert(get(p,"pin000",64,&b)==OS64_UI_ICON_NO_MEMORY);
        fail_after=SIZE_MAX;
    }
    assert(get(p,"pin000",64,&b)==OS64_UI_ICON_OK);
    put(p,&b);
    put(p,&a);
    assert(os64_icon_catalog_refresh(c)==OS64_UI_OK);
    os64_icon_catalog_stats(c,&st); assert(st.records==0 && st.retained_bytes==0);
    assert(live==baseline && calls>saved);
    assert(get(p,"install-later",24,&b)==OS64_UI_ICON_MISSING);
    char source[512], destination[512];
    snprintf(source,sizeof(source),"%s/staged.png",argv[1]);
    snprintf(destination,sizeof(destination),"%s/install-later-24.png",argv[1]);
    assert(rename(source,destination)==0);
    assert(get(p,"install-later",24,&b)==OS64_UI_ICON_MISSING);
    assert(os64_icon_catalog_refresh(c)==OS64_UI_OK);
    assert(get(p,"install-later",24,&b)==OS64_UI_ICON_OK);
    put(p,&b);

    os64_ui_icon_lease_t held[130]={0};
    char key[16];
    size_t count=0;
    for (size_t i=0;i<130;++i) {
        snprintf(key,sizeof(key),"pin%03zu",i);
        os64_ui_icon_status_t rc=get(p,key,64,&held[i]);
        if (rc==OS64_UI_ICON_LIMIT) break;
        assert(rc==OS64_UI_ICON_OK && held[i].pixels[0]==0x7f115bcb);
        ++count;
    }
    assert(count>100 && count<130);
    os64_icon_catalog_stats(c,&st);
    assert(st.retained_bytes<=OS64_ICON_CATALOG_PIXEL_BUDGET && st.leases==count);
    put(p,&held[0]);
    snprintf(key,sizeof(key),"pin%03zu",count);
    assert(get(p,key,64,&held[count])==OS64_UI_ICON_OK);
    assert(held[1].pixels[0]==0x7f115bcb);
    put(p,&held[count]);
    for (size_t i=1;i<count;++i) put(p,&held[i]);
    assert(os64_icon_catalog_destroy(c)==OS64_UI_OK);
    assert(live==0);
    printf("icon catalog host: PASS (cold peak %zu, 16-bit 64px peak %zu bytes incl. host allocator header; pinned %zu)\n",cold,wide_peak,count);
    return 0;
}
