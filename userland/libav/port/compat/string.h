#ifndef OS64_AV_STRING_H
#define OS64_AV_STRING_H
/* Private to the FFmpeg port (libav.so): the <string.h> its sources call,
 * mapped onto libos64 where libos64 has the function and onto av_libc.c
 * where it does not. Not a libc for anyone else. */
#include <stddef.h>
#include "os64/str.h"
#define memcpy os64_memcpy
#define memmove os64_memmove
#define memset os64_memset
#define memcmp os64_memcmp
#define memchr os64_memchr
#define strlen os64_strlen
#define strcmp os64_strcmp
#define strchr os64_strchr
#define strrchr os64_strrchr
#include "av_libc.h"
#define strncmp avlibc_strncmp
#define strstr avlibc_strstr
#define strspn avlibc_strspn
#define strcspn avlibc_strcspn
#define strcpy avlibc_strcpy
#define strerror avlibc_strerror
#endif
