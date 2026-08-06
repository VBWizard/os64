#ifndef OS64_FILE_PAGER_H
#define OS64_FILE_PAGER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FILE_PAGER_MORE,
    FILE_PAGER_LESS
} file_pager_mode_t;

typedef struct {
    const char *program;
    const char *path;
    uint32_t page_lines;
    file_pager_mode_t mode;
} file_pager_options_t;

// Page one explicitly named regular file while stdin remains the keyboard.
// Standard-input documents arrive with controlling TTY support later.
int file_pager_run(const file_pager_options_t *options);

// Strict positive decimal parser shared by both command-line veneers.
bool file_pager_parse_lines(const char *text, uint32_t *lines);

#endif
