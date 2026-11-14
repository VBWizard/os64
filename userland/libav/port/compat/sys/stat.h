#ifndef OS64_AV_SYS_STAT_H
#define OS64_AV_SYS_STAT_H
/* Private to the FFmpeg port: the type and macros os_support.h names. */
#include <stdint.h>
struct stat { int64_t st_size; uint32_t st_mode; int64_t st_mtime; };
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
int avport_fstat(int fd, struct stat *out);
int avport_mkdir(const char *path, int mode);
#define fstat avport_fstat
#define mkdir avport_mkdir
#endif
