#ifndef GTERM_SETTINGS_H
#define GTERM_SETTINGS_H
#include <stdbool.h>
#include <stdint.h>
#define GTERM_HISTORY_DEFAULT 2000u
bool gterm_history_parse(const char *,uint32_t *);
const char *gterm_history_error(int64_t result);
uint32_t gterm_history_load(void);
int gterm_history_save(uint32_t);
#endif
