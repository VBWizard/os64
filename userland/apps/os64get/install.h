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
    uint64_t received_length;
    uint32_t received_crc;
    uint64_t old_length;
    uint32_t old_crc;
    bool received;
    bool existed;
    bool ready;
    bool skip;
} install_file_t;

// One invocation owns the recorded paths. Completed backups survive cleanup.
bool install_init(const char *archive);
// Resolution is read-only; reserve scratch after ruling out an unchanged file.
bool install_resolve(install_file_t *file, const char *destination);
bool install_reserve(install_file_t *file);
bool install_plan(install_file_t *file, const char *destination);
bool install_conflicts(const install_file_t *a, const install_file_t *b);
// Record the completed transfer's byte fingerprint; preparation checks the
// staged file against it after the download handle has been synced and closed.
void install_received(install_file_t *file, uint64_t length, uint32_t crc);
bool install_prepare(install_file_t *file);
bool install_recheck(const install_file_t *file);
bool install_begin_commit(void);
bool install_commit(install_file_t *file);
bool install_cleanup(install_file_t *files, unsigned count);
// Empty until at least one original backup has been finalized.
const char *install_archive(void);
void install_cancel(int signo);
bool install_cancelled(void);
bool install_cancel_requested(void);

#endif
