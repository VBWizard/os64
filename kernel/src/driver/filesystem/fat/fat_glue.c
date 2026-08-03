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
    else if (strcmp(mode, "w") == 0) fat_mode = FA_WRITE | FA_CREATE_ALWAYS;
    else if (strcmp(mode, "a") == 0) fat_mode = FA_WRITE | FA_OPEN_APPEND;
	else if (strcmp(mode, "c") == 0) fat_mode = FA_WRITE | FA_CREATE_ALWAYS;
	else
		return -1;

	FIL* fat_file = kmalloc(sizeof(FIL));  // FIL object (FAT filesystem file handle)
	*vfs_file = kmalloc(sizeof(vfs_file_t));
	if (fat_file == NULL || *vfs_file == NULL)
	{
		// NOTE: kfree(NULL) is NOT a no-op in this kernel — free_memory can't
		// find address 0 and panics — so each free gets its own guard.
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
	(*vfs_file)->fops = vfs_fs != NULL ? vfs_fs->fops : &fat_fops;
	(*vfs_file)->owner = vfs_fs;
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

    if (f_close(fat_file) != FR_OK) {
        return -1; // Error
    }
    kfree(vfs_file); // Free the VFS file object
	kfree(fat_file);
    return 0; // Success
}

static int fat_seek(vfs_file_t* vfs_file, long offset, int whence) {
    FIL* fat_file = (FIL*)vfs_file->handle;
    FSIZE_t new_pos;

    // Calculate the new position based on whence
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = f_tell(fat_file) + offset;
            break;
        case SEEK_END:
            new_pos = f_size(fat_file) + offset;
            break;
        default:
            return -1; // Invalid whence
    }

    if (f_lseek(fat_file, new_pos) != FR_OK) {
        return -1; // Seek failed
    }

    return 0; // Success
}

/// @brief Sync an open file to disk
/// @param vfs_file The handle of the file to sync
/// @return The status of the sync attempt
static int fat_sync(vfs_file_t* vfs_file)
{
	return f_sync(vfs_file->handle);
}

static char* fat_gets(vfs_file_t* vfs_file, char* buffer, int length)
{
	return f_gets(buffer, length, vfs_file->handle);
}

static int fat_puts(vfs_file_t* vfs_file, char* buffer)
{
	return f_puts(buffer, vfs_file->handle);
}

static int fat_tell(vfs_file_t* vfs_file)
{
	return f_tell((FIL*)vfs_file->handle);
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

    // Store the context in the VFS filesystem
    vfs_fs->fs_specific = fat_fs;
    vfs_fs->fops = &fat_fops;
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
	return 1;
}

static int fat_mkdir(char* path, vfs_filesystem_t* vfs_fs)
{
	char lPath[255];
	strncpy(lPath, path, 255);
	create_fat_path(lPath, vfs_fs);
	return f_mkdir(lPath);
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
	return 0;
}

// FAT filesystem operations
vfs_file_operations_t fat_fops = {
	.initialize = fat_initialize,
    .open  = fat_open,
    .read  = fat_read,
	.fgets = fat_gets,
	.fputs = fat_puts,
	.tell = fat_tell,
	.fprintf = fat_fprintf,
#ifdef DISK_WRITING_ENABLED
    .write = fat_write,
#endif
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
