#ifndef OS64_PORTER_TRANSCRIPT_H
#define OS64_PORTER_TRANSCRIPT_H

#include "porter.h"

typedef struct {
    int32_t fd;
    bool failed;
    char path[PORTER_PATH];
} porter_transcript_t;

bool porter_write_all(int32_t fd, const char *data, size_t size);

// Start and finish once per invocation. The caller owns live output and can
// continue to use it after a transcript error.
bool porter_transcript_open(porter_transcript_t *log, int argc, char *const argv[]);
void porter_transcript_write(porter_transcript_t *log, const char *data, size_t size);
bool porter_transcript_close(porter_transcript_t *log, int status);

#endif
