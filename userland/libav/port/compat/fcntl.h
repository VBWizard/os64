#ifndef OS64_AV_FCNTL_H
#define OS64_AV_FCNTL_H
/* Private to the FFmpeg port. No file is ever opened by the library: open
 * answers -1, so FFmpeg takes the path it takes for a missing file. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_BINARY 0
#include <stddef.h>
#include <stdint.h>
typedef int64_t off_t;
int avport_open(const char *path, int flags, ...);
int avport_close(int fd);
int64_t avport_read(int fd, void *buffer, size_t length);
#define open avport_open
#define close avport_close
#define read avport_read
#endif
