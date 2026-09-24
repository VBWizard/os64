#ifndef DECORATIONS_H
#define DECORATIONS_H
#include <stddef.h>
#include <stdint.h>
typedef struct decoration_pending decoration_pending_t;
decoration_pending_t *decorations_begin(void);
void decorations_discard(decoration_pending_t *);
int decorations_write(decoration_pending_t *, const void *, size_t);
int decorations_status(char *, size_t);
#endif
