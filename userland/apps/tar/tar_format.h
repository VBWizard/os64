#ifndef OS64_TAR_FORMAT_H
#define OS64_TAR_FORMAT_H

// The byte-level POSIX ustar format. This layer has no os64 dependencies so
// the host test can put its headers in front of another tar implementation.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAR_BLOCK_SIZE 512
#define TAR_PATH_CAP   256

#define TAR_TYPE_REGULAR   '0'
#define TAR_TYPE_DIRECTORY '5'

typedef struct {
    char path[TAR_PATH_CAP];
    uint64_t size;
    uint64_t mtime;
    uint32_t mode;
    char type;
} tar_entry_t;

typedef enum {
    TAR_FORMAT_OK = 0,
    TAR_FORMAT_PATH_TOO_LONG,
    TAR_FORMAT_BAD_CHECKSUM,
    TAR_FORMAT_BAD_MAGIC,
    TAR_FORMAT_BAD_NUMBER,
    TAR_FORMAT_EMPTY_NAME
} tar_format_result_t;

bool tar_block_is_zero(const uint8_t block[TAR_BLOCK_SIZE]);
tar_format_result_t tar_header_encode(uint8_t block[TAR_BLOCK_SIZE],
                                      const tar_entry_t *entry);
tar_format_result_t tar_header_decode(const uint8_t block[TAR_BLOCK_SIZE],
                                      tar_entry_t *entry);

// Turn a source operand into a relative member name. Leading slashes and
// lexical parent steps are removed so an archive created by this program is
// also acceptable to its extraction gate.
tar_format_result_t tar_member_path(const char *source,
                                    char out[TAR_PATH_CAP]);

// Normalize a member name for extraction, rejecting either spelling that can
// escape the current directory: an absolute name or any parent component.
tar_format_result_t tar_extract_path(const char *member,
                                     char out[TAR_PATH_CAP]);

const char *tar_format_error(tar_format_result_t result);

#endif // OS64_TAR_FORMAT_H
