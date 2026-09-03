#include "fat_glue.h"
#include "ff.h"
#include "vfs.h"
#include "block_device.h"
#include "sprintf.h"
#include "panic.h"
#include "kmalloc.h"
#include "strings.h"
#include "memcpy.h"
#include "time.h"  // For time conversion functions
#include "spinlock.h"  // ff_mutex_* reentrancy hooks (FF_FS_REENTRANT)
#include "serial_logging.h"  // printd — the read-only-device refusal in disk_write

extern uint64_t kSystemCurrentTime; // Your kernel's epoch time variable

vfs_filesystem_t* vfs_get_device_by_fat_disk_number(uint8_t fatDiskNumber)
{
	// FAT_DISK_NONE marks every non-FAT filesystem in the dlist (vfs.h). It
	// must never be matchable: resolving it would hand FatFs a filesystem —
	// and a partition base sector — that was never FAT's to touch. Real FAT
	// disk numbers are 1-based, so a pdrv of 0 (uninitialized/leaked) falls
	// through to NULL and the callers' "Cannot find vfs device" panic names
	// the bug instead of letting the write land on the root partition.
	if (fatDiskNumber == FAT_DISK_NONE)
		return NULL;

	if (kBlockDeviceDList == NULL)
	{
		return NULL;
	}

	for (dlist_node_t* node = kBlockDeviceDList->head; node != NULL; node = node->next)
	{
		vfs_filesystem_t* vfsdev = (vfs_filesystem_t*)node->data;
		if (vfsdev != NULL && vfsdev->fatDiskNumber == fatDiskNumber)
		{
			return vfsdev;
		}
	}
	return NULL;
}

/***********               DISK LEVEL METHODS               ************/

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
	(void) pdrv;
	return 0;
}


/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber to identify the drive */
)
{
	(void) pdrv;
	return 0;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
	vfs_filesystem_t* fs = vfs_get_device_by_fat_disk_number(pdrv);

	if (fs==NULL)
		panic("fat disk_read: Cannot find vfs device for disk number %u\n",pdrv);

	return fs->bops->read(fs->block_device_info, 
				fs->block_device_info->block_device->partition_table->parts[fs->partNumber]->partStartSector + sector, buff, count);
	}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/
DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
	vfs_filesystem_t* fs = vfs_get_device_by_fat_disk_number(pdrv);

	if (fs==NULL)
		panic("fat disk_write: Cannot find vfs device for disk number %u\n",pdrv);

	// A driver that never installed a write op is READ-ONLY, and saying so is
	// the whole job here. Without this check the call below dispatches through
	// a NULL function pointer — and because VA 0 is still mapped in os64, that
	// does not fault cleanly: the CPU happily EXECUTES whatever bytes live at
	// address 0 and runs off into hyperspace. That is precisely how the first
	// boot with a /home partition on the AHCI disk died (2026-08-02): AHCI
	// installs ops->read and not ops->write, kmalloc zeroes everything, and a
	// single `echo > /home/proof.txt` took the machine down with RIP=0x3 and an
	// unkillable #GP storm. RES_WRPRT is FatFs's own word for it — the write
	// fails, the OS lives, and the message names the device.
	if (fs->bops == NULL || fs->bops->write == NULL)
	{
		printd(DEBUG_VFS, "fat disk_write: device %u ('%s') has no write op — read-only device, refusing %u sector(s) at LBA %lu\n",
		       pdrv, fs->block_device_info->block_device->name, count, (uint64_t)sector);
		return RES_WRPRT;
	}

	// Propagate the driver's verdict (0 = success, like disk_read does).
	// Swallowing it here told FatFs its metadata was safely on disk when the
	// driver had just said otherwise — on the ramdisk that's invisible, on
	// real hardware it's how filesystems die.
	return fs->bops->write(fs->block_device_info,
	fs->block_device_info->block_device->partition_table->parts[fs->partNumber]->partStartSector + sector, buff, count);
}

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	vfs_filesystem_t* fs = vfs_get_device_by_fat_disk_number(pdrv);

	// The NULL check must come BEFORE the partition lookup derefs fs (it used
	// to come after, which made it decoration).
	if (fs==NULL)
		panic("fat disk_ioctl: Cannot find vfs device for disk number %u\n",pdrv);

	partEntry_t* partition = fs->block_device_info->block_device->partition_table->parts[fs->partNumber];

	// Out-parameter widths are FatFs's contract, not ours: LBA_t (4 bytes with
	// FF_LBA64=0) for the count, WORD for the sector size, DWORD for the block
	// size. Writing 2 bytes into a DWORD out-param leaves the top half as
	// whatever was on the caller's stack — GET_BLOCK_SIZE used to do exactly
	// that.
	uint16_t sector_size = 0;
	uint32_t block_size = 0;

	switch(cmd)
	{
		case CTRL_SYNC:
			break;
		case GET_SECTOR_COUNT:
			memcpy(buff, &partition->partTotalSectors, sizeof(partition->partTotalSectors));
			break;
		case GET_SECTOR_SIZE:
			sector_size = DEFAULT_SECTOR_SIZE;
			memcpy(buff, &sector_size, sizeof(sector_size));
			break;
		case GET_BLOCK_SIZE:
			block_size = 1;   // erase-block granularity unknown: 1 = "no alignment hint"
			memcpy(buff, &block_size, sizeof(block_size));
			break;
		case CTRL_TRIM:
			break;
	}
	return 0;
}

