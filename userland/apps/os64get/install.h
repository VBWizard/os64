#ifndef OS64GET_INSTALL_H
#define OS64GET_INSTALL_H

#include <stdbool.h>
#include <stdint.h>

#define INSTALL_PATH_MAX 256
#define INSTALL_MAX_FILES 256

typedef struct {
    char dest[INSTALL_PATH_MAX];
    char part[INSTALL_PATH_MAX];
    char directory[INSTALL_PATH_MAX];
    char backup_part[INSTALL_PATH_MAX];
    char backup[INSTALL_PATH_MAX];
    uint64_t old_length;
    uint32_t old_crc;
    bool existed;
    bool ready;
    bool skip;
} install_file_t;

// One invocation owns the recorded paths. Completed backups survive cleanup.
bool install_init(const char *archive);
bool install_plan(install_file_t *file, const char *destination);
bool install_conflicts(const install_file_t *a, const install_file_t *b);
bool install_prepare(install_file_t *file);
bool install_recheck(const install_file_t *file);
bool install_begin_commit(void);
bool install_commit(install_file_t *file);
bool install_cleanup(install_file_t *files, unsigned count);
const char *install_archive(void);
void install_cancel(int signo);
bool install_cancelled(void);
bool install_cancel_requested(void);

#endif
