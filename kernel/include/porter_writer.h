#ifndef OS64_KERNEL_PORTER_WRITER_H
#define OS64_KERNEL_PORTER_WRITER_H

#include <stdbool.h>
#include <stdint.h>
#include "os64/storage.h"

struct s_thread;

int porter_writer_acquire(struct s_thread *thread, uint64_t task, os64_porter_owner_t *out);
bool porter_writer_held(const struct s_thread *thread);
void porter_writer_depart(const struct s_thread *thread);

#endif