/***********                  MISC METHODS                  ************/

// ── FatFs reentrancy hooks (FF_FS_REENTRANT) ───────────────────────────────
//
// Before these existed, FatFs ran with NO synchronization at all — and every
// FATFS object carries a shared sector-window buffer (fs->win) that every
// operation on that volume reads AND writes. Two cores inside f_read/f_open
// on the same volume at the same time silently corrupt each other's view of
// the FAT. That was survivable while file I/O was a boot-time, one-core
// affair; it stopped being survivable the moment the open() syscall let any
// two ring-3 tasks (on any two cores) do file I/O concurrently — plus the
// demand pager, which f_reads ELF pages from page-fault context whenever it
// pleases.
//
// FatFs asks for one mutex per volume, plus one "system" slot at index
// FF_VOLUMES (only exercised when FF_FS_LOCK > 0 — with FF_FS_LOCK == 0
// lock_volume() takes exactly ONE mutex per API call, no nesting, no
// re-take, so a non-recursive lock is safe here).
//
// Why a spinlock and not something fancier: this is the allocator's own
// irqsave idiom (spinlock.h), and it matches how file I/O actually runs
// today — syscall bodies already execute with IF=0 (SFMASK), so holding a
// spinlock across a FatFs call changes nothing for them. The known cost: a
// long disk operation holds the lock (and the core's interrupts) for the
// whole transfer, so a second core wanting the same volume spins. When the
// interruptible-syscall-bodies debt is paid, this should graduate to a
// sleeping mutex — the hook shape won't change, only these four bodies.
//
// The saved-RFLAGS slot is per-volume and written only AFTER the lock is
// held (acquire returns the flags) and read only BEFORE it is released, so
// hand-off between cores can't clobber a waiter's flags.
static spinlock_t ff_volume_locks[FF_VOLUMES + 1];
static uint64_t   ff_volume_lock_flags[FF_VOLUMES + 1];

int ff_mutex_create (int vol)   /* 1: created, 0: failed */
{
	ff_volume_locks[vol] = 0;
	return 1;
}

void ff_mutex_delete (int vol)
{
	ff_volume_locks[vol] = 0;
}

int ff_mutex_take (int vol)     /* 1: got it, 0: timeout — we spin, so never 0 */
{
	ff_volume_lock_flags[vol] = spinlock_acquire_irqsave(&ff_volume_locks[vol]);
	return 1;
}

void ff_mutex_give (int vol)
{
	spinlock_release_irqrestore(&ff_volume_locks[vol], ff_volume_lock_flags[vol]);
}

void* ff_memalloc (	/* Returns pointer to the allocated memory block (null if not enough core) */
	UINT msize		/* Number of bytes to allocate */
)
{
	return kmalloc((size_t)msize);	/* Allocate a new memory block */
}

void ff_memfree (
	void* mblock	/* Pointer to the memory block to free (no effect if null) */
)
{
	kfree(mblock);	/* Free the memory block */
}

