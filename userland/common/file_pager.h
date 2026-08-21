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
    const char *path;             // NULL = read the document from stdin
    uint32_t page_lines;
    file_pager_mode_t mode;
} file_pager_options_t;

typedef enum {
    FILE_PAGER_RESULT_DONE,
    FILE_PAGER_RESULT_ERROR,
    FILE_PAGER_RESULT_QUIT,
    FILE_PAGER_RESULT_NEXT,
    FILE_PAGER_RESULT_PREVIOUS
} file_pager_result_t;

// Page one named file, or stdin when path is NULL.  Keys always come from a
// separately minted controlling-terminal handle, so a pipe can remain the
// document on stdin without competing with navigation input.
file_pager_result_t file_pager_run(const file_pager_options_t *options);

// Use the controlling terminal's live height, reserving its bottom row for
// the pager prompt. Falls back when /proc/self/tty is unavailable.
uint32_t file_pager_default_lines(uint32_t fallback);

// Strict positive decimal parser shared by both command-line veneers.
bool file_pager_parse_lines(const char *text, uint32_t *lines);

#endif
