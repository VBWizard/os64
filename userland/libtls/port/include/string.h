#ifndef OS64_BEARSSL_STRING_H
#define OS64_BEARSSL_STRING_H

// Private include path for upstream core compilation, not installed as libc.
#include "os64/str.h"
#define memcpy os64_memcpy
#define memmove os64_memmove
#define memset os64_memset
#define strlen os64_strlen
#define memcmp os64_bearssl_memcmp
int os64_bearssl_memcmp(const void *a, const void *b, size_t size);

#endif
