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

extern uint64_t kSystemCurrentTime; // Your kernel's epoch time variable

vfs_filesystem_t* vfs_get_device_by_fat_disk_number(uint8_t fatDiskNumber)
{

	dlist_node_t* bdl = (dlist_node_t*)kBlockDeviceDList;
	vfs_filesystem_t* vfsdev;

	do
	{
		if (bdl->data != 0)
		{
			vfsdev = (vfs_filesystem_t*)bdl->data;
			if (vfsdev->fatDiskNumber == fatDiskNumber)
				return vfsdev;
		}
		bdl = bdl->next;
	} while (bdl);
	return NULL;
}

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

	fs->bops->read(fs->block_device_info, 
				fs->block_device_info->block_device->partition_table->parts[fs->partNumber]->partStartSector + sector, buff, count);

	return 0;
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

	fs->bops->write(fs->block_device_info, 
	fs->block_device_info->block_device->partition_table->parts[fs->partNumber]->partStartSector + sector, buff, count);
	return 0;
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
	partEntry_t* partition = fs->block_device_info->block_device->partition_table->parts[fs->partNumber];

	if (fs==NULL)
		panic("fat disk_write: Cannot find vfs device for disk number %u\n",pdrv);

	uint16_t temp = 0;

	switch(cmd)
	{
		case CTRL_SYNC:
			break;
		case GET_SECTOR_COUNT:
			memcpy(buff, &partition->partTotalSectors,4);
			break;
		case GET_SECTOR_SIZE:
			temp = DEFAULT_SECTOR_SIZE;
			memcpy(buff, &temp, 2);
			break;
		case GET_BLOCK_SIZE:
			temp = 1;
			memcpy(buff, &temp, 2);
			break;
		case CTRL_TRIM:
			break;
	}
	return 0;
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

static int fat_open (vfs_file_t** vfs_file, const char* path, const char* mode, vfs_filesystem_t* vfs_fs)
{
	FIL* fat_file = kmalloc(sizeof(FIL));  // FIL object (FAT filesystem file handle)
    BYTE fat_mode = 0;

	*vfs_file = kmalloc(sizeof(vfs_file_t));
	// Convert VFS mode string to FAT mode flags
    if (strcmp(mode, "r") == 0) fat_mode = FA_READ;
    else if (strcmp(mode, "w") == 0) fat_mode = FA_WRITE | FA_CREATE_ALWAYS;
    else if (strcmp(mode, "a") == 0) fat_mode = FA_WRITE | FA_OPEN_APPEND;
	else if (strcmp(mode, "c") == 0) fat_mode = FA_WRITE | FA_CREATE_NEW;
	
	char lPath[255];
	strncpy(lPath, path, 255);
	create_fat_path(lPath, vfs_fs);

    if (f_open(fat_file, lPath, fat_mode) != FR_OK) {
        return -1; // Error
    }

    (*vfs_file)->handle = fat_file;
	(*vfs_file)->f_path = (void*)path;
	(*vfs_file)->owner = 0x0;
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

static int fat_fprintf(vfs_file_t* vfs_file, const char* fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	return f_printf(vfs_file->handle, fmt, args);
	va_end(args);
}

static int fat_initialize(vfs_filesystem_t* vfs_fs) {
    // Allocate FAT context
    FATFS* fat_fs = kmalloc(sizeof(FATFS));
	char* drive_label = kmalloc(255);

	create_fat_path(drive_label, vfs_fs);

    // Mount the FATFS
    if (f_mount(fat_fs, drive_label, 1) != FR_OK) {
        kfree(drive_label);
        return -1; // Failed to mount
    }

    // Store the context in the VFS filesystem
    vfs_fs->fs_specific = fat_fs;
    vfs_fs->fops = &fat_fops;
	kfree(drive_label);
    return 0; // Success
}

static int fat_uninitialize(vfs_filesystem_t* vfs_fs)
{
	int retVal = 0;
	char* drive_label = kmalloc(255);
	
	create_fat_path(drive_label, vfs_fs);

	retVal = f_unmount(drive_label);
	kfree(vfs_fs->fs_specific);
	kfree(drive_label);
	return retVal;
}

// FAT directory methods
static int fat_open_dir(vfs_directory_t** vfs_dir, const char* path, vfs_filesystem_t* vfs_fs)
{
	DIR* dir=kmalloc(sizeof(DIR));
	char tempPath[255];

	strncpy(tempPath, path, 255);
	create_fat_path(tempPath, vfs_fs);

	if (f_opendir(dir, tempPath))
		{
			kfree(dir);
			return -1;
		}
	*vfs_dir = kmalloc(sizeof(vfs_directory_t));
	(*vfs_dir)->handle=dir;
	return 0;
}

static int fat_close_dir(vfs_directory_t* vfs_dir)
{
	return f_closedir(vfs_dir->handle);
}

static int fat_read_dir(vfs_directory_t* vfs_dir, void* filInfo)
{

	DIR* dir=vfs_dir->handle;
	return f_readdir(dir, filInfo);
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
	.read=fat_read_dir
};

typedef uint32_t DWORD;

