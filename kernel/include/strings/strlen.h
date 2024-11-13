#ifndef STRLEN_H
#define STRLEN_H

#include <stddef.h>

size_t strlen(const char* str);
size_t strnlen(const char* str, int maxLen);

#endif