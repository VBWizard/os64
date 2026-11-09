#define _POSIX_C_SOURCE 200809L
#include "../userland/apps/framestudio/storage.h"
#include "os64/io.h"
#include "os64/conf.h"
#include "os64/mem.h"
#include "os64/decoration_startup.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "decoration_legacy.h"

extern void frame_test_deny_next_alloc(void);

static char directory[128];
static char included_directory[128];
static const char *frames_target;
static const char *host_path(const char *path)
{
    static char translated[512];
    size_t n=strlen(FRAME_INCLUDED_DIR);
    if(strncmp(path,FRAME_INCLUDED_DIR,n) || (path[n] && path[n]!='/'))return path;
    if(!included_directory[0])return "/nonexistent-os64-included-frames";
    assert(snprintf(translated,sizeof(translated),"%s%s",included_directory,path+n)<(int)sizeof(translated));
    return translated;
}
static DIR *listing;
static bool fail_write,fail_sync,fail_rename,fail_read,fail_unlink;
static unsigned rename_calls,apply_calls,unlink_calls;
static uint64_t session_generation;
static bool race_apply,refuse_apply;
static const void *expected_bundle;
static size_t expected_length;
static os64_decor_status_t active_status;
static unsigned status_reads;
static bool change_during_lookup,fail_status;
int os64_decor_current(os64_decor_status_t *out)
{
    ++status_reads;
    if(fail_status)return -1;
    *out=active_status;
    if(change_during_lookup && status_reads==2)++out->generation;
    return 0;
}
int os64_decor_generation(uint64_t *generation)
{*generation=session_generation;return 0;}
int os64_decor_apply(const void *bytes,size_t length,uint64_t expected)
{
    ++apply_calls;assert(expected==0);
    assert(length==expected_length && !memcmp(bytes,expected_bundle,length));
    if(race_apply)session_generation=7;
    if(refuse_apply || expected!=session_generation)return -1;
    ++session_generation;return 0;
}
int64_t os64_conf_target(const char *name,char *out,size_t cap)
{
    assert(!strcmp(name,"frames") || !strcmp(name,"decoration.startup"));
    if(frames_target && !strcmp(name,"frames"))return snprintf(out,cap,"%s",frames_target)<(int)cap?0:-1;
    return snprintf(out,cap,!strcmp(name,"frames")?"%s":"%s/decoration.startup",directory)<(int)cap?0:-1;
}
int64_t os64_conf_find(const char *name,char *out,size_t cap)
{
    assert(!strcmp(name,"decoration.startup"));
    if(os64_conf_target(name,out,cap))return -1;
    if(!access(out,F_OK))return 0;
    if(snprintf(out,cap,"%s/lower.startup",directory)>=(int)cap)return -1;
    return access(out,F_OK)?-1:0;
}
uint64_t os64_taskid(void){return 77;}
int64_t os64_getcwd(char *out,size_t cap)
{return snprintf(out,cap,"/home")<(int)cap?5:-1;}
int64_t os64_mkdir(const char *path){return mkdir(path,0700);}
int64_t os64_open(const char *path,const char *mode)
{
    /* File operations must not consult the missing font sources. */
    assert(!strncmp(path,directory,strlen(directory)) || !strncmp(path,FRAME_INCLUDED_DIR,strlen(FRAME_INCLUDED_DIR)));
    int flags=!strcmp(mode,"x")?O_WRONLY|O_CREAT|O_EXCL:O_RDONLY;
    return open(host_path(path),flags,0600);
}
int64_t os64_close(int32_t fd)
{if(fd==900){closedir(listing);listing=NULL;return 0;}return close(fd);}
int64_t os64_seek(int32_t fd,int64_t offset,int32_t whence){return lseek(fd,offset,whence);}
int64_t os64_read(int32_t fd,void *bytes,size_t size)
{if(fail_read && lseek(fd,0,SEEK_CUR)>=113)return -1;return read(fd,bytes,size>113?113:size);}
int64_t os64_write(int32_t fd,const void *bytes,size_t size)
{if(fail_write && lseek(fd,0,SEEK_CUR)>=127)return -1;return write(fd,bytes,size>127?127:size);}
int64_t os64_sync(int32_t fd){return fail_sync?-1:fsync(fd);}
int64_t os64_unlink(const char *path){++unlink_calls;return fail_unlink?-1:unlink(path);}
int64_t os64_stat(const char *path,os64_dirent_t *out)
{struct stat st;int rc=stat(path,&st);if(!rc){memset(out,0,sizeof(*out));out->size=st.st_size;if(S_ISDIR(st.st_mode))out->flags=OS64_DE_DIR;}return rc;}
int64_t os64_rename_with_flags(const char *from,const char *to,uint64_t flags)
{
    ++rename_calls;
    assert(flags==OS64_RENAME_NOREPLACE || flags==OS64_RENAME_REQUIRE_ATOMIC_REPLACE);
    if(fail_rename)return -1;
    if(flags==OS64_RENAME_NOREPLACE && access(to,F_OK)==0)return -1;
    return rename(from,to);
}
int64_t os64_opendir(const char *path)
{assert(!listing);listing=opendir(host_path(path));return listing?900:-1;}
int64_t os64_readdir(int32_t fd,os64_dirent_t *out)
{
    assert(fd==900);errno=0;struct dirent *entry=readdir(listing);
    if(!entry)return errno?-1:0;
    memset(out,0,sizeof(*out));snprintf(out->name,sizeof(out->name),"%s",entry->d_name);
    if(entry->d_name[0]=='.' && (!entry->d_name[1] || (entry->d_name[1]=='.' && !entry->d_name[2])))out->flags=OS64_DE_DIR;
    return 1;
}
static void unchanged(const frame_draft_t *original,const void *bytes,size_t length)
{
    frame_draft_t loaded;void *out=NULL;size_t size=0;
    assert(frame_load("Blue grain",&loaded,&out,&size)==FRAME_STORE_OK);
    assert(frame_same(original,&loaded) && size==length && !memcmp(bytes,out,length));os64_free(out);
    DIR *d=opendir(directory);assert(d);struct dirent *e;
    while((e=readdir(d)))assert(!strstr(e->d_name,".new"));
    closedir(d);
}
static void install_expect(const void *bytes,size_t length)
{
    expected_bundle=bytes;expected_length=length;session_generation=0;
    unsigned before=apply_calls;
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_APPLIED);
    assert(session_generation==1 && apply_calls==before+1);
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_SKIPPED && apply_calls==before+1);
}
static void test_startup(const void *bundle,size_t length,const void *next,size_t next_length)
{
    session_generation=0;
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_ABSENT && !session_generation);
    assert(os64_decor_startup_save(bundle,length-1)==OS64_DECOR_STARTUP_INVALID);
    assert(os64_decor_startup_save(NULL,length)==OS64_DECOR_STARTUP_INVALID);
    assert(os64_decor_startup_save(bundle,length)==0 && !session_generation);
    install_expect(bundle,length);
    unsigned before=apply_calls;
    assert(os64_decor_startup_save(next,next_length)==0 && session_generation==1 && apply_calls==before);
    install_expect(next,next_length);
    frame_test_deny_next_alloc();
    assert(os64_decor_startup_save(bundle,length)==OS64_DECOR_STARTUP_MEMORY);
    install_expect(next,next_length);
    session_generation=0;frame_test_deny_next_alloc();
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_MEMORY && !session_generation);
    install_expect(next,next_length);
    fail_write=true;assert(os64_decor_startup_save(bundle,length)==OS64_DECOR_STARTUP_IO);fail_write=false;
    install_expect(next,next_length);
    fail_sync=true;assert(os64_decor_startup_save(bundle,length)==OS64_DECOR_STARTUP_IO);fail_sync=false;
    install_expect(next,next_length);
    fail_rename=true;assert(os64_decor_startup_save(bundle,length)==OS64_DECOR_STARTUP_IO);fail_rename=false;
    install_expect(next,next_length);
    fail_rename=true;assert(os64_decor_startup_save(NULL,0)==OS64_DECOR_STARTUP_IO);fail_rename=false;
    install_expect(next,next_length);
    session_generation=0;fail_read=true;
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_IO && !session_generation);fail_read=false;
    refuse_apply=true;
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_IO && !session_generation);refuse_apply=false;
    race_apply=true;
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_SKIPPED && session_generation==7);race_apply=false;
    char path[256],lower[256];
    snprintf(path,sizeof(path),"%s/decoration.startup",directory);
    snprintf(lower,sizeof(lower),"%s/lower.startup",directory);
    assert(!rename(path,lower));
    install_expect(next,next_length); /* Lower layer is visible without an override. */
    before=apply_calls;
    assert(os64_decor_startup_save(NULL,0)==0 && session_generation==1 && before==apply_calls);
    session_generation=0;
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_DEFAULT && !session_generation && before==apply_calls);
    assert(os64_decor_startup_save(bundle,length)==0);
    int fd=open(path,O_RDWR);assert(fd>=0);uint8_t byte;
    assert(lseek(fd,-1,SEEK_END)>0 && read(fd,&byte,1)==1);
    byte^=1;assert(lseek(fd,-1,SEEK_END)>0 && write(fd,&byte,1)==1);close(fd);
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_INVALID && !session_generation && before==apply_calls);
    /* A damaged higher layer must not silently select a different lower look. */
    fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0 && write(fd,"broken",6)==6);close(fd);
    assert(os64_decor_startup_install()==OS64_DECOR_STARTUP_INVALID && !session_generation);
    assert(!unlink(path) && !unlink(lower));
    DIR *d=opendir(directory);assert(d);struct dirent *e;
    while((e=readdir(d)))assert(!strstr(e->d_name,".new"));
    closedir(d);
    puts("startup decoration: PASS next-boot isolation, embedded snapshots, replacement failures, fallback/default masking, corrupt data, generation-zero race");
}
static void test_delete(const frame_draft_t *draft,const void *bundle,size_t length)
{
    assert(frame_save("Blue grain",draft,bundle,length,false)==FRAME_STORE_OK);
    assert(frame_save("A second look",draft,bundle,length,false)==FRAME_STORE_OK);
    assert(os64_decor_startup_save(bundle,length)==0);
    frame_draft_t loaded;void *copy=NULL;size_t copy_length=0;
    assert(frame_load("Blue grain",&loaded,&copy,&copy_length)==FRAME_STORE_OK);
    session_generation=9;unsigned publications=apply_calls,removals=unlink_calls;
    assert(frame_delete(NULL)==FRAME_STORE_INVALID);
    assert(frame_delete("../decoration.startup")==FRAME_STORE_INVALID);
    assert(frame_delete(" Blue grain")==FRAME_STORE_INVALID && unlink_calls==removals);
    fail_unlink=true;assert(frame_delete("Blue grain")==FRAME_STORE_IO);fail_unlink=false;
    unchanged(draft,bundle,length);
    assert(frame_delete("Blue grain")==FRAME_STORE_OK);
    assert(session_generation==9 && apply_calls==publications);
    assert(frame_same(draft,&loaded) && copy_length==length && !memcmp(bundle,copy,length));
    frame_saved_t names[4];assert(frame_saved_list(names,4)==1 && !strcmp(names[0].name,"A second look"));
    assert(frame_delete("Blue grain")==FRAME_STORE_IO);
    /* The startup bundle survives removal of its source composition. */
    install_expect(bundle,length);
    char path[256];snprintf(path,sizeof(path),"%s/A second look.frame",directory);
    int fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0 && write(fd,"broken",6)==6);close(fd);
    assert(frame_delete("A second look")==FRAME_STORE_OK);
    assert(frame_saved_list(names,4)==0);
    /* os64's unlink also removes empty directories; refuse a substituted one. */
    assert(!mkdir(path,0700));removals=unlink_calls;
    assert(frame_delete("A second look")==FRAME_STORE_IO && unlink_calls==removals);
    assert(!rmdir(path));
    snprintf(path,sizeof(path),"%s/decoration.startup",directory);assert(!unlink(path));
    os64_free(copy);
    puts("frame deletion: PASS name confinement, refusal preservation, exact collection removal, corrupt files, directory refusal, retained loaded assets and startup snapshot, session isolation");
}
static void test_active(const frame_draft_t *draft,const void *bundle,size_t length)
{
    frame_saved_t names[4];frame_draft_t loaded=*draft;
    void *out=NULL;size_t size=0,selected=99;uint64_t generation=99;
    active_status=(os64_decor_status_t){7,os64_decor_fingerprint(bundle,length),(uint32_t)length};
    assert(frame_save("Z duplicate",draft,bundle,length,false)==FRAME_STORE_OK);
    assert(frame_save("A match",draft,bundle,length,false)==FRAME_STORE_OK);
    char path[256];snprintf(path,sizeof(path),"%s/0 broken.frame",directory);
    int fd=open(path,O_WRONLY|O_CREAT,0600);assert(fd>=0 && write(fd,"broken",6)==6);close(fd);
    int count=frame_saved_list(names,4);assert(count==3);
    unsigned publications=apply_calls;
    assert(frame_load_active(names,count,&selected,&loaded,&out,&size,&generation)==1);
    assert(selected==1 && generation==7 && size==length && !memcmp(out,bundle,size));
    assert(frame_same(draft,&loaded));os64_free(out);
    /* A changed live generation must not establish a saved baseline. */
    change_during_lookup=true;status_reads=0;selected=99;generation=99;
    assert(!frame_load_active(names,count,&selected,&loaded,&out,&size,&generation));
    assert(!out && !size && selected==99 && generation==99 && frame_same(draft,&loaded));
    change_during_lookup=false;
    fail_status=true;
    assert(!frame_load_active(names,count,&selected,&loaded,&out,&size,&generation));fail_status=false;
    ++active_status.fingerprint;
    assert(!frame_load_active(names,count,&selected,&loaded,&out,&size,&generation));
    --active_status.fingerprint;--active_status.bytes;
    assert(!frame_load_active(names,count,&selected,&loaded,&out,&size,&generation));
    active_status.bytes=0;
    assert(!frame_load_active(names,count,&selected,&loaded,&out,&size,&generation));
    active_status.bytes=(uint32_t)length;
    fail_read=true;
    assert(!frame_load_active(names,count,&selected,&loaded,&out,&size,&generation));fail_read=false;
    assert(frame_delete("A match")==FRAME_STORE_OK);
    /* A missing entry is skipped; the next identical composition wins. */
    assert(frame_load_active(names,count,&selected,&loaded,&out,&size,&generation)==1 && selected==2);
    os64_free(out);
    assert(frame_delete("Z duplicate")==FRAME_STORE_OK);
    assert(!frame_load_active(names,count,&selected,&loaded,&out,&size,&generation));
    assert(apply_calls==publications && !out && !size);
    assert(!unlink(path));
    for(unsigned version=4;version<=5;++version){
        size_t legacy_length;void *legacy=legacy_bundle(bundle,length,version,&legacy_length);
        active_status=(os64_decor_status_t){8,os64_decor_fingerprint(legacy,legacy_length),(uint32_t)legacy_length};
        assert(active_status.fingerprint!=os64_decor_fingerprint(bundle,length));
        assert(frame_save("Legacy",draft,legacy,legacy_length,false)==FRAME_STORE_OK);
        count=frame_saved_list(names,4);assert(count==1);
        assert(frame_load_active(names,count,&selected,&loaded,&out,&size,&generation)==1);
        assert(generation==8 && size==legacy_length && !memcmp(out,legacy,size));
        os64_free(out);os64_free(legacy);assert(frame_delete("Legacy")==FRAME_STORE_OK);
    }
    puts("frame active match: PASS V4/V5/V6 bundles, sorted duplicates, missing/corrupt entries, exact contents/length, legacy/default, read failure, racing Apply, no publication");
}
static void test_included(const frame_draft_t *draft,const void *bundle,size_t length)
{
    strcpy(included_directory,"/tmp/os64-included-XXXXXX");assert(mkdtemp(included_directory));
    void *file=NULL;size_t bytes=0;
    assert(frame_encode(draft,bundle,length,&file,&bytes)==FRAME_STORE_OK);
    char path[256];snprintf(path,sizeof(path),"%s/Included.frame",included_directory);
    FILE *f=fopen(path,"wb");assert(f && fwrite(file,1,bytes,f)==bytes && !fclose(f));os64_free(file);
    frame_saved_t names[4];assert(frame_saved_list(names,4)==1 && names[0].included);
    frame_saved_t selected=names[0];frame_draft_t loaded;void *copy=NULL;size_t size=0;
    assert(frame_load_entry(&selected,&loaded,&copy,&size)==FRAME_STORE_OK);
    assert(frame_same(draft,&loaded) && size==length && !memcmp(copy,bundle,length));os64_free(copy);
    assert(frame_delete("Included")==FRAME_STORE_IO && !access(path,F_OK));
    const char *aliases[]={FRAME_INCLUDED_DIR,"/etc//./frames/","/home/../etc/frames","../etc/frames"};
    for(size_t i=0;i<sizeof(aliases)/sizeof(aliases[0]);++i){
        frames_target=aliases[i];
        assert(frame_saved_list(names,4)==1 && names[0].included);
        assert(frame_save("Included",draft,bundle,length,true)==FRAME_STORE_INVALID);
        assert(frame_delete("Included")==FRAME_STORE_INVALID && !access(path,F_OK));
    }
    frames_target=NULL;
    fail_status=false;change_during_lookup=false;status_reads=0;
    active_status=(os64_decor_status_t){9,os64_decor_fingerprint(bundle,length),(uint32_t)length};
    size_t index=7;uint64_t generation=0;
    assert(frame_load_active(names,1,&index,&loaded,&copy,&size,&generation)==1 && index==0 && generation==9);
    os64_free(copy);
    assert(frame_save("Included",draft,bundle,length,false)==FRAME_STORE_OK);
    assert(frame_saved_list(names,4)==1 && !names[0].included);
    selected=names[0];
    char personal[256];snprintf(personal,sizeof(personal),"%s/Included.frame",directory);
    f=fopen(personal,"wb");assert(f && fwrite("broken",1,6,f)==6 && !fclose(f));
    assert(frame_load_entry(&selected,&loaded,&copy,&size)==FRAME_STORE_INVALID && !copy && !size);
    assert(frame_delete("Included")==FRAME_STORE_OK);
    assert(frame_load_entry(&selected,&loaded,&copy,&size)==FRAME_STORE_IO && !copy && !size);
    assert(frame_saved_list(names,4)==1 && names[0].included);
    assert(frame_load_entry(&names[0],&loaded,&copy,&size)==FRAME_STORE_OK);os64_free(copy);
    assert(frame_save("Personal",draft,bundle,length,false)==FRAME_STORE_OK);
    assert(frame_saved_list(names,1)==-FRAME_STORE_LIMIT);
    assert(frame_saved_list(names,4)==2 && names[0].included && !names[1].included);
    assert(frame_delete("Personal")==FRAME_STORE_OK);
    assert(!unlink(path));
    assert(frame_load_entry(&names[0],&loaded,&copy,&size)==FRAME_STORE_IO && !copy && !size);
    assert(!rmdir(included_directory));included_directory[0]=0;
    assert(frame_saved_list(names,4)==0);
    puts("included frames: PASS discovery, source-pinned load, active match, personal shadow, corrupt personal refusal, deletion reveal, config aliases and bounds");
}

