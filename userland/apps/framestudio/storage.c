#include "storage.h"
#include "os64/os64.h"
#include "os64/conf.h"

typedef struct {
    uint32_t magic,version,bytes,bundle_offset,bundle_bytes,font_size,checksum,reserved;
    char face[3][OS64_FONT_PATH_CAP];
} frame_file_t;
_Static_assert(sizeof(frame_file_t)==FRAME_FILE_HEADER_BYTES,"frame file header");

bool frame_name_valid(const char *name)
{
    if(!name || !*name || *name==' ')return false;
    size_t n=0;
    for(;name[n];++n){unsigned char c=name[n];
        if(n==FRAME_NAME_MAX || !((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
            (c>='0'&&c<='9') || c==' ' || c=='-' || c=='_' || c=='&' || c=='(' || c==')'))return false;
    }
    return name[n-1]!=' ';
}
static uint32_t checksum(const void *bytes,size_t length)
{
    const uint8_t *p=bytes;uint32_t hash=2166136261u;
    for(size_t i=0;i<length;++i){uint8_t c=i>=offsetof(frame_file_t,checksum) && i<offsetof(frame_file_t,checksum)+4?0:p[i];hash=(hash^c)*16777619u;}
    return hash;
}
static bool valid_font(const frame_file_t *h)
{
    if(h->font_size<8 || h->font_size>96)return false;
    for(unsigned i=0;i<3;++i){size_t n=0;
        while(n<OS64_FONT_PATH_CAP && h->face[i][n]){if((unsigned char)h->face[i][n]<32)return false;++n;}
        if(n==OS64_FONT_PATH_CAP || (!i && !n))return false;
        if(n && h->face[i][0]!='/' && !os64_streq(h->face[i],"builtin"))return false;
    }
    return true;
}
static os64_decor_header_t recipe(const os64_decor_header_t *h)
{
    os64_decor_header_t out;os64_decor_header_copy(h,&out);
    out.version=OS64_DECOR_VERSION;
    out.bytes=out.glyph_count=out.pair_count=out.glyph_offset=out.pair_offset=0;
    out.mask_offset=out.mask_bytes=out.line_height=out.baseline=0;
    out.tile_offset=out.tile_bytes=0;
    return out;
}
int frame_encode(const frame_draft_t *draft,const void *bundle,size_t length,void **out,size_t *size)
{
    if(out)*out=NULL;
    if(size)*size=0;
    os64_decor_view_t view;
    if(!draft || !out || !size || !os64_decor_validate(bundle,length,&view))return FRAME_STORE_INVALID;
    frame_draft_t canonical=*draft;canonical.style=recipe(view.header);
    if(!frame_same(draft,&canonical))return FRAME_STORE_INVALID;
    frame_file_t header={.magic=0x314d5246,.version=1,.bytes=(uint32_t)(sizeof(header)+length),
        .bundle_offset=sizeof(header),.bundle_bytes=(uint32_t)length,.font_size=draft->font.size};
    os64_memcpy(header.face,draft->font.face,sizeof(header.face));
    if(!valid_font(&header))return FRAME_STORE_INVALID;
    uint8_t *bytes=os64_malloc(header.bytes);if(!bytes)return FRAME_STORE_MEMORY;
    os64_memcpy(bytes,&header,sizeof(header));os64_memcpy(bytes+sizeof(header),bundle,length);
    ((frame_file_t *)bytes)->checksum=checksum(bytes,header.bytes);
    *out=bytes;*size=header.bytes;return FRAME_STORE_OK;
}
int frame_decode(const void *bytes,size_t size,frame_draft_t *draft,void **bundle,size_t *length)
{
    if(bundle)*bundle=NULL;
    if(length)*length=0;
    if(!bytes || !draft || !bundle || !length || ((uintptr_t)bytes&3u) ||
        size<sizeof(frame_file_t) || size>FRAME_FILE_BYTES_MAX)return FRAME_STORE_INVALID;
    const frame_file_t *h=bytes;
    if(h->magic!=0x314d5246 || h->version!=1 || h->bytes!=size || h->reserved ||
        h->bundle_offset!=sizeof(*h) || h->bundle_bytes!=size-sizeof(*h) ||
        !valid_font(h) || h->checksum!=checksum(bytes,size))return FRAME_STORE_INVALID;
    const void *source=(const uint8_t *)bytes+sizeof(*h);os64_decor_view_t view;
    if(!os64_decor_validate(source,h->bundle_bytes,&view))return FRAME_STORE_INVALID;
    void *copy=os64_malloc(h->bundle_bytes);if(!copy)return FRAME_STORE_MEMORY;
    os64_memcpy(copy,source,h->bundle_bytes);
    frame_draft_t next={.style=recipe(view.header)};next.font.size=h->font_size;
    os64_memcpy(next.font.face,h->face,sizeof(h->face));
    *draft=next;*bundle=copy;*length=h->bundle_bytes;return FRAME_STORE_OK;
}
static int personal_directory(char *dir)
{
    char target[OS64_CONF_PATH_MAX],cwd[OS64_CONF_PATH_MAX];
    if(os64_conf_target("frames",target,sizeof(target)))return FRAME_STORE_IO;
    cwd[0]=0;
    if(target[0]!='/' && os64_getcwd(cwd,sizeof(cwd))<0)return FRAME_STORE_IO;
    /* Match VFS component resolution so config aliases cannot turn the
     * included directory into a writable personal collection. */
    const char *parts[2]={cwd,target};size_t used=1;dir[0]='/';
    for(unsigned i=0;i<2;++i){const char *p=parts[i];
        while(*p){
            while(*p=='/')++p;
            const char *start=p;while(*p && *p!='/')++p;
            size_t n=(size_t)(p-start);
            if(!n || (n==1 && start[0]=='.'))continue;
            if(n==2 && start[0]=='.' && start[1]=='.'){
                while(used>1 && dir[used-1]!='/')--used;
                if(used>1)--used;
                continue;
            }
            size_t slash=used>1?1:0;
            if(used+slash+n>=OS64_CONF_PATH_MAX)return FRAME_STORE_INVALID;
            if(slash)dir[used++]='/';
            os64_memcpy(dir+used,start,n);used+=n;
        }
    }
    dir[used]=0;return FRAME_STORE_OK;
}
static int paths(const char *name,char *dir,char *path)
{
    if(!frame_name_valid(name))return FRAME_STORE_INVALID;
    int result=personal_directory(dir);if(result)return result;
    if(os64_streq(dir,FRAME_INCLUDED_DIR))return FRAME_STORE_INVALID;
    int n=os64_snprintf(path,OS64_CONF_PATH_MAX,"%s/%s.frame",dir,name);
    return n>0 && n<OS64_CONF_PATH_MAX?FRAME_STORE_OK:FRAME_STORE_INVALID;
}
static bool name_after(const char *a,const char *b)
{
    while(*a && *a==*b){++a;++b;}
    return (unsigned char)*a>(unsigned char)*b;
}
static int scan_collection(const char *dir,bool included,frame_saved_t *entries,
    size_t capacity,size_t *count)
{
    int64_t fd=os64_opendir(dir);
    /* An older installation may have no included collection. */
    if(fd<0)return included?0:-FRAME_STORE_IO;
    os64_dirent_t entry;size_t scanned=0;int result=0;int64_t rc;
    while((rc=os64_readdir(fd,&entry))==1){
        if(++scanned>512){result=-FRAME_STORE_LIMIT;break;}
        if(entry.flags&OS64_DE_DIR)continue;
        size_t n=os64_strlen(entry.name);
        if(n<=6 || n>FRAME_NAME_MAX+6 || !os64_streq(entry.name+n-6,".frame"))continue;
        entry.name[n-6]=0;if(!frame_name_valid(entry.name))continue;
        bool duplicate=false;
        for(size_t i=0;i<*count;++i)if(os64_streq(entries[i].name,entry.name)){duplicate=true;break;}
        if(duplicate)continue;
        if(*count==capacity){result=-FRAME_STORE_LIMIT;break;}
        frame_saved_t *saved=&entries[(*count)++];
        os64_strcopy(saved->name,sizeof(saved->name),entry.name);saved->included=included;
    }
    os64_close(fd);if(rc<0)return -FRAME_STORE_IO;if(result)return result;
    return 0;
}
int frame_saved_list(frame_saved_t *entries,size_t capacity)
{
    char dir[OS64_CONF_PATH_MAX];
    if(!entries || !capacity)return -FRAME_STORE_INVALID;
    int result=personal_directory(dir);if(result)return -result;
    size_t count=0;
    if(!os64_streq(dir,FRAME_INCLUDED_DIR)){
        (void)os64_mkdir(dir);
        result=scan_collection(dir,false,entries,capacity,&count);if(result)return result;
    }
    result=scan_collection(FRAME_INCLUDED_DIR,true,entries,capacity,&count);if(result)return result;
    for(size_t i=1;i<count;++i){frame_saved_t value=entries[i];size_t j=i;
        while(j && name_after(entries[j-1].name,value.name)){entries[j]=entries[j-1];--j;}entries[j]=value;
    }
    return (int)count;
}
int frame_save(const char *name,const frame_draft_t *draft,const void *bundle,size_t length,bool replace)
{
    char dir[OS64_CONF_PATH_MAX],path[OS64_CONF_PATH_MAX],temp[OS64_CONF_PATH_MAX];
    int result=paths(name,dir,path);if(result)return result;
    void *bytes=NULL;size_t size=0;result=frame_encode(draft,bundle,length,&bytes,&size);if(result)return result;
    (void)os64_mkdir(dir);
    static uint64_t sequence;int64_t fd=-1;
    /* Exclusive creation avoids reusing a staging file left by an old task. */
    for(unsigned attempt=0;attempt<16 && fd<0;++attempt){
        uint64_t seq=__atomic_fetch_add(&sequence,1,__ATOMIC_RELAXED);
        int n=os64_snprintf(temp,sizeof(temp),"%s/.%lu.%lu.new",dir,(unsigned long)os64_taskid(),(unsigned long)seq);
        if(n<0 || (size_t)n>=sizeof(temp))break;
        fd=os64_open(temp,"x");
    }
    if(fd<0){os64_free(bytes);return FRAME_STORE_IO;}
    for(size_t used=0;used<size;){
        int64_t n=os64_write(fd,(uint8_t *)bytes+used,size-used);
        if(n==OS64_INTERRUPTED)continue;
        if(n<=0 || (uint64_t)n>size-used){result=FRAME_STORE_IO;break;}used+=(size_t)n;
    }
    if(!result && os64_sync(fd))result=FRAME_STORE_IO;
    os64_close(fd);os64_free(bytes);
    if(!result && os64_rename_with_flags(temp,path,replace?OS64_RENAME_REQUIRE_ATOMIC_REPLACE:OS64_RENAME_NOREPLACE)){
        os64_dirent_t entry;result=!replace && !os64_stat(path,&entry)?FRAME_STORE_EXISTS:FRAME_STORE_IO;
    }
    if(result)(void)os64_unlink(temp);
    return result;
}
static int load_path(const char *path,frame_draft_t *draft,void **bundle,size_t *length)
{
    if(bundle)*bundle=NULL;
    if(length)*length=0;
    if(!draft || !bundle || !length)return FRAME_STORE_INVALID;
    int result=FRAME_STORE_OK;
    int64_t fd=os64_open(path,"r");if(fd<0)return FRAME_STORE_IO;
    int64_t size=os64_seek(fd,0,OS64_SEEK_END);
    if(size<(int64_t)sizeof(frame_file_t) || size>FRAME_FILE_BYTES_MAX || os64_seek(fd,0,OS64_SEEK_SET)!=0){os64_close(fd);return FRAME_STORE_INVALID;}
    void *bytes=os64_malloc((size_t)size);if(!bytes){os64_close(fd);return FRAME_STORE_MEMORY;}
    for(size_t used=0;used<(size_t)size;){
        int64_t n=os64_read(fd,(uint8_t *)bytes+used,(size_t)size-used);
        if(n==OS64_INTERRUPTED)continue;
        if(n<=0 || (uint64_t)n>(size_t)size-used){result=FRAME_STORE_IO;break;}used+=(size_t)n;
    }
    uint8_t tail;int64_t n;
    do{n=os64_read(fd,&tail,1);}while(n==OS64_INTERRUPTED);
    if(n!=0)result=FRAME_STORE_IO;
    os64_close(fd);
    if(!result)result=frame_decode(bytes,(size_t)size,draft,bundle,length);
    os64_free(bytes);return result;
}

int frame_load_entry(const frame_saved_t *entry,frame_draft_t *draft,void **bundle,size_t *length)
{
    if(bundle)*bundle=NULL;
    if(length)*length=0;
    if(!entry || !draft || !bundle || !length || !frame_name_valid(entry->name))return FRAME_STORE_INVALID;
    char dir[OS64_CONF_PATH_MAX],path[OS64_CONF_PATH_MAX];
    if(entry->included){
        int n=os64_snprintf(path,sizeof(path),"%s/%s.frame",FRAME_INCLUDED_DIR,entry->name);
        if(n<0 || (size_t)n>=sizeof(path))return FRAME_STORE_INVALID;
    }else{
        int result=paths(entry->name,dir,path);if(result)return result;
    }
    return load_path(path,draft,bundle,length);
}
int frame_load(const char *name,frame_draft_t *draft,void **bundle,size_t *length)
{
    if(bundle)*bundle=NULL;
    if(length)*length=0;
    if(!frame_name_valid(name))return FRAME_STORE_INVALID;
    frame_saved_t entry={0};os64_strcopy(entry.name,sizeof(entry.name),name);
    return frame_load_entry(&entry,draft,bundle,length);
}

int frame_delete(const char *name)
{
    char dir[OS64_CONF_PATH_MAX],path[OS64_CONF_PATH_MAX];
    int result=paths(name,dir,path);if(result)return result;
    /* unlink also accepts empty directories; the collection browser excludes
     * those, so a directory substituted for an entry must be refused here. */
    os64_dirent_t entry;
    if(os64_stat(path,&entry) || (entry.flags&OS64_DE_DIR))return FRAME_STORE_IO;
    return os64_unlink(path)?FRAME_STORE_IO:FRAME_STORE_OK;
}

int frame_load_active(const frame_saved_t *entries,size_t count,size_t *selected,
    frame_draft_t *draft,void **bundle,size_t *length,uint64_t *generation)
{
    if(bundle)*bundle=NULL;
    if(length)*length=0;
    if(!generation || !entries || count>FRAME_SAVED_MAX || !selected || !draft || !bundle || !length)return 0;
    os64_decor_status_t before,after;
    if(os64_decor_current(&before) || !before.bytes)return 0;
    for(size_t i=0;i<count;++i){
        frame_draft_t next;void *bytes=NULL;size_t size=0;
        if(frame_load_entry(&entries[i],&next,&bytes,&size))continue;
        if(size==before.bytes && os64_decor_fingerprint(bytes,size)==before.fingerprint){
            if(os64_decor_current(&after) || before.generation!=after.generation ||
                before.fingerprint!=after.fingerprint || before.bytes!=after.bytes){
                os64_free(bytes);return 0;
            }
            *selected=i;*draft=next;*bundle=bytes;*length=size;*generation=before.generation;return 1;
        }
        os64_free(bytes);
    }
    return 0;
}
