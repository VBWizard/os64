#include "test_framework.h"

#include "CONFIG.h"
#include "memory/memcmp.h"
#include "memory/memset.h"
#include "memory/vma.h"
#include "exceptions.h"
#include "smp_core.h"
#include "vfs.h"

extern vfs_filesystem_t* kRootFilesystem;
extern char kRootPartUUID[36];

/// @brief Open a file, read expected bytes, map a file-backed VMA, fault it in,
///        then compare the mapped bytes and fault count to the direct read.
bool test_vma_file_backed_page_fault_resolved(void)
{
    if (kRootFilesystem == NULL && kRootPartUUID[0] != '\0') {
        vfs_mount_root_part((char*)&kRootPartUUID);
    }

    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\t[Test] vma_file_backed_page_fault_resolved... SKIP (no root fs)\n");
        return true;
    }

    if (kRootFilesystem->fops == NULL ||
        kRootFilesystem->fops->open == NULL ||
        kRootFilesystem->fops->read == NULL ||
        kRootFilesystem->fops->seek == NULL ||
        kRootFilesystem->fops->close == NULL) {
        TEST_FAIL("vma_file_backed_page_fault_resolved: VFS file ops unavailable");
    }

    vfs_file_t* file = NULL;
    if (kRootFilesystem->fops->open(&file, "/partition_info", "r", kRootFilesystem) != 0) {
        TEST_FAIL("vma_file_backed_page_fault_resolved: Failed to open /partition_info");
    }

    uint8_t expected[16];
    memset(expected, 0, sizeof(expected));
    int bytes_read = kRootFilesystem->fops->read(file, expected, sizeof(expected));
    if (bytes_read <= 0) {
        kRootFilesystem->fops->close(file);
        TEST_FAIL("vma_file_backed_page_fault_resolved: Failed to read expected data");
    }

    if (kRootFilesystem->fops->seek(file, 0, SEEK_SET) < 0) {
        kRootFilesystem->fops->close(file);
        TEST_FAIL("vma_file_backed_page_fault_resolved: Failed to seek file");
    }

    uintptr_t test_addr = 0x500000;
    vma_t *vma = vma_create(test_addr, test_addr + PAGE_SIZE, PROT_READ, MAP_PRIVATE, file, 0);
    if (vma == NULL) {
        kRootFilesystem->fops->close(file);
        TEST_FAIL("vma_file_backed_page_fault_resolved: Failed to create VMA");
    }

    vma_add(get_core_local_storage()->task, vma);

    uint64_t old_faults = kPageFaultCount;

    volatile uint8_t *ptr = (volatile uint8_t *)test_addr;
    uint8_t actual[16];
    for (int i = 0; i < (int)sizeof(expected); i++) {
        actual[i] = ptr[i];
    }

    kRootFilesystem->fops->close(file);
    vma->file = NULL;

    int compare_len = bytes_read < (int)sizeof(expected) ? bytes_read : (int)sizeof(expected);
    if (memcmp(expected, actual, (size_t)compare_len) != 0) {
        TEST_FAIL("vma_file_backed_page_fault_resolved: Data mismatch");
    }

    if (kPageFaultCount != old_faults + 1) {
        TEST_FAIL("vma_file_backed_page_fault_resolved: Page fault not recorded");
    }

    return true;
}
