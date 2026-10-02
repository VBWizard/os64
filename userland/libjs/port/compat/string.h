#ifndef OS64_JSPORT_STRING_H
#define OS64_JSPORT_STRING_H
#include "os64/str.h"
#define memcpy os64_memcpy
#define memmove os64_memmove
#define memset os64_memset
#define memcmp os64_memcmp
#define memchr os64_memchr
#define strlen os64_strlen
#define strcmp os64_strcmp
#define strncmp os64_strncmp
#define strchr os64_strchr
#define strrchr os64_strrchr
#endif
