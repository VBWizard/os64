#ifndef STRFTIME_H
#define STRFTIME_H

#include <stddef.h>
#include "time.h"

size_t strftime(char *s, size_t maxsize, const char *format, const struct tm *t);
size_t strftime_epoch(char *buffer, size_t maxsize, const char *format, long epoch_time);

#endif
