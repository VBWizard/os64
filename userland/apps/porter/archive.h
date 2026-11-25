#ifndef OS64_PORTER_ARCHIVE_H
#define OS64_PORTER_ARCHIVE_H

#include "porter.h"

typedef int64_t (*porter_read_t)(void *context, void *buffer, size_t size);

typedef struct {
    char name[PORTER_PATH], path[PORTER_PATH];
    porter_identity_t identity;
} porter_member_t;

// An empty output path validates a member without writing it.
bool porter_archive_decode(porter_read_t read, void *source,
                           const porter_member_t *members, size_t count);
bool porter_archive_backup(const char *path, const porter_member_t *members, size_t count);
bool porter_archive_local(const char *path, const porter_member_t *members, size_t count);
bool porter_get_archive(const char *host, uint16_t port, const char *target,
                        const size_t *indices, const porter_member_t *members, size_t count);
uint64_t porter_tar_size(const porter_member_t *members, size_t count);

#endif
