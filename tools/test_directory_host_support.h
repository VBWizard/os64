#ifndef TEST_DIRECTORY_HOST_SUPPORT_H
#define TEST_DIRECTORY_HOST_SUPPORT_H
#include "os64/dirent.h"
#include <stddef.h>
#include <stdint.h>
extern const os64_dirent_t *test_entries;
extern size_t test_entry_count;
extern int test_read_error_at, test_close_error, test_open_error;
extern const char *test_mounts;
extern const char *test_home;
extern int test_alloc_fail;
void test_reset_io(void);
#endif