void test_frame_storage(const frame_draft_t *draft,const void *bundle,size_t length)
{
    strcpy(directory,"/tmp/os64-frames-XXXXXX");assert(mkdtemp(directory));
    assert(frame_name_valid("Blue grain & stripes (2)"));
    assert(!frame_name_valid("../outside") && !frame_name_valid(" blue") && !frame_name_valid("blue "));
    char long_name[42];memset(long_name,'a',41);long_name[41]=0;assert(!frame_name_valid(long_name));
    void *file=NULL;size_t file_size=0;
    assert(frame_encode(draft,bundle,length,&file,&file_size)==FRAME_STORE_OK);
    assert(file_size==length+FRAME_FILE_HEADER_BYTES);
    frame_draft_t loaded;void *out=NULL;size_t size=0;
    assert(frame_decode(file,file_size,&loaded,&out,&size)==FRAME_STORE_OK);
    assert(frame_same(draft,&loaded) && size==length && !memcmp(bundle,out,length));os64_free(out);
    for(size_t i=0;i<file_size;i+=file_size/19+1){
        loaded=*draft;out=(void *)1;size=1;
        assert(frame_decode(file,i,&loaded,&out,&size)==FRAME_STORE_INVALID && !out && !size && frame_same(draft,&loaded));
    }
    uint8_t *corrupt=malloc(file_size);assert(corrupt);memcpy(corrupt,file,file_size);
    corrupt[file_size-1]^=1;
    assert(frame_decode(corrupt,file_size,&loaded,&out,&size)==FRAME_STORE_INVALID && !out && !size);
    free(corrupt);os64_free(file);
    assert(frame_save("../outside",draft,bundle,length,false)==FRAME_STORE_INVALID);
    assert(frame_save("Blue grain",draft,bundle,length,false)==FRAME_STORE_OK);
    unchanged(draft,bundle,length);
    frame_draft_t changed=*draft;changed.style.symbols[0]=(os64_decor_symbol_t){0xff000000,0xff223344};changed.style.active_face2^=0x0033aa55;
    void *next=NULL;size_t next_length=0;
    assert(os64_decor_restyle(bundle,length,&changed.style,&next,&next_length)==OS64_FONT_OK);
    assert(frame_save("Blue grain",&changed,next,next_length,false)==FRAME_STORE_EXISTS);
    unchanged(draft,bundle,length);
    unsigned before=rename_calls;
    fail_write=true;assert(frame_save("Blue grain",&changed,next,next_length,true)==FRAME_STORE_IO);fail_write=false;
    assert(before==rename_calls);unchanged(draft,bundle,length);
    fail_sync=true;assert(frame_save("Blue grain",&changed,next,next_length,true)==FRAME_STORE_IO);fail_sync=false;
    assert(before==rename_calls);unchanged(draft,bundle,length);
    fail_rename=true;assert(frame_save("Blue grain",&changed,next,next_length,true)==FRAME_STORE_IO);fail_rename=false;
    unchanged(draft,bundle,length);
    fail_read=true;loaded=*draft;assert(frame_load("Blue grain",&loaded,&out,&size)==FRAME_STORE_IO && !out && !size && frame_same(draft,&loaded));fail_read=false;
    assert(frame_save("Blue grain",&changed,next,next_length,true)==FRAME_STORE_OK);
    unchanged(&changed,next,next_length);
    frame_saved_t names[4];assert(frame_saved_list(names,4)==1 && !strcmp(names[0].name,"Blue grain"));
    assert(frame_save("A second look",draft,bundle,length,false)==FRAME_STORE_OK);
    assert(frame_saved_list(names,4)==2 && !strcmp(names[0].name,"A second look"));
    assert(frame_saved_list(names,1)==-FRAME_STORE_LIMIT);
    char path[256];snprintf(path,sizeof(path),"%s/Blue grain.frame",directory);
    int fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0 && write(fd,"broken",6)==6);close(fd);
    loaded=*draft;assert(frame_load("Blue grain",&loaded,&out,&size)==FRAME_STORE_INVALID && !out && !size && frame_same(draft,&loaded));
    unlink(path);snprintf(path,sizeof(path),"%s/A second look.frame",directory);unlink(path);
    test_startup(bundle,length,next,next_length);
    test_delete(draft,bundle,length);
    test_active(draft,bundle,length);
    test_included(draft,bundle,length);
    assert(!rmdir(directory));
    os64_free(next);
    puts("frame storage: PASS embedded-asset round trip, bounds/checksum, names, short I/O, write/sync/rename refusal, replacement, collection");
}
