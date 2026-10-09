#ifndef OS64_MOUSE_SETTINGS_H
#define OS64_MOUSE_SETTINGS_H
#include "os64/mouse.h"

int os64_mouse_read(os64_mouse_snapshot_t *out);
int os64_mouse_apply(const os64_mouse_command_t *command);
int os64_mouse_save(const os64_mouse_setting_t *setting);
// Restore the complete validated mouse.conf only while the session generation
// is zero. Offline device preferences are available when the device arrives.
int os64_mouse_startup(void);
bool os64_mouse_config_parse(const char *text,size_t length,os64_mouse_command_t *out);
#endif
