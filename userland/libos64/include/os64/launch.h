#ifndef OS64_LAUNCH_H
#define OS64_LAUNCH_H
#include <stdint.h>
#define OS64_LAUNCH_PATH 256u
typedef enum {
    OS64_LAUNCH_OK, OS64_LAUNCH_UNASSOCIATED, OS64_LAUNCH_CONFIG_ERROR,
    OS64_LAUNCH_INVALID, OS64_LAUNCH_SPAWN_ERROR
} os64_launch_status_t;
/* Resolve a final extension, case insensitively, from a complete configuration.
 * config is an absolute path or a bare config-search name. Values are absolute
 * executable paths; empty values remove a mapping. Last duplicate wins.
 * out is empty on failure. Missing configuration is an error, not an unknown
 * extension. The caller owns application-choice UI and child collection. */
os64_launch_status_t os64_launch_association(const char *config,const char *file,
    char out[OS64_LAUNCH_PATH]);
/* Borrow the final extension when it can be stored as a configuration key. */
const char *os64_launch_extension(const char *file);
/* Merge into the chosen config; bare names inherit the currently readable file
 * into the top search directory. An absolute name updates that readable file.
 * Refuses malformed/unreadable input and values the dialect cannot round-trip. */
os64_launch_status_t os64_launch_remember(const char *config,const char *file,
    const char *application);
/* Launch exactly [application, file, NULL]. Both paths must be absolute and fit
 * OS64_LAUNCH_PATH. No shell parsing, PATH search or extension-based execution.
 * A positive pid on success belongs to the caller, which must reap it. */
os64_launch_status_t os64_launch_file(const char *application,const char *file,
    int64_t *pid);
#endif
