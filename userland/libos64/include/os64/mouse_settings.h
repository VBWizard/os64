#ifndef OS64_MOUSE_SETTINGS_H
#define OS64_MOUSE_SETTINGS_H
#include "os64/mouse.h"

int os64_mouse_read(os64_mouse_snapshot_t *out);
int os64_mouse_apply(const os64_mouse_command_t *command);
int os64_mouse_save(const os64_mouse_setting_t *setting);
// Forget a disconnected identity on disk, then in the live registry. A failed
// live command after the file commit returns SAVED_ONLY; refresh before retrying.
#define OS64_MOUSE_FORGET_SAVED_ONLY 1
int os64_mouse_forget(const os64_mouse_setting_t *setting,uint64_t expected_generation);
// Restore the complete validated mouse.conf only while the session generation
// is zero. Offline device preferences are available when the device arrives.
int os64_mouse_startup(void);
bool os64_mouse_config_parse(const char *text,size_t length,os64_mouse_command_t *out);
#endif
