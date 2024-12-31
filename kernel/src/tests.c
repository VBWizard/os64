#include "tests.h"
#include "kmalloc.h"
#include "serial_logging.h"
#include "memset.h"

int testVFS(vfs_filesystem_t *testFS)
{
	printd(DEBUG_HARDDRIVE, "\nDisk tests:\n");
	vfs_file_t* testFile = NULL;
	vfs_directory_t* testDir = NULL;
	FILINFO* fi=kmalloc(sizeof(FILINFO));
	uint32_t lResult=0;
	char* contents = kmalloc(4096);

	if(testFS->fops->open(&testFile, "/partition_info", "r", testFS)==0)
	{
		testFS->fops->read(testFile, contents, 4096);
		printd(DEBUG_HARDDRIVE, "/partition_info file contents via read = %s\n",contents);
		testFS->fops->seek(testFile, 0, SEEK_SET);
		memset(contents, 0, 4096);
		testFS->fops->fgets(testFile, contents, 4096);
		printd(DEBUG_HARDDRIVE, "/partition_info file contents via fgets = %s \n",contents);
		testFS->fops->close(testFile);
	}
	else
	{
		printd(DEBUG_HARDDRIVE, "File /partition_info NOT FOUND!\n");
		return -1;
	}

	if (testFS->dops->open(&testDir, "/", testFS)==0)
	{
		do
		{
			if(!testFS->dops->read(testDir, fi) && fi->fname[0] != '\0')
			{
				if (fi->fattrib & AM_DIR)
					printd(DEBUG_HARDDRIVE, "Directory: %s\n",fi->fname);
				else
					printd(DEBUG_HARDDRIVE, "File: %s - Attrib 0x%02x - Size %u\n",fi->fname, fi->fattrib, fi->fsize);
			}
		} while (fi->fname[0] != '\0');
	}
	else
	{
		printd(DEBUG_HARDDRIVE, "Failed to open root directory!\n");
		return -2;
	}

	if (testFS->bops->write)
	{
		if (testFS->fops->open(&testFile, "/test2","c", testFS)==0)
		{
			testFS->fops->write(testFile, "Hello world from Chris!\n",24);
			testFS->fops->close(testFile);
			printd(DEBUG_HARDDRIVE, "New test file /test2 written\n");
		}
		else
		{
			printd(DEBUG_HARDDRIVE, "Failed to create file /test2!\n");
			return -3;
		}

		if(testFS->fops->open(&testFile, "/test2", "r", testFS)==0)
		{
			testFS->fops->fgets(testFile, contents, 4096);
			testFS->fops->close(testFile);
			printd(DEBUG_HARDDRIVE, "New file test2 Contents = %s\n",contents);
		}
		else
		{
			printd(DEBUG_HARDDRIVE, "Failed to read file test2 using fgets!\n");
			return -4;
		}

		lResult = testFS->dops->mkdir("/testdir", testFS);
		if (lResult==FR_OK || lResult==FR_EXIST)
		{
			if (lResult==FR_EXIST)
				printd(DEBUG_HARDDRIVE, "Directory /testdir already exists\n");

			if (testFS->fops->open(&testFile, "/testdir/testfile","c", testFS)==0)
			{
				testFS->fops->write(testFile, "Hello world from Chris too!\n",27);
				testFS->fops->close(testFile);
				printd(DEBUG_HARDDRIVE, "New test file /testdir/testfile written\n");
			}
			else
				printd(DEBUG_HARDDRIVE, "Failed to open file /testdir/testfile for create\n");

			if(testFS->fops->open(&testFile, "/testdir/testfile", "r", testFS)==0)
			{
				testFS->fops->fgets(testFile, contents, 4096);
				testFS->fops->close(testFile);
				printd(DEBUG_HARDDRIVE, "New file /testdir/testfile Contents = %s\n",contents);
			}
			else
				printd(DEBUG_HARDDRIVE, "Failed to open file /testdir/testfile for write\n");
		}
		else
		{
			printd(DEBUG_HARDDRIVE, "Failed to make directory /testdir\n");
			return -5;
		}
	}
	else
		printd(DEBUG_HARDDRIVE, "Disk %s does not have a write function, skipping write tests\n", testFS->block_device_info->ATADeviceModel);
	kfree(contents);

	return 0;
}
