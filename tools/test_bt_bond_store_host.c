#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../kernel/src/driver/system/usb/bt_bond.c"
#include "../kernel/src/driver/system/usb/bt_bond_store.c"

typedef struct { bool exists; uint8_t data[160]; size_t length; } test_file_t;
static test_file_t saved,temporary,saved_second,temporary_second;
static vfs_file_t handle;
static test_file_t *opened;
static size_t position;
static unsigned gate,renames;
static bool read_error,write_error,sync_error,close_error,rename_error,unreadable,demote_on_write;
static void *io_allocation;
static size_t io_bytes;
static bool kernel_context,allocation_error;
void *kmalloc_try(uint64_t n)
{ assert(!io_allocation); if(allocation_error) return NULL; io_bytes=n; return io_allocation=calloc(1,n); }
void kfree(void *p)
{
    assert(p==io_allocation && !kernel_context);
    for(size_t i=0;i<io_bytes;i++) assert(!((uint8_t *)p)[i]);
    free(p); io_allocation=NULL;
}
void call_in_kernel_context(void (*fn)(void *),void *arg)
{
    assert(arg==io_allocation && !kernel_context);
    kernel_context=true; fn(arg); kernel_context=false;
}
static int test_open(vfs_file_t **out,const char *path,const char *mode,vfs_filesystem_t *fs)
{
    (void)fs; assert(gate && !opened);
    bool second=strstr(path,"_1")!=NULL;
    test_file_t *f=strstr(path,".new")?(second?&temporary_second:&temporary):(second?&saved_second:&saved);
    if(mode[0]=='r' && (!f->exists || unreadable)) return -1;
    if(mode[0]=='x' && f->exists) return -1;
    if(mode[0]=='w' || mode[0]=='x') { f->exists=true; f->length=0; }
    opened=f; position=0; handle.owner=fs; *out=&handle; return 0;
}
static int test_read(vfs_file_t *f,void *out,size_t n)
{
    assert(gate && f==&handle && opened);
    if(read_error) return -1;
    if(n>7) n=7;
    if(n>opened->length-position) n=opened->length-position;
    memcpy(out,opened->data+position,n); position+=n; return (int)n;
}
static int test_write(vfs_file_t *f,const void *data,size_t n)
{
    assert(gate && f==&handle && opened);
    vfs_filesystem_t *fs=f->owner;
    if(write_error || fs->read_only) return -1;
    if(n>9) n=9;
    assert(position+n<=sizeof(opened->data));
    memcpy(opened->data+position,data,n); position+=n; opened->length=position;
    if(demote_on_write) {
        fs->read_only=true; fs->fops->write=NULL; fs->fops->sync=NULL;
        fs->fops->rename=NULL; fs->fops->rm=NULL;
    }
    return (int)n;
}
static int test_sync(vfs_file_t *f) { assert(gate && f==&handle); return sync_error?-1:0; }
static int test_close(vfs_file_t *f)
{ assert(gate && f==&handle && opened); opened=NULL; return close_error?-1:0; }
static int test_rename(const char *old,const char *new,vfs_filesystem_t *fs,uint64_t flags)
{
    (void)fs; assert(gate && !opened && strstr(old,".new") && !strstr(new,".new"));
    assert(flags==OS64_RENAME_REQUIRE_ATOMIC_REPLACE); renames++;
    if(rename_error) return -1;
    assert((strstr(old,"_1")!=NULL)==(strstr(new,"_1")!=NULL));
    if(strstr(old,"_1")) { saved_second=temporary_second; temporary_second=(test_file_t){0}; }
    else { saved=temporary; temporary=(test_file_t){0}; }
    return 0;
}
static int test_remove(const char *path,vfs_filesystem_t *fs)
{ assert(gate && strstr(path,".new")); if(fs->read_only) return -1; if(strstr(path,"_1")) temporary_second=(test_file_t){0}; else temporary=(test_file_t){0}; return 0; }
static vfs_file_operations_t operations={.open=test_open,.read=test_read,.write=test_write,
    .sync=test_sync,.close=test_close,.rename=test_rename,.rm=test_remove};
