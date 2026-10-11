#ifndef OS64_WEBP_CONFIG_H
#define OS64_WEBP_CONFIG_H
#define HAVE_BUILTIN_BSWAP16 1
#define HAVE_BUILTIN_BSWAP32 1
#define HAVE_BUILTIN_BSWAP64 1
#ifndef OS64_WEBP_SCALAR
#define WEBP_HAVE_SSE2 1
#endif
/* Worker threads and CPU backends above the userland SSE2 baseline are absent. */
#endif
