#ifndef OS64_SSH_INTERNAL_H
#define OS64_SSH_INTERNAL_H
#include "ssh_engine.h"
#ifdef SSH_HOST
#include <string.h>
#else
#include "os64/str.h"
#define memcpy os64_memcpy
#define memmove os64_memmove
#define memset os64_memset
#define strlen os64_strlen
int memcmp(const void *, const void *, size_t);
#endif
int ssh_packet_send(ssh_engine *s, const uint8_t *p, size_t n);
void ssh_connection_packet(ssh_engine *s, const uint8_t *p, size_t n);
#endif
