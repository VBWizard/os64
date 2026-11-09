#include "os64/decoration_startup.h"
#include "os64/decoration_prepare.h"
#include "os64/os64.h"
#include "os64/conf.h"

#define STARTUP_NAME "decoration.startup"
#define STARTUP_MAGIC 0x31545344u
/* A zero-length payload explicitly selects the compiled decoration. Prepared
 * glyphs and tiles in a nonempty payload remove source-file dependencies. */
typedef struct { uint32_t magic,version,length,checksum; } startup_header_t;
_Static_assert(sizeof(startup_header_t)==16,"startup header");
static uint32_t checksum(const void *bytes,size_t length)
{
    const uint8_t *p=bytes;uint32_t hash=2166136261u;
    for(size_t i=0;i<length;++i){
        uint8_t c=i>=offsetof(startup_header_t,checksum) && i<sizeof(startup_header_t)?0:p[i];
        hash=(hash^c)*16777619u;
    }
    return hash;
}
int os64_decor_startup_save(const void *bundle,size_t length)
{
    os64_decor_view_t view;
    if((!bundle && length) || (bundle && !os64_decor_validate(bundle,length,&view)))
        return OS64_DECOR_STARTUP_INVALID;
    size_t size=sizeof(startup_header_t)+length;
    uint8_t *bytes=os64_malloc(size);
    if(!bytes)return OS64_DECOR_STARTUP_MEMORY;
    startup_header_t *h=(startup_header_t *)bytes;
    *h=(startup_header_t){.magic=STARTUP_MAGIC,.version=1,.length=(uint32_t)length};
    if(length)os64_memcpy(bytes+sizeof(*h),bundle,length);
    h->checksum=checksum(bytes,size);
    char path[OS64_CONF_PATH_MAX],temp[OS64_CONF_PATH_MAX];
    int result=OS64_DECOR_STARTUP_IO;int64_t fd=-1;
    if(os64_conf_target(STARTUP_NAME,path,sizeof(path)))goto done;
    static uint64_t sequence;
    /* Stage beside the target so the checked rename stays on one filesystem. */
    for(unsigned attempt=0;attempt<16 && fd<0;++attempt){
        uint64_t seq=__atomic_fetch_add(&sequence,1,__ATOMIC_RELAXED);
        int n=os64_snprintf(temp,sizeof(temp),"%s.%lu.%lu.new",path,
            (unsigned long)os64_taskid(),(unsigned long)seq);
        if(n<0 || (size_t)n>=sizeof(temp))goto done;
        fd=os64_open(temp,"x");
    }
    if(fd<0)goto done;
    for(size_t used=0;used<size;){
        int64_t n=os64_write(fd,bytes+used,size-used);
        if(n==OS64_INTERRUPTED)continue;
        if(n<=0 || (uint64_t)n>size-used)goto close_failed;
        used+=(size_t)n;
    }
    if(os64_sync(fd))goto close_failed;
    os64_close(fd);fd=-1;
    if(os64_rename_with_flags(temp,path,OS64_RENAME_REQUIRE_ATOMIC_REPLACE))goto remove_temp;
    result=0;goto done;
close_failed:
    os64_close(fd);
remove_temp:
    (void)os64_unlink(temp);
done:
    os64_free(bytes);return result;
}
static int load(void **out,size_t *length)
{
    *out=NULL;*length=0;
    char path[OS64_CONF_PATH_MAX];
    if(os64_conf_find(STARTUP_NAME,path,sizeof(path)))return OS64_DECOR_STARTUP_ABSENT;
    int64_t fd=os64_open(path,"r");if(fd<0)return OS64_DECOR_STARTUP_IO;
    int result=OS64_DECOR_STARTUP_INVALID;
    int64_t size=os64_seek(fd,0,OS64_SEEK_END);
    if(size<(int64_t)sizeof(startup_header_t) || size>(int64_t)(sizeof(startup_header_t)+OS64_DECOR_BYTES_MAX) ||
        os64_seek(fd,0,OS64_SEEK_SET)!=0){os64_close(fd);return result;}
    uint8_t *bytes=os64_malloc((size_t)size);
    if(!bytes){os64_close(fd);return OS64_DECOR_STARTUP_MEMORY;}
    for(size_t used=0;used<(size_t)size;){
        int64_t n=os64_read(fd,bytes+used,(size_t)size-used);
        if(n==OS64_INTERRUPTED)continue;
        if(n<=0 || (uint64_t)n>(size_t)size-used){result=OS64_DECOR_STARTUP_IO;goto done;}
        used+=(size_t)n;
    }
    uint8_t tail;int64_t n;
    do{n=os64_read(fd,&tail,1);}while(n==OS64_INTERRUPTED);
    if(n!=0){result=OS64_DECOR_STARTUP_IO;goto done;}
    const startup_header_t *h=(const startup_header_t *)bytes;
    if(h->magic!=STARTUP_MAGIC || h->version!=1 || h->length!=(size_t)size-sizeof(*h) ||
        h->checksum!=checksum(bytes,(size_t)size))goto done;
    if(!h->length){result=OS64_DECOR_STARTUP_DEFAULT;goto done;}
    os64_decor_view_t view;
    if(!os64_decor_validate(bytes+sizeof(*h),h->length,&view))goto done;
    /* Keep the allocation base with the caller; the payload is aligned at +16. */
    *out=bytes;*length=h->length;bytes=NULL;result=OS64_DECOR_STARTUP_APPLIED;
done:
    os64_close(fd);os64_free(bytes);return result;
}
int os64_decor_startup_install(void)
{
    uint64_t generation;
    if(os64_decor_generation(&generation))return OS64_DECOR_STARTUP_IO;
    if(generation)return OS64_DECOR_STARTUP_SKIPPED;
    void *bytes=NULL;size_t length=0;
    int result=load(&bytes,&length);
    if(result!=OS64_DECOR_STARTUP_APPLIED)return result;
    if(os64_decor_apply((uint8_t *)bytes+sizeof(startup_header_t),length,0)){
        /* Another publisher may have won after the initial generation read. */
        result=!os64_decor_generation(&generation) && generation?
            OS64_DECOR_STARTUP_SKIPPED:OS64_DECOR_STARTUP_IO;
    }
    os64_free(bytes);return result;
}
