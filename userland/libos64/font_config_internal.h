#ifndef OS64_FONT_CONFIG_INTERNAL_H
#define OS64_FONT_CONFIG_INTERNAL_H
#include "os64/font_config.h"
/* Stat once, bound the allocation/read to that size and remaining budget.
 * Growth is refused; the observed size is charged even on read failure.
 * Success transfers the buffer. */
os64_font_config_status_t font_source_read(const char *, size_t *remaining,
                                          uint8_t **bytes, size_t *length);
#endif
