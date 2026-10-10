#ifndef OS64_WEBP_STDLIB_H
#define OS64_WEBP_STDLIB_H
#include <stddef.h>
void *webp_port_malloc(size_t size);
void *webp_port_calloc(size_t count, size_t size);
void webp_port_free(void *ptr);
#define malloc webp_port_malloc
#define calloc webp_port_calloc
#define free webp_port_free
static inline int abs(int x) { return x < 0 ? -x : x; }
#endif
