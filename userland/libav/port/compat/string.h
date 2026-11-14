#ifndef OS64_AV_STRING_H
#define OS64_AV_STRING_H
/* Private to the FFmpeg port (libav.so): the <string.h> its sources call.
 * A name libos64 exports is mapped onto libos64, for libav and for a
 * program that includes FFmpeg's headers alike. The names av_libc.c
 * supplies are libav's own and leave it under no name (exports.map), so
 * they are mapped only inside libav's build (HAVE_AV_CONFIG_H, which
 * FFmpeg defines for its own sources and shared.mk for port/): a caller
 * that met them would fail to link. */
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
#ifdef HAVE_AV_CONFIG_H
#include "av_libc.h"
#define strncmp avlibc_strncmp
#define strstr avlibc_strstr
#define strspn avlibc_strspn
#define strcspn avlibc_strcspn
#define strcpy avlibc_strcpy
#endif
#endif
