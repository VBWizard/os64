#include "test_framework.h"

#include "CONFIG.h"
#include "memory/memcmp.h"
#include "memory/memset.h"
#include "memory/vma.h"
#include "memory/paging.h"   // paging_walk_paging_table — "is this page mapped?"
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

    task_t *task = get_core_local_storage()->task;
    vma_add(task, vma);

    // "Was the fault resolved?" is a question about THIS page, so ask it about
    // this page: unmapped before the access, mapped after.
    //
    // It used to be asked of kPageFaultCount — ONE global counter bumped by
    // every page fault on every core — with an assertion that it rose by
    // EXACTLY ONE. That only holds on a machine where nothing else faults for
    // the duration, which was true while the tests ran on an idle system and
    // stopped being true the moment anything else was running: /bin/logd
    // demand-paging its own ELF alongside the suite made the delta 2 or 3 and
    // failed a test that was working perfectly. A test whose result depends on
    // what OTHER cores are doing isn't testing what it claims to.
    uintptr_t before = paging_walk_paging_table((pt_entry_t *)task->pml4v, test_addr);
    if (before != 0 && before != 0xbadbadba) {
        kRootFilesystem->fops->close(file);
        TEST_FAIL("vma_file_backed_page_fault_resolved: page was already mapped — the read cannot fault");
    }

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

    uintptr_t after = paging_walk_paging_table((pt_entry_t *)task->pml4v, test_addr);
    if (after == 0 || after == 0xbadbadba) {
        TEST_FAIL("vma_file_backed_page_fault_resolved: page still unmapped — the fault was never resolved");
    }

    return true;
}

/// @brief Verify that a file-backed VMA whose file content ends partway into a
///        page zero-fills the remainder rather than reading past file_size.
///
/// This mirrors an ELF PT_LOAD segment with p_filesz < p_memsz where the file
/// data ends mid-page: elf_map_segment lowers vma->file_size to the true file
/// extent, and vma_resolve_backing_page must read only that many bytes and zero
/// the BSS tail.  We map /partition_info (37 bytes) over a full page but declare
/// only PARTIAL_BYTES of it as real file content; the bytes from PARTIAL_BYTES to
/// end-of-page must read back as zero even though the file itself has more data
/// there — which is exactly what the pre-fix code got wrong.
// Boundary chosen to sit inside the file's real content (37 bytes) so the region
// [PARTIAL_BYTES, 37) is a genuine before/after discriminator: the old code
// surfaced the file's text there, the fixed code surfaces zeros.
#define PARTIAL_BYTES 20

bool test_vma_partial_page_bss_zero_filled(void)
{
    if (kRootFilesystem == NULL && kRootPartUUID[0] != '\0') {
        vfs_mount_root_part((char*)&kRootPartUUID);
    }

    if (kRootFilesystem == NULL) {
        printd(DEBUG_TESTS, "\t[Test] vma_partial_page_bss_zero_filled... SKIP (no root fs)\n");
        return true;
    }

    vfs_file_t* file = NULL;
    if (kRootFilesystem->fops->open(&file, "/partition_info", "r", kRootFilesystem) != 0) {
        TEST_FAIL("vma_partial_page_bss_zero_filled: Failed to open /partition_info");
    }

    uint8_t expected[PARTIAL_BYTES];
    memset(expected, 0, sizeof(expected));
    int bytes_read = kRootFilesystem->fops->read(file, expected, sizeof(expected));
    if (bytes_read < PARTIAL_BYTES) {
        kRootFilesystem->fops->close(file);
        TEST_FAIL("vma_partial_page_bss_zero_filled: file smaller than expected");
    }

    if (kRootFilesystem->fops->seek(file, 0, SEEK_SET) < 0) {
        kRootFilesystem->fops->close(file);
        TEST_FAIL("vma_partial_page_bss_zero_filled: Failed to seek file");
    }

    // Map a full page but declare only PARTIAL_BYTES as file-backed.
    uintptr_t test_addr = 0x520000;
    vma_t *vma = vma_create(test_addr, test_addr + PAGE_SIZE, PROT_READ, MAP_PRIVATE, file, 0);
    if (vma == NULL) {
        kRootFilesystem->fops->close(file);
        TEST_FAIL("vma_partial_page_bss_zero_filled: Failed to create VMA");
    }
    vma->file_size = (uint64_t)PARTIAL_BYTES;   // simulate p_filesz ending mid-page
    vma_add(get_core_local_storage()->task, vma);

    // Fault the page in and snapshot both regions.
    volatile uint8_t *ptr = (volatile uint8_t *)test_addr;
    uint8_t actual[PARTIAL_BYTES];
    for (int i = 0; i < PARTIAL_BYTES; i++) {
        actual[i] = ptr[i];
    }
    bool tail_all_zero = true;
    for (int i = PARTIAL_BYTES; i < (int)PAGE_SIZE; i++) {
        if (ptr[i] != 0) {
            tail_all_zero = false;
            break;
        }
    }

    kRootFilesystem->fops->close(file);
    vma->file = NULL;

    if (memcmp(expected, actual, (size_t)PARTIAL_BYTES) != 0) {
        TEST_FAIL("vma_partial_page_bss_zero_filled: file region mismatch");
    }

    if (!tail_all_zero) {
        TEST_FAIL("vma_partial_page_bss_zero_filled: BSS tail not zero-filled");
    }

    return true;
}