DWORD get_fattime(void) {
    // Convert epoch time (kSystemCurrentTime) to calendar time
    time_t raw_time = (time_t)kSystemCurrentTime; // Cast epoch time
    struct tm *timeinfo = kmalloc(sizeof(struct tm));
	gmtime(&raw_time, timeinfo);      // Convert to UTC time

    if (!timeinfo) {
        // Return default time if conversion fails (e.g., 1980-01-01 00:00:00)
        return ((DWORD)(1980 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
    }

    // Pack the time components into a DWORD
    DWORD fattime = 0;
    fattime |= ((DWORD)(timeinfo->tm_year - 80) << 25); // Year since 1980
    fattime |= ((DWORD)(timeinfo->tm_mon + 1) << 21);   // Month (0–11 -> 1–12)
    fattime |= ((DWORD)(timeinfo->tm_mday) << 16);      // Day (1–31)
    fattime |= ((DWORD)(timeinfo->tm_hour) << 11);      // Hour (0–23)
    fattime |= ((DWORD)(timeinfo->tm_min) << 5);        // Minute (0–59)
    fattime |= ((DWORD)(timeinfo->tm_sec / 2));         // Second (0–59, divided by 2)

	kfree(timeinfo);

    return fattime;
}

void create_fat_path(char* fsPath, vfs_filesystem_t* vfs_fs)
{
	char tempPath[255];

	strncpy(tempPath, fsPath, 255);
	sprintf(fsPath,"%u:%s", vfs_fs->fatDiskNumber, tempPath);

}

/***********             PARTITION LEVEL METHODS            ************/

static int fat_open (vfs_file_t** vfs_file, const char* path, const char* mode, vfs_filesystem_t* vfs_fs)
{
    BYTE fat_mode = 0;

	// Validate the mode BEFORE allocating anything. The old fall-through left
	// fat_mode = 0 for an unrecognized string — an open with no access flags,
	// which "succeeds" and then can't read, a miserable thing to debug from
	// the far side of a syscall. ("c" predates "w" here and is byte-identical
	// to it; kept for the existing callers/tests.)
    if (strcmp(mode, "r") == 0) fat_mode = FA_READ;
    else if (strcmp(mode, "u") == 0) fat_mode = FA_READ | FA_WRITE;
    else if (strcmp(mode, "w") == 0) fat_mode = FA_WRITE | FA_CREATE_ALWAYS;
    else if (strcmp(mode, "a") == 0) fat_mode = FA_WRITE | FA_OPEN_APPEND;
	else if (strcmp(mode, "c") == 0) fat_mode = FA_WRITE | FA_CREATE_ALWAYS;
	else
		return -1;

	// A READ-ONLY MOUNT REFUSES A MUTATING OPEN AT THE DOOR, the way
	// ext2_open_rw does. FAT needs it more: an f_open carrying
	// FA_CREATE_ALWAYS or FA_OPEN_APPEND WRITES — a directory entry, a
	// truncated cluster chain — so the open is the mutation and there is no
	// later dispatch site for a missing write verb to refuse at. Uncovered
	// that is a ring-3 kernel panic rather than a leak: the metadata write
	// lands on a partition the block tripwire knows is mounted read-only,
	// and that tripwire panics by design.
	//
	// Written as an ALLOW-list of the two flags that only ever read or
	// position, so any other flag — including one this driver does not use
	// today — is refused by default rather than by having been remembered
	// here. (FA_WRITE passes: "u" neither creates nor truncates, and the
	// NULL write verb is what stops its writes.)
	if (vfs_fs->read_only && (fat_mode & ~(BYTE)(FA_READ | FA_WRITE)) != 0)
		return -1;

	FIL* fat_file = kmalloc(sizeof(FIL));  // FIL object (FAT filesystem file handle)
	*vfs_file = kmalloc(sizeof(vfs_file_t));
	if (fat_file == NULL || *vfs_file == NULL)
	{
		// NOTE: these guards predate 2026-08-06, when kfree(NULL) genuinely
		// panicked (free_memory couldn't find address 0). kfree is a proper
		// C-convention no-op on NULL now — the guards stay as harmless
		// belt-and-suspenders, but new code need not copy them.
		if (fat_file != NULL)
			kfree(fat_file);
		if (*vfs_file != NULL)
			kfree(*vfs_file);
		*vfs_file = NULL;
		return -1;
	}

	char lPath[255];
	strncpy(lPath, path, 255);
	create_fat_path(lPath, vfs_fs);

    if (f_open(fat_file, lPath, fat_mode) != FR_OK) {
		// Failure must give back what it took — this path used to leak both
		// allocations on every ENOENT (i.e. on every PATH-typo in the shell).
		kfree(fat_file);
		kfree(*vfs_file);
		*vfs_file = NULL;
        return -1; // Error
    }

    (*vfs_file)->handle = fat_file;
	(*vfs_file)->f_path = (void*)path;
	// FAT's file identity (vfs.h f_ident): the start cluster, which is where a
	// FAT file's chain — and so the file itself — begins. Weaker than an inode
	// in principle (FAT recycles cluster numbers freely, and an empty file has
	// no cluster at all, reporting 0 = "cannot identify"), sound in practice
	// for as long as this handle lives: FF_FS_LOCK refuses to unlink a file
	// somebody has open, so the chain cannot be freed and the number cannot be
	// handed to anything else.
	(*vfs_file)->f_ident = (uint64_t)fat_file->obj.sclust;
	(*vfs_file)->fops = vfs_fs != NULL ? vfs_fs->fops : &fat_fops;
	(*vfs_file)->owner = vfs_fs;

	// Thread onto the VFS open-file registry — what lets sync(1) reach this
	// file while it's still open. FAT is the registry's whole reason to
	// exist: until f_sync/f_close, a written file's true length lives only
	// in this FIL, and every fresh open reads the stale directory entry.
	vfs_openfile_register(*vfs_file);
    return 0;
}

// FAT read wrapper
static int fat_read(vfs_file_t* vfs_file, void* buffer, size_t size) {
    UINT bytes_read;
    FIL* fat_file = (FIL*)vfs_file->handle;

    if (f_read(fat_file, buffer, size, &bytes_read) != FR_OK) {
        return -1; // Error
    }
    return bytes_read; // Return number of bytes read
}

#ifdef DISK_WRITING_ENABLED
// FAT write wrapper
static int fat_write(vfs_file_t* vfs_file, const void* buffer, size_t size) {
    UINT bytes_written;
    FIL* fat_file = (FIL*)vfs_file->handle;

    if (f_write(fat_file, buffer, size, &bytes_written) != FR_OK) {
        return -1; // Error
    }
    return bytes_written; // Return number of bytes written
}
#endif
// FAT close wrapper
static int fat_close(vfs_file_t* vfs_file) {
    FIL* fat_file = (FIL*)vfs_file->handle;

	// MARKED, not unlinked. A concurrent vfs_sync_all must never find a file
	// whose FIL is about to be freed — the mark is enough for that. Unlinking
	// here was too much: f_close below is real disk I/O on this volume, and a
	// file off the registry is a file unmount counts as gone, so it could free
	// the filesystem mid-f_close (vfs.h, the `closing` contract).
	vfs_openfile_mark_closing(vfs_file);

	// FREE ON BOTH PATHS. This used to `return -1` before the two kfrees, so a
	// failing close leaked the vfs_file_t AND the FIL — and nothing upstream
	// could clean up after it, because until rd14 nothing upstream even knew
	// the close had failed. Found while making that failure reportable.
	//
	// Freeing after a FAILED f_close is correct, not optimistic: FatFs does
	// not touch a FIL again once f_close has returned either way, and the
	// handle layer has already dropped its reference, so the alternative to
	// freeing is leaking forever. What the caller loses is the DATA, not the
	// bookkeeping — and it is now told so, loudly (handle.c).
	FRESULT fr = f_close(fat_file);
	// Done with the volume — off the registry, so an unmount waiting on this
	// file may proceed. Nothing below touches the filesystem.
	vfs_openfile_unregister(vfs_file);
	kfree(vfs_file); // Free the VFS file object
	kfree(fat_file);
	return (fr == FR_OK) ? 0 : -1;
}

static int fat_seek(vfs_file_t* vfs_file, long offset, int whence) {
    FIL* fat_file = (FIL*)vfs_file->handle;
    uint64_t base;

    // Calculate the new position based on whence
    switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = f_tell(fat_file);
            break;
        case SEEK_END:
            base = f_size(fat_file);
            break;
        default:
            return -1; // Invalid whence
    }

    // Avoid unsigned wrap: a negative displacement before byte zero must not
    // become a giant exFAT offset. The public position range is int64_t.
    if (base > INT64_MAX)
        return -1;
    int64_t signed_base = (int64_t)base;
    if ((offset > 0 && signed_base > INT64_MAX - offset) ||
        (offset < 0 && signed_base < INT64_MIN - offset))
        return -1;
    int64_t target = signed_base + offset;
    if (target < 0)
        return -1;

    if (f_lseek(fat_file, (FSIZE_t)target) != FR_OK) {
        return -1; // Seek failed
    }

    return 0; // Success
}

/// @brief Sync an open file to disk
/// @param vfs_file The handle of the file to sync
/// @return 0 on success, -1 on failure (the seam's neutral vocabulary)
static int fat_sync(vfs_file_t* vfs_file)
{
	// Normalize the FRESULT (2026-08-04). Returning it raw was a live bug:
	// FRESULT errors are POSITIVE (FR_DISK_ERR == 1), and syscall_sync only
	// treats rc < 0 as failure — so a FatFs sync error reported SUCCESS to
	// ring 3. The seam speaks 0/-1; the FatFs dialect stays inside the glue.
	return f_sync(vfs_file->handle) == FR_OK ? 0 : -1;
}

static char* fat_gets(vfs_file_t* vfs_file, char* buffer, int length)
{
	return f_gets(buffer, length, vfs_file->handle);
}

static int fat_puts(vfs_file_t* vfs_file, char* buffer)
{
	return f_puts(buffer, vfs_file->handle);
}

static int64_t fat_tell(vfs_file_t* vfs_file)
{
	FSIZE_t position = f_tell((FIL*)vfs_file->handle);
	return position <= INT64_MAX ? (int64_t)position : -1;
}

// Format with the KERNEL's printf engine, then hand FatFs finished bytes.
// The old body passed the va_list itself to f_printf as if it were the first
// variadic argument — any %-format printed the pointer value of the list, not
// the caller's data. (f_printf has no va_list variant, so formatting on our
// side is the correct division of labor anyway: one printf dialect in the
// kernel, not two.)
#define FAT_FPRINTF_MAX 512
static int fat_fprintf(vfs_file_t* vfs_file, const char* fmt, ...)
{
	char buffer[FAT_FPRINTF_MAX];
	va_list args;

	va_start(args, fmt);
	int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (len < 0)
		return -1;

	return f_puts(buffer, vfs_file->handle);
}

static int fat_initialize(vfs_filesystem_t* vfs_fs) {
    // Allocate FAT context
    FATFS* fat_fs = kmalloc(sizeof(FATFS));
	if (fat_fs == NULL)
		return -1;

	// All f_mount needs is the volume prefix ("N:"). This used to go through
	// create_fat_path on a kmalloc'd buffer, which only produced a clean "N:"
	// because every allocation in this kernel arrives zeroed (so the "path"
	// being prefixed was the empty string). Deliberate guarantee or not,
	// build-the-string-you-mean beats prefix-the-string-you-hope-is-empty.
	char drive_label[8];
	sprintf(drive_label, "%u:", vfs_fs->fatDiskNumber);

    // Mount the FATFS
    if (f_mount(fat_fs, drive_label, 1) != FR_OK) {
        kfree(fat_fs);   // the failure path used to leak the FATFS too
        return -1; // Failed to mount
    }

    // Store the context in the VFS filesystem. fs->fops is left ALONE: it is
    // the per-mount COPY kRegisterFilesystem made, and this function used to
    // overwrite it with &fat_fops — the GLOBAL table — which leaked the copy,
    // made vfs_demote_mount_readonly strip the global (demoting every FAT
    // mount at once), and handed unmount's teardown a kernel .data address to
    // kfree (the allocator names its victim; it named this).
    vfs_fs->fs_specific = fat_fs;
    return 0; // Success
}

static int fat_uninitialize(vfs_filesystem_t* vfs_fs)
{
	char drive_label[8];
	sprintf(drive_label, "%u:", vfs_fs->fatDiskNumber);

	int retVal = f_unmount(drive_label);
	kfree(vfs_fs->fs_specific);
	vfs_fs->fs_specific = NULL;   // a dangling context pointer is a re-mount trap
	return retVal;
}

/***********           DIRECTORY LEVEL METHODS          ************/
// FAT directory methods
static int fat_open_dir(vfs_directory_t** vfs_dir, const char* path, vfs_filesystem_t* vfs_fs)
{
	DIR* dir=kmalloc(sizeof(DIR));
	if (dir == NULL)
		return -1;

	char tempPath[255];
	strncpy(tempPath, path, 255);
	create_fat_path(tempPath, vfs_fs);

	if (f_opendir(dir, tempPath))
		{
			kfree(dir);
			return -1;
		}
	*vfs_dir = kmalloc(sizeof(vfs_directory_t));
	if (*vfs_dir == NULL)
	{
		f_closedir(dir);
		kfree(dir);
		return -1;
	}
	(*vfs_dir)->handle=dir;
	// Mirror fat_open: the caller's pointer lands in f_path (a caller wanting
	// handle lifetime must pass a string with handle lifetime — syscall_open
	// does), and dops/owner make the object self-describing so the handle
	// layer can close it without knowing which filesystem it came from.
	(*vfs_dir)->f_path = (char*)path;
	(*vfs_dir)->dops = vfs_fs != NULL ? vfs_fs->dops : &fat_dops;
	(*vfs_dir)->owner = vfs_fs;
	return 0;
}

static int fat_close_dir(vfs_directory_t* vfs_dir)
{
	int result = f_closedir(vfs_dir->handle);
	// Close frees what open allocated (this used to leak BOTH objects on
	// every directory listing).
	kfree(vfs_dir->handle);
	kfree(vfs_dir);
	return result;
}

// Fill one fs-neutral entry (see dops->read in vfs.h): FILINFO stays inside
// this driver, translated here. Returns 1 = entry, 0 = end, <0 = error.
// FAT's packed timestamp → epoch seconds (the dirent's mtime contract).
// The format is MS-DOS 1.25's, unchanged since 1982: fdate = Y7/M4/D5 with
// year counted from 1980, ftime = H5/M6/S5 with seconds HALVED (5 bits
// couldn't hold 60, so FAT has never been able to remember an odd second).
// Converted as-if-UTC via mktime_simple — FAT timestamps are wall-clock
// local by spec and carry no zone, so honesty means "the numbers on the
// label", not an invented conversion.
static uint64_t fat_mtime_epoch(WORD fdate, WORD ftime)
{
	if (fdate == 0)
		return 0;   // never stamped — 0 is the dirent's "no time to give"

	struct tm t;
	t.tm_year = ((fdate >> 9) & 0x7F) + 80;   // FAT's 1980 base → tm's 1900
	t.tm_mon  = ((fdate >> 5) & 0x0F) - 1;    // 1-12 → 0-11
	t.tm_mday =  fdate        & 0x1F;
	t.tm_hour = (ftime >> 11) & 0x1F;
	t.tm_min  = (ftime >> 5)  & 0x3F;
	t.tm_sec  = (ftime & 0x1F) * 2;           // the halved seconds
	return (uint64_t)mktime_simple(&t);
}

static int fat_read_dir(vfs_directory_t* vfs_dir, os64_dirent_t* entry)
{
	DIR* dir = vfs_dir->handle;
	FILINFO fi;

	if (f_readdir(dir, &fi) != FR_OK)
		return -1;
	if (fi.fname[0] == '\0')
		return 0;   // FatFs's end-of-directory signal, converted to ours

	strncpy(entry->name, fi.fname, OS64_DIRENT_NAME_MAX);
	entry->name[OS64_DIRENT_NAME_MAX] = '\0';
	entry->size = (fi.fattrib & AM_DIR) ? 0 : (uint64_t)fi.fsize;
	entry->flags = (fi.fattrib & AM_DIR) ? OS64_DE_DIR : 0;
	entry->mtime = fat_mtime_epoch(fi.fdate, fi.ftime);
	return 1;
}

static int fat_mkdir(char* path, vfs_filesystem_t* vfs_fs)
{
	char lPath[255];
	strncpy(lPath, path, 255);
	create_fat_path(lPath, vfs_fs);
	// Normalized to the seam's 0/-1 (2026-08-04, the day ext2 became the
	// second mkdir implementation and the neutral contract stopped being
	// theoretical). The raw FRESULT used to leak through here — test_main.c
	// had to include ff.h just to spell FR_EXIST. Failures are logged with
	// the FatFs-native code before it's flattened, same as fat_rm.
	FRESULT res = f_mkdir(lPath);
	if (res != FR_OK)
		printd(DEBUG_VFS, "fat_mkdir: f_mkdir('%s') failed, FRESULT=%u\n", lPath, res);
	return res == FR_OK ? 0 : -1;
}

// stat is readdir for exactly one name (dops->stat in vfs.h): same FILINFO →
// os64_dirent_t translation as fat_read_dir, sourced by f_stat instead of
// f_readdir.
static int fat_stat(const char* path, os64_dirent_t* entry, vfs_filesystem_t* vfs_fs)
{
	// FatFs refuses f_stat on the volume root BY DESIGN (the root directory
	// has no directory entry of its own to report — true on the actual disk,
	// not just in FatFs). Synthesize the answer instead: it's a directory.
	if (path[0] == '/' && path[1] == '\0')
	{
		entry->name[0] = '/';
		entry->name[1] = '\0';
		entry->size = 0;
		entry->flags = OS64_DE_DIR;
		entry->mtime = 0;   // the root has no dirent, so it truly has no time
		return 0;
	}

	char lPath[255];
	strncpy(lPath, path, 255);
	create_fat_path(lPath, vfs_fs);

	FILINFO fi;
	if (f_stat(lPath, &fi) != FR_OK)
		return -1;

	strncpy(entry->name, fi.fname, OS64_DIRENT_NAME_MAX);
	entry->name[OS64_DIRENT_NAME_MAX] = '\0';
	entry->size = (fi.fattrib & AM_DIR) ? 0 : (uint64_t)fi.fsize;
	entry->flags = (fi.fattrib & AM_DIR) ? OS64_DE_DIR : 0;
	entry->mtime = fat_mtime_epoch(fi.fdate, fi.ftime);
	return 0;
}

// FAT filesystem operations
// Delete a file. FatFs's f_unlink does the whole job — free the cluster
// chain, clear the directory entry — so this is only path construction plus
// an honest error code.
//
// Deliberately NOT gated on DISK_WRITING_ENABLED alongside .write below: a
// build with writing compiled out should fail an rm loudly through the NULL
// check in syscall_unlink, not quietly succeed at deleting nothing. It IS
// left out of the read-only ext2 fops entirely, which is the same statement
// made the honest way.
static int fat_rm(const char* filename, vfs_filesystem_t* vfs_fs)
{
	char lPath[512];

	if (filename == NULL || vfs_fs == NULL)
		return -1;

	strncpy(lPath, filename, sizeof(lPath) - 1);
	lPath[sizeof(lPath) - 1] = '\0';
	create_fat_path(lPath, vfs_fs);   // "N:" + path — FatFs addresses volumes by number

	FRESULT res = f_unlink(lPath);
	if (res != FR_OK)
	{
		printd(DEBUG_VFS, "fat_rm: f_unlink('%s') failed, FRESULT=%u\n", lPath, (uint32_t)res);
		return -1;
	}

	printd(DEBUG_VFS, "fat_rm: deleted '%s'\n", lPath);
	return 0;
}

// Rename a file — and the place where FAT's age shows.
//
// With flags zero, the seam preserves its original REPLACE behavior. ext2
// can make that replacement atomic,
// because one directory-block write swings a name onto a different inode.
// FAT CANNOT, and this function is where we say so instead of pretending:
//
//   - FatFs's f_rename refuses outright when the destination exists
//     (FR_EXIST) — there is no repoint-in-place primitive to reach for.
//   - So an existing destination is REMOVED FIRST, and between that unlink
//     and the rename that follows there is a real window in which neither
//     name resolves. A crash inside it loses the old file and does not
//     gain the new one.
//
// That is not a bug to be fixed here; it is what FAT is. The filesystem has
// no concept of a file identity separate from its directory entry — that
// idea IS the inode, and it is exactly what Ken Thompson's 1969 filesystem
// had and MS-DOS's 1981 one did not. A DEBTS row carries it so the
// difference stays visible. The opt-in policies do not pretend otherwise:
// NOREPLACE leaves an existing destination alone, and REQUIRE_ATOMIC_REPLACE
// refuses one because FAT cannot provide that guarantee. When the destination
// is absent, FatFs's own f_rename still closes a race by refusing if another
// caller creates it before the operation acquires the volume lock.
static int fat_rename(const char* oldpath, const char* newpath,
                      vfs_filesystem_t* vfs_fs, uint64_t flags)
{
	char lOld[512], lNew[512];

	if (oldpath == NULL || newpath == NULL || vfs_fs == NULL ||
	    (flags & ~((uint64_t)OS64_RENAME_FLAG_MASK)) != 0 ||
	    flags == OS64_RENAME_FLAG_MASK)
		return -1;

	strncpy(lOld, oldpath, sizeof(lOld) - 1);
	lOld[sizeof(lOld) - 1] = '\0';
	create_fat_path(lOld, vfs_fs);   // "N:" + path — FatFs addresses volumes by number

	strncpy(lNew, newpath, sizeof(lNew) - 1);
	lNew[sizeof(lNew) - 1] = '\0';
	create_fat_path(lNew, vfs_fs);

	// Is something already sitting on the destination name? f_stat answers
	// without opening (an open would take a handle we would then have to be
	// careful to close on every path out of here).
	FILINFO fi;
	FRESULT st = f_stat(lNew, &fi);
	if (st == FR_OK)
	{
		if (flags & (OS64_RENAME_NOREPLACE |
		             OS64_RENAME_REQUIRE_ATOMIC_REPLACE))
		{
			printd(DEBUG_VFS, "fat_rename: destination '%s' exists — requested safe policy refuses replacement\n",
			       lNew);
			return -1;
		}
		// Directories are never replaced — the seam's ruling, and here it is
		// also self-preservation: f_unlink on a non-empty directory fails,
		// and on an empty one it would silently remove a directory the
		// caller only meant to rename onto.
		if (fi.fattrib & AM_DIR)
		{
			printd(DEBUG_VFS, "fat_rename: refusing to replace directory '%s'\n", lNew);
			return -1;
		}
		FRESULT del = f_unlink(lNew);
		if (del != FR_OK)
		{
			printd(DEBUG_VFS, "fat_rename: could not clear destination '%s', FRESULT=%u\n",
			       lNew, (uint32_t)del);
			return -1;
		}
		// THE WINDOW IS OPEN HERE. Announced, not hidden — if a rename ever
		// dies in this gap, the log says exactly which two names were in
		// flight when it happened.
		printd(DEBUG_VFS, "fat_rename: destination '%s' removed (FAT has no atomic replace — window open)\n",
		       lNew);
	}

	FRESULT res = f_rename(lOld, lNew);
	if (res != FR_OK)
	{
		printd(DEBUG_VFS, "fat_rename: f_rename('%s' -> '%s') failed, FRESULT=%u\n",
		       lOld, lNew, (uint32_t)res);
		return -1;
	}

	printd(DEBUG_VFS, "fat_rename: renamed '%s' -> '%s'\n", lOld, lNew);
	return 0;
}

// df's numbers. f_getfree WALKS THE FAT to count free clusters (FatFs keeps
// no running total unless FSINFO is trusted), so this does real disk I/O —
// kernel context, like every other FatFs call. Totals are clusters × cluster
// size; the two reserved FAT entries are not clusters, hence n_fatent - 2.
static int fat_space(vfs_filesystem_t *vfs_fs, uint64_t *total_bytes, uint64_t *free_bytes)
{
	char drive_label[8];
	sprintf(drive_label, "%u:", vfs_fs->fatDiskNumber);

	DWORD free_clusters = 0;
	FATFS *ff = NULL;
	if (f_getfree(drive_label, &free_clusters, &ff) != FR_OK || ff == NULL)
		return -1;

	uint64_t cluster_bytes = (uint64_t)ff->csize * FF_MAX_SS;   // fixed sector size build (FF_MIN_SS == FF_MAX_SS), so FATFS carries no ssize member
	if (total_bytes)
		*total_bytes = (uint64_t)(ff->n_fatent - 2) * cluster_bytes;
	if (free_bytes)
		*free_bytes = (uint64_t)free_clusters * cluster_bytes;
	return 0;
}

vfs_file_operations_t fat_fops = {
	.initialize = fat_initialize,
	.space = fat_space,
    .open  = fat_open,
    .read  = fat_read,
	.fgets = fat_gets,
	.fputs = fat_puts,
	.tell = fat_tell,
	.fprintf = fat_fprintf,
#ifdef DISK_WRITING_ENABLED
    .write = fat_write,
#endif
	.rm = fat_rm,
	.rename = fat_rename,
    .close = fat_close,
	.seek = fat_seek,
	.sync = fat_sync,
	.uninitialize = fat_uninitialize
};

// FAT directory operations
vfs_directory_operations_t fat_dops = {
	.open=fat_open_dir,
	.close=fat_close_dir,
	.read=fat_read_dir,
	.mkdir=fat_mkdir,
	.stat=fat_stat
};
