#ifndef OS64_DIRECTORY_INTERNAL_H
#define OS64_DIRECTORY_INTERNAL_H
#include "os64/directory.h"
os64_ui_status_code_t os64_directory_data_cancelled(const os64_directory_t *,
    bool (*cancelled)(void *), void *cancel_user, os64_ui_data_t **out);
#endif