static vfs_filesystem_t filesystem={.fops=&operations};
void vfs_path_enter(void) { assert(!gate && kernel_context); gate=1; }
void vfs_path_exit(void) { assert(gate && !opened); gate=0; }
vfs_filesystem_t *vfs_resolve_mount(const char *path,const char **tail)
{ assert(gate && !strncmp(path,"/home/",6)); *tail=path; return &filesystem; }

int main(void)
{
    bt_le_bond_t bond={0},loaded; bool automatic;
    allocation_error=true; assert(!bt_bond_load(&loaded,&automatic) && !saved.exists);
    assert(!bt_bond_save(&bond,false)); allocation_error=false;
    assert(bt_bond_load(&loaded,&automatic) && saved.exists && !loaded.valid && !automatic);
    bond.valid=true; bond.address_type=1; bond.peer[5]=0xc1;
    bond.last_address_type=1; bond.last_peer[5]=0xc1; bond.has_irk=true;
    memset(bond.ltk,0x5a,16); memset(bond.irk,0xa5,16);
    assert(bt_bond_save(&bond,true) && !temporary.exists && renames==1);
    assert(bt_bond_load(&loaded,&automatic) && loaded.valid && automatic);
    assert(!memcmp(loaded.ltk,bond.ltk,16) && !memcmp(loaded.irk,bond.irk,16));
    test_file_t previous=saved;
    for(unsigned kind=0;kind<4;kind++) {
        write_error=kind==0; sync_error=kind==1; close_error=kind==2; rename_error=kind==3;
        bond.ltk[0]++;
        assert(!bt_bond_save(&bond,true) && !temporary.exists);
        assert(!memcmp(&saved,&previous,sizeof(saved)));
    }
    write_error=sync_error=close_error=rename_error=false;
    demote_on_write=true;
    assert(!bt_bond_save(&bond,true) && !memcmp(&saved,&previous,sizeof(saved)));
    demote_on_write=false; filesystem.read_only=false;
    operations.write=test_write; operations.sync=test_sync;
    operations.rename=test_rename; operations.rm=test_remove;
    temporary=(test_file_t){0};
    unreadable=true; assert(!bt_bond_load(&loaded,&automatic));
    assert(!memcmp(&saved,&previous,sizeof(saved))); unreadable=false;
    read_error=true; assert(!bt_bond_load(&loaded,&automatic)); read_error=false;
    close_error=true; assert(!bt_bond_load(&loaded,&automatic)); close_error=false;
    for(unsigned n=0;n<BT_BOND_RECORD_BYTES;n++) {
        saved.length=n; assert(!bt_bond_load(&loaded,&automatic));
    }
    saved=previous; saved.data[8]^=1; assert(!bt_bond_load(&loaded,&automatic));
    saved=previous; saved.length++; assert(!bt_bond_load(&loaded,&automatic));
    saved=previous; bond=(bt_le_bond_t){0};
    assert(bt_bond_save(&bond,false) && bt_bond_load(&loaded,&automatic));
    assert(!loaded.valid && !automatic);
    test_file_t slot_zero=saved;
    bond.valid=true; bond.address_type=1; bond.peer[5]=0xc2;
    bond.last_address_type=1; bond.last_peer[5]=0xc2; memset(bond.ltk,0x37,16);
    assert(bt_bond_load_slot(1,&loaded,&automatic) && !loaded.valid);
    assert(bt_bond_save_slot(1,&bond,true));
    assert(bt_bond_load_slot(1,&loaded,&automatic) && loaded.valid && automatic);
    assert(!memcmp(loaded.ltk,bond.ltk,16) && !memcmp(&slot_zero,&saved,sizeof(saved)));
    assert(bt_bond_load(&loaded,&automatic) && !loaded.valid && !automatic);
    test_file_t slot_one=saved_second;
    rename_error=true; assert(!bt_bond_save_slot(1,&bond,false)); rename_error=false;
    assert(!memcmp(&slot_one,&saved_second,sizeof(slot_one)) && !temporary_second.exists);
    assert(!bt_bond_load_slot(BT_BOND_SLOTS,&loaded,&automatic));
    assert(!bt_bond_save_slot(BT_BOND_SLOTS,&bond,false));
    puts("PASS: independent slot records, slot-local atomic replacement and invalid slot refusal");
    puts("PASS: production bond store, short I/O, exclusive creation, atomic replacement, corruption and forget tombstone");
    return 0;
}
