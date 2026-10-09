#include "driver/system/usb/bt_manager.h"
#include "driver/filesystem/vfs/vfs.h"
#include "os64/syscall_numbers.h"
#include "crypto/wipe.h"
#include "memory/kmalloc.h"
#include "memory/vma.h"

// Machine state follows Os64's SSH host-key storage model: local programs are
// trusted. This is not a file-permission or at-rest encryption boundary.
static const char *const bond_paths[BT_BOND_SLOTS]={"/home/bluetooth_bond","/home/bluetooth_bond_1"};
static const char *const bond_temps[BT_BOND_SLOTS]={"/home/bluetooth_bond.new","/home/bluetooth_bond_1.new"};

static bool bond_write(const vfs_file_operations_t *ops,vfs_file_t *file,const uint8_t *record)
{
    size_t done=0; bool good=true;
    while(done<BT_BOND_RECORD_BYTES) {
        int n=ops->write(file,record+done,BT_BOND_RECORD_BYTES-done);
        if(n<=0 || (size_t)n>BT_BOND_RECORD_BYTES-done) { good=false; break; }
        done+=(size_t)n;
    }
    if(good && ops->sync(file)!=0) good=false;
    if(ops->close(file)!=0) good=false;
    return good;
}
static bool bond_load_io(unsigned slot,bt_le_bond_t *bond,bool *automatic)
{
    uint8_t record[BT_BOND_RECORD_BYTES+1]; bool good=false;
    crypto_wipe(bond,sizeof(*bond)); *automatic=false;
    vfs_path_enter();
    const char *tail=NULL; vfs_filesystem_t *fs=vfs_resolve_mount(bond_paths[slot],&tail);
    if(!fs || !fs->fops) goto out;
    // Demotion clears writable slots concurrently. Retained callbacks check
    // read_only under the filesystem lock; reloading a cleared slot could fault.
    vfs_file_operations_t ops=*fs->fops;
    if(!ops.open || !ops.close || !ops.read) goto out;
    vfs_file_t *file=NULL;
    if(ops.open(&file,tail,"r",fs)!=0) {
        // Like SSH key creation, exclusive open distinguishes absence from an
        // unreadable existing record without replacing the latter.
        if(!ops.write || !ops.sync || ops.open(&file,tail,"x",fs)!=0) goto out;
        bt_bond_encode(record,bond,false); good=bond_write(&ops,file,record); goto out;
    }
    size_t used=0; bool eof=false;
    while(used<sizeof(record)) {
        int n=ops.read(file,record+used,sizeof(record)-used);
        if(n<0 || (size_t)n>sizeof(record)-used) break;
        if(!n) { eof=true; break; }
        used+=(size_t)n;
    }
    int closed=ops.close(file);
    good=eof && !closed && bt_bond_decode(bond,automatic,record,used);
out:
    vfs_path_exit(); crypto_wipe(record,sizeof(record)); return good;
}
static bool bond_save_io(unsigned slot,const bt_le_bond_t *bond,bool automatic)
{
    uint8_t record[BT_BOND_RECORD_BYTES]; bool good=false;
    bt_bond_encode(record,bond,automatic);
    vfs_path_enter();
    const char *tail=NULL,*temporary=NULL;
    vfs_filesystem_t *fs=vfs_resolve_mount(bond_paths[slot],&tail);
    vfs_filesystem_t *temp_fs=vfs_resolve_mount(bond_temps[slot],&temporary);
    if(!fs || fs!=temp_fs || !fs->fops) goto out;
    vfs_file_operations_t ops=*fs->fops;
    if(!ops.open || !ops.close || !ops.write || !ops.sync || !ops.rename || !ops.rm) goto out;
    vfs_file_t *file=NULL;
    if(ops.open(&file,temporary,"w",fs)!=0) goto out;
    if(bond_write(&ops,file,record))
        good=ops.rename(temporary,tail,fs,OS64_RENAME_REQUIRE_ATOMIC_REPLACE)==0;
    if(!good) (void)ops.rm(temporary,fs);
out:
    vfs_path_exit(); crypto_wipe(record,sizeof(record)); return good;
}

typedef struct {
    bt_le_bond_t bond;
    bool loading,automatic,good;
    unsigned slot;
} bond_io_t;
static void bond_io_kernel(void *arg)
{
    bond_io_t *io=arg;
    io->good=io->loading?bond_load_io(io->slot,&io->bond,&io->automatic):bond_save_io(io->slot,&io->bond,io->automatic);
}
static bool bond_transfer(unsigned slot,const bt_le_bond_t *input,bt_le_bond_t *output,bool *automatic)
{
    // kworker has its own CR3 and stack. Disk callbacks need kernel mappings;
    // marshalled arguments live in the HHDM across the trampoline's stack swap.
    if(slot>=BT_BOND_SLOTS) return false;
    bond_io_t *io=kmalloc_try(sizeof(*io));
    if(!io) return false;
    io->slot=slot; io->loading=output!=NULL; io->automatic=*automatic;
    if(input) io->bond=*input;
    call_in_kernel_context(bond_io_kernel,io);
    bool good=io->good;
    if(good && output) { *output=io->bond; *automatic=io->automatic; }
    crypto_wipe(io,sizeof(*io)); kfree(io); return good;
}
bool bt_bond_load_slot(unsigned slot,bt_le_bond_t *bond,bool *automatic)
{
    crypto_wipe(bond,sizeof(*bond)); *automatic=false;
    return bond_transfer(slot,NULL,bond,automatic);
}
bool bt_bond_save_slot(unsigned slot,const bt_le_bond_t *bond,bool automatic)
{ return bond_transfer(slot,bond,NULL,&automatic); }
bool bt_bond_load(bt_le_bond_t *bond,bool *automatic)
{ return bt_bond_load_slot(0,bond,automatic); }
bool bt_bond_save(const bt_le_bond_t *bond,bool automatic)
{ return bt_bond_save_slot(0,bond,automatic); }
